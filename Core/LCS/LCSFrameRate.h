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

// 60 fps for LCS: the frame cap (kLCSFrameCap) and the millisecond carry (kLCSTimerCarry), both
// in or both out, following LCS::Settings().frameRate60.
//
// Applied from LCS::Tick and re-checked every tick, so a savestate (which replaces PSP memory
// wholesale) ends up in whatever state the setting asks for. A word holding neither the game's op
// nor ours is never written. docs/lcs/frame-rate.md has the measurements.

namespace LCS {

void FrameRateTick();
void FrameRateReset();

// Whether both patches are in.
bool FrameRateIs60();

}  // namespace LCS
