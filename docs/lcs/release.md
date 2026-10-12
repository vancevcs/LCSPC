# The release build

The VCS fork's game build (its boot-and-saves.md, "The main menu at launch", and its
Tools/vcspackage.py), for Windows: the packaged exe presents as the game, finds the disc, checks it
is this build's disc, and carries nothing of the machine it was built on.

## The game and the workshop

`LCS::PresentAsGame()` decides, and nothing else does. A Debug build is the workshop; a Release exe
is the game unless `workshop.txt` sits beside it. The repository's root has one, so the exe the
build writes there (every test, every dev run) keeps PPSSPP's logo, game list, menu bar, ImGui
debugger and log file; the packaged folder is the exe alone, renamed, so it is the game. VCS's rule
was Release = game, which here would have taken the log and the debugger from every measurement:
all of this fork's work runs on Release builds. The file asks nothing of a disc or of lcs.ini, so the
answer is the same from the first moment of a first run (VCS's PresentAsGame once answered "no" for a
whole first run, before anything had told it about a disc).

In the game build, each a few lines in the upstream file, marked Fork-specific:

| what | where |
|---|---|
| the start screen instead of PPSSPP's logo and game list | `NativeInit` (UI/NativeApp.cpp) |
| no menu bar (gated, not by forcing the saved `bShowMenuBar`, which the workshop shares) | `ShowMenuBar()`, Windows/MainWindow.cpp |
| the window's title: the disc's own ("GTA: Liberty City Stories"), no ID, no PPSSPP version | Windows/main.cpp `SET_WINDOW_TITLE`, Core/System.cpp |
| no ImGui debugger, refused at the draw (the flag is saved; a config saved with it open would bring it back) and at its key | UI/ImDebugger/ImDebugger.cpp, UI/NativeApp.cpp |
| no log file | Core/System.cpp |
| no "texture pack loaded" notice and no "shader cache disabled" warning | GPU/Common/TextureReplacer.cpp, UI/EmuScreen.cpp |
| QUIT GAME quits; no F1 to PPSSPP's own pause screen | UI/LCSMenuScreen.cpp |

**Texture dumping stays on**, unlike VCS's game build: lcs.ini `DumpTextures` forces it at every LCS
boot in every build (hd-textures.md), and a player's dumps land in `memstick/PSP/TEXTURES/ULUS10041/
new` (the README asks for them).

Every build wears the game's icon (`Windows/lcs.ico`, from the game's logo) and names
(`Windows/ppsspp.rc`); the resource compiler gets neither _DEBUG nor NDEBUG, so it can't differ.

## The start screen

`CreateStartScreen` (UI/LCSMenuScreen.cpp): the disc given on the command line, else the one booted
last (lcs.ini `GamePath`, written by `EmuScreen::bootComplete` when LCS is active), else one beside
the exe - **each only if it is ULUS10041 version 3.00**, read from the disc's PARAM.SFO through an
ISOFileSystem before anything boots it (`CheckDisc`). Every address this fork changes is that disc's;
another release or version would boot into a game the fork can't touch, so it says what it found
instead: `LCSNoDiscScreen`, in the menu's own look, with CHOOSE DISC (PPSSPP's file browser, the
choice checked the same way) and QUIT. It looks again when the window comes back to the front, and
boots a disc that has appeared. The EBOOT's own hash is checked again as it loads.

Measured on the packaged folder (windowed): nothing beside it, "no disc"; a file of zeroes named
.iso, "not this disc: ... isn't a PSP disc image"; the VCS disc, "vcs.iso is ULUS10160 version 1.03.
This build plays only the USA disc, ULUS10041 version 3.00"; the LCS disc, straight into the
Rockstar logo, the window titled "GTA: Liberty City Stories", no menu bar (`GetMenu` null), and its
lcs.ini's `GamePath` written (so LCS was active: only then is it written).

## No builder's paths

`Windows/fix_2017.props` passes `/d1trimfile:<checkout>` (so `__FILE__` is relative) and
`/PDBALTPATH:%_PDB%` (the .pdb by file name), the VCS fork's two. The packager refuses to finish if
any file it stages contains the builder's home folder. Measured: the exe holds neither the builder's
user name nor the checkout's folder name after a full rebuild (6 minutes).

## The package

`python Tools/lcspackage.py [--zip]` stages `dist/GTA.Liberty.City.Stories.PC`: the exe as
`GTA Liberty City Stories.exe`, `assets/` without the WebSocket debugger's web UI, `memstick/PSP/`
with `SYSTEM` (ppsspp.ini, lcs.ini, controls.ini), the texture pack without its dump folder, and
`LCS/ENGLISH.GXT` (Tools/lcsgxtkeys.py), README.txt and LICENSE.TXT. The docstring lists what is
left out and why. Settings ship as tuned (VCS's lesson: upstream's defaults are a different
program), less this machine's: ppsspp.ini without `[Recent]`, `[PlayTime]`, `CurrentDirectory`, the
window rectangle, the run count, the MAC address, with logging, the debugger and the FPS counter off;
lcs.ini without `GamePath` and the auto-save slot, and with the two inverts at the code's default,
being one person's choice. 861 files, 86 MB.

No `installed.txt` and no `workshop.txt` may be in it, and the script checks: the first would send
the memory stick to Documents, the second would make it the workshop.
