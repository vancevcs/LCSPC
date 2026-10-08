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
| `LCS::WantsExtraWorldMemory()` | `InitMemorySizeForGame()` in `Core/PSPLoaders.cpp` | the partition is sized while the ISO loads, before `LCS::Init`; it reads `lcs.ini` itself (world-memory.md) |
| `LCSSmaa` | `DrawEngineVulkan` (constructor, destructor, `DestroyDeviceObjects`, top of `Flush`) and `GPU_Vulkan::BeginHostFrame` | the seam between the world and the HUD is a flush, and the pass binds targets of its own (anti-aliasing.md) |
| Debug -> LCS | `UI/ImDebugger/ImDebugger.cpp/.h` (menu item, draw call, `lcsOpen` ini key) | |
| `LCS::HandleHostKey()` | `NativeKey()`, in the `passKeyThrough` branch, before `g_controlMapper.Key` | a claimed key must not also reach the mapper |
| `LCS::HandleMouseDelta()` | `NativeMouseDelta()`, after the `bMouseControl` check, skipped while ImGui wants the mouse | mouse look only with PPSSPP's mouse control on |
| `LCS::OnFrameBoundary()` | `sceKernelGetSystemTimeWide()`, with `$ra` | the game calls it right after its frame wait, every frame: between frames, the only safe moment to write the camera. Not `sceDisplayWaitVblankStart`, which a frame that has used up the cap skips (input.md) |
| `lcs_camera_prepare` / `lcs_camera_finish`, `lcs_fire_raycast` / `lcs_fire_raycast_done` | `Core/HLE/ReplaceTables.cpp` entries, plus `GetReplacementFuncIndexByName` / `WriteReplaceInstructionAt` | `REPFLAG_HOOKENTER` hooks installed by address, never by hash, so no other game can match them (chase-camera.md, free-aim.md) |
| `LCSDevKeyPipeTick()` | `NativeFrame()`, after `g_OSD.Update` | dev only, `LCS_KEY_PIPE` |
| `CreatePauseScreen()` | the three `GamePauseScreen` pushes in `UI/EmuScreen.cpp` | the LCS front end over LCS, PPSSPP's pause screen for everything else (front-end.md) |
| `FontFamily::Display` | `Common/Render/Text/Font.h`, a row in `g_fontDescs` (`draw_text.cpp`) | Pricedown for the menu at the same time as the UI sans: one face per family, so a third family |
| `UISound::LCS_*` | `Common/UI/Root.h`, `LoadDefaultSample` in `UI/BackgroundAudio.cpp` | the game's own menu sounds, added rather than swapped in so PPSSPP's menus never play them |

## Files

```
Core/LCS/            no ImGui, UI or renderer dependencies
  LCSAddresses.h     the table, the build fingerprint, code addresses with their expected ops
  LCSMemory          optional reads and checked writes; a bad address is never fatal
  LCSState           per-tick decoded state, optional per field
  LCSGame            lifecycle and tick: the only entry point the rest of PPSSPP calls
  LCSSettings        PSP/SYSTEM/lcs.ini, loaded when the layer activates; the front end's option table
  LCSFrameRate       60 fps: frame cap + millisecond carry, re-checked every tick (frame-rate.md)
  LCSInput           keyboard and mouse by context; mouse look on foot (input.md)
  LCSChaseCam        the camera on foot and in vehicles: two hooks and a program in CCamera::Process (chase-camera.md)
  LCSFreeAim         mouse free aim: the gun target, the camera's angles and a hook on the player's shot (free-aim.md)
  LCSMips.h          encoders for programs written into PSP memory
GPU/Common/LCSSmaa  SMAA over the world before the HUD, Vulkan only (anti-aliasing.md)
UI/ImDebugger/ImLCS  the Debug -> LCS window (state, table, input, aim, PSP pad, camera, scratchpad)
UI/LCSMenuScreen     the front end: the pause menu over LCS, its pages, the cheats (front-end.md)
UI/LCSDevInput       dev only: LCS_KEY_PIPE scripted keys and mouse
assets/lcs/          the menu's backdrop, Pricedown and sounds, read through g_VFS
Tools/lcsmenuart.py  the backdrop, cropped from the game's pause-menu texture
Tools/lcsmenusfx.py  the menu's three sounds, from the disc's front-end sound bank
Tools/lcsimage.py    the EBOOT relocated into a RAM image: find, disasm, callers, xref
Tools/lcsscm.py      script opcode -> handler address
Tools/lcsws.py       WebSocket debugger client: read, write, watch, press
```

Tick order: base check (first tick only) -> `FrameRateTick` -> `InstallChaseCam` ->
`InstallFreeAim` -> `UpdateSharedState` -> `InputTick` (context, buttons, stick, the pad panel). The mouse is applied from
`OnFrameBoundary`, not the tick; the chase camera's hooks run inside the game's camera update.

## Threads

`HandleHostKey` and `HandleMouseDelta` run on the UI thread and only record, under `LCSInput`'s
mutex; the context they read is an atomic `InputTick` publishes. Everything that touches PSP memory
or sceCtrl runs on the emu thread: `Tick`, `OnFrameBoundary` (an HLE syscall), the chase
camera's hooks (replacement functions), and the ImGui window.

## Patching code

`ReadCodeWord` / `SetCodeWord` in `LCSMemory` are the one way to change an instruction: read
through `Memory::Read_Instruction` (it sees through JIT block markers), never write a word that
holds neither the game's op nor ours, and to write, `InvalidateICacheRangeImmediate` first
(restores the original op over a marker), then the raw read/write, then
`InvalidateICacheRangeDeferred`. A set of words that only works together (the frame rate's six) is
checked in full before any is written. Upstream renamed `InvalidateICache`, which the VCS fork and
the guide still call it.

## When to write game state

From the vblank tick, a write can land mid-frame while the game holds the value in a register, and
be lost when it stores its own result. Anything the game reads and writes back every frame (the
camera's angles) is written at the frame boundary instead (`OnFrameBoundary`). Buttons and the stick go
through sceCtrl, which the game samples once a frame, so the tick is fine for those.

A new source file goes in all seven build files: `Core/CMakeLists.txt`, `Core/Core.vcxproj` and
`.filters`, `UWP/CoreUWP/CoreUWP.vcxproj` and `.filters`, `android/jni/Android.mk`,
`libretro/Makefile.common` (Core only). UI files: `UI/CMakeLists.txt`, `UI/UI.vcxproj` and
`.filters`, `UWP/UI_UWP/UI_UWP.vcxproj` and `.filters`, `android/jni/Android.mk`. GPU files:
`GPU/CMakeLists.txt`, `GPU/GPU.vcxproj` and `.filters`, `UWP/GPU_UWP/GPU_UWP.vcxproj` and
`.filters`, `android/jni/Android.mk`, `libretro/Makefile.common`.

## Seams not added yet

From the guide, added only when a feature needs them: pad axes in `NativeAxis()`, call hosts in
non-blocking syscalls (game calls), `__KernelLoadExec` (boot-time patches), `ISOFileSystem::ReadFile`
(disc file substitution), frameskip in `sceDisplay.cpp` (only if 60 fps turns out to drop frames
somewhere), and the renderer.
