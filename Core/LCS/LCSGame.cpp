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

#include <cstring>

#include "Common/Crypto/sha1.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSChaseCam.h"
#include "Core/LCS/LCSFrameRate.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"

namespace LCS {

// Candidate: the compat flag, disc ID and version all match, so the EBOOT gets hashed.
// Active: the hash matched too, and the first tick found the module where the table expects it.
static bool g_candidate = false;
static bool g_active = false;
static bool g_baseChecked = false;
static std::string g_discID;
static std::string g_discVersion;
static std::string g_inactiveReason;
static std::string g_ebootHash;
static u64 g_tickCount = 0;

static void Deactivate(const std::string &reason) {
	g_active = false;
	g_candidate = false;
	g_inactiveReason = reason;
	NOTICE_LOG(Log::System, "LCS: inactive - %s", reason.c_str());
}

void Init() {
	g_candidate = false;
	g_active = false;
	g_baseChecked = false;
	g_ebootHash.clear();
	g_tickCount = 0;
	ClearState();
	FrameRateReset();
	ResetInput();

	g_discID = g_paramSFO.GetDiscID();
	g_discVersion = g_paramSFO.GetValueString("DISC_VERSION");

	if (!PSP_CoreParameter().compat.flags().LCSOverhaul) {
		// Every other game lands here. Say nothing in the log for them.
		g_inactiveReason = "the LCSOverhaul compat flag is off for this disc";
		return;
	}
	if (g_discID != kLCSDiscID) {
		Deactivate(StringFromFormat("disc ID %s, only %s is supported", g_discID.c_str(), kLCSDiscID));
		return;
	}
	if (g_discVersion != kLCSDiscVersion) {
		Deactivate(StringFromFormat("disc version %s, only %s is supported (addresses differ between revisions)",
			g_discVersion.c_str(), kLCSDiscVersion));
		return;
	}

	g_candidate = true;
	g_inactiveReason = "waiting for the EBOOT to load";
	if (g_Config.bLoadPlugins) {
		// A PSP plugin such as GTALCS.PPSSPP.WidescreenFix patches the code this layer reads and
		// hooks. It shows up as a hook refusing to install, which looks exactly like a wrong address.
		NOTICE_LOG(Log::System, "LCS: PSP plugins are enabled (LoadPlugins). Turn them off for this game while measuring.");
	}
}

void NoteBootExecutable(const u8 *elf, size_t size) {
	if (!g_candidate || g_active) {
		return;
	}

	u8 digest[20];
	sha1(const_cast<unsigned char *>(elf), (int)size, digest);
	g_ebootHash.clear();
	for (u8 b : digest) {
		g_ebootHash += StringFromFormat("%02x", b);
	}

	if (size != kLCSEbootSize || memcmp(digest, kLCSEbootSha1, sizeof(digest)) != 0) {
		Deactivate(StringFromFormat("EBOOT %s (%d bytes) is not the build the address table was measured on",
			g_ebootHash.c_str(), (int)size));
		return;
	}

	g_active = true;
	g_inactiveReason.clear();
	// Only once we know this is the game: no other boot reads or creates lcs.ini.
	LoadSettings();
	int known = 0;
	for (const LCSAddrEntry &entry : kLCSAddresses) {
		if (IsAddrSet(entry.id)) {
			known++;
		}
	}
	NOTICE_LOG(Log::System, "LCS: active for %s v%s, EBOOT %s (%d/%d addresses known)",
		g_discID.c_str(), g_discVersion.c_str(), g_ebootHash.c_str(), known, (int)LCSAddr::Count);
}

void Shutdown() {
	if (g_active) {
		RemoveChaseCam();
	}
	ClearState();
	FrameRateReset();
	ResetInput();
	g_candidate = false;
	g_active = false;
	g_baseChecked = false;
	g_tickCount = 0;
}

void Tick() {
	if (!g_active) {
		return;
	}

	// The hash says which code this is, not where it was put. One instruction at a known address
	// checks both the base and that nothing else has patched the frame wait. Read_Instruction,
	// because a raw read of code the JIT has compiled can return a block marker.
	if (!g_baseChecked) {
		const LCSCodeWord &cap = kLCSFrameCap;
		const u32 op = Memory::IsValidAddress(cap.address) ? Memory::Read_Instruction(cap.address, false).encoding : 0;
		if (op != cap.stock && op != cap.patched) {
			Deactivate(StringFromFormat("expected %08x at %08x, found %08x (module not at %08x, or patched)",
				cap.stock, cap.address, op, kLCSModuleBase));
			return;
		}
		g_baseChecked = true;
		NOTICE_LOG(Log::System, "LCS: module base %08x confirmed", kLCSModuleBase);
	}

	g_tickCount++;
	FrameRateTick();
	InstallChaseCam();
	UpdateSharedState();
	InputTick(GetState());
}

void OnFrameBoundary(u32 returnAddress) {
	if (!g_active || !g_baseChecked || returnAddress != kLCSFrameBoundaryReturn) {
		return;
	}
	InputFrameBoundary();
}

bool IsActive() {
	return g_active;
}

const std::string &GetDiscID() {
	return g_discID;
}

const std::string &GetDiscVersion() {
	return g_discVersion;
}

const std::string &InactiveReason() {
	return g_inactiveReason;
}

const std::string &EbootHash() {
	return g_ebootHash;
}

u64 GetTickCount() {
	return g_tickCount;
}

}  // namespace LCS
