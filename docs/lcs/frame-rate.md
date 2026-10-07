# LCS at 60 fps

`Core/LCS/LCSFrameRate.cpp`, switched by `FrameRate60` in `PSP/SYSTEM/lcs.ini` or the "60 fps"
checkbox in Debug -> LCS. Off by default.

## What it takes: two patches, no clock change

1. **The cap.** The frame wait at `0x08ab3368` loops on `sceDisplayWaitVblankStart` until the vblank
   counter (`0x08b56a90`) reaches 2: `sltiu $a0, $a0, 2` at `0x08ab3378`. Making it 1 halves the
   wait. The same instruction VCS has; ThirteenAG's plugin NOPs the loop-back branch instead.
2. **The millisecond carry.** `CTimer::Update` (`0x0891b328`) adds each frame's delta to
   `m_snTimeInMilliseconds` as a whole number of milliseconds and drops the fraction (`TimeStep`
   keeps it, so physics speed is right; script waits and timers run slow). The game has a carry
   path of its own behind a flag that is never set (`0x08b56c7c`), quantising to 16 ms. Five words
   at `0x0891b3ec`-`0x0891b40c` take that path unconditionally with a 1 ms quantum, held in `$t3`
   (dead at that point; next written at `0x0891b434`). The same fix as VCS's.

**No CPU clock change.** VCS needed its emulated CPU at 666 MHz to fit a frame in one vblank
([vcs-30fps-lock]). LCS doesn't: at the game's own 222 MHz it held 59.94 fps at 100% emulation
speed on foot near the safehouse and driving through Harwood with steering. Heavier places
(Portland downtown with traffic, Staunton, rain) are not measured yet; if one drops below 60, the
VCS answer (raise the clock, force auto frameskip) is the place to start.

## Measurements (2026-10-07, Release build, Harwood / Saint Mark's)

Rates per emulated second over 4-6 s; emulation speed was 100% throughout.

| | game fps | game ms per second | TimeStep |
|---|---|---|---|
| stock | 29.97 | 989.0 | 1.6576-1.6794 |
| cap only | 59.77 | 959.1 | 0.8234-0.8452 |
| cap + carry | 60.11 | 999.9 | 0.80 / 0.85 |
| carry only (at 30) | 29.97 | 999.9 | 1.65 / 1.70 |

The shipped game's clock already runs 1.1% slow at 30. The carry is only applied together with the
cap, so 30 fps stays exactly stock.

## Behaviour of the switch

- All six words are checked before any is written; they go in or out together. A word holding
  neither the game's op nor ours (another build, someone else's patch) stops the whole set and logs
  `LCS: frame rate words are neither the game's nor ours; leaving them alone` once. Tested with a
  harmless foreign word: the cap stayed stock, then went back in when the word was removed.
- Re-checked every tick through `Read_Instruction`, so a savestate that brings back stock code is
  re-patched within a frame (tested by writing stock words over the debugger: re-applied within
  0.2 s).
- The carry's remainder (`0x08b36bcc`) is zeroed when the carry goes in.
- Logs `LCS: frame rate 60 (cap and millisecond carry in)` / `... 30 ...` on each change.

## Not checked yet

- **Audio.** ThirteenAG's 60 fps option also runs `cAudioManager::Service` (`0x08a9a5e4`) only every
  other frame, which suggests some audio is stepped per frame. Needs ears: listen for doubled-speed
  sounds or radio at 60.
- Frame-counted game logic (anything that counts `m_FrameCounter` rather than milliseconds) would
  run twice as fast. Nothing has been seen yet; watch cutscene timing, the HUD's flashing, and
  mission timers.
- The heavy areas above.
