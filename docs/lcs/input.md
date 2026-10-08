# LCS input: keyboard and mouse

`Core/LCS/LCSInput.cpp`, with the seams in `UI/NativeApp.cpp` (`NativeKey`, `NativeMouseDelta`) and
`Core/HLE/sceKernelTime.cpp` (`sceKernelGetSystemTimeWide`, the frame boundary). Settings in
`PSP/SYSTEM/lcs.ini` and the Input tab of Debug -> LCS: `KeyboardMouse`, `MouseLook`,
`MouseSensitivity` (rad per count, default 0.0025), `InvertMouseY`, `FootChaseCamera` /
`VehicleChaseCamera` for the camera the mouse turns (chase-camera.md), and `MouseFreeAim`
(free-aim.md).

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
| Right mouse | aim: with a gun, straight into free aim, aimed by the mouse | | |
| Middle mouse | lock on (the game's R, no free aim) | | |
| Space | jump | handbrake | select |
| Left Shift | sprint | (claimed, does nothing: PPSSPP's rapid fire) | |
| F | enter vehicle / hang up | exit / hang up | map: place / remove the marker |
| Q / E | previous / next weapon (target when locked on) | look left / right, and the drive-by to that side | map: zoom out / in |
| mouse wheel | next / previous weapon | next / previous station | map: zoom |
| R / T | | previous / next station | |
| H | | horn | |
| C | look behind | look behind | |
| X | free aim (tap while locked on) | | |
| Tab | answer phone, swap for a pickup | answer phone | |
| V | camera mode | camera mode | |
| G | side missions on / off | side missions on / off | |
| Enter | pause menu | pause menu | select |
| Backspace | | | back (on a page this fork opened: back to this fork's menu) |

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

## The drive-by on Q and E

The game turns the gun out of the window only with L or R held **and** the stick pushed to that
side (reported: Q alone looked left, Q + A aimed). So in a vehicle Q also pushes the stick fully
left and E fully right (`kDriveByKeys`), and fire is the only other key a drive-by needs. While one
of them is held it decides the stick's x on its own (Q with D held still aims left); both together
is no side. Measured on the car fixture with the SMG chosen on foot: Q held put L at 255 and the
stick at -128 in `NewState`, and Q + left mouse fired out of the driver's window (ammo 150 -> 136).
E puts R and +126 there, but the game didn't fire to the right in that fixture, where Vincenzo sits
in the passenger seat; not tried with that seat empty. The car steers with the stick as it did with
Q + A.

## The game's own menu

With the pause menu up the context is Menu (above). On a page this fork's menu opened (MAP, BRIEF,
STATS: front-end.md "The game's own pages"), Backspace posts PPSSPP's pause instead of pressing
Circle, which would lift the focus to the tab strip the bridge keeps hidden; Escape already is the
pause. The map's controls are on keys: Q / E and the wheel are L / R (zoom), F is Square (the target
marker), Enter is Cross (the legend). While the bridge itself is pressing buttons, the player's keys
and stick stand down (`FrontEndDriving`), as for a cheat.

## Mouse look on foot: taking the camera over

With `FootChaseCamera` on (the default) the mouse turns the chase camera, which replaces mode 4's
view and writes `Beta` / `Alpha` from it every frame so walking and aiming follow the view
(chase-camera.md). The takeover below stays either way: it is what makes those angles hold. With
`FootChaseCamera` off, the mouse writes the angles itself and mode 4 is the view, as measured below.

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

**Applied between frames.** The mouse is applied at the frame boundary: the game's
`jal sceKernelGetSystemTimeWide` at `0x08ab3398`, right after its frame wait (return address
`0x08ab33a0`), not the vblank tick, so it can never land while the camera code holds Beta in a
register. The first seam was the wait's own `sceDisplayWaitVblankStart` (`0x08ab3384`), which a
frame that has already used up the cap skips: in the trees of the first mission's park at 60 fps
the mouse arrived 1-4 s late in bursts. The call after the loop is made every frame; measured
there, on foot and in the car, 23-40 ms from the pipe to the camera, every step exact.

**Measured (JIT, Release):** 25 x 20 counts turned exactly +1.250 rad, 25 x -20 exactly -1.250,
25 x 4 exactly +0.250, one 400-count move +1.000, no drift at rest over 3 s, pitch 20 x 6 counts
+0.300 and back -0.300. Walking with W while turning bent the path with the camera (heading 1.53
-> 0.41 rad).

Mouse look needs PPSSPP's **"Use mouse control"** (`UseMouse`): `NativeMouseDelta` returns before
the LCS seam without it, and Windows' raw input only turns the left button into a key event with it
on. It also hides the cursor over the window. Raw input arrives only while PPSSPP has focus.

## Mouse look in a vehicle

In a car, on a bike or in a boat the mouse turns the chase camera (`ChaseCamAddLook`), not the
game's angles: mode 18 swings a written yaw back. chase-camera.md has all of it. With
`VehicleChaseCamera` off the mouse does nothing in a vehicle, as before.

## PSP buttons from the debugger

Debug -> LCS -> Pad presses PSP buttons through this layer: a click taps one (held 6 vblanks), a
Hold box keeps it down, and a sequence ("Up Square Square Down Left Square Square Right", "+" for
buttons together) plays as taps with 6-vblank gaps, which is what the game's cheats need. Presets
for the three weapon sets, all three confirmed (weapons appear in `ped+0x594`). The pipe's
`pad <sequence>` plays the same queue.

## Testing without the keyboard

`LCS_KEY_PIPE=<file>` makes `UI/LCSDevInput.cpp` read `press`/`down`/`up`/`look`/`wait` lines
from that file once per frame (header comment has the syntax) and feed them to `NativeKey` and
`LCS::HandleMouseDelta`; `savestate <path>` and `loadstate <path>` make and load fixtures
(chase-camera.md "Testing" has one in a car). Nothing reaches Windows. Read the result back over
the debugger: the game's own `NewState` at `Pads[0]+0x02` shows which PSP buttons a key produced.
Three traps:

- **The pipe runs per host frame: 30 Hz at the game's 30 fps, 60 at 60.** 25 `look` + `wait 1`
  pairs take 1.67 s at 30; measure after that, or the tail lands in the next measurement. Hours
  went into "lost" mouse input that was a too-short wait.
- **Under a menu, only releases reach the layer**, from the pipe as from a real keyboard
  (front-end.md "Two traps"); a press goes to the screen.
- **Pipe keys skip NativeKey's ImGui filter.** With an ImGui window focused (one click on
  Debug -> LCS), NativeKey keeps keyboard keys from the game; a test then saw F and S do nothing.
  The pipe hands keys to `LCS::HandleHostKey` first, and only the ones it doesn't claim go through
  `NativeKey`.
- **Run tests with "Use mouse control" off.** With it on, the desk's real mouse moves the camera
  whenever PPSSPP has focus, which it takes at launch. The pipe's `look` bypasses the setting.
- **Don't minimise PPSSPP during boot.** It stalls the boot, and killing it then left
  `FailedGraphicsBackends.txt` behind, so the next start switched to Direct3D 11 behind a modal
  dialog. Delete that file to undo it.

## Not done

- Walking while free-aiming (free-aim.md), lock-on aimed with the mouse, and the scoped and
  first-person weapons (modes 7/8/34/45/46).
- Gamepads: the layer only claims keyboard and mouse; a pad goes to PPSSPP's mapper with the PSP
  layout.
- A walk modifier, direct weapon selection by number.
- The help text still names PSP buttons; VCS rewrote its GXT tokens to key names
  (`Tools/vcsgxtkeys.py` in the VCS fork). The token table above is where that starts.
