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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/NativeApp.h"
#include "Core/KeyMap.h"
#include "Core/LCS/LCSInput.h"
#include "UI/LCSDevInput.h"

static bool ParseDevKey(const std::string &name, InputKeyCode *code, InputDeviceID *device) {
	*device = DEVICE_ID_KEYBOARD;
	if (name.size() == 6 && equalsNoCase(name.substr(0, 5), "MOUSE") && name[5] >= '1' && name[5] <= '5') {
		static const InputKeyCode kButtons[] = {
			NKCODE_EXT_MOUSEBUTTON_1, NKCODE_EXT_MOUSEBUTTON_2, NKCODE_EXT_MOUSEBUTTON_3,
			NKCODE_EXT_MOUSEBUTTON_4, NKCODE_EXT_MOUSEBUTTON_5,
		};
		*code = kButtons[name[5] - '1'];
		*device = DEVICE_ID_MOUSE;
		return true;
	}
	if (equalsNoCase(name, "WHEELUP") || equalsNoCase(name, "WHEELDOWN")) {
		*code = equalsNoCase(name, "WHEELUP") ? NKCODE_EXT_MOUSEWHEEL_UP : NKCODE_EXT_MOUSEWHEEL_DOWN;
		*device = DEVICE_ID_MOUSE;
		return true;
	}
	for (int i = 1; i < 1000; i++) {
		if (equalsNoCase(KeyMap::GetKeyName((InputKeyCode)i), name)) {
			*code = (InputKeyCode)i;
			return true;
		}
	}
	return false;
}

void LCSDevKeyPipeTick() {
	static const char *path = getenv("LCS_KEY_PIPE");
	if (!path || !*path) {
		return;
	}
	static std::deque<std::string> queue;
	static int wait = 0;

	std::string contents;
	if (File::ReadTextFileToString(Path(path), &contents)) {
		File::Delete(Path(path));
		std::vector<std::string> lines;
		SplitString(contents, '\n', lines);
		for (std::string line : lines) {
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.pop_back();
			}
			if (!line.empty()) {
				queue.push_back(line);
			}
		}
	}

	if (wait > 0) {
		wait--;
		return;
	}
	while (!queue.empty()) {
		const std::string line = queue.front();
		queue.pop_front();
		const size_t space = line.find(' ');
		const std::string verb = line.substr(0, space);
		const std::string arg = space == std::string::npos ? "" : line.substr(space + 1);
		if (verb == "wait") {
			wait = atoi(arg.c_str());
			return;
		}
		if (verb == "look") {
			// Straight to the LCS layer rather than through NativeMouseDelta, so a test runs with
			// "Use mouse control" off and the desk's real mouse can't add to what the script sends.
			float dx = 0.0f, dy = 0.0f;
			if (sscanf(arg.c_str(), "%f %f", &dx, &dy) == 2) {
				LCS::HandleMouseDelta(dx, dy);
			}
			continue;
		}
		InputKeyCode code;
		InputDeviceID device;
		if (!ParseDevKey(arg, &code, &device)) {
			NOTICE_LOG(Log::System, "LCS_KEY_PIPE: no key called '%s'", arg.c_str());
			continue;
		}
		if (verb == "press") {
			NativeKey(KeyInput(device, code, KeyInputFlags::DOWN));
			queue.push_front("up " + arg);
			wait = 2;
			return;
		}
		NativeKey(KeyInput(device, code, verb == "up" ? KeyInputFlags::UP : KeyInputFlags::DOWN));
	}
}
