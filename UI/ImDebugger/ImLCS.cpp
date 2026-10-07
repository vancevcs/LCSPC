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

#include "ext/imgui/imgui.h"
#include "Core/System.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSState.h"
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
		if (ImGui::BeginTabItem("Scratchpad")) {
			DrawScratchpad();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}
