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

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"

// Keyboard and mouse for LCS, by context: WASD is the stick on foot and the pedals in a car, the
// mouse turns the camera. docs/lcs/input.md has the bindings and why each is what it is.
//
// Threads: HandleHostKey and HandleMouseDelta run on the UI thread and only record (under a
// mutex). InputTick runs on the emu thread from LCS::Tick and is the only thing that touches
// sceCtrl or PSP memory. Only buttons this layer pressed are ever released.

namespace LCS {

struct LCSState;

enum class InputContext {
	Unknown,    // no player, or the layer is off: nothing is claimed
	OnFoot,
	InVehicle,
	Menu,       // the game's pause menu
};

const char *InputContextName(InputContext context);

struct KeyMapping {
	InputContext context;
	InputKeyCode key;
	u32 psp;                // CTRL_* bits; 0 claims the key and presses nothing
	const char *description;
};

extern const KeyMapping kKeyMappings[];
extern const size_t kKeyMappingCount;

// From NativeKey. True when the key is claimed (PPSSPP's mapper must not also see it).
bool HandleHostKey(const KeyInput &key);
// From NativeMouseDelta. True when the mouse is driving the camera.
bool HandleMouseDelta(float dx, float dy);

// From LCS::Tick, after the state is decoded: context, buttons and the stick.
void InputTick(const LCSState &state);
// From the game's frame boundary (LCS::OnFrameBoundary), between frames: the mouse turns the camera.
void InputFrameBoundary();
// Releases everything this layer holds and forgets held keys.
void ResetInput();

InputContext CurrentInputContext();
u32 AppliedButtons();
bool MouseLookEngaged();

}  // namespace LCS
