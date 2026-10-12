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

// GetVehicleAppearance's classes (0x088a08c4), from the handling flags. Not m_vehType.
enum class VehicleClass {
	Unknown,
	Car,
	Bike,
	Heli,
	Boat,
	Plane,
};

const char *VehicleClassName(VehicleClass klass);

struct LCSState {
	std::optional<u32> playerBase;
	std::optional<float> health;

	std::optional<bool> inVehicle;
	// Only set while inVehicle is true: the ped's vehicle pointer goes stale on foot.
	std::optional<u32> playerVehicle;
	// CVehicle::m_vehType of playerVehicle, raw (2 is a train).
	std::optional<u32> vehicleType;
	VehicleClass vehicleClass = VehicleClass::Unknown;

	// The selected slot (0-9) and the weapon type in it.
	std::optional<u32> weaponSlot;
	std::optional<u32> weaponType;

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

// The weather, for the renderer's passes (the shadows thin in the rain, the roads go wet). Read
// fresh, emu thread. `valid` false when the rain can't be read: dry roads, not permanent rain.
struct WeatherState {
	bool valid = false;
	float rain = 0.0f;      // 0..1, clamped
	int oldType = 0;
	int newType = 0;
	float interp = 0.0f;
	int forcedType = -1;    // the script's, or -1 released
};
WeatherState ReadWeather();
// The sun, from the clock by GTA III's CTimeCycle formula, normalise(sin a, 1, 0.2 - cos a) with a
// the day's angle from midnight: the game's directional light frame points along exactly its
// negation at every hour measured. Toward the sun, Z up; below the horizon at night. LCS hands
// the GE no time-of-day light (its lit draws use one fixed direction), so this is the only sun
// there is. `colour` is the time cycle's light, 0..1. Emu thread.
struct SunState {
	bool valid = false;
	float dir[3] = {};
	float colour[3] = { 1.0f, 1.0f, 1.0f };
};
SunState ReadSun();
// TheCamera's and the player's positions in world units, axis 0..2. Emu thread.
std::optional<float> CameraWorld(int axis);
std::optional<float> PlayerWorld(int axis);

// The state as of the last LCS::Tick. Tick calls UpdateSharedState; everything else reads.
void UpdateSharedState();
const LCSState &GetState();
void ClearState();

}  // namespace LCS
