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

#include <algorithm>
#include <cmath>

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

SunState ReadSun() {
	SunState sun;
	const std::optional<u8> hours = ReadU8(kLCSClockHours);
	const std::optional<u8> minutes = ReadU8(kLCSClockMinutes);
	if (!hours || !minutes || *hours > 23 || *minutes > 59) {
		return sun;
	}
	const float angle = 2.0f * 3.14159265f * (float)(*minutes + 60 * *hours) / (24.0f * 60.0f);
	const float v[3] = { sinf(angle), 1.0f, 0.2f - cosf(angle) };
	const float inv = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (int c = 0; c < 3; c++) {
		sun.dir[c] = v[c] * inv;
	}
	sun.valid = true;
	for (int c = 0; c < 3; c++) {
		const std::optional<float> k = ReadFloat(kLCSTimeCycleLight + 4 * c);
		// Clamped, and NaN refused: a renderer multiplies by it every frame.
		if (k && *k >= 0.0f && *k <= 4.0f) {
			sun.colour[c] = std::min(*k, 1.0f);
		}
	}
	return sun;
}

WeatherState ReadWeather() {
	WeatherState w;
	const std::optional<u16> oldType = ReadU16(kLCSWeatherOld);
	const std::optional<u16> newType = ReadU16(kLCSWeatherNew);
	const std::optional<u16> forced = ReadU16(kLCSWeatherForced);
	if (const std::optional<float> interp = ReadFloat(kLCSWeatherInterp)) {
		w.interp = *interp;
	}
	w.oldType = (s16)oldType.value_or(0);
	w.newType = (s16)newType.value_or(0);
	w.forcedType = (s16)forced.value_or(0xffff);
	if (kLCSWeatherRain == 0) {
		return w;
	}
	const std::optional<float> rain = ReadFloat(kLCSWeatherRain);
	if (!rain) {
		return w;
	}
	w.valid = true;
	// Clamped: the one value a renderer multiplies by every frame, and a wrong address that reads
	// 1e30 would be a white screen rather than a wrong number.
	w.rain = *rain > 0.0f ? std::min(*rain, 1.0f) : 0.0f;
	return w;
}

std::optional<float> CameraWorld(int axis) {
	if (axis < 0 || axis > 2) {
		return std::nullopt;
	}
	return ReadFloat(kLCSTheCamera + kLCSCameraPosOffset + (u32)axis * 4);
}

std::optional<float> PlayerWorld(int axis) {
	const std::optional<u32> ped = ReadU32(LookupAddr(LCSAddr::PlayerBase).address);
	if (axis < 0 || axis > 2 || !ped || !*ped) {
		return std::nullopt;
	}
	return ReadFloat(*ped + kLCSEntityPositionOffset + (u32)axis * 4);
}

const LCSState &GetState() {
	return g_state;
}

void ClearState() {
	g_state = LCSState();
}

}  // namespace LCS
