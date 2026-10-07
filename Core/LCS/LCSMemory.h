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

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/LCS/LCSAddresses.h"

// Bounds-checked typed reads and writes of PSP memory for the LCS layer.
//
// A wrong address is a normal condition while hunting for addresses, not a crash. Every access is
// validated against PPSSPP's memory map and fails with std::nullopt (or false) rather than faulting,
// asserting or logging.
//
// Emu thread only: from LCS::Tick (driven by hleEnterVblank) or from the ImGui debugger, which runs
// on the CPU thread (docs/DebuggerThreading.md).

namespace LCS {

std::optional<u8> ReadU8(u32 address);
std::optional<u16> ReadU16(u32 address);
std::optional<u32> ReadU32(u32 address);
std::optional<s32> ReadS32(u32 address);
std::optional<float> ReadFloat(u32 address);

// Reads a pointer and checks that it points somewhere valid. nullopt for null, too.
std::optional<u32> ReadPointer(u32 address);

// Where an entry lives right now: its address, or for a based entry, the base pointer plus the
// offset. nullopt when unset or when the base pointer isn't valid (during a load, for instance).
std::optional<u32> ResolveAddr(LCSAddr id);

std::optional<u32> ReadAddrU32(LCSAddr id);
std::optional<s32> ReadAddrS32(LCSAddr id);
std::optional<float> ReadAddrFloat(LCSAddr id);

// Reads an entry at its table type, widened to u32. Float entries return nullopt.
std::optional<u32> ReadAddrAsU32(LCSAddr id);
std::optional<bool> ReadAddrBool(LCSAddr id);

// Writes return whether they happened. An unset or invalid target is a silent no-op.
bool WriteU8(u32 address, u8 value);
bool WriteU16(u32 address, u16 value);
bool WriteU32(u32 address, u32 value);
bool WriteFloat(u32 address, float value);
bool WriteAddrU8(LCSAddr id, u8 value);
bool WriteAddrU16(LCSAddr id, u16 value);
bool WriteAddrFloat(LCSAddr id, float value);

// --- Code words ---
// Read through Memory::Read_Instruction, which sees through JIT block markers.

enum class CodeWordState {
	Stock,
	Patched,
	Foreign,   // neither: another build, code not loaded yet, or someone else's patch
};
CodeWordState ReadCodeWord(const LCSCodeWord &word);

// Puts a word into the asked-for form, if it holds the game's op or ours (never otherwise).
// Returns whether it is now in that form. Writes, and invalidates the JIT, only on a change.
bool SetCodeWord(const LCSCodeWord &word, bool patched);

}  // namespace LCS
