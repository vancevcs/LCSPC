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
	NOTICE_LOG(Log::System, "LCS: settings loaded (FrameRate60 = %s)", g_settings.frameRate60 ? "true" : "false");
}

void SaveSettings() {
	IniFile ini;
	ini.Load(SettingsPath());
	Section *section = ini.GetOrCreateSection("Settings");
	section->Set("FrameRate60", g_settings.frameRate60);
	if (!ini.Save(SettingsPath())) {
		ERROR_LOG(Log::System, "LCS: could not write %s", SettingsPath().c_str());
	}
}

}  // namespace LCS
