# LCS vehicle chase camera

`Core/LCS/LCSChaseCam.cpp`: the camera you look through in a car, bike or boat, turned by the
mouse. The VCS fork's `VCSChaseCam` is the same design, ported; its `docs/vcs/chase-camera.md` has
the history. Setting `VehicleChaseCamera` in `lcs.ini` (default on; also needs `KeyboardMouse` and
`MouseLook`), and Debug -> LCS -> Camera shows its state and a few session-only tuning sliders.

## Why not write the angles

Mode 18 (`CamOnAString`) swings back to where its own spring wants it: `Beta` written to 1.679
between frames came back as 1.346. So, as in VCS, the game computes its camera and this replaces
the result inside the game's frame, which leaves the collision and the timing the game's own and
gives the renderer, the streamer and the radar the camera the player sees.

## How

`CCamera::Process` (`0x08900c78`) keeps the frame's camera in three stack vectors from `0x08900d1c`
to its end:

| vector | stack | register | loaded from (`0x089012cc`) | in TheCamera's matrix |
|---|---|---|---|---|
| Source | `sp+0x70` | `$fp - 0x10` | `CCam+0x1b0` (via `$s5 = sp+0x70`) | `+0x30` |
| Front | `sp+0x80` | `$fp` | `CCam+0x1a0` (`kLCSCamFrontOffset`) | `+0x10` |
| Up | `sp+0x90` | `$s7` | `CCam+0x1d0` | `+0x20` |

The first build had Front and Up the other way round, from an unchecked note. Read live, the
matrix's `+0x20` row was `(0, -0.05, 0.999)` with the on-foot camera level, which is Up, and the
loads above settle it.

1. **Prepare** (`REPFLAG_HOOKENTER` at `0x08902174`, `lwc1 $f12, 0x1be8($s0)`): the first
   instruction after the game's own camera clip (`jal ProcessLineOfSight` at `0x08902128`) and its
   near-clip call, where the shake is read. Reads the three vectors; in a vehicle (input context
   InVehicle, mode 18 or 22) works out the pivot, where the player is looking and how far back to
   sit, and writes five rays into a 1 KiB block in user memory.
2. **The program.** The `jal 0x088ea388` at `0x08902318` (the cross product that starts the matrix
   build) is pointed at a program in the block. It calls `CWorld::ProcessLineOfSight`
   (`0x088c68f4`, 13 arguments as VCS's) per ray with the game's own clip flags
   (1, 0, 0, 1, 0, 1, 1, 0, 0), reaches the finish hook (a `HOOKENTER` on a `nop` in the block),
   calls `RwCameraSetNearClipPlane` (`0x0887352c`) if asked, and jumps on to `0x088ea388`.
3. **Finish**: pulls the camera in to the nearest hit (at once; easing back out at 3/s), lifts it
   out of the car's box if it is inside, keeps the game's shake (post - pre), blends from or to
   the game's camera over 0.25 s / 0.15 s, lowers the near plane to 0.6 x the camera's clearance
   from the box (never above the game's value), and writes Source, Front and Up.

Framing is from the collision box (model info `[0x08b4d3bc][entity+0x58]`, `CColModel *` at
`+0x14`, box at `+0x10`/`+0x20`): pivot at 0.95 of the box top, distance `(5.0 + 0.65 * length)`,
x 0.8 / 1.35 at TheCamera's car zoom (`+0xcc`) 1 / 3. Pitch is limited to +0.90 / -1.30 and the
camera position orbits at most 0.35 below the pivot; looking further up tilts the view. Q / E / C
(the game's L / R / both, read from `Pads[0]` NewState, so a pad glances too) look left, right and
behind, and letting go returns behind the car. After 1.5 s with no look, a vehicle moving faster
than 3 units/s swings back behind itself.

The mouse goes to `ChaseCamAddLook` from the frame boundary (`LCS::OnFrameBoundary`) in the
vehicle context while the hooks are in; on foot it still writes the game's angles (input.md).

## Installing, and savestates

`InstallChaseCam` runs every tick and re-checks the magic word, the jal and both hooks through
`Read_Instruction` (both hook sites are JIT block starts, so the raw words are block markers).
Savestates, measured with the `loadstate` pipe command:

- A savestate keeps the jal and the block (plain memory) but not the replacement ops: after a
  load the prepare site holds the game's op and the finish site a `nop`. The check logs
  `patch changed (... prepare c60c1be8, finish 00000000)`, adopts the block by its magic and puts
  the hooks back; the view re-anchors to the game's camera with the usual blend. Mouse look was
  exact before and after (-0.500 for 200 counts both times).
- Booting with `--state=` does the same on the first tick.
- A state taken with the camera on, loaded with it off, would leave the jal into the block; the
  next tick puts the game's op back and frees the block (logged).
- A state from another build whose replacement index differs is still recognised through its
  block.

## Measured (JIT, Release, 60 fps, Vincenzo's car, box 5.58 long)

| | result |
|---|---|
| install | once per boot, `block 09ffec00, 60 instructions` |
| distance | 8.62 behind (formula 8.63) |
| 200 counts right / up | yaw -0.500, pitch +0.500, exact; 400 down -1.000 |
| look latency | 32-40 ms (two frames) |
| Q / E / C held | +pi/2, -pi/2, pi from the heading, exact; back behind on release |
| recentre | from 1.0 rad off to 0.06 after 2 s of driving (it starts once 1.5 s have passed without a look) |
| looking up 0.55 on a slope | 5/5 rays hit the ground, camera in to 3.84, near 0.90 -> 0.26 (0.44 clear of the box); the bumper rendered whole |
| driving at 32 units/s | a hit pulled it in to 3.8 at once |
| out of the car | mode 4 hands back; on-foot mouse look exact after it (-0.500) |
| switched off with an "on" state | the game's camera, the mouse doesn't turn it |

`LCS_CAM_TRACE=1` logs one line a game second (mode, angles, distances, hits, near).

## Testing

`memstick/PSP/PPSSPP_STATE/lcs_car_vincenzo.ppst` (local, the memstick is gitignored): Toni in
Vincenzo's car at the start of the first mission, made with the pipe's `savestate`. Boot it with
`--state=<path>`, or `loadstate <path>` through the pipe. To make it again: with a save on the
memory stick the game loads it after the intro (Cross skips the intro); Start -> game ->
New Game -> Yes (Delete Save Data is two rows below Load Game: check the highlight), Cross
through the cutscenes, and Toni walks to the car and gets in on his own about 26 s later.

## Not done / unchecked

- On foot is still the angle-writing camera (input.md); VCS moved on foot onto this camera too.
- Drive-by aiming with the mouse, and the first-person and top-down car views (left to the game).
- Boats (mode 22) and bikes: the code takes them, but only Vincenzo's car was measured.
- The near-plane change is from the trace and a screenshot, not a read of `RwCamera+0x80`.
