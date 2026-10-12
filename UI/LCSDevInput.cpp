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
#include "Common/UI/ScreenManager.h"
#include "Core/KeyMap.h"
#include "Core/HW/Display.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/SaveState.h"
#include "UI/LCSDevInput.h"

extern ScreenManager *g_screenManager;  // UI/NativeApp.cpp

// A pad's button, as an XInput pad sends it: "PAD:A", "PAD:LB", "PAD:START", "PAD:DUP".
static bool ParsePadButton(const std::string &name, InputKeyCode *code) {
	static const struct { const char *name; InputKeyCode code; } kButtons[] = {
		{ "A", NKCODE_BUTTON_A }, { "B", NKCODE_BUTTON_B }, { "X", NKCODE_BUTTON_X }, { "Y", NKCODE_BUTTON_Y },
		{ "LB", NKCODE_BUTTON_L1 }, { "RB", NKCODE_BUTTON_R1 }, { "LS", NKCODE_BUTTON_THUMBL }, { "RS", NKCODE_BUTTON_THUMBR },
		{ "START", NKCODE_BUTTON_START }, { "VIEW", NKCODE_BUTTON_SELECT },
		{ "DUP", NKCODE_DPAD_UP }, { "DDOWN", NKCODE_DPAD_DOWN }, { "DLEFT", NKCODE_DPAD_LEFT }, { "DRIGHT", NKCODE_DPAD_RIGHT },
	};
	for (const auto &b : kButtons) {
		if (equalsNoCase(name, b.name)) {
			*code = b.code;
			return true;
		}
	}
	return false;
}

static bool ParseDevKey(const std::string &name, InputKeyCode *code, InputDeviceID *device) {
	*device = DEVICE_ID_KEYBOARD;
	if (name.size() > 4 && equalsNoCase(name.substr(0, 4), "PAD:")) {
		*device = DEVICE_ID_XINPUT_0;
		return ParsePadButton(name.substr(4), code);
	}
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

// To the LCS layer first, as NativeKey would, but without NativeKey's ImGui filtering: with an
// ImGui window focused (a click on Debug -> LCS is enough) NativeKey keeps keyboard keys from the
// game, and a test then sees nothing happen. Keys the layer doesn't claim go through NativeKey as
// usual. The screen's own gate is kept: under a screen that passes nothing to the mapper (the LCS
// menu), only releases reach the layer, exactly as from a real keyboard.
static void SendDevKey(const KeyInput &key) {
	const bool gated = (key.flags & KeyInputFlags::DOWN) && g_screenManager &&
		g_screenManager->PassInputToMapper() == InputMode::None;
	if (gated || !LCS::HandleHostKey(key)) {
		NativeKey(key);
	}
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
		if (verb == "savestate" || verb == "loadstate") {
			// Test fixtures: a state at a known place (in a car, at a wall) instead of a boot and a
			// scripted walk. Queued like a hotkey, so it lands between frames.
			const Path file(arg);
			const bool save = verb == "savestate";
			auto done = [save, arg](SaveState::Status status, std::string_view message, std::string_view) {
				NOTICE_LOG(Log::System, "LCS_KEY_PIPE: %s %s - %s", save ? "savestate" : "loadstate", arg.c_str(),
					status == SaveState::Status::SUCCESS ? "done" : std::string(message).c_str());
			};
			if (save) {
				SaveState::Save(file, -1, done);
			} else {
				SaveState::Load(file, -1, done);
			}
			continue;
		}
		if (verb == "pad") {
			// PSP buttons, as Debug -> LCS -> Pad plays them: "pad Up Square Square Down".
			std::vector<u32> masks;
			std::string bad;
			if (LCS::ParsePadSequence(arg, &masks, &bad)) {
				LCS::QueuePadTaps(masks);
			} else {
				NOTICE_LOG(Log::System, "LCS_KEY_PIPE: no PSP button called '%s'", bad.c_str());
			}
			continue;
		}
		if (verb == "axis") {
			// An XInput pad's axis through NativeAxis, as a real one arrives: "axis LT 1", "axis RX 0.5".
			// LX LY RX RY (Y positive pushed away), LT RT.
			static const struct { const char *name; InputAxis axis; } kAxes[] = {
				{ "LX", JOYSTICK_AXIS_X }, { "LY", JOYSTICK_AXIS_Y }, { "RX", JOYSTICK_AXIS_Z },
				{ "RY", JOYSTICK_AXIS_RZ }, { "LT", JOYSTICK_AXIS_LTRIGGER }, { "RT", JOYSTICK_AXIS_RTRIGGER },
			};
			char name[8] = {};
			float value = 0.0f;
			if (sscanf(arg.c_str(), "%7s %f", name, &value) == 2) {
				for (const auto &a : kAxes) {
					if (equalsNoCase(name, a.name)) {
						AxisInput axis{};
						axis.deviceId = DEVICE_ID_XINPUT_0;
						axis.axisId = a.axis;
						axis.value = value;
						NativeAxis(&axis, 1);
					}
				}
			}
			continue;
		}
		if (verb == "set") {
			// An lcs.ini row by its key, live and unsaved, as the menu would set it: "set Shadows 0".
			const size_t gap = arg.find(' ');
			const std::string key = arg.substr(0, gap);
			const std::string value = gap == std::string::npos ? "" : arg.substr(gap + 1);
			bool found = false;
			for (const LCS::Option &opt : LCS::Options()) {
				if (!opt.iniKey || !equalsNoCase(key, opt.iniKey)) {
					continue;
				}
				found = true;
				switch (opt.type) {
				case LCS::OptionType::Bool:
					*opt.boolValue = value == "1" || equalsNoCase(value, "true");
					break;
				case LCS::OptionType::Float:
					*opt.floatValue = (float)atof(value.c_str());
					break;
				case LCS::OptionType::Int:
				case LCS::OptionType::Choice:
					*opt.intValue = atoi(value.c_str());
					break;
				}
				if (opt.onChange) {
					opt.onChange();
				}
			}
			NOTICE_LOG(Log::System, "LCS_KEY_PIPE: set %s %s%s", key.c_str(), value.c_str(), found ? "" : " - no such row");
			continue;
		}
		if (verb == "fps") {
			float vps = 0.0f, fps = 0.0f, actual = 0.0f;
			__DisplayGetFPS(&vps, &fps, &actual);
			NOTICE_LOG(Log::System, "LCS_KEY_PIPE: fps %.1f (vps %.1f, actual %.1f)", fps, vps, actual);
			continue;
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
			SendDevKey(KeyInput(device, code, KeyInputFlags::DOWN));
			queue.push_front("up " + arg);
			wait = 2;
			return;
		}
		SendDevKey(KeyInput(device, code, verb == "up" ? KeyInputFlags::UP : KeyInputFlags::DOWN));
	}
}
