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

#include <string>
#include <vector>

// The LCS layer's own settings, in PSP/SYSTEM/lcs.ini, section [Settings]. Read when the layer
// activates, written when one is changed. Hand-editable; a missing file means defaults.
//
// Read and written on the emu thread (LCS::Tick and the ImGui debugger), and on the UI thread by
// the front end (UI/LCSMenuScreen), which only runs while the emulator is paused.

namespace LCS {

struct LCSSettings {
	// 60 fps: the frame cap plus the millisecond carry (LCSFrameRate).
	bool frameRate60 = false;

	// Keyboard and mouse by context (LCSInput). Off means every key goes to PPSSPP's own mapper.
	bool keyboardMouse = true;
	// The mouse turns the camera (needs PPSSPP's "Use mouse control", which also traps the cursor).
	bool mouseLook = true;
	// Radians per mouse count.
	float mouseSensitivity = 0.0025f;
	bool invertMouseY = false;
	// In a vehicle the mouse orbits a chase camera of the fork's own (LCSChaseCam). Off leaves the
	// game's vehicle camera, which the mouse can't turn. Needs keyboardMouse and mouseLook.
	bool vehicleChaseCamera = true;
	// The same on foot: the mouse orbits a camera that comes in along its line when something is
	// in the way. Off goes back to writing the game's own angles. Needs keyboardMouse and mouseLook.
	bool footChaseCamera = true;
	// Right mouse with a gun drops straight into the game's free aim, the mouse aims and the shot
	// goes where the reticle is (LCSFreeAim). Off leaves R as the game's lock-on.
	bool freeAim = true;
	// A PSP-2000 memory partition, which the game answers with a 12MB streaming reserve instead of
	// 5MB. Read at boot (WantsExtraWorldMemory), so a change takes a restart; and savestates made
	// with it one way don't load the other.
	bool worldMemory = false;
	// SMAA over the world, before the HUD (GPU/Common/LCSSmaa; Vulkan only).
	bool antiAliasing = true;
	// PPSSPP's "Save new textures", turned on at every boot: each texture drawn for the first time
	// goes to TEXTURES/ULUS10041/new, which the HD pack is bound from (Tools/lcshd.py), so every
	// session adds to it. Off leaves PPSSPP's own checkbox in charge.
	bool dumpTextures = true;

	// Free aim's own channel (LCSFreeAim): radians per mouse count, and its vertical sense. A file
	// from before these existed takes the look settings' values, so nothing changes for it.
	float aimSensitivity = 0.0025f;
	bool invertAimY = false;
	// The scoped weapons' (sniper, rocket launcher): radians per count at the widest view, scaled by
	// the zoom like the aim's. Vertical sense is the aim's. A file from before it takes the aim's.
	float scopedSensitivity = 0.0025f;

	// Save after every story mission, into the game's last save slot (LCSFrontEnd).
	bool autoSave = true;
	// A line on the radar along the roads to the map's marker or a mission's destination, while
	// driving (LCSRoute).
	bool gpsRoute = true;
	// The text language, the game's order: English, French, German, Italian, Spanish. Handed to the
	// game as the PSP's system language when it asks at boot, so it takes a restart.
	int language = 0;

	// The game's own settings, from the Display, Audio and Controls pages of its front end, which
	// this fork's menu stands in for. Mirrored into the game by LCSFrontEnd whenever it disagrees,
	// which also puts them back after a save loads its own. The defaults are the game's (its RESET
	// GAME SETTINGS, 0x08adcad4 case 0x2a).
	int gameBrightness = 288;     // 128..384 in 32s
	bool gameSubtitles = false;   // the game's default in English
	bool gameHud = true;
	int gameRadar = 0;            // map and blips, blips only, off
	int sfxVolume = 96;           // 0..127
	int musicVolume = 80;         // the radio
	bool gameInvertLook = false;  // the stick's look up and down (the game's INVERT LOOK)
	float padLookSensitivity = 0.007f;  // squared in the game's look response

	// A pad laid out like the PC game's (LCSInput's kPadMappings): triggers aim and fire, the right
	// stick turns the camera. Off gives a pad the PSP's own layout through PPSSPP's mapper.
	bool padScheme = true;
	// The right stick's look, in mouse counts a vblank at full deflection: it goes into the mouse's
	// accumulator, so the sensitivity, the FOV scale and the free aim behind it are the mouse's.
	float padLookSpeed = 14.0f;
	bool padInvertLookY = false;

	// The save slot (the S<n> of ULUS10041S<n>) the auto-save last wrote, so the save list can say
	// which it is; -1 before the first. Written by LCSFrontEnd.
	int autoSaveSlot = -1;
};

LCSSettings &Settings();
// The fork turns the camera (the chase camera, free aim, the scope) from the mouse or a pad's right
// stick: mouse look on, and keyboard and mouse or the pad scheme with it.
bool CameraLookOwned();
void LoadSettings();
void SaveSettings();
// lcs.ini's WorldMemory, read straight from the file: the partition is sized while the ISO loads,
// before the layer activates or loads its settings.
bool WantsExtraWorldMemory();
// Turns PPSSPP's "Save new textures" on if dumpTextures is set. Never turns it off.
void ApplyTextureDump();
// The PSP system language the game is told at boot (sceUtilityGetSystemParamInt), from `language`.
int SystemLanguageForGame();
// Rows that stand in for a part of a PPSSPP setting (the FPS counter's bit) read it again. The
// front end calls this before building a page.
void RefreshOptionProxies();

// --- The front end's option table ---
//
// The VCS fork's port of re3's CFO ("Custom Frontend Options"): a menu row is the variable it
// edits plus the ini key it saves to, so adding an option is one row in one table - no renderer
// change, no switch arm, no new save code. LoadSettings and SaveSettings walk the same table.
//
// The table owns names, ranges and persistence; the values stay where they live (LCSSettings,
// the chase camera's tuning, g_Config). Rows a player has no opinion about stay in the debugger.

enum class OptionType {
	Bool,
	Float,
	Int,     // a number within a range, moved a step at a time
	Choice,  // an int index into a list of labels
};

// Which page an option appears on. A page is built by filtering the table, so the order of the
// rows is the order on screen.
enum class OptionPage {
	Mouse,
	Controller,
	Aiming,
	Audio,
	Graphics,
	GraphicsAdvanced,  // the graphics rows a player rarely touches, one page down
	Gameplay,
	Developer,  // in no menu: saved in lcs.ini, edited in Debug -> LCS
};

struct Option {
	OptionPage page;
	OptionType type;

	// Stable id, and the key under [Settings] in lcs.ini. Renaming one silently resets that
	// setting for everyone who has a file. Null for external and placeholder rows.
	const char *iniKey;

	const char *label;  // the row itself
	const char *help;   // one line, shown along the bottom while the row is selected

	// Exactly one of these is set, according to type.
	bool *boolValue;
	float *floatValue;
	int *intValue;

	// Choice only: the labels, one per index.
	const char *const *choices;
	int numChoices;

	// A setting PPSSPP owns (g_Config, saved by PPSSPP): not loaded or saved here.
	bool external;

	// A row for something this fork doesn't do yet. It is on its page, drawn greyed, and does
	// nothing; its value is a stand-in that isn't saved. When the feature lands the row gets a real
	// variable and an ini key and stops being one.
	bool placeholder;

	// Called after the value really changed. Null for settings read live.
	void (*onChange)();

	// Only live while this is true: greyed and inert otherwise.
	bool *enabledBy;

	// How the value reads, where a label or format can't say it. Null for the rules in ValueText.
	std::string (*valueText)(const Option &opt);

	// Float only; the same range the debugger window uses.
	float minValue;
	float maxValue;

	// Int only. stepInt is one press, and the granularity the block strip snaps to.
	int minInt;
	int maxInt;
	int stepInt;

	// How the value reads. For a Float, null shows its 0-100 position in the range (and the menu
	// draws blocks); for an Int, null draws blocks instead of the number.
	const char *format;

	// How many blocks the strip draws; 0 is ten. An Int whose range is fewer steps than ten says so,
	// so that one press always lights exactly one more block (brightness has eight).
	int blocks;

	// The compiled-in value, captured when the table is built (before any ini is read), so
	// RESTORE DEFAULTS restores what the code shipped with. External rows pass theirs explicitly.
	bool defaultBool;
	float defaultFloat;
	int defaultInt;
};

// Every option, in menu order. Built on first call; LoadSettings calls it first.
const std::vector<Option> &Options();

// Put one page back to the values the code shipped with.
void ResetPage(OptionPage page);

// Position of a Float or Int option within its range, 0..1, which is what the blocks draw.
float GetNormalized(const Option &opt);
void SetNormalized(const Option &opt, float t);
// The write paths: clamp, then fire onChange only on a real change.
void SetInt(const Option &opt, int value);
void SetBool(const Option &opt, bool value);

// What the right-hand column shows: "ON", "OFF", "63", or a formatted value.
std::string ValueText(const Option &opt);

}  // namespace LCS
