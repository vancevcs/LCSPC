# GTA: Liberty City Stories "PC version" fork of PPSSPP

Turns PPSSPP into a dedicated PC-feeling front end for **GTA: Liberty City Stories** (PSP,
`ULUS10041`, revision **3.00** only - see docs/LCS_ADDRESSES.md "Scope").

**This file is rules and a map, not a log.** Findings go in docs/lcs/ (one file per subsystem) and
addresses in docs/LCS_ADDRESSES.md. Keep this file under ~150 lines. Upstream's AGENTS.md still
applies (tabs, line endings, build files, commit style).

The playbook is `../LCS_FORK_GUIDE.md` (workspace root). The VCS fork in `../../gtavcspc/ppsspp` is
the reference for every mechanism; its addresses and constants are places to look first, never
entries.

## Hard constraints
- **Zero behaviour change for every other game.** Gated on the `LCSOverhaul` compat flag, the disc
  ID, `DISC_VERSION`, and the EBOOT SHA-1. With any of them off, `LCS::Tick()` returns on its first
  line.
- **PSP memory is read on the emu thread only.**
- **No hardcoded addresses outside `Core/LCS/LCSAddresses.h`.**
- **It must build and run with any subset of addresses unset.**
- **Minimal diff to upstream.** Every upstream edit is a few lines marked "Fork-specific".
- **Don't push without asking.** Git identity for this repo is not decided yet: don't commit with
  the global (personal) identity until a repo-local one is set.

## Build and run
- `MSBuild Windows/PPSSPP.sln /t:PPSSPPWindows /p:Configuration=Release /p:Platform=x64 /m`
  (MSBuild via vswhere - AGENTS.md "Build and validation"). Exes land in the repo root. Check the
  exe's timestamp before trusting a measurement.
- New source file: the seven build files listed in docs/lcs/architecture.md.
- Live debugging: `--debugger=1337`; breakpoints need `CPUCore = 0`.
- Logging: `NOTICE_LOG` for anything read back from a run; WARN is dropped at level 2.
- `LoadPlugins = False` for ULUS10041 - a PSP plugin patching the same code looks like a wrong
  address. `LCS::Init` logs a NOTICE when plugins are on.
- The game is the ISO in the workspace root, or `../disc/` (extracted, bootable as a folder).
- Static RE needs no emulator: `python Tools/lcsimage.py disasm|find|callers|xref ...` and
  `python Tools/lcsscm.py`. Ghidra project: `%USERPROFILE%\lcs.gpr`.

## Code map
Core/LCS/ (no ImGui, UI, or renderer dependencies)
  LCSAddresses.h  the table + build fingerprint; code addresses carry a byte pattern and expected op
  LCSMemory       optional reads; a bad address is never fatal
  LCSState        per-tick decoded state, optional per field
  LCSGame         lifecycle + tick; the only entry point the rest of PPSSPP sees
  LCSMips.h       encoders for programs written into PSP memory
UI/ImDebugger/ImLCS   the "LCS" debugger window (Debug -> LCS)
Tools/lcsimage.py, Tools/lcsscm.py   offline EBOOT image and script-opcode map

Seams: init/shutdown in `__KernelInit`/`__KernelShutdown`; fingerprint in `__KernelLoadELFFromPtr`;
tick in `hleEnterVblank()` (not `__DisplayVblankBeginCallback`); compat flag in
Core/Compatibility.* + assets/compat.ini. Full table: docs/lcs/architecture.md.

## Working rules
- Measure, don't guess. Cheapest first: the game's Controls screen and GXT -> the script command
  handlers (`Tools/lcsscm.py`) -> offline disassembly -> live measurement.
- LCS dispatches script commands by opcode range (GTA III style), not through VCS's table.
- Confirm an address by writing to it. Derived values are overwritten at once; stored ones stick.
- Hooks: install from Tick after module load; verify the expected op via Read_Instruction; check a
  function's SECOND instruction (the first is a JIT block marker); InvalidateICache; reinstall after
  savestates.
- Game calls: only from inside a non-blocking syscall on the main thread.
- Only release input you pressed.
- Verify positively: log that a thing ran; a screenshot without the subject verifies nothing.

## Deep notes
| File | Covers | Read before |
|---|---|---|
| docs/LCS_ADDRESSES.md | every address, how found, how confirmed, what was ruled out | adding or using an address |
| docs/lcs/architecture.md | the gate, seams, files, build lists | adding a hook into PPSSPP |
