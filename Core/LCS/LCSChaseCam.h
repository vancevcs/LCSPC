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

// The camera the player looks through in a vehicle, turned by the mouse. docs/lcs/chase-camera.md
// has the measurements; the VCS fork's VCSChaseCam is the same design.
//
// Why not write the angles, as on foot: mode 18 (CamOnAString) swings back to where its own
// spring wants it. Measured: Beta written to 1.679 between frames came back as 1.346. So this
// lets the game compute its camera and then replaces the result, inside the game's own frame:
//
//   1. At 0x08902174 (a HOOKENTER replacement), CCamera::Process has this frame's camera in three
//      stack vectors: Source (sp+0x70), Front (sp+0x80, $fp) and Up (sp+0x90, $s7). The prepare
//      hook reads them, works out the pivot above the vehicle, the direction the player is
//      looking and how far back the camera wants to be, and writes a fan of rays from the pivot
//      into a block of PSP memory.
//   2. At 0x08902318 the game is about to build TheCamera's matrix from those vectors. That `jal`
//      goes to a small program in the block instead, which calls the game's own
//      CWorld::ProcessLineOfSight once per ray and then reaches the finish hook. That hook pulls
//      the camera in to the nearest hit and writes Source, Front and Up over the game's; the
//      program then jumps to the function the jal was meant to call.
//
// So the collision and the timing are the game's, and everything downstream of the matrix (the
// renderer, the streamer, the radar) sees the camera the player sees. It owns modes 18 and 22 in
// a vehicle and nothing else: on foot, aiming, cutscenes and the other car views stay the game's,
// with a short blend each way.
//
// Threads: both hooks run on the emu thread inside the game's frame; ChaseCamAddLook is called on
// the emu thread from the frame wait (LCSInput).

namespace LCS {

struct LCSChaseCamTuning {
	// Pivot height as a fraction of the collision box's top.
	float pivotScale = 0.95f;
	// How far back: (base + perLength * box length) at the middle zoom level, scaled by the near
	// and far factors at the other two (Select cycles them; TheCamera+0xcc reads 1, 2, 3).
	float distanceBase = 5.0f;
	float distancePerLength = 0.65f;
	float zoomNear = 0.8f;
	float zoomFar = 1.35f;
	float distanceScale = 1.0f;
	// How far up and down the view may tilt, radians.
	float lookUp = 0.90f;
	float lookDown = 1.30f;
	// The pitch the view settles to behind a moving vehicle: slightly down.
	float restPitch = -0.12f;
	// Swing back behind the vehicle once the mouse has been left alone and the vehicle is moving.
	bool recentre = true;
	float recentreDelay = 1.5f;     // seconds of no look input
	float recentreRate = 2.0f;      // 1/seconds, at full urgency
	float recentreMinSpeed = 3.0f;  // world units a second
	// The game's own look left / right / behind (L, R, both: Q, E, C) turn this camera too.
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

	// --- Keeping the lens out of the vehicle ---
	// The near clip plane is lowered to a fraction of the camera's clearance from the vehicle's
	// box, through the game's own RwCameraSetNearClipPlane, never above the game's own value.
	bool nearClip = true;
	float nearClipFactor = 0.6f;
	float nearClipMin = 0.05f;
	// If the camera still ends up inside the box, it is lifted out over the top by this much.
	float subjectMargin = 0.12f;
	// The camera's position may only orbit this far below the pivot. Looking further up tilts the
	// view instead of swinging the camera down under the car.
	float orbitUp = 0.35f;

	// --- Handing the view over ---
	float blendIn = 0.25f;          // seconds, from the game's camera to ours
	float blendOut = 0.15f;         // seconds, from ours back to whatever the game switched to
	// Carry the game's camera shake (explosions, rough ground) over onto this camera.
	bool keepShake = true;
};

LCSChaseCamTuning &ChaseCamTuning();

// Put both hooks in place, or take them out when the setting is off. Called every tick from
// LCS::Tick, and cheap once installed: it re-checks that the patch is still there, because
// loading a savestate replaces PSP memory underneath it.
void InstallChaseCam();

// Put the game's instructions back and give the block back. Called from LCS::Shutdown.
void RemoveChaseCam();

// Whether the hooks are in: mouse look in a vehicle goes here when they are.
bool ChaseCamInstalled();

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
