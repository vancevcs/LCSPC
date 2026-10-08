# LCS chase camera (on foot and in vehicles)

`Core/LCS/LCSChaseCam.cpp`: the camera you look through on foot and in a car, bike or boat,
turned by the mouse. The VCS fork's `VCSChaseCam` is the same design, ported; its
`docs/vcs/chase-camera.md` has the history. Settings `FootChaseCamera` and `VehicleChaseCamera` in
`lcs.ini` (both default on; both also need `KeyboardMouse` and `MouseLook`). Debug -> LCS -> Camera
shows its state and a few session-only tuning sliders.

## Why not write the angles

In a vehicle, mode 18 (`CamOnAString`) swings back to where its own spring wants it: `Beta` written
to 1.679 between frames came back as 1.346. On foot the angles do stick once `Process_FollowPed`'s
spring, yaw correction and nudge are out (input.md), but mode 4 is a camera on a string with its own
distance band (`TheCamera+0x13c` + table `[1]`, at most `[4]`) and pulls in only where its own line
to the player is blocked, so looking up put the camera into the ground and a wall beside the player
was ignored. So, as in VCS, the game computes its camera and this replaces the result inside the
game's frame, which leaves the collision and the timing the game's own and gives the renderer, the
streamer and the radar the camera the player sees. When something is in the way the camera moves
in along the line it looks down, as San Andreas on PC does.

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
   near-clip call, where the shake is read. Reads the three vectors, decides the kind (below),
   works out the pivot, where the player is looking and how far back to sit, and writes five rays
   into a 1 KiB block in user memory.
2. **The program.** The `jal 0x088ea388` at `0x08902318` (the cross product that starts the matrix
   build) is pointed at a program in the block. It calls `CWorld::ProcessLineOfSight`
   (`0x088c68f4`, 13 arguments as VCS's) per ray with the game's own clip flags
   (1, 0, 0, 1, 0, 1, 1, 0, 0; vehicles on as well on foot), reaches the finish hook (a `HOOKENTER`
   on a `nop` in the block), calls `RwCameraSetNearClipPlane` (`0x0887352c`) if asked, and jumps on
   to `0x088ea388`.
3. **Finish**: pulls the camera in to the nearest hit (at once; easing back out at 3/s), lifts it
   out of the player's capsule or the car's box if it is inside, keeps the game's shake
   (post - pre), blends from or to the game's camera over 0.25 s / 0.15 s, lowers the near plane to
   0.6 x the camera's clearance from the player or the car (never above the game's value), and
   writes Source, Front and Up. On foot it also writes the game's `Beta` (the view's yaw + pi) and
   `Alpha` (the pitch, clamped to the game's 0.785 / -1.484), so the player walks where the camera
   looks and aiming starts from the view.

**The kind**, from the game's state in the hook, not the input context (decoded at the vblank, a
frame behind the camera): mode 4 is on foot whatever the in-vehicle flag says, and mode 18 / 22 is
the vehicle whenever `m_pMyVehicle` is set. Each lag was a cut: getting in, mode 18 arrives a frame
before the flag; getting out, mode 4 runs about 3 s before the flag clears. Anything else (aiming,
cutscenes, the other car views) is the game's. Between on foot and a vehicle with no frame in
between, the view is kept and the pivot slides across (6/s) while the distance eases.

**On foot**: pivot 0.6 above the player's origin (`Process_FollowPed`'s own target height, table
`[0]` at `0x08b359bc`), 4.5 back (its band's top: `TheCamera+0x13c` 2.5 + table `[1]` 2.0; measured
4.1-4.5 in the open), pitch +1.20 / -1.35, the orbit is the view (looking up brings the camera down
behind the legs). Parked cars block the camera, except the player's own while the player is inside
its box (getting in or out: the rays start at the player, so every one hit it and the camera
snapped from 8.6 to the minimum). C (L + R) looks behind and letting go returns to the view; L
alone is the game's look-around and R alone its lock-on, so neither glances on foot.

**In a vehicle**: framing from the collision box (model info `[0x08b4d3bc][entity+0x58]`,
`CColModel *` at `+0x14`, box at `+0x10`/`+0x20`): pivot at 0.95 of the box top, distance
`(5.0 + 0.65 * length)`, x 0.8 / 1.35 at TheCamera's car zoom (`+0xcc`) 1 / 3. Pitch +0.90 / -1.30;
the camera position orbits at most 0.35 below the pivot, so looking further up tilts the view.
Q / E / C (the game's L / R / both, read from `Pads[0]` NewState, so a pad glances too) look left,
right and behind, and letting go returns behind the car. After 1.5 s with no look, a vehicle moving
faster than 3 units/s swings back behind itself.

The mouse goes to `ChaseCamAddLook` from the frame boundary (`LCS::OnFrameBoundary`). On foot the
LCSInput takeover (spring, yaw correction, nudge) stays in while mode 4 has the mouse, so the angles
the camera writes hold.

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

## Measured (JIT, Release)

In Vincenzo's car (box 5.58 long), 60 fps:

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
| switched off with an "on" state | the game's camera, the mouse doesn't turn it |

On foot, the park at the start of the first mission, 30 fps:

| | result |
|---|---|
| took the view | 4.500 behind the pivot |
| 200 counts right | yaw -0.500 exact; the game's `Beta` follows exactly |
| looking up 1.0 | the ground brought it in to 1.77, 0.84 under the player's origin, behind the legs |
| walking after a 1.0 rad turn | 9.36 units along the camera's yaw, off by 0.000 |
| C held / let go | pi from the player's heading; back to the previous view |
| Vincenzo's car between the camera and the player | in to 0.82 / 0.52 / 0.55 at pitch 0 / 0.25 / 0.5 |
| getting out | vehicle -> on foot with no hand-back; the camera never moved more than 1.2 per ~90 ms (the distance easing 8.6 -> 4.5) |
| getting in | on foot -> vehicle directly; no jump over 0.6 per ~90 ms |

`LCS_CAM_TRACE=1` logs one line a game second (kind, mode, angles, distances, hits, near) and one
on every change of kind.

## Testing

Two local fixtures in `memstick/PSP/PPSSPP_STATE/` (the memstick is gitignored), made with the
pipe's `savestate`:

- `lcs_car_vincenzo.ppst`: Toni in Vincenzo's car at the start of the first mission, on a slope:
  the car rolls when it loads, so brake (S) before F or the exit is refused, and don't drive it
  far: the mission's destination is close and starts a cutscene.
- `lcs_foot_park.ppst`: Toni on foot in the same park, the car out of reach.

Boot one with `--state=<path>`, or `loadstate <path>` through the pipe. To make them again: with a
save on the memory stick the game loads it after the intro (Cross skips the intro); Start -> game
-> New Game -> Yes (Delete Save Data is two rows below Load Game: check the highlight), Cross
through the cutscenes, and Toni walks to the car and gets in on his own about 26 s later.

The look command (`kOffCommand` in the block) sets an exact yaw and pitch, which is how the car
was put between the camera and the player.

## Not done / unchecked

- Aiming (lock-on, free aim, first person) is the game's camera; handing over to and from it was not
  checked live, nor the drive-by.
- Looking up with a car right behind the player leaves only sky: the camera is pushed to the head
  and lifted over it. Same as VCS.
- Boats (mode 22) and bikes: the code takes them, but only Vincenzo's car was measured (a bike's
  2.18 box took the view in a manual run).
- The near-plane change is from the trace and a screenshot, not a read of `RwCamera+0x80`.
