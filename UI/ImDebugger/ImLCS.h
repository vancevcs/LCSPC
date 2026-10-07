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

#include "Common/CommonTypes.h"

struct ImConfig;

// The LCS debugger window (Debug -> LCS). In UI/ so that Core/LCS has no ImGui dependency.
//
// Shows whether the layer is active and why not, the decoded state, the address table with live
// values, and a scratchpad of candidate addresses read every frame. Runs on the CPU thread, like
// the rest of the ImGui debugger (docs/DebuggerThreading.md).

class ImLCSWindow {
public:
	void Draw(ImConfig &cfg);

private:
	void DrawStatus();
	void DrawDecodedState();
	void DrawAddressTable();
	void DrawScratchpad();

	static constexpr int kScratchRows = 8;

	struct ScratchRow {
		char addressText[16] = "";
		bool hold = false;   // Stop refreshing, so a fast-changing value can be read.
		u32 heldRaw = 0;
		bool hasHeld = false;
	};

	ScratchRow scratch_[kScratchRows];
};
