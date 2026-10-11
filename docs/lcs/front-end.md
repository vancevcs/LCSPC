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
| root | RESUME, START NEW GAME (to the game page), MAP, STATS, BRIEF (the game's own pages), CHEATS, OPTIONS, QUIT GAME | |
| game | NEW GAME, LOAD GAME and DELETE GAME: the saves on the memory stick (each `ULUS10041S*/PARAM.SFO`), `*` on the newest, `(AUTO)` on the auto-save's slot | |
| mouse | keyboard and mouse, mouse look, sensitivity, invert, on-foot camera, vehicle camera, vehicle camera follows | |
| controller | look sensitivity and invert look (the game's, for the stick), stick deadzone (PPSSPP's) | controller scheme (the VCS pad scheme) |
| aiming | free aim, aim sensitivity, invert aim (free aim's own channel), sniper and RPG sensitivity (the scope, free-aim.md) | |
| audio | sound, master volume (PPSSPP's), SFX and radio volume (the game's) | custom soundtracks (the game's setter has to be called) |
| display | brightness (the game's), resolution, fullscreen, show FPS (PPSSPP's), frame limiter (`FrameRate60`) | widescreen |
| advanced | texture quality, anisotropic filtering (PPSSPP's), anti-aliasing, world memory | shadows, ambient occlusion, water |
| gameplay | auto-save, GPS route (gps.md), language (next boot), subtitles, HUD, radar (the game's) | |
| key bindings | the cards: on foot, in vehicle, menus, built from `kKeyMappings` (`LCS::KeyListing`); any line rebound by clicking it; RESET KEYS | |
| cheats | all 35 rows | |

Move-while-aiming and direct weapon selection, which VCS has rows for, are left out on purpose.

`VehicleRecentre` is a new `lcs.ini` key (the chase camera's `vehicleRecentre`, which was not saved
before). `LoadSettings` and `SaveSettings` now walk the option table rather than listing keys.

A row on `OptionPage::Developer` is saved and loaded like any other but is on no menu page:
`DumpTextures` (hd-textures.md) is one, edited in Debug -> LCS.

## The game's own pages

`Core/LCS/LCSFrontEnd.cpp`, the VCS fork's `VCSFrontEnd` in LCS's terms. LCS keeps its map, brief,
stats and save list on the pause menu Start opens, and this fork's menu took the pause key, so the
MAP, STATS and BRIEF rows open the game's menu and walk it to the page. The row asks and closes this
menu (it pauses the emulator, and the walk needs the game running); `FrontEndTick` does the rest.

**LCS's front end is a GTA III page table, not VCS's widget tree.** No widget names in the EBOOT;
`FrontEndMenuManager` (`0x08b8ee20`) holds the tab the strip is on (`+0x00`, 1..8), the page
(`+0x564`), the selected entry (`+0x568`) and whether the page or the strip has focus (`+0x590`).
The page table is at `0x08b316b4` (docs/LCS_ADDRESSES.md "The game's front end"). The tabs are the
same grid as VCS's: map, brief, game, stats, controls along the top, audio, display, multiplayer
below, with right wrapping inside a row; the game's own neighbour table (`0x08b5905c`, a second one
for Spanish) says which press leads where, and the walk searches it for the shortest way.

**It presses; it never writes the page.** Start until `MenuActive`, the d-pad until the tab shows
the page, Cross until the page has focus - each press watched until it lands. Two things make that
work:

- **The menu takes no input while it fades.** Every direction getter returns 0 until `+0x141` is
  set (d-pad and stick released, the fade at `+0x18` done), and a page fades in at `+0x584` after
  each tab change. Measured: ~0.3 s after opening, ~0.1 s per tab. A press made earlier is dropped
  without a trace, and the first build lost one per tab and waited out a timeout each time (STATS in
  3.8 s). Pressing only when all three say so: MAP up in 1 s, STATS (two tabs) in about 1.3.
- **Stepping into the page is what takes the tabs away.** The strip is only drawn while `+0x590` is
  0. The button hints are one call, `jal 0x08adf0c0` at `0x08ae5114`, NOPed while the bridge shows a
  page (`kLCSMenuHintsCall`) and put back the moment it doesn't - and by the tick after a savestate
  brings the patch back with nothing showing.

What the hints said is drawn by this fork instead, in the same strip (the PSP picture's bottom 48
lines, through `CalculateDisplayOutputRect`): the keys that work on that page (input.md "The game's
own menu"). The zone name the map draws there stays.

**The pages with a mouse, and from the first look** (reported: the map didn't move with the mouse
as VCS's does, STATS didn't scroll, BRIEF was a blank page that took nothing):

- **The map drags.** With the left button held on the map page, the mouse moves the map's centre
  (`+0x474` / `+0x478`, in the PSP's screen units around (240, 114)) a quarter of a unit a count:
  four 1080p pixels a unit, so about a pixel a count, and the city follows the pointer. It is kept
  within the game's own scroll limit, size - 130 either side of the middle (`0x08adae64`). Nothing
  derives the centre, so a written one stays. Measured on a settled page: 40 counts down moved the
  centre +10.0, 40 right +10.0; 20 units moved the picture 80 px.
- **The wheel scrolls STATS and BRIEF**: two STATS lines or one brief a notch, written into the
  offset each page's draw scrolls by (`0x08b59cb4`, `0x08b59c40`), which the game moves only while
  a direction is held. Down brings up what is below, as a PC list does; the game's own down moves
  STATS the other way.
- **STATS opens at its first line.** Its offset is kept from the last look, and the first look
  showed the first line alone at the bottom of an empty page; the bridge writes 136, the first line
  at the top, as the page arrives.
- **An empty BRIEF says so.** It lists the missions' dialogue since the game was loaded
  (`CMessages::PreviousBriefs`), so after a load there is nothing on it. This fork then draws NO
  BRIEFS YET and what will go there over the page.

Measured after a fresh boot into a save: STATS opened at 136 with its first line at the top, a
notch down took it to 166 and the list up two lines; BRIEF's 20 entries all read 0 and the page
showed NO BRIEFS YET. With a brief on it (a cheat's), BRIEF drew it as before.

**Back comes back here.** Escape or Backspace on one of those pages shuts the game's menu (Start,
watched until `MenuActive` drops) and this menu comes back on the row it was opened from; the next
Escape is the world. `CreatePauseScreen` makes that decision: it returns null when it closed the
game's menu instead, or while a sequence or the firmware's save dialog is up. The game's own menu
opened with Enter (its Start) is closed by Escape without this menu coming back. A curtain with no
word covers the frames between the two menus.

## Loading, a new game, deleting

All three are this menu's own pages and questions (the game's words: `FESZ_QR`, `FESZ_QL`).

- **LOAD** walks to the Game page, presses LOAD GAME, steers the firmware's list to the save picked
  here, answers the game's "load over this game?" (page 10) with YES, and waits for the world. The
  load list holds only the saves that exist, so the bridge steers by save name ("S3"), which
  `PSPSaveDialog::LCSPeek` reports per entry (`Core/LCS/LCSSaveDialog.h`). Measured: about 3 s from
  the click to the load starting, then the game's own load behind the curtain.
- **NEW GAME** walks to NEW GAME, then YES on the question (page 9, rows text / NO / YES). The world
  is torn down by that press, so nothing of ours is patched when it lands. Measured: the intro
  started 1.4 s after the click, frame counter reset, camera mode 17.
- **DELETE** removes `ULUS10041S<n>` from the memory stick here. The game lists the stick again every
  time it opens a list, so nothing in the game needs telling.

## The auto-save

After every story mission, into the game's last save slot (`S7`), so it can only overwrite the
previous auto-save. `AutoSave` in lcs.ini, the Gameplay page.

**The trigger:** `register_mission_passed` (031D) copies the mission's GXT key to `0x08b5e398` and
adds one to `0x08b5e2c0`; odd jobs (059A) add one and leave the key. The key is saved and loaded,
the counter isn't. So "the counter rose and the key changed" is a story mission passed and nothing
else: a load brings its own key without the counter rising (checked: loading two saves fired
nothing). The save waits 150 game frames, then for a moment the game would let you save in - no
mission running (`$560` 0), not in a cutscene (camera mode 17), alive, no menu - for at most 1800.

**What a save is made of.** The request byte `FrontEndMenuManager+0x46b` (what `activate_save_menu`,
03DD, sets) opens the game's save list; it doesn't prepare a save. The safe house's routine (MAIN.SCM
`0x176d5`) does that first, and the script's boot check starts the intro unless `$6` is 1 (`0x6933`):
VCS's `$4` trap again. So the bridge does what the routine does: `$560` = 1 for the save (and back
after), `$6` = 1, `$7..$9` = `$537..$539` (the safe house the load path puts you in), every weapon
type's ammo into `$124..$156` (`kLCSSavedWeapons`, the routine's own table: the load path,
`INITSAV`, clears the weapons and gives these back), the armed weapon into `$158`, armour into
`$160`. Loading such a save puts you at your last safe house with your weapons, as a safe-house save
does. Script globals are `[0x08b38d7c] + N*4` (LCS's operand bytes `0xcc`.. are globals; VCS's start
at `0xcd`).

Then the dialog: the list steered to the last entry, the overwrite prompt moved to YES (it opens on
NO), "save completed" answered with Back (it takes nothing else), and the game's menu closed if it
is still up. Measured with a simulated mission pass: `S7` written 5 s later (150 frames), and
loading it back came up in the Saint Mark's safe house with `$6` 1 and `$560` 0.

**A save the game names after its last mission** shows here by that name; one with none (only a
test's fake key) shows as SAVED GAME.

**Never the cheat warning.** With a cheat used (every cheat sets the byte at `0x08b56432`) the save
menu opened on page 11, "Warning! One or more cheats have been activated" (`FET_WRN`), instead of
the save list. That page waits for a press the auto-save never makes, and the game sat on it with
nothing answering but Alt + F4 (reported). Its only caller (`0x08ad9494`) branches on the byte;
the branch at `0x08ad94b4` is made unconditional (`kLCSSaveCheatWarning`), every tick, because a
savestate brings the game's op back. So the save list always opens, for the auto-save and for a
safe house's save alike. Measured: weapon set 1 took the byte from 0 to 1, and the save request then
opened page 12 and stayed on it for the 3 s watched.

## Boot

The game opens on two movies, the logos and then the credits (about 26 s), then the legal card, the
firmware's "Loading from Memory Stick" while the newest save autoloads, and its own loading screen.
Both of these were in the VCS fork and reported missing here:

- **Any key skips the intro**, Shift included: every key and mouse button but Escape (PPSSPP's
  pause), the F keys and the wheel, while the movies play (`LCS::IntroPlaying`). The press goes
  nowhere else, and its release with it. Cross skips the movie that is playing, so a skip taps
  Cross, 6 vblanks in every 30, until both are gone, for at most 15 s (`IntroTick`).
- **LOADING covers the rest.** The movies end where the game opens `ENTRIES.DAT`, by sector
  (`disc0:/sce_lbn0x6450_size0x30a8`), once per boot; `sceIoOpen` hands every name to
  `LCS::NoteFileOpen`. That raises the curtain with LOADING, which comes down as any other does, 30
  game frames into the world. A world that runs (a savestate boots straight into one) is past the
  intro.

Measured on a boot into a save, captured every 0.8 s: one Shift press at 4.2 s, during the logos;
the intro over 0.8 s later; LOADING up by 5.7 s; the world running at 9.3 s and the curtain gone by
11.8 s. Untouched, the credits end at 26 s, and `ENTRIES.DAT` is opened that moment.

## The curtain

Over every sequence the player didn't watch, and at boot from the intro's end to the world (Boot):
the menu's backdrop over the whole screen and LOADING
or SAVING bottom left with walking dots (the VCS fork's loading screen with LCS's backdrop), or the
backdrop alone while a page is opened or closed. Drawn by EmuScreen (`DrawLCSOverlay`), not pushed
as a screen, because a screen would pause the game it is hiding. Keys are swallowed while it is up
(`NativeKey`; releases still go through). It comes down 30 game frames after its sequence ends -
the game's clock stops while the world loads, so that counts from the first frame of the new world -
and fades out over half a second; a ceiling of 2400 vblanks keeps it from ever staying.

`EmuScreen::hasVisibleUI` has to say yes for it: without it `renderUI` never runs over the game, and
the first build drew nothing at all.

## The game's own settings

Brightness, subtitles, HUD mode, radar mode, SFX and radio volume, and for the stick the game's
look speed and invert look, are the game's own (its Display, Audio and Controls pages, which this
menu stands in for). They are mirrored in lcs.ini and pushed into the game whenever it disagrees,
which also puts them back after a save loads its own. Each volume is two values: the preference the
save keeps and the byte the mixer reads (`0x08b89d94` / `95`, which the page's setter writes) - both
are written on a change, and only then, so the game's own fades are left alone. While the bridge
presses buttons the live SFX level is held at 0, so the walk doesn't blip.

Language is the PSP system language the game asks for at boot (`0x089c0e28`), so the row hands the
game its choice there (`sceUtilityGetSystemParamInt`) and takes effect at the next boot.

Show FPS is PPSSPP's own counter bit; the row reads it back each time the page is built.

## Key bindings

Enter on a line of a key card, or a click, and its keyboard cell says PRESS A KEY; the next key
pressed is that line's (a line of several keys, WASD, asks once per direction). Escape cancels; the
wheel isn't taken. input.md "Rebinding" has the rules. RESET KEYS puts every context back. Measured:
on-foot Jump onto K made K the game's Square and Space nothing; then onto F, and Enter Vehicle took K.

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
