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

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSFrameRate.h"
#include "Core/LCS/LCSSettings.h"

namespace LCS {

static bool g_is60 = false;

// What the log last said, so it says each change once.
enum class Reported {
	At30,
	At60,
	Foreign,
};
static Reported g_reported = Reported::At30;

enum class WordState {
	Stock,
	Patched,
	Foreign,   // Neither: another build, code not loaded yet, or someone else's patch.
};

static WordState ReadWord(const LCSCodeWord &word) {
	if (!Memory::IsValid4AlignedAddress(word.address)) {
		return WordState::Foreign;
	}
	// Read_Instruction sees through a JIT block marker.
	const u32 op = Memory::Read_Instruction(word.address, true).encoding;
	return op == word.stock ? WordState::Stock : op == word.patched ? WordState::Patched : WordState::Foreign;
}

// Puts one word into the asked-for form. The caller has already checked it isn't Foreign.
// Invalidating immediately puts the game's own word back over any block marker, so the raw read
// and write below see code rather than a marker. Tick runs between blocks (from the vblank event),
// so no block is executing. The deferred invalidate after the write is belt and braces.
static void WriteWord(const LCSCodeWord &word, bool patched) {
	const u32 target = patched ? word.patched : word.stock;
	currentMIPS->InvalidateICacheRangeImmediate(word.address, 4);
	if (Memory::ReadUnchecked_U32(word.address) != target) {
		Memory::WriteUnchecked_U32(target, word.address);
		currentMIPS->InvalidateICacheRangeDeferred(word.address, 4);
	}
}

void FrameRateTick() {
	if (!currentMIPS) {
		return;
	}
	const bool want = Settings().frameRate60;

	// All six words are checked before any is written: a half-applied set (the cap without the
	// carry) is a game whose clock runs 4% slow, and the reverse changes nothing anyone asked for.
	bool foreign = false;
	bool changes = false;
	auto check = [&](const LCSCodeWord &word) {
		const WordState state = ReadWord(word);
		if (state == WordState::Foreign) {
			foreign = true;
		} else if ((state == WordState::Patched) != want) {
			changes = true;
		}
	};
	check(kLCSFrameCap);
	for (const LCSCodeWord &word : kLCSTimerCarry) {
		check(word);
	}

	if (!foreign && changes) {
		WriteWord(kLCSFrameCap, want);
		for (const LCSCodeWord &word : kLCSTimerCarry) {
			WriteWord(word, want);
		}
		if (want) {
			Memory::WriteUnchecked_U32(0, kLCSTimerRemainder);
		}
	}

	g_is60 = want && !foreign;
	const Reported now = foreign ? Reported::Foreign : g_is60 ? Reported::At60 : Reported::At30;
	if (now != g_reported) {
		if (foreign) {
			NOTICE_LOG(Log::System, "LCS: frame rate words are neither the game's nor ours; leaving them alone");
		} else {
			NOTICE_LOG(Log::System, "LCS: frame rate %d (cap and millisecond carry %s)", g_is60 ? 60 : 30, g_is60 ? "in" : "out");
		}
		g_reported = now;
	}
}

void FrameRateReset() {
	g_is60 = false;
	g_reported = Reported::At30;
}

bool FrameRateIs60() {
	return g_is60;
}

}  // namespace LCS
