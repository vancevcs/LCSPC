# LCS mouse free aim

`Core/LCS/LCSFreeAim.cpp`, with the fire hook's two entries in `Core/HLE/ReplaceTables.cpp`. Setting
`MouseFreeAim` in `lcs.ini` (default on; also needs `KeyboardMouse` and `MouseLook`); Debug -> LCS
-> Aim shows its state. The VCS fork's `VCSFireHook` and the aim half of `VCSCamera` are the same
design, and its `docs/vcs/aiming.md` is the long story of why it is shaped this way.

## The game's free aim

Hold R (lock on), tap d-pad down: free aim, the game's help line `FREE1`. Measured with the rifle
(weapon type 26):

- the camera goes to **mode 11** (Process `0x088f07cc`, shared with 28), FOV 50, over the shoulder,
  with a reticle drawn at the middle of the screen;
- `ped+0xc9d` goes to 1 (0 in lock-on and on foot), and mode 11 then skips its crosshair offsets
  and its pitch clamp;
- `ped+0x6bc` (`m_pPointGunAt`) is a placeholder **object** (type 4) the game keeps along the
  camera's Front from the aim pivot, re-projected onto whatever the aim hits (9.2 m on level
  ground, 3.5 looking down, 90 = the rifle's range looking at the sky);
- mode 11 turns Beta / Alpha toward that target by at most `0.1 * TimeStep` rad a frame
  (`0x08b35920`), and the ped turns with it. The stick moves the target, not the camera.

So the target is the master. Measured: Beta written +0.5 came back within 0.12 s with the target
unmoved; the target written 0.3 rad round was kept, and the camera followed within a frame.

The **pivot** mode 11 orbits (`0x088ebad8`): the ped's position + 0.6 up (+0.1 x min(1, (70 - FOV)
/ 20), and 0 or 0.15 by weapon) + its right vector x (0.55 + 0.1 x min(1, (70 - FOV) / 35)), pulled
back by a building raycast. The camera's centre line passes through it: 0.000 off the line at
three pitches. `LCSFreeAim` uses the formula and then projects it onto the live centre line, which
removes whatever the formula misses.

## What this does

- **Into free aim from the mouse.** Right mouse is R; on its press with an instant-hit gun (weapon
  info fire type 1) the input layer presses d-pad down 4 vblanks later for 6, as VCS's
  `autoFreeAim` does. Middle mouse is the same R with no press: the game's lock-on. C (L + R) never
  starts free aim.
- **The mouse aims** (`FreeAimLook`, from the frame boundary, every frame while free aim is
  active): its own yaw and pitch, anchored to the game's on entry, scaled by FOV / 70 so a count
  is the same distance on screen, pitch kept to -0.49 / +0.67 (what the stick reached). Each frame
  it writes them into the camera's Beta / Alpha, which removes the 0.1-a-frame turn lag, and puts
  the target along that aim from the pivot at the game's own distance, so the camera's turn toward
  the target has nothing to do and nothing pulls back.
- **The stick stays centred** in free aim: there WASD is the game's aim stick and turned the view
  against the mouse (0.04 rad a second either way) while moving no one. The player stands to aim,
  as on the PSP.
- **The shot goes down the reticle** (`Hook_lcs_fire_raycast`, a `HOOKENTER` on
  `jal 0x08852188` at `0x08858638` in `CWeapon::FireInstantHit`): only the player's shot (`$t1` is
  the shooter), only in free aim. The start moves along the camera's centre line to level with the
  muzzle (re3's `Find3rdPersonCamTargetVector`), the end goes out the weapon's range (weapon info
  `+4`; 90 for the rifle) along it. The start is borrowed: `Hook_lcs_fire_raycast_done`, inside the
  wrapper on the first instruction after `ProcessLineOfSight` returns (`0x088521dc`; the op after
  the call in FireInstantHit is a branch), gives it back, so the flash and the tracer stay at the gun.

The site was found with log-only breakpoints on the wrapper's nine call sites, under the
interpreter: firing in free aim hit `0x08858638` once a bullet and nothing else.

## Measured (JIT, Release, rifle, the park fixture at 30 fps)

| | result |
|---|---|
| right mouse alone | mode 11, `ped+0xc9d` 1 |
| 200 counts right | Beta -0.358 (expected -0.357 = 200 x 0.0025 x 50/70) |
| held 1 s, mouse still | 0.0000 of drift |
| 120 counts up | Alpha +0.214 (expected 0.214) |
| the ped | turns with the aim (heading 2.731 against the view's 2.737) |
| WASD held in free aim | nothing moves (it moved the aim 0.04 before the stick was centred) |
| shots at the ground, 8 m | 5/5 hit 0.000 off the camera's centre line |
| after a savestate load | hooks reinstalled (logged), shots still 0.000 off the line |
| letting go | mode 4, the chase camera takes the view from where the aim was |
| middle mouse | R without free aim |

`LCS_CAM_TRACE=1` logs every redirected shot: weapon, range, the hit and its distance from the line.

## Fixtures

`lcs_foot_armed.ppst` (all three weapon-set cheats, fists out) and `lcs_foot_rifle.ppst` (the
rifle, type 26, out) in `memstick/PSP/PPSSPP_STATE/`, in the first mission's park. The cheats were
typed with the pipe's `pad` verb, the queue Debug -> LCS -> Pad plays.

## Not done / unchecked

- **Walking while aiming.** The game skips movement in free aim. VCS lets a run that was going
  carry on and patches the skip back in to stop (`MoveGateBranch`, a brake); LCS's equivalent in
  the player control code is not found yet.
- Lock-on with the mouse (the target switching on the d-pad still works).
- Scoped weapons (sniper, first-person modes) and the drive-by: the game's, untouched.
- Only the rifle was measured; the pistol, SMGs and shotguns go through the same fire site, but the
  shotgun's pellets may take another path (`0x0885b36c` has its own three raycasts).
- The reticle position is the middle of the screen as drawn; mode 11's own crosshair offsets
  (`TheCamera+0x180/+0x184`, 0.53 / 0.47) are skipped in free aim and not used.
