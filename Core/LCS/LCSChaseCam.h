// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/LCS/LCSInput.h"

// The camera the player looks through on foot and in a vehicle, turned by the mouse.
// docs/lcs/chase-camera.md has the measurements; the VCS fork's VCSChaseCam is the same design.
//
// The game computes its camera and this replaces the result, inside the game's own frame:
//
//   1. At 0x08902174 (a HOOKENTER replacement), CCamera::Process has this frame's camera in three
//      stack vectors: Source (sp+0x70), Front (sp+0x80, $fp) and Up (sp+0x90, $s7). The prepare
//      hook reads them, works out the pivot (above the player, or above the vehicle), the direction
//      the player is looking and how far back the camera wants to be, and writes a fan of rays from
//      the pivot into a block of PSP memory.
//   2. At 0x08902318 the game is about to build TheCamera's matrix from those vectors. That `jal`
//      goes to a small program in the block instead, which calls the game's own
//      CWorld::ProcessLineOfSight once per ray and then reaches the finish hook. That hook pulls
//      the camera in to the nearest hit and writes Source, Front and Up over the game's; the
//      program then jumps to the function the jal was meant to call.
//
// So the collision and the timing are the game's, and everything downstream of the matrix (the
// renderer, the streamer, the radar) sees the camera the player sees. When something is in the
// way the camera moves in along the line it looks down, as San Andreas on PC does: look up and it
// comes down behind the player's legs; stand in an alley and it comes in close.
//
// Why not write the angles: in a vehicle, mode 18 (CamOnAString) swings a written yaw back
// (1.679 written, 1.346 kept). On foot the angles do stick once Process_FollowPed's spring and yaw
// correction are out (LCSInput), but mode 4 keeps its own distance band and pulls in only where its
// own line of sight to the player is blocked, so looking up put the camera into the ground.
//
// It owns mode 4 on foot and modes 18 and 22 in a vehicle, and nothing else: aiming, cutscenes and
// the other car views stay the game's, with a short blend each way. On foot it writes the game's
// own angles every frame too, so walking and aiming start from the view the player has.
//
// Threads: both hooks run on the emu thread inside the game's frame; ChaseCamAddLook is called on
// the emu thread at the frame boundary (LCSInput).

namespace LCS {

struct LCSChaseCamTuning {
	// --- On foot ---
	// The pivot's height above the player's origin, and how far back the camera sits with nothing
	// in the way: Process_FollowPed's own target height (table [0] at 0x08b359bc) and the top of its
	// distance band (TheCamera+0x13c 2.5 + table [1] 2.0; measured 4.1-4.5 in the open).
	float footPivotHeight = 0.6f;
	float footDistance = 4.5f;
	float footLookUp = 1.20f;
	float footLookDown = 1.35f;
	// Keep the camera out of parked cars. The player's own vehicle is never in play on foot.
	bool footCollideVehicles = true;
	// The player as a capsule for the near plane and for lifting the camera out: this radius
	// around the origin's vertical axis, from footBodyBelow under it to footBodyAbove over it.
	float footBodyRadius = 0.35f;
	float footBodyBelow = 1.05f;
	float footBodyAbove = 0.85f;

	// --- Vehicles ---
	// Pivot height as a fraction of the collision box's top.
	float vehiclePivotScale = 0.95f;
	// How far back: (base + perLength * box length) at the middle zoom level, scaled by the near
	// and far factors at the other two (Select cycles them; TheCamera+0xcc reads 1, 2, 3).
	float vehicleDistanceBase = 5.0f;
	float vehicleDistancePerLength = 0.65f;
	float vehicleZoomNear = 0.8f;
	float vehicleZoomFar = 1.35f;
	float vehicleDistanceScale = 1.0f;
	float vehicleLookUp = 0.90f;
	float vehicleLookDown = 1.30f;
	// The pitch the view settles to behind a moving vehicle: slightly down.
	float vehicleRestPitch = -0.12f;
	// Swing back behind the vehicle once the mouse has been left alone and the vehicle is moving.
	bool vehicleRecentre = true;
	float recentreDelay = 1.5f;     // seconds of no look input
	float recentreRate = 2.0f;      // 1/seconds, at full urgency
	float recentreMinSpeed = 3.0f;  // world units a second
	// The camera's position may only orbit this far below the pivot. Looking further up tilts the
	// view instead of swinging the camera down under the car.
	float vehicleOrbitUp = 0.35f;
	// The game's own look left / right / behind (L, R, both: Q, E, C) turn this camera too; on
	// foot only behind (L alone is the game's look-around, R alone its lock-on).
	bool glances = true;

	// --- Collision ---
	// 1 casts along the view only; 5 adds four rays spread to coneRadius at the camera, so the
	// near plane cannot clip into a wall the centre ray just misses.
	int rays = 5;
	float coneRadius = 0.25f;
	// How far short of a hit the camera stops, and the closest it will ever come.
	float collisionMargin = 0.2f;
	float minDistance = 0.35f;
	// A hit pulls the camera in at once; everything else eases at this rate.
	float easeOutRate = 3.0f;
	bool collideObjects = true;

	// --- Keeping the lens out of the player and the vehicle ---
	// The near clip plane is lowered to a fraction of the camera's clearance from the player's
	// capsule or the vehicle's box, through the game's own RwCameraSetNearClipPlane, never above
	// the game's own value.
	bool nearClip = true;
	float nearClipFactor = 0.6f;
	float nearClipMin = 0.05f;
	// If the camera still ends up inside, it is lifted out over the top by this much.
	float subjectMargin = 0.12f;

	// --- Handing the view over ---
	float blendIn = 0.25f;          // seconds, from the game's camera to ours
	float blendOut = 0.15f;         // seconds, from ours back to whatever the game switched to
	float pivotSettleRate = 6.0f;   // 1/seconds, for the pivot moving between the player and a car
	// Carry the game's camera shake (explosions, rough ground) over onto this camera.
	bool keepShake = true;
};

LCSChaseCamTuning &ChaseCamTuning();

// Put both hooks in place, or take them out when both settings are off. Called every tick from
// LCS::Tick, and cheap once installed: it re-checks that the patch is still there, because
// loading a savestate replaces PSP memory underneath it.
void InstallChaseCam();

// Put the game's instructions back and give the block back. Called from LCS::Shutdown.
void RemoveChaseCam();

// Whether mouse look in this context goes to the chase camera (its hooks are in and its setting
// for the context is on).
bool ChaseCamTakesLook(InputContext context);

// Turn the view, in radians. Yaw is the direction the camera looks, counter-clockwise from
// above; pitch is up.
void ChaseCamAddLook(float yawRadians, float pitchRadians);

// The two replacement entries in ReplaceTables.cpp.
int Hook_lcs_camera_prepare();
int Hook_lcs_camera_finish();

// For the debugger.
struct LCSChaseCamStats {
	const char *status = "";
	bool installed = false;
	bool owning = false;
	const char *kind = "";
	int mode = -1;
	u64 frames = 0;
	u64 ownedFrames = 0;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float gameDistance = 0.0f;
	float wantDistance = 0.0f;
	float allowedDistance = 0.0f;
	float distance = 0.0f;
	float clearance = 0.0f;
	float nearClip = 0.0f;
	int rays = 0;
	int hits = 0;
	float pivot[3] = {};
	float boxLength = 0.0f;
	bool boxFromGame = false;
	float speed = 0.0f;
	float sinceLook = 0.0f;
	bool glancing = false;
	bool blendingOut = false;
	float blendIn = 0.0f;
	u32 block = 0;
};
void ChaseCamGetStats(LCSChaseCamStats *out);

}  // namespace LCS
