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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/Request.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/KeyMap.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSChaseCam.h"
#include "Core/LCS/LCSFreeAim.h"
#include "Core/LCS/LCSFrontEnd.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"

namespace LCS {

static constexpr InputContext kFoot = InputContext::OnFoot;
static constexpr InputContext kCar = InputContext::InVehicle;
static constexpr InputContext kMenu = InputContext::Menu;

// The stock PSP layout (configuration 1 in the game's options), from the game's own help text: the
// C0* control tokens in ENGLISH.GXT (docs/lcs/input.md lists them). Keys follow the VCS fork's
// where the action exists in both games.
const KeyMapping kKeyMappings[] = {
	{ kFoot, NKCODE_SPACE,             CTRL_SQUARE,                    "Jump" },
	{ kFoot, NKCODE_SHIFT_LEFT,        CTRL_CROSS,                     "Sprint" },
	{ kFoot, NKCODE_EXT_MOUSEBUTTON_1, CTRL_CIRCLE,                    "Attack / fire" },
	{ kFoot, NKCODE_CTRL_LEFT,         CTRL_CIRCLE,                    "Attack / fire" },
	{ kFoot, NKCODE_EXT_MOUSEBUTTON_2, CTRL_RTRIGGER,                  "Aim (hold; free aim with a gun)" },
	{ kFoot, NKCODE_EXT_MOUSEBUTTON_3, CTRL_RTRIGGER,                  "Lock on (hold)" },
	{ kFoot, NKCODE_X,                 CTRL_DOWN,                      "Free aim (tap while locked on)" },
	{ kFoot, NKCODE_F,                 CTRL_TRIANGLE,                  "Enter vehicle / hang up" },
	{ kFoot, NKCODE_Q,                 CTRL_LEFT,                      "Previous weapon / previous target" },
	{ kFoot, NKCODE_E,                 CTRL_RIGHT,                     "Next weapon / next target" },
	{ kFoot, NKCODE_TAB,               CTRL_LTRIGGER,                  "Answer phone / swap for a weapon pickup" },
	{ kFoot, NKCODE_C,                 CTRL_LTRIGGER | CTRL_RTRIGGER,  "Look behind" },
	{ kFoot, NKCODE_V,                 CTRL_SELECT,                    "Change camera" },
	{ kFoot, NKCODE_G,                 CTRL_UP,                        "Side missions on / off" },
	{ kFoot, NKCODE_ENTER,             CTRL_START,                     "Pause menu" },
	// Walk: no button, a smaller push of the stick (ApplyStick, kWalkDeflection). Both Alts, so the
	// right one isn't left to PPSSPP's mapping.
	{ kFoot, NKCODE_ALT_LEFT,          0,                              "Walk (with WASD)" },
	{ kFoot, NKCODE_ALT_RIGHT,         0,                              "Walk (with WASD)" },

	{ kCar,  NKCODE_W,                 CTRL_CROSS,                     "Accelerate" },
	{ kCar,  NKCODE_S,                 CTRL_SQUARE,                    "Brake / reverse" },
	// The game's configuration 2, which the input layer holds it in (kLCSPadModeOffset): R the
	// handbrake, and every look L with the stick that way (kGlanceKeys).
	{ kCar,  NKCODE_SPACE,             CTRL_RTRIGGER,                  "Handbrake" },
	{ kCar,  NKCODE_EXT_MOUSEBUTTON_1, CTRL_CIRCLE,                    "Fire (a drive-by with Q or E held)" },
	{ kCar,  NKCODE_CTRL_LEFT,         CTRL_CIRCLE,                    "Fire (a drive-by with Q or E held)" },
	{ kCar,  NKCODE_Q,                 CTRL_LTRIGGER,                  "Look left / drive-by left" },
	{ kCar,  NKCODE_E,                 CTRL_LTRIGGER,                  "Look right / drive-by right" },
	{ kCar,  NKCODE_C,                 CTRL_LTRIGGER,                  "Look behind" },
	{ kCar,  NKCODE_H,                 CTRL_DOWN,                      "Horn" },
	{ kCar,  NKCODE_R,                 CTRL_LEFT,                      "Previous radio station" },
	{ kCar,  NKCODE_T,                 CTRL_RIGHT,                     "Next radio station" },
	{ kCar,  NKCODE_F,                 CTRL_TRIANGLE,                  "Exit vehicle / hang up" },
	{ kCar,  NKCODE_TAB,               CTRL_LTRIGGER,                  "Answer phone" },
	{ kCar,  NKCODE_V,                 CTRL_SELECT,                    "Change camera" },
	{ kCar,  NKCODE_G,                 CTRL_UP,                        "Side missions on / off" },
	{ kCar,  NKCODE_ENTER,             CTRL_START,                     "Pause menu" },
	// Claimed so PPSSPP's rapid-fire (its default for Left Shift) can't hit the pedals.
	{ kCar,  NKCODE_SHIFT_LEFT,        0,                              "(nothing)" },

	{ kMenu, NKCODE_W,                 CTRL_UP,                        "Move" },
	{ kMenu, NKCODE_S,                 CTRL_DOWN,                      "Move" },
	{ kMenu, NKCODE_A,                 CTRL_LEFT,                      "Move" },
	{ kMenu, NKCODE_D,                 CTRL_RIGHT,                     "Move" },
	{ kMenu, NKCODE_ENTER,             CTRL_CROSS,                     "Select" },
	{ kMenu, NKCODE_SPACE,             CTRL_CROSS,                     "Select" },
	{ kMenu, NKCODE_DEL,               CTRL_CIRCLE,                    "Back" },
	// The map's own controls, which its hint bar names: L / R zoom, Square the target marker.
	{ kMenu, NKCODE_Q,                 CTRL_LTRIGGER,                  "Zoom out (map)" },
	{ kMenu, NKCODE_E,                 CTRL_RTRIGGER,                  "Zoom in (map)" },
	{ kMenu, NKCODE_F,                 CTRL_SQUARE,                    "Place / remove marker (map)" },
	// No button: held, the mouse drags the map (LCS::DragMap).
	{ kMenu, NKCODE_EXT_MOUSEBUTTON_1, 0,                              "Drag (map)" },
};
const size_t kKeyMappingCount = ARRAY_SIZE(kKeyMappings);

// The keys that move the left stick, per context. On foot WASD walks; in a vehicle A/D steer and
// the arrows lean a bike or aim a water cannon ("analog stick up and down" in the game's text).
struct StickKey {
	InputContext context;
	InputKeyCode key;
	float x, y;
};
static const StickKey kStickKeys[] = {
	{ kFoot, NKCODE_W, 0.0f, 1.0f },
	{ kFoot, NKCODE_S, 0.0f, -1.0f },
	{ kFoot, NKCODE_A, -1.0f, 0.0f },
	{ kFoot, NKCODE_D, 1.0f, 0.0f },
	{ kCar, NKCODE_A, -1.0f, 0.0f },
	{ kCar, NKCODE_D, 1.0f, 0.0f },
	{ kCar, NKCODE_DPAD_UP, 0.0f, 1.0f },
	{ kCar, NKCODE_DPAD_DOWN, 0.0f, -1.0f },
};

// The looks in a vehicle: in the game's configuration 2 each is L with the stick that way (the
// game's own words: VELL "L button + analog stick left", VELR, VELB "L button + analog stick
// down"), and a drive-by is a look and fire. So Q, E and C are L, and while one is held it decides
// the stick on its own: Q with D held still looks left. With L held the stick doesn't steer
// (measured: Q matched L alone). Q and E together are C. The first build pressed R for E, which in
// configuration 2 is the handbrake: E steered and braked, Q + E stopped the car (reported).
struct GlanceKey {
	InputKeyCode key;
	float x, y;
};
static const GlanceKey kGlanceKeys[] = {
	{ NKCODE_Q, -1.0f, 0.0f },
	{ NKCODE_E, 1.0f, 0.0f },
	{ NKCODE_C, 0.0f, -1.0f },
};

// Walk: how far the stick is pushed while Alt is held. Measured on foot: nothing to 0.2, a walk of
// 0.044 a frame from 0.3 to 0.6, faster from 0.7, the run (0.176) at 1.0 - so the middle of the walk.
// After the diagonal's normalisation, or W + D would be past it.
static constexpr float kWalkDeflection = 0.45f;

// One notch of the wheel is a press of one d-pad direction: weapons on foot, stations in a car.
static constexpr int kPulseHoldTicks = 4;   // vblanks; the game samples the pad once a frame
static constexpr int kPulseGapTicks = 2;

// --- Shared with the UI thread, under g_mutex ---

struct HeldKey {
	InputContext context;   // where it was pressed
	bool claimed;           // whether its press was kept from PPSSPP's mapper
	bool silent = false;    // claimed, and spent on something else: it presses nothing while held
	// The key it stands for: the shipped key of the action it is bound to in `context`. Everything
	// downstream (the table, the stick, the drive-by, the aim key) asks about this one.
	InputKeyCode as = NKCODE_UNKNOWN;
};

// Rebinding (the front end's key cards): per context, the shipped keys whose action the player moved
// to another key. A key not in here does what the table says. Under g_mutex.
static std::map<std::pair<int, int>, InputKeyCode> g_bindings;
static std::mutex g_mutex;
static std::map<InputKeyCode, HeldKey> g_held;
static float g_mouseDX = 0.0f;
static float g_mouseDY = 0.0f;
static int g_wheelUp = 0;
static int g_wheelDown = 0;

static std::atomic<InputContext> g_context{InputContext::Unknown};
static std::atomic<bool> g_mouseDriving{false};

// --- Emu thread only ---

static u32 g_lastMask = 0;
static bool g_stickHeld = false;
static bool g_cameraTakenOver = false;
static int g_pendingWheel = 0;      // positive: next, negative: previous
static u32 g_pulseButton = 0;
static int g_pulseTicks = 0;
static int g_pulseGap = 0;

// PSP buttons pressed from Debug -> LCS -> Pad. Under g_mutex: the window queues, the tick plays.
// A tap is held for kPadTapTicks vblanks, then nothing for kPadGapTicks: the game samples the pad
// once a frame (two vblanks at 30 fps), and a cheat needs a release between two presses of the
// same button.
static constexpr int kPadTapTicks = 6;
static constexpr int kPadGapTicks = 6;
static std::deque<u32> g_padQueue;
static u32 g_padTap = 0;
static int g_padTicks = 0;      // > 0 holding g_padTap, < 0 in the gap after it
static u32 g_padHeld = 0;

// The aim key (right mouse: R) going down with a gun presses the game's Free Aim (d-pad down) too,
// so aiming is free aim from the start, as on a PC, instead of lock-on to break out of. VCS's
// autoFreeAim. kFreeAimDelayTicks after the edge, held kFreeAimPulseTicks.
static constexpr int kFreeAimDelayTicks = 4;
static constexpr int kFreeAimPulseTicks = 6;
static bool g_aimKeyWas = false;
static int g_freeAimPulse = 0;

// Wheel notches made in the scope, for its zoom at the next frame boundary (LCS::ScopeLook).
static int g_scopeZoomNotches = 0;
// The left button is held on the game's map page: the mouse drags it (LCS::DragMap).
static bool g_mapDragging = false;

const char *InputContextName(InputContext context) {
	switch (context) {
	case InputContext::OnFoot: return "on foot";
	case InputContext::InVehicle: return "in vehicle";
	case InputContext::Menu: return "menu";
	default: return "none";
	}
}

static bool ContextClaims(InputContext context, InputKeyCode key) {
	for (const KeyMapping &mapping : kKeyMappings) {
		if (mapping.context == context && mapping.key == key) {
			return true;
		}
	}
	for (const StickKey &stick : kStickKeys) {
		if (stick.context == context && stick.key == key) {
			return true;
		}
	}
	return false;
}

// A key pressed in one context still acts after a change between on foot and a vehicle (W held
// while getting in drives off), but never across the menu: Enter opens it as Start and must not
// then select the first entry as Cross.
// What a key pressed in `context` stands for, under g_mutex. The shipped key whose action was bound
// to it; or, if an action of this context moved off it, nothing (it stays claimed and presses nothing:
// the VCS fork's rule, so the old key doesn't fall through to PPSSPP's own mapping of it - Space is
// the PSP's Start there); or itself.
static bool TranslateKeyLocked(InputContext context, InputKeyCode physical, InputKeyCode *as) {
	for (const auto &[from, to] : g_bindings) {
		if (from.first == (int)context && to == physical) {
			*as = (InputKeyCode)from.second;
			return true;
		}
	}
	if (g_bindings.count({ (int)context, (int)physical })) {
		*as = NKCODE_UNKNOWN;
		return false;
	}
	*as = physical;
	return true;
}

// The scoped weapons' camera is the mouse's whenever the mouse looks at all (LCSFreeAim's ScopeLook).
static bool MouseAimsScope(InputContext context) {
	return context == kFoot && Settings().mouseLook && ScopeActive();
}

static bool KeyLive(const HeldKey &held, InputContext now) {
	if (!held.claimed || held.silent) {
		return false;
	}
	return held.context == now || (held.context != kMenu && now != kMenu);
}

bool HandleHostKey(const KeyInput &key) {
	if (key.deviceId != DEVICE_ID_KEYBOARD && key.deviceId != DEVICE_ID_MOUSE) {
		return false;
	}
	// CHAR events carry a codepoint, not a key code, and have neither DOWN nor UP.
	if (key.flags & KeyInputFlags::CHAR) {
		return false;
	}
	const bool down = (key.flags & KeyInputFlags::DOWN) != 0;
	const bool up = (key.flags & KeyInputFlags::UP) != 0;
	if (!down && !up) {
		return false;
	}
	// Unknown whenever the layer is inactive or switched off, so this is the gate for every
	// other game too.
	const InputContext context = g_context.load(std::memory_order_relaxed);

	std::lock_guard<std::mutex> guard(g_mutex);
	// The intro: any key but Escape (PPSSPP's pause) and the F keys skips it, Shift included, as in
	// the VCS fork. Kept from everything else, and its release with it.
	if (down && IntroPlaying() && key.keyCode != NKCODE_ESCAPE && key.keyCode != NKCODE_BACK &&
			!(key.keyCode >= NKCODE_F1 && key.keyCode <= NKCODE_F12) &&
			key.keyCode != NKCODE_EXT_MOUSEWHEEL_UP && key.keyCode != NKCODE_EXT_MOUSEWHEEL_DOWN) {
		if (!g_held.count(key.keyCode)) {
			g_held[key.keyCode] = HeldKey{ InputContext::Unknown, true, true, NKCODE_UNKNOWN };
			RequestIntroSkip();
		}
		return true;
	}
	if (key.keyCode == NKCODE_EXT_MOUSEWHEEL_UP || key.keyCode == NKCODE_EXT_MOUSEWHEEL_DOWN) {
		if (context != kFoot && context != kCar && context != kMenu) {
			return false;
		}
		if (down) {
			(key.keyCode == NKCODE_EXT_MOUSEWHEEL_UP ? g_wheelUp : g_wheelDown)++;
		}
		return true;
	}

	auto it = g_held.find(key.keyCode);
	if (down) {
		// Auto-repeat sends more downs; the first one decided.
		if (it != g_held.end()) {
			return it->second.claimed;
		}
		// Not recorded at all with no context (every other game): a key pressed now and still
		// held later stays PPSSPP's, and its release finds nothing here and goes there too.
		if (context == InputContext::Unknown) {
			return false;
		}
		InputKeyCode as;
		if (!TranslateKeyLocked(context, key.keyCode, &as)) {
			g_held[key.keyCode] = HeldKey{ context, true, true, NKCODE_UNKNOWN };
			return true;
		}
		// Back (Backspace as shipped) on a page this fork opened is Back to this fork's menu, the way
		// Escape is. As the game's Circle it would lift the focus to the tab strip, hidden on purpose.
		if (context == kMenu && as == NKCODE_DEL && BridgeOwnsMenu()) {
			g_held[key.keyCode] = HeldKey{ context, true, true, as };
			System_PostUIMessage(UIMessage::REQUEST_GAME_PAUSE);
			return true;
		}
		const bool claimed = ContextClaims(context, as);
		g_held[key.keyCode] = HeldKey{ context, claimed, false, as };
		return claimed;
	}
	// A release goes wherever its press went, so neither side is left with a key down forever.
	const bool claimed = it != g_held.end() && it->second.claimed;
	if (it != g_held.end()) {
		g_held.erase(it);
	}
	return claimed;
}

bool HandleMouseDelta(float dx, float dy) {
	if (!g_mouseDriving.load(std::memory_order_relaxed)) {
		return false;
	}
	std::lock_guard<std::mutex> guard(g_mutex);
	g_mouseDX += dx;
	g_mouseDY += dy;
	return true;
}

// This tick's share of the pad panel: its held buttons and the tap being played, if any.
static u32 PadPanelStep() {
	std::lock_guard<std::mutex> guard(g_mutex);
	if (g_padTicks == 0 && !g_padQueue.empty()) {
		g_padTap = g_padQueue.front();
		g_padQueue.pop_front();
		g_padTicks = kPadTapTicks;
	}
	u32 mask = g_padHeld;
	if (g_padTicks > 0) {
		mask |= g_padTap;
		if (--g_padTicks == 0) {
			g_padTicks = -kPadGapTicks;
		}
	} else if (g_padTicks < 0) {
		g_padTicks++;
	}
	return mask;
}

static void ApplyButtons(InputContext context, const std::vector<std::pair<InputKeyCode, HeldKey>> &held, int wheel) {
	u32 mask = 0;
	// While the front-end bridge walks the game's menu, the player's keys stand down: a held W is
	// the d-pad's up, and a press between two of the bridge's is one it didn't make.
	const bool bridge = FrontEndDriving();
	if (context != InputContext::Unknown && !bridge) {
		for (const auto &[key, h] : held) {
			if (!KeyLive(h, context)) {
				continue;
			}
			for (const KeyMapping &mapping : kKeyMappings) {
				if (mapping.context == context && mapping.key == h.as) {
					mask |= mapping.psp;
				}
			}
		}
	}

	// Right mouse on foot, the aim binding; the middle button is the same R for the game's lock-on and
	// starts no free aim, and neither does C (L + R, look behind).
	bool aimKey = false;
	if (context == kFoot && (mask & CTRL_LTRIGGER) == 0) {
		for (const auto &[key, h] : held) {
			if (h.as == NKCODE_EXT_MOUSEBUTTON_2 && KeyLive(h, context)) {
				aimKey = true;
			}
		}
	}
	// Not with a sniper rifle: R alone takes it to its scope.
	if (aimKey && !g_aimKeyWas && Settings().freeAim && PlayerHasFirearm() && !PlayerHasScopedWeapon()) {
		g_freeAimPulse = kFreeAimDelayTicks + kFreeAimPulseTicks;
	} else if (!aimKey) {
		g_freeAimPulse = 0;
	}
	g_aimKeyWas = aimKey;
	if (g_freeAimPulse > 0) {
		if (g_freeAimPulse <= kFreeAimPulseTicks) {
			mask |= CTRL_DOWN;
		}
		g_freeAimPulse--;
	}

	// On STATS and BRIEF the wheel scrolls (they move only while a direction is held).
	if (context == kMenu && !bridge && wheel != 0 && ScrollGamePage(wheel)) {
		wheel = 0;
	}
	// In the scope the wheel zooms, at the frame boundary, and changes no weapon.
	if (!bridge && MouseAimsScope(context)) {
		g_scopeZoomNotches += wheel;
		g_pendingWheel = 0;
	} else if ((context == kFoot || context == kCar || context == kMenu) && !bridge) {
		g_pendingWheel += wheel;
	} else {
		g_pendingWheel = 0;
	}
	if (g_pulseTicks > 0) {
		mask |= g_pulseButton;
		if (--g_pulseTicks == 0) {
			g_pulseGap = kPulseGapTicks;
		}
	} else if (g_pulseGap > 0) {
		g_pulseGap--;
	} else if (g_pendingWheel != 0) {
		// In the game's menu a notch zooms the map (R in, L out); in play it is the d-pad's left
		// and right, weapons on foot and stations in a vehicle.
		if (context == kMenu) {
			g_pulseButton = g_pendingWheel > 0 ? CTRL_RTRIGGER : CTRL_LTRIGGER;
		} else {
			g_pulseButton = g_pendingWheel > 0 ? CTRL_RIGHT : CTRL_LEFT;
		}
		g_pendingWheel += g_pendingWheel > 0 ? -1 : 1;
		g_pulseTicks = kPulseHoldTicks;
		mask |= g_pulseButton;
	}
	mask |= PadPanelStep();
	mask |= FrontEndButtonMask();

	// Release only what this layer pressed last tick. A real pad or PPSSPP's mapper may hold the
	// same button, and clearing it would cancel their press.
	const u32 clear = g_lastMask & ~mask;
	if (mask != 0 || clear != 0) {
		__CtrlUpdateButtons(mask, clear);
	}
	g_lastMask = mask;
}

static void ApplyStick(InputContext context, const std::vector<std::pair<InputKeyCode, HeldKey>> &held) {
	float x = 0.0f;
	float y = 0.0f;
	// In the game's free aim the stick is the aim: WASD there turned the view against the mouse
	// (0.04 rad a second each way) and moved nobody. So it stays centred and the player stands to
	// aim, as on the PSP. The scope the same: D held a second turned it 0.8 rad.
	const bool freeAim = context == kFoot && ((Settings().freeAim && FreeAimActive()) || MouseAimsScope(context));
	const bool bridge = FrontEndDriving();
	float glanceX = 0.0f;
	float glanceY = 0.0f;
	int glances = 0;
	bool walk = false;
	for (const auto &[key, h] : held) {
		if (freeAim || bridge) {
			break;
		}
		if (!KeyLive(h, context)) {
			continue;
		}
		for (const StickKey &stick : kStickKeys) {
			if (stick.context == context && stick.key == h.as) {
				x += stick.x;
				y += stick.y;
			}
		}
		if (context == kCar) {
			for (const GlanceKey &look : kGlanceKeys) {
				if (look.key == h.as) {
					glanceX += look.x;
					glanceY += look.y;
					glances++;
				}
			}
		}
		if (context == kFoot && (h.as == NKCODE_ALT_LEFT || h.as == NKCODE_ALT_RIGHT)) {
			walk = true;
		}
	}
	if (glances > 0) {
		// Left and right together is behind, like C.
		if (glanceX == 0.0f && glanceY == 0.0f) {
			glanceY = -1.0f;
		}
		x = glanceX;
		y = glanceY;
	}
	if (x != 0.0f || y != 0.0f) {
		// A diagonal is the same speed as a straight line, as on the nub.
		const float length = std::sqrt(x * x + y * y);
		if (length > 1.0f) {
			x /= length;
			y /= length;
		}
		if (walk) {
			x *= kWalkDeflection;
			y *= kWalkDeflection;
		}
		__CtrlSetAnalogXY(CTRL_STICK_LEFT, x, y);
		g_stickHeld = true;
	} else if (g_stickHeld) {
		// Released once; a real stick sends its own events and is left alone after that.
		__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
		g_stickHeld = false;
	}
}

// The on-foot camera, stock or taken over by the mouse. Taken over means: Process_FollowPed's pitch
// spring off, its yaw correction out, and its obstacle nudge zeroed, so the angles the mouse
// writes are the angles the camera keeps. Asserted every tick while taken over, because a
// savestate brings the game's values back with everything else.
static void SetCameraTakenOver(bool takenOver) {
	for (const LCSDataFloat &word : kLCSFollowPedPitchSpring) {
		WriteFloat(word.address, takenOver ? 1.0f : word.stock);
	}
	SetCodeWord(kLCSFollowPedYawCorrection, takenOver);
	if (takenOver) {
		WriteFloat(kLCSFollowPedYawNudge, 0.0f);
	}
	g_cameraTakenOver = takenOver;
}

static float WrapAngle(float a) {
	const float twoPi = 6.28318530718f;
	a = std::fmod(a, twoPi);
	return a < 0.0f ? a + twoPi : a;
}

// The mouse turns the on-foot camera (Cams[ActiveCam] in Process_FollowPed). Applied as a change to
// whatever the camera holds now, so a script turning the camera still turns it. With the chase
// camera on foot, the movement goes to it instead, and it writes the angles from inside the frame;
// the takeover stays, so what it writes holds.
static void ApplyMouseLook(InputContext context, float dx, float dy, bool chaseCam) {
	const LCSSettings &settings = Settings();
	const std::optional<u8> active = ReadU8(kLCSTheCamera + kLCSCamActiveOffset);
	const u32 cam = kLCSTheCamera + kLCSCamsOffset + active.value_or(0) * kLCSCamStride;
	const std::optional<u16> mode = active && *active <= 2 ? ReadU16(cam + kLCSCamModeOffset) : std::nullopt;
	const bool onFoot = context == kFoot && mode && *mode == kLCSCamModeFollowPed;

	const bool takeOver = settings.mouseLook && onFoot;
	if (takeOver && !g_cameraTakenOver) {
		// Whatever yaw rate the correction built up would otherwise carry on for a few frames.
		WriteFloat(cam + kLCSCamBetaSpeedOffset, 0.0f);
	}
	if (takeOver || g_cameraTakenOver) {
		SetCameraTakenOver(takeOver);
	}
	if (!takeOver || (dx == 0.0f && dy == 0.0f)) {
		return;
	}

	const float sens = settings.mouseSensitivity;
	const float ySign = settings.invertMouseY ? -1.0f : 1.0f;
	if (chaseCam) {
		// The chase camera's convention: yaw counter-clockwise from above, pitch up.
		ChaseCamAddLook(-dx * sens, -dy * sens * ySign);
		return;
	}
	const std::optional<float> beta = ReadFloat(cam + kLCSCamBetaOffset);
	const std::optional<float> alpha = ReadFloat(cam + kLCSCamAlphaOffset);
	if (beta) {
		// The stick to the right lowers Beta (Process_FollowPed negates LookAroundLeftRight).
		WriteFloat(cam + kLCSCamBetaOffset, WrapAngle(*beta - dx * sens));
	}
	if (alpha) {
		// Positive Alpha looks up; the mouse away from you (negative dy) looks up.
		const float pitch = *alpha - dy * sens * ySign;
		WriteFloat(cam + kLCSCamAlphaOffset, std::max(kLCSFollowPedPitchMin, std::min(kLCSFollowPedPitchMax, pitch)));
	}
}

void InputTick(const LCSState &state) {
	const LCSSettings &settings = Settings();
	InputContext context = InputContext::Unknown;
	if (settings.keyboardMouse && state.playerBase) {
		if (ReadAddrBool(LCSAddr::MenuActive).value_or(false)) {
			context = kMenu;
		} else if (state.inVehicle.value_or(false)) {
			context = kCar;
		} else {
			context = kFoot;
		}
	}
	g_context.store(context, std::memory_order_relaxed);
	// In a vehicle only the chase camera can be turned: the game's own swings back (chase-camera.md).
	const bool mouseCamera = context == kFoot || (context == kCar && ChaseCamTakesLook(kCar));

	std::vector<std::pair<InputKeyCode, HeldKey>> held;
	int wheel;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		held.assign(g_held.begin(), g_held.end());
		wheel = g_wheelUp - g_wheelDown;
		g_wheelUp = 0;
		g_wheelDown = 0;
	}
	// The map page takes the mouse while the drag button is held.
	g_mapDragging = false;
	if (context == kMenu && MapPageUp()) {
		for (const auto &[key, h] : held) {
			if (h.as == NKCODE_EXT_MOUSEBUTTON_1 && KeyLive(h, context)) {
				g_mapDragging = true;
			}
		}
	}
	g_mouseDriving.store(settings.keyboardMouse && ((settings.mouseLook && mouseCamera) || g_mapDragging),
		std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		// Mouse movement that arrived while it couldn't turn anything is dropped, not saved up
		// for the next time it can.
		if (!mouseCamera && !g_mapDragging) {
			g_mouseDX = 0.0f;
			g_mouseDY = 0.0f;
		}
	}

	ApplyButtons(context, held, wheel);
	ApplyStick(context, held);
	// Leaving the on-foot camera hands it back at once, without waiting for a frame wait.
	if (context != kFoot && g_cameraTakenOver) {
		SetCameraTakenOver(false);
	}
}

void InputFrameBoundary() {
	float dx, dy;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		dx = g_mouseDX;
		dy = g_mouseDY;
		g_mouseDX = 0.0f;
		g_mouseDY = 0.0f;
	}
	const InputContext context = g_context.load(std::memory_order_relaxed);
	if (context == kMenu) {
		if (g_mapDragging && (dx != 0.0f || dy != 0.0f)) {
			DragMap(dx, dy);
		}
		return;
	}
	// Free aim first: it is on foot, in mode 11, which no other look path takes.
	const LCSSettings &s = Settings();
	if (context == kFoot && s.freeAim && s.keyboardMouse && s.mouseLook && FreeAimLook(dx, dy)) {
		return;
	}
	// Then the scope, the other aim on foot (modes 7, 8, 34, 45, 46), with the wheel's zoom.
	const int zoomNotches = g_scopeZoomNotches;
	g_scopeZoomNotches = 0;
	if (context == kFoot && s.keyboardMouse && s.mouseLook && ScopeLook(dx, dy, zoomNotches)) {
		return;
	}
	if (context == kCar && ChaseCamTakesLook(kCar)) {
		// The chase camera's convention: yaw counter-clockwise from above, pitch up. The mouse to
		// the right turns the view right; away from you looks up.
		const LCSSettings &settings = Settings();
		const float ySign = settings.invertMouseY ? -1.0f : 1.0f;
		ChaseCamAddLook(-dx * settings.mouseSensitivity, -dy * settings.mouseSensitivity * ySign);
		return;
	}
	ApplyMouseLook(context, dx, dy, ChaseCamTakesLook(context));
}

void ResetInput() {
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		g_held.clear();
		g_mouseDX = 0.0f;
		g_mouseDY = 0.0f;
		g_wheelUp = 0;
		g_wheelDown = 0;
		g_padQueue.clear();
		g_padTicks = 0;
		g_padHeld = 0;
	}
	g_context.store(InputContext::Unknown, std::memory_order_relaxed);
	g_mouseDriving.store(false, std::memory_order_relaxed);
	if (g_lastMask != 0) {
		__CtrlUpdateButtons(0, g_lastMask);
		g_lastMask = 0;
	}
	if (g_stickHeld) {
		__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
		g_stickHeld = false;
	}
	if (g_cameraTakenOver) {
		SetCameraTakenOver(false);
	}
	g_pendingWheel = 0;
	g_pulseTicks = 0;
	g_pulseGap = 0;
	g_aimKeyWas = false;
	g_freeAimPulse = 0;
	g_scopeZoomNotches = 0;
	g_mapDragging = false;
}

InputContext CurrentInputContext() {
	return g_context.load(std::memory_order_relaxed);
}

bool KeyboardLayerOn() {
	return IsActive() && Settings().keyboardMouse;
}

u32 AppliedButtons() {
	return g_lastMask;
}

struct PadButtonName {
	const char *name;
	u32 mask;
};
static const PadButtonName kPadNames[] = {
	{ "Up", CTRL_UP }, { "Down", CTRL_DOWN }, { "Left", CTRL_LEFT }, { "Right", CTRL_RIGHT },
	{ "Triangle", CTRL_TRIANGLE }, { "Circle", CTRL_CIRCLE }, { "Cross", CTRL_CROSS }, { "X", CTRL_CROSS },
	{ "Square", CTRL_SQUARE }, { "L", CTRL_LTRIGGER }, { "R", CTRL_RTRIGGER },
	{ "Start", CTRL_START }, { "Select", CTRL_SELECT },
};

// "Up Square, Square Down" -> masks; a "+" joins buttons pressed together ("L+R"). False with the
// first word it doesn't know in *bad.
bool ParsePadSequence(const std::string &text, std::vector<u32> *out, std::string *bad) {
	out->clear();
	std::string word;
	auto flush = [&]() {
		if (word.empty()) {
			return true;
		}
		u32 mask = 0;
		size_t start = 0;
		while (start <= word.size()) {
			const size_t plus = word.find('+', start);
			const std::string part = word.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
			u32 one = 0;
			for (const PadButtonName &n : kPadNames) {
				if (equalsNoCase(part, n.name)) {
					one = n.mask;
				}
			}
			if (!one) {
				*bad = part;
				return false;
			}
			mask |= one;
			if (plus == std::string::npos) {
				break;
			}
			start = plus + 1;
		}
		out->push_back(mask);
		word.clear();
		return true;
	};
	for (char c : text) {
		if (c == ' ' || c == ',' || c == '\t') {
			if (!flush()) {
				return false;
			}
		} else {
			word += c;
		}
	}
	return flush();
}

void QueuePadTaps(const std::vector<u32> &masks) {
	std::lock_guard<std::mutex> guard(g_mutex);
	for (u32 mask : masks) {
		if (mask != 0) {
			g_padQueue.push_back(mask);
		}
	}
}

void SetPadHeld(u32 mask, bool held) {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_padHeld = held ? (g_padHeld | mask) : (g_padHeld & ~mask);
}

u32 PadHeld() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_padHeld;
}

int PadTapsPending() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return (int)g_padQueue.size() + (g_padTicks > 0 ? 1 : 0);
}

bool MouseLookEngaged() {
	return g_cameraTakenOver;
}

void DropPendingMouse() {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_mouseDX = 0.0f;
	g_mouseDY = 0.0f;
	g_wheelUp = 0;
	g_wheelDown = 0;
}

// --- The controls card ---

// What the card calls a key: the names a PC game's controls screen uses, PPSSPP's own where those
// would do ("LShift" would not).
std::string CardKeyName(InputKeyCode key) {
	switch (key) {
	case NKCODE_EXT_MOUSEBUTTON_1: return "LMB";
	case NKCODE_EXT_MOUSEBUTTON_2: return "RMB";
	case NKCODE_EXT_MOUSEBUTTON_3: return "MMB";
	case NKCODE_EXT_MOUSEWHEEL_UP: return "WHEEL UP";
	case NKCODE_EXT_MOUSEWHEEL_DOWN: return "WHEEL DOWN";
	case NKCODE_SHIFT_LEFT: return "LEFT SHIFT";
	case NKCODE_CTRL_LEFT: return "LEFT CTRL";
	case NKCODE_DEL: return "BACKSPACE";
	case NKCODE_DPAD_UP: return "UP ARROW";
	case NKCODE_DPAD_DOWN: return "DOWN ARROW";
	default: {
		std::string name = KeyMap::GetKeyName(key);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)toupper(c); });
		return name;
	}
	}
}

static std::string CardPspName(u32 mask) {
	static const struct { u32 bit; const char *name; } kNames[] = {
		{ CTRL_UP, "UP" }, { CTRL_DOWN, "DOWN" }, { CTRL_LEFT, "LEFT" }, { CTRL_RIGHT, "RIGHT" },
		{ CTRL_CROSS, "CROSS" }, { CTRL_CIRCLE, "CIRCLE" }, { CTRL_SQUARE, "SQUARE" },
		{ CTRL_TRIANGLE, "TRIANGLE" }, { CTRL_LTRIGGER, "L" }, { CTRL_RTRIGGER, "R" },
		{ CTRL_START, "START" }, { CTRL_SELECT, "SELECT" },
	};
	std::string out;
	for (const auto &n : kNames) {
		if (mask & n.bit) {
			out += out.empty() ? n.name : std::string(" + ") + n.name;
		}
	}
	return out;
}

static InputKeyCode BoundKeyLocked(InputContext context, InputKeyCode shipped) {
	auto it = g_bindings.find({ (int)context, (int)shipped });
	return it != g_bindings.end() ? it->second : shipped;
}

InputKeyCode BoundKey(InputContext context, InputKeyCode shipped) {
	std::lock_guard<std::mutex> guard(g_mutex);
	return BoundKeyLocked(context, shipped);
}

// Binds `key` to the action whose shipped key is `shipped`. A key another action of the context has
// is swapped rather than taken: that action gets this one's old key, because an action left with no
// key is something the game can no longer do.
void BindKey(InputContext context, InputKeyCode shipped, InputKeyCode key) {
	std::lock_guard<std::mutex> guard(g_mutex);
	const InputKeyCode old = BoundKeyLocked(context, shipped);
	if (old == key) {
		return;
	}
	// Who has `key` now: an action bound to it, or the action shipped on it (if not moved off).
	InputKeyCode other = NKCODE_UNKNOWN;
	for (const auto &[from, to] : g_bindings) {
		if (from.first == (int)context && to == key) {
			other = (InputKeyCode)from.second;
		}
	}
	if (other == NKCODE_UNKNOWN && ContextClaims(context, key) && !g_bindings.count({ (int)context, (int)key })) {
		other = key;
	}
	auto set = [&](InputKeyCode action, InputKeyCode to) {
		if (action == to) {
			g_bindings.erase({ (int)context, (int)action });
		} else {
			g_bindings[{ (int)context, (int)action }] = to;
		}
	};
	set(shipped, key);
	if (other != NKCODE_UNKNOWN && other != shipped) {
		set(other, old);
	}
	NOTICE_LOG(Log::System, "LCS input: %s: %s -> %s%s", InputContextName(context), CardKeyName(shipped).c_str(),
		CardKeyName(key).c_str(), other != NKCODE_UNKNOWN && other != shipped ? " (swapped)" : "");
}

void ResetKeyBindings() {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_bindings.clear();
}

bool HasKeyBindings() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return !g_bindings.empty();
}

// lcs.ini [KeyBindings]: "<context>.<shipped key code> = <key code>", only for what moved.
static const char *const kContextIniNames[] = { "None", "OnFoot", "InVehicle", "Menu" };

void LoadKeyBindings(const Section *section) {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_bindings.clear();
	if (!section) {
		return;
	}
	for (int c = 1; c < (int)ARRAY_SIZE(kContextIniNames); c++) {
		for (const KeyMapping &m : kKeyMappings) {
			char name[48];
			snprintf(name, sizeof(name), "%s.%d", kContextIniNames[c], (int)m.key);
			int to = 0;
			if ((int)m.context == c && section->Get(name, &to) && to > 0 && to != NKCODE_ESCAPE) {
				g_bindings[{ c, (int)m.key }] = (InputKeyCode)to;
			}
		}
		for (const StickKey &st : kStickKeys) {
			char name[48];
			snprintf(name, sizeof(name), "%s.%d", kContextIniNames[c], (int)st.key);
			int to = 0;
			if ((int)st.context == c && section->Get(name, &to) && to > 0 && to != NKCODE_ESCAPE) {
				g_bindings[{ c, (int)st.key }] = (InputKeyCode)to;
			}
		}
	}
}

void SaveKeyBindings(Section *section) {
	std::lock_guard<std::mutex> guard(g_mutex);
	section->Clear();
	for (const auto &[from, to] : g_bindings) {
		char name[48];
		snprintf(name, sizeof(name), "%s.%d", kContextIniNames[from.first], from.second);
		section->Set(name, (int)to);
	}
}

std::vector<KeyListingRow> KeyListing(InputContext context) {
	std::vector<KeyListingRow> rows;
	std::lock_guard<std::mutex> guard(g_mutex);
	auto part = [&](KeyListingRow &row, InputKeyCode shipped, const char *what) {
		const InputKeyCode bound = BoundKeyLocked(context, shipped);
		row.parts.push_back({ shipped, bound, what });
		row.keys.push_back(CardKeyName(bound));
	};
	// The stick and the camera first, as a controls screen lists them; they are not in the table.
	if (context == kFoot) {
		KeyListingRow walk{ "Walk", {}, "STICK" };
		part(walk, NKCODE_W, "forward");
		part(walk, NKCODE_S, "back");
		part(walk, NKCODE_A, "left");
		part(walk, NKCODE_D, "right");
		rows.push_back(walk);
		rows.push_back({ "Look", { "MOUSE" }, "" });
	} else if (context == kCar) {
		KeyListingRow steer{ "Steer", {}, "STICK" };
		part(steer, NKCODE_A, "left");
		part(steer, NKCODE_D, "right");
		rows.push_back(steer);
		KeyListingRow lean{ "Lean / aim the water cannon", {}, "STICK" };
		part(lean, NKCODE_DPAD_UP, "forward");
		part(lean, NKCODE_DPAD_DOWN, "back");
		rows.push_back(lean);
		rows.push_back({ "Look around", { "MOUSE" }, "" });
	}
	for (size_t i = 0; i < kKeyMappingCount; i++) {
		const KeyMapping &m = kKeyMappings[i];
		if (m.context != context || m.psp == 0) {
			continue;
		}
		auto it = std::find_if(rows.begin(), rows.end(), [&](const KeyListingRow &row) {
			return row.action == m.description;
		});
		if (it == rows.end()) {
			rows.push_back({ m.description, {}, CardPspName(m.psp) });
			it = rows.end() - 1;
		} else if (it->psp.find(CardPspName(m.psp)) == std::string::npos) {
			it->psp += " / " + CardPspName(m.psp);
		}
		// The menu's Move row is four keys, one per direction: say which is being asked for.
		const char *what = nullptr;
		if (context == kMenu && m.psp == CTRL_UP) what = "up";
		if (context == kMenu && m.psp == CTRL_DOWN) what = "down";
		if (context == kMenu && m.psp == CTRL_LEFT) what = "left";
		if (context == kMenu && m.psp == CTRL_RIGHT) what = "right";
		part(*it, m.key, what);
	}
	// The wheel is one notch per press of the d-pad's left or right, which is the row above it.
	if (context == kFoot || context == kCar) {
		for (KeyListingRow &row : rows) {
			if (row.psp == "RIGHT") {
				row.keys.push_back(CardKeyName(NKCODE_EXT_MOUSEWHEEL_UP));
			} else if (row.psp == "LEFT") {
				row.keys.push_back(CardKeyName(NKCODE_EXT_MOUSEWHEEL_DOWN));
			}
		}
	}
	return rows;
}

}  // namespace LCS
