// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

// A bridge into the game's own front end, the VCS fork's VCSFrontEnd in LCS's terms
// (docs/lcs/front-end.md "The game's own pages").
//
// LCS keeps its map, brief, stats and save list on the pause menu Start opens, and this fork's own
// menu took the pause key. So the map, brief and stats rows open the game's menu, walk its tab
// strip to the page, step into it (which is what takes the tabs away) and hide the button hints;
// LOAD and NEW GAME walk on to the game's own Game page; an auto-save asks for the game's save
// menu the way a safe house does. The bridge presses the buttons a player would and watches the
// menu's own fields until each press has landed: it never writes the page.
//
// LCS's front end is a GTA III page table, not VCS's named widget tree: FrontEndMenuManager at
// 0x08b8ee20 holds the tab (+0x00), the page (+0x564), the selected entry (+0x568) and whether the
// page or the tab strip has focus (+0x590). docs/LCS_ADDRESSES.md "The game's front end".
//
// Threads: the requests and the queries marked "any thread" are atomics; everything else runs on
// the emu thread from LCS::Tick, which is the only place PSP memory and the pad are touched.

namespace LCS {

// The pages the root menu's MAP, BRIEF and STATS rows show.
enum class GamePage {
	Map,
	Brief,
	Stats,
};

// What is over the screen while a sequence the player didn't watch runs: the menu's backdrop with
// LOADING or SAVING on it, or with nothing (Menu: a page of the game's menu being opened or closed
// for them). It also silences the keys (NativeKey), so nothing pressed lands in a page being walked.
enum class CurtainKind {
	None,
	Loading,
	Saving,
	Menu,
};

// --- Requests (any thread; queued, done on the far side of closing this fork's menu) ---

void RequestGamePage(GamePage page);
// Shut the game's menu. With `reopenMenu`, this fork's menu comes back once it has gone
// (TakeMenuAfterClose): Back from one of the game's pages is the menu it was reached from.
void RequestCloseGameMenu(bool reopenMenu);
// Load the save in ULUS10041S<slot>, through the game's LOAD GAME and the firmware's list.
void RequestLoadSlot(int slot);
// Start the story again, through the game's NEW GAME.
void RequestNewGame();
// Save now, as the auto-save would (Debug -> LCS).
void RequestAutoSave();

// --- Queries (any thread) ---

// The game's pause menu is up.
bool GameMenuUp();
// The menu on screen is one this bridge opened onto a page, which is when Back and Escape lead
// back to this fork's menu and the hints are hidden.
bool BridgeOwnsMenu();
// The page the bridge is showing, or -1.
int OwnedGamePage();
// A sequence is running (pressing, walking, or driving the save dialog).
bool FrontEndBusy();
// A savedata dialog is on screen, the bridge's or the game's.
bool SaveDialogUp();
CurtainKind ActiveCurtain();
// Take the curtain down from outside: a screen that pauses the emulator stops the tick that would.
void DropCurtain();
// True once after a RequestCloseGameMenu(true) has finished. UI thread.
bool TakeMenuAfterClose();
// The save slot the auto-save writes (the game's last), or -1 before the save list was seen.
int AutoSaveSlot();

// The save icon: the game's own save list is up with nothing of ours running. True once; the UI
// puts this fork's SAVE GAME page over it, and answers with a slot 0..7, or -1 for no save.
bool TakeIconSave();
void AnswerIconSave(int slot);

// --- Boot: the intro before the world ---

// The game's movies (logos, credits) are playing: from boot until they are over or a world runs.
bool IntroPlaying();
// Skip them: Cross tapped until both are gone (a tap skips the one playing). Any thread.
void RequestIntroSkip();
// From sceIoOpen, emu thread. The file the game opens as the movies end raises LOADING, which comes
// down once the world (the autoloaded save, or the story's start) has run a moment.
void NoteFileOpen(const char *filename);

// --- The game's own pages, for the input layer ---

// Emu thread. STATS or BRIEF scrolled by wheel notches (positive up); false when neither has focus.
bool ScrollGamePage(int notches);
// Emu thread. The map page moved by a mouse movement in counts; false when it isn't up.
bool DragMap(float dx, float dy);
// Any thread: the map page has focus (a drag wants the mouse); BRIEF is up with nothing on it.
bool MapPageUp();
bool BriefEmpty();

// --- Emu thread ---

// Advances the bridge, the auto-save watch, the curtain and the game's own settings. From
// LCS::Tick, before InputTick (which applies FrontEndButtonMask).
void FrontEndTick();
// The PSP buttons the bridge holds this tick; the player's own stand down while FrontEndDriving.
u32 FrontEndButtonMask();
bool FrontEndDriving();
// From LCS::OnFrameBoundary, between frames: what a save needs written while no script runs.
void FrontEndFrameBoundary();
// Abandons everything and puts the game's code back. Init, shutdown.
void FrontEndReset();
// One phrase for Debug -> LCS.
const char *FrontEndStatus();
// The latest story mission's GXT key and how many game frames the auto-save still waits.
void LatestMissionKey(char *out, size_t size);
int AutoSaveDelayLeft();

}  // namespace LCS
