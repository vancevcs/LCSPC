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

// What the PSP's savedata dialog is showing, so the front-end bridge (LCSFrontEnd) can drive it
// closed-loop. A port of the VCS fork's VCSSaveDialog.h.
//
// Loading and saving both end at a screen the game doesn't own: the firmware's slot list, its
// "overwrite?" prompt, its progress bar, all drawn by PPSSPP. Nothing in PSP memory says which of
// them is up or which slot is highlighted, so PSPSaveDialog reports it. Read-only: the bridge still
// presses the buttons a player would.
//
//   save:  list -> (slot has data) overwrite prompt -> writing -> done
//   load:  list -> loading -> the world restarts under you
//
// The prompt opens on NO and LEFT moves it to YES. `busy` covers the IO thread and the fades; a
// press during either is one the screen underneath never sees.

namespace LCS {

struct SaveDialogPeek {
	bool active = false;    // a savedata dialog is up
	bool save = false;      // saving, as opposed to loading or deleting
	bool list = false;      // the slot list has focus
	bool confirm = false;   // a yes/no prompt is up
	bool busy = false;      // reading, writing, fading, or a screen the bridge doesn't drive
	bool done = false;      // finished, waiting to be acknowledged
	int selected = -1;      // index into the list
	int count = 0;          // entries in the list
	int yesno = 0;          // 1 while YES is highlighted on a prompt
	// The PSP buttons the dialog treats as OK and Back (CTRL_ masks): a firmware setting, and
	// "save completed" accepts only Back.
	u32 okButton = 0;
	u32 cancelButton = 0;
	// Each entry's save name ("S0", "S3"): the game's own slot, which is what the bridge steers to.
	// A load list holds only the saves that exist, so an index is not a slot.
	static constexpr int kMaxNames = 16;
	char names[kMaxNames][16]{};
};

// False when no savedata dialog is up. Emu thread (Core/HLE/sceUtility.cpp).
bool PeekSaveDialog(SaveDialogPeek *out);

}  // namespace LCS
