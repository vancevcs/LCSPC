# LCS layer: architecture

How the GTA: Liberty City Stories layer sits inside PPSSPP, and why each seam is where it is. The
playbook this follows is `LCS_FORK_GUIDE.md` in the workspace root (`Dev/gtalcspc/`); the VCS fork
in `Dev/gtavcspc/ppsspp` is the reference implementation of every mechanism.

## The gate

`LCS::IsActive()` needs all of:

1. the `LCSOverhaul` compat flag ([assets/compat.ini](../../assets/compat.ini), `ULUS10041` only),
2. disc ID `ULUS10041`,
3. `DISC_VERSION` `3.00` (the disc ID is shared by v1.05, v2.00 and v3.00),
4. the decrypted EBOOT's SHA-1 equal to `kLCSEbootSha1`,
5. on the first tick, `kLCSFrameCapOp` at `kLCSFrameCapSite` (checks the module base, and that
   nothing has patched the frame wait).

1-3 are decided in `LCS::Init`, 4 in `LCS::NoteBootExecutable`, 5 in `LCS::Tick`. Every other game
fails 1, silently. A boot that passes 1 but fails a later step logs one `LCS: inactive - <reason>`
NOTICE line, and Debug -> LCS shows the reason.

## Seams

| seam | where | why there |
|---|---|---|
| `LCS::Init()` | `__KernelInit()`, after `__PPGeInit` | runs per boot, after `PARAM.SFO` is read and before the EBOOT loads |
| `LCS::NoteBootExecutable()` | `__KernelLoadELFFromPtr()`, after the ELF magic check, for `*BOOT.BIN` | the only point where the decrypted executable exists as bytes. Hashing it there gives the same SHA-1 as the `DumpFileTypes` dump, so it can be checked offline with `certutil` |
| `LCS::Tick()` | `hleEnterVblank()`, after `UpdateAutoMovements` | once per vblank on the emu thread, where PSP memory is safe to read. Not `__DisplayVblankBeginCallback`, which only fires for threads waiting in `sceDisplayWaitVblankCB` |
| `LCS::Shutdown()` | `__KernelShutdown()`, before `__CtrlShutdown` | so held buttons can be released while sceCtrl is alive |
| compat flag | `Core/Compatibility.h/.cpp`, `assets/compat.ini` | |
| Debug -> LCS | `UI/ImDebugger/ImDebugger.cpp/.h` (menu item, draw call, `lcsOpen` ini key) | |

## Files

```
Core/LCS/            no ImGui, UI or renderer dependencies
  LCSAddresses.h     the table, the build fingerprint, code addresses with their expected ops
  LCSMemory          optional reads and checked writes; a bad address is never fatal
  LCSState           per-tick decoded state, optional per field
  LCSGame            lifecycle and tick: the only entry point the rest of PPSSPP calls
  LCSSettings        PSP/SYSTEM/lcs.ini, loaded when the layer activates
  LCSFrameRate       60 fps: frame cap + millisecond carry, re-checked every tick (frame-rate.md)
  LCSMips.h          encoders for programs written into PSP memory
UI/ImDebugger/ImLCS  the Debug -> LCS window (state, table, scratchpad, the 60 fps switch)
Tools/lcsimage.py    the EBOOT relocated into a RAM image: find, disasm, callers, xref
Tools/lcsscm.py      script opcode -> handler address
Tools/lcsws.py       WebSocket debugger client: read, write, watch, press
```

Tick order: base check (first tick only) -> `FrameRateTick` -> `UpdateSharedState`.

## Patching code

`LCSFrameRate.cpp` has the pattern every code patch should follow until a second user justifies a
shared helper: check every word of a set through `Memory::Read_Instruction` (it sees through JIT
block markers) and write none unless all hold either the game's op or ours; to write,
`InvalidateICacheRangeImmediate` first (restores the original op over a marker), then the raw
read/write, then `InvalidateICacheRangeDeferred`. Upstream renamed `InvalidateICache`, which the VCS
fork and the guide still call it.

A new source file goes in all seven build files: `Core/CMakeLists.txt`, `Core/Core.vcxproj` and
`.filters`, `UWP/CoreUWP/CoreUWP.vcxproj` and `.filters`, `android/jni/Android.mk`,
`libretro/Makefile.common` (Core only). UI files: `UI/CMakeLists.txt`, `UI/UI.vcxproj` and
`.filters`, `UWP/UI_UWP/UI_UWP.vcxproj` and `.filters`, `android/jni/Android.mk`.

## Seams not added yet

From the guide, added only when a feature needs them: host keys / axes / mouse in `UI/NativeApp.cpp`
(input), by-address `REPFLAG_HOOKENTER` install in `Core/HLE/ReplaceTables.*` (hooks), call hosts in
non-blocking syscalls (game calls), `__KernelLoadExec` (boot-time patches), `ISOFileSystem::ReadFile`
(disc file substitution), frameskip in `sceDisplay.cpp` (only if 60 fps turns out to drop frames
somewhere), and the renderer.
