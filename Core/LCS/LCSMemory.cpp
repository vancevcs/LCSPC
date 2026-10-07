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

#include <cstring>

#include "Core/MemMap.h"
#include "Core/LCS/LCSMemory.h"

namespace LCS {

// Before a game boots, Memory::base is null and the unchecked accessors would dereference it. The
// debugger window can stay open across a game shutdown, so this matters.
static bool MemoryReady() {
	return Memory::base != nullptr;
}

std::optional<u8> ReadU8(u32 address) {
	if (!MemoryReady() || !Memory::IsValidRange(address, sizeof(u8))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U8(address);
}

std::optional<u16> ReadU16(u32 address) {
	if (!MemoryReady() || !Memory::IsValid2AlignedAddress(address) || !Memory::IsValidRange(address, sizeof(u16))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U16(address);
}

std::optional<u32> ReadU32(u32 address) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(u32))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_U32(address);
}

std::optional<s32> ReadS32(u32 address) {
	const std::optional<u32> value = ReadU32(address);
	if (!value) {
		return std::nullopt;
	}
	s32 result;
	memcpy(&result, &*value, sizeof(result));
	return result;
}

std::optional<float> ReadFloat(u32 address) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(float))) {
		return std::nullopt;
	}
	return Memory::ReadUnchecked_Float(address);
}

std::optional<u32> ReadPointer(u32 address) {
	const std::optional<u32> ptr = ReadU32(address);
	if (!ptr || *ptr == 0 || !Memory::IsValidAddress(*ptr)) {
		return std::nullopt;
	}
	return ptr;
}

std::optional<u32> ResolveAddr(LCSAddr id) {
	const LCSAddrEntry &entry = LookupAddr(id);
	if (entry.base == kNoBase) {
		if (entry.address == kUnsetAddress) {
			return std::nullopt;
		}
		return entry.address;
	}

	// One level of indirection. A base that is itself based is refused rather than misread.
	const LCSAddrEntry &baseEntry = LookupAddr(entry.base);
	if (baseEntry.base != kNoBase || baseEntry.address == kUnsetAddress) {
		return std::nullopt;
	}
	const std::optional<u32> basePtr = ReadPointer(baseEntry.address);
	if (!basePtr) {
		return std::nullopt;
	}
	return *basePtr + entry.address;
}

std::optional<u32> ReadAddrU32(LCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadU32(*addr) : std::nullopt;
}

std::optional<s32> ReadAddrS32(LCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadS32(*addr) : std::nullopt;
}

std::optional<float> ReadAddrFloat(LCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? ReadFloat(*addr) : std::nullopt;
}

std::optional<u32> ReadAddrAsU32(LCSAddr id) {
	const std::optional<u32> addr = ResolveAddr(id);
	if (!addr) {
		return std::nullopt;
	}
	switch (LookupAddr(id).type) {
	case LCSAddrType::U8:
	{
		const std::optional<u8> value = ReadU8(*addr);
		return value ? std::optional<u32>(*value) : std::nullopt;
	}
	case LCSAddrType::U16:
	{
		const std::optional<u16> value = ReadU16(*addr);
		return value ? std::optional<u32>(*value) : std::nullopt;
	}
	case LCSAddrType::U32:
	case LCSAddrType::S32:
		return ReadU32(*addr);
	default:
		return std::nullopt;
	}
}

std::optional<bool> ReadAddrBool(LCSAddr id) {
	const std::optional<u32> value = ReadAddrAsU32(id);
	if (!value) {
		return std::nullopt;
	}
	return *value != 0;
}

bool WriteU8(u32 address, u8 value) {
	if (!MemoryReady() || !Memory::IsValidRange(address, sizeof(u8))) {
		return false;
	}
	Memory::WriteUnchecked_U8(value, address);
	return true;
}

bool WriteU16(u32 address, u16 value) {
	if (!MemoryReady() || !Memory::IsValid2AlignedAddress(address) || !Memory::IsValidRange(address, sizeof(u16))) {
		return false;
	}
	Memory::WriteUnchecked_U16(value, address);
	return true;
}

bool WriteU32(u32 address, u32 value) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(u32))) {
		return false;
	}
	Memory::WriteUnchecked_U32(value, address);
	return true;
}

bool WriteFloat(u32 address, float value) {
	if (!MemoryReady() || !Memory::IsValid4AlignedRange(address, sizeof(float))) {
		return false;
	}
	Memory::WriteUnchecked_Float(value, address);
	return true;
}

bool WriteAddrU8(LCSAddr id, u8 value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteU8(*addr, value) : false;
}

bool WriteAddrU16(LCSAddr id, u16 value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteU16(*addr, value) : false;
}

bool WriteAddrFloat(LCSAddr id, float value) {
	const std::optional<u32> addr = ResolveAddr(id);
	return addr ? WriteFloat(*addr, value) : false;
}

}  // namespace LCS
