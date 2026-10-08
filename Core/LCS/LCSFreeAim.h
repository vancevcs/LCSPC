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

// Mouse free aim on foot. docs/lcs/free-aim.md has the measurements; the VCS fork's VCSFireHook and
// the aim half of VCSCamera are the same design, and docs/vcs/aiming.md is why it is shaped this way.
//
// LCS's own free aim (hold R, tap d-pad down: camera mode 11, ped+0xc9d set) is driven by a
// placeholder object the ped points its gun at (ped+0x6bc). The game places it along the camera's
// Front from the aim pivot, the camera turns toward it at 0.1 * TimeStep rad a frame, and the ped
// turns with it. A written Beta alone is pulled back to it within a frame or two; a written target
// is kept. So, as in VCS:
//
//   - the mouse integrates its own yaw and pitch, and at the frame boundary writes them into the
//     camera's Beta / Alpha (no turn-rate lag) AND puts the target along that aim from the pivot
//     (so the camera's turn toward the target has nothing to do, and nothing pulls back);
//   - a HOOKENTER at the player's raycast in CWeapon::FireInstantHit sends the bullet down the
//     camera's centre line, where the reticle is, starting level with the muzzle; a second hook
//     after the raycast gives the game its own source back, so the flash and tracer stay at the gun.
//
// Threads: everything here runs on the emu thread (the hooks inside the game's frame, the look from
// LCS::OnFrameBoundary, the install from LCS::Tick).

namespace LCS {

struct LCSFreeAimTuning {
	// Send the player's shots down the camera's centre line in free aim. Off leaves the game's own
	// shot, from the gun to the gun target.
	bool redirectShots = true;
	// Used when the weapon's own range (CWeaponInfo + 4) can't be read, and as a floor.
	float fallbackRange = 60.0f;
	// The target's distance from the pivot when the game's current one is unusable.
	float targetDistance = 40.0f;
};

LCSFreeAimTuning &FreeAimTuning();

// Hooks in or out, re-checked every tick like the chase camera's. From LCS::Tick.
void InstallFreeAim();
// From LCS::Shutdown.
void RemoveFreeAim();

// Whether the player is in the game's free aim on foot right now: mode 11 with ped+0xc9d set and
// a placeholder (an object) as the gun target. Lock-on onto a ped is not free aim.
bool FreeAimActive();
// Whether the player's gun fires something the free aim can take: not melee, not unarmed.
bool PlayerHasFirearm();

// The mouse on foot, in counts, from the frame boundary, every frame (with zeros too: the aim is held
// every frame). True when free aim took it; false outside free aim, which also lets go of the aim.
bool FreeAimLook(float dx, float dy);

// The two replacement entries in ReplaceTables.cpp.
int Hook_lcs_fire_raycast();
int Hook_lcs_fire_raycast_done();

struct LCSFreeAimStats {
	const char *status = "";
	bool installed = false;
	bool active = false;
	bool holding = false;
	float yaw = 0.0f;       // as the game's Beta
	float pitch = 0.0f;     // as the game's Alpha
	u32 target = 0;
	float targetDistance = 0.0f;
	u64 seen = 0;           // the player's raycasts the hook saw
	u64 redirected = 0;
	float lastRange = 0.0f;
	int lastWeaponType = -1;
	bool haveHit = false;        // the last redirected shot hit something
	float lastHit[3] = {};
	float lastHitOffLine = 0.0f; // its distance from the camera's centre line
};
void FreeAimGetStats(LCSFreeAimStats *out);

}  // namespace LCS
