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

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"

// Lifecycle and per-frame driver for the GTA: Liberty City Stories layer. The only part of
// Core/LCS the rest of PPSSPP calls:
//
//   __KernelInit()           -> LCS::Init()                (Core/HLE/sceKernel.cpp)
//   __KernelLoadELFFromPtr() -> LCS::NoteBootExecutable()  (Core/HLE/sceKernelModule.cpp)
//   hleEnterVblank()         -> LCS::Tick()                (Core/HLE/sceDisplay.cpp)
//   __KernelShutdown()       -> LCS::Shutdown()            (Core/HLE/sceKernel.cpp)
//
// Active only with the LCSOverhaul compat flag, disc ULUS10041, DISC_VERSION 3.00, the EBOOT hash
// in LCSAddresses.h, and the module at its expected base. Otherwise Tick returns on its first line
// and nothing else here runs.

namespace LCS {

// Once per boot, before the EBOOT loads. Decides whether this boot is a candidate.
void Init();

// The decrypted boot executable, before it is loaded. Hashes it if this boot is a candidate, and
// activates on a match.
void NoteBootExecutable(const u8 *elf, size_t size);

// Before __CtrlShutdown. Safe when inactive.
void Shutdown();

// Once per vblank, on the emu thread.
void Tick();

bool IsActive();

// For the debugger window and the log.
const std::string &GetDiscID();
const std::string &GetDiscVersion();
// Why the layer is inactive, or empty when active.
const std::string &InactiveReason();
// Hex SHA-1 of the boot executable, empty until one has been hashed.
const std::string &EbootHash();
u64 GetTickCount();

}  // namespace LCS
