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

#include <optional>

#include "Common/CommonTypes.h"

// Game state decoded once per vblank. Every field is optional: "not found" and "found, zero" are
// different answers, and an unset address must never read as a plausible value.

namespace LCS {

struct LCSState {
	std::optional<u32> playerBase;
	std::optional<float> health;

	std::optional<bool> inVehicle;
	// Only set while inVehicle is true: the ped's vehicle pointer goes stale on foot.
	std::optional<u32> playerVehicle;
	// CVehicle::m_vehType of playerVehicle. Raw value; which number is which class is unconfirmed.
	std::optional<u32> vehicleType;

	std::optional<u32> activeCam;
	std::optional<u32> camMode;
	std::optional<float> camYaw;
	std::optional<float> camPitch;
	std::optional<float> camFOV;

	std::optional<u32> timeInMs;
	std::optional<float> timeStep;
	std::optional<u32> frameCounter;

	// True if anything at all was read this tick.
	bool anyValid = false;
};

void UpdateState(LCSState *state);

// The state as of the last LCS::Tick. Tick calls UpdateSharedState; everything else reads.
void UpdateSharedState();
const LCSState &GetState();
void ClearState();

}  // namespace LCS
