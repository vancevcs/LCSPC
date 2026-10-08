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

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "ext/imgui/imgui.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/KeyMap.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSChaseCam.h"
#include "Core/LCS/LCSFrameRate.h"
#include "Core/LCS/LCSFreeAim.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"
#include "GPU/Common/LCSSmaa.h"
#include "UI/ImDebugger/ImDebugger.h"
#include "UI/ImDebugger/ImLCS.h"

static const ImVec4 kUnsetColor = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
static const ImVec4 kGoodColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
static const ImVec4 kBadColor = ImVec4(0.90f, 0.45f, 0.45f, 1.0f);

// "unset" in grey rather than a misleading zero.
static void TextUnset() {
	ImGui::TextColored(kUnsetColor, "unset");
}

static void TextOptionalBool(const std::optional<bool> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::TextColored(*value ? kGoodColor : kUnsetColor, "%s", *value ? "true" : "false");
}

static void TextOptionalU32(const std::optional<u32> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::Text("%u  (0x%08x)", *value, *value);
}

static void TextOptionalFloat(const std::optional<float> &value) {
	if (!value) {
		TextUnset();
		return;
	}
	ImGui::Text("%.4f", *value);
}

static const char *AddrTypeName(LCS::LCSAddrType type) {
	switch (type) {
	case LCS::LCSAddrType::U8: return "u8";
	case LCS::LCSAddrType::U16: return "u16";
	case LCS::LCSAddrType::U32: return "u32";
	case LCS::LCSAddrType::S32: return "s32";
	case LCS::LCSAddrType::Float: return "float";
	default: return "?";
	}
}

void ImLCSWindow::DrawStatus() {
	if (!PSP_IsInited()) {
		ImGui::TextColored(kUnsetColor, "No game running.");
		return;
	}

	const std::string &disc = LCS::GetDiscID();
	const std::string &version = LCS::GetDiscVersion();
	if (!LCS::IsActive()) {
		ImGui::TextColored(kBadColor, "Inactive");
		ImGui::SameLine();
		ImGui::TextWrapped("- %s", LCS::InactiveReason().c_str());
		ImGui::Text("Disc: %s  version %s", disc.empty() ? "(none)" : disc.c_str(), version.empty() ? "(none)" : version.c_str());
		if (!LCS::EbootHash().empty()) {
			ImGui::Text("EBOOT: %s", LCS::EbootHash().c_str());
		}
		return;
	}

	ImGui::TextColored(kGoodColor, "Active");
	ImGui::SameLine();
	ImGui::Text("- %s v%s", disc.c_str(), version.c_str());
	ImGui::Text("EBOOT: %s", LCS::EbootHash().c_str());

	// A frozen tick count means the vblank seam isn't running, a different problem from a wrong
	// address. The game's own frame counter is next to it for the same reason.
	ImGui::Text("Ticks: %llu", (unsigned long long)LCS::GetTickCount());
	const std::optional<u32> frames = LCS::GetState().frameCounter;
	ImGui::SameLine();
	if (frames) {
		ImGui::Text("   Game frames: %u", *frames);
	} else {
		ImGui::TextColored(kUnsetColor, "   Game frames: unset");
	}

	int known = 0;
	for (const LCS::LCSAddrEntry &entry : LCS::kLCSAddresses) {
		if (LCS::IsAddrSet(entry.id)) {
			known++;
		}
	}
	ImGui::Text("Addresses known: %d / %d", known, (int)LCS::LCSAddr::Count);

	// Saved to lcs.ini at once; LCS::Tick applies it on the next vblank.
	bool frameRate60 = LCS::Settings().frameRate60;
	if (ImGui::Checkbox("60 fps", &frameRate60)) {
		LCS::Settings().frameRate60 = frameRate60;
		LCS::SaveSettings();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(%s)", LCS::FrameRateIs60() ? "cap and carry in" : "stock");
	ImGui::SameLine();
	bool worldMemory = LCS::Settings().worldMemory;
	if (ImGui::Checkbox("World memory", &worldMemory)) {
		LCS::Settings().worldMemory = worldMemory;
		LCS::SaveSettings();
	}
	ImGui::SameLine();
	const u32 reserve = LCS::ReadU32(LCS::kLCSMainMemoryReserve).value_or(0);
	ImGui::TextDisabled("(%d MB partition, %.1f MB reserve; takes a restart)", (int)(Memory::g_MemorySize >> 20),
		reserve / (1024.0f * 1024.0f));

	bool smaa = LCS::Settings().antiAliasing;
	if (ImGui::Checkbox("SMAA", &smaa)) {
		LCS::Settings().antiAliasing = smaa;
		LCS::SaveSettings();
	}
	ImGui::SameLine();
	static const char *kViews[] = { "as it ships", "split: left smoothed", "blend weights" };
	ImGui::SetNextItemWidth(170.0f);
	ImGui::Combo("##smaaview", &LCSSmaa::DebugView(), kViews, 3);
	ImGui::SameLine();
	ImGui::TextDisabled("(%s; Vulkan only)", LCSSmaa::IsActive() ? "running" : "off");
}

void ImLCSWindow::DrawDecodedState() {
	const LCS::LCSState &state = LCS::GetState();

	if (!ImGui::BeginTable("lcsstate", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		return;
	}
	ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 130.0f);
	ImGui::TableSetupColumn("Value");
	ImGui::TableHeadersRow();

	auto row = [](const char *label) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(label);
		ImGui::TableNextColumn();
	};

	row("playerBase");    TextOptionalU32(state.playerBase);
	row("health");        TextOptionalFloat(state.health);
	row("inVehicle");     TextOptionalBool(state.inVehicle);
	row("playerVehicle"); TextOptionalU32(state.playerVehicle);
	row("vehicleType");   TextOptionalU32(state.vehicleType);
	row("vehicleClass");  ImGui::TextUnformatted(LCS::VehicleClassName(state.vehicleClass));
	row("weaponSlot");    TextOptionalU32(state.weaponSlot);
	row("weaponType");    TextOptionalU32(state.weaponType);
	row("activeCam");     TextOptionalU32(state.activeCam);
	row("camMode");       TextOptionalU32(state.camMode);
	row("camYaw");        TextOptionalFloat(state.camYaw);
	row("camPitch");      TextOptionalFloat(state.camPitch);
	row("camFOV");        TextOptionalFloat(state.camFOV);
	row("timeInMs");      TextOptionalU32(state.timeInMs);
	row("timeStep");      TextOptionalFloat(state.timeStep);
	row("frameCounter");  TextOptionalU32(state.frameCounter);

	ImGui::EndTable();
}

void ImLCSWindow::DrawAddressTable() {
	ImGui::TextWrapped(
		"Addresses live in Core/LCS/LCSAddresses.h and docs/LCS_ADDRESSES.md. Hover a name for its note.");
	ImGui::Spacing();

	if (!ImGui::BeginTable("lcsaddrs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		return;
	}
	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 120.0f);
	ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 45.0f);
	ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
	ImGui::TableSetupColumn("Live value");
	ImGui::TableHeadersRow();

	for (size_t i = 0; i < ARRAY_SIZE(LCS::kLCSAddresses); i++) {
		const LCS::LCSAddrEntry &entry = LCS::kLCSAddresses[i];

		ImGui::TableNextRow();
		ImGui::PushID((int)i);

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(entry.name);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", entry.note);
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted(AddrTypeName(entry.type));

		ImGui::TableNextColumn();
		if (!LCS::IsAddrSet(entry.id)) {
			TextUnset();
		} else if (entry.base != LCS::kNoBase) {
			// Where it lives right now, so it can be pasted into the scratchpad.
			const std::optional<u32> resolved = LCS::ResolveAddr(entry.id);
			if (resolved) {
				ImGui::Text("0x%08x", *resolved);
			} else {
				ImGui::TextDisabled("(base null)");
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s + 0x%x", LCS::LookupAddr(entry.base).name, entry.address);
			}
		} else {
			ImGui::Text("0x%08x", entry.address);
		}

		ImGui::TableNextColumn();
		if (!LCS::IsAddrSet(entry.id)) {
			ImGui::TextDisabled("-");
		} else if (entry.type == LCS::LCSAddrType::Float) {
			TextOptionalFloat(LCS::ReadAddrFloat(entry.id));
		} else {
			TextOptionalU32(LCS::ReadAddrAsU32(entry.id));
		}

		ImGui::PopID();
	}

	ImGui::EndTable();
}

void ImLCSWindow::DrawScratchpad() {
	ImGui::TextWrapped(
		"Type a candidate address in hex. Each row is re-read every frame and shown four ways, so a "
		"float and an int can be told apart by which column looks sane. Invalid addresses read as '-'.");
	ImGui::Spacing();

	if (!ImGui::BeginTable("lcsscratch", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		return;
	}
	ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("Hold", ImGuiTableColumnFlags_WidthFixed, 45.0f);
	ImGui::TableSetupColumn("u32", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("s32", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("float", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("u8 / hex");
	ImGui::TableHeadersRow();

	for (int i = 0; i < kScratchRows; i++) {
		ScratchRow &rowData = scratch_[i];

		ImGui::TableNextRow();
		ImGui::PushID(i);

		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText("##addr", rowData.addressText, sizeof(rowData.addressText), ImGuiInputTextFlags_CharsHexadecimal);

		ImGui::TableNextColumn();
		ImGui::Checkbox("##hold", &rowData.hold);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Stop refreshing this row.");
		}

		u32 address = 0;
		bool haveAddress = false;
		if (rowData.addressText[0] != '\0') {
			char *end = nullptr;
			const unsigned long parsed = strtoul(rowData.addressText, &end, 16);
			if (end != rowData.addressText) {
				address = (u32)parsed;
				haveAddress = true;
			}
		}

		// Every column describes the same word: the live one, or the held one.
		std::optional<u32> raw;
		if (haveAddress) {
			if (rowData.hold && rowData.hasHeld) {
				raw = rowData.heldRaw;
			} else {
				raw = LCS::ReadU32(address);
				if (raw) {
					rowData.heldRaw = *raw;
					rowData.hasHeld = true;
				}
			}
		} else {
			rowData.hasHeld = false;
		}

		ImGui::TableNextColumn();
		if (raw) {
			ImGui::Text("%u", *raw);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (raw) {
			s32 signedValue;
			memcpy(&signedValue, &*raw, sizeof(signedValue));
			ImGui::Text("%d", signedValue);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (raw) {
			float asFloat;
			memcpy(&asFloat, &*raw, sizeof(asFloat));
			ImGui::Text("%.4f", asFloat);
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::TableNextColumn();
		if (rowData.hold && rowData.hasHeld) {
			ImGui::Text("%u / 0x%08x", rowData.heldRaw & 0xFF, rowData.heldRaw);
		} else if (haveAddress) {
			const std::optional<u8> byteValue = LCS::ReadU8(address);
			if (byteValue) {
				ImGui::Text("%u / 0x%08x", *byteValue, raw.value_or(0));
			} else {
				ImGui::TextDisabled("-");
			}
		} else {
			ImGui::TextDisabled("-");
		}

		ImGui::PopID();
	}

	ImGui::EndTable();

	if (ImGui::Button("Clear all rows")) {
		for (int i = 0; i < kScratchRows; i++) {
			scratch_[i] = ScratchRow();
		}
	}
}

static std::string ButtonNames(u32 mask) {
	static const struct { u32 bit; const char *name; } kNames[] = {
		{ CTRL_CROSS, "Cross" }, { CTRL_CIRCLE, "Circle" }, { CTRL_SQUARE, "Square" }, { CTRL_TRIANGLE, "Triangle" },
		{ CTRL_LTRIGGER, "L" }, { CTRL_RTRIGGER, "R" }, { CTRL_UP, "Up" }, { CTRL_DOWN, "Down" },
		{ CTRL_LEFT, "Left" }, { CTRL_RIGHT, "Right" }, { CTRL_START, "Start" }, { CTRL_SELECT, "Select" },
	};
	std::string out;
	for (const auto &n : kNames) {
		if (mask & n.bit) {
			if (!out.empty()) {
				out += " + ";
			}
			out += n.name;
		}
	}
	return out.empty() ? "-" : out;
}

void ImLCSWindow::DrawInput() {
	LCS::LCSSettings &settings = LCS::Settings();
	bool changed = false;
	changed |= ImGui::Checkbox("Keyboard and mouse", &settings.keyboardMouse);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Mouse look", &settings.mouseLook);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Invert mouse Y", &settings.invertMouseY);
	ImGui::SetNextItemWidth(200.0f);
	changed |= ImGui::SliderFloat("Mouse sensitivity", &settings.mouseSensitivity, 0.0005f, 0.01f, "%.4f rad/count");
	if (changed) {
		LCS::SaveSettings();
	}
	if (settings.mouseLook && !g_Config.bMouseControl) {
		ImGui::TextColored(kBadColor, "PPSSPP's \"Use mouse control\" is off: no mouse look or left click.");
	}

	ImGui::Separator();
	ImGui::Text("Context: %s", LCS::InputContextName(LCS::CurrentInputContext()));
	ImGui::Text("Holding: %s", ButtonNames(LCS::AppliedButtons()).c_str());
	ImGui::Text("On-foot pitch spring: %s", LCS::MouseLookEngaged() ? "off (mouse look)" : "stock");

	ImGui::Separator();
	if (!ImGui::BeginTable("lcsbindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_ScrollY)) {
		return;
	}
	ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110.0f);
	ImGui::TableSetupColumn("Does");
	ImGui::TableHeadersRow();
	ImGui::TableNextRow();
	ImGui::TableNextColumn(); ImGui::TextUnformatted("on foot");
	ImGui::TableNextColumn(); ImGui::TextUnformatted("W A S D");
	ImGui::TableNextColumn(); ImGui::TextUnformatted("Move (the stick)");
	ImGui::TableNextRow();
	ImGui::TableNextColumn(); ImGui::TextUnformatted("in vehicle");
	ImGui::TableNextColumn(); ImGui::TextUnformatted("A D / Up Down");
	ImGui::TableNextColumn(); ImGui::TextUnformatted("Steer / lean or aim (the stick)");
	for (size_t i = 0; i < LCS::kKeyMappingCount; i++) {
		const LCS::KeyMapping &m = LCS::kKeyMappings[i];
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(LCS::InputContextName(m.context));
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(KeyMap::GetKeyName(m.key).c_str());
		ImGui::TableNextColumn();
		ImGui::Text("%s  (%s)", m.description, ButtonNames(m.psp).c_str());
	}
	ImGui::EndTable();
}

void ImLCSWindow::DrawCamera() {
	LCS::LCSSettings &settings = LCS::Settings();
	bool changed = ImGui::Checkbox("On-foot chase camera", &settings.footChaseCamera);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Vehicle chase camera", &settings.vehicleChaseCamera);
	if (changed) {
		LCS::SaveSettings();
	}
	if (!settings.keyboardMouse || !settings.mouseLook) {
		ImGui::SameLine();
		ImGui::TextColored(kBadColor, "needs Keyboard and mouse and Mouse look (Input tab)");
	}

	LCS::LCSChaseCamStats s;
	LCS::ChaseCamGetStats(&s);
	ImGui::Text("Status: %s", s.status);
	if (s.block) {
		ImGui::SameLine();
		ImGui::Text("(block %08x)", s.block);
	}
	ImGui::Text("Owning: %s   mode %d   frames %llu / %llu owned", s.owning ? s.kind : "no", s.mode,
		(unsigned long long)s.frames, (unsigned long long)s.ownedFrames);
	ImGui::Text("Yaw %.3f   pitch %.3f   since look %.1f s%s", s.yaw, s.pitch, s.sinceLook,
		s.glancing ? "   glancing" : "");
	ImGui::Text("Distance: game %.2f   want %.2f   allowed %.2f   now %.2f", s.gameDistance,
		s.wantDistance, s.allowedDistance, s.distance);
	ImGui::Text("Rays %d, hits %d   clearance %.2f   near %.3f", s.rays, s.hits, s.clearance, s.nearClip);
	ImGui::Text("Pivot %.2f %.2f %.2f   box %.2f long%s   speed %.1f", s.pivot[0], s.pivot[1], s.pivot[2],
		s.boxLength, s.boxFromGame ? "" : " (default)", s.speed);
	ImGui::Text("Blend in %.2f%s", s.blendIn, s.blendingOut ? "   blending out" : "");

	ImGui::Separator();
	LCS::LCSChaseCamTuning &t = LCS::ChaseCamTuning();
	ImGui::PushItemWidth(200.0f);
	ImGui::SliderFloat("On foot: distance", &t.footDistance, 1.5f, 8.0f);
	ImGui::SliderFloat("On foot: pivot height", &t.footPivotHeight, 0.0f, 1.5f);
	ImGui::SliderFloat("Vehicle: distance scale", &t.vehicleDistanceScale, 0.5f, 2.0f);
	ImGui::SliderFloat("Vehicle: pivot (of box top)", &t.vehiclePivotScale, 0.5f, 2.0f);
	ImGui::SliderFloat("Vehicle: rest pitch", &t.vehicleRestPitch, -0.6f, 0.3f);
	ImGui::SliderFloat("Vehicle: orbit up limit", &t.vehicleOrbitUp, 0.0f, 1.2f);
	ImGui::Checkbox("Recentre", &t.vehicleRecentre);
	ImGui::SameLine();
	ImGui::Checkbox("Glances", &t.glances);
	ImGui::SameLine();
	ImGui::Checkbox("Near clip", &t.nearClip);
	ImGui::SameLine();
	ImGui::Checkbox("Keep shake", &t.keepShake);
	ImGui::SliderFloat("Recentre delay", &t.recentreDelay, 0.0f, 5.0f, "%.1f s");
	ImGui::SliderInt("Rays", &t.rays, 0, 5);
	ImGui::PopItemWidth();
	ImGui::TextDisabled("Tuning is for this session only.");
}

// --- Pad: PSP buttons pressed from here, for cheats and anything a key isn't bound to ---

// The game's cheats are typed on the pad during play, on foot or in a car.
struct PadCheat {
	const char *name;
	const char *sequence;
};
static const PadCheat kPadCheats[] = {
	{ "Weapon set 1", "Up Square Square Down Left Square Square Right" },
	{ "Weapon set 2", "Up Circle Circle Down Left Circle Circle Right" },
	{ "Weapon set 3", "Up Cross Cross Down Left Cross Cross Right" },
};

void ImLCSWindow::DrawPad() {
	ImGui::TextWrapped("Click to tap a PSP button (held 6 vblanks). Hold keeps it down until clicked again. "
		"Presses go through the LCS input layer, so they reach the game even with this window focused.");

	auto tap = [](const char *label, u32 mask, float width = 70.0f) {
		if (ImGui::Button(label, ImVec2(width, 0))) {
			LCS::QueuePadTaps({ mask });
		}
	};
	auto hold = [](const char *label, u32 mask) {
		bool held = (LCS::PadHeld() & mask) != 0;
		if (ImGui::Checkbox(label, &held)) {
			LCS::SetPadHeld(mask, held);
		}
	};

	tap("L", CTRL_LTRIGGER); ImGui::SameLine(); hold("hold##L", CTRL_LTRIGGER);
	ImGui::SameLine(260.0f);
	tap("R", CTRL_RTRIGGER); ImGui::SameLine(); hold("hold##R", CTRL_RTRIGGER);

	const float x0 = ImGui::GetCursorPosX();
	ImGui::SetCursorPosX(x0 + 74.0f); tap("Up", CTRL_UP);
	ImGui::SameLine(x0 + 260.0f + 74.0f); tap("Triangle", CTRL_TRIANGLE);
	ImGui::SetCursorPosX(x0); tap("Left", CTRL_LEFT); ImGui::SameLine(x0 + 148.0f); tap("Right", CTRL_RIGHT);
	ImGui::SameLine(x0 + 260.0f); tap("Square", CTRL_SQUARE); ImGui::SameLine(x0 + 260.0f + 148.0f); tap("Circle", CTRL_CIRCLE);
	ImGui::SetCursorPosX(x0 + 74.0f); tap("Down", CTRL_DOWN);
	ImGui::SameLine(x0 + 260.0f + 74.0f); tap("Cross", CTRL_CROSS);
	ImGui::SetCursorPosX(x0 + 74.0f); tap("Select", CTRL_SELECT);
	ImGui::SameLine(x0 + 260.0f + 74.0f); tap("Start", CTRL_START);

	ImGui::Separator();
	ImGui::TextUnformatted("Sequence (spaces between taps, + for buttons together, e.g. L+R):");
	ImGui::SetNextItemWidth(-80.0f);
	ImGui::InputText("##cheat", cheatText_, sizeof(cheatText_));
	ImGui::SameLine();
	if (ImGui::Button("Play", ImVec2(70.0f, 0))) {
		std::vector<u32> masks;
		std::string bad;
		if (LCS::ParsePadSequence(cheatText_, &masks, &bad)) {
			cheatError_.clear();
			LCS::QueuePadTaps(masks);
		} else {
			cheatError_ = "No button called '" + bad + "'";
		}
	}
	if (!cheatError_.empty()) {
		ImGui::TextColored(kBadColor, "%s", cheatError_.c_str());
	}
	for (const PadCheat &cheat : kPadCheats) {
		if (ImGui::Button(cheat.name)) {
			std::vector<u32> masks;
			std::string bad;
			if (LCS::ParsePadSequence(cheat.sequence, &masks, &bad)) {
				LCS::QueuePadTaps(masks);
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", cheat.sequence);
	}
	const int pending = LCS::PadTapsPending();
	ImGui::Text("Playing: %s", pending ? std::to_string(pending).append(" taps left").c_str() : "-");
	ImGui::Text("Holding: %s", ButtonNames(LCS::AppliedButtons()).c_str());
}

void ImLCSWindow::DrawAim() {
	LCS::LCSSettings &settings = LCS::Settings();
	if (ImGui::Checkbox("Mouse free aim", &settings.freeAim)) {
		LCS::SaveSettings();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("right mouse with a gun: the game's free aim, aimed by the mouse");
	LCS::LCSFreeAimTuning &t = LCS::FreeAimTuning();
	ImGui::Checkbox("Shots down the reticle", &t.redirectShots);

	LCS::LCSFreeAimStats s;
	LCS::FreeAimGetStats(&s);
	ImGui::Separator();
	ImGui::Text("Fire hook: %s", s.status);
	ImGui::Text("Free aim: %s%s   firearm: %s", s.active ? "active" : "-", s.holding ? " (holding)" : "",
		LCS::PlayerHasFirearm() ? "yes" : "no");
	ImGui::Text("Aim: Beta %.3f  Alpha %.3f   target %08x at %.1f", s.yaw, s.pitch, s.target, s.targetDistance);
	ImGui::Text("Player shots seen %llu, redirected %llu   last: weapon %d, range %.0f",
		(unsigned long long)s.seen, (unsigned long long)s.redirected, s.lastWeaponType, s.lastRange);
	if (s.haveHit) {
		ImGui::Text("Last hit %.2f %.2f %.2f, %.3f off the camera's centre line", s.lastHit[0], s.lastHit[1],
			s.lastHit[2], s.lastHitOffLine);
	}
}

void ImLCSWindow::Draw(ImConfig &cfg) {
	ImGui::SetNextWindowSize(ImVec2(600, 480), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("LCS", &cfg.lcsOpen)) {
		ImGui::End();
		return;
	}

	DrawStatus();
	ImGui::Separator();

	if (ImGui::BeginTabBar("lcstabs")) {
		if (ImGui::BeginTabItem("State")) {
			DrawDecodedState();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Addresses")) {
			DrawAddressTable();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Input")) {
			DrawInput();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Aim")) {
			DrawAim();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Pad")) {
			DrawPad();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Camera")) {
			DrawCamera();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Scratchpad")) {
			DrawScratchpad();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}
