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

const char *VehicleClassName(VehicleClass klass) {
	switch (klass) {
	case VehicleClass::Car: return "car";
	case VehicleClass::Bike: return "bike";
	case VehicleClass::Heli: return "heli";
	case VehicleClass::Boat: return "boat";
	case VehicleClass::Plane: return "plane";
	default: return "unknown";
	}
}

// The same test as GetVehicleAppearance (0x088a08c4).
static VehicleClass ReadVehicleClass(u32 vehicle) {
	const std::optional<u32> handling = ReadPointer(vehicle + kLCSVehicleHandlingOffset);
	const std::optional<u32> flags = handling ? ReadU32(*handling + kLCSHandlingFlagsOffset) : std::nullopt;
	if (!flags) {
		return VehicleClass::Unknown;
	}
	switch (*flags & kLCSHandlingTypeMask) {
	case 0: return VehicleClass::Car;
	case kLCSHandlingIsBike: return VehicleClass::Bike;
	case kLCSHandlingIsHeli: return VehicleClass::Heli;
	case kLCSHandlingIsPlane: return VehicleClass::Plane;
	case kLCSHandlingIsBoat: return VehicleClass::Boat;
	default: return VehicleClass::Unknown;
	}
}

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
				state->vehicleClass = ReadVehicleClass(*state->playerVehicle);
			}
		}

		state->weaponSlot = ReadAddrAsU32(LCSAddr::WeaponSlot);
		if (state->weaponSlot && *state->weaponSlot < kLCSWeaponSlots) {
			state->weaponType = ReadU32(*state->playerBase + kLCSPedWeaponsOffset + *state->weaponSlot * kLCSWeaponStride);
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
