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
#include <optional>
#include <vector>

#include "Common/Log.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSChaseCam.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSMips.h"
#include "Core/LCS/LCSSettings.h"

namespace LCS {

namespace {

const float kPi = 3.14159265f;
const float kTwoPi = 6.28318531f;

struct CamVec {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

CamVec Add(CamVec a, CamVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CamVec Sub(CamVec a, CamVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CamVec Mul(CamVec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(CamVec a, CamVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(CamVec a) { return std::sqrt(Dot(a, a)); }
CamVec Lerp(CamVec a, CamVec b, float t) { return Add(a, Mul(Sub(b, a), t)); }

CamVec Normalize(CamVec a, CamVec fallback) {
	const float len = Length(a);
	return len > 1e-5f ? Mul(a, 1.0f / len) : fallback;
}

// `up` made exactly perpendicular to `front`. The game builds its matrix from Front and Up and
// takes Up as it is (0x0890232c), so a skewed one would shear the view.
CamVec Orthonormal(CamVec up, CamVec front, CamVec fallback) {
	return Normalize(Sub(up, Mul(front, Dot(up, front))), fallback);
}

float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

float WrapPi(float a) {
	a = std::fmod(a + kPi, kTwoPi);
	if (a < 0.0f) {
		a += kTwoPi;
	}
	return a - kPi;
}

float SmoothStep(float t) {
	t = Clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// The fraction of a gap an exponential ease closes in dt.
float EaseFactor(float rate, float dt) {
	return rate > 0.0f ? 1.0f - std::exp(-rate * dt) : 1.0f;
}

std::optional<CamVec> ReadVec(u32 address) {
	const std::optional<float> x = ReadFloat(address);
	const std::optional<float> y = ReadFloat(address + 4);
	const std::optional<float> z = ReadFloat(address + 8);
	if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
		return std::nullopt;
	}
	return CamVec{*x, *y, *z};
}

bool WriteVec(u32 address, CamVec v) {
	return WriteFloat(address, v.x) && WriteFloat(address + 4, v.y) && WriteFloat(address + 8, v.z);
}

// Where CCamera::Process keeps this frame's three vectors, from the registers it keeps them in:
// Front in $fp, Up in $s7, Source just below Front. Valid in both hooks: $fp and $s7 are
// callee-saved, and nothing between the two moves them.
struct CamVectors {
	u32 source;
	u32 front;
	u32 up;
};

CamVectors CurrentVectors() {
	const u32 front = currentMIPS->r[MIPS_REG_FP];
	return CamVectors{ front - 0x10, front, currentMIPS->r[MIPS_REG_S7] };
}

// --- The block ------------------------------------------------------------------------------------
//
//   +0x000  magic
//   +0x004  ray count this frame: written by the prepare hook, zero whenever it declines
//   +0x008  ProcessLineOfSight's flags: $t0..$t3, then the five stack words
//   +0x02c  a look command from outside (kOffCommand)
//   +0x038  the near clip plane the finish hook wants, and whether it wants one
//   +0x040  the rays, kRayStride each:
//             +0x00 start   +0x10 end   +0x20 CColPoint   +0x48 entity out   +0x4c hit
//   +0x240  the program
const u32 kMagic = 0x4D41434C;  // "LCAM"
const u32 kOffMagic = 0x000;
const u32 kOffCount = 0x004;
const u32 kOffFlags = 0x008;
// A look command, for a test with nobody at the mouse: write the angles, then the flags, with the
// debugger's memory.write. Bit 0 sets the yaw at +0x30, bit 1 the pitch at +0x34, both radians in
// the camera's convention (ChaseCamAddLook's). The prepare hook clears the flags once it has taken
// them. The block's address is in the line the install logs.
const u32 kOffCommand = 0x02c;
const u32 kOffCommandYaw = 0x030;
const u32 kOffCommandPitch = 0x034;
const u32 kOffNear = 0x038;
const u32 kOffNearSet = 0x03c;
const u32 kOffRays = 0x040;
const u32 kRayStride = 0x60;
const u32 kRayStart = 0x00;
const u32 kRayEnd = 0x10;
const u32 kRayCol = 0x20;
const u32 kRayEntity = 0x48;
const u32 kRayHit = 0x4c;
const int kMaxRays = 5;
const u32 kOffCode = 0x240;
const u32 kBlockSize = 0x400;

// The program the patched jal lands in. At entry $a0..$a2 hold the cross product's arguments and
// $ra points back into CCamera::Process; the program leaves both as it found them and tail-calls
// the cross product, so from the game's side the jal did what it always did.
//
// It keeps nothing but $s0 (the block) and $s1 (the ray counter) of its own, and saves both. The
// caller's $fp and $s7, which locate the three vectors, are callee-saved, so neither this program
// nor ProcessLineOfSight moves them.
std::vector<u32> BuildProgram(u32 block, int *finishIndex) {
	std::vector<u32> c;
	auto emit = [&c](u32 op) {
		c.push_back(op);
		return (int)c.size() - 1;
	};

	emit(Addiu(kRegSP, kRegSP, -0x40));
	emit(Sw(kRegRA, kRegSP, 0x20));
	emit(Sw(kRegA0, kRegSP, 0x24));
	emit(Sw(kRegA1, kRegSP, 0x28));
	emit(Sw(kRegA2, kRegSP, 0x2c));
	emit(Sw(kRegS0, kRegSP, 0x30));
	emit(Sw(kRegS1, kRegSP, 0x34));
	emit(Lui(kRegS0, block >> 16));
	emit(Ori(kRegS0, kRegS0, block & 0xffff));
	emit(Lw(kRegS1, kRegS0, kOffCount));

	const int loop = emit(Nop());  // beq $s1, $zero, done
	emit(Nop());
	emit(Addiu(kRegS1, kRegS1, -1));
	// $t4 = block + s1 * 0x60
	emit(Sll(kRegT4, kRegS1, 5));
	emit(Sll(kRegT5, kRegS1, 6));
	emit(Addu(kRegT4, kRegT4, kRegT5));
	emit(Addu(kRegT4, kRegT4, kRegS0));
	emit(Addiu(kRegA0, kRegT4, (int)(kOffRays + kRayStart)));
	emit(Addiu(kRegA1, kRegT4, (int)(kOffRays + kRayEnd)));
	emit(Addiu(kRegA2, kRegT4, (int)(kOffRays + kRayCol)));
	emit(Addiu(kRegA3, kRegT4, (int)(kOffRays + kRayEntity)));
	emit(Lw(kRegT0, kRegS0, kOffFlags + 0x00));
	emit(Lw(kRegT1, kRegS0, kOffFlags + 0x04));
	emit(Lw(kRegT2, kRegS0, kOffFlags + 0x08));
	emit(Lw(kRegT3, kRegS0, kOffFlags + 0x0c));
	for (int i = 0; i < 4; i++) {
		emit(Lw(kRegV0, kRegS0, (int)kOffFlags + 0x10 + i * 4));
		emit(Sw(kRegV0, kRegSP, i * 4));
	}
	emit(Lw(kRegV0, kRegS0, kOffFlags + 0x20));
	emit(Jal(kLCSProcessLineOfSight));
	emit(Sw(kRegV0, kRegSP, 0x10));  // delay slot: runs before the call, with the fifth flag
	emit(Sll(kRegT4, kRegS1, 5));
	emit(Sll(kRegT5, kRegS1, 6));
	emit(Addu(kRegT4, kRegT4, kRegT5));
	emit(Addu(kRegT4, kRegT4, kRegS0));
	const int back = emit(Nop());  // b loop
	emit(Sw(kRegV0, kRegT4, (int)(kOffRays + kRayHit)));

	const int done = emit(Nop());  // the finish hook replaces this nop
	// The near clip plane, through the game's own setter, on the frames the finish hook asked.
	emit(Lw(kRegT4, kRegS0, kOffNearSet));
	const int noNear = emit(Nop());  // beq $t4, $zero, skip
	emit(Nop());
	// lw sign-extends its offset, and the low half here is above 0x8000: the upper half rounds up.
	emit(Lui(kRegA0, (kLCSSceneCameraPtr + 0x8000) >> 16));
	emit(Lw(kRegA0, kRegA0, (int)(s16)(kLCSSceneCameraPtr & 0xffff)));
	const int noCamera = emit(Nop());  // beq $a0, $zero, skip
	emit(Nop());
	emit(Jal(kLCSRwCameraSetNearClip));
	emit(Lwc1(kRegF12, kRegS0, kOffNear));  // delay slot: the argument
	const int skip = (int)c.size();
	emit(Lw(kRegA0, kRegSP, 0x24));
	emit(Lw(kRegA1, kRegSP, 0x28));
	emit(Lw(kRegA2, kRegSP, 0x2c));
	emit(Lw(kRegS0, kRegSP, 0x30));
	emit(Lw(kRegS1, kRegSP, 0x34));
	emit(Lw(kRegRA, kRegSP, 0x20));
	emit(J(kLCSCamBasisFunc));
	emit(Addiu(kRegSP, kRegSP, 0x40));

	c[loop] = Beq(kRegS1, kRegZero, done - (loop + 1));
	c[back] = B(loop - (back + 1));
	c[noNear] = Beq(kRegT4, kRegZero, skip - (noNear + 1));
	c[noCamera] = Beq(kRegA0, kRegZero, skip - (noCamera + 1));
	*finishIndex = done;
	return c;
}

// --- Installation ---
u32 g_block = 0;
bool g_hooked = false;
u32 g_finishAddr = 0;
const char *g_status = "not installed";

enum class CamKind {
	None,
	OnFoot,
	Vehicle,
};

const char *KindName(CamKind kind) {
	switch (kind) {
	case CamKind::OnFoot: return "on foot";
	case CamKind::Vehicle: return "vehicle";
	default: return "-";
	}
}

// --- The view the player asked for ---
float g_yaw = 0.0f;      // the direction the camera looks: atan2(Front.y, Front.x)
float g_pitch = 0.0f;    // up is positive: asin(Front.z)
float g_sinceLook = 1e3f;

// --- One frame, carried from the prepare hook to the finish hook ---
bool g_prepared = false;
CamKind g_kind = CamKind::None;
int g_mode = -1;
float g_dt = 0.0f;
CamVec g_pivot;
CamVec g_front;
CamVec g_up;
// From the camera to the pivot: the view direction, except where a vehicle's orbit is capped
// (vehicleOrbitUp).
CamVec g_orbit;
float g_clearance = 0.0f;
float g_near = 0.0f;
CamVec g_preSource;
CamVec g_preFront;
CamVec g_preUp;
float g_wantDistance = 0.0f;
int g_rayCount = 0;

// --- Kept between frames ---
bool g_owned = false;
CamKind g_lastKind = CamKind::None;
CamVec g_lastPivot;
CamVec g_pivotOffset;
float g_gameDistance = 0.0f;
float g_distance = 0.0f;
float g_allowed = 0.0f;
int g_hits = 0;
float g_blend = 1.0f;
bool g_wasGlancing = false;
bool g_glancing = false;

// What the finish hook last wrote, which a blend out starts from.
CamVec g_outSource;
CamVec g_outFront;
CamVec g_outUp;
bool g_blendingOut = false;
float g_blendOutT = 0.0f;

u32 g_vehicle = 0;
CamVec g_vehiclePos;
bool g_haveVehiclePos = false;
float g_speed = 0.0f;

u64 g_frames = 0;
u64 g_ownedFrames = 0;

// A dev hatch, read once: LCS_CAM_TRACE=1 writes one NOTICE line a game second (mode, distances,
// hits), which is how a run with nobody watching the screen finds out what the camera did.
bool g_envRead = false;
bool g_trace = false;
float g_traceTimer = 0.0f;

void ReadEnvironment() {
	if (g_envRead) {
		return;
	}
	g_envRead = true;
	if (const char *trace = getenv("LCS_CAM_TRACE")) {
		g_trace = atoi(trace) != 0;
	}
}

bool WantedFor(CamKind kind) {
	const LCSSettings &s = Settings();
	if (!s.keyboardMouse || !s.mouseLook) {
		return false;
	}
	switch (kind) {
	case CamKind::OnFoot: return s.footChaseCamera;
	case CamKind::Vehicle: return s.vehicleChaseCamera;
	default: return false;
	}
}

bool Wanted() {
	return WantedFor(CamKind::OnFoot) || WantedFor(CamKind::Vehicle);
}

void PitchLimits(CamKind kind, float *lo, float *hi) {
	const LCSChaseCamTuning &t = ChaseCamTuning();
	const float up = kind == CamKind::Vehicle ? t.vehicleLookUp : t.footLookUp;
	const float down = kind == CamKind::Vehicle ? t.vehicleLookDown : t.footLookDown;
	// Never quite vertical: Up is built from the pitch and degenerates at +/-90 degrees.
	*lo = -Clamp(down, 0.0f, 1.5f);
	*hi = Clamp(up, 0.0f, 1.5f);
}

// Seconds of game time in the frame being processed. TimeStep is in 50 fps units (1.67 at 30 fps)
// and 0 with the game paused, which correctly stops every ease.
float GameSeconds() {
	const float step = ReadAddrFloat(LCSAddr::TimeStep).value_or(0.0f);
	if (!(step > 0.0f)) {
		return 0.0f;
	}
	return Clamp(step, 0.0f, 5.0f) / 50.0f;
}

u32 ActiveCam() {
	u8 index = ReadU8(kLCSTheCamera + kLCSCamActiveOffset).value_or(0);
	if (index > 2) {
		index = 0;
	}
	return kLCSTheCamera + kLCSCamsOffset + index * kLCSCamStride;
}

// The vehicle's collision box: its top for the pivot, its length for the distance.
struct VehicleBox {
	CamVec min{-1.0f, -2.25f, -0.6f};
	CamVec max{1.0f, 2.25f, 1.0f};
	float top = 1.0f;
	float length = 4.5f;
	bool fromGame = false;
};

std::optional<VehicleBox> ReadVehicleBox(u32 vehicle) {
	const std::optional<u16> model = ReadU16(vehicle + kLCSEntityModelOffset);
	const std::optional<u32> table = ReadPointer(kLCSModelInfoTablePtr);
	const std::optional<u32> count = ReadU32(kLCSModelInfoCountAddr);
	if (!model || !table || !count || (s16)*model < 0 || *model >= *count) {
		return std::nullopt;
	}
	const std::optional<u32> info = ReadPointer(*table + *model * 4);
	if (!info) {
		return std::nullopt;
	}
	const std::optional<u32> col = ReadPointer(*info + kLCSModelInfoColModelOffset);
	if (!col) {
		return std::nullopt;
	}
	const std::optional<CamVec> lo = ReadVec(*col + kLCSColModelBoxMinOffset);
	const std::optional<CamVec> hi = ReadVec(*col + kLCSColModelBoxMaxOffset);
	if (!lo || !hi || !(hi->z > 0.1f && hi->z < 12.0f)) {
		return std::nullopt;
	}
	const float length = hi->y - lo->y;
	if (!(length > 0.3f && length < 60.0f) || !(hi->x > lo->x) || !(hi->z > lo->z)) {
		return std::nullopt;
	}
	return VehicleBox{*lo, *hi, hi->z, length, true};
}

// What the camera must stay out of this frame: the player, or the player's vehicle. Set by the
// prepare hook, read by the finish hook.
CamVec g_subjectPos;
CamVec g_vehRight{1.0f, 0.0f, 0.0f};
CamVec g_vehForward{0.0f, 1.0f, 0.0f};
CamVec g_vehUp{0.0f, 0.0f, 1.0f};
VehicleBox g_box;

// The player's own vehicle while the player is inside its box, getting in or out: the rays start
// at the player, so on foot every one of them hit it and the camera snapped in to its minimum. Its
// hits are skipped while this is set; a car the player stands next to still counts.
u32 g_ignoreEntity = 0;

bool InsideEntityBox(u32 entity, CamVec p, float margin) {
	const std::optional<CamVec> pos = ReadVec(entity + kLCSEntityPositionOffset);
	const std::optional<CamVec> right = ReadVec(entity + kLCSEntityRightOffset);
	const std::optional<CamVec> forward = ReadVec(entity + kLCSEntityForwardOffset);
	const std::optional<CamVec> up = ReadVec(entity + kLCSEntityUpOffset);
	const std::optional<VehicleBox> box = ReadVehicleBox(entity);
	if (!pos || !right || !forward || !up || !box) {
		return false;
	}
	const CamVec d = Sub(p, *pos);
	const CamVec local{Dot(d, *right), Dot(d, *forward), Dot(d, *up)};
	return local.x > box->min.x - margin && local.x < box->max.x + margin &&
		local.y > box->min.y - margin && local.y < box->max.y + margin &&
		local.z > box->min.z - margin && local.z < box->max.z + margin;
}

CamVec VehicleLocal(CamVec p) {
	const CamVec d = Sub(p, g_subjectPos);
	return CamVec{Dot(d, g_vehRight), Dot(d, g_vehForward), Dot(d, g_vehUp)};
}

// How far `p` is from the surface of the player's capsule or the vehicle's box: negative inside.
float SubjectClearance(CamKind kind, CamVec p) {
	const LCSChaseCamTuning &t = ChaseCamTuning();
	if (kind == CamKind::Vehicle) {
		const CamVec local = VehicleLocal(p);
		const CamVec out{
			std::max({g_box.min.x - local.x, 0.0f, local.x - g_box.max.x}),
			std::max({g_box.min.y - local.y, 0.0f, local.y - g_box.max.y}),
			std::max({g_box.min.z - local.z, 0.0f, local.z - g_box.max.z}),
		};
		const float outside = Length(out);
		if (outside > 0.0f) {
			return outside;
		}
		// Inside: the distance to the nearest face, as a negative number.
		return -std::min({local.x - g_box.min.x, g_box.max.x - local.x, local.y - g_box.min.y,
			g_box.max.y - local.y, local.z - g_box.min.z, g_box.max.z - local.z});
	}
	const float z = Clamp(p.z, g_subjectPos.z - t.footBodyBelow, g_subjectPos.z + t.footBodyAbove);
	const CamVec axis{g_subjectPos.x, g_subjectPos.y, z};
	return Length(Sub(p, axis)) - t.footBodyRadius;
}

// Lift the camera up and out over the top of the player or the vehicle if it has ended up inside
// (backed into a corner, or pulled in under a low ceiling). The view keeps looking where the
// player asked; only the position moves.
CamVec KeepOutOfSubject(CamKind kind, CamVec p) {
	const LCSChaseCamTuning &t = ChaseCamTuning();
	const float margin = t.subjectMargin;
	if (kind == CamKind::Vehicle) {
		const CamVec local = VehicleLocal(p);
		const bool inside = local.x > g_box.min.x - margin && local.x < g_box.max.x + margin &&
			local.y > g_box.min.y - margin && local.y < g_box.max.y + margin &&
			local.z > g_box.min.z - margin && local.z < g_box.max.z + margin;
		if (inside) {
			// Along world up, scaled by how upright the vehicle is so a tilted one still gets cleared.
			p.z += (g_box.max.z + margin - local.z) / std::max(g_vehUp.z, 0.3f);
		}
		return p;
	}
	const float dx = p.x - g_subjectPos.x;
	const float dy = p.y - g_subjectPos.y;
	const float r = t.footBodyRadius + margin;
	const float top = g_subjectPos.z + t.footBodyAbove + margin;
	if (dx * dx + dy * dy < r * r && p.z > g_subjectPos.z - t.footBodyBelow && p.z < top) {
		p.z = top;
	}
	return p;
}

// The game's own look left and right: L and R in pad 0's NewState, whatever pressed them (Q, E
// and C here, or a pad). Read from the game rather than from the keys, so a pad glances too.
bool GlanceHeld(bool left) {
	const u32 field = left ? kPadL1 : kPadR1;
	return ReadU16(kLCSPads + kLCSPadNewState + field).value_or(0) != 0;
}

void InvalidateWord(u32 address) {
	currentMIPS->InvalidateICacheRangeImmediate(address, 4);
}

// The block a jal at the matrix-build call goes to, when it is one of ours: a savestate taken with
// the camera installed brings the jal and the block back with it.
u32 OurBlockFromJal(u32 op) {
	if ((op >> 26) != 3) {
		return 0;
	}
	const u32 target = (kLCSCamBasisCall & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
	return ReadU32(target - kOffCode + kOffMagic).value_or(0) == kMagic ? target - kOffCode : 0;
}

// A savestate taken with the camera installed, loaded with it switched off: the jal into the block
// and the prepare hook came back with the memory, and nothing in this session knows the
// instruction under the hook (the JIT would run a nop where the game reads its shake). Put the
// game's two instructions back and free the block.
void RemoveLeftovers() {
	const u32 op = Memory::Read_Instruction(kLCSCamBasisCall, false).encoding;
	const u32 block = op == kLCSCamBasisCallOp ? 0 : OurBlockFromJal(op);
	if (!block) {
		return;
	}
	InvalidateWord(kLCSCamBasisCall);
	Memory::WriteUnchecked_U32(kLCSCamBasisCallOp, kLCSCamBasisCall);
	currentMIPS->InvalidateICacheRangeDeferred(kLCSCamBasisCall, 4);
	InvalidateWord(kLCSCamPreShake);
	if (MIPS_IS_REPLACEMENT(Memory::ReadUnchecked_U32(kLCSCamPreShake))) {
		Memory::WriteUnchecked_U32(kLCSCamPreShakeOp, kLCSCamPreShake);
		currentMIPS->InvalidateICacheRangeDeferred(kLCSCamPreShake, 4);
	}
	WriteU32(block + kOffMagic, 0);
	userMemory.Free(block);
	NOTICE_LOG(Log::HLE, "LCS: a savestate brought the chase camera back while it is switched off - the game's instructions are restored");
}

void Unhook() {
	if (!g_hooked || !currentMIPS) {
		return;
	}
	// The jal first: once it is back, nothing reaches the program, and the rest is bookkeeping.
	if (Memory::IsValid4AlignedAddress(kLCSCamBasisCall)) {
		InvalidateWord(kLCSCamBasisCall);
		Memory::WriteUnchecked_U32(kLCSCamBasisCallOp, kLCSCamBasisCall);
		currentMIPS->InvalidateICacheRangeDeferred(kLCSCamBasisCall, 4);
	}
	// Then the game's own op over the prepare hook, written outright: after a savestate the
	// replacement's saved instruction may not be this session's, and restoring a missing one
	// writes a nop.
	InvalidateWord(kLCSCamPreShake);
	RestoreReplacedInstruction(kLCSCamPreShake);
	Memory::WriteUnchecked_U32(kLCSCamPreShakeOp, kLCSCamPreShake);
	currentMIPS->InvalidateICacheRangeDeferred(kLCSCamPreShake, 4);
	if (g_finishAddr) {
		InvalidateWord(g_finishAddr);
		RestoreReplacedInstruction(g_finishAddr);
		currentMIPS->InvalidateICacheRangeDeferred(g_finishAddr, 4);
	}
	g_hooked = false;
	g_prepared = false;
	g_owned = false;
	g_blendingOut = false;
}

void ChaseCamReset() {
	g_prepared = false;
	g_owned = false;
	g_kind = CamKind::None;
	g_lastKind = CamKind::None;
	g_pivotOffset = CamVec{};
	g_blendingOut = false;
	g_blend = 1.0f;
	g_sinceLook = 1e3f;
	g_distance = 0.0f;
	g_vehicle = 0;
	g_haveVehiclePos = false;
	g_speed = 0.0f;
	g_wasGlancing = false;
	g_glancing = false;
	g_frames = 0;
	g_ownedFrames = 0;
}

}  // namespace

LCSChaseCamTuning &ChaseCamTuning() {
	static LCSChaseCamTuning tuning;
	return tuning;
}

void InstallChaseCam() {
	if (!IsActive() || !currentMIPS) {
		return;
	}
	if (!Wanted()) {
		if (g_hooked) {
			Unhook();
			g_status = "switched off";
			NOTICE_LOG(Log::HLE, "LCS: chase camera switched off, the game's vehicle camera is back");
		}
		RemoveLeftovers();
		return;
	}

	const int prepareIndex = GetReplacementFuncIndexByName("lcs_camera_prepare");
	const int finishIndex = GetReplacementFuncIndexByName("lcs_camera_finish");
	if (prepareIndex < 0 || finishIndex < 0) {
		g_status = "no replacement entries";
		return;
	}
	const u32 replacePrepare = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)prepareIndex;
	const u32 replaceFinish = MIPS_EMUHACK_CALL_REPLACEMENT | (u32)finishIndex;

	if (g_hooked) {
		// Every tick, one read each. A savestate replaces PSP memory wholesale, and this file's
		// idea of what is installed with it, so the only trustworthy answer is what is there.
		// All three through Read_Instruction: both hooks sit where the JIT starts blocks, so the
		// raw word is a block marker once the code has run.
		const bool magic = ReadU32(g_block + kOffMagic).value_or(0) == kMagic;
		const u32 basis = Memory::Read_Instruction(kLCSCamBasisCall, false).encoding;
		const u32 prepare = Memory::Read_Instruction(kLCSCamPreShake, false).encoding;
		const u32 finish = Memory::Read_Instruction(g_finishAddr, false).encoding;
		if (magic && basis == Jal(g_block + kOffCode) && prepare == replacePrepare && finish == replaceFinish) {
			return;
		}
		// Expected after loading a savestate; the view re-anchors to the game's camera, which is
		// where the loaded state has it.
		NOTICE_LOG(Log::HLE, "LCS: chase camera patch changed (magic %d, call %08x, prepare %08x, finish %08x) - reinstalling",
			magic ? 1 : 0, basis, prepare, finish);
		g_hooked = false;
		g_block = 0;
		g_status = "the patch was lost - reinstalling";
	}

	if (!Memory::IsValid4AlignedAddress(kLCSCamBasisCall) ||
	    Memory::Read_Instruction(kLCSCamClipLosCall, false).encoding != kLCSCamClipLosCallOp ||
	    Memory::Read_Instruction(kLCSRwCameraSetNearClip + 4, false).encoding != kLCSRwCameraSetNearClipOp2) {
		g_status = "waiting for the camera code";
		return;
	}

	InvalidateWord(kLCSCamPreShake);
	InvalidateWord(kLCSCamBasisCall);

	// The jal is either the game's own or, after loading a savestate taken with this installed, a
	// jal into a block of ours that came back with the state. Adopt that block rather than leave
	// the game calling into memory this file has no record of.
	const u32 basisRaw = Memory::ReadUnchecked_U32(kLCSCamBasisCall);
	const u32 block0 = basisRaw == kLCSCamBasisCallOp ? 0 : OurBlockFromJal(basisRaw);
	if (basisRaw != kLCSCamBasisCallOp && !block0) {
		g_status = "the matrix-build call is not the one measured";
		return;
	}
	// The prepare site: the game's op, our replacement, or - with our block adopted - a replacement
	// from the build that took the savestate, whose entry may have had another index.
	const u32 preRaw = Memory::ReadUnchecked_U32(kLCSCamPreShake);
	if (preRaw != kLCSCamPreShakeOp && preRaw != replacePrepare && !(block0 && MIPS_IS_REPLACEMENT(preRaw))) {
		g_status = "the pre-shake instruction is not the one measured";
		return;
	}
	u32 block = block0;

	if (!block) {
		u32 size = kBlockSize;
		block = userMemory.Alloc(size, true, "LCS chase camera");
		if (block == (u32)-1) {
			g_status = "no room in the game's memory partition";
			return;
		}
	}
	if (((block + kOffCode) & 0xf0000000u) != (kLCSCamBasisCall & 0xf0000000u)) {
		userMemory.Free(block);
		g_status = "the block landed out of jal range";
		return;
	}

	int finishOffset = 0;
	const std::vector<u32> code = BuildProgram(block, &finishOffset);
	if (kOffCode + code.size() * 4 > kBlockSize) {
		g_status = "the program outgrew its block";
		return;
	}

	// An adopted block's code may already be compiled: put the words back over any block marker
	// before rewriting them.
	currentMIPS->InvalidateICacheRangeImmediate(block + kOffCode, (u32)code.size() * 4);
	bool ok = WriteU32(block + kOffCount, 0);
	// The flags, the look command and the near clip all start zeroed.
	for (u32 off = kOffFlags; ok && off < kOffRays; off += 4) {
		ok = WriteU32(block + off, 0);
	}
	for (size_t i = 0; ok && i < code.size(); i++) {
		ok = WriteU32(block + kOffCode + (u32)i * 4, code[i]);
	}
	ok = ok && WriteU32(block + kOffMagic, kMagic);
	if (!ok) {
		g_status = "could not write the block";
		return;
	}
	currentMIPS->InvalidateICacheRangeDeferred(block + kOffCode, (u32)code.size() * 4);

	// The finish hook sits on a nop the program was just written with, so the instruction saved
	// under it is that nop.
	g_finishAddr = block + kOffCode + (u32)finishOffset * 4;
	WriteReplaceInstructionAt(g_finishAddr, finishIndex);
	currentMIPS->InvalidateICacheRangeDeferred(g_finishAddr, 4);

	// The prepare hook sits on a game instruction. Put the measured original back first, whatever
	// is there: a replacement that arrived with a savestate has no saved instruction behind it in
	// this session, and without one the JIT would run a nop where the game reads its shake.
	Memory::WriteUnchecked_U32(kLCSCamPreShakeOp, kLCSCamPreShake);
	WriteReplaceInstructionAt(kLCSCamPreShake, prepareIndex);
	currentMIPS->InvalidateICacheRangeDeferred(kLCSCamPreShake, 4);

	// The jal last, once everything it reaches is ready.
	Memory::WriteUnchecked_U32(Jal(block + kOffCode), kLCSCamBasisCall);
	currentMIPS->InvalidateICacheRangeDeferred(kLCSCamBasisCall, 4);

	g_block = block;
	g_hooked = true;
	g_prepared = false;
	g_owned = false;
	g_status = "installed";
	ReadEnvironment();
	NOTICE_LOG(Log::HLE, "LCS: chase camera installed - block %08x, %d instructions, finish at %08x",
		block, (int)code.size(), g_finishAddr);
}

void RemoveChaseCam() {
	Unhook();
	if (g_block) {
		WriteU32(g_block + kOffMagic, 0);
		userMemory.Free(g_block);
		g_block = 0;
	}
	g_finishAddr = 0;
	g_status = "not installed";
	ChaseCamReset();
}

bool ChaseCamTakesLook(InputContext context) {
	if (!g_hooked) {
		return false;
	}
	switch (context) {
	case InputContext::OnFoot: return WantedFor(CamKind::OnFoot);
	case InputContext::InVehicle: return WantedFor(CamKind::Vehicle);
	default: return false;
	}
}

void ChaseCamAddLook(float yawRadians, float pitchRadians) {
	if (yawRadians == 0.0f && pitchRadians == 0.0f) {
		return;
	}
	g_yaw = WrapPi(g_yaw + yawRadians);
	float lo, hi;
	PitchLimits(g_lastKind, &lo, &hi);
	g_pitch = Clamp(g_pitch + pitchRadians, lo, hi);
	g_sinceLook = 0.0f;
}

int Hook_lcs_camera_prepare() {
	g_prepared = false;
	if (!g_hooked || !g_block || !IsActive()) {
		return 0;
	}
	// Cheap, and the only defence against writing into memory a savestate has handed to someone
	// else: the hook can run before the next tick's install check has noticed the block is gone.
	if (ReadU32(g_block + kOffMagic).value_or(0) != kMagic) {
		return 0;
	}
	WriteU32(g_block + kOffCount, 0);
	WriteU32(g_block + kOffNearSet, 0);
	g_rayCount = 0;

	const LCSChaseCamTuning &t = ChaseCamTuning();
	g_frames++;
	g_dt = GameSeconds();

	const CamVectors vectors = CurrentVectors();
	const std::optional<CamVec> preSource = ReadVec(vectors.source);
	const std::optional<CamVec> preFront = ReadVec(vectors.front);
	const std::optional<CamVec> preUp = ReadVec(vectors.up);
	if (!preSource || !preFront || !preUp) {
		return 0;
	}
	g_preSource = *preSource;
	g_preFront = *preFront;
	g_preUp = *preUp;

	g_mode = (int)ReadU16(ActiveCam() + kLCSCamModeOffset).value_or(0xffff);
	const InputContext context = CurrentInputContext();
	// By the game's camera mode and the player's state as the game has it this frame, not the input
	// context, which is decoded at the vblank and lags the camera by up to a frame. Each lag was a
	// cut: getting in, the game switches to mode 18 a frame before the in-vehicle flag reaches the
	// context; getting out, it is in mode 4 about three seconds before it clears the flag. So mode 4
	// is on foot whatever the flag says, and mode 18 / 22 is the vehicle whenever the player has one
	// (m_pMyVehicle is set from the moment the player starts getting in).
	CamKind kind = CamKind::None;
	const bool playing = context == InputContext::OnFoot || context == InputContext::InVehicle;
	const u32 vehicle = ReadAddrU32(LCSAddr::PlayerVehicle).value_or(0);
	if (playing && g_mode == kLCSCamModeFollowPed && WantedFor(CamKind::OnFoot)) {
		kind = CamKind::OnFoot;
	} else if (playing && vehicle != 0 && WantedFor(CamKind::Vehicle) &&
	           (g_mode == kLCSCamModeCamOnAString || g_mode == kLCSCamModeBehindBoat)) {
		kind = CamKind::Vehicle;
	}

	// The subject: the player, or the vehicle. Its matrix rows: right, forward, up, position.
	const u32 subject = kind == CamKind::OnFoot ? ReadAddrU32(LCSAddr::PlayerBase).value_or(0)
		: kind == CamKind::Vehicle ? vehicle : 0;
	const std::optional<CamVec> pos = subject ? ReadVec(subject + kLCSEntityPositionOffset) : std::nullopt;
	const std::optional<CamVec> forward = subject ? ReadVec(subject + kLCSEntityForwardOffset) : std::nullopt;
	const std::optional<CamVec> rightRow = subject ? ReadVec(subject + kLCSEntityRightOffset) : std::nullopt;
	const std::optional<CamVec> upRow = subject ? ReadVec(subject + kLCSEntityUpOffset) : std::nullopt;
	if (!pos || !forward || !rightRow || !upRow) {
		kind = CamKind::None;
	}
	if (g_trace && kind != g_kind) {
		NOTICE_LOG(Log::HLE, "LCS cam: %s -> %s (mode %d, %s, frame %llu)", KindName(g_kind), KindName(kind), g_mode,
			InputContextName(context), (unsigned long long)g_frames);
	}
	g_kind = kind;

	if (kind == CamKind::None) {
		if (g_owned) {
			g_owned = false;
			g_lastKind = CamKind::None;
			// Ease into whatever the game switched to, unless it cut somewhere else entirely,
			// which is a cut the game meant.
			g_blendingOut = t.blendOut > 0.0f && Length(Sub(g_outSource, g_preSource)) < 15.0f;
			g_blendOutT = 0.0f;
		}
		g_prepared = g_blendingOut;
		return 0;
	}
	g_blendingOut = false;

	// --- The pivot ---
	CamVec pivot;
	g_subjectPos = *pos;
	const float heading = std::atan2(forward->y, forward->x);
	g_ignoreEntity = 0;
	if (kind == CamKind::OnFoot) {
		pivot = CamVec{pos->x, pos->y, pos->z + t.footPivotHeight};
		g_haveVehiclePos = false;
		g_speed = 0.0f;
		// The box itself, no margin: standing at the door after getting out is already outside
		// it (with 0.3 the car stopped blocking the camera there), and the door is the last place
		// the player is inside it.
		if (vehicle != 0 && InsideEntityBox(vehicle, *pos, 0.0f)) {
			g_ignoreEntity = vehicle;
		}
	} else {
		g_box = ReadVehicleBox(subject).value_or(VehicleBox{});
		g_vehRight = *rightRow;
		g_vehForward = *forward;
		g_vehUp = *upRow;
		// World up, never the vehicle's: a car on its side must not tip the pivot with it.
		pivot = CamVec{pos->x, pos->y, pos->z + g_box.top * t.vehiclePivotScale};
		if (subject == g_vehicle && g_haveVehiclePos && g_dt > 0.0f) {
			g_speed = Length(Sub(*pos, g_vehiclePos)) / g_dt;
		} else {
			g_speed = 0.0f;
		}
		g_vehicle = subject;
		g_vehiclePos = *pos;
		g_haveVehiclePos = true;
	}

	g_gameDistance = Length(Sub(g_preSource, pivot));

	if (!g_owned) {
		// Take the view from wherever the game has the camera this frame: after aiming, a
		// cutscene, a load or getting in, that is what the player was just looking at.
		const CamVec front = Normalize(g_preFront, CamVec{1.0f, 0.0f, 0.0f});
		g_yaw = std::atan2(front.y, front.x);
		g_pitch = std::asin(Clamp(front.z, -1.0f, 1.0f));
		// From the game's distance, so the blend in starts where the game's camera is and the
		// distance eases out to ours rather than jumping there.
		g_distance = g_gameDistance;
		g_blend = t.blendIn > 0.0f ? 0.0f : 1.0f;
		g_pivotOffset = CamVec{};
		g_wasGlancing = false;
		static bool announcedFoot = false, announcedVehicle = false;
		bool &announced = kind == CamKind::OnFoot ? announcedFoot : announcedVehicle;
		if (!announced) {
			announced = true;
			if (kind == CamKind::OnFoot) {
				NOTICE_LOG(Log::HLE, "LCS: chase camera took the on-foot view (mode %d)", g_mode);
			} else {
				NOTICE_LOG(Log::HLE, "LCS: chase camera took the vehicle view (mode %d, box %.2f long%s)",
					g_mode, g_box.length, g_box.fromGame ? "" : ", default box");
			}
		}
	} else if (kind != g_lastKind) {
		// Straight from the player to a car or back, with no frame in between: keep the view, and
		// let the pivot slide across rather than jump.
		g_pivotOffset = Add(g_pivotOffset, Sub(g_lastPivot, pivot));
		g_wasGlancing = false;
	}
	g_owned = true;
	g_ownedFrames++;

	// A look command from outside, taken after the anchor so it is not overwritten by it.
	const u32 command = ReadU32(g_block + kOffCommand).value_or(0);
	if (command != 0) {
		WriteU32(g_block + kOffCommand, 0);
		if (command & 1) {
			g_yaw = WrapPi(ReadFloat(g_block + kOffCommandYaw).value_or(g_yaw));
		}
		if (command & 2) {
			g_pitch = ReadFloat(g_block + kOffCommandPitch).value_or(g_pitch);
		}
		g_sinceLook = 0.0f;
		NOTICE_LOG(Log::HLE, "LCS: chase camera look command - yaw %.3f pitch %.3f", g_yaw, g_pitch);
	}

	g_lastPivot = pivot;
	g_pivotOffset = Mul(g_pivotOffset, 1.0f - EaseFactor(t.pivotSettleRate, g_dt));
	g_pivot = Add(pivot, g_pivotOffset);

	// --- How far back ---
	// Not from the game's camera. A vehicle is framed from its box, so a bus is still framed like
	// a bus and Select's zoom level still scales it.
	if (kind == CamKind::Vehicle) {
		const float zoom = ReadFloat(kLCSTheCamera + kLCSCamCarZoomOffset).value_or(2.0f);
		const float zoomScale = zoom < 1.5f ? t.vehicleZoomNear : (zoom > 2.5f ? t.vehicleZoomFar : 1.0f);
		g_wantDistance = (t.vehicleDistanceBase + t.vehicleDistancePerLength * g_box.length) * zoomScale *
			t.vehicleDistanceScale;
	} else {
		g_wantDistance = t.footDistance;
	}
	g_wantDistance = Clamp(g_wantDistance, std::max(t.minDistance, 0.1f), 60.0f);

	// --- Where to look ---
	g_sinceLook += g_dt;
	float lo, hi;
	PitchLimits(kind, &lo, &hi);
	float yaw = g_yaw;
	float pitch = g_pitch;
	// On foot only look behind (L + R): L alone is the game's look-around, R alone its lock-on.
	const bool left = t.glances && GlanceHeld(true);
	const bool right = t.glances && GlanceHeld(false);
	g_glancing = kind == CamKind::Vehicle ? (left || right) : (left && right);
	if (g_glancing) {
		yaw = heading + (left && right ? kPi : (left ? kPi * 0.5f : -kPi * 0.5f));
		pitch = kind == CamKind::Vehicle ? t.vehicleRestPitch : 0.0f;
	} else if (kind == CamKind::Vehicle) {
		if (g_wasGlancing) {
			// Letting go of a glance is asking for the default view back.
			g_yaw = heading;
			g_pitch = t.vehicleRestPitch;
		} else if (t.vehicleRecentre && g_sinceLook > t.recentreDelay && g_speed > t.recentreMinSpeed) {
			const float urgency = Clamp(g_speed / (t.recentreMinSpeed * 4.0f), 0.25f, 1.0f);
			const float k = EaseFactor(t.recentreRate * urgency, g_dt);
			g_yaw = WrapPi(g_yaw + WrapPi(heading - g_yaw) * k);
			g_pitch += (t.vehicleRestPitch - g_pitch) * k;
		}
		yaw = g_yaw;
		pitch = g_pitch;
	}
	// On foot, letting go of look-behind returns to the view the player had: g_yaw never moved.
	g_wasGlancing = g_glancing;
	g_pitch = Clamp(g_pitch, lo, hi);
	pitch = Clamp(pitch, lo, hi);
	g_lastKind = kind;

	const float cy = std::cos(yaw), sy = std::sin(yaw);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	g_front = CamVec{cy * cp, sy * cp, sp};
	g_up = CamVec{-cy * sp, -sy * sp, cp};
	const CamVec right3 = CamVec{sy, -cy, 0.0f};

	// Where the camera sits is the orbit. In a vehicle it stops just below the pivot: looking further
	// up tilts the view rather than swinging the camera down through the car. On foot the two are
	// one, so looking up brings the camera down behind the player's legs.
	const float orbitPitch = kind == CamKind::Vehicle ? std::min(pitch, t.vehicleOrbitUp) : pitch;
	const float co = std::cos(orbitPitch), so = std::sin(orbitPitch);
	g_orbit = CamVec{cy * co, sy * co, so};
	const CamVec orbitUp = CamVec{-cy * so, -sy * so, co};

	// --- The rays ---
	// All from the pivot, fanning out to coneRadius at the far end. Starting them at the pivot
	// keeps every start on the player's side of a wall the player is standing against.
	const float reach = g_wantDistance + t.collisionMargin;
	const CamVec end = Sub(g_pivot, Mul(g_orbit, reach));
	static const float kFan[kMaxRays][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	const int count = std::clamp(t.rays, 0, kMaxRays);
	for (int i = 0; i < count; i++) {
		const u32 ray = g_block + kOffRays + (u32)i * kRayStride;
		const CamVec e = Add(end, Add(Mul(right3, kFan[i][0] * t.coneRadius), Mul(orbitUp, kFan[i][1] * t.coneRadius)));
		WriteVec(ray + kRayStart, g_pivot);
		WriteVec(ray + kRayEnd, e);
		WriteU32(ray + kRayEntity, 0);
		WriteU32(ray + kRayHit, 0);
	}
	const u32 flags[9] = {
		1,                                                              // buildings
		(kind == CamKind::OnFoot && t.footCollideVehicles) ? 1u : 0u,   // vehicles: never our own
		0,                                                              // peds
		t.collideObjects ? 1u : 0u,                                     // objects
		0, 1, 1, 0, 0,                                                  // as the game's own camera clip
	};
	for (int i = 0; i < 9; i++) {
		WriteU32(g_block + kOffFlags + (u32)i * 4, flags[i]);
	}
	WriteU32(g_block + kOffCount, (u32)count);
	g_rayCount = count;
	g_prepared = true;
	return 0;
}

int Hook_lcs_camera_finish() {
	if (!g_prepared || !g_block || ReadU32(g_block + kOffMagic).value_or(0) != kMagic) {
		g_prepared = false;
		return 0;
	}
	g_prepared = false;
	const LCSChaseCamTuning &t = ChaseCamTuning();

	const CamVectors vectors = CurrentVectors();
	const std::optional<CamVec> postSource = ReadVec(vectors.source);
	const std::optional<CamVec> postFront = ReadVec(vectors.front);
	const std::optional<CamVec> postUp = ReadVec(vectors.up);
	if (!postSource || !postFront || !postUp) {
		return 0;
	}

	if (g_blendingOut) {
		g_blendOutT += t.blendOut > 0.0f ? g_dt / t.blendOut : 1.0f;
		if (g_blendOutT >= 1.0f) {
			g_blendingOut = false;
			return 0;
		}
		const float b = SmoothStep(g_blendOutT);
		const CamVec source = Lerp(g_outSource, *postSource, b);
		const CamVec front = Normalize(Lerp(g_outFront, *postFront, b), *postFront);
		const CamVec up = Orthonormal(Lerp(g_outUp, *postUp, b), front, *postUp);
		WriteVec(vectors.source, source);
		WriteVec(vectors.front, front);
		WriteVec(vectors.up, up);
		return 0;
	}

	// --- Collision: the nearest hit along the view, measured along the view ---
	float allowed = g_wantDistance;
	int hits = 0;
	const CamVec back = Mul(g_orbit, -1.0f);
	for (int i = 0; i < g_rayCount; i++) {
		const u32 ray = g_block + kOffRays + (u32)i * kRayStride;
		if (ReadU32(ray + kRayHit).value_or(0) == 0) {
			continue;
		}
		if (g_ignoreEntity != 0 && ReadU32(ray + kRayEntity).value_or(0) == g_ignoreEntity) {
			continue;
		}
		const std::optional<CamVec> point = ReadVec(ray + kRayCol);
		if (!point) {
			continue;
		}
		hits++;
		allowed = std::min(allowed, Dot(Sub(*point, g_pivot), back) - t.collisionMargin);
	}
	allowed = std::max(allowed, t.minDistance);
	g_hits = hits;
	g_allowed = allowed;

	// A hit pulls in at once, or the near plane goes through the wall for however long an ease
	// takes. Anything else eases, in either direction: out past an obstruction that has cleared,
	// and in when the wanted distance itself shrinks (getting out of a car is not a collision).
	if (hits > 0 && allowed < g_distance) {
		g_distance = allowed;
	} else {
		g_distance += (allowed - g_distance) * EaseFactor(t.easeOutRate, g_dt);
	}

	CamVec source = KeepOutOfSubject(g_kind, Sub(g_pivot, Mul(g_orbit, g_distance)));
	CamVec front = g_front;
	CamVec up = g_up;

	if (t.keepShake) {
		source = Add(source, Sub(*postSource, g_preSource));
		front = Normalize(Add(front, Sub(*postFront, g_preFront)), g_front);
		up = Orthonormal(Add(up, Sub(*postUp, g_preUp)), front, g_up);
	}
	if (g_blend < 1.0f) {
		g_blend = std::min(1.0f, g_blend + (t.blendIn > 0.0f ? g_dt / t.blendIn : 1.0f));
		const float b = SmoothStep(g_blend);
		source = Lerp(*postSource, source, b);
		front = Normalize(Lerp(*postFront, front, b), front);
		up = Orthonormal(Lerp(*postUp, up, b), front, up);
	}

	// The near clip plane: a fraction of the camera's clearance from the player or the vehicle,
	// never more than the game already chose this frame. The program calls the setter once this
	// hook returns.
	g_clearance = SubjectClearance(g_kind, source);
	g_near = 0.0f;
	if (t.nearClip) {
		const u32 rwCamera = ReadU32(kLCSSceneCameraPtr).value_or(0);
		const float gameNear = rwCamera ? ReadFloat(rwCamera + kLCSRwCameraNearOffset).value_or(0.0f) : 0.0f;
		if (gameNear > 0.0f && gameNear < 10.0f) {
			const float want = Clamp(g_clearance * t.nearClipFactor, std::max(t.nearClipMin, 0.01f), gameNear);
			g_near = want;
			if (want < gameNear - 0.001f) {
				WriteFloat(g_block + kOffNear, want);
				WriteU32(g_block + kOffNearSet, 1);
			}
		}
	}

	WriteVec(vectors.source, source);
	WriteVec(vectors.front, front);
	WriteVec(vectors.up, up);
	g_outSource = source;
	g_outFront = front;
	g_outUp = up;

	// On foot the game's own angles follow the view: the player walks relative to them, and the
	// aim camera starts from them. Process_FollowPed builds Front as (-cos Beta cos Alpha,
	// -sin Beta cos Alpha, sin Alpha), so Beta is the view's yaw plus pi. They hold because
	// LCSInput has the spring, the yaw correction and the nudge out while the mouse has mode 4.
	// Never in a vehicle: mode 18 swings them back, and that is what this camera stays away from.
	if (g_kind == CamKind::OnFoot) {
		const u32 cam = ActiveCam();
		float beta = std::fmod(g_yaw + kPi, kTwoPi);
		if (beta < 0.0f) {
			beta += kTwoPi;
		}
		WriteFloat(cam + kLCSCamBetaOffset, beta);
		WriteFloat(cam + kLCSCamAlphaOffset, Clamp(g_pitch, kLCSFollowPedPitchMin, kLCSFollowPedPitchMax));
	}

	if (g_trace) {
		g_traceTimer += g_dt;
		if (g_traceTimer >= 1.0f) {
			g_traceTimer = 0.0f;
			NOTICE_LOG(Log::HLE,
				"LCS cam: %s mode %d yaw %.3f pitch %.3f game %.2f want %.2f allowed %.2f now %.2f hits %d/%d "
				"pivot %.2f %.2f %.2f source %.2f %.2f %.2f speed %.1f clear %.2f near %.2f%s",
				KindName(g_kind), g_mode, g_yaw, g_pitch, g_gameDistance, g_wantDistance, g_allowed, g_distance,
				g_hits, g_rayCount, g_pivot.x, g_pivot.y, g_pivot.z, source.x, source.y, source.z, g_speed,
				g_clearance, g_near, g_glancing ? " glancing" : g_ignoreEntity ? " ignoring own vehicle" : "");
		}
	}
	return 0;
}

void ChaseCamGetStats(LCSChaseCamStats *out) {
	out->status = g_status;
	out->installed = g_hooked;
	out->owning = g_owned;
	out->kind = KindName(g_owned ? g_lastKind : CamKind::None);
	out->mode = g_mode;
	out->frames = g_frames;
	out->ownedFrames = g_ownedFrames;
	out->yaw = g_yaw;
	out->pitch = g_pitch;
	out->gameDistance = g_gameDistance;
	out->wantDistance = g_wantDistance;
	out->allowedDistance = g_allowed;
	out->distance = g_distance;
	out->clearance = g_clearance;
	out->nearClip = g_near;
	out->rays = g_rayCount;
	out->hits = g_hits;
	out->pivot[0] = g_pivot.x;
	out->pivot[1] = g_pivot.y;
	out->pivot[2] = g_pivot.z;
	out->boxLength = g_box.length;
	out->boxFromGame = g_box.fromGame;
	out->speed = g_speed;
	out->sinceLook = g_sinceLook;
	out->glancing = g_glancing;
	out->blendingOut = g_blendingOut;
	out->blendIn = g_blend;
	out->block = g_block;
}

}  // namespace LCS
