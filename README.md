GTA: Liberty City Stories - PC version
======================================

**Grand Theft Auto: Liberty City Stories**, the 2005 PSP game, made to play like a PC GTA: mouse
look and mouse aiming, WASD, a gamepad laid out like a PC game's, a GPS route on the radar, HD
textures, SMAA, a 60 fps option and a GTA-style menu. The game's tutorial messages name your keys
instead of PSP buttons.

It runs on **Windows**. Bring your own copy of the game: no game files are included.

**[Download the latest release](https://github.com/vancevcs/LCSPC/releases)**

The sister project for Vice City Stories is [VCSPC](https://github.com/vancevcs/VCSPC).

---

## What you need

- **Your own copy of the game**: the USA UMD of *Grand Theft Auto: Liberty City Stories*, disc ID
  **`ULUS10041`**, **version 3.00**, dumped to an `.iso` or `.cso`. Other releases and versions are
  not supported, because every memory address this build relies on is that disc's. The game checks
  the disc before it starts and says what it found if it is another one.
- 64-bit Windows 10 or 11, a Vulkan-capable GPU, and about 2 GB of free RAM.

## Getting started

1. Unzip the release anywhere. Nothing gets installed.
2. Put your `.iso` or `.cso` in that folder, next to the game.
3. Run **GTA Liberty City Stories**. It starts fullscreen and goes straight into the game.

The first run remembers the disc. Keep the folder together: your saves and settings live in the
`memstick` folder beside the game, and nothing is written anywhere else.

## Features

### Controls that feel like a PC game
- **Mouse look.** The camera follows the mouse on foot and in vehicles, and pulls in around walls
  the way the PC GTAs do. In a car it swings back behind you once you let go.
- **Mouse aiming.** Hold the right mouse button to aim, place the crosshair with the mouse and click
  to fire; the shot goes where the crosshair is. The sniper rifle and the rocket launcher aim their
  scope with the mouse and zoom with the wheel.
- **Controls that depend on what you're doing.** Space is jump on foot and the handbrake in a car;
  Q and E change weapon on foot and look left and right in a car, with a drive-by on fire.
- **Walking.** Hold Alt with WASD to walk instead of run.
- **Gamepad support**, laid out like a modern game: triggers aim and fire on foot and drive in a
  car, the right stick turns the camera, the bumpers look left and right. Xbox and PlayStation pads
  both work.

### The world
- **A GPS route.** Place a marker on the map, or take a mission, and the radar draws the route
  along the roads.
- **HD textures**, included and switched on: the menus and the HUD, and a growing share of the city.
- **SMAA anti-aliasing** over the world, before the HUD so its text stays sharp.
- **A 60 fps option**, with the game still running at its own speed.

### Menus and saves
- **A Liberty City Stories-style menu** opens with Escape (Start on a pad), with settings for
  controls, audio, display and gameplay, and cards listing every control for the keyboard, Xbox and
  PlayStation pads side by side. Any keyboard control can be given another key.
- **The game's own map, brief and stats** open from that menu; the map drags with the mouse and
  zooms with the wheel.
- **Load, delete, or start a new game** from your own save list, each save named after its last
  mission. Saving at a safe house opens the same menu's save page.
- **Auto-save after every story mission.** When you start the game it continues from your newest
  save.
- **Tutorial messages that name keys**: "Press LEFT SHIFT while running to start sprinting".
- **Every cheat** as a menu row.

## Controls

**Options → Controller setup → Key bindings** has the full list for every device. The main ones:

| | on foot | driving |
|---|---|---|
| **W A S D** | move | W/S accelerate and brake, A/D steer |
| **Mouse** | look | look |
| **Left mouse / Left Ctrl** | attack, fire | fire (with Q or E: a drive-by) |
| **Right mouse** | aim (hold) | |
| **Middle mouse** | the game's lock-on | |
| **Space** | jump | handbrake |
| **Left Shift** | sprint | |
| **F** | enter a vehicle | exit |
| **Q / E** | previous / next weapon (wheel too) | look left / right |
| **C** | look behind | look behind |
| **R / T** | | radio station (wheel too) |
| **H** | | horn |
| **V** | change camera | change camera |
| **Esc** | menu | menu |

| key | what it does |
|---|---|
| **Alt + WASD** | walk instead of run |
| **G** | side missions on and off: vigilante, taxi, paramedic |
| **Tab** | answer the phone, or swap for a weapon on the ground |
| **X** while locked on | the game's free aim |
| **Mouse wheel** in a scope | zoom |
| **Up / Down arrows** | lean a bike, aim a water cannon |
| **Enter** | the game's own pause menu |
| **Backspace** | from the game's map, brief or stats, back to the menu |

## Settings worth knowing

- **Display setup → Frame limiter** switches 30 and 60 fps.
- **Display setup → Advanced** has the texture quality (HD or the PSP's own), anti-aliasing and
  world memory.
- **Gameplay** has the auto-save, the GPS route, the language, and the game's own subtitles, HUD
  and radar settings.
- Settings are saved to `memstick/PSP/SYSTEM/lcs.ini`.
- New textures the game draws are saved to `memstick/PSP/TEXTURES/ULUS10041/new`: the HD pack is
  completed from them.

## Building from source

```
git clone --recursive https://github.com/vancevcs/LCSPC.git
msbuild Windows\PPSSPP.sln /t:PPSSPPWindows /p:Configuration=Release /p:Platform=x64 /m
```

Visual Studio 2022 with the C++ desktop workload. The exe lands in the repository's root, beside
`workshop.txt`, which keeps it the developer's build: PPSSPP's menu bar, its game list, the in-game
debugger (**Debug → LCS**) and the log file. Without that file beside it, the same exe presents as
the game. `python Tools/lcspackage.py --zip` turns a build into the release folder and zip; it needs
a populated `memstick/` folder, which you can copy from a release.

### Where things are

| path | what it is |
|---|---|
| `Core/LCS/` | the game layer: input, the chase camera, aiming, the GPS route, the menu bridge, saves, settings, memory addresses |
| `GPU/Common/LCSSmaa.*` | anti-aliasing |
| `UI/LCSMenuScreen.*` | the menu, the start screen |
| `assets/lcs/` | the menu's backdrop, typeface and sounds |
| `Tools/lcs*.py` | packaging, the keyboard-text patcher, the HD texture tools, and reverse-engineering tools |
| `docs/lcs/`, `docs/LCS_ADDRESSES.md` | design notes for each feature, and how every game address was found |

## Licence and credits

This is free software under the GNU GPL, version 2 or later; see `LICENSE.TXT`. It is built on the
[PPSSPP](https://github.com/hrydgard/ppsspp) emulator, and follows the
[VCSPC](https://github.com/vancevcs/VCSPC) port of Vice City Stories. The menu typeface, Pricedown,
is by Ray Larabie.

No game data is included, apart from the menu's backdrop and three short menu sound effects taken
from the game (`assets/lcs/`). Grand Theft Auto and Liberty City Stories are trademarks of Take-Two
Interactive Software. This project is not affiliated with or endorsed by Rockstar Games or Take-Two.
