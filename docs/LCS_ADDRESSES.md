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
- Decoding MAIN.SCM: a jump or call target is the file offset - 8 (this file and the docs quote file
  offsets); a mission's negative targets count from its `script_name` (`03A9`). `05AE` / `05AF` are
  LCS's calls with arguments, not in `LCSSCM.INI`'s shape: three raw bytes (arguments, results, the
  callee's locals base), the typed target, then the arguments typed (`0x2c6be`: call `0x2c07f` with
  1). `0126` `is_player_in_zone` takes its zone as an 8-byte name. With those two a linear
  disassembly of INTERIO, VIC1 and the load path stays in step.
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

**Live:** entering Vincenzo's car (Triangle, injected) set `ped+0x538` to 1 and `ped+0x534` to the
car, and the camera went to mode 18.

### Vehicle class and model - live

- `vehicle+0x344` is `m_vehType`: `0x089d5860` (re3's `FindPlayerTrain`) compares it with 2. Read 0
  in Vincenzo's car, so 0 is a car, as in VC.
- **The scripts classify by handling, not `m_vehType`.** `is_char_on_any_bike` (`047F`),
  `is_player_in_any_heli` (`04AF`), `is_player_in_any_plane` (`04B1`), `is_boat` (`02A1`) and
  `player driving_boat` (`04AD`) all call `0x088a08c4` (VC's `GetVehicleAppearance`):
  `vehicle+0x150` is `pHandling`, and `handling+0xd0 & 0xf0000` is 0 car (returns 1), `0x10000`
  bike (2), `0x20000` heli (3), `0x80000` boat (4), `0x40000` plane (5). `LCSState::vehicleClass`
  does the same test. Vincenzo's car: flags `0x8002`, so car.
- **Model index:** `entity+0x58` (s16). `is_car_model` (`013C`) compares it; the car read 152, the
  player ped 0. VCS's `+0x56` reads garbage here. The vehicle pool pointer is `0x08b4c540`
  (`GetAt` `0x08afb924`), next to the ped pool's `0x08b4c53c`.
- **Entity type:** `entity+0x44`, bits `0x0e` >> 1: ped 3 (read `0x9a446`), vehicle 2 (read
  `0x8198204`). The same encoding as VCS's, four bytes earlier. `Process_FollowPed` tests
  `(+0x44 & 0xe) == 6` for "the target is a ped".

### Weapons - static (Toni starts the first mission unarmed, so all slots read 0)

`get_current_char_weapon` (`0475`, `0x088e72bc`): `lb $a0, 0x6b8(ped)` (current slot), then
`lw 0x594(ped + slot*0x1c)`. So `CPed::m_weapons[]` starts at **`ped+0x594`**, stride 0x1c, 10
slots, and `m_currentWeapon` is the byte at **`ped+0x6b8`**. `get_char_weapon_in_slot` (`04BD`)
reads type at `0x578 + n*0x1c` and ammo at `0x584 + n*0x1c`, so script slot numbers are index + 1.
CWeapon: type `+0`, state `+4`, clip `+8`, total `+0xc`; this matches ThirteenAG's `ped + 0x590`
offsets shifted by 4. VCS: array `+0x58c`, slot byte `+0x789`.

### Pad - live

`CPad::GetPad` (`0x08a96408`): `Pads[]` at **`0x08b89da0`**, stride 0xc0. `NewState` at `+0x02`,
`OldState` at `+0x34`, 24 shorts each:

| `NewState+` | field | `is_button_pressed` N | | `NewState+` | field | N |
|---|---|---|---|---|---|---|
| 0x00 / 0x02 | left stick X / Y | 0 / 1 | | 0x18-0x1e | second d-pad | (none) |
| 0x04 / 0x06 | right stick X / Y | 2 / 3 | | 0x20 / 0x22 | Start / Select | 12 / 13 |
| 0x08 / 0x0a | L1 / L2 | 4 / 5 | | 0x24 / 0x26 | Square / Triangle | 14 / 15 |
| 0x0c / 0x0e | R1 / R2 | 6 / 7 | | 0x28 / 0x2a | Cross / Circle | 16 / 17 |
| 0x10-0x16 | d-pad Up, Down, Left, Right | 8-11 | | 0x2c / 0x2e | L3 / R3 | 18 / 19 |

- **Found:** `is_button_pressed` (`00E6`) calls `0x0887ba88(script, pad, N)`, a switch from N to
  these fields. So the script numbering is VCS's ([vcs-pad-button-enum]), and the struct also has
  ThirteenAG's four extra shorts. Both were right; the numbering skips them.
- **The extra shorts are a second d-pad.** Every stick and d-pad getter (`0x08a96c60`-`0x08a96d70`)
  tests the byte at **`0x08b56434`**: while it is set, the left stick reads `(Right - Left) / 2`
  and `(Down - Up) / 2` from the d-pad, and the d-pad getters read `+0x18..+0x1e` instead. This is
  VCS's `CameraInputMode` mechanism. Read 0 in play.
- `0x08b56433` (u8) inverts `LookAroundUpDown`.
- `CPad::Mode` at `+0x82` (u16): the controller configuration 0..3, the help text's `C0`..`C3`
  token sets (`LookAround*` read the stick only while it is below 4). **2 is the default**: RESET
  GAME SETTINGS sets 2 (`0x08ade44c`), a save carries its own (`0x088b221c`), and the fixtures and
  the saves on hand read 2. In a vehicle 2 and 3 take R for the handbrake (GetHandBrake
  `0x08a97e3c`; 0 and 1 Square + Cross) and L with the stick for the looks (`0x08a976cc` left,
  `0x08a978a0` right, `0x08a97a78` behind; 0 and 1 L, R, L + R); 1 and 3 swap the stick and the
  d-pad. Written 0 live, the input layer put 2 back (input.md "The game's own layout").
- `DisablePlayerControls` at `+0x86` (u16; read 160 while Toni was free to walk and enter the car,
  so it is probably a bit mask, not a counter).
- **Live:** holding D-Up over the debugger set `+0x12` (Pads[0]) to 255, D-Right `+0x18`, L `+0x0a`,
  Cross `+0x2a`. **Start set nothing** in `NewState` within 0.15 s; not explained yet.

### Heap sizing - static

LCS sizes its world from free memory, the lever that decided VCS's draw distance
([draw-distance.md] in the VCS docs):

- `MainMemoryManager` (`0x08aa4af8`): `sceKernelCreateFpl` of `sceKernelMaxFreeMemSize() -
  reserve`. The reserve is the word at `0x08b5666c`, 5.0 MB (`0x4e6666`) as shipped, raised to
  **12 MB when more than 22 MB is free**.
- `StreamingHeap` (`0x08a0b794`): what is left, minus 512 KB (minus `0x63800` under 8 MB free).
- Both have a fixed-size branch (`0xc9a400` and `0x47ac00`) behind the byte `0x08b56668`, which
  nothing writes.

So unlike VCS (one fixed reserve), LCS already has a large-memory path in the retail code.
Measured ([lcs/world-memory.md](lcs/world-memory.md)): on the retail partition the reserve reads
4.90 MB and the heap 4.48 MB; with a PSP-2000 partition 12.00 and 11.47 MB. The heap object is at
`0x08b89870` (size at +0, +0x14 moves with what is loaded).

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

`CTimer::Update` starts at `0x0891b328`. It takes the frame delta in ms (a float from the timer
object at `0x08e32db0`), converts it to ticks of 1/294912 s at `0x08b5e5b4`, adds
`ticks * TimeScale / 294912` to `m_snTimeInMilliseconds` **as an integer**, and sets
`TimeStep = ticks * TimeScale / 5898240`. It clamps `TimeStep` to [0.5, 3.333] and the ms advance
to 60 per frame. The fraction lost to the integer is what [lcs/frame-rate.md](lcs/frame-rate.md)
puts back.

### FrameVblanks `0x08b56a90` - live

The frame wait at `0x08ab3368` loops on `sceDisplayWaitVblankStart` until the word at `0x08b56a90`
reaches 2 (`sltiu $a0, $a0, 2` at `0x08ab3378`, `kLCSFrameCap`), then zeroes it. The same
function then enqueues the frame's display list (`sceGeListEnQueue` at `0x08ab3428`). Patching the
`sltiu` to 1 gives 60 fps; see [lcs/frame-rate.md](lcs/frame-rate.md).

### Camera

`TheCamera` is at **`0x08b833a0`**: `restore_camera` (`015F`), `set_camera_behind_player`
(`0378`) and `is_player_using_first_person_camera` (`05D0`) all pass it as `this`.

| field | offset | absolute (Cams[0]) | how found |
|---|---|---|---|
| `ActiveCam` (u8) | `TheCamera+0x7f` | `0x08b8341f` | 64 `lbu` uses; `0x08819708` multiplies it by 0x290 to index `Cams[]` |
| `Cams[3]` | `TheCamera+0x190`, stride **0x290** | `0x08b83530` | same site: `Cams[ActiveCam].Mode` is `TheCamera+0x1ac` |
| `Mode` (s16) | `CCam+0x1c` | `0x08b8354c` | `CCam::Process` (`0x088efc44`, switch at `0x088f0098`): `lh 0x1c(s0)`, 46-way switch, table `0x08b13da0` |
| `Alpha` (pitch) | `CCam+0xbc` | `0x08b835ec` | `Process_FollowPed`: clamped between two limits, rate at `+0xc0` |
| `FOV` | `CCam+0xc4` | `0x08b835f4` | `Process_FollowPed`: look rates scale by `FOV / 80` |
| `Beta` (yaw) | `CCam+0xcc` | `0x08b835fc` | `Process_FollowPed`: wrapped to [0, 2PI), rate at `+0xd0` |
| `Front` | `CCam+0x1a0` | | written from Alpha/Beta at the end of `Process_FollowPed`; `CCamera::Process` loads it into `sp+0x80` (`$fp`) |
| `Source`, `Up` | `CCam+0x1b0`, `CCam+0x1d0` | | loaded by `CCamera::Process` at `0x089012cc` into `sp+0x70` and `sp+0x90` (`$s7`) |
| car zoom | `TheCamera+0xcc` (float) | `0x08b8346c` | 1, 2, 3 as Select cycles the car distances |
| on-foot distance base | `TheCamera+0x13c` (float) | `0x08b834dc` | `Process_FollowPed` adds table `[1]` to it for the bottom of its distance band; 2.5 in play, 0.5 in the first mission's cutscene |
| TheCamera's matrix | `TheCamera+0x00` | | right `+0`, Front `+0x10`, Up `+0x20`, Source `+0x30`, copied from the three stack vectors at `0x0890232c` |
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

**What `Process_FollowPed` does to a written angle** (measured; [lcs/input.md](lcs/input.md) has
how mouse look switches each off):

| | address | effect |
|---|---|---|
| pitch spring | tuning table `0x08b359bc`, `[5]` (0.8; second set 0.9 at `0x08b35a0c`) | Alpha moves `1 - [5]^TimeStep` back to a default each frame; `[13]`/`[14]` (0.785 / 1.484) are the pitch limits |
| target and distance | same table, `[0]` 0.6, `[1]` 2.0, `[4]` 4.0 | the camera looks at the player's origin + `[0]` up; its distance band runs from `TheCamera+0x13c + [1]` up to at least `[4]`. Measured 4.1-4.5 in the open |
| yaw correction | `add.s $f12, $f12, $f13` at `0x088f2acc` | adds `(targetYaw - Beta) / TimeStep` to the yaw rate; undoes a written yaw |
| obstacle nudge | float `0x08b35d70` | `(i - 2) * 0.03` per ray hit, added to Beta, decays by 0.95 |
| yaw rate | `CCam+0xd0` | smoothed by `[8]`, clamped to `[9]` |
| BetaLast / AlphaLast | `0x08b35d7c` / `0x08b35d78` | snapped onto when within 0.0001 |

Write sites of `Cams[0].Beta`, from a log-only memory watch in the interpreter: `0x088f1e84`
(nudge), `0x088f2b68` (integration), `0x088f2b84` / `0x088f2b9c` (wrap), `0x088f2cc8` (the snap).

**Confirmed live (2026-10-07):** on foot `Mode` read 4, `FOV` 70.0, `ActiveCam` 0. Writing `Beta`
from 6.21 to 4.21 over the debugger turned the view (camera behind Toni facing the car's rear ->
side-on to the garages) and it stayed, easing to 4.38 and holding there. So `CamYaw` is the
writable look input on foot, as `CameraYaw` is in VCS. In the cutscene and flyby, `Mode` read 17
and `FOV` 51.77. `Alpha` by writing: positive looks up (held at 0.45 the view looked into the
trees, at -0.25 down onto the car roof), springing back unless the spring is off. In a car (mode 18)
a written `Beta` moved the camera side-on and settled part-way back (1.679 written, 1.346 kept).
Not yet checked: `ActiveCam` ever being 1.

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
| `CTimer::Update` | `0x0891b328` | | the writer of `m_FrameCounter` (`0x0891b648`); the carry patch is at `+0xc4` |
| `CPad::GetPad` | `0x08a96408` | | called with 0 all over the pad code |
| `CPad::LookAroundLeftRight` / `UpDown` | `0x08a98c8c` / `0x08a98d8c` | | ThirteenAG's camera pattern `34 00 80 14 ...` sits in the first |
| `GetVehicleAppearance` | `0x088a08c4` | | called by every "is in any bike/heli/plane/boat" handler |
| `CCam::Process` | `0x088efc44` (switch at `0x088f0098`) | | the 46-way switch on `Mode`; one caller, `0x08900fac` |
| `MainMemoryManager` / `StreamingHeap` setup | `0x08aa4af8` / `0x08a0b794` | | `sceKernelMaxFreeMemSize` callers that pass the result to `sceKernelCreateFpl` |
| `cAudioManager::Service` | `0x08a9a5e4` | | ThirteenAG: `14 00 BF AF ? ? ? ? 25 80 80 00 0C 00 04 92`, -8 |
| `CCamera::Process` | `0x08900c78` | | calls `CCam::Process` (`0x08900fac`); keeps Source/Front/Up at `sp+0x70/0x80/0x90` |
| chase camera: prepare hook | `0x08902174` | `c60c1be8` `lwc1 f12,0x1be8(s0)` | after the camera clip's LOS call (`0x08902128`, `0e231a3d`) and near-clip call; reads the shake |
| chase camera: matrix build | `0x08902318` | `0e23a8e2` `jal 0x088ea388` | the cross product (a0 out `sp+0x40`, a1 Up, a2 Front) that starts the matrix |
| `CWorld::ProcessLineOfSight` | `0x088c68f4` | 2nd: `310800ff` | the camera clip's call: (start, end, colpoint, entity, 1, 0, 0, 1, 0, 1, 1, 0, 0) |
| `RwCameraSetNearClipPlane` | `0x0887352c` | 2nd: `afb00010` | `CCamera::Process` sets 0.05 through it in the clip path; near is `RwCamera+0x80`; `Scene.camera` is `0x08b5e204` |
| model info table | `[0x08b4d3bc]`, count `0x08b5e140` | | `CBaseModelInfo *` per model; `CColModel *` at `+0x14`, its box at `+0x10`/`+0x20`; `CEntity` model index at `+0x58` (s16) |

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

### MenuActive `0x08b8ef51` (u8) - confirmed

`FrontEndMenuManager` is at `0x08b8ee20` and `m_bMenuActive` at `+0x131`: ThirteenAG's
`gMenuActivated` pattern (`B0 FF BD 27 ...`, second match, `0x089c2e84`) loads the base with
`lui`/`addiu` and reads `lbu 0x131`. Code always reaches it through the base register, so `xref`
finds nothing at the absolute address. Live: 0 in play, 1 with the pause menu up after Start, 0
again after a second Start (and after Backspace -> Circle through the input layer). The game frame
counter keeps advancing while it is up.

### The game's front end - live, [lcs/front-end.md](lcs/front-end.md) "The game's own pages"

`FrontEndMenuManager` (`0x08b8ee20`) as its input handler (`0x08adcad4`, decompiled) and draw
(`0x08adf724`, `0x08adfc40`) use it:

| field | meaning | confirmed |
|---|---|---|
| `+0x000` s32 | the tab the strip is on, 1..8 | live: 1 on opening, right stepped 1, 2, 3, 4, 5, 1 |
| `+0x018` s32 | the menu's fade, 255 when done; input waits for it | live: 150 -> 255 over 0.1 s after opening |
| `+0x141` u8 | input enabled: set once the d-pad and stick are released and the fade is done; every direction getter (`0x08ad954c`, `..95b0`, `..9614`, `..96fc`) returns 0 while clear | live: 0 for ~0.2 s after opening |
| `+0x470` f32 | the map page's half-size, PSP screen units; the zoom | live: 399.9 |
| `+0x474` / `+0x478` f32 | the map's centre, PSP screen units; the scroll keeps it within size - 130 of (240, 114) (`0x08adae64`), and moves the cursor at `+0x47c` / `+0x480` past that | written live: the map moved and stayed; 20 units of y moved the picture 80 px at 1080p |
| `+0x46b` u8 | the save request: `activate_save_menu` (03DD, `0x089e8310`) sets it, `has_save_game_finished` (03DE) is `!(MenuActive or it)` | written over the debugger: the save list opened on page 12 |
| `+0x564` s32 | the page, an index into the page table | live: map 7, brief 2, game 1, stats 0, the save list 12 |
| `+0x568` s32 | the selected entry | live: LOAD GAME 0, NEW GAME 1; YES 2 on the questions |
| `+0x574` s32 | the page before | |
| `+0x584` s32 | the page's fade-in, 255 when done | live: 0 / 100 / 200 -> 255 after each tab change |
| `+0x590` u8 | 1 once a page has focus, 0 while the tab strip has it (the strip is only drawn then) | live: Cross on the strip set it, Circle cleared it |

The page table is `0x08b316b4`, 0x116 a page: an 8-byte GXT key, then 15 entries of 0x12 - an action
(s16), an 8-byte key, a target page (+0xb), x, y, alignment. Pages: 0 stats, 1 game, 2 brief,
3 controls, 4 audio, 5 display, 6 language, 7 map, 9 new game question, 10 load question, 11 the
cheat warning, 12 the save list (the firmware's dialog over it), 13 the reset question, 14+
multiplayer. Tabs 1-8 show pages 7, 2, 1, 0, 3, 4, 5, 14 (the handler's switch; Spanish puts
controls last). The neighbour tables: `0x08b5905c` and, for language 4, `0x08b59134`; per tab
`[up, down, left, right, -, label]`, 0x18 bytes.

Start opens and closes the menu from anywhere in it (closing measured from inside a page).

The hints: `FUN_08adf0c0` draws every one, called once, from `0x08ae5114` (`jal`, `0e2b7c30`).

The cheat warning: the save request opens page 11 (`FET_WRN`) while the byte at **`0x08b56432`** is
set (every cheat sets it), else 12. The one caller that asks for 11 is `0x08ad9494`; its `beqz` at
**`0x08ad94b4`** (`10a00005`) made `b` (`10000005`) opens the save list always. Live: weapon set 1
set the byte 0 -> 1, and with the patch the request opened page 12.

STATS and BRIEF scroll by an offset each, f32: **`0x08b59cb4`** and **`0x08b59c40`**, read by their
draws (`0x08ae366c`, `0x08ae490c`) and moved 30 / speed a frame only while up or down is held. A
STATS line sits at `i * 15 + 196 - offset`, wrapped, so 136 puts the first at the top; a brief takes
20, and 11 more a wrapped line. Written live: 136 put the first line at the top, 166 two lines on.

`CMessages::PreviousBriefs` **`0x08dd1090`**: what BRIEF lists, 20 entries 32 bytes apart, the text
pointer first (0 empty) - the missions' dialogue since the game was loaded. Read live: all 0 after
a boot into a save; a cheat's brief showed on the page.

The intro's end: the game opens `/PSP_GAME/USRDIR/ENTRIES.DAT`, by sector
(`disc0:/sce_lbn0x6450_size0x30a8`), once per boot, the moment its two movies are over. Live: at
26 s untouched, 0.8 s after a skip.

### Missions passed - static and live

`register_mission_passed` (031D, `0x08a24740`): copies the 8-byte GXT key at the script's IP to
**`0x08b5e398`**, adds one to **`0x08b5e2c0`**, stores `TimeInMs` at `0x08b5e4b4`.
`register_oddjob_mission_passed` (059A, `0x08a32444`) adds one and leaves the key. The key is read
by the save code (`0x088b253c`); the counter is zeroed at `0x08845840` and isn't saved. Live: a key
and counter written together fired the auto-save; loading a save changed the key and not the count.

### Script globals - static, from MAIN.SCM

`GetPointerToScriptVariable` (`0x08956f38`): operand bytes `0x0c..0x6b` are locals, `0x6c..0xcb`
local arrays, **`0xcc..0xe5` globals** (index `(byte - 0xcc) << 8 | next`), `0xe6..` global arrays;
a global is `[0x08b38d7c] + index * 4`. `CollectParameters` (`0x08961bec`) types: 0 end, 1/2 zero,
3/4/5 floats with 1/2/3 bytes, 6/9 32-bit, 7 int8, 8 int16. The safe house's save routine
(`0x176d5` in MAIN.SCM): `$560` = 1, `$6` = 1, `$7..$9` = `$537..$539`, the ammo of 33 weapon types
into `$124..$156`, `$158` armed weapon, `$160` armour, `$161` money, pickups `$290..$293`, then 03DD.
The boot check (`0x6933`) runs the intro unless `$6` is 1; the load path (`0x48838`) gives the
weapons back from `$124..$156`. Live: a save made with these set loaded in the safe house with `$6`
1.

The safe houses (INTERIO, `0x2c225`): `$211` the house a save was made in (1 Portland, 2 Staunton,
3 Shoreside, -1 none), set before INTERIO's save (`0x2c721`, `0x2c966`, `0x2cc0e`) and back to -1 at
the exit (`0x2cd99`) or a new game (`0x6ec4`); `$1116` 1 while the player is inside one; `$1131..
$1133` each house's interior swapped in (`0xd7c4`: `swap_building`, then `set_area_visible 1`);
`$513..$515` the house is the player's (story progress, `0x13c73`; VIC1's pass sets `$513`). Their
save spots, written into `$537..$539` before the save: (1138.8, -242.4, 22.0), (272.1, -417.6,
60.1), (-845.3, 304.4, 41.0). **Every load** runs `0x47b6f`: `create_player` at `$537..$539`,
heading `$162` (180 since a new game), and with `$6` 1 a call of INTERIO's enter (`0x2c07f`) with
`$211` and `$1116` = 1. The enter is `display_radar 0` (`0x2c082`) and, for a house, its interior,
lighting and pickups; a house's exit (`0x2cdf4`) is `display_radar 1`. Only the main script's
new-game branch (`$6` 0, `0x6e53`) resets these, so a load keeps the saved ones. Live: a save with
`$211` -1 loaded with the radar hidden, `$1116` 1 and no interior; with 1, inside the Saint Mark's
house, and its exit marker put the radar back (front-end.md "The auto-save").

### Disc positions - static, from the ISO

`kLCSDiscPatches`: what `LCS::PatchDiscRead` stands in for as the disc is read. ENGLISH.GXT
(`/PSP_GAME/USRDIR/TEXT/`, 526,264 bytes) at **`0x42f8000`**, sector 34288: found by searching the
ISO for the whole file's bytes (one match), checked at run time by its `TABL` header. Live: the
patch's log line on the first read, and the keyboard's names in RAM after the boot.

### The radar and the area - static, written live

- `display_radar` (`0586`, `0x08a318a8`) writes both **`0x08b5e56c`** (the script's) and
  **`0x08b5e56d`** (the HUD's): 1 hides. The HUD (`0x0898c20c`) skips the radar while `0x08b5e56d`
  is set or the radar setting is off (`0x08ada6c0`), nothing else; the camera's scope modes (7, 8,
  34, 46) set `0x08b5e56d` alone (`0x088ee304`) and its restore clears it unless `0x08b5e56c` is set
  (`0x088ee370`). Neither is saved; two inits clear both (`0x08986834`, `0x08988818`). `0x089632c8`, by its
  shape a Lua binding (the multiplayer modes), reads and writes `0x08b5e56d` too.
- **`0x08b58df0`** (s32) the area, `CGame::currArea`: `set_area_visible` (`04C0`, `0x089923f4`)
  stores it; 1 inside a safe house. It **is** saved: the save header's `+0x0c` (`0x088b0a3c`), back
  at a load (`0x088b20bc`).
- The Ammu-Nations (INDAMMU, COMAMMU, PHIAMMU), VIC1 (`0x4ac4a`) and SALS2 use `display_radar` too.

### The game's own settings - static, written live

| | address | |
|---|---|---|
| brightness | `0x08b59c68` s32 | 128..384 in 32s; the menu's action 0x0d (`0x08ada240`); 288 as shipped. Written live: the world brightened |
| subtitles | `0x08b59c6c` u8 | action 0x0b |
| HUD mode | `0x08b59c4a` u8 | action 0x23. Written 0 live: the money, health and clock went |
| radar mode | `0x08b59c4c` s32 | action 0x22, 0..2: map and blips, blips only, off |
| radio volume | `0x08b59c7c` s32 + `0x08b89d95` u8 | action 0x0e: the preference, and the mixer's byte its setter (`0x088647e8` -> `0x088b6194`) writes |
| SFX volume | `0x08b59c78` s32 + `0x08b89d94` u8 | action 0x0f, setter `0x088647b0` |
| invert look | `0x08b56433` u8 | action 0x06; saved with the game |
| look speed | `0x08b3591c` float | 0.007, never written by the game |
| language | `0x08b59c9c` s32 | 0..4 English, French, German, Italian, Spanish; set at boot from `sceUtilityGetSystemParamInt(8)` (`0x089c0e28`), not saved |

A save restores the first five (`0x088b21a4`..).

### Free aim - live and static, [lcs/free-aim.md](lcs/free-aim.md)

| | where | how found |
|---|---|---|
| free-aim camera | mode 11, Process `0x088f07cc` (shared with 28) | entered with R held + d-pad down; read live |
| `m_pPointGunAt` | `ped+0x6bc` | mode 11 and FireInstantHit both read it; in free aim an object (type 4) the game keeps along Front from the pivot |
| free aiming | `ped+0xc9d` (u8) | mode 11 tests it; 1 in free aim, 0 in lock-on and on foot |
| entity type | `(entity+0x44 & 0xe) >> 1` | 1 building, 2 vehicle, 3 ped, 4 object (mode 11 and FireInstantHit test 6 and 0xc) |
| aim pivot | `0x088ebad8` | decompiled; the formula reproduces a point on the camera's centre line to 0.000 |
| turn rate toward the target | `0x08b35920` = 0.1 (x TimeStep) | decompiled |
| weapon info | `[[0x08b5e660] + (byte 0x08b56c7c ? 0 : 4)] + type x 0x70`; fire type +0, range +4 | GetWeaponInfo `0x0894d97c`; type 26 reads 1 / 90.0 live |
| CWeapon::FireInstantHit | `0x08857ef8` (weapon, shooter, source) | calls GetWeaponInfo and the raycast wrapper four times |
| raycast wrapper | `0x08852188` | forwards a0-a3 and the flags to ProcessLineOfSight; nine callers, all in CWeapon |
| the player's free-aim shot | `jal 0x08852188` at `0x08858638` (`0e214862`); `$t1` the shooter | log-only breakpoints on all nine call sites, interpreter: one hit per bullet, here only |
| crosshair position | `TheCamera+0x180` / `+0x184` (0.53 / 0.47) | mode 11 reads them; skipped in free aim |
| screen aspect | `0x08b30d68` (1.7778 live) | mode 11 divides the crosshair's y by it |

### The scoped weapons - live and static, [lcs/free-aim.md](lcs/free-aim.md) "The scope"

| | where | how found |
|---|---|---|
| the scope camera | modes 7 (sniper), 8 (rocket launcher), 34, 45, 46: one Process, `0x088f6f44` | CCam::Process's switch, decompiled; 7 and 8 read live with types 29 and 30 in hand |
| its look | Beta / Alpha += a smoothed rate (`CCam+0xd0` / `+0xc0`) from the stick, x FOV / 80; Alpha clamped to -1.562 / +1.047 (mode 45: -0.532) | decompiled; Beta -0.5 and Alpha 0.3 written live held for a second, the ped's heading followed |
| its Front | (cos Beta cos Alpha, sin Beta cos Alpha, sin Alpha), not mode 4's negated one | decompiled; read back live to 0.001 |
| zoom (modes 7 and 46) | Square (`CPad+0x26`, getter `0x08a989d8`) in, Cross (`+0x2a`, `0x08a98a18`) out, x or / (1 + 0.0255 TimeStep) a frame; FOV 15..70 (46: 3..70) | decompiled |
| zoom target | `0x08b35e0c` float | the FOV the scope eases to (`0x088ea7bc`) with neither zoom button held; written live 20.0: FOV 20.3 within 0.15 s |
| sniper slot | `m_weapons[8]`: types 28 and 29, range 100, fire type 1 | weapon info `+0x68` is the slot (28/29 read 8; 30, the rocket launcher, 7) |
| weapon info flags | `+0x6c`: the snipers' 0x28040 is also type 27's, a rifle that free-aims | so the slot, not a flag, tells the sniper from the rifles |

### The GPS route - live and static, [lcs/gps.md](lcs/gps.md)

| | where | how found |
|---|---|---|
| ThePaths | `[0x08b3980c]`, a `CPathFind` | get_closest_car_node (02C6, `0x08a220b0`) loads it for FindNodeClosestToCoors `0x0897572c` and the node-to-coordinates call `0x089777a8` |
| its arrays and counts | nodes `+0x00`, links `+0x08` (to `+0x0c`), 5519 / 2196 car / 3323 ped at `+0x14` / `+0x18` / `+0x1c`, 4604 car links `+0x24` | read live; the array sizes meet the next pointer exactly |
| a node | 20 bytes: x y z s16 x8 at `+4`, first link `+0xc`, link count low nibble of `+0x10`, 0x20 switched off | `0x089777a8` decompiled; the structure checks in gps.md |
| a link | u16, node in `& 0x3fff` | `0x089777a8` masks it; 11510 of 11510 resolve and are mutual |
| radar transform | origin `0x08b6e218`, sin `0x08b6e220`, cos `0x08b6e224`, range `0x08b5e7c0` | `0x08967b20` decompiled; origin read = the car, sin/cos a unit vector, range 96 |
| radar box | left 12, top 196, 65.8 (multiplayer: 170, 92) | the getters `0x08986fe4`, `0x08986f84`, `0x08987008`, `0x08986fb4`, on byte `0x08b56c7c` |
| radar drawn | `0x08b5e214` = 1, `0x08b83415` = 0, RADAR MODE not 2, `0x08b5e56d` = 0 | CHud::Draw `0x08988940` decompiled |
| blips | `0x08e4aa20`, 75 x 0x50: type `+4`, handle `+8`, position `+0xc`, in use `+0x33`, display `+0x3e`, sprite `+0x40` | SetCoordBlip `0x08967e40` and the blip draw `0x08969f8c` decompiled |
| the map's marker | a place blip (type 4), sprite 49 | placed one live: a new blip at the cursor |
| pools | vehicles `[0x08b4c540]` 0x6e0, peds `[0x08b4c53c]` 0xcb0, objects `[0x08b4c54c]` 0x220; {entries, flags, capacity} | GetAt `0x08afb924` / `0x08afb978` / `0x08afb8dc`; the player's car and ped resolve from their handles |

### The frame boundary's return address `0x08ab33a0`

The `jal sceKernelGetSystemTimeWide` at `0x08ab3398`, the first call after the frame wait loop:
the input layer writes the camera only when the syscall is called from here
(`kLCSFrameBoundaryReturn`). It replaced the wait's own `jal sceDisplayWaitVblankStart`
(`0x08ab3384`, return `0x08ab338c`), which the loop skips whenever the frame has already used up the
cap: `0x08ab3378` tests the vblank count before the first call. Stubs named live with the
debugger's `memory.disasm`: `0x08b0b8cc` sceDisplayWaitVblankStart, `0x08b0bbc4`
sceKernelGetSystemTimeWide, `0x08b0baec` sceKernelSysClock2USecWide.

## Not found yet

The aiming state (lock-on vs free aim), why Start doesn't show in `NewState`, the meaning of
`DisablePlayerControls`' bits, and a weapon in hand to confirm the weapon array live.
