# LCS input: keyboard and mouse

`Core/LCS/LCSInput.cpp`, with the seams in `UI/NativeApp.cpp` (`NativeKey`, `NativeMouseDelta`) and
`Core/HLE/sceKernelTime.cpp` (`sceKernelGetSystemTimeWide`, the frame boundary). Settings in
`PSP/SYSTEM/lcs.ini` and the Input tab of Debug -> LCS: `KeyboardMouse`, `MouseLook`,
`MouseSensitivity` (rad per count, default 0.0025), `InvertMouseY`, `FootChaseCamera` /
`VehicleChaseCamera` for the camera the mouse turns (chase-camera.md), and `MouseFreeAim`
(free-aim.md).

## The game's own layout

Read before any code, from the help text: `ENGLISH.GXT`'s `C0*` entries are the control tokens the
tutorial lines use (`~k~ ~PDSPR~`), one set per control configuration (C0-C3, `CPad::Mode` at
`Pads[0]+0x82`). **The default is `C2`**: RESET GAME SETTINGS sets 2 (`0x08ade44c`), a save carries
its own (`0x088b221c`), and the fixtures and the saves on hand all read 2. An earlier version of this file
called `C0` the default and the car keys were built on it, which is why E steered and braked
(below). `C2` is `C0` on foot and differs only in a vehicle:

| in a vehicle | `C0` | `C2` |
|---|---|---|
| handbrake (`VEHB`, GetHandBrake `0x08a97e3c`) | Square + Cross | R |
| look left (`VELL`, `0x08a976cc`) | L | L + stick left |
| look right (`VELR`, `0x08a978a0`) | R | L + stick right |
| look behind (`VELB`, `0x08a97a78`) | L + R | L + stick down |

`C1` and `C3` are `C0` and `C2` with the stick and the d-pad swapped (radio, horn, weapons, steering
on the d-pad). The keyboard holds the game in `C2` while KeyboardMouse is on (LCSFrontEnd's
`GamePrefsTick`). The rest, common to all four:

| on foot | | in a vehicle | |
|---|---|---|---|
| sprint | Cross (`PDSPR`) | accelerate | Cross (`VEACC`) |
| jump | Square (code: `0x08a98308`) | brake / reverse | Square (`VEBRK`) |
| enter / hang up | Triangle (`VEEE`) | handbrake | R (`VEHB`, `C2`) |
| lock on | R (`PDLT`) | horn | D-Down (`VEHN`) |
| fire | Circle (`PDFW`) | radio | D-Left / D-Right (`VECRS`) |
| change weapon | D-Left / D-Right (`PDCWE`) | look left / right / behind | L + stick left / right / down (`C2`) |
| change target | D-Left / D-Right while locked on (`PDCTL`) | drive-by | a look left or right, fire with Circle (`HELP19A`) |
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
| Alt + W A S D | walk (the stick pushed 0.45) | | |
| Up / Down arrows | (PPSSPP's d-pad) | lean a bike, aim a water cannon | (PPSSPP's d-pad) |
| Left mouse, Left Ctrl | fire | fire | map: drag (left mouse) |
| Right mouse | aim: with a gun, straight into free aim, aimed by the mouse; with a sniper rifle or rocket launcher, the scope | | |
| Middle mouse | lock on (the game's R, no free aim) | | |
| Space | jump; in the sniper's scope, zoom in (the game's Square) | handbrake (R) | select |
| Left Shift | sprint; in the sniper's scope, zoom out (Cross) | (claimed, does nothing: PPSSPP's rapid fire) | |
| F | enter vehicle / hang up | exit / hang up | map: place / remove the marker |
| Q / E | previous / next weapon (target when locked on) | look left / right, and the drive-by to that side | map: zoom out / in |
| mouse wheel | next / previous weapon; in the sniper's scope, zoom | next / previous station | map: zoom; STATS, BRIEF: scroll |
| R / T | | previous / next station | |
| H | | horn | |
| C | look behind | look behind (L + stick down; Q + E too) | |
| X | free aim (tap while locked on) | | |
| Tab | answer phone, swap for a pickup | answer phone | |
| V | camera mode | camera mode | |
| G | side missions on / off | side missions on / off | |
| Enter | pause menu | pause menu | select |
| Backspace | | | back (on a page this fork opened: back to this fork's menu) |

Escape, F-keys and everything not in the table stay PPSSPP's. During the intro (the logos and the
credits) every other key skips it (front-end.md "Boot").

Alt walks because a key is all or nothing and the game reads a walk from how far the stick is
pushed. Measured on the park fixture, by deflection: nothing to 0.2, 0.044 a frame from 0.3 to 0.6,
faster from 0.7, the run (0.176) at 1.0. Alt pushes 0.45 after the diagonal's normalisation:
Alt + W walked 0.0440, Alt + W + D 0.0434, right Alt + S 0.0440, W alone ran 0.1768. Windows' own
use of Alt (focusing the menu bar on its release) is turned off while the layer has the keyboard:
`SC_KEYMENU` in `Windows/MainWindow.cpp`, which upstream only does when PPSSPP maps an Alt. Checked by
posting that command to the window: the thread didn't enter menu mode.

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

## Rebinding

The key cards (front-end.md "Key bindings") rebind per context: an action is named by its shipped key
in `kKeyMappings` or `kStickKeys`, and `BoundKey(context, shipped)` is what does it now. A press is
translated once, in `HandleHostKey`, into the shipped key it stands for (`HeldKey::as`), and
everything after that - the table, the stick, the drive-by keys, the aim key - asks about that key,
so none of it changed. Three rules, the VCS fork's:

- **A clash swaps.** Binding Jump to F hands Enter Vehicle Jump's old key: an action left with no
  key is something the game can no longer do.
- **A key an action moved off stays claimed** and presses nothing, so it doesn't fall through to
  PPSSPP's own mapping of it (Space is the PSP's Start there).
- **Escape can't be bound**: it is the pause and the capture's cancel.

`lcs.ini` keeps what moved under `[KeyBindings]`, `<context>.<shipped key code> = <key code>`
(NKCODE numbers, `OnFoot.62 = 39` is Jump on K). A key held across a context change keeps the meaning
it had when it went down. The game's tutorial lines name PSP buttons either way ("Not done").

## The looks and the drive-by on Q, E and C

In the game's `C2` every look in a vehicle is L with the stick that way, and a drive-by is a look
left or right and fire. So Q, E and C all press L, and while one is held it decides the stick on its
own (`kGlanceKeys`): Q fully left, E fully right, C fully down; Q + E is C. With L held the stick
doesn't steer.

The first build pressed R for E and L + R for C, which is `C0`. In `C2` R is the handbrake, so E
steered right (the stick) and braked, and C and Q + E stopped the car (reported, and measured:
W + E turned the car 2.95 rad against W alone, W + C took it to a standstill), while Space, Square +
Cross, was throttle and brake together and no handbrake at all.

Measured on the car fixture, held 1.6 s after 2 s of W, against W alone (4.49 units/s at the end,
the car turning -0.93 rad on the hill): Space ended at 0.66; E turned the car -0.96 and the camera
-1.56 rad from it (right); Q -0.96 and +1.58 (left); C and Q + E +3.11 (behind), the car still at
7.1. Drive-bys with the SMG selected (slot 5, written over the debugger: the fixture's mission keeps
Toni in the car): Q + left mouse fired 17 rounds out of the left window and E + left mouse 17 out of
the right, which the first build never managed; C + fire and fire alone shot nothing, the game's
own rule.

## The pad

The VCS fork's pad scheme (its input.md, "The gamepad is remapped"), on LCS's layout:
`kPadMappings`, a second context table beside the keyboard's, and `LCS::HandleHostAxis`, the axis
half, from `NativeAxis` as `HandleHostKey` is from `NativeKey`. Xbox names throughout; a
PlayStation pad on PPSSPP's Windows HID driver reports numbers (Cross is `BUTTON_2`), turned into
names at the door (`NamedPadButton`, keyed on the driver's pad names). `PadScheme` off (the
Controller page) gives a pad the PSP's own layout through PPSSPP's mapper again.

| control | on foot | in a vehicle | in the game's menu |
|---|---|---|---|
| left stick | move (a small push walks) | steer, lean | the game's nub |
| right stick | the camera (free aim, the scope) | the chase camera | |
| A B X Y | sprint, fire, jump, enter | horn, fire, handbrake (R), exit | Cross, Back, Square, Triangle |
| LT / RT | lock on (R) / fire | brake (Square) / accelerate (Cross) | (claimed) |
| LB / RB | the scope's zoom out / in; both: look behind (L + R) | look left / right (L + stick); both: behind | zoom (L / R) |
| d-pad | weapons, side missions, free aim (down while locked) | stations, side missions, horn | the game's d-pad |
| left / right stick click | L (phone, pickup swap) / camera (Select) | L (phone) / camera | (claimed) |
| View | the game's pause menu (Start) | same | closes it (Start) |
| Start | this fork's menu | same | same |

The face buttons stay where the PSP had them on foot, since A B X Y sit where Cross Circle Square
Triangle do. What moves is what a PSP had nowhere to put: aim and fire onto the triggers, the
pedals onto them in a vehicle, the camera onto a stick, the looks onto the bumpers. The rules
taken from VCS, each of which it learnt the hard way:

- **Every control is claimed in every context.** PPSSPP's XInput defaults put its pause on the left
  trigger, fast-forward on the right one and the speed toggle on the right stick click; an
  unclaimed control opens a menu or triples the speed mid-fight. The `psp = 0` rows are that.
- **The triggers press past 0.45 and let go below 0.35**: one line, where a finger rests, is
  crossed on noise alone, and the right trigger is Fire.
- **The right stick becomes mouse movement at the edge**: squared, sign kept, `PadLookSpeed`
  counts a vblank at full deflection into the accumulator the mouse fills, so the chase camera,
  free aim and the scope take it as they take the mouse. `CameraLookOwned` (mouse look on, with
  keyboard and mouse or the pad scheme) is the one gate for all three, so a player who turns
  keyboard and mouse off keeps the right stick.
- **The pad aims by lock-on.** LT is R and nothing more (the mouse's free-aim pulse is for right
  mouse alone); free aim is still the game's own d-pad down while locked, aimed by the right stick
  through the mouse's path. The scope is the exception: LT is R, and the scope comes up.
- **Axes are told regardless of the screen** (`NativeAxis` hands every one to the layer, then the
  unclaimed ones to the mapper only if no screen is up): an axis carries its whole state, so a
  trigger let go under the menu is still a release.
- **Y signs come from PPSSPP's own mapping** (`PadYSigns`): XInput reports a stick positive pushed
  away, DirectInput and the HID driver the other way.
- **B on a page this fork opened is Back to this fork's menu**, the pad's half of the Backspace
  rule; anywhere else in the game's menu it is the game's Circle.

The cards (front-end.md "Key bindings") are four columns, as VCS's: action, keyboard and mouse,
Xbox, PlayStation, the pad cells built from `kPadMappings` by the keyboard line's description.

Measured through the pipe as an XInput pad (`press PAD:A`, `axis LT 1`, below), reading `Pads[0]`
NewState: on foot A Cross, X Square, LT R, RT Circle, LB + RB L and R, LB alone nothing, the
clicks Select and L, View Start (the game's menu opened, and closed on the second press); LT held
at 0.4 after 1.0, let go at 0.3, and 0.4 from rest pressed nothing; the left stick at (0.5, 1)
moved Toni 4.65 units in a second, centred again on release; the right stick at full deflection
turned the camera 2.1 rad a second. In the car RT Cross, LT Square, X R, A Down, LB L with the
stick at -128, RB L with +126, both L with the stick down, B Circle, Y Triangle. Start opened this
menu and closed it again. Not tried on a real pad: no pad is on this desk.

## The help text names keys

The tutorial lines name no button: they carry a token ("Press ~k~ ~PDSPR~ while running"), resolved
through `C<configuration><token>` in MAIN ("C2PDSPR" reads "~X~"). So the control vocabulary of the
whole game is 36 strings per configuration, and `Tools/lcsgxtkeys.py` (the VCS fork's
vcsgxtkeys.py, on LCS's text) rewrites them to this fork's keys, the same in C0..C3 (the layer
holds C2, and a player on another would see PSP buttons again). Its mapping is hand-written
against `kKeyMappings`: change a binding, change it there; `--report` lists what is unmapped.

- Every line using a token was read before its name was chosen. Lock on is named RIGHT MOUSE (the
  aim: free aim with a gun, the scope with a sniper rifle); the zooms "the mouse wheel"; a bike's
  lean "the UP and DOWN arrows".
- Four lines are rewritten whole, because two PSP controls are one on a mouse: looking around on
  foot (L and the stick), the scope's fine aim, and the two zoom lines ("Use the mouse wheel to
  zoom in and out").
- An article in front of a token goes, by rule: "Press the ~PDFW~" was "Press the LEFT MOUSE".
- The writer's `--verify` rebuilds the file unchanged byte for byte (526,264 bytes, 91 tables), the
  check that caught two wrong guesses in VCS (strings aren't pooled; TKEY is ordered by key, TDAT
  isn't).

**Delivered at read time.** LCS opens its files by sector (`disc0:/sce_lbn...`), so there is no name
to redirect, and the ISO isn't touched: `LCS::PatchDiscRead`, called by `ISOFileSystem::ReadFile`
on both its read paths, overwrites whatever part of a read falls inside a patched range from a file
in `memstick/PSP/LCS/` (`kLCSDiscPatches`: ENGLISH.GXT at `0x42f8000`, sector 34288, found by
searching the ISO for the file's bytes). The disc is checked for the file's header (`TABL`) on the
first read that shows it, and a mismatch turns the patch off for the boot. The patched file is
524,008 bytes, padded to the original's length. It is generated, not in git (it is the game's own
text); the release ships it.

Measured both ways, booting into the save: with the file, "LEFT SHIFT" 15 times in RAM, "RIGHT
MOUSE" 4 and "analog stick left and analog stick right" none, and the log's "stands in at
0x42f8000"; with it moved aside, none of the keys' names and the PSP's line 10 times. Not read on
screen. The other languages keep the PSP's names.

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
`LCS::HandleMouseDelta`; `PAD:A` (`LB`, `RS`, `START`, `VIEW`, `DUP`...) is an XInput pad's button,
and `axis LT 1` (`LX LY RX RY LT RT`) its axis, through `NativeAxis`; `savestate <path>` and `loadstate <path>` make and load fixtures
(chase-camera.md "Testing" has one in a car); `set <key> <value>` sets an lcs.ini row live without
saving it (`set Shadows 0`), and `fps` logs the frame rate (shadows-and-water.md measured with both). Nothing reaches Windows. Read the result back over
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

- Walking while free-aiming (free-aim.md) and lock-on aimed with the mouse. The scoped weapons
  are done (free-aim.md "The scope").
- The pad on real hardware (only the pipe's XInput pad was measured), and a PlayStation pad's
  number translation, read off PPSSPP's HID driver as in VCS.
- Direct weapon selection by number.
- The help text for a pad player: it names the keyboard's keys (as VCS's does).
