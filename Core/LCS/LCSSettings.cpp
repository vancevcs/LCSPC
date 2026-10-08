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

#include "Common/Data/Format/IniFile.h"
#include "Common/File/Path.h"
#include "Common/Log.h"
#include "Core/System.h"
#include "Core/LCS/LCSSettings.h"

namespace LCS {

static LCSSettings g_settings;

static Path SettingsPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "lcs.ini";
}

LCSSettings &Settings() {
	return g_settings;
}

void LoadSettings() {
	g_settings = LCSSettings();
	IniFile ini;
	// A missing file is the normal first run; every value keeps its default.
	ini.Load(SettingsPath());
	const Section *section = ini.GetOrCreateSection("Settings");
	section->Get("FrameRate60", &g_settings.frameRate60);
	section->Get("KeyboardMouse", &g_settings.keyboardMouse);
	section->Get("MouseLook", &g_settings.mouseLook);
	section->Get("MouseSensitivity", &g_settings.mouseSensitivity);
	section->Get("InvertMouseY", &g_settings.invertMouseY);
	section->Get("VehicleChaseCamera", &g_settings.vehicleChaseCamera);
	section->Get("FootChaseCamera", &g_settings.footChaseCamera);
	section->Get("MouseFreeAim", &g_settings.freeAim);
	section->Get("WorldMemory", &g_settings.worldMemory);
	section->Get("AntiAliasing", &g_settings.antiAliasing);
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
	section->Set("FrameRate60", g_settings.frameRate60);
	section->Set("KeyboardMouse", g_settings.keyboardMouse);
	section->Set("MouseLook", g_settings.mouseLook);
	section->Set("MouseSensitivity", g_settings.mouseSensitivity);
	section->Set("InvertMouseY", g_settings.invertMouseY);
	section->Set("VehicleChaseCamera", g_settings.vehicleChaseCamera);
	section->Set("FootChaseCamera", g_settings.footChaseCamera);
	section->Set("MouseFreeAim", g_settings.freeAim);
	section->Set("WorldMemory", g_settings.worldMemory);
	section->Set("AntiAliasing", g_settings.antiAliasing);
	if (!ini.Save(SettingsPath())) {
		ERROR_LOG(Log::System, "LCS: could not write %s", SettingsPath().c_str());
	}
}

}  // namespace LCS
