# LCS front end (the pause menu)

`UI/LCSMenuScreen.cpp`: Escape over a running LCS raises this instead of PPSSPP's pause screen. It
is a port of the VCS fork's `VCSMenuScreen` (its `docs/vcs/front-end.md` has the design and the
history): the same page table, the same rows, the value blocks, the hover, the sounds, the bottom
bar. What changed is the look, which is LCS's own pause menu, sampled from the game:

| | from |
|---|---|
| backdrop | the game's pause-menu texture, the bridge in silhouette (`Tools/lcsmenuart.py` crops it from the HD UI pack's `Fronted/Background.png`; the PSP original works too) |
| headings | red `#AE0000`, lowercase Pricedown, top left, drawn as text |
| rows | light blue `#76B0DC`, selected white, in Pricedown, centred |
| bottom bar | the texture's strip colours, `#0C0B10` with a `#1D171F` top edge |
| card headers | the game's tab orange `#FDB336` |
| sounds | `SFX_FE_HIGHLIGHT` / `SELECT` / `BACK` from `AUDIO/SET0/SFX3_PSP.RAW` (`Tools/lcsmenusfx.py`) |

The backdrop is stretched over the screen the way the game stretches it over the PSP's, not
cropped to cover: it is drawn for that, and cropping cut the bridge's deck off. The texture's own
strip (below row 848) is not shipped; the menu draws its bar in the strip's colours instead.

VCS baked its headings to images because they are a brush script PPSSPP cannot draw. LCS's are
Pricedown, which `FontFamily::Display` already draws, so they are text and need no art tool. The
LCS disc keeps the same three front-end sounds under the same names as VCS's; the select blip is
byte-identical.

`CreatePauseScreen` (in the same file) picks this or `GamePauseScreen` on `LCS::IsActive()`; the
three pause sites in `UI/EmuScreen.cpp` call it.

## What is real and what is a placeholder

Placeholders are rows for things to come: drawn dimmed, reachable by the keyboard (so the bar can
say "Coming later" and what they will do), and inert. An option placeholder edits a stand-in value
that is never saved, so no ini key is promised before the feature exists. Making one real is giving
its table row a variable and an ini key.

| page | real | placeholder |
|---|---|---|
| root | RESUME, START NEW GAME (to the game page), CHEATS, OPTIONS, QUIT GAME | MAP, STATS, BRIEF (VCS walks the game's own front end for these) |
| game | LOAD GAME and DELETE GAME list the saves on the memory stick (read from each `ULUS10041S*/PARAM.SFO`) | NEW GAME; loading and deleting a listed save |
| mouse | keyboard and mouse, mouse look, sensitivity, invert, on-foot camera, vehicle camera, vehicle camera follows | |
| controller | | all four rows (the VCS pad scheme) |
| aiming | free aim | aim sensitivity, invert aim, scoped sensitivity |
| audio | sound, master volume (PPSSPP's) | SFX and radio volume (the game's), custom soundtracks |
| display | resolution, fullscreen (PPSSPP's), frame limiter (`FrameRate60`) | brightness, widescreen, show FPS |
| advanced | texture quality, anisotropic filtering (PPSSPP's), anti-aliasing, world memory | shadows, ambient occlusion, water |
| gameplay | | auto-save, GPS route, language, subtitles, HUD |
| key bindings | the cards: on foot, in vehicle, menus, built from `kKeyMappings` (`LCS::KeyListing`) | rebinding, RESET KEYS |
| cheats | all 35 rows | |

Move-while-aiming and direct weapon selection, which VCS has rows for, are left out on purpose.

`VehicleRecentre` is a new `lcs.ini` key (the chase camera's `vehicleRecentre`, which was not saved
before). `LoadSettings` and `SaveSettings` now walk the option table rather than listing keys.

## The cheats

A cheat row queues its eight presses on the PSP pad (`LCS::QueuePadTaps`, the queue Debug -> LCS ->
Pad plays) and closes the menu, because the presses play on the running game. The sequences are the
published PSP ones (grandtheftwiki.com). Measured: FULL ARMOUR from the menu took the player's
armour (`ped+0x4bc`) from 0 to 100, and the three weapon sets were confirmed earlier (free-aim.md).
The rest are unchecked; a row that does nothing is a wrong sequence, one line in `kCheats`.

## Two traps this upstream has that VCS's didn't

- **Keys under a screen still reach the game's layer.** `UIScreen::PassInputToMapper()` returns
  `Keyboard` in this PPSSPP, so `NativeKey` handed key presses made in the menu to
  `LCS::HandleHostKey`: Enter on RESUME was also the game's Start, and the game opened its own menu
  as soon as it ran. The menu returns `InputMode::None`, so only releases go through (a key held as
  the menu opened is still let go). The key pipe (`SendDevKey`) keeps the same gate.
- **`RecreateViews` restores focus by position in the tree.** Every page's rows sit at the same
  place, so a new page opened on the old page's row number. Each page's list is tagged with its own
  name; Back now returns to the row it left from (`lastFocus_`), as the game's menus do.

Also: mouse movement keeps arriving while the menu is up (raw input does not stop), so the menu
drops what the input layer collected (`LCS::DropPendingMouse`) when it opens and closes, or the
camera would jump on resume.

## Dev affordances

- **F1 on the root page** switches to PPSSPP's own pause screen (savestates, its settings). The
  bar says so.
- QUIT GAME does what PPSSPP's Exit does: the game list, or the app with `PauseMenuExitsEmulator`.

## Testing

The pipe drives it (`press Esc` opens it, `Up`/`Down`/`Left`/`Right`/`Enter`). With the ImGui
debugger shown and one of its windows focused, PPSSPP withholds key presses from screens
(`WantCaptureKeyboard`): arrows still move the focus (they are turned into focus moves earlier), but
Enter never activates a row. Test with `ShowImDebugger = False`, or click away from the ImGui
windows first.

Measured on the armed fixture: every page opened and rendered, left/right moved the blocks two
steps and back (`MouseSensitivity` read back 0.0025), F1 and back to the game, QUIT -> NO kept
focus on QUIT, QUIT -> YES reached the game list, Esc resumed with the game's own menu closed.

Not checked: hover and clicks with a real mouse (no synthetic input here; the code is VCS's), the
sounds by ear (the log shows no sample failing to load), a pad.
