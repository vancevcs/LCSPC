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

#include "Common/Common.h"
#include "Common/CommonTypes.h"

// THE address table for GTA: Liberty City Stories, USA (ULUS10041), revision 3.00 only.
//
// This is the only place in the codebase where an LCS memory address may appear. Everything else
// refers to entries by their LCSAddr value and goes through LCSMemory.
//
// Any entry may be kUnsetAddress. That is a supported state: reads of unset entries return
// std::nullopt without touching PSP memory, and the debugger window shows "unset".
//
// docs/LCS_ADDRESSES.md says, for every entry, how it was found, how it was confirmed, and what was
// ruled out. Code addresses carry the byte pattern that finds them in another revision.

namespace LCS {

// Sentinel for "not found yet". Address 0 is never a valid PSP user address.
inline constexpr u32 kUnsetAddress = 0;

// --- The build every address below was measured on ---
//
// ULUS10041 shipped as v1.05, v2.00 and v3.00 under the same disc ID, and the code moves between
// them. The disc ID and DISC_VERSION are a cheap early filter; the EBOOT hash is what decides.
inline constexpr const char *kLCSDiscID = "ULUS10041";
inline constexpr const char *kLCSDiscVersion = "3.00";

// SHA-1 of the decrypted EBOOT, exactly as PPSSPP's DumpFileTypes = 1 writes it.
// `certutil -hashfile ULUS10041_EBOOT.BIN SHA1` gives 0c8da52387fe6a709bdbbd0ed4a53b3fb6a8b1c9.
inline constexpr u32 kLCSEbootSize = 4539680;
inline constexpr u8 kLCSEbootSha1[20] = {
	0x0c, 0x8d, 0xa5, 0x23, 0x87, 0xfe, 0x6a, 0x70, 0x9b, 0xdb,
	0xbd, 0x0e, 0xd4, 0xa5, 0x3b, 0x3f, 0xb6, 0xa8, 0xb1, 0xc9,
};

// Where PPSSPP loads the GTA3 module. Not something we choose, so it is checked on the first tick
// against an instruction whose address follows from it (kLCSFrameCap).
inline constexpr u32 kLCSModuleBase = 0x08804000;

// --- Code addresses ---
// Each with the instruction expected there, read through Memory::Read_Instruction.

// One instruction, in the game's form and ours.
struct LCSCodeWord {
	u32 address;
	u32 stock;
	u32 patched;
};

// The 30 fps cap in the frame wait: `sltiu $a0, $a0, 2` against the vblank counter
// (LCSAddr::FrameVblanks). The same instruction as VCS's; 1 makes it 60. The game fits a frame in
// one vblank at its own 222 MHz (measured driving through Harwood: 59.94 fps at 100% speed), so
// unlike VCS no CPU clock change goes with it. CTimer follows the system clock, so TimeStep halves.
// Pattern (ThirteenAG, GTALCS.PPSSPP.WidescreenFix, MIT): 02 00 84 2C ? ? ? ? 00 00 00 00 ? ? ? ? 00 00 00 00
inline constexpr LCSCodeWord kLCSFrameCap = { 0x08AB3378, 0x2C840002, 0x2C840001 };

// The return address of `jal sceKernelGetSystemTimeWide` at 0x08ab3398, the first call after the
// frame wait. The frame's logic is done and the next one has not started, which makes it the one
// moment the camera's angles can be written without the game holding them in a register: written
// from the vblank tick instead, small mouse moves were lost under the JIT. Every frame reaches it.
// The wait's own `jal sceDisplayWaitVblankStart` (0x08ab3384, the first seam) does not: a frame
// that has already used up the cap skips the loop, and in a heavy scene at 60 fps most did, so
// mouse look arrived seconds late in bursts. Other callers of the syscall are told apart by this
// address.
inline constexpr u32 kLCSFrameBoundaryReturn = 0x08AB33A0;

// The millisecond carry in CTimer::Update (0x0891b328). The game converts each frame's delta to
// whole milliseconds and drops the fraction, so its clock runs at 989 ms a second at 30 fps and 959
// at 60. It has a carry path of its own behind a flag that is never set (0x08b56c7c), quantising to
// 16 ms. These five words take that path unconditionally with a 1 ms quantum held in $t3 (dead
// there: next written at 0x0891b434). Measured at 60 fps: 999.9 ms a second, TimeStep 0.80/0.85.
// The same fix as VCS's kVCSTimerCarry.
inline constexpr LCSCodeWord kLCSTimerCarry[] = {
	{ 0x0891B3EC, 0x3C0908B5, 0x3C0B0004 },  // lui $t1, 0x8b5          ->  lui $t3, 0x4
	{ 0x0891B3F0, 0x91296C7C, 0x356B8000 },  // lbu $t1, 0x6c7c($t1)    ->  ori $t3, $t3, 0x8000
	{ 0x0891B3F4, 0x1120000A, 0x00000000 },  // beqz $t1, +10           ->  nop
	{ 0x0891B408, 0x3C0A0048, 0x00000000 },  // lui $t2, 0x48           ->  nop
	{ 0x0891B40C, 0x008A001B, 0x008B001B },  // divu $a0, $t2           ->  divu $a0, $t3
};
// The carry's remainder, in ticks of 1/294912 s. Zeroed when the carry goes in, so whatever the
// game left there does not land on one frame.
inline constexpr u32 kLCSTimerRemainder = 0x08B36BCC;

// CRunningScript::ProcessOneCommand. Reads the opcode at script+0x18, stores the NOT bit at
// script+0x216, and dispatches by range to 17 ProcessCommandsXToY switches (tools/lcsscm.py).
inline constexpr u32 kLCSProcessOneCommand = 0x08957A90;
inline constexpr u32 kLCSCollectParameters = 0x08961BEC;
inline constexpr u32 kLCSStoreParameters = 0x08957EB8;

// FindPlayerPed / FindPlayerVehicle. Second instruction of each is `lbu $a0, -0x1e94($a0)`
// (CWorld::PlayerInFocus). Patterns (ThirteenAG, as above):
//   C0 29 04 00 21 30 A5 00 21 28 A6 00 00 21 04 00 23 20 A4 00 ? ? ? ? ? ? ? ? 21 20 85 00 08 00 E0 03, -8
//   same prefix, then 00 00 84 8C ? ? ? ? 00 00 00 00 38 05 85 90, -8
inline constexpr u32 kLCSFindPlayerPed = 0x089D58B0;
inline constexpr u32 kLCSFindPlayerVehicle = 0x089D57B4;
inline constexpr u32 kLCSFindPlayerOp2 = 0x9084E16C;

// --- Struct offsets ---
// Offsets inside game structs, for values read through a pointer the table can't chase on its own.

// TheCamera and its CCams (docs/LCS_ADDRESSES.md "Camera"). Cams[ActiveCam] is the live one.
inline constexpr u32 kLCSTheCamera = 0x08B833A0;
inline constexpr u32 kLCSCamActiveOffset = 0x7f;   // u8 ActiveCam
inline constexpr u32 kLCSCamsOffset = 0x190;       // CCam Cams[3]
inline constexpr u32 kLCSCamStride = 0x290;
inline constexpr u32 kLCSCamModeOffset = 0x1c;     // s16
inline constexpr u32 kLCSCamAlphaOffset = 0xbc;    // pitch: positive looks up
inline constexpr u32 kLCSCamBetaOffset = 0xcc;     // yaw, [0, 2PI): the stick to the right lowers it
// Front, three floats; Process_FollowPed builds (-cos(Beta) cos(Alpha), -sin(Beta) cos(Alpha),
// sin(Alpha)) from the angles. Writing it alongside Beta does not stop the yaw correction
// (kLCSFollowPedYawCorrection): measured, no change at all.
inline constexpr u32 kLCSCamFrontOffset = 0x1a0;
inline constexpr int kLCSCamModeFollowPed = 4;
inline constexpr int kLCSCamModeBehindCar = 3;
inline constexpr int kLCSCamModeCamOnAString = 18;
inline constexpr int kLCSCamModeBehindBoat = 22;

// Process_FollowPed's tuning table (0x08b359bc, two sets of 15 floats; DAT_08b58df0 picks one).
// [5] is the pitch spring: each frame Alpha moves (1 - [5]^TimeStep) of the way back to a default,
// about 31% at 30 fps. 1.0 turns it off, which is what lets a mouse-set pitch stay put (measured:
// Alpha written to 0.6 and -0.4 held exactly; with the spring on it was back in 0.6 s).
// [13] and [14] are the pitch limits, up and down.
struct LCSDataFloat {
	u32 address;
	float stock;
};
inline constexpr LCSDataFloat kLCSFollowPedPitchSpring[] = {
	{ 0x08B359D0, 0.8f },
	{ 0x08B35A0C, 0.9f },
};
inline constexpr float kLCSFollowPedPitchMax = 0.7854f;
inline constexpr float kLCSFollowPedPitchMin = -1.4835f;

// Process_FollowPed's obstacle nudge: five rays around the camera, and each that hits adds
// (i - 2) * 0.03 rad to this float, which is then added to Beta every frame and decays by 0.95.
// The tail is worth up to 20 times the hit, which is why a mouse turn that ends facing a wall
// drifted 0.16 rad back on its own. Zeroed each tick while the mouse drives the camera.
inline constexpr u32 kLCSFollowPedYawNudge = 0x08B35D70;

// BetaSpeed: the yaw rate Process_FollowPed integrates into Beta, smoothed by table[8] and clamped
// to table[9].
inline constexpr u32 kLCSCamBetaSpeedOffset = 0xd0;

// The yaw correction in Process_FollowPed: `add.s $f12, $f12, $f13` adds (targetYaw - Beta) /
// TimeStep to the stick's yaw rate. targetYaw is the yaw of Front (rebuilt each frame from where
// the camera actually is) plus the swing in behind a walking player. So a Beta written from outside
// is steered back toward the camera's old position: measured, 25 mouse steps of 0.05 rad kept
// 0.066 of 1.25. Without the add, Beta keeps what it is given (+0.995 of 1.25 with momentum left
// from the stock run, no drift at rest). Taken out only while the mouse drives the on-foot camera;
// the cost is the auto-swing behind a walking player, which a PC mouse camera doesn't do anyway.
inline constexpr LCSCodeWord kLCSFollowPedYawCorrection = { 0x088F2ACC, 0x460D6300, 0x00000000 };

// --- The vehicle chase camera (LCSChaseCam, docs/lcs/chase-camera.md) ---
//
// CCamera::Process (0x08900c78) holds this frame's camera in three stack vectors from 0x08900d1c
// to its end: Source at sp+0x70, Front at sp+0x80 ($fp) and Up at sp+0x90 ($s7). They are loaded
// from Cams[ActiveCam] at 0x089012cc: Front from CCam+0x1a0 (kLCSCamFrontOffset), Up from +0x1d0,
// Source from +0x1b0. The matrix takes Front as its +0x10 row and Up as +0x20 (0x0890232c). Front
// is always Source + 0x10, so both hooks find all three from $fp and $s7, whatever $sp is.
//
// The prepare hook: the first instruction after the game's own camera clip and near-clip call,
// `lwc1 $f12, 0x1be8($s0)` (the shake amount). A branch target, so the JIT starts a block there.
inline constexpr u32 kLCSCamPreShake = 0x08902174;
inline constexpr u32 kLCSCamPreShakeOp = 0xC60C1BE8;
// `jal 0x088ea388` (cross product, a0 = sp+0x40 out, a1 = Up, a2 = Front), the first step of
// building TheCamera's matrix from the three vectors. Pointed at the chase camera's program.
inline constexpr u32 kLCSCamBasisCall = 0x08902318;
inline constexpr u32 kLCSCamBasisCallOp = 0x0E23A8E2;
inline constexpr u32 kLCSCamBasisFunc = 0x088EA388;
// CWorld::ProcessLineOfSight: (start, end, CColPoint &, CEntity *&, buildings, vehicles, peds,
// objects, dummies, ignoreSeeThrough, ignoreSomeObjects, ...; the last five on the stack), the
// same 13 arguments as VCS's. The game's camera clip calls it at kLCSCamClipLosCall with
// (1, 0, 0, 1, 0, 1, 1, 0, 0); that call's word is the install's check that this is the code.
inline constexpr u32 kLCSProcessLineOfSight = 0x088C68F4;
inline constexpr u32 kLCSCamClipLosCall = 0x08902128;
inline constexpr u32 kLCSCamClipLosCallOp = 0x0E231A3D;
// RwCameraSetNearClipPlane(RwCamera *, float in $f12), checked by its second instruction
// (`sw $s0, 0x10($sp)`). The near plane is RwCamera+0x80. Scene.camera, the RwCamera the frame is
// rendered with, is at kLCSSceneCameraPtr (CCamera::Process passes [Scene+4]).
inline constexpr u32 kLCSRwCameraSetNearClip = 0x0887352C;
inline constexpr u32 kLCSRwCameraSetNearClipOp2 = 0xAFB00010;
inline constexpr u32 kLCSRwCameraNearOffset = 0x80;
inline constexpr u32 kLCSSceneCameraPtr = 0x08B5E204;
// TheCamera's car zoom level: 1, 2 or 3 as Select cycles the CamOnAString distances.
inline constexpr u32 kLCSCamCarZoomOffset = 0xcc;

// CEntity: its matrix (right, forward, up, position rows of four floats) and model index (s16).
inline constexpr u32 kLCSEntityRightOffset = 0x00;
inline constexpr u32 kLCSEntityForwardOffset = 0x10;
inline constexpr u32 kLCSEntityUpOffset = 0x20;
inline constexpr u32 kLCSEntityPositionOffset = 0x30;
inline constexpr u32 kLCSEntityModelOffset = 0x58;
// CModelInfo::ms_modelInfoPtrs (a pointer to the array) and its length. CBaseModelInfo's
// CColModel * is at +0x14; a CColModel's bounding box at +0x10 (min) and +0x20 (max).
inline constexpr u32 kLCSModelInfoTablePtr = 0x08B4D3BC;
inline constexpr u32 kLCSModelInfoCountAddr = 0x08B5E140;
inline constexpr u32 kLCSModelInfoColModelOffset = 0x14;
inline constexpr u32 kLCSColModelBoxMinOffset = 0x10;
inline constexpr u32 kLCSColModelBoxMaxOffset = 0x20;

// --- Free aim (LCSFreeAim, docs/lcs/free-aim.md) ---
//
// The game's free aim: hold R, tap d-pad down. Camera mode 11 (Process at 0x088f07cc, shared with
// 28), ped+0xc9d set. The rest of a CCam's basis, next to Front (kLCSCamFrontOffset):
inline constexpr int kLCSCamModeFreeAim = 11;
inline constexpr u32 kLCSCamFOVOffset = 0xc4;
inline constexpr u32 kLCSCamSourceOffset = 0x1b0;
inline constexpr u32 kLCSCamUpOffset = 0x1d0;
// CPed::m_pPointGunAt: in free aim a placeholder OBJECT the game keeps along the camera's Front
// from the aim pivot. Mode 11 turns Beta / Alpha toward it by at most 0.1 * TimeStep a frame
// (0x08b35920), so it is the aim's master: a written Beta came back within 0.12 s, a written
// target was kept and the camera followed it within a frame.
inline constexpr u32 kLCSPedAimTargetOffset = 0x6bc;
// u8, 1 while free aiming (0 in lock-on and on foot). Mode 11 skips its crosshair offsets and its
// pitch clamp while it is set.
inline constexpr u32 kLCSPedFreeAimingOffset = 0xc9d;
// CEntity's flags; (flags & 0xe) >> 1 is the type: 1 building, 2 vehicle, 3 ped, 4 object.
inline constexpr u32 kLCSEntityFlagsOffset = 0x44;
inline constexpr u32 kLCSEntityTypeObject = 4;
// The aim pivot (0x088ebad8): the ped's position + 0.6 up (+0.1 * min(1, (70 - FOV) / 20) more, and
// 0 or 0.15 by weapon) + its right vector * (0.55 + 0.1 * min(1, (70 - FOV) / 35)), pulled back
// by a building raycast from the ped. The camera's centre line passes through it: measured 0.000
// off the line at three pitches.
inline constexpr float kLCSAimPivotHeight = 0.6f;
inline constexpr float kLCSAimPivotShoulder = 0.55f;
// The pitch free aim reached with the stick held down / up for 2 s.
inline constexpr float kLCSFreeAimPitchMin = -0.49f;
inline constexpr float kLCSFreeAimPitchMax = 0.67f;
// CWeaponInfo: kLCSWeaponInfoTables holds a pointer to two table pointers, [0] when the byte at
// kLCSWeaponInfoSelect is set, else [1] (GetWeaponInfo, 0x0894d97c); 0x70 a weapon type; the fire
// type at +0 (0 melee, 1 instant hit, 2 projectile...) and the range at +4 (FireInstantHit scales
// its shot to it). Read live: type 26 is 1 / 90.0, 25 is 1 / 45.0, 12 is 2 / 30.0.
inline constexpr u32 kLCSWeaponInfoTables = 0x08B5E660;
inline constexpr u32 kLCSWeaponInfoSelect = 0x08B56C7C;
inline constexpr u32 kLCSWeaponInfoStride = 0x70;
inline constexpr u32 kLCSWeaponInfoRangeOffset = 0x04;
// CWeapon::FireInstantHit (0x08857ef8; the weapon, the shooter, the source): the player's free-aim
// shot raycasts through `jal 0x08852188` here, a wrapper that forwards (start, end, colpoint,
// entity, flags...) to ProcessLineOfSight. At the jal, $a0 / $a1 point at the shot's start and end
// and $t1 is the shooter. Caught with logging breakpoints: one hit per bullet, at this site only.
inline constexpr u32 kLCSFireRaycastCall = 0x08858638;
inline constexpr u32 kLCSFireRaycastCallOp = 0x0E214862;
// Inside the wrapper, the first instruction after ProcessLineOfSight returns (`lw $ra, 0x40($sp)`):
// where the borrowed start is given back. The op after the call in FireInstantHit is a branch,
// which a HOOKENTER can't sit on.
inline constexpr u32 kLCSFireRaycastDone = 0x088521DC;
inline constexpr u32 kLCSFireRaycastDoneOp = 0x8FBF0040;

// CVehicle::m_vehType. FindPlayerTrain (0x089d5860) compares it with 2.
inline constexpr u32 kLCSVehicleTypeOffset = 0x344;

// What the scripts' is_*_on_any_bike / in_any_heli / in_any_plane / is_boat ask
// (GetVehicleAppearance, 0x088a08c4): the handling flags' type nibble.
inline constexpr u32 kLCSVehicleHandlingOffset = 0x150;   // CVehicle::pHandling
inline constexpr u32 kLCSHandlingFlagsOffset = 0xd0;      // tHandlingData::Flags
inline constexpr u32 kLCSHandlingTypeMask = 0xf0000;
inline constexpr u32 kLCSHandlingIsBike = 0x10000;
inline constexpr u32 kLCSHandlingIsHeli = 0x20000;
inline constexpr u32 kLCSHandlingIsPlane = 0x40000;
inline constexpr u32 kLCSHandlingIsBoat = 0x80000;

// CPed::m_weapons[10] and CPed::m_currentWeapon (get_current_char_weapon, 0475). A CWeapon is 0x1c
// bytes: type +0, state +4, ammo in clip +8, ammo total +0xc. Script slot numbers are index + 1.
inline constexpr u32 kLCSPedWeaponsOffset = 0x594;
inline constexpr u32 kLCSWeaponStride = 0x1c;
inline constexpr u32 kLCSWeaponSlots = 10;

// CPad::Pads[], from CPad::GetPad (0x08a96408). NewState and OldState are CControllerStates of 24
// shorts; the offsets below are within one. is_button_pressed N (00E6, via 0x0887ba88) numbers
// them like VCS: 0-3 sticks, 4-7 shoulders, 8-11 d-pad, 12 Start, 13 Select, 14 Square,
// 15 Triangle, 16 Cross, 17 Circle. The struct has four more shorts between d-pad and Start
// (+0x18..+0x1e), which the numbering skips: a second d-pad, read instead of the real one while
// LCSAddr::PadDpadAsStick is set (and the real d-pad then stands in for the left stick).
inline constexpr u32 kLCSPads = 0x08b89da0;
inline constexpr u32 kLCSPadStride = 0xc0;
inline constexpr u32 kLCSPadNewState = 0x02;
inline constexpr u32 kLCSPadOldState = 0x34;
enum LCSPadField : u32 {
	kPadLeftX = 0x00, kPadLeftY = 0x02, kPadRightX = 0x04, kPadRightY = 0x06,
	kPadL1 = 0x08, kPadL2 = 0x0a, kPadR1 = 0x0c, kPadR2 = 0x0e,
	kPadDUp = 0x10, kPadDDown = 0x12, kPadDLeft = 0x14, kPadDRight = 0x16,
	kPadAltDUp = 0x18, kPadAltDDown = 0x1a, kPadAltDLeft = 0x1c, kPadAltDRight = 0x1e,
	kPadStart = 0x20, kPadSelect = 0x22, kPadSquare = 0x24, kPadTriangle = 0x26,
	kPadCross = 0x28, kPadCircle = 0x2a, kPadLeftShock = 0x2c, kPadRightShock = 0x2e,
};

// How to interpret the bytes at an address.
enum class LCSAddrType {
	U8,
	U16,
	U32,
	S32,
	Float,
};

// One entry per value read out of the game. Keep in sync with kLCSAddresses below.
enum class LCSAddr {
	// --- Player ---
	PlayerInFocus,     // CWorld::PlayerInFocus. Index into CWorld::Players.
	PlayerBase,        // CWorld::Players[0].m_pPed: pointer to the local player ped.
	PlayerHealth,      // Ped health, float.
	PlayerInVehicle,   // Ped's bInVehicle. FindPlayerVehicle returns 0 unless this is set.
	PlayerVehicle,     // Ped's m_pMyVehicle. Only meaningful while PlayerInVehicle is set.
	WeaponSlot,        // Ped's m_currentWeapon: index into m_weapons[] (kLCSPedWeaponsOffset).

	// --- Timer ---
	TimeInMs,          // CTimer::m_snTimeInMilliseconds. Game clock, follows the system clock.
	TimeStep,          // CTimer::ms_fTimeStep. Frame delta in 1/50 s; clamped to 3.333.
	FrameCounter,      // CTimer::m_FrameCounter. +1 per game frame.
	FrameVblanks,      // Vblanks since the last frame flip; the frame wait loops until it reaches 2.

	// --- Scripts ---
	ScriptParams,      // CTheScripts::ScriptParams[0]. CollectParameters writes here.

	// --- Camera ---
	// TheCamera is at 0x08b833a0; Cams[3] at +0x190, stride 0x290. These read Cams[0].
	ActiveCam,         // TheCamera.ActiveCam: which of Cams[] is live.
	CamMode,           // Cams[0].Mode. 4 on foot, 18/22 in vehicles, 11/28 lock-on, 7/8/34/45/46 first person.
	CamPitch,          // Cams[0].Alpha, radians. Process_FollowPed clamps it.
	CamYaw,            // Cams[0].Beta, radians in [0, 2PI).
	CamFOV,            // Cams[0].FOV, degrees. Look rates scale with FOV/80.
	WeaponCamMode,     // TheCamera.PlayerWeaponMode.Mode. Using1stPersonWeaponMode tests it.
	LookSensitivity,   // Squared in the on-foot look response.

	// --- Pad 0 (kLCSPads) ---
	PadLeftX,          // NewState.LeftStickX: the nub.
	PadLeftY,
	PadDPadUp,
	PadDPadDown,
	PadDPadLeft,
	PadDPadRight,
	PadMode,           // CPad::Mode. LookAround* read the stick only while it is below 4.
	PadDisableControls, // CPad::DisablePlayerControls. Every action getter returns 0 while set.
	PadDpadAsStick,    // u8. Stick getters read the d-pad, d-pad getters the second d-pad.
	InvertLook,        // u8. LookAroundUpDown negates the stick while set.

	// --- Front end ---
	MenuActive,        // FrontEndMenuManager.m_bMenuActive: the pause menu (map, brief, stats...) is up.

	Count,
};

// Sentinel for the `base` field: this entry's address is absolute.
inline constexpr LCSAddr kNoBase = LCSAddr::Count;

struct LCSAddrEntry {
	LCSAddr id;
	const char *name;      // Shown in the debugger window.
	LCSAddrType type;

	// When base == kNoBase, an absolute address, and kUnsetAddress means "not found". Otherwise a
	// byte offset from the pointer stored at `base` (one level of indirection only).
	u32 address;
	LCSAddr base;

	const char *note;      // Short hint; the long version lives in docs/LCS_ADDRESSES.md.
};

// Order must match the LCSAddr enum exactly - LookupAddr indexes into this directly.
inline constexpr LCSAddrEntry kLCSAddresses[] = {
	{ LCSAddr::PlayerInFocus,   "PlayerInFocus",   LCSAddrType::U8,    0x08b5e16c, kNoBase,             "u8. 0 in single player" },
	{ LCSAddr::PlayerBase,      "PlayerBase",      LCSAddrType::U32,   0x08b89b10, kNoBase,             "CWorld::Players[0].m_pPed. Players[] stride 0x170" },
	{ LCSAddr::PlayerHealth,    "PlayerHealth",    LCSAddrType::Float, 0x4b8,      LCSAddr::PlayerBase, "ped+0x4b8. get_player_health (022A) reads it" },
	{ LCSAddr::PlayerInVehicle, "PlayerInVehicle", LCSAddrType::U8,    0x538,      LCSAddr::PlayerBase, "ped+0x538. FindPlayerVehicle tests it" },
	{ LCSAddr::PlayerVehicle,   "PlayerVehicle",   LCSAddrType::U32,   0x534,      LCSAddr::PlayerBase, "ped+0x534. Stale when PlayerInVehicle is 0" },
	{ LCSAddr::WeaponSlot,      "WeaponSlot",      LCSAddrType::U8,    0x6b8,      LCSAddr::PlayerBase, "ped+0x6b8. get_current_char_weapon (0475) indexes m_weapons with it" },
	{ LCSAddr::TimeInMs,        "TimeInMs",        LCSAddrType::U32,   0x08b5e144, kNoBase,             "Set to 1 by CTimer::Initialise (0x0891b1a4)" },
	{ LCSAddr::TimeStep,        "TimeStep",        LCSAddrType::Float, 0x08b5e148, kNoBase,             "CTimer::Update clamps it to 3.333" },
	{ LCSAddr::FrameCounter,    "FrameCounter",    LCSAddrType::U32,   0x08b5e174, kNoBase,             "Incremented at the end of CTimer::Update (0x0891b648)" },
	{ LCSAddr::FrameVblanks,    "FrameVblanks",    LCSAddrType::U32,   0x08b56a90, kNoBase,             "Read by the 30 fps cap; zeroed after the wait" },
	{ LCSAddr::ScriptParams,    "ScriptParams",    LCSAddrType::U32,   0x08dcf550, kNoBase,             "Passed to CollectParameters as $a3" },
	{ LCSAddr::ActiveCam,       "ActiveCam",       LCSAddrType::U8,    0x08b8341f, kNoBase,             "TheCamera+0x7f. Indexes Cams[] (stride 0x290)" },
	{ LCSAddr::CamMode,         "CamMode",         LCSAddrType::U16,   0x08b8354c, kNoBase,             "Cams[0]+0x1c. CCam::Process (0x088efc44) switches on it" },
	{ LCSAddr::CamPitch,        "CamPitch",        LCSAddrType::Float, 0x08b835ec, kNoBase,             "Cams[0]+0xbc (Alpha). Rate at +0xc0" },
	{ LCSAddr::CamYaw,          "CamYaw",          LCSAddrType::Float, 0x08b835fc, kNoBase,             "Cams[0]+0xcc (Beta). Rate at +0xd0" },
	{ LCSAddr::CamFOV,          "CamFOV",          LCSAddrType::Float, 0x08b835f4, kNoBase,             "Cams[0]+0xc4" },
	{ LCSAddr::WeaponCamMode,   "WeaponCamMode",   LCSAddrType::U16,   0x08b83cf4, kNoBase,             "TheCamera+0x954 (PlayerWeaponMode.Mode)" },
	{ LCSAddr::LookSensitivity, "LookSensitivity", LCSAddrType::Float, 0x08b3591c, kNoBase,             "Squared in Process_FollowPed's look response" },
	{ LCSAddr::PadLeftX,        "PadLeftX",        LCSAddrType::U16,   0x08b89da2, kNoBase,             "Pads[0]+0x02. s16" },
	{ LCSAddr::PadLeftY,        "PadLeftY",        LCSAddrType::U16,   0x08b89da4, kNoBase,             "Pads[0]+0x04. s16" },
	{ LCSAddr::PadDPadUp,       "PadDPadUp",       LCSAddrType::U16,   0x08b89db2, kNoBase,             "Pads[0]+0x12" },
	{ LCSAddr::PadDPadDown,     "PadDPadDown",     LCSAddrType::U16,   0x08b89db4, kNoBase,             "Pads[0]+0x14" },
	{ LCSAddr::PadDPadLeft,     "PadDPadLeft",     LCSAddrType::U16,   0x08b89db6, kNoBase,             "Pads[0]+0x16" },
	{ LCSAddr::PadDPadRight,    "PadDPadRight",    LCSAddrType::U16,   0x08b89db8, kNoBase,             "Pads[0]+0x18" },
	{ LCSAddr::PadMode,         "PadMode",         LCSAddrType::U16,   0x08b89e22, kNoBase,             "Pads[0]+0x82 (ThirteenAG: lhu a2, 0x82)" },
	{ LCSAddr::PadDisableControls, "PadDisableControls", LCSAddrType::U16, 0x08b89e26, kNoBase,       "Pads[0]+0x86 (ThirteenAG: lhu a1, 0x86)" },
	{ LCSAddr::PadDpadAsStick,  "PadDpadAsStick",  LCSAddrType::U8,    0x08b56434, kNoBase,             "Tested by every CPad stick/d-pad getter (0x08a96c60..)" },
	{ LCSAddr::InvertLook,      "InvertLook",      LCSAddrType::U8,    0x08b56433, kNoBase,             "LookAroundUpDown (0x08a98d8c) negates Y while set" },
	{ LCSAddr::MenuActive,      "MenuActive",      LCSAddrType::U8,    0x08b8ef51, kNoBase,             "FrontEndMenuManager (0x08b8ee20) +0x131. 1 with the pause menu up" },
};

static_assert(ARRAY_SIZE(kLCSAddresses) == (size_t)LCSAddr::Count,
	"kLCSAddresses must have exactly one entry per LCSAddr, in enum order");

inline constexpr const LCSAddrEntry &LookupAddr(LCSAddr id) {
	return kLCSAddresses[(size_t)id];
}

// Whether we know where this value lives. For a based entry the offset may legitimately be 0, so
// having a base is itself the signal.
inline constexpr bool IsAddrSet(LCSAddr id) {
	const LCSAddrEntry &entry = LookupAddr(id);
	if (entry.base != kNoBase) {
		return IsAddrSet(entry.base);
	}
	return entry.address != kUnsetAddress;
}

}  // namespace LCS
