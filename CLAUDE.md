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
- **Anonymous commits, no remote.** The repo-local identity is `lcspc <lcspc@noreply.invalid>` with
  `user.useConfigOnly`; never commit or tag with the global (personal) identity. There is no GitHub
  remote; don't add one or push without asking.

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
  `python Tools/lcsscm.py`. Ghidra project: `%USERPROFILE%\lcs.gpr`; decompile headlessly with
  `../ghidra_scripts/DecompileAt.java` (usage in its header).
- Live: `--debugger=1337`, then `python Tools/lcsws.py read|write|watch|press`. Drive the game with
  button presses through it, never Windows keystrokes.

## Code map
Core/LCS/ (no ImGui, UI, or renderer dependencies)
  LCSAddresses.h  the table + build fingerprint; code addresses carry a byte pattern and expected op
  LCSMemory       optional reads; a bad address is never fatal
  LCSState        per-tick decoded state, optional per field
  LCSGame         lifecycle + tick; the only entry point the rest of PPSSPP sees
  LCSSettings     PSP/SYSTEM/lcs.ini, and the option table the front end is built from
  LCSFrameRate    60 fps (cap + millisecond carry)
  LCSInput        keyboard and mouse by context; mouse look on foot
  LCSChaseCam     the camera on foot and in vehicles, replaced inside CCamera::Process
  LCSFreeAim      mouse free aim: gun target + camera angles, and the player's shot down the reticle
  LCSFrontEnd     the bridge into the game's own menu: map/brief/stats, load, new game, auto-save,
                  the loading curtain, the game's own settings (LCSSaveDialog.h: the savedata peek)
  LCSMips.h       encoders for programs written into PSP memory
GPU/Common/LCSSmaa    SMAA over the world before the HUD (Vulkan)
UI/ImDebugger/ImLCS   the "LCS" debugger window (Debug -> LCS)
UI/LCSMenuScreen      the front end: the pause menu Escape raises over LCS (assets/lcs/)
UI/LCSDevInput        dev only: LCS_KEY_PIPE scripted keys and mouse (docs/lcs/input.md "Testing")
Tools/lcsimage.py, Tools/lcsscm.py   offline EBOOT image and script-opcode map
Tools/lcsmenuart.py, lcsmenusfx.py    the menu's backdrop and sounds, from the game's own files
Tools/lcswad.py, lcshd.py   the Android OBB (container, PVR) and the HD texture pipeline
Tools/lcstxd.py       texture tool ported from VCS (+ lcsrw, lcsmatch): PC packs + OBB, auto-assign
Tools/lcstxdweb.py    the OBB's HD textures in the browser pane, manual binds

Seams: init/shutdown in `__KernelInit`/`__KernelShutdown`; fingerprint in `__KernelLoadELFFromPtr`;
tick in `hleEnterVblank()` (not `__DisplayVblankBeginCallback`); keys and mouse in `NativeKey` /
`NativeMouseDelta`; the frame boundary in `sceKernelGetSystemTimeWide`; by-address hooks in
`Core/HLE/ReplaceTables.*`; the memory partition in `InitMemorySizeForGame`; SMAA in
`DrawEngineVulkan::Flush`; the pause screen in `CreatePauseScreen` (EmuScreen), the curtain in
`DrawLCSOverlay` (EmuScreen::renderUI); the savedata dialog in `LCS::PeekSaveDialog`
(sceUtility); compat flag in Core/Compatibility.* + assets/compat.ini. Full table:
docs/lcs/architecture.md.

## Working rules
- Measure, don't guess. Cheapest first: the game's Controls screen and GXT -> the script command
  handlers (`Tools/lcsscm.py`) -> offline disassembly -> live measurement.
- LCS dispatches script commands by opcode range (GTA III style), not through VCS's table.
- Confirm an address by writing to it. Derived values are overwritten at once; stored ones stick.
- Hooks: install from Tick after module load; verify the expected op via Read_Instruction; check a
  function's SECOND instruction (the first is a JIT block marker); InvalidateICache; reinstall after
  savestates.
- Game calls: only from inside a non-blocking syscall on the main thread.
- State the game reads and writes back every frame (the camera's angles) is written at the frame
  boundary (`LCS::OnFrameBoundary`), not the vblank tick, or a write can land mid-frame and be lost.
  Not in the frame wait itself: a frame that has used up the cap skips it.
- Savestates keep plain code patches but not replacement (`HOOKENTER`) ops: re-check every tick
  and put hooks back (chase-camera.md). Test fixtures: the pipe's `savestate` / `loadstate`.
- Test input with `LCS_KEY_PIPE` and "Use mouse control" off; give queued lines time to run (the
  pipe ticks per host frame: 30 Hz at 30 fps). docs/lcs/input.md "Testing" has the traps.
- Only release input you pressed.
- Verify positively: log that a thing ran; a screenshot without the subject verifies nothing.

## Deep notes
| File | Covers | Read before |
|---|---|---|
| docs/LCS_ADDRESSES.md | every address, how found, how confirmed, what was ruled out | adding or using an address |
| docs/lcs/architecture.md | the gate, seams, files, build lists, how to patch code | adding a hook into PPSSPP |
| docs/lcs/frame-rate.md | 60 fps: what it patches, measurements, what's unchecked | touching CTimer or the frame wait |
| docs/lcs/input.md | the game's layout, the bindings, key ownership, the on-foot camera takeover, testing | touching input or the camera |
| docs/lcs/chase-camera.md | the chase camera: hook sites, the program, on foot / vehicle framing, savestates, the fixtures | touching the camera or adding a hook |
| docs/lcs/free-aim.md | the game's free aim, the gun target, the fire hook, measurements, the armed fixtures | touching aiming or shooting |
| docs/lcs/world-memory.md | the PSP-2000 partition and the game's own larger streaming heap | touching memory size or draw distance |
| docs/lcs/anti-aliasing.md | SMAA, where it runs, the world drawn in pieces, the debug views | touching LCSSmaa or the draw engine seam |
| docs/lcs/front-end.md | the pause menu, the game's own pages, load / new game / auto-save, the curtain, the game's settings | touching the menu, a setting, or anything that drives the game's menu |
| docs/lcs/hd-textures.md | the Android OBB format, matching dumps to HD art, the brightness match, DDS | touching the texture pack or the Android files |
