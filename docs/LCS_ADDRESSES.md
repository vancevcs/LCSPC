# GTA: Liberty City Stories - address table notes

Every address in [Core/LCS/LCSAddresses.h](../Core/LCS/LCSAddresses.h), how it was found, how it
was confirmed, and what was ruled out. An entry is only as good as its "confirmed" line.

## Scope

Everything here was measured on exactly one build:

| | |
|---|---|
| disc | `ULUS10041`, `DISC_VERSION` **3.00**, `PSP_SYSTEM_VER` 2.60 |
| ISO | Redump "Grand Theft Auto - Liberty City Stories (USA) (En,Fr,De,Es,It) (v3.00)", 1,255,243,776 bytes, SHA-1 `826dba2aeb56007c0e916d517c71855cfd494f73` |
| decrypted EBOOT | 4,539,680 bytes, SHA-1 `0c8da52387fe6a709bdbbd0ed4a53b3fb6a8b1c9` (PPSSPP `DumpFileTypes = 1`) |
| module | `GTA3`, relocatable PRX (`e_type 0xffa0`) with section headers, loaded at **`0x08804000`** |
| `.text` | `0x08804000`-`0x08b0b5d4`; import stubs `0x08b0b5d4`-`0x08b0bd3c` |

`ULUS10041` also shipped as v1.05 and v2.00. `LCS::Init` refuses any `DISC_VERSION` other than 3.00,
and `LCS::NoteBootExecutable` refuses any EBOOT whose SHA-1 differs, so another revision stays
inactive with the reason shown in Debug -> LCS. `BOOT.BIN` on this disc is zero-filled, so the
decrypted executable only exists as PPSSPP's dump.

**The module base.** Two independent checks. ThirteenAG's plugin names many of its pointers after
the absolute address they had in its author's build (`sub_894D97C`, `ptr_8A25970`, ...), and those
patterns land on exactly those addresses here at base `0x08804000`. And `LCS::Tick` checks the
frame-cap instruction at `kLCSFrameCapSite` on its first tick and logs `LCS: module base 08804000
confirmed`.

## Tools

No emulator needed for anything in this file marked *static*:

- `python Tools/lcsimage.py` relocates the dumped EBOOT exactly as `ElfReader::LoadRelocations`
  does (127,640 relocations) and answers `find` (byte patterns), `disasm`, `callers` and `xref`
  (code that forms an address with `lui` + imm16) against it.
- `python Tools/lcsscm.py` maps every script opcode to its handler (below).
- Ghidra: project `%USERPROFILE%\lcs.gpr`, program `/ULUS10041_EBOOT.BIN`, imported with
  `-loader PspElfLoader -loader-imagebase 08804000` and fully analysed.

"Static" means read from the code that uses the value. It still needs a live check (the debugger
window's Addresses tab shows every entry's live value); an entry becomes *confirmed* when a write
or a run shows the game treating it that way.

## Script commands: range dispatch, not a table

VCS dispatches script commands through a pointer-to-member table. **LCS does not.** It does it the
GTA III way:

- `CRunningScript::ProcessOneCommand` at `0x08957a90`. Reads the opcode at `script+0x18` (the IP)
  through the script space pointer at `0x08b38d7c`, stores the NOT bit (`0x8000`) at
  `script+0x216`, masks with `0x7fff`, then walks a ladder of `slti` checks.
- 17 range functions, each a compiler switch (`addiu rX, a1, -BASE; sltiu; lw at, TABLE(at); jr at`):

| opcodes | function | | opcodes | function |
|---|---|---|---|---|
| 0000-0063 | `0x08958d50` | | 0389-03ec | `0x089e6000` |
| 0064-00c7 | `0x0895c784` | | 03ed-0450 | `0x088e423c` |
| 00c8-0130 | `0x0895f6d4` | | 0451-04b4 | `0x088e629c` |
| 0131-0194 | `0x089dc60c` | | 04b5-0518 | `0x08991b34` |
| 0195-01f8 | `0x089deca0` | | 0519-057c | `0x08994874` |
| 01f9-025c | `0x08a1dff0` | | 057d-05d8 | `0x08a31364` |
| 025d-02c0 | `0x08a20518` | | 05d9-063f | `0x0897c88c` |
| 02c1-0324 | `0x08a21e94` | | 0640-06a3 | `0x08862b04` |
| 0325-0388 | `0x089e1bd4` | | | |

Not every opcode in a range is in its jump table; `lcsscm.py` prints `(outside)` for those (80)
and `[unimplemented]` where the case is the switch's default (317).

- `CollectParameters` `0x08961bec` (this, &ip, count, `ScriptParams`); `StoreParameters`
  `0x08957eb8`. `ScriptParams` is `0x08dcf550`.
- `LCSSCM.INI`'s numbering is LCS's own and matches the code at the sites read so far: `0001`
  `wait` (collects 1, adds `CTimer::m_snTimeInMilliseconds`, stores the wake time at
  `script+0x210`), `022A` `get_player_health`, `022B` `get_char_health`, `02D3`
  `get_ground_z_for_3d_coord`.

## Entries

### PlayerInFocus `0x08b5e16c` (u8) - static

`CWorld::PlayerInFocus`. `FindPlayerPed` (`0x089d58b0`) and `FindPlayerVehicle` (`0x089d57b4`) both
start `lui a0, 0x8b6; lbu a0, -0x1e94(a0)` and use it to index `CWorld::Players`.

### PlayerBase `0x08b89b10` (pointer) - live

`CWorld::Players[0].m_pPed`. `CWorld::Players` is at `0x08b89b10` with a stride of `0x170`
(`FindPlayerPed` computes `i*0x180 - i*0x10`); `m_pPed` is at `+0`. The table reads player 0, not
`Players[PlayerInFocus]`: they are the same in single player, and multiplayer is out of scope.

- **Found:** `FindPlayerPed`, located with ThirteenAG's pattern (one match).
- **Also seen:** `get_player_health` (`022A`) computes the same `Players[i]` address.
- **Live (2026-10-07):** `0x0994b4f0` during the intro cutscene and in the first mission, so the
  ped is on the heap and the based entries are needed. Its matrix translation at `+0x30` read
  (1250.0, -1161.0, 11.86), a plausible Portland position: the entity layout matches VCS's.

### PlayerHealth `PlayerBase + 0x4b8` (float) - **confirmed**

- **Found:** `get_player_health` (`022A`, handler `0x08a1f3ac`): `lwc1 $f12, 0x4b8(ped)`,
  `trunc.w.s`, `StoreParameters`. `get_char_health` (`022B`, `0x08a1f418`) reads the same offset
  from a ped-pool lookup (`CPools::GetPedPool()` at `0x08b4c53c`, `GetAt` at `0x08afb978`).
- **Confirmed:** read 100.0; wrote 57.0 over the debugger and it still read 57.0 two seconds later,
  so it is the stored value, not a per-frame copy. Restored to 100.0.
- VCS's was `+0x4e4`.

### PlayerInVehicle `PlayerBase + 0x538` (u8), PlayerVehicle `PlayerBase + 0x534` (pointer) - static

`CPed::bInVehicle` and `CPed::m_pMyVehicle`. `FindPlayerVehicle` returns `ped+0x534` only when the
byte at `ped+0x538` is nonzero (ThirteenAG's pattern for it ends on that `lbu $a1, 0x538($a0)`). The
pointer is not cleared on foot, so `LCSState` reads it only while the flag is set.

`0x089d5808` (`FindPlayerEntity`) returns the vehicle when in one, else the ped.

### Vehicle type: `vehicle + 0x344` - static, meaning unconfirmed

`0x089d5860` calls `FindPlayerVehicle` and returns it only if `vehicle+0x344 == 2`, which is
re3's `FindPlayerTrain` (`VEHICLE_TYPE_TRAIN == 2` in GTA III). So `+0x344` is `m_vehType`. Which
numbers mean car, boat and bike in LCS is not measured yet; `LCSState::vehicleType` is the raw value.

### TimeInMs `0x08b5e144`, TimeStep `0x08b5e148`, FrameCounter `0x08b5e174` - live

All three read 0 through the boot art sequence (before `CTimer::Initialise`). In play: the frame
counter advances, `TimeStep` reads 1.6576-1.6791 (30 fps in 1/50 s units; VCS reads 1.668), and
skipping the intro cutscene restarts the counter from 0 (a new game re-initialises the timer).

- `CTimer::Initialise` at `0x0891b1a4` writes `m_snTimeInMilliseconds = 1` (`0x08b5e144`), zeroes
  `0x08b5e150` and `0x08b5e154`, sets the floats at `0x08b5e158` (time scale) and `0x08b5e148`
  (time step) to 1.0, and zeroes `0x08b5e174`. It then calls `sceKernelReferThreadProfiler` and
  `sceKernelReferGlobalProfiler`, the same pair VCS's `CTimer::Initialise` asks before falling back
  to the system clock ([vcs-30fps-lock]).
- `CTimer::Update` ends at `0x0891b66c`: it clamps the time step to 3.333 (`0x40555555`) and does
  `m_FrameCounter++` at `0x0891b648`. The only other writer of `0x08b5e174` is `0x088b2104`, in the
  save-game loader, which restores it with the rest of the timer block.
- `0x08b5e144` is ThirteenAG's `dword_8B5E144`, found there by the pattern
  `70 00 05 8E 23 20 85 00`, -8.

### FrameVblanks `0x08b56a90` - static

The frame wait at `0x08ab3368` loops on `sceDisplayWaitVblankStart` until the word at `0x08b56a90`
reaches 2 (`sltiu $a0, $a0, 2` at `0x08ab3378`, `kLCSFrameCapSite`), then zeroes it. The same
function then enqueues the frame's display list (`sceGeListEnQueue` at `0x08ab3428`).

### Camera

`TheCamera` is at **`0x08b833a0`**: `restore_camera` (`015F`), `set_camera_behind_player`
(`0378`) and `is_player_using_first_person_camera` (`05D0`) all pass it as `this`.

| field | offset | absolute (Cams[0]) | how found |
|---|---|---|---|
| `ActiveCam` (u8) | `TheCamera+0x7f` | `0x08b8341f` | 64 `lbu` uses; `0x08819708` multiplies it by 0x290 to index `Cams[]` |
| `Cams[3]` | `TheCamera+0x190`, stride **0x290** | `0x08b83530` | same site: `Cams[ActiveCam].Mode` is `TheCamera+0x1ac` |
| `Mode` (s16) | `CCam+0x1c` | `0x08b8354c` | `CCam::Process` (`0x088f0098`): `lh 0x1c(s0)`, 46-way switch, table `0x08b13da0` |
| `Alpha` (pitch) | `CCam+0xbc` | `0x08b835ec` | `Process_FollowPed`: clamped between two limits, rate at `+0xc0` |
| `FOV` | `CCam+0xc4` | `0x08b835f4` | `Process_FollowPed`: look rates scale by `FOV / 80` |
| `Beta` (yaw) | `CCam+0xcc` | `0x08b835fc` | `Process_FollowPed`: wrapped to [0, 2PI), rate at `+0xd0` |
| `Front` | `CCam+0x1a0` | | written from Alpha/Beta at the end of `Process_FollowPed` |
| `PlayerWeaponMode.Mode` (s16) | `TheCamera+0x954` | `0x08b83cf4` | `CCamera::Using1stPersonWeaponMode` (`0x088ee37c`) |

**Modes** (from `CCam::Process`'s jump table; names are III's, which the numbering follows):

| mode | process function | used for |
|---|---|---|
| 4 `FOLLOWPED` | `0x088f1a74` | on foot (seen live) |
| 18 `CAM_ON_A_STRING`, 22 `BEHINDBOAT` | `0x088f364c` (shared) | vehicles, boats |
| 3 `BEHINDCAR` | `0x088eb33c` | |
| 11 `SYPHON`, 28 `SYPHON_CRIM_IN_FRONT` | `0x088f07cc` (shared) | lock-on aim |
| 7, 8, 34, 45, 46 | `0x088f6f44` (shared) | first-person weapons; `Using1stPersonWeaponMode` tests exactly these |
| 16 `1STPERSON` | `0x088f55f0` | |
| 17 `FLYBY` | `0x088f6654` | cutscenes and the opening flyby (seen live) |
| 36 `FIGHT_CAM`, 38 `SNIPER_RUNABOUT`, 12, 14, 15, 23, 29, 32, 33, 35 | own functions | |

1, 2, 5, 6, 9, 10, 13, 19-21, 24-27, 30, 31, 37, 39-44 are not implemented (default case). VCS
groups its modes the same way (7/8/34/45/46/47 one aim response, 11/28 another).

**The look response** (`Process_FollowPed`, decompiled): stick X from `CPad::LookAroundLeftRight`
(`0x08a98c8c`), negated; stick Y from `CPad::LookAroundUpDown` (`0x08a98d8c`); both zeroed when
`FindPlayerTrain` (`0x089d5860`) is non-null. Target yaw rate = `sens^2 * |x| * x * FOV/80 / 14`,
pitch rate = `sens^2 * |y| * y * FOV/80 * 3/70`, each smoothed by `pow(k, TimeStep)` and clamped,
then integrated into Beta / Alpha. `sens` is the float at `0x08b3591c` (`LookSensitivity`). The last
Alpha and Beta are also kept in `0x08b35d78` / `0x08b35d7c`. Both `LookAround*` functions are what
ThirteenAG's dual-analog patch redirects (`34 00 80 14 ...`, calls at +0x84 and +0x13C).

**Confirmed live (2026-10-07):** on foot `Mode` read 4, `FOV` 70.0, `ActiveCam` 0. Writing `Beta`
from 6.21 to 4.21 over the debugger turned the view (camera behind Toni facing the car's rear ->
side-on to the garages) and it stayed, easing to 4.38 and holding there. So `CamYaw` is the
writable look input on foot, as `CameraYaw` is in VCS. In the cutscene and flyby, `Mode` read 17
and `FOV` 51.77. Not yet checked: `Alpha` by writing, `ActiveCam` ever being 1, and which fields
the vehicle mode (18) uses.

## Code addresses

| | address | expected op | found by |
|---|---|---|---|
| frame cap | `0x08ab3378` | `2c840002` `sltiu a0,a0,2` | ThirteenAG: `02 00 84 2C ? ? ? ? 00 00 00 00 ? ? ? ? 00 00 00 00` (the plugin NOPs the loop-back branch 20 bytes on) |
| `ProcessOneCommand` | `0x08957a90` | | `andi a1, a1, 0x7fff` followed by the `slti` ladder |
| `CollectParameters` | `0x08961bec` | | called by every handler with `ScriptParams` in `$a3` |
| `StoreParameters` | `0x08957eb8` | | called by the getters after writing `ScriptParams[0]` |
| `FindPlayerPed` | `0x089d58b0` | 2nd: `9084e16c` | ThirteenAG: `C0 29 04 00 21 30 A5 00 21 28 A6 00 00 21 04 00 23 20 A4 00 ? ? ? ? ? ? ? ? 21 20 85 00 08 00 E0 03`, -8 |
| `FindPlayerVehicle` | `0x089d57b4` | 2nd: `9084e16c` | same prefix, then `00 00 84 8C ? ? ? ? 00 00 00 00 38 05 85 90`, -8 |
| `CTimer::Initialise` | `0x0891b1a4` | | the only caller of `sceKernelReferThreadProfiler` |
| `cAudioManager::Service` | `0x08a9a5e4` | | ThirteenAG: `14 00 BF AF ? ? ? ? 25 80 80 00 0C 00 04 92`, -8 |

## ThirteenAG's patterns against this build

All three PSP plugins in `ThirteenAG/WidescreenFixesPack` (MIT; `GTALCS.PPSSPP.WidescreenFix`,
`.ImVehLM`, `.Project2DFX`) were run against the relocated image. Every pattern that the code
actually uses matches, and nearly all of them at exactly the offset the plugin's variable name
gives (`ptr_292A28` at `+0x292a28`, and so on). So the plugin author's build is this code.

Misses, none of which matter:

- `ptr_2955F4` asks for the *second* match of `26 00 04 86`; this image has one, at exactly
  `+0x2955f4`.
- `ptr_187228` / `ptr_187898` (zone and vehicle name scaling) match nothing: the pattern expects a
  wildcard where the code has `lui a0, 0x08b5`. Both sites are commented out in the plugin.
- A few names do not match their address (`ptr_2030C8` is at `+0x2af38c`, `ptr_130C4C` at
  `+0x1bd364`); the patterns themselves match once each, so these are stale names.

## Imports that matter

From the EBOOT's stub table, names resolved against PPSSPP's own HLE tables:

- **Call hosts:** `sceDisplayIsVblank`, `sceKernelPowerTick` and `sceGeListEnQueue` are all imported,
  the three VCS used. `sceGeListEnQueue` is called from the frame flip (`0x08ab3428`, once a frame).
  `sceDisplayIsVblank` has no direct `jal` callers. Whether each is called every frame, on the main
  thread, is not measured yet.
- `sceCtrlReadBufferPositive`: called from `0x0886ac9c`, `0x089c27dc`, `0x089c2814`, `0x08ad5744`.
- **Heap sizing:** LCS does not import `sceKernelTotalFreeMemSize`. It imports
  `sceKernelMaxFreeMemSize` (9 call sites) and `sceKernelAllocPartitionMemory` (one, `0x08aea424`).
  The VCS draw-distance finding (heap size from free memory) has to be re-checked against these.

## Not found yet

The weapon slot (ThirteenAG: `ped+0x6b8`, u8) and weapon array (`ped+0x590 + slot*0x1c`), the
aiming state, `CPad` (ThirteenAG's layout: `NewState` at `+0x02`, `OldState` at `+0x34`, `Mode` at
`+0x82`, `DisablePlayerControls` at `+0x86`) and the `is_button_pressed N` numbering, the vehicle
type numbers, and the menu/game state.
