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

#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSState.h"

namespace LCS {

static LCSState g_state;

void UpdateState(LCSState *state) {
	*state = LCSState();

	state->playerBase = ReadPointer(LookupAddr(LCSAddr::PlayerBase).address);
	if (state->playerBase) {
		state->health = ReadAddrFloat(LCSAddr::PlayerHealth);
		state->inVehicle = ReadAddrBool(LCSAddr::PlayerInVehicle);
		if (state->inVehicle.value_or(false)) {
			const std::optional<u32> vehicle = ResolveAddr(LCSAddr::PlayerVehicle);
			state->playerVehicle = vehicle ? ReadPointer(*vehicle) : std::nullopt;
			if (state->playerVehicle) {
				state->vehicleType = ReadU32(*state->playerVehicle + kLCSVehicleTypeOffset);
			}
		}
	}

	state->activeCam = ReadAddrAsU32(LCSAddr::ActiveCam);
	state->camMode = ReadAddrAsU32(LCSAddr::CamMode);
	state->camYaw = ReadAddrFloat(LCSAddr::CamYaw);
	state->camPitch = ReadAddrFloat(LCSAddr::CamPitch);
	state->camFOV = ReadAddrFloat(LCSAddr::CamFOV);

	state->timeInMs = ReadAddrU32(LCSAddr::TimeInMs);
	state->timeStep = ReadAddrFloat(LCSAddr::TimeStep);
	state->frameCounter = ReadAddrU32(LCSAddr::FrameCounter);

	state->anyValid = state->playerBase || state->timeInMs || state->frameCounter;
}

void UpdateSharedState() {
	UpdateState(&g_state);
}

const LCSState &GetState() {
	return g_state;
}

void ClearState() {
	g_state = LCSState();
}

}  // namespace LCS
