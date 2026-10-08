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
#include <cstdio>

#include "Common/Common.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "Core/LCS/LCSChaseCam.h"
#include "Core/LCS/LCSSettings.h"

namespace LCS {

static LCSSettings g_settings;

static Path SettingsPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "lcs.ini";
}

LCSSettings &Settings() {
	return g_settings;
}

// --- Labels ---

// Mirrors PPSSPP's own settings screen, so the two never disagree about what an index means.
static const char *const kResolutionLabels[] = {
	"Auto", "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x",
};
static const char *const kAnisoLabels[] = { "Off", "2x", "4x", "8x", "16x" };

// The placeholders' ladders, the VCS fork's words for the same rows.
static const char *const kWidescreenLabels[] = { "Off", "Crop", "Wider" };
static const char *const kShadowLabels[] = { "LOW", "MEDIUM", "HIGH", "ULTRA" };
static const char *const kWaterLabels[] = { "LOW", "MEDIUM", "HIGH" };
// The disc's five GXTs, each in its own language and without accents (the menu face has none).
static const char *const kLanguageLabels[] = {
	"ENGLISH", "FRANCAIS", "DEUTSCH", "ESPANOL", "ITALIANO",
};

// "3x (1440x816)": the multiplier is the setting, the pixel count what it does. Auto rounds the
// window up to a whole multiple of 480, the same arithmetic as CalculateRenderResolution.
static std::string ResolutionText(const Option &opt) {
	const int index = std::clamp(*opt.intValue, 0, (int)ARRAY_SIZE(kResolutionLabels) - 1);
	const int scale = index != 0 ? index : std::max((g_display.pixel_xres + 479) / 480, 1);
	char buffer[48];
	snprintf(buffer, sizeof(buffer), "%s (%dx%d)", kResolutionLabels[index], 480 * scale, 272 * scale);
	return std::string(buffer);
}

// --- Placeholder values ---
//
// What the rows for things not built yet edit: in memory only, never saved, so no ini key is
// promised before the feature exists. The values are the ones the VCS fork ships.
static struct {
	bool padScheme = true;
	float padLookSpeed = 14.0f;
	bool padInvertY = false;
	float padDeadzone = 0.15f;
	float aimSensitivity = 0.0012f;
	bool aimInvertY = false;
	float scopedSensitivity = 0.05f;
	int sfxVolume = 100;
	int radioVolume = 100;
	bool customTracks = false;
	int brightness = 50;
	int widescreen = 0;
	bool showFps = false;
	int shadows = 1;
	bool ambientOcclusion = false;
	int water = 1;
	bool autoSave = false;
	bool gpsRoute = false;
	int language = 0;
	bool subtitles = true;
	bool hud = true;
} g_placeholder;

// --- The table ---

const std::vector<Option> &Options() {
	// Function-local, because the rows point into other function-local statics
	// (ChaseCamTuning) and a file-scope table would race their construction.
	static const std::vector<Option> options = [] {
		LCSSettings &s = g_settings;
		LCSChaseCamTuning &chase = ChaseCamTuning();
		std::vector<Option> opts;

		auto addBool = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, bool *value, bool external = false, bool defaultValue = false,
				void (*onChange)() = nullptr, std::string (*valueText)(const Option &) = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Bool;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.boolValue = value;
			opt.external = external;
			opt.onChange = onChange;
			opt.valueText = valueText;
			// For one of ours the compiled-in value is the default; for one PPSSPP owns the live
			// value is whatever the user saved, so it is passed.
			opt.defaultBool = external ? defaultValue : *value;
			opts.push_back(opt);
		};
		auto addFloat = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, float *value, float minValue, float maxValue,
				const char *format = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Float;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.floatValue = value;
			opt.minValue = minValue;
			opt.maxValue = maxValue;
			opt.format = format;
			opt.defaultFloat = *value;
			opts.push_back(opt);
		};
		auto addInt = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, int *value, int minValue, int maxValue, int step,
				int defaultValue, bool external = false) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Int;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.intValue = value;
			opt.minInt = minValue;
			opt.maxInt = maxValue;
			opt.stepInt = step;
			opt.external = external;
			opt.defaultInt = defaultValue;
			opts.push_back(opt);
		};
		auto addChoice = [&opts](OptionPage page, const char *iniKey, const char *label,
				const char *help, int *value, const char *const *choices, int numChoices,
				int defaultIndex, bool external = false, void (*onChange)() = nullptr,
				std::string (*valueText)(const Option &) = nullptr) {
			Option opt{};
			opt.page = page;
			opt.type = OptionType::Choice;
			opt.iniKey = iniKey;
			opt.label = label;
			opt.help = help;
			opt.intValue = value;
			opt.choices = choices;
			opt.numChoices = numChoices;
			opt.external = external;
			opt.onChange = onChange;
			opt.valueText = valueText;
			opt.defaultInt = defaultIndex;
			opts.push_back(opt);
		};
		// Afterthoughts on the row just added, rather than parameters on all four adders.
		auto enabledBy = [&opts](bool *flag) {
			opts.back().enabledBy = flag;
		};
		auto placeholder = [&opts]() {
			opts.back().placeholder = true;
			opts.back().iniKey = nullptr;
		};

		// --- Mouse ---

		addBool(OptionPage::Mouse, "KeyboardMouse", "Keyboard and mouse",
			"The PC layout: keys by situation, mouse look, free aim. Off gives the keyboard "
			"PPSSPP's own PSP mapping.",
			&s.keyboardMouse);
		addBool(OptionPage::Mouse, "MouseLook", "Mouse look",
			"The mouse turns the camera on foot and in vehicles. Needs Use mouse control on, in "
			"PPSSPP's control settings.",
			&s.mouseLook);
		enabledBy(&s.keyboardMouse);
		addFloat(OptionPage::Mouse, "MouseSensitivity", "Look sensitivity",
			"How far the camera turns per unit of mouse movement.",
			&s.mouseSensitivity, 0.0005f, 0.01f);
		enabledBy(&s.mouseLook);
		addBool(OptionPage::Mouse, "InvertMouseY", "Invert look vertically",
			"Push the mouse forward to look down.", &s.invertMouseY);
		enabledBy(&s.mouseLook);
		addBool(OptionPage::Mouse, "FootChaseCamera", "On-foot camera",
			"The mouse orbits a camera that comes in when a wall is in the way. Off turns the "
			"game's own camera instead.",
			&s.footChaseCamera, false, false, nullptr, [](const Option &opt) {
				return std::string(*opt.boolValue ? "CHASE" : "GAME");
			});
		enabledBy(&s.mouseLook);
		addBool(OptionPage::Mouse, "VehicleChaseCamera", "Vehicle camera",
			"The mouse orbits a camera behind the vehicle. Off leaves the game's vehicle camera, "
			"which the mouse cannot turn.",
			&s.vehicleChaseCamera, false, false, nullptr, [](const Option &opt) {
				return std::string(*opt.boolValue ? "CHASE" : "GAME");
			});
		enabledBy(&s.mouseLook);
		addBool(OptionPage::Mouse, "VehicleRecentre", "Vehicle camera follows",
			"The view swings back behind a moving vehicle a moment after you stop moving the mouse.",
			&chase.vehicleRecentre);
		enabledBy(&s.vehicleChaseCamera);

		// --- Controller (to come: the pad's own scheme) ---

		addBool(OptionPage::Controller, nullptr, "Controller scheme",
			"A pad laid out like the PC game's, with the right stick on the camera.",
			&g_placeholder.padScheme);
		placeholder();
		addFloat(OptionPage::Controller, nullptr, "Look sensitivity",
			"How fast the right stick turns the camera.",
			&g_placeholder.padLookSpeed, 4.0f, 40.0f);
		placeholder();
		addBool(OptionPage::Controller, nullptr, "Invert look vertically",
			"Push the right stick forward to look down.", &g_placeholder.padInvertY);
		placeholder();
		addFloat(OptionPage::Controller, nullptr, "Stick deadzone",
			"How far a stick must move before it counts.",
			&g_placeholder.padDeadzone, 0.02f, 0.45f);
		placeholder();

		// --- Aiming ---

		addBool(OptionPage::Aiming, "MouseFreeAim", "Free aim",
			"Right mouse with a gun aims where the mouse points and the shot goes to the reticle. "
			"Off leaves the game's lock-on.",
			&s.freeAim);
		enabledBy(&s.keyboardMouse);
		addFloat(OptionPage::Aiming, nullptr, "Aim sensitivity",
			"Aiming on a channel of its own, slower than looking.",
			&g_placeholder.aimSensitivity, 0.00005f, 0.008f);
		placeholder();
		addBool(OptionPage::Aiming, nullptr, "Invert aim vertically", "",
			&g_placeholder.aimInvertY);
		placeholder();
		addFloat(OptionPage::Aiming, nullptr, "Sniper and RPG sensitivity",
			"Only for the two scoped weapons.",
			&g_placeholder.scopedSensitivity, 0.005f, 0.2f);
		placeholder();

		// --- Audio ---

		addBool(OptionPage::Audio, nullptr, "Sound",
			"Master switch. Off silences everything, whatever the volume below says.",
			&g_Config.bEnableSound, true, true);
		addInt(OptionPage::Audio, nullptr, "Master volume",
			"Overall volume, applied by the emulator.",
			&g_Config.iGameVolume, VOLUME_OFF, VOLUMEHI_FULL, VOLUMEHI_FULL / 10,
			Config::GetDefaultValueInt(&g_Config.iGameVolume), true);
		enabledBy(&g_Config.bEnableSound);
		addInt(OptionPage::Audio, nullptr, "SFX volume",
			"Engines, weapons, footsteps and the people talking. The game's own setting.",
			&g_placeholder.sfxVolume, 0, 100, 10, 100);
		placeholder();
		addInt(OptionPage::Audio, nullptr, "Radio volume",
			"The car radio. The game's own menu calls this MUSIC.",
			&g_placeholder.radioVolume, 0, 100, 10, 100);
		placeholder();
		addBool(OptionPage::Audio, nullptr, "Custom soundtracks",
			"Your own music on the radio, from the memory stick.",
			&g_placeholder.customTracks);
		placeholder();

		// --- Display ---

		addInt(OptionPage::Graphics, nullptr, "Brightness",
			"How bright the game draws the world. The game's own Display setting.",
			&g_placeholder.brightness, 0, 100, 10, 50);
		placeholder();
		addChoice(OptionPage::Graphics, nullptr, "Resolution",
			"Internal rendering resolution. Higher is sharper and costs more.",
			&g_Config.iInternalResolution, kResolutionLabels, ARRAY_SIZE(kResolutionLabels),
			Config::GetDefaultValueInt(&g_Config.iInternalResolution), true, []() {
				System_PostUIMessage(UIMessage::GPU_RENDER_RESIZED);
			}, &ResolutionText);
		addBool(OptionPage::Graphics, nullptr, "Fullscreen",
			"Fill the screen. The window comes back where it was when this is turned off.",
			&g_Config.bFullScreen, true, true, []() {
				System_ApplyFullscreenState();
			});
		addChoice(OptionPage::Graphics, nullptr, "Widescreen",
			"Fill a screen wider than the PSP's without stretching it.",
			&g_placeholder.widescreen, kWidescreenLabels, ARRAY_SIZE(kWidescreenLabels), 0);
		placeholder();
		addBool(OptionPage::Graphics, "FrameRate60", "Frame limiter",
			"30 is the game as it shipped. 60 draws twice as often at the same speed.",
			&s.frameRate60, false, false, nullptr, [](const Option &opt) {
				return std::string(*opt.boolValue ? "60 FPS" : "30 FPS");
			});
		addBool(OptionPage::Graphics, nullptr, "Show FPS",
			"The frame rate in the corner of the screen.", &g_placeholder.showFps);
		placeholder();

		// --- Display, advanced ---

		addBool(OptionPage::GraphicsAdvanced, nullptr, "Texture quality",
			"HIGH uses the installed HD texture pack. LOW draws the PSP's own textures.",
			&g_Config.bReplaceTextures, true, true, []() {
				System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
			}, [](const Option &opt) {
				return std::string(*opt.boolValue ? "HIGH" : "LOW");
			});
		addChoice(OptionPage::GraphicsAdvanced, nullptr, "Shadow quality",
			"Real shadows from the sun, which the PSP game has none of.",
			&g_placeholder.shadows, kShadowLabels, ARRAY_SIZE(kShadowLabels), 1);
		placeholder();
		addBool(OptionPage::GraphicsAdvanced, nullptr, "Ambient occlusion",
			"Soft darkening in corners and creases, where a wall meets the pavement.",
			&g_placeholder.ambientOcclusion);
		placeholder();
		addChoice(OptionPage::GraphicsAdvanced, nullptr, "Water quality",
			"Waves, reflections and sun glint on the river, which the PSP draws flat.",
			&g_placeholder.water, kWaterLabels, ARRAY_SIZE(kWaterLabels), 1);
		placeholder();
		addBool(OptionPage::GraphicsAdvanced, "AntiAliasing", "Anti-aliasing",
			"Smooths the jagged edges of the city without blurring the HUD. Vulkan only.",
			&s.antiAliasing, false, false, nullptr, [](const Option &opt) {
				return std::string(*opt.boolValue ? "SMAA" : "OFF");
			});
		addChoice(OptionPage::GraphicsAdvanced, nullptr, "Anisotropic filtering",
			"Sharpens textures seen at a shallow angle, like the road ahead.",
			&g_Config.iAnisotropyLevel, kAnisoLabels, ARRAY_SIZE(kAnisoLabels),
			Config::GetDefaultValueInt(&g_Config.iAnisotropyLevel), true);
		addBool(OptionPage::GraphicsAdvanced, "WorldMemory", "World memory",
			"A PSP-2000's memory: the game keeps more of the city loaded. After a restart, and "
			"savestates from the other setting won't load.",
			&s.worldMemory, false, false, nullptr, [](const Option &opt) {
				return std::string(*opt.boolValue ? "LARGE" : "PSP");
			});

		// --- Gameplay (to come) ---

		addBool(OptionPage::Gameplay, nullptr, "Auto-save",
			"Save after every story mission, over the auto-save slot.", &g_placeholder.autoSave);
		placeholder();
		addBool(OptionPage::Gameplay, nullptr, "GPS route",
			"A route to your marker on the radar, along the roads.", &g_placeholder.gpsRoute);
		placeholder();
		addChoice(OptionPage::Gameplay, nullptr, "Language",
			"The game's text and menus; the voices stay English.",
			&g_placeholder.language, kLanguageLabels, ARRAY_SIZE(kLanguageLabels), 0);
		placeholder();
		addBool(OptionPage::Gameplay, nullptr, "Subtitles",
			"Dialogue as text on screen, in cutscenes and on the phone.", &g_placeholder.subtitles);
		placeholder();
		addBool(OptionPage::Gameplay, nullptr, "HUD",
			"Health, armour, money, the weapon and the clock.", &g_placeholder.hud);
		placeholder();

		return opts;
	}();
	return options;
}

// --- Persistence, by the table ---

static bool Saved(const Option &opt) {
	return opt.iniKey && !opt.external && !opt.placeholder;
}

void LoadSettings() {
	// First, so the table captures the compiled-in defaults before anything is read.
	const std::vector<Option> &options = Options();
	g_settings = LCSSettings();
	IniFile ini;
	// A missing file is the normal first run; every value keeps its default.
	ini.Load(SettingsPath());
	const Section *section = ini.GetOrCreateSection("Settings");
	// A missing key leaves the value alone, so each one is put back to its default first: not
	// every row lives in g_settings (the chase camera's tuning holds one).
	for (const Option &opt : options) {
		if (!Saved(opt)) {
			continue;
		}
		switch (opt.type) {
		case OptionType::Bool:
			*opt.boolValue = opt.defaultBool;
			section->Get(opt.iniKey, opt.boolValue);
			break;
		case OptionType::Float:
			*opt.floatValue = opt.defaultFloat;
			section->Get(opt.iniKey, opt.floatValue);
			break;
		case OptionType::Int:
		case OptionType::Choice:
			*opt.intValue = opt.defaultInt;
			section->Get(opt.iniKey, opt.intValue);
			break;
		}
	}
	NOTICE_LOG(Log::System, "LCS: settings loaded (FrameRate60 = %s, KeyboardMouse = %s, MouseLook = %s, MouseSensitivity = %g, VehicleChaseCamera = %s, FootChaseCamera = %s)",
		g_settings.frameRate60 ? "true" : "false", g_settings.keyboardMouse ? "true" : "false",
		g_settings.mouseLook ? "true" : "false", g_settings.mouseSensitivity,
		g_settings.vehicleChaseCamera ? "true" : "false", g_settings.footChaseCamera ? "true" : "false");
}

bool WantsExtraWorldMemory() {
	IniFile ini;
	if (!ini.Load(SettingsPath())) {
		return false;
	}
	const Section *section = ini.GetSection("Settings");
	bool value = false;
	return section && section->Get("WorldMemory", &value) && value;
}

void SaveSettings() {
	IniFile ini;
	ini.Load(SettingsPath());
	Section *section = ini.GetOrCreateSection("Settings");
	for (const Option &opt : Options()) {
		if (!Saved(opt)) {
			continue;
		}
		switch (opt.type) {
		case OptionType::Bool: section->Set(opt.iniKey, *opt.boolValue); break;
		case OptionType::Float: section->Set(opt.iniKey, *opt.floatValue); break;
		case OptionType::Int:
		case OptionType::Choice: section->Set(opt.iniKey, *opt.intValue); break;
		}
	}
	if (!ini.Save(SettingsPath())) {
		ERROR_LOG(Log::System, "LCS: could not write %s", SettingsPath().c_str());
	}
}

// --- Editing ---

void ResetPage(OptionPage page) {
	for (const Option &opt : Options()) {
		if (opt.page != page) {
			continue;
		}
		switch (opt.type) {
		case OptionType::Bool: *opt.boolValue = opt.defaultBool; break;
		case OptionType::Float: *opt.floatValue = opt.defaultFloat; break;
		case OptionType::Int:
		case OptionType::Choice: *opt.intValue = opt.defaultInt; break;
		}
		// Every type: a reset moves the same values the controls do, and owes the same news.
		if (opt.onChange) {
			opt.onChange();
		}
	}
}

float GetNormalized(const Option &opt) {
	if (opt.type == OptionType::Int) {
		if (opt.maxInt <= opt.minInt) {
			return 0.0f;
		}
		return std::clamp((float)(*opt.intValue - opt.minInt) / (float)(opt.maxInt - opt.minInt), 0.0f, 1.0f);
	}
	if (opt.type != OptionType::Float || opt.maxValue <= opt.minValue) {
		return 0.0f;
	}
	return std::clamp((*opt.floatValue - opt.minValue) / (opt.maxValue - opt.minValue), 0.0f, 1.0f);
}

void SetNormalized(const Option &opt, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	if (opt.type == OptionType::Int) {
		// Snapped to the step, so a drag can only land where the arrow keys could.
		const int span = opt.maxInt - opt.minInt;
		const int step = opt.stepInt > 0 ? opt.stepInt : 1;
		const int offset = ((int)(t * (float)span + 0.5f) + step / 2) / step * step;
		SetInt(opt, opt.minInt + offset);
		return;
	}
	if (opt.type == OptionType::Float) {
		*opt.floatValue = opt.minValue + t * (opt.maxValue - opt.minValue);
	}
}

void SetInt(const Option &opt, int value) {
	if (opt.type != OptionType::Int && opt.type != OptionType::Choice) {
		return;
	}
	const int low = opt.type == OptionType::Int ? opt.minInt : 0;
	const int high = opt.type == OptionType::Int ? opt.maxInt : opt.numChoices - 1;
	value = std::clamp(value, low, high);
	if (value == *opt.intValue) {
		return;
	}
	*opt.intValue = value;
	if (opt.onChange) {
		opt.onChange();
	}
}

void SetBool(const Option &opt, bool value) {
	if (opt.type != OptionType::Bool || value == *opt.boolValue) {
		return;
	}
	*opt.boolValue = value;
	if (opt.onChange) {
		opt.onChange();
	}
}

std::string ValueText(const Option &opt) {
	if (opt.valueText) {
		return opt.valueText(opt);
	}
	char buffer[32];
	switch (opt.type) {
	case OptionType::Bool:
		return *opt.boolValue ? "ON" : "OFF";
	case OptionType::Choice:
		return std::string(opt.choices[std::clamp(*opt.intValue, 0, opt.numChoices - 1)]);
	case OptionType::Int:
		snprintf(buffer, sizeof(buffer), opt.format ? opt.format : "%d", *opt.intValue);
		return std::string(buffer);
	case OptionType::Float:
		if (opt.format) {
			snprintf(buffer, sizeof(buffer), opt.format, *opt.floatValue);
		} else {
			snprintf(buffer, sizeof(buffer), "%d", (int)(GetNormalized(opt) * 100.0f + 0.5f));
		}
		return std::string(buffer);
	}
	return std::string();
}

}  // namespace LCS
