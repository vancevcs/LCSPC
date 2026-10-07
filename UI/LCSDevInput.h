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

// Dev only: keyboard and mouse events read from the file named by LCS_KEY_PIPE and fed through
// NativeKey (keys) and LCS::HandleMouseDelta (mouse), so the LCS input layer can be tested from a
// script without sending keystrokes to Windows. Nothing happens when the variable is unset.
//
// The file is read and deleted once a frame; append lines to it:
//   press KEY        down, then up two frames later
//   down KEY / up KEY
//   look DX DY       a mouse movement in counts, straight to LCS::HandleMouseDelta
//   wait N           N frames
//   savestate PATH / loadstate PATH   a PPSSPP savestate, for test fixtures
// KEY is a PPSSPP key name (W, Space, LShift, Enter, Backspace...), MOUSE1-MOUSE5, WHEELUP,
// WHEELDOWN.

void LCSDevKeyPipeTick();
