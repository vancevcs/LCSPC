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

// The LCS front end: the pause menu Escape raises over a running GTA: Liberty City Stories, in
// place of PPSSPP's own. A port of the VCS fork's VCSMenuScreen - the same page table, rows,
// blocks, sounds and bottom bar - in LCS's colours, over the game's own pause-menu backdrop.
// docs/lcs/front-end.md has what is real and what is a placeholder.
//
// Every other game still gets GamePauseScreen: CreatePauseScreen decides, on LCS::IsActive.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/View.h"
#include "Core/LCS/LCSInput.h"
#include "UI/BaseScreens.h"

namespace LCS {
struct Option;
enum class OptionPage;
enum class GamePage;
}

enum class LCSMenuPage {
	Root,
	Settings,
	Controls,
	Mouse,
	Controller,
	Aiming,
	Audio,
	Graphics,
	GraphicsAdvanced,
	Gameplay,
	Game,
	LoadGame,
	DeleteGame,
	// Only over the firmware's save list, when a safe house's save icon opened it (LCS::TakeIconSave).
	// Not reachable from the root: saving is something you do in the world.
	SaveGame,
	Confirm,
	Bindings,
	KeysOnFoot,
	KeysVehicle,
	KeysMenu,
	Cheats,
	CheatsPlayer,
	CheatsVehicles,
	CheatsPedestrians,
	CheatsWorld,
};

// One row: a label that does something when chosen, or a setting with its value beside it.
// Hover selects, as on a PC; left and right edit; the selection is carried by colour alone.
class LCSMenuItem : public UI::ClickableItem {
public:
	LCSMenuItem(std::string_view label, UI::LayoutParams *layoutParams = nullptr);
	LCSMenuItem(const LCS::Option *option, UI::LayoutParams *layoutParams = nullptr);

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;
	bool Key(const KeyInput &input) override;
	bool Touch(const TouchInput &input) override;
	void FocusChanged(UI::FocusFlags focusFlags) override;
	std::string DescribeText() const override;

	const LCS::Option *option() const { return option_; }
	std::string help() const;
	void SetHelp(std::string_view help) { help_ = help; }
	// Left-aligned at x instead of centred: a list of saves reads as a column.
	void SetLeftAligned(float x) { leftAlignX_ = x; }
	// Something to come: drawn dimmed, reachable, and inert. An option row is one when its table
	// entry says so.
	void SetPlaceholder() { placeholder_ = true; }
	bool IsPlaceholder() const;

protected:
	void ClickInternal() override;

private:
	Bounds HitBounds() const;
	void ClaimFocus();
	Bounds BlockStripBounds() const;
	float ValueFractionAt(float x) const;
	void Adjust(int direction);
	void Cycle();

	std::string label_;
	std::string help_;
	const LCS::Option *option_ = nullptr;
	float leftAlignX_ = -1.0f;
	bool placeholder_ = false;
	bool draggingValue_ = false;
	// The glyphs' box, measured in Draw: rows are as wide as the screen, and hover and clicks
	// should only land on the words.
	mutable Bounds hitBounds_;
	mutable bool hitBoundsValid_ = false;
};

// One line of a controls card: an action, its keys, its PSP button. A click asks for a new key.
class LCSBindingRow : public UI::ClickableItem {
public:
	explicit LCSBindingRow(const LCS::KeyListingRow &row, UI::LayoutParams *layoutParams = nullptr);

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;
	bool Touch(const TouchInput &input) override;
	void FocusChanged(UI::FocusFlags focusFlags) override;
	std::string DescribeText() const override;
	// Waiting for a key: the keyboard cell shows `text` instead of the keys.
	void SetCapturing(bool capturing, const std::string &text) { capturing_ = capturing; captureText_ = text; }

protected:
	void ClickInternal() override;

private:
	std::string name_;
	std::vector<std::string> cells_;
	bool bindable_ = false;
	bool capturing_ = false;
	std::string captureText_;
};

class LCSMenuScreen : public UIBaseDialogScreen {
public:
	LCSMenuScreen(const Path &gamePath, bool bootPending);
	~LCSMenuScreen();

	const char *tag() const override { return "LCSMenu"; }

	bool key(const KeyInput &key) override;
	// None: no key goes on to the game's layer while this is up, only releases (NativeKey lets
	// those through, so a key held as the menu opened is still let go). UIScreen's default passes
	// the keyboard, and then Enter on RESUME was also the game's Start once it ran again.
	InputMode PassInputToMapper() const override { return InputMode::None; }
	void deviceLost() override;
	void deviceRestored(Draw::DrawContext *draw) override;

protected:
	void CreateViews() override;
	void update() override;
	void DrawBackground(UIContext &dc) override;
	ViewLayoutMode LayoutMode() const override { return ViewLayoutMode::ApplyInsets; }

private:
	void GoToPage(LCSMenuPage page);
	LCSMenuPage ParentPage(LCSMenuPage page) const;
	const char *PageTitle(LCSMenuPage page) const;
	bool GoBack();
	void CancelIconSave();

public:
	void OpenForIconSave();

private:
	void GoToConfirm(const char *title, std::string question, std::string detail,
		std::function<void()> action);

	static bool IsOptionPage(LCSMenuPage page);
	static LCS::OptionPage ToOptionPage(LCSMenuPage page);
	static bool IsKeyListPage(LCSMenuPage page);
	static LCS::InputContext ToKeyContext(LCSMenuPage page);
	static bool IsCheatPage(LCSMenuPage page);

	LCSMenuItem *AddRow(UI::ViewGroup *parent, const char *label, const char *help = nullptr);
	void AddPageRow(UI::ViewGroup *parent, const char *label, LCSMenuPage target);
	void AddGamePageRow(UI::ViewGroup *parent, const char *label, const char *help, LCS::GamePage page);
	void AddPlaceholderRow(UI::ViewGroup *parent, const char *label, const char *help);
	void AddBackRow(UI::ViewGroup *parent);
	void AddOptionRows(UI::ViewGroup *parent, LCSMenuPage page);
	void AddSaveRows(UI::ViewGroup *parent, bool deleting);
	void AddSaveSlotRows(UI::ViewGroup *parent);
	void AddBindingRows(UI::ViewGroup *parent, LCSMenuPage page);
	void AddCheatRows(UI::ViewGroup *parent, LCSMenuPage page);
	void FitRows(UI::LinearLayout *list);
	void StartCapture(int line);
	void ShowCapture();
	bool CaptureKey(const KeyInput &key);
	Bounds ListPanel() const;

	void OnResume(UI::EventParams &e);
	void OnExitGame(UI::EventParams &e);
	void OnRestoreDefaults(UI::EventParams &e);

	bool bootPending_;
	LCSMenuPage page_ = LCSMenuPage::Root;

	// The confirmation page: whatever asked, what it asked, and what YES does. YES only records
	// what to do; update() does it, because a click handler that rebuilds or finishes the screen
	// it is running in is how the VCS menu once stopped taking input.
	const char *confirmTitle_ = "paused";
	std::string confirmQuestion_;
	std::string confirmDetail_;
	std::function<void()> confirmAction_;
	LCSMenuPage confirmParent_ = LCSMenuPage::Root;
	bool pendingExit_ = false;
	bool pendingEmulatorMenu_ = false;
	// A save slot to load or delete, and a new game: what the question's YES asked for.
	int pendingLoad_ = -1;
	int pendingDelete_ = -1;
	bool pendingNewGame_ = false;
	// The save icon's page: open for it, answered, and the slot its question's YES chose.
	bool iconSave_ = false;
	bool iconSaveAnswered_ = false;
	int pendingSaveSlot_ = -1;

	std::vector<LCSMenuItem *> rows_;
	std::vector<LCSBindingRow *> bindingRows_;
	// The key card on screen, and the line (and the key within it) waiting for a key, or -1.
	std::vector<LCS::KeyListingRow> listing_;
	int captureLine_ = -1;
	int capturePart_ = 0;
	int bindingFocus_ = -1;
	bool pendingRebuild_ = false;
	// The row each page was left on, so Back lands where you came from rather than at the top.
	std::map<LCSMenuPage, int> lastFocus_;
	int listRowCount_ = 0;
	float listRowHeight_ = 0.0f;
	std::unique_ptr<struct LCSMenuArt> art_;
};

// The game build's first screen when there is no disc of this build's to boot: what was found and why
// it isn't the one, CHOOSE DISC and QUIT, in the menu's own look. Looks again whenever the app comes to
// the front, and boots a disc that has appeared beside the game.
class LCSNoDiscScreen : public UIScreen {
public:
	explicit LCSNoDiscScreen(std::string finding);
	~LCSNoDiscScreen();
	const char *tag() const override { return "LCSNoDisc"; }
	void sendMessage(UIMessage message, const char *value) override;
	void deviceLost() override;
	void deviceRestored(Draw::DrawContext *draw) override;

protected:
	void CreateViews() override;
	void DrawBackground(UIContext &dc) override;

private:
	void Boot(const Path &path);
	std::string finding_;
	LCSMenuItem *choose_ = nullptr;
	std::unique_ptr<struct LCSMenuArt> art_;
};

// The game build's start (LCS::PresentAsGame), in place of PPSSPP's logo and game list: the disc given
// on the command line, else the one booted last (lcs.ini GamePath), else one beside the game - each
// only if it is this build's disc, ULUS10041 version 3.00 - or LCSNoDiscScreen saying what it found.
UIScreen *CreateStartScreen(const Path &bootFile);

// The pause screen for whatever is running: this menu over LCS, PPSSPP's own otherwise. Called
// from EmuScreen's pause sites. Null when the LCS front end took the pause itself: closing the
// game's own menu, or ignoring it while a sequence walks that menu.
UIScreen *CreatePauseScreen(const Path &gamePath, bool bootPending);
// True once when this menu should come back: Back from one of the game's pages it opened, after the
// page has gone. EmuScreen::update asks every frame.
bool LCSMenuDue();
// This menu on its SAVE GAME page, once, when a safe house's save icon put up the firmware's save
// list (LCS::TakeIconSave); null otherwise. EmuScreen::update asks every frame.
UIScreen *TakeIconSaveScreen(const Path &gamePath, bool bootPending);
// The LCS front end's curtain (LOADING, SAVING) and the hint line on the game's pages, over the game.
// From EmuScreen::renderUI; nothing for any other game.
void DrawLCSOverlay(UIContext &dc);
// Whether DrawLCSOverlay has anything to draw (EmuScreen skips its UI pass otherwise).
bool LCSOverlayVisible();
void ReleaseLCSOverlayArt();
