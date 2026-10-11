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
#include <cstdlib>
#include <iterator>
#include <optional>

#include "Common/Log.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSFreeAim.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSSettings.h"

namespace LCS {

namespace {

const float kTwoPi = 6.28318531f;

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

bool Normalize(Vec3 *v) {
	const float len = Length(*v);
	if (!(len > 1e-5f)) {
		return false;
	}
	*v = Mul(*v, 1.0f / len);
	return true;
}

float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

std::optional<Vec3> ReadVec(u32 address) {
	const std::optional<float> x = ReadFloat(address);
	const std::optional<float> y = ReadFloat(address + 4);
	const std::optional<float> z = ReadFloat(address + 8);
	if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
		return std::nullopt;
	}
	return Vec3{*x, *y, *z};
}

bool WriteVec(u32 address, Vec3 v) {
	return WriteFloat(address, v.x) && WriteFloat(address + 4, v.y) && WriteFloat(address + 8, v.z);
}

u32 ActiveCam() {
	u8 index = ReadU8(kLCSTheCamera + kLCSCamActiveOffset).value_or(0);
	if (index > 2) {
		index = 0;
	}
	return kLCSTheCamera + kLCSCamsOffset + index * kLCSCamStride;
}

u32 PlayerPed() {
	return ReadAddrU32(LCSAddr::PlayerBase).value_or(0);
}

int EntityType(u32 entity) {
	const std::optional<u32> flags = ReadU32(entity + kLCSEntityFlagsOffset);
	return flags ? (int)((*flags & 0xe) >> 1) : -1;
}

int CurrentWeaponType(u32 ped) {
	const std::optional<u8> slot = ReadU8(ped + 0x6b8);
	if (!slot || *slot >= kLCSWeaponSlots) {
		return -1;
	}
	const std::optional<u32> type = ReadU32(ped + kLCSPedWeaponsOffset + *slot * kLCSWeaponStride);
	return type && *type < 64 ? (int)*type : -1;
}

std::optional<u32> WeaponInfo(int type) {
	if (type < 0) {
		return std::nullopt;
	}
	const u8 select = ReadU8(kLCSWeaponInfoSelect).value_or(0);
	const std::optional<u32> pair = ReadPointer(kLCSWeaponInfoTables);
	const std::optional<u32> table = pair ? ReadPointer(*pair + (select ? 0 : 4)) : std::nullopt;
	if (!table) {
		return std::nullopt;
	}
	return *table + (u32)type * kLCSWeaponInfoStride;
}

// --- Installation ---
bool g_installed = false;
const char *g_status = "not installed";

// --- The aim ---
bool g_active = false;
bool g_holding = false;
float g_yaw = 0.0f;      // as the game's Beta: the view looks along (-cos, -sin)
float g_pitch = 0.0f;    // as the game's Alpha: up is positive
u32 g_target = 0;
float g_targetDistance = 0.0f;

// --- The shot ---
u64 g_seen = 0;
u64 g_redirected = 0;
float g_lastRange = 0.0f;
int g_lastWeaponType = -1;
bool g_restorePending = false;
u32 g_restoreAddr = 0;
Vec3 g_restoreSource;
// Where the last redirected shot hit, and how far that is from the camera's centre line: the check
// that the bullet went where the reticle is, read off the game's own CColPoint.
u32 g_colPointAddr = 0;
Vec3 g_rayOrigin;
Vec3 g_rayDir;
bool g_haveHit = false;
Vec3 g_lastHit;
float g_lastHitOffLine = 0.0f;

// --- The scope ---
bool g_scopeActive = false;
int g_scopeMode = -1;
float g_scopeFov = 0.0f;
float g_scopeZoomTarget = 0.0f;
u64 g_scopeFrames = 0;

// One wheel notch narrows or widens the scope's view by this factor: the sniper's whole range,
// 70 to 15 degrees, in seven.
const float kScopeZoomStep = 1.25f;

// LCS_CAM_TRACE=1 (the chase camera's hatch) also logs every redirected shot and where it hit.
bool Trace() {
	static const bool trace = [] {
		const char *v = getenv("LCS_CAM_TRACE");
		return v && atoi(v) != 0;
	}();
	return trace;
}

int ReplacementIndex(const char *name) {
	return GetReplacementFuncIndexByName(name);
}

// One site: the game's op or our replacement, nothing else. Through Read_Instruction, since the
// raw word of code that has run is a JIT block marker.
bool HookSite(u32 address, u32 stockOp, int index) {
	const u32 replaceOp = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)index;
	const u32 now = Memory::Read_Instruction(address, false).encoding;
	if (now == replaceOp) {
		return true;
	}
	if (now != stockOp) {
		return false;
	}
	currentMIPS->InvalidateICacheRangeImmediate(address, 4);
	WriteReplaceInstructionAt(address, index);
	currentMIPS->InvalidateICacheRangeDeferred(address, 4);
	return Memory::Read_Instruction(address, false).encoding == replaceOp;
}

void UnhookSite(u32 address, u32 stockOp) {
	currentMIPS->InvalidateICacheRangeImmediate(address, 4);
	RestoreReplacedInstruction(address);
	// Written outright: restoring an instruction this session never saved writes a nop.
	Memory::WriteUnchecked_U32(stockOp, address);
	currentMIPS->InvalidateICacheRangeDeferred(address, 4);
}

void Unhook() {
	if (!g_installed || !currentMIPS) {
		return;
	}
	UnhookSite(kLCSFireRaycastCall, kLCSFireRaycastCallOp);
	UnhookSite(kLCSFireRaycastDone, kLCSFireRaycastDoneOp);
	g_installed = false;
	g_restorePending = false;
}

// The pivot mode 11 orbits (0x088ebad8), estimated from the ped, then put onto the camera's live
// centre line, which passes through the real one: what is left of the estimate's error is along
// the line, where it changes no direction. The estimate misses only the per-weapon 0.15 and the
// building pull-back.
std::optional<Vec3> AimPivot(u32 ped, u32 cam) {
	const std::optional<Vec3> pos = ReadVec(ped + kLCSEntityPositionOffset);
	const std::optional<Vec3> right = ReadVec(ped + kLCSEntityRightOffset);
	const std::optional<Vec3> source = ReadVec(cam + kLCSCamSourceOffset);
	std::optional<Vec3> front = ReadVec(cam + kLCSCamFrontOffset);
	const float fov = ReadFloat(cam + kLCSCamFOVOffset).value_or(70.0f);
	if (!pos || !right || !source || !front || !Normalize(&*front)) {
		return std::nullopt;
	}
	const float zoom = Clamp(70.0f - fov, 0.0f, 70.0f);
	const float up = kLCSAimPivotHeight + 0.1f * std::min(1.0f, zoom / 20.0f);
	const float side = kLCSAimPivotShoulder + 0.1f * std::min(1.0f, zoom / 35.0f);
	const Vec3 estimate = Add(Add(*pos, Mul(*right, side)), Vec3{0.0f, 0.0f, up});
	return Add(*source, Mul(*front, Dot(Sub(estimate, *source), *front)));
}

}  // namespace

LCSFreeAimTuning &FreeAimTuning() {
	static LCSFreeAimTuning tuning;
	return tuning;
}

bool PlayerHasFirearm() {
	const u32 ped = PlayerPed();
	const int type = ped ? CurrentWeaponType(ped) : -1;
	const std::optional<u32> info = WeaponInfo(type);
	// Fire type 1 is instant hit, the guns. 0 is melee; the rest throw, launch or spray.
	return info && ReadU32(*info).value_or(0) == 1;
}

bool FreeAimActive() {
	const u32 ped = PlayerPed();
	if (!ped || ReadAddrBool(LCSAddr::PlayerInVehicle).value_or(false)) {
		return false;
	}
	if (ReadU8(ped + kLCSPedFreeAimingOffset).value_or(0) == 0) {
		return false;
	}
	if ((int)ReadU16(ActiveCam() + kLCSCamModeOffset).value_or(0xffff) != kLCSCamModeFreeAim) {
		return false;
	}
	const u32 target = ReadU32(ped + kLCSPedAimTargetOffset).value_or(0);
	return target != 0 && Memory::IsValidAddress(target) && EntityType(target) == (int)kLCSEntityTypeObject;
}

bool FreeAimLook(float dx, float dy) {
	g_active = FreeAimActive();
	if (!g_active) {
		g_holding = false;
		g_target = 0;
		return false;
	}
	const u32 ped = PlayerPed();
	const u32 cam = ActiveCam();

	if (!g_holding) {
		// From wherever the game has the aim, so entering free aim never jumps.
		const std::optional<float> beta = ReadFloat(cam + kLCSCamBetaOffset);
		const std::optional<float> alpha = ReadFloat(cam + kLCSCamAlphaOffset);
		if (!beta || !alpha) {
			return true;
		}
		g_yaw = *beta;
		g_pitch = *alpha;
		g_holding = true;
	}

	// The aim's own sensitivity and sense (the Aiming page), scaled by the zoom so a count is the
	// same distance on screen: mode 11 narrows the view to 50 degrees.
	const LCSSettings &s = Settings();
	const float fov = ReadFloat(cam + kLCSCamFOVOffset).value_or(70.0f);
	const float scale = s.aimSensitivity * Clamp(fov / 70.0f, 0.2f, 2.0f);
	const float ySign = s.invertAimY ? -1.0f : 1.0f;
	g_yaw = std::fmod(g_yaw - dx * scale, kTwoPi);
	if (g_yaw < 0.0f) {
		g_yaw += kTwoPi;
	}
	g_pitch = Clamp(g_pitch - dy * scale * ySign, kLCSFreeAimPitchMin, kLCSFreeAimPitchMax);

	// The camera now, so there is no turn-rate lag...
	WriteFloat(cam + kLCSCamBetaOffset, g_yaw);
	WriteFloat(cam + kLCSCamAlphaOffset, g_pitch);

	// ...and the gun target along the same aim, so the camera's turn toward it has nothing to do.
	// Front exactly as mode 11 builds it from the angles.
	const Vec3 front{-std::cos(g_yaw) * std::cos(g_pitch), -std::sin(g_yaw) * std::cos(g_pitch), std::sin(g_pitch)};
	const u32 target = ReadU32(ped + kLCSPedAimTargetOffset).value_or(0);
	const std::optional<Vec3> pivot = AimPivot(ped, cam);
	const std::optional<Vec3> targetPos = target ? ReadVec(target + kLCSEntityPositionOffset) : std::nullopt;
	g_target = target;
	if (!pivot || !targetPos) {
		return true;
	}
	// Keep the game's distance (it re-projects the target onto what the aim hits every frame).
	float distance = Length(Sub(*targetPos, *pivot));
	if (!(distance > 2.0f && distance < 500.0f)) {
		distance = FreeAimTuning().targetDistance;
	}
	g_targetDistance = distance;
	WriteVec(target + kLCSEntityPositionOffset, Add(*pivot, Mul(front, distance)));
	return true;
}

bool PlayerHasScopedWeapon() {
	const u32 ped = PlayerPed();
	return ped && ReadU8(ped + 0x6b8).value_or(0xff) == kLCSWeaponSlotSniper && CurrentWeaponType(ped) > 0;
}

bool ScopeActive() {
	const u32 ped = PlayerPed();
	if (!ped || ReadAddrBool(LCSAddr::PlayerInVehicle).value_or(false)) {
		return false;
	}
	const int mode = (int)ReadU16(ActiveCam() + kLCSCamModeOffset).value_or(0xffff);
	return std::find(std::begin(kLCSCamModesScoped), std::end(kLCSCamModesScoped), mode) != std::end(kLCSCamModesScoped);
}

bool ScopeLook(float dx, float dy, int zoomNotches) {
	g_scopeActive = ScopeActive();
	if (!g_scopeActive) {
		g_scopeMode = -1;
		return false;
	}
	const u32 cam = ActiveCam();
	g_scopeMode = (int)ReadU16(cam + kLCSCamModeOffset).value_or(0xffff);
	const std::optional<float> fov = ReadFloat(cam + kLCSCamFOVOffset);
	g_scopeFov = fov.value_or(0.0f);
	g_scopeZoomTarget = ReadFloat(kLCSScopeZoomTarget).value_or(0.0f);

	// The wheel, through the zoom target, so the game eases the view there as it does after Square
	// or Cross. Only the two modes that zoom; the others keep 70 and the notch is spent.
	if (zoomNotches != 0 && fov && (g_scopeMode == kLCSCamModeSniper || g_scopeMode == kLCSCamModeCamera)) {
		const float from = g_scopeZoomTarget > 0.0f ? g_scopeZoomTarget : *fov;
		const float minFov = g_scopeMode == kLCSCamModeCamera ? kLCSScopeFOVMinCamera : kLCSScopeFOVMin;
		g_scopeZoomTarget = Clamp(from / std::pow(kScopeZoomStep, (float)zoomNotches), minFov, kLCSScopeFOVMax);
		WriteFloat(kLCSScopeZoomTarget, g_scopeZoomTarget);
	}

	if (dx == 0.0f && dy == 0.0f) {
		return true;
	}
	const std::optional<float> beta = ReadFloat(cam + kLCSCamBetaOffset);
	const std::optional<float> alpha = ReadFloat(cam + kLCSCamAlphaOffset);
	if (!beta || !alpha) {
		return true;
	}
	// A change on what the camera holds, as the stick's would be. Scaled by the zoom like the aim, so
	// a count moves the picture the same distance at any zoom.
	const LCSSettings &s = Settings();
	const float scale = s.scopedSensitivity * Clamp(fov.value_or(70.0f) / 70.0f, 0.02f, 2.0f);
	const float ySign = s.invertAimY ? -1.0f : 1.0f;
	// Front is (cos(Beta), sin(Beta)) here, so the mouse to the right lowers Beta. Not wrapped: the
	// game wraps it on foot, and a turret mounted on a vehicle keeps it relative and clamps it.
	WriteFloat(cam + kLCSCamBetaOffset, *beta - dx * scale);
	const float pitchMin = g_scopeMode == kLCSCamModeHeliCannon ? kLCSScopePitchMinHeliCannon : kLCSScopePitchMin;
	WriteFloat(cam + kLCSCamAlphaOffset, Clamp(*alpha - dy * scale * ySign, pitchMin, kLCSScopePitchMax));
	if (g_scopeFrames++ == 0) {
		NOTICE_LOG(Log::HLE, "LCS: the mouse aimed the scope (camera mode %d, FOV %.1f)", g_scopeMode, fov.value_or(0.0f));
	}
	return true;
}

int Hook_lcs_fire_raycast() {
	if (!g_installed || !IsActive() || !FreeAimTuning().redirectShots) {
		return 0;
	}
	// The shooter is FireInstantHit's second argument, in $t1 for the wrapper. NPCs shoot through
	// here too, and they do it while the player aims.
	const u32 ped = PlayerPed();
	if (!ped || currentMIPS->r[MIPS_REG_T1] != ped) {
		return 0;
	}
	g_seen++;
	if (!FreeAimActive()) {
		return 0;
	}

	const u32 srcPtr = currentMIPS->r[MIPS_REG_A0];
	const u32 dstPtr = currentMIPS->r[MIPS_REG_A1];
	const std::optional<Vec3> source = ReadVec(srcPtr);
	const std::optional<Vec3> target = ReadVec(dstPtr);
	const u32 cam = ActiveCam();
	const std::optional<Vec3> camPos = ReadVec(cam + kLCSCamSourceOffset);
	std::optional<Vec3> dir = ReadVec(cam + kLCSCamFrontOffset);
	if (!source || !target || !camPos || !dir || !Normalize(&*dir)) {
		return 0;
	}

	// The weapon's own range, not the length of the game's ray: that is the distance to whatever
	// the gun target sits on.
	const int type = CurrentWeaponType(ped);
	const std::optional<u32> info = WeaponInfo(type);
	float range = info ? ReadFloat(*info + kLCSWeaponInfoRangeOffset).value_or(0.0f) : 0.0f;
	if (!(range > 1.0f && range < 1000.0f)) {
		range = std::max(FreeAimTuning().fallbackRange, Length(Sub(*target, *source)));
	}
	g_lastRange = range;
	g_lastWeaponType = type;

	// The reticle is the middle of the screen, so the bullet goes down the camera's centre line,
	// started level with the muzzle (re3's Find3rdPersonCamTargetVector does the same): from the
	// camera it could hit something between the lens and the player.
	const Vec3 origin = Add(*camPos, Mul(*dir, Dot(Sub(*source, *camPos), *dir)));
	// Borrowed for the raycast only, and the restore armed before the write: FireInstantHit draws
	// the flash and the tracer from this vector afterwards.
	g_restoreAddr = srcPtr;
	g_restoreSource = *source;
	g_restorePending = true;
	g_colPointAddr = currentMIPS->r[MIPS_REG_A2];
	g_rayOrigin = *camPos;
	g_rayDir = *dir;
	WriteVec(srcPtr, origin);
	if (WriteVec(dstPtr, Add(origin, Mul(*dir, range)))) {
		g_redirected++;
		static bool announced = false;
		if (!announced) {
			announced = true;
			NOTICE_LOG(Log::HLE, "LCS: a free-aim shot went down the camera's centre line (weapon %d, range %.0f)", type, range);
		}
	}
	return 0;
}

int Hook_lcs_fire_raycast_done() {
	if (g_restorePending) {
		g_restorePending = false;
		WriteVec(g_restoreAddr, g_restoreSource);
		// $v0 is ProcessLineOfSight's answer here; the CColPoint starts with the point.
		const std::optional<Vec3> hit = currentMIPS->r[MIPS_REG_V0] != 0 ? ReadVec(g_colPointAddr) : std::nullopt;
		g_haveHit = hit.has_value();
		if (hit) {
			g_lastHit = *hit;
			const Vec3 d = Sub(*hit, g_rayOrigin);
			g_lastHitOffLine = Length(Sub(d, Mul(g_rayDir, Dot(d, g_rayDir))));
		}
		if (Trace()) {
			NOTICE_LOG(Log::HLE, "LCS aim: shot %llu, weapon %d, range %.0f, %s %.2f %.2f %.2f, %.3f off the centre line, %.2f along",
				(unsigned long long)g_redirected, g_lastWeaponType, g_lastRange, hit ? "hit" : "no hit",
				g_lastHit.x, g_lastHit.y, g_lastHit.z, hit ? g_lastHitOffLine : 0.0f,
				hit ? Dot(Sub(g_lastHit, g_rayOrigin), g_rayDir) : 0.0f);
		}
	}
	return 0;
}

void InstallFreeAim() {
	if (!IsActive() || !currentMIPS) {
		return;
	}
	if (!Settings().freeAim) {
		if (g_installed) {
			Unhook();
			g_status = "switched off";
		}
		return;
	}
	const int callIndex = ReplacementIndex("lcs_fire_raycast");
	const int doneIndex = ReplacementIndex("lcs_fire_raycast_done");
	if (callIndex < 0 || doneIndex < 0) {
		g_status = "no replacement entries";
		return;
	}
	const u32 callOp = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)callIndex;
	const u32 doneOp = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)doneIndex;
	if (g_installed && Memory::Read_Instruction(kLCSFireRaycastCall, false).encoding == callOp &&
	    Memory::Read_Instruction(kLCSFireRaycastDone, false).encoding == doneOp) {
		return;
	}
	if (g_installed) {
		// A savestate keeps code but not replacement ops.
		NOTICE_LOG(Log::HLE, "LCS: free-aim hooks changed (a savestate?) - reinstalling");
		g_installed = false;
		g_restorePending = false;
	}
	// The restore first: a redirect with no restore behind it would leave the flash at the camera.
	if (!HookSite(kLCSFireRaycastDone, kLCSFireRaycastDoneOp, doneIndex)) {
		g_status = "the raycast wrapper is not the one measured";
		return;
	}
	if (!HookSite(kLCSFireRaycastCall, kLCSFireRaycastCallOp, callIndex)) {
		g_status = "the fire site is not the one measured";
		return;
	}
	g_installed = true;
	g_status = "installed";
	NOTICE_LOG(Log::HLE, "LCS: free-aim fire hook installed at %08x, restore at %08x", kLCSFireRaycastCall, kLCSFireRaycastDone);
}

void RemoveFreeAim() {
	Unhook();
	g_status = "not installed";
	g_active = false;
	g_holding = false;
	g_seen = 0;
	g_redirected = 0;
	g_scopeActive = false;
	g_scopeMode = -1;
	g_scopeFrames = 0;
}

void FreeAimGetStats(LCSFreeAimStats *out) {
	out->status = g_status;
	out->installed = g_installed;
	out->active = g_active;
	out->holding = g_holding;
	out->yaw = g_yaw;
	out->pitch = g_pitch;
	out->target = g_target;
	out->targetDistance = g_targetDistance;
	out->seen = g_seen;
	out->redirected = g_redirected;
	out->lastRange = g_lastRange;
	out->lastWeaponType = g_lastWeaponType;
	out->haveHit = g_haveHit;
	out->lastHit[0] = g_lastHit.x;
	out->lastHit[1] = g_lastHit.y;
	out->lastHit[2] = g_lastHit.z;
	out->lastHitOffLine = g_lastHitOffLine;
	out->scopeActive = g_scopeActive;
	out->scopeMode = g_scopeMode;
	out->scopeFov = g_scopeFov;
	out->scopeZoomTarget = g_scopeZoomTarget;
	out->scopeFrames = g_scopeFrames;
}

}  // namespace LCS
