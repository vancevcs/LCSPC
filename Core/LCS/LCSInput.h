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
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"

// Keyboard and mouse for LCS, by context: WASD is the stick on foot and the pedals in a car, the
// mouse turns the camera. docs/lcs/input.md has the bindings and why each is what it is.
//
// Threads: HandleHostKey and HandleMouseDelta run on the UI thread and only record (under a
// mutex). InputTick runs on the emu thread from LCS::Tick and is the only thing that touches
// sceCtrl or PSP memory. Only buttons this layer pressed are ever released.

class Section;

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

// PSP buttons pressed from Debug -> LCS -> Pad (CTRL_* masks). Taps play in order on the vblank
// tick, each held 6 vblanks with 6 between, which is what a cheat needs; held buttons stay down
// until released. ResetInput drops both.
void QueuePadTaps(const std::vector<u32> &masks);
void SetPadHeld(u32 mask, bool held);
u32 PadHeld();
int PadTapsPending();
// "Up Square, Square Down" -> CTRL_* masks, one per tap; "+" joins buttons pressed together
// ("L+R"). Names: Up Down Left Right Triangle Circle Cross (or X) Square L R Start Select, any
// case. False with the first unknown word in *bad.
bool ParsePadSequence(const std::string &text, std::vector<u32> *out, std::string *bad);

InputContext CurrentInputContext();
// The keyboard is this layer's (LCS active, KeyboardMouse on): Windows' Alt menu stands down, since
// Alt walks (MainWindow's SC_KEYMENU).
bool KeyboardLayerOn();
u32 AppliedButtons();
bool MouseLookEngaged();

// Mouse movement and wheel notches that arrived while a menu was up. Raw mouse deltas keep coming
// with the emulator paused and would be spent on the camera at the first frame after; the front
// end drops them as it closes. UI thread.
void DropPendingMouse();

// One key of a card line, as bound: the action's shipped key (its name in kKeyMappings and
// lcs.ini) and the key that does it now. `what` names it within its line ("forward"), or null.
struct KeyBindPart {
	InputKeyCode shipped;
	InputKeyCode bound;
	const char *what;
};

// One line of the front end's controls card: an action, the keys and mouse buttons that do it in
// one context, and the PSP button(s) they press. Built from kKeyMappings and the stick keys, so it
// cannot disagree with what the keys do. `parts` are the keys that can be rebound, in order; a line
// without any (the mouse) is a fact, not a setting.
struct KeyListingRow {
	std::string action;
	std::vector<std::string> keys;
	std::string psp;
	std::vector<KeyBindPart> parts;
};
std::vector<KeyListingRow> KeyListing(InputContext context);

// Rebinding, per context: an action is named by its shipped key, and BoundKey is what does it now.
// A clash swaps the two actions' keys. Escape can't be bound (it is the pause and the cancel).
// lcs.ini [KeyBindings] keeps only what moved. UI thread and emu thread alike (under the mutex).
InputKeyCode BoundKey(InputContext context, InputKeyCode shipped);
void BindKey(InputContext context, InputKeyCode shipped, InputKeyCode key);
void ResetKeyBindings();
bool HasKeyBindings();
void LoadKeyBindings(const Section *section);
void SaveKeyBindings(Section *section);
// A key as the card names it ("LMB", "LEFT SHIFT", "F").
std::string CardKeyName(InputKeyCode key);

}  // namespace LCS
