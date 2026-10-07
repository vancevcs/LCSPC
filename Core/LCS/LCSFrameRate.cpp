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
#include "Core/LCS/LCSMemory.h"
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
		const CodeWordState state = ReadCodeWord(word);
		if (state == CodeWordState::Foreign) {
			foreign = true;
		} else if ((state == CodeWordState::Patched) != want) {
			changes = true;
		}
	};
	check(kLCSFrameCap);
	for (const LCSCodeWord &word : kLCSTimerCarry) {
		check(word);
	}

	if (!foreign && changes) {
		SetCodeWord(kLCSFrameCap, want);
		for (const LCSCodeWord &word : kLCSTimerCarry) {
			SetCodeWord(word, want);
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
