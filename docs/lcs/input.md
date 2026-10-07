# LCS input: keyboard and mouse

`Core/LCS/LCSInput.cpp`, with the seams in `UI/NativeApp.cpp` (`NativeKey`, `NativeMouseDelta`) and
`Core/HLE/sceDisplay.cpp` (`sceDisplayWaitVblankStart`). Settings in `PSP/SYSTEM/lcs.ini` and the
Input tab of Debug -> LCS: `KeyboardMouse`, `MouseLook`, `MouseSensitivity` (rad per count,
default 0.0025), `InvertMouseY`.

## The game's own layout

Read before any code, from the help text: `ENGLISH.GXT`'s `C0*` entries are the control tokens the
tutorial lines use (`~k~ ~PDSPR~`), one set per control configuration (C0-C3). Configuration 1
(`C0`), the default:

| on foot | | in a vehicle | |
|---|---|---|---|
| sprint | Cross (`PDSPR`) | accelerate | Cross (`VEACC`) |
| jump | Square (code: `0x08a98308`) | brake / reverse | Square (`VEBRK`) |
| enter / hang up | Triangle (`VEEE`) | handbrake | Square + Cross (`VEHB`) |
| lock on | R (`PDLT`) | horn | D-Down (`VEHN`) |
| fire | Circle (`PDFW`) | radio | D-Left / D-Right (`VECRS`) |
| change weapon | D-Left / D-Right (`PDCWE`) | look left / right / behind | L / R / L + R (`VELL`, `VELR`, `VELB`) |
| change target | D-Left / D-Right while locked on (`PDCTL`) | drive-by | hold L or R, fire with Circle (`HELP19A`) |
| free aim | tap D-Down while locked on (`FREE1`) | bike weight, water cannon | stick up / down (`VEWEI`, `VEWEU`) |
| look around | hold L, use the stick (`PDLO1`) | | |
| look behind | L + R (`PDBAK`) | | |
| answer phone, swap pickup | L (`ANS`, `PUCF`) | | |
| camera mode | Select (`CVEIW`) | | |
| side missions | D-Up (`TGSUB`) | | |

The pause menu: D-pad moves, Cross selects, Circle backs out. The `CPad` getters agree with all of
this (`0x08a9802c` accelerate reads Cross, `0x08a97eb4` brake reads Square, `0x08a97f90` fire reads
Circle, `0x08a98240` target reads R, ...).

## Bindings

The table is `kKeyMappings` in `LCSInput.cpp`; keys follow the VCS fork's where the action exists
in both games. Debug -> LCS -> Input lists it.

| key | on foot | in a vehicle | in the pause menu |
|---|---|---|---|
| W A S D | move (the stick) | W accelerate, S brake / reverse, A D steer | move |
| Up / Down arrows | (PPSSPP's d-pad) | lean a bike, aim a water cannon | (PPSSPP's d-pad) |
| Left mouse, Left Ctrl | fire | fire | |
| Right mouse | lock on | | |
| Space | jump | handbrake | select |
| Left Shift | sprint | (claimed, does nothing: PPSSPP's rapid fire) | |
| F | enter vehicle / hang up | exit / hang up | |
| Q / E | previous / next weapon (target when locked on) | look left / right (hold for drive-by) | |
| mouse wheel | next / previous weapon | next / previous station | |
| R / T | | previous / next station | |
| H | | horn | |
| C | look behind | look behind | |
| X | free aim (tap while locked on) | | |
| Tab | answer phone, swap for a pickup | answer phone | |
| V | camera mode | camera mode | |
| G | side missions on / off | side missions on / off | |
| Enter | pause menu | pause menu | select |
| Backspace | | | back |

Escape, F-keys and everything not in the table stay PPSSPP's.

## How it works

- **Contexts.** Each tick: no player ped (or the layer off) is *none* and nothing is claimed;
  `MenuActive` (`0x08b8ef51`) set is the pause menu; `ped+0x538` set is a vehicle; otherwise on foot.
- **A key's release goes where its press went.** `HandleHostKey` records each claimed press with
  the context it was pressed in. A release is claimed only if its press was, so neither this layer
  nor PPSSPP's mapper is ever left holding a key nobody will release. With no context nothing is
  recorded at all.
- **Keys don't cross the menu.** A key pressed on foot still acts in a vehicle (W held while getting
  in drives off), but not across the pause menu: Enter opens it as Start and must not then select
  the first page as Cross.
- **Only what this layer pressed is released** (`__CtrlUpdateButtons(mask, lastMask & ~mask)`), so a
  real pad or PPSSPP's own mapping of the same button isn't cancelled. The stick is set while a
  movement key is held and centred once when the last is released.
- **The wheel** becomes a 4-vblank d-pad press with a 2-vblank gap per notch, queued.

## Mouse look on foot: taking the camera over

Writing `Cams[ActiveCam].Beta` / `Alpha` turns the camera, but `Process_FollowPed` fights three
ways, each measured and each switched off only while the mouse drives the on-foot camera
(`SetCameraTakenOver`):

1. **The pitch spring.** Alpha moves `1 - table[5]^TimeStep` of the way back to a default each frame
   (31% at 30 fps; table at `0x08b359bc`, `[5]` = 0.8 / 0.9). A written pitch was back in 0.6 s.
   Set to 1.0: pitch holds exactly.
2. **The yaw correction.** `add.s $f12, $f12, $f13` at `0x088f2acc` adds `(targetYaw - Beta) /
   TimeStep` to the yaw rate, where targetYaw is the yaw of where the camera actually is plus the
   swing in behind a walking player. It steered a written yaw back: 25 steps of 0.05 rad kept 0.066
   of 1.25. NOPed: exact. The cost is no auto-swing behind a walking player, which a PC mouse camera
   doesn't do. Writing `Front` (`+0x1a0`) consistently instead was tried and changed nothing.
3. **The obstacle nudge.** Rays around the camera add `(i - 2) * 0.03` rad to `0x08b35d70` per hit,
   applied to Beta and decaying by 0.95. Zeroed each frame.

`BetaSpeed` (`+0xd0`) is zeroed once when the takeover starts. Everything is restored the moment
the context isn't on foot or the camera isn't mode 4 (getting into a car was checked: the stock
instruction and spring came back, and went out again on getting out).

**Applied between frames.** The mouse is applied from the game's frame wait
(`sceDisplayWaitVblankStart` called from `0x08ab3384`, return address `0x08ab338c`), not the vblank
tick, so it can never land while the camera code holds Beta in a register.

**Measured (JIT, Release):** 25 x 20 counts turned exactly +1.250 rad, 25 x -20 exactly -1.250,
25 x 4 exactly +0.250, one 400-count move +1.000, no drift at rest over 3 s, pitch 20 x 6 counts
+0.300 and back -0.300. Walking with W while turning bent the path with the camera (heading 1.53
-> 0.41 rad).

Mouse look needs PPSSPP's **"Use mouse control"** (`UseMouse`): `NativeMouseDelta` returns before
the LCS seam without it, and Windows' raw input only turns the left button into a key event with it
on. It also hides the cursor over the window. Raw input arrives only while PPSSPP has focus.

## Testing without the keyboard

`LCS_KEY_PIPE=<file>` makes `UI/LCSDevInput.cpp` read `press`/`down`/`up`/`look`/`wait` lines
from that file once per frame (header comment has the syntax) and feed them to `NativeKey` and
`LCS::HandleMouseDelta`. Nothing reaches Windows. Read the result back over the debugger: the
game's own `NewState` at `Pads[0]+0x02` shows which PSP buttons a key produced. Three traps:

- **The pipe runs per host frame, which is 30 Hz in this game.** 25 `look` + `wait 1` pairs take
  1.67 s; measure after that, or the tail lands in the next measurement. Hours went into "lost"
  mouse input that was a too-short wait.
- **Run tests with "Use mouse control" off.** With it on, the desk's real mouse moves the camera
  whenever PPSSPP has focus, which it takes at launch. The pipe's `look` bypasses the setting.
- **Don't minimise PPSSPP during boot.** It stalls the boot, and killing it then left
  `FailedGraphicsBackends.txt` behind, so the next start switched to Direct3D 11 behind a modal
  dialog. Delete that file to undo it.

## Not done

- Mouse look in vehicles (`Process_Cam_On_A_String`, mode 18): a written yaw half-sticks and the
  camera swings back behind the car. Not claimed there yet; VCS needed an in-frame camera
  replacement for this (`docs/vcs/aiming.md` and `VCSChaseCam.cpp` in the VCS fork).
- Free aim with the mouse (the game's free aim is entered with D-Down while locked on), and aiming
  in general (modes 11/28, 7/8/34/45/46).
- Gamepads: the layer only claims keyboard and mouse; a pad goes to PPSSPP's mapper with the PSP
  layout.
- Rebinding, a walk modifier, direct weapon selection by number.
- The help text still names PSP buttons; VCS rewrote its GXT tokens to key names
  (`Tools/vcsgxtkeys.py` in the VCS fork). The token table above is where that starts.
