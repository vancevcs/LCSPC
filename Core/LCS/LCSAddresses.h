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

// --- The scoped weapons (LCSFreeAim's scope look, docs/lcs/free-aim.md "The scope") ---
//
// CCam::Process (0x088efc44) sends modes 7 (sniper), 8 (rocket launcher), 34, 45 and 46 to one
// first-person handler, 0x088f6f44. On foot it adds a smoothed, FOV-scaled stick rate to Beta and
// Alpha (the rates at CCam+0xd0 / +0xc0), clamps Alpha, builds Front as (cos(Beta) cos(Alpha),
// sin(Beta) cos(Alpha), sin(Alpha)) - not Process_FollowPed's negated one - and turns the ped to
// it. With the stick centred the rates are 0 and written angles stay: measured, Beta -0.5 and
// Alpha 0.3 held for a second, and the ped's heading followed.
inline constexpr int kLCSCamModesScoped[] = { 7, 8, 34, 45, 46 };
inline constexpr int kLCSCamModeSniper = 7;
inline constexpr int kLCSCamModeHeliCannon = 45;
inline constexpr int kLCSCamModeCamera = 46;
// Its pitch limits: 60 degrees up, 89.5 down (mode 45: 30.5 down).
inline constexpr float kLCSScopePitchMax = 1.0471976f;
inline constexpr float kLCSScopePitchMin = -1.5620697f;
inline constexpr float kLCSScopePitchMinHeliCannon = -0.53232545f;
// Modes 7 and 46 zoom; the rest hold FOV 70. Square divides the FOV by (1 + 0.0255 * TimeStep) a
// frame and Cross multiplies it (CPad 0x08a989d8 / 0x08a98a18), and this static then holds the new
// FOV; with neither held the FOV eases to it (0x088ea7bc) while it is more than 0.5 off. So a
// written target is a smooth zoom: measured 70 -> 20.3 within 0.15 s. Entering the mode sets it
// to the FOV of the moment.
inline constexpr u32 kLCSScopeZoomTarget = 0x08B35E0C;
inline constexpr float kLCSScopeFOVMax = 70.0f;
inline constexpr float kLCSScopeFOVMin = 15.0f;
inline constexpr float kLCSScopeFOVMinCamera = 3.0f;
// m_weapons[8] holds both sniper rifles, types 28 and 29 (CWeaponInfo+0x68 is a weapon's slot).
inline constexpr u32 kLCSWeaponSlotSniper = 8;

// --- The GPS route (LCSRoute, docs/lcs/gps.md) ---
//
// ThePaths: a pointer to CPathFind, which get_closest_car_node (02C6, 0x08a220b0) hands to
// FindNodeClosestToCoors (0x0897572c) and the node-to-coordinates call (0x089777a8).
inline constexpr u32 kLCSThePathsPtr = 0x08B3980C;
inline constexpr u32 kLCSPathNodeArray = 0x00;      // pointer to the nodes
inline constexpr u32 kLCSPathLinkArray = 0x08;      // pointer to the links, u16 each
inline constexpr u32 kLCSPathAfterLinks = 0x0c;     // pointer to the array after them: where they end
inline constexpr u32 kLCSPathNodeCount = 0x14;      // 5519 = car + ped
inline constexpr u32 kLCSPathCarNodeCount = 0x18;   // 2196: the car nodes come first
// A node is VC's CPathNode: 20 bytes, x / y / z as s16 times 8 at +4, the first link (s16) at
// +0xc, and at +0x10 the link count in the low nibble with 0x20 for a road switched off (the
// Callahan Bridge and the Porter Tunnel at the start of the story; switch_roads_off sets it).
inline constexpr u32 kLCSPathNodeStride = 20;
inline constexpr u32 kLCSPathNodePos = 0x04;
inline constexpr float kLCSPathNodeScale = 8.0f;
inline constexpr u32 kLCSPathNodeFirstLink = 0x0c;
inline constexpr u32 kLCSPathNodeFlags = 0x10;
inline constexpr u8 kLCSPathNodeDisabled = 0x20;
// A link is a node index in the low 14 bits; the top two are flags (0x8000 on 432, 0xc000 on 144).
inline constexpr u16 kLCSPathLinkMask = 0x3fff;

// The radar (CRadar::TransformRealWorldPointToRadarSpace, 0x08967b20): a world point is
// d = (point - origin) / range, then (cos * d.x + sin * d.y, cos * d.y - sin * d.x) in the unit
// disc, +y forward. Origin is the player's position; range 96.0 as read.
inline constexpr u32 kLCSRadarOrigin = 0x08B6E218;   // two floats
inline constexpr u32 kLCSRadarSin = 0x08B6E220;
inline constexpr u32 kLCSRadarCos = 0x08B6E224;
inline constexpr u32 kLCSRadarRange = 0x08B5E7C0;
// The disc's box on the PSP's 480x272 screen, from its getters (0x08986fe4 and three more): left
// 12, top 196, 65.8 square, or top 170 and 92 square while kLCSMultiplayer is set. The disc to
// the screen is (left + w/2 + x w/2, top + h/2 - y h/2) (0x08967860). Checked on a capture: the
// ring's centre within a pixel of (44.9, 228.9).
inline constexpr float kLCSRadarLeft = 12.0f;
inline constexpr float kLCSRadarTop = 196.0f;
inline constexpr float kLCSRadarSize = 65.8f;
inline constexpr u32 kLCSMultiplayer = 0x08B56C7C;   // u8; kLCSWeaponInfoSelect is the same byte
// CHud::Draw (0x08988940) draws the radar only while this is 1 and the camera's cutscene bars
// (TheCamera+0x75) are off, then only with RADAR MODE (kLCSRadarMode) not OFF and this byte clear.
inline constexpr u32 kLCSHudWantsToDraw = 0x08B5E214;
inline constexpr u32 kLCSCamWideScreenOn = 0x08B83415;
inline constexpr u32 kLCSRadarSuppressed = 0x08B5E56D;

// CRadar::ms_RadarTrace (SetCoordBlip, 0x08967e40): 75 blips of 0x50 bytes.
inline constexpr u32 kLCSRadarTrace = 0x08E4AA20;
inline constexpr u32 kLCSBlipStride = 0x50;
inline constexpr int kLCSBlipCount = 75;
inline constexpr u32 kLCSBlipType = 0x04;     // 1 car, 2 ped, 3 object, 4 coordinate, 5 contact point
inline constexpr u32 kLCSBlipHandle = 0x08;   // a pool handle, for types 1..3
inline constexpr u32 kLCSBlipPos = 0x0c;      // three floats, for 4 and 5
inline constexpr u32 kLCSBlipInUse = 0x33;    // u8
inline constexpr u32 kLCSBlipDisplay = 0x3e;  // u16: 0 nothing, 1 the 3D marker, 2 the radar, 3 both
inline constexpr u32 kLCSBlipSprite = 0x40;   // u16: 0 a plain blip
// The map's own marker ("Player Target", placed with Square on the map page): a coordinate blip
// with this sprite, colour 0, radar only. A mission's destination is a plain blip (sprite 0).
inline constexpr u16 kLCSBlipSpriteMapMarker = 49;
inline constexpr int kLCSBlipTypeCar = 1;
inline constexpr int kLCSBlipTypePed = 2;
inline constexpr int kLCSBlipTypeObject = 3;
inline constexpr int kLCSBlipTypeCoord = 4;
inline constexpr int kLCSBlipTypeContact = 5;
// The pools a blip's handle indexes (the blip draw, 0x08969f8c): a pointer to {entries, flags};
// slot = handle >> 8, live while flags[slot] == (handle & 0xff); the capacity at +8. Entry sizes
// from GetAt; checked live with the player's own car and ped (handles 0x1001, 0x101).
inline constexpr u32 kLCSPedPoolPtr = 0x08B4C53C;
inline constexpr u32 kLCSVehiclePoolPtr = 0x08B4C540;
inline constexpr u32 kLCSObjectPoolPtr = 0x08B4C54C;
inline constexpr u32 kLCSPedSize = 0xcb0;
inline constexpr u32 kLCSVehicleSize = 0x6e0;
inline constexpr u32 kLCSObjectSize = 0x220;

// The reserve MainMemoryManager (0x08aa4af8) leaves out of sceKernelMaxFreeMemSize, which is what
// the streaming heap (0x08a0b794) is then built from, less 512KB: 5MB as shipped (0x4e6666), 12MB
// when more than 22MB is free (docs/lcs/world-memory.md).
inline constexpr u32 kLCSMainMemoryReserve = 0x08B5666C;

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
// CPad::Mode (u16), the game's controller configuration 0..3, the text's C0..C3 tokens. A save
// carries it (0x088b221c), RESET GAME SETTINGS sets 2 (0x08ade44c), and the keyboard is built on 2:
// on foot it is 0's layout, in a vehicle R is the handbrake (GetHandBrake 0x08a97e3c; 0 and 1 want
// Square + Cross) and the looks are L with the stick (0x08a976cc, 0x08a978a0, 0x08a97a78; 0 and 1
// look with L, R and L + R). 1 and 3 swap the stick and the d-pad. Read 2 in the fixtures.
inline constexpr u32 kLCSPadModeOffset = 0x82;
inline constexpr u16 kLCSPadModeKeyboard = 2;
enum LCSPadField : u32 {
	kPadLeftX = 0x00, kPadLeftY = 0x02, kPadRightX = 0x04, kPadRightY = 0x06,
	kPadL1 = 0x08, kPadL2 = 0x0a, kPadR1 = 0x0c, kPadR2 = 0x0e,
	kPadDUp = 0x10, kPadDDown = 0x12, kPadDLeft = 0x14, kPadDRight = 0x16,
	kPadAltDUp = 0x18, kPadAltDDown = 0x1a, kPadAltDLeft = 0x1c, kPadAltDRight = 0x1e,
	kPadStart = 0x20, kPadSelect = 0x22, kPadSquare = 0x24, kPadTriangle = 0x26,
	kPadCross = 0x28, kPadCircle = 0x2a, kPadLeftShock = 0x2c, kPadRightShock = 0x2e,
};

// --- The game's front end (LCSFrontEnd, docs/lcs/front-end.md "The game's own pages") ---
//
// FrontEndMenuManager is a GTA III-style menu: a static page table and a few indices into it, not
// VCS's named widget tree. Read from its input handler (0x08adcad4) and its draw (0x08adf724).
inline constexpr u32 kLCSFrontEnd = 0x08B8EE20;
inline constexpr u32 kLCSFeTabOffset = 0x000;         // s32: the tab the strip is on, 1..8
inline constexpr u32 kLCSFeActiveOffset = 0x131;      // u8: the menu is up (LCSAddr::MenuActive)
inline constexpr u32 kLCSFeSaveRequestOffset = 0x46b; // u8: activate_save_menu (03DD) sets it
inline constexpr u32 kLCSFePageOffset = 0x564;        // s32: the page, an index into the table
inline constexpr u32 kLCSFeRowOffset = 0x568;         // s32: the selected entry on the page
inline constexpr u32 kLCSFeInPageOffset = 0x590;      // u8: 0 while the tab strip has focus
// The menu takes no input until these say so (measured: ~0.3 s after it opens, ~0.1 s after a tab
// change): +0x141 is set once the d-pad and stick are let go and the fade at +0x18 is done (every
// direction getter, 0x08ad954c.., returns 0 until then), and +0x584 is the page's own fade-in.
inline constexpr u32 kLCSFeInputReadyOffset = 0x141;  // u8
inline constexpr u32 kLCSFeFadeOffset = 0x018;        // s32, 255 when done
inline constexpr u32 kLCSFePageFadeOffset = 0x584;    // s32, 255 when done
// The page table at 0x08b316b4, 0x116 bytes a page: a GXT key, then 15 entries of 0x12 (an action,
// a key, a target page, x, y, alignment). The pages the bridge goes to:
inline constexpr int kLCSPageStats = 0;
inline constexpr int kLCSPageGame = 1;         // LOAD GAME, NEW GAME, DELETE SAVE DATA, RESET
inline constexpr int kLCSPageBrief = 2;
inline constexpr int kLCSPageMap = 7;
inline constexpr int kLCSPageNewGameAsk = 9;   // "start a new game?": text, NO, YES
inline constexpr int kLCSPageLoadAsk = 10;     // "load over this game?": text, NO, YES (action 2)
inline constexpr int kLCSPageSaveList = 12;    // the firmware's save list is up over this
// The rows on those pages: LOAD GAME and NEW GAME on the Game page; NO and YES on the question.
inline constexpr int kLCSRowLoadGame = 0;
inline constexpr int kLCSRowNewGame = 1;
inline constexpr int kLCSRowAskYes = 2;
// The tab strip: per tab (1..8) [up, down, left, right, -, label], -1 where nothing is that way.
// Measured: tabs 1-5 map, brief, game, stats, controls along the top, 6-8 audio, display,
// multiplayer below; right wraps within a row, up and down change rows. The language at
// kLCSLanguage == 4 (Spanish) uses the second table, which has the controls tab last.
inline constexpr u32 kLCSTabGrid = 0x08B5905C;
inline constexpr u32 kLCSTabGridSpanish = 0x08B59134;
inline constexpr u32 kLCSTabGridStride = 0x18;
inline constexpr int kLCSTabCount = 8;
// Which page each tab shows (the input handler's switch), in each table.
inline constexpr int kLCSTabPage[kLCSTabCount + 1] = { -1, 7, 2, 1, 0, 3, 4, 5, 14 };
inline constexpr int kLCSTabPageSpanish[kLCSTabCount + 1] = { -1, 7, 2, 1, 0, 4, 5, 14, 3 };
// The button hints along the bottom: `jal 0x08adf0c0`, the only call to what draws them. Taken out
// while the bridge shows a page; the tabs are already gone once a page has focus (the strip is
// only drawn while +0x590 is 0).
inline constexpr LCSCodeWord kLCSMenuHintsCall = { 0x08AE5114, 0x0E2B7C30, 0x00000000 };
// The save menu opens on page 11, "Warning! One or more cheats have been activated" (FET_WRN), when
// the byte at 0x08b56432 is set (every cheat sets it), else on 12, the save list (0x08ad9494, the
// only caller that asks for 11). Its `beqz` made a `b`: the save list always. The warning takes a
// press the auto-save never sent, and the save stalled behind it (reported, 2026-10-11).
inline constexpr LCSCodeWord kLCSSaveCheatWarning = { 0x08AD94B4, 0x10A00005, 0x10000005 };
// The map page draws its rect centred on (+0x474, +0x478) with half-size +0x470, in the PSP's
// screen units, and its scroll keeps |x - 240| and |y - 114| within size - 130 (0x08adae64; past
// that it moves the cursor at +0x47c / +0x480 instead). Not derived: a written centre stays.
inline constexpr u32 kLCSFeMapCentreXOffset = 0x474;  // float
inline constexpr u32 kLCSFeMapCentreYOffset = 0x478;  // float
inline constexpr u32 kLCSFeMapHalfSizeOffset = 0x470; // float
inline constexpr float kLCSMapCentreX = 240.0f;
inline constexpr float kLCSMapCentreY = 114.0f;
inline constexpr float kLCSMapEdge = 130.0f;
// STATS and BRIEF (their draws, 0x08ae366c and 0x08ae490c) scroll by an offset that moves
// 30 / speed a frame while up or down is held, and at no other time. A STATS line is 15 apart and
// sits at i * 15 + 196 - offset (wrapped), so 136 puts the first at the top; it opened wherever it
// was left, with the first line alone at the bottom.
inline constexpr u32 kLCSStatsScroll = 0x08B59CB4;    // float
inline constexpr float kLCSStatsScrollTop = 136.0f;
inline constexpr float kLCSStatsLine = 15.0f;
inline constexpr u32 kLCSBriefScroll = 0x08B59C40;    // float
inline constexpr float kLCSBriefEntry = 31.0f;        // 20 an entry, 11 more a wrapped line
// CMessages::PreviousBriefs, what BRIEF lists: 20 of 32 bytes, the text pointer first (0 empty). The
// dialogue of the missions since the game was loaded; a save loads none.
inline constexpr u32 kLCSPreviousBriefs = 0x08DD1090;
inline constexpr u32 kLCSPreviousBriefStride = 32;
inline constexpr int kLCSPreviousBriefCount = 20;
// The file the game opens once per boot, the moment its two intro movies (logos, credits) are over:
// /PSP_GAME/USRDIR/ENTRIES.DAT, by sector and size as the game asks for it (LCSFrontEnd's boot).
inline constexpr const char *kLCSIntroOverFile = "disc0:/sce_lbn0x6450_size0x30a8";

// register_mission_passed (031D, 0x08a24740) copies the mission's GXT key here (8 bytes) and adds
// one to the counter; register_oddjob_mission_passed (059A) adds one and leaves the key. The key is
// saved and loaded (0x088b253c), the counter is not (zeroed at 0x08845840), so "the counter rose
// and the key changed" is a story mission passed and nothing else.
inline constexpr u32 kLCSLatestMissionKey = 0x08B5E398;
inline constexpr u32 kLCSMissionsPassed = 0x08B5E2C0;

// The script space: a pointer to it, and global N at [ptr] + N * 4 (GetPointerToScriptVariable,
// 0x08956f38: an operand byte 0xcc-0xe5 is a global, (byte - 0xcc) << 8 | next).
inline constexpr u32 kLCSScriptSpacePtr = 0x08B38D7C;
// What the safe house's save routine (MAIN.SCM 0x176d5) sets before 03DD activate_save_menu, and
// which a save made anywhere else must set too (front-end.md "What a save is made of"):
inline constexpr u32 kLCSGlobalFromSave = 6;      // 0 in a save loads into the opening mission (0x6933)
inline constexpr u32 kLCSGlobalRestartX = 7;      // $7..$9 = $537..$539
inline constexpr u32 kLCSGlobalSafehouseX = 537;  // the safe house the load path creates the player at
inline constexpr u32 kLCSGlobalOnMission = 560;   // 1 for the length of the save
inline constexpr u32 kLCSGlobalArmedWeapon = 158; // the load path (INITSAV, 0x48838) gives these back
inline constexpr u32 kLCSGlobalArmour = 160;
// The ammo the routine stores per weapon type, and where: give_player weapon N ammo $global.
struct LCSSavedWeapon {
	u8 type;
	u16 global;
};
inline constexpr LCSSavedWeapon kLCSSavedWeapons[] = {
	{ 36, 124 }, { 1, 125 }, { 2, 126 }, { 7, 127 }, { 3, 128 }, { 4, 129 }, { 6, 130 },
	{ 8, 131 }, { 10, 132 }, { 5, 133 }, { 9, 134 }, { 11, 135 }, { 12, 136 }, { 15, 137 },
	{ 14, 138 }, { 13, 139 }, { 17, 140 }, { 18, 141 }, { 19, 142 }, { 20, 143 }, { 21, 144 },
	{ 22, 145 }, { 24, 146 }, { 23, 147 }, { 25, 148 }, { 27, 149 }, { 26, 150 }, { 30, 151 },
	{ 32, 152 }, { 31, 153 }, { 33, 154 }, { 28, 155 }, { 29, 156 },
};
// A CWeapon's total ammo (get_ammo_in_player_weapon, 041E, reads +0xc) and the ped's armour
// (get_char_armour, 04E2: a float, truncated).
inline constexpr u32 kLCSWeaponAmmoOffset = 0x0c;
inline constexpr u32 kLCSPedArmourOffset = 0x4bc;

// The game's own settings, as its Display and Audio pages write them (0x08ada240, 0x08ad9f7c) and a
// save restores them (0x088b21a4). Each volume is two values: the preference the page shows and the
// save keeps (s32, 0..127), and the byte the mixer reads (the page's setter writes it; nothing
// recomputes it from the preference).
inline constexpr u32 kLCSBrightness = 0x08B59C68;   // s32, 128..384 in 32s; 288 as shipped
inline constexpr u32 kLCSSubtitles = 0x08B59C6C;    // u8
inline constexpr u32 kLCSHudOn = 0x08B59C4A;        // u8
inline constexpr u32 kLCSRadarMode = 0x08B59C4C;    // s32, 0..2
inline constexpr u32 kLCSMusicVolume = 0x08B59C7C;  // s32
inline constexpr u32 kLCSSfxVolume = 0x08B59C78;    // s32
inline constexpr u32 kLCSMusicLevel = 0x08B89D95;   // u8, cAudioManager's (0x08b89d90 + 5)
inline constexpr u32 kLCSSfxLevel = 0x08B89D94;     // u8 (+ 4)
// The text language, 0..4 English, French, German, Italian, Spanish. Set once at boot from the
// PSP's system language (0x089c0e28 asks sceUtilityGetSystemParamInt(8)) and not saved.
inline constexpr u32 kLCSLanguage = 0x08B59C9C;

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
