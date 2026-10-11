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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

#include "Common/Common.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSFrontEnd.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSSaveDialog.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"

namespace LCS {

namespace {

// --- Shared with the UI thread ---

enum class Req {
	None,
	Page,
	Close,
	CloseReopen,
	Load,
	NewGame,
	Save,
};

std::atomic<int> g_request{(int)Req::None};
std::atomic<int> g_requestArg{-1};
std::atomic<bool> g_menuUp{false};
std::atomic<bool> g_owned{false};
std::atomic<int> g_ownedPage{-1};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_dialogUp{false};
std::atomic<bool> g_menuAfterClose{false};
std::atomic<int> g_curtain{(int)CurtainKind::None};
std::atomic<int> g_autoSaveSlot{-1};
std::atomic<bool> g_introPlaying{false};
std::atomic<bool> g_introSkipRequested{false};
std::atomic<bool> g_mapPageUp{false};
std::atomic<bool> g_briefEmpty{false};

// --- Emu thread ---

enum class Phase {
	Idle,
	CloseFirst,    // the game's menu was already up: shut it, then open it again for the walk
	OpenMenu,      // Start, until the menu is up
	Walk,          // the d-pad along the tab strip, until the tab shows the page
	Enter,         // Cross, until the page rather than the strip has focus
	Owned,         // a page is showing for the player; nothing pressed
	LoadPick,      // LOAD GAME on the Game page, until the save list is up
	NewGamePick,   // NEW GAME on the Game page, until the question is up
	NewGameAsk,    // YES on the question, then the one press that ends this world
	Dialog,        // the firmware's save list and prompts
	Settle,        // a load or a new game is under way: wait for the menu to go
	Close,         // Start, until the menu is gone
};

const char *PhaseName(Phase phase) {
	switch (phase) {
	case Phase::Idle: return "idle";
	case Phase::CloseFirst: return "closing the game's menu first";
	case Phase::OpenMenu: return "opening the game's menu";
	case Phase::Walk: return "walking the tab strip";
	case Phase::Enter: return "stepping into the page";
	case Phase::Owned: return "showing a page";
	case Phase::LoadPick: return "LOAD GAME";
	case Phase::NewGamePick: return "NEW GAME";
	case Phase::NewGameAsk: return "answering YES";
	case Phase::Dialog: return "in the save dialog";
	case Phase::Settle: return "waiting for the world";
	case Phase::Close: return "closing the game's menu";
	}
	return "?";
}

// Vblanks. The game samples the pad once a frame (two vblanks at 30 fps), so a press is held long
// enough to be seen and released long enough that two of the same button are two presses.
constexpr int kHoldVblanks = 4;
constexpr int kGapVblanks = 6;
// How long one press may take to show in the menu's fields before it is made again.
constexpr int kStepTimeout = 45;
// Start can be declined (a cutscene keeps the menu to itself); it is tried this often.
constexpr int kOpenTimeout = 120;
constexpr int kMaxStartAttempts = 3;
// Presses one sequence may spend before giving up: a lap of the tabs is five.
constexpr int kMaxPresses = 24;
// The firmware dialog: how long to wait for it to appear, and for the whole of it.
constexpr int kDialogAppearTimeout = 600;
constexpr int kDialogTimeout = 3600;
// A load or a new game rebuilds the world; the menu goes when it has.
constexpr int kSettleTimeout = 3600;
// Everything up to the save dialog or the settle, together: the walk waits for the menu to be ready,
// and a real pad's stick held off centre keeps it from ever being.
constexpr int kWalkTimeout = 1200;
// The curtain comes down this many game frames after its sequence ends - the game's clock doesn't
// run while the world loads, so these count from the first frame there is a world again - and no
// later than the ceiling, because it holds the keys.
constexpr int kCurtainSettleFrames = 30;
constexpr int kCurtainMaxVblanks = 2400;
// A story mission ends into its fanfare, a phone call, a cutscene; the save waits this many game
// frames, then for a moment the save can be made in, for at most kAutoSavePatience.
constexpr int kAutoSaveDelayFrames = 150;
constexpr int kAutoSavePatience = 1800;
// FOLLOWPED-free camera modes that mean a cutscene or a fixed shot is running.
constexpr int kCamModeFlyby = 17;

Phase g_phase = Phase::Idle;
Req g_target = Req::None;
int g_wantPage = -1;
int g_wantSlot = -1;
bool g_reopen = false;
const char *g_status = "idle";
int g_waited = 0;
int g_presses = 0;
int g_attempts = 0;
int g_fromTab = -1;
int g_seqVblanks = 0;
// The phase's first press has been made: later ones only after a step timeout.
bool g_stepPressed = false;
bool g_sawDialog = false;
bool g_saveIsAuto = false;

// The press in flight, and the last one made.
u32 g_mask = 0;
u32 g_lastPress = 0;
int g_pressHold = 0;
int g_pressGap = 0;

bool g_hintsHidden = false;

// The curtain's countdown.
bool g_curtainSequenceDone = false;
int g_curtainSettle = 0;
int g_curtainVblanks = 0;
u32 g_curtainFrame = 0;
bool g_haveCurtainFrame = false;

// The auto-save watch.
u32 g_missionCount = 0;
u64 g_missionKey = 0;
bool g_haveMission = false;
bool g_autoSaveArmed = false;
int g_autoSaveDelay = 0;
int g_autoSaveWaited = 0;
u32 g_watchFrame = 0;
bool g_haveWatchFrame = false;

// The live SFX level is held at 0 while the bridge drives.
bool g_quiet = false;

// The script state a save is made with, and what it was before.
bool g_savePrepared = false;
u32 g_saveSpace = 0;
u32 g_savedOnMission = 0;

struct FeState {
	bool valid = false;
	bool up = false;
	int tab = -1;
	int page = -1;
	int row = -1;
	bool inPage = false;
	// The menu will see a press now. One made during its fades is dropped without a trace.
	bool ready = false;
};

FeState ReadFe() {
	FeState fe;
	const std::optional<u8> up = ReadU8(kLCSFrontEnd + kLCSFeActiveOffset);
	const std::optional<s32> tab = ReadS32(kLCSFrontEnd + kLCSFeTabOffset);
	const std::optional<s32> page = ReadS32(kLCSFrontEnd + kLCSFePageOffset);
	const std::optional<s32> row = ReadS32(kLCSFrontEnd + kLCSFeRowOffset);
	const std::optional<u8> inPage = ReadU8(kLCSFrontEnd + kLCSFeInPageOffset);
	if (!up || !tab || !page || !row || !inPage) {
		return fe;
	}
	fe.valid = true;
	fe.up = *up != 0;
	fe.tab = *tab;
	fe.page = *page;
	fe.row = *row;
	fe.inPage = *inPage != 0;
	fe.ready = ReadU8(kLCSFrontEnd + kLCSFeInputReadyOffset).value_or(0) != 0 &&
		ReadS32(kLCSFrontEnd + kLCSFeFadeOffset).value_or(0) >= 255 &&
		ReadS32(kLCSFrontEnd + kLCSFePageFadeOffset).value_or(0) >= 255;
	return fe;
}

bool Spanish() {
	return ReadS32(kLCSLanguage).value_or(0) == 4;
}

int TabForPage(int page) {
	const int *pages = Spanish() ? kLCSTabPageSpanish : kLCSTabPage;
	for (int tab = 1; tab <= kLCSTabCount; tab++) {
		if (pages[tab] == page) {
			return tab;
		}
	}
	return -1;
}

// The first d-pad press on the way from one tab to another, by a breadth-first search of the game's
// own neighbour table; 0 when there is no way.
u32 TabStep(int from, int to) {
	static const u32 kDirs[4] = { CTRL_UP, CTRL_DOWN, CTRL_LEFT, CTRL_RIGHT };
	const u32 grid = Spanish() ? kLCSTabGridSpanish : kLCSTabGrid;
	if (from < 1 || from > kLCSTabCount || to < 1 || to > kLCSTabCount || from == to) {
		return 0;
	}
	int firstDir[kLCSTabCount + 1];
	std::fill(std::begin(firstDir), std::end(firstDir), -1);
	int queue[kLCSTabCount + 1];
	int head = 0, tail = 0;
	queue[tail++] = from;
	firstDir[from] = 4;
	while (head < tail) {
		const int tab = queue[head++];
		for (int dir = 0; dir < 4; dir++) {
			const std::optional<s32> next = ReadS32(grid + tab * kLCSTabGridStride + dir * 4);
			if (!next || *next < 1 || *next > kLCSTabCount || firstDir[*next] != -1) {
				continue;
			}
			firstDir[*next] = tab == from ? dir : firstDir[tab];
			if (*next == to) {
				return kDirs[firstDir[*next]];
			}
			queue[tail++] = *next;
		}
	}
	return 0;
}

// --- Pressing ---

bool Pressing() {
	return g_pressHold > 0 || g_pressGap > 0;
}

void Press(u32 mask) {
	g_mask = mask;
	g_lastPress = mask;
	g_pressHold = kHoldVblanks;
	g_pressGap = kGapVblanks;
	g_presses++;
	g_waited = 0;
}

void StepPress() {
	if (g_pressHold > 0) {
		if (--g_pressHold == 0) {
			g_mask = 0;
		}
	} else if (g_pressGap > 0) {
		g_pressGap--;
	}
}

// --- The hints ---

void SetHintsHidden(bool hidden) {
	if (hidden == g_hintsHidden) {
		return;
	}
	if (SetCodeWord(kLCSMenuHintsCall, hidden)) {
		g_hintsHidden = hidden;
	}
}

// --- The curtain ---

void RaiseCurtain(CurtainKind kind) {
	g_curtain.store((int)kind, std::memory_order_relaxed);
	g_curtainSequenceDone = false;
	g_curtainSettle = 0;
	g_curtainVblanks = 0;
	g_haveCurtainFrame = false;
}

void LowerCurtain() {
	g_curtain.store((int)CurtainKind::None, std::memory_order_relaxed);
}

void CurtainTick(const LCSState &state) {
	if (g_curtain.load(std::memory_order_relaxed) == (int)CurtainKind::None) {
		return;
	}
	if (++g_curtainVblanks >= kCurtainMaxVblanks) {
		NOTICE_LOG(Log::System, "LCS front end: curtain down at its ceiling (%s)", g_status);
		LowerCurtain();
		return;
	}
	if (!g_curtainSequenceDone) {
		return;
	}
	if (!state.frameCounter || !state.playerBase) {
		g_haveCurtainFrame = false;
		return;
	}
	if (g_haveCurtainFrame && *state.frameCounter != g_curtainFrame) {
		g_curtainSettle++;
	}
	g_curtainFrame = *state.frameCounter;
	g_haveCurtainFrame = true;
	if (g_curtainSettle >= kCurtainSettleFrames) {
		LowerCurtain();
	}
}

// --- Boot: the intro, its skip, and LOADING after it ---
//
// The game plays two movies before anything else: the logos, then the credits (about 26 s). Cross
// skips the one playing, so a skip taps Cross until both are gone, and any key asks for it (the
// input layer: LCS::RequestIntroSkip). The movies end where the game opens ENTRIES.DAT, once per
// boot; after that come the legal card, the firmware's "Loading from Memory Stick" while the newest
// save autoloads, and the game's own loading screen. The curtain covers all three, as the VCS
// fork's does, and comes down 30 game frames into the world. Measured, two boots: the first Cross
// at 4.7 s went from the logos to the credits, the second at 9.5 s to the legal card, the world at
// 14.2 s; untouched, the credits end at 26 s and ENTRIES.DAT is opened that moment.

bool g_introOver = false;
u32 g_introMask = 0;
int g_introSkipLeft = 0;

// A mouse count in the map's units (the PSP's screen): about one pixel of a 1080p picture.
constexpr float kMapDragUnitsPerCount = 0.25f;

constexpr int kIntroSkipVblanks = 900;   // tapping for at most 15 s
constexpr int kIntroTapPeriod = 30;      // a tap every half second...
constexpr int kIntroTapHold = 6;         // ...held 6 vblanks

void IntroTick(const LCSState &state) {
	// A world that runs is past the intro (a savestate boots straight into one).
	if (!g_introOver && state.frameCounter.value_or(0) != 0) {
		g_introOver = true;
	}
	g_introPlaying.store(!g_introOver, std::memory_order_relaxed);
	if (g_introSkipRequested.exchange(false, std::memory_order_relaxed) && !g_introOver && g_introSkipLeft == 0) {
		g_introSkipLeft = kIntroSkipVblanks;
		NOTICE_LOG(Log::System, "LCS front end: skipping the intro");
	}
	g_introMask = 0;
	if (g_introSkipLeft > 0) {
		if (g_introOver) {
			g_introSkipLeft = 0;
		} else {
			g_introMask = (kIntroSkipVblanks - g_introSkipLeft) % kIntroTapPeriod < kIntroTapHold ? CTRL_CROSS : 0;
			g_introSkipLeft--;
		}
	}
}

// --- Making a save a save ---
//
// The request byte opens the save menu; it doesn't prepare a save. The safe house's routine does
// that in the instructions before it asks (MAIN.SCM 0x176d5), and a file written without them loads
// into the opening mission: the script's boot check starts the intro unless $6 is 1 (0x6933). So
// this does what the routine does - $6, the restart position, the weapons and armour the load path
// gives back - and $560 for the length of it, put back afterwards as the routine does.

u32 ScriptSpace() {
	return ReadU32(kLCSScriptSpacePtr).value_or(0);
}

bool WriteGlobal(u32 space, u32 index, u32 value) {
	return space != 0 && WriteU32(space + index * 4, value);
}

std::optional<u32> ReadGlobal(u32 space, u32 index) {
	return space != 0 ? ReadU32(space + index * 4) : std::nullopt;
}

bool PrepareScriptForSave(const LCSState &state) {
	const u32 space = ScriptSpace();
	if (!space || !state.playerBase) {
		return false;
	}
	const u32 ped = *state.playerBase;
	const std::optional<u32> onMission = ReadGlobal(space, kLCSGlobalOnMission);
	if (!onMission) {
		return false;
	}
	g_savedOnMission = *onMission;
	g_saveSpace = space;
	g_savePrepared = true;

	WriteGlobal(space, kLCSGlobalOnMission, 1);
	WriteGlobal(space, kLCSGlobalFromSave, 1);
	for (u32 i = 0; i < 3; i++) {
		const std::optional<u32> pos = ReadGlobal(space, kLCSGlobalSafehouseX + i);
		if (pos) {
			WriteGlobal(space, kLCSGlobalRestartX + i, *pos);
		}
	}
	// get_ammo_in_player_weapon for every type the routine stores: the slot holding that type's
	// total, or 0.
	for (const LCSSavedWeapon &weapon : kLCSSavedWeapons) {
		u32 ammo = 0;
		for (u32 slot = 0; slot < kLCSWeaponSlots; slot++) {
			const u32 base = ped + kLCSPedWeaponsOffset + slot * kLCSWeaponStride;
			if (ReadU32(base).value_or(0) == weapon.type) {
				ammo = ReadU32(base + kLCSWeaponAmmoOffset).value_or(0);
			}
		}
		WriteGlobal(space, weapon.global, ammo);
	}
	if (state.weaponType) {
		WriteGlobal(space, kLCSGlobalArmedWeapon, *state.weaponType);
	}
	const std::optional<float> armour = ReadFloat(ped + kLCSPedArmourOffset);
	if (armour) {
		WriteGlobal(space, kLCSGlobalArmour, (u32)(s32)*armour);
	}
	return true;
}

void RestoreScriptAfterSave() {
	if (!g_savePrepared) {
		return;
	}
	g_savePrepared = false;
	// Only into the script space it was taken from: a load in between brought its own.
	if (ScriptSpace() == g_saveSpace) {
		WriteGlobal(g_saveSpace, kLCSGlobalOnMission, g_savedOnMission);
	}
}

// --- Ending a sequence ---

void SetOwned(bool owned, int page) {
	g_owned.store(owned, std::memory_order_relaxed);
	g_ownedPage.store(owned ? page : -1, std::memory_order_relaxed);
	SetHintsHidden(owned);
}

void GoIdle(const char *why) {
	NOTICE_LOG(Log::System, "LCS front end: %s", why);
	g_status = why;
	g_phase = Phase::Idle;
	g_stepPressed = false;
	g_target = Req::None;
	g_mask = 0;
	g_pressHold = 0;
	g_pressGap = 0;
	g_busy.store(false, std::memory_order_relaxed);
	RestoreScriptAfterSave();
	// The curtain's own countdown takes it from here.
	g_curtainSequenceDone = true;
}

void Abandon(const char *why, const char *message, const FeState &fe) {
	SetOwned(false, -1);
	const bool closeMenu = fe.valid && fe.up && !g_dialogUp.load(std::memory_order_relaxed);
	GoIdle(why);
	LowerCurtain();
	if (message) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, message, 4.0f, "lcs_frontend");
	}
	if (closeMenu) {
		// Leaving the player in a menu they didn't ask for is worse than closing it.
		g_phase = Phase::Close;
		g_stepPressed = false;
		g_target = Req::Close;
		g_reopen = false;
		g_busy.store(true, std::memory_order_relaxed);
		g_waited = 0;
		g_presses = 0;
	}
}

void Begin(Req target, Phase phase) {
	g_target = target;
	g_phase = phase;
	g_waited = 0;
	g_presses = 0;
	g_attempts = 0;
	g_fromTab = -1;
	g_seqVblanks = 0;
	g_sawDialog = false;
	g_stepPressed = false;
	g_busy.store(true, std::memory_order_relaxed);
	g_status = PhaseName(phase);
}

// --- The phases ---

void Arrive(const FeState &fe) {
	switch (g_target) {
	case Req::Page:
		SetOwned(true, g_wantPage);
		LowerCurtain();
		// STATS opens where it was left, which on a first look is the first line alone at the
		// bottom of an empty page: from the top instead.
		if (g_wantPage == kLCSPageStats) {
			WriteFloat(kLCSStatsScroll, kLCSStatsScrollTop);
		}
		g_phase = Phase::Owned;
		g_stepPressed = false;
		g_busy.store(false, std::memory_order_relaxed);
		g_status = "showing a page";
		NOTICE_LOG(Log::System, "LCS front end: page %d up (tab %d)", fe.page, fe.tab);
		break;
	case Req::Load:
		g_phase = Phase::LoadPick;
		g_stepPressed = false;
		g_waited = 0;
		g_fromTab = -1;
		g_lastPress = 0;
		break;
	case Req::NewGame:
		g_phase = Phase::NewGamePick;
		g_stepPressed = false;
		g_waited = 0;
		g_fromTab = -1;
		g_lastPress = 0;
		break;
	default:
		GoIdle("arrived with nothing to do");
		break;
	}
	g_status = PhaseName(g_phase);
}

void OpenMenuTick(const FeState &fe) {
	if (fe.up) {
		g_phase = g_wantPage == fe.page && fe.inPage ? Phase::Enter : Phase::Walk;
		g_stepPressed = false;
		g_waited = 0;
		g_status = PhaseName(g_phase);
		return;
	}
	if (g_attempts == 0 || g_waited > kOpenTimeout) {
		if (g_attempts >= kMaxStartAttempts) {
			Abandon("the game's menu would not open", "The game's menu can't open right now", fe);
			return;
		}
		g_attempts++;
		Press(CTRL_START);
	}
}

void WalkTick(const FeState &fe) {
	const int wantTab = TabForPage(g_wantPage);
	if (wantTab < 0) {
		Abandon("no tab shows that page", nullptr, fe);
		return;
	}
	if (fe.tab == wantTab && fe.page == g_wantPage) {
		g_phase = Phase::Enter;
		g_stepPressed = false;
		g_waited = 0;
		g_status = PhaseName(g_phase);
		return;
	}
	if (!fe.ready) {
		g_waited = 0;
		return;
	}
	if (fe.inPage) {
		// The tab strip moves only while it has focus. Circle there would close the menu, so this
		// is only ever pressed from inside a page.
		if (!g_stepPressed || g_waited > kStepTimeout) {
			g_stepPressed = true;
			Press(CTRL_CIRCLE);
		}
		return;
	}
	if (g_presses >= kMaxPresses) {
		Abandon("ran out of presses on the tab strip", "Couldn't reach that page", fe);
		return;
	}
	if (fe.tab != g_fromTab || g_waited > kStepTimeout) {
		const u32 step = TabStep(fe.tab, wantTab);
		if (!step) {
			Abandon("no way along the tab strip", nullptr, fe);
			return;
		}
		g_fromTab = fe.tab;
		Press(step);
	}
}

void EnterTick(const FeState &fe) {
	if (fe.inPage && fe.page == g_wantPage) {
		Arrive(fe);
		return;
	}
	if (fe.page != g_wantPage) {
		g_phase = Phase::Walk;
		g_stepPressed = false;
		g_waited = 0;
		return;
	}
	if (g_presses >= kMaxPresses) {
		Abandon("the page would not take focus", "Couldn't open that page", fe);
		return;
	}
	if (!fe.ready) {
		g_waited = 0;
		return;
	}
	// Only from the strip: inside the page Cross activates the selected entry.
	if (!g_stepPressed || g_waited > kStepTimeout) {
		g_stepPressed = true;
		Press(CTRL_CROSS);
	}
}

// Moves the selection on the current page to `row`, then presses Cross. Returns true once Cross
// has been pressed.
bool PickRow(const FeState &fe, int row) {
	if (g_presses >= kMaxPresses) {
		Abandon("could not reach the entry", "Couldn't reach that entry", fe);
		return false;
	}
	if (!fe.ready) {
		g_waited = 0;
		return false;
	}
	if (fe.row != row) {
		if (g_waited == 0 || g_waited > kStepTimeout || fe.row != g_fromTab) {
			g_fromTab = fe.row;
			Press(fe.row < row ? CTRL_DOWN : CTRL_UP);
		}
		return false;
	}
	// Straight after the move that got here; again only if one went unseen.
	if (g_waited == 0 || g_waited > kStepTimeout || g_lastPress != (u32)CTRL_CROSS) {
		Press(CTRL_CROSS);
		return true;
	}
	return false;
}

void LoadPickTick(const FeState &fe) {
	if (fe.page == kLCSPageSaveList || g_dialogUp.load(std::memory_order_relaxed)) {
		g_phase = Phase::Dialog;
		g_stepPressed = false;
		g_waited = 0;
		g_status = PhaseName(g_phase);
		return;
	}
	if (fe.page != kLCSPageGame || !fe.inPage) {
		Abandon("left the Game page", "Couldn't open LOAD GAME", fe);
		return;
	}
	PickRow(fe, kLCSRowLoadGame);
}

void NewGamePickTick(const FeState &fe) {
	if (fe.page == kLCSPageNewGameAsk) {
		g_phase = Phase::NewGameAsk;
		g_stepPressed = false;
		g_waited = 0;
		g_fromTab = -1;
		g_status = PhaseName(g_phase);
		return;
	}
	if (fe.page != kLCSPageGame || !fe.inPage) {
		Abandon("left the Game page", "Couldn't open NEW GAME", fe);
		return;
	}
	PickRow(fe, kLCSRowNewGame);
}

void NewGameAskTick(const FeState &fe) {
	if (fe.page != kLCSPageNewGameAsk) {
		// The press landed: the game is tearing this world down.
		g_phase = Phase::Settle;
		g_stepPressed = false;
		g_waited = 0;
		g_status = PhaseName(g_phase);
		return;
	}
	if (fe.row == kLCSRowAskYes) {
		// Nothing of ours may point into this world when it goes: the hint patch is code, not
		// state, but put it back before rather than after.
		SetOwned(false, -1);
	}
	if (PickRow(fe, kLCSRowAskYes)) {
		NOTICE_LOG(Log::System, "LCS front end: NEW GAME confirmed");
		g_phase = Phase::Settle;
		g_stepPressed = false;
		g_waited = 0;
		g_status = PhaseName(g_phase);
	}
}

int FindName(const SaveDialogPeek &peek, const char *name) {
	for (int i = 0; i < peek.count && i < SaveDialogPeek::kMaxNames; i++) {
		if (!strcmp(peek.names[i], name)) {
			return i;
		}
	}
	return -1;
}

void DialogTick(const FeState &fe) {
	SaveDialogPeek peek;
	const bool up = PeekSaveDialog(&peek);
	if (!up) {
		if (g_sawDialog) {
			if (g_target == Req::Load) {
				g_phase = Phase::Settle;
				g_stepPressed = false;
				g_waited = 0;
				g_status = PhaseName(g_phase);
				return;
			}
			// A save: done, or cancelled. The game shuts its menu itself after a save it asked
			// for; one we asked for is shut the same way if it is still up.
			RestoreScriptAfterSave();
			if (fe.up) {
				g_phase = Phase::Close;
				g_stepPressed = false;
				g_target = Req::Close;
				g_reopen = false;
				g_waited = 0;
				g_presses = 0;
				g_status = "closing after the save";
			} else {
				GoIdle(g_saveIsAuto ? "auto-saved" : "saved");
			}
			return;
		}
		if (g_waited > kDialogAppearTimeout) {
			Abandon("the save dialog never appeared", "The save list didn't open", fe);
		}
		return;
	}
	if (!g_sawDialog) {
		g_sawDialog = true;
		g_waited = 0;
	}
	if (g_waited > kDialogTimeout) {
		// Up, but not moving through anything this knows how to answer: it is the player's now.
		GoIdle("save dialog not answered");
		LowerCurtain();
		g_OSD.Show(OSDType::MESSAGE_WARNING, "Finish in the save list", 4.0f, "lcs_frontend");
		return;
	}
	if (peek.busy) {
		return;
	}
	const u32 ok = peek.okButton ? peek.okButton : (u32)CTRL_CROSS;
	const u32 back = peek.cancelButton ? peek.cancelButton : (u32)CTRL_CIRCLE;

	if (peek.list) {
		int want = -1;
		if (peek.save) {
			// The auto-save's own slot: the last the game offers, so it can only ever overwrite
			// the previous auto-save and the others stay the player's.
			want = std::min(peek.count, (int)SaveDialogPeek::kMaxNames) - 1;
			if (want >= 0 && peek.names[want][0] == 'S') {
				const int slot = atoi(peek.names[want] + 1);
				g_autoSaveSlot.store(slot, std::memory_order_relaxed);
				// Saved with the rest when the menu next closes; only read by the save list.
				Settings().autoSaveSlot = slot;
			}
		} else {
			char name[16];
			snprintf(name, sizeof(name), "S%d", g_wantSlot);
			want = FindName(peek, name);
			if (want < 0) {
				Abandon("that save isn't in the list", "That save isn't on the memory stick", fe);
				return;
			}
		}
		if (want >= 0 && peek.selected != want) {
			if (g_presses >= kMaxPresses) {
				Abandon("could not reach the slot", "Couldn't reach that save slot", fe);
				return;
			}
			Press(peek.selected > want ? CTRL_UP : CTRL_DOWN);
			return;
		}
		Press(ok);
		return;
	}
	if (peek.confirm) {
		// The prompt opens on NO; LEFT moves it to YES.
		Press(peek.yesno != 1 ? (u32)CTRL_LEFT : ok);
		return;
	}
	if (peek.done) {
		// "Save completed" takes Back and nothing else.
		Press(back);
	}
}

void SettleTick(const FeState &fe) {
	const bool dialog = g_dialogUp.load(std::memory_order_relaxed);
	if (!fe.up && !dialog) {
		GoIdle(g_target == Req::NewGame ? "new game started" : "loaded");
		return;
	}
	// After the list, the game asks whether to load over the world it has: YES.
	if (g_target == Req::Load && fe.up && !dialog && fe.page == kLCSPageLoadAsk) {
		PickRow(fe, kLCSRowAskYes);
		return;
	}
	// Back on the Game page: the load was cancelled or failed.
	if (g_target == Req::Load && fe.up && !dialog && fe.page == kLCSPageGame && g_waited > kOpenTimeout) {
		Abandon("the load didn't start", "That save didn't load", fe);
		return;
	}
	if (g_waited > kSettleTimeout) {
		GoIdle("the world never came back");
		LowerCurtain();
	}
}

void CloseTick(const FeState &fe) {
	if (!fe.up) {
		SetOwned(false, -1);
		const bool reopen = g_reopen;
		GoIdle("game's menu closed");
		if (reopen) {
			g_menuAfterClose.store(true, std::memory_order_relaxed);
		}
		return;
	}
	if (g_dialogUp.load(std::memory_order_relaxed)) {
		// Start does nothing to the firmware's dialog; the player finishes or cancels it.
		return;
	}
	if (g_presses >= kMaxStartAttempts + 1) {
		GoIdle("the game's menu would not close");
		LowerCurtain();
		return;
	}
	if (g_waited == 0 && g_presses == 0) {
		Press(CTRL_START);
	} else if (g_waited > kOpenTimeout) {
		Press(CTRL_START);
	}
}

void CloseFirstTick(const FeState &fe) {
	if (!fe.up) {
		g_phase = Phase::OpenMenu;
		g_stepPressed = false;
		g_waited = 0;
		g_attempts = 0;
		g_status = PhaseName(g_phase);
		return;
	}
	if (g_waited == 0 || g_waited > kOpenTimeout) {
		if (g_presses > kMaxStartAttempts) {
			Abandon("the game's menu would not close", nullptr, fe);
			return;
		}
		Press(CTRL_START);
	}
}

void OwnedTick(const FeState &fe) {
	// The player shut it some other way, or stepped out of the page: from then on it is the game's
	// own menu, with its own hints.
	if (!fe.up || !fe.inPage || fe.page != g_ownedPage.load(std::memory_order_relaxed)) {
		SetOwned(false, -1);
		g_phase = Phase::Idle;
		g_stepPressed = false;
		g_target = Req::None;
		g_status = fe.up ? "page left" : "game's menu closed";
	}
}

// --- Requests ---

void TakeRequest(const LCSState &state, const FeState &fe) {
	const Req req = (Req)g_request.exchange((int)Req::None, std::memory_order_relaxed);
	if (req == Req::None) {
		return;
	}
	const int arg = g_requestArg.load(std::memory_order_relaxed);
	if (g_phase != Phase::Idle && g_phase != Phase::Owned) {
		NOTICE_LOG(Log::System, "LCS front end: request %d dropped, busy (%s)", (int)req, g_status);
		return;
	}
	if (!fe.valid || !state.playerBase) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, "Not while the game is loading", 3.0f, "lcs_frontend");
		return;
	}
	switch (req) {
	case Req::Page:
		g_wantPage = arg;
		RaiseCurtain(CurtainKind::Menu);
		Begin(Req::Page, fe.up ? Phase::CloseFirst : Phase::OpenMenu);
		break;
	case Req::Close:
	case Req::CloseReopen:
		if (!fe.up) {
			if (req == Req::CloseReopen) {
				g_menuAfterClose.store(true, std::memory_order_relaxed);
			}
			return;
		}
		g_reopen = req == Req::CloseReopen;
		if (g_reopen) {
			// Covers the world for the frames between the game's menu going and this fork's.
			RaiseCurtain(CurtainKind::Menu);
		}
		Begin(Req::Close, Phase::Close);
		break;
	case Req::Load:
		g_wantSlot = arg;
		g_wantPage = kLCSPageGame;
		RaiseCurtain(CurtainKind::Loading);
		Begin(Req::Load, fe.up ? Phase::CloseFirst : Phase::OpenMenu);
		break;
	case Req::NewGame:
		g_wantPage = kLCSPageGame;
		RaiseCurtain(CurtainKind::Loading);
		Begin(Req::NewGame, fe.up ? Phase::CloseFirst : Phase::OpenMenu);
		break;
	case Req::Save:
		if (fe.up) {
			NOTICE_LOG(Log::System, "LCS front end: save request dropped, the game's menu is up");
			return;
		}
		if (!PrepareScriptForSave(state)) {
			NOTICE_LOG(Log::System, "LCS front end: save request dropped, no script space");
			return;
		}
		g_saveIsAuto = arg != 0;
		RaiseCurtain(CurtainKind::Saving);
		Begin(Req::Save, Phase::Dialog);
		WriteU8(kLCSFrontEnd + kLCSFeSaveRequestOffset, 1);
		NOTICE_LOG(Log::System, "LCS front end: save requested (%s)", g_saveIsAuto ? "auto" : "asked");
		break;
	default:
		break;
	}
	if (g_phase != Phase::Idle) {
		NOTICE_LOG(Log::System, "LCS front end: start %s (arg %d)", PhaseName(g_phase), arg);
	}
}

// --- The auto-save ---

void MissionWatchTick(const LCSState &state, const FeState &fe) {
	const std::optional<u32> count = ReadU32(kLCSMissionsPassed);
	const std::optional<u32> keyLo = ReadU32(kLCSLatestMissionKey);
	const std::optional<u32> keyHi = ReadU32(kLCSLatestMissionKey + 4);
	if (!count || !keyLo || !keyHi || !state.playerBase) {
		return;
	}
	const u64 key = (u64)*keyLo | ((u64)*keyHi << 32);
	if (!g_haveMission) {
		g_missionCount = *count;
		g_missionKey = key;
		g_haveMission = true;
		return;
	}
	// A load brings its own key and zeroes the counter: that is not a mission passed.
	if (*count > g_missionCount && key != g_missionKey && key != 0) {
		char name[9]{};
		memcpy(name, &key, 8);
		if (Settings().autoSave) {
			NOTICE_LOG(Log::System, "LCS front end: mission passed (%s), auto-save in %d frames", name, kAutoSaveDelayFrames);
			g_autoSaveArmed = true;
			g_autoSaveDelay = kAutoSaveDelayFrames;
			g_autoSaveWaited = 0;
		} else {
			NOTICE_LOG(Log::System, "LCS front end: mission passed (%s), auto-save off", name);
		}
	}
	g_missionCount = *count;
	g_missionKey = key;

	if (!g_autoSaveArmed || !state.frameCounter) {
		g_haveWatchFrame = false;
		return;
	}
	const bool advanced = g_haveWatchFrame && *state.frameCounter != g_watchFrame;
	g_watchFrame = *state.frameCounter;
	g_haveWatchFrame = true;
	if (!advanced) {
		return;
	}
	if (g_autoSaveDelay > 0) {
		g_autoSaveDelay--;
		return;
	}
	// A moment the game would let the player save in: no mission running, nothing on screen, the
	// player standing in the world.
	const u32 space = ScriptSpace();
	const bool onMission = ReadGlobal(space, kLCSGlobalOnMission).value_or(1) != 0;
	const bool cutscene = state.camMode.value_or(kCamModeFlyby) == (u32)kCamModeFlyby;
	const bool alive = state.health.value_or(0.0f) > 0.0f;
	if (!onMission && !cutscene && alive && !fe.up && g_phase == Phase::Idle &&
			!g_dialogUp.load(std::memory_order_relaxed)) {
		g_autoSaveArmed = false;
		g_requestArg.store(1, std::memory_order_relaxed);
		g_request.store((int)Req::Save, std::memory_order_relaxed);
		return;
	}
	if (++g_autoSaveWaited >= kAutoSavePatience) {
		g_autoSaveArmed = false;
		NOTICE_LOG(Log::System, "LCS front end: auto-save gave up waiting (mission %d, cutscene %d, menu %d)",
			onMission, cutscene, fe.up);
	}
}

// --- The game's own settings ---
//
// Mirrored, not edited in place: the menu runs on the UI thread and PSP memory belongs to this
// one. Pushed only when the game disagrees, which is also what puts them back after a save loads
// its own.

template <typename T>
void ApplyPref(u32 address, std::optional<T> (*read)(u32), bool (*write)(u32, T), T want) {
	const std::optional<T> have = read(address);
	if (have && *have != want) {
		write(address, want);
	}
}

std::optional<s32> ReadS32Opt(u32 address) {
	return ReadS32(address);
}

bool WriteS32(u32 address, s32 value) {
	return WriteU32(address, (u32)value);
}

void GamePrefsTick(const LCSState &state) {
	if (!state.playerBase) {
		return;
	}
	const LCSSettings &s = Settings();
	ApplyPref<s32>(kLCSBrightness, &ReadS32Opt, &WriteS32, s.gameBrightness);
	ApplyPref<u8>(kLCSSubtitles, &ReadU8, &WriteU8, s.gameSubtitles ? 1 : 0);
	ApplyPref<u8>(kLCSHudOn, &ReadU8, &WriteU8, s.gameHud ? 1 : 0);
	ApplyPref<s32>(kLCSRadarMode, &ReadS32Opt, &WriteS32, s.gameRadar);
	ApplyPref<u8>(LookupAddr(LCSAddr::InvertLook).address, &ReadU8, &WriteU8, s.gameInvertLook ? 1 : 0);
	// The keyboard's car keys are the game's configuration 2 (kLCSPadModeOffset): R the handbrake,
	// L and the stick the looks. A save carries its own; the game's default is 2.
	if (s.keyboardMouse) {
		ApplyPref<u16>(kLCSPads + kLCSPadModeOffset, &ReadU16, &WriteU16, kLCSPadModeKeyboard);
	}
	const u32 lookSensitivity = LookupAddr(LCSAddr::LookSensitivity).address;
	const std::optional<float> look = ReadFloat(lookSensitivity);
	if (look && std::fabs(*look - s.padLookSensitivity) > 1e-6f) {
		WriteFloat(lookSensitivity, s.padLookSensitivity);
	}
	// A volume is the preference and the byte the mixer reads; both move together, and only on a
	// change - the game may move the byte itself (a fade), and that isn't ours to undo.
	const u8 sfxLevel = (u8)std::clamp(s.sfxVolume, 0, 127);
	const u8 musicLevel = (u8)std::clamp(s.musicVolume, 0, 127);
	const std::optional<s32> sfx = ReadS32(kLCSSfxVolume);
	if (sfx && *sfx != s.sfxVolume) {
		WriteS32(kLCSSfxVolume, s.sfxVolume);
		WriteU8(kLCSSfxLevel, sfxLevel);
	}
	const std::optional<s32> music = ReadS32(kLCSMusicVolume);
	if (music && *music != s.musicVolume) {
		WriteS32(kLCSMusicVolume, s.musicVolume);
		WriteU8(kLCSMusicLevel, musicLevel);
	}
	// The bridge's presses would blip in the game's menu: the live SFX level is 0 while it drives
	// and back when it stops. The preference is never touched, so nothing of it reaches a save.
	const bool quiet = FrontEndDriving();
	if (quiet != g_quiet) {
		g_quiet = quiet;
		WriteU8(kLCSSfxLevel, quiet ? 0 : sfxLevel);
	}
}

}  // namespace

// --- Requests ---

void RequestGamePage(GamePage page) {
	int index = kLCSPageMap;
	switch (page) {
	case GamePage::Map: index = kLCSPageMap; break;
	case GamePage::Brief: index = kLCSPageBrief; break;
	case GamePage::Stats: index = kLCSPageStats; break;
	}
	g_requestArg.store(index, std::memory_order_relaxed);
	g_request.store((int)Req::Page, std::memory_order_relaxed);
}

void RequestCloseGameMenu(bool reopenMenu) {
	g_request.store((int)(reopenMenu ? Req::CloseReopen : Req::Close), std::memory_order_relaxed);
}

void RequestLoadSlot(int slot) {
	g_requestArg.store(slot, std::memory_order_relaxed);
	g_request.store((int)Req::Load, std::memory_order_relaxed);
}

void RequestNewGame() {
	g_request.store((int)Req::NewGame, std::memory_order_relaxed);
}

void RequestAutoSave() {
	g_requestArg.store(1, std::memory_order_relaxed);
	g_request.store((int)Req::Save, std::memory_order_relaxed);
}

// --- Queries ---

bool GameMenuUp() {
	return g_menuUp.load(std::memory_order_relaxed);
}

bool BridgeOwnsMenu() {
	return g_owned.load(std::memory_order_relaxed);
}

int OwnedGamePage() {
	return g_ownedPage.load(std::memory_order_relaxed);
}

bool FrontEndBusy() {
	return g_busy.load(std::memory_order_relaxed) || g_request.load(std::memory_order_relaxed) != (int)Req::None;
}

bool SaveDialogUp() {
	return g_dialogUp.load(std::memory_order_relaxed);
}

CurtainKind ActiveCurtain() {
	return (CurtainKind)g_curtain.load(std::memory_order_relaxed);
}

void DropCurtain() {
	g_curtain.store((int)CurtainKind::None, std::memory_order_relaxed);
}

bool TakeMenuAfterClose() {
	return g_menuAfterClose.exchange(false, std::memory_order_relaxed);
}

int AutoSaveSlot() {
	return g_autoSaveSlot.load(std::memory_order_relaxed);
}

// --- Emu thread ---

void FrontEndTick() {
	const LCSState &state = GetState();
	const FeState fe = ReadFe();
	g_menuUp.store(fe.valid && fe.up, std::memory_order_relaxed);
	SaveDialogPeek peek;
	g_dialogUp.store(PeekSaveDialog(&peek), std::memory_order_relaxed);

	IntroTick(state);
	CurtainTick(state);
	MissionWatchTick(state, fe);
	GamePrefsTick(state);
	// Every tick, as a savestate brings the game's op back: the save menu never opens on the cheat
	// warning (kLCSSaveCheatWarning).
	SetCodeWord(kLCSSaveCheatWarning, true);
	const bool inPage = fe.valid && fe.up && fe.inPage;
	g_mapPageUp.store(inPage && fe.page == kLCSPageMap, std::memory_order_relaxed);
	bool briefEmpty = false;
	if (inPage && fe.page == kLCSPageBrief) {
		briefEmpty = true;
		for (int i = 0; i < kLCSPreviousBriefCount && briefEmpty; i++) {
			briefEmpty = ReadU32(kLCSPreviousBriefs + (u32)i * kLCSPreviousBriefStride).value_or(0) == 0;
		}
	}
	g_briefEmpty.store(briefEmpty, std::memory_order_relaxed);

	// A savestate taken while a page was showing brings the hint patch back with it.
	if (g_phase == Phase::Idle && !g_owned.load(std::memory_order_relaxed) &&
			ReadCodeWord(kLCSMenuHintsCall) == CodeWordState::Patched) {
		SetCodeWord(kLCSMenuHintsCall, false);
		g_hintsHidden = false;
	}

	if (Pressing()) {
		StepPress();
		return;
	}
	g_waited++;
	TakeRequest(state, fe);
	if (!fe.valid && g_phase != Phase::Idle) {
		Abandon("the front end can't be read", nullptr, fe);
		return;
	}
	if (g_busy.load(std::memory_order_relaxed) && g_phase != Phase::Dialog && g_phase != Phase::Settle &&
			++g_seqVblanks > kWalkTimeout) {
		Abandon("the game's menu didn't respond", "The game's menu didn't respond", fe);
		return;
	}
	switch (g_phase) {
	case Phase::Idle: break;
	case Phase::CloseFirst: CloseFirstTick(fe); break;
	case Phase::OpenMenu: OpenMenuTick(fe); break;
	case Phase::Walk: WalkTick(fe); break;
	case Phase::Enter: EnterTick(fe); break;
	case Phase::Owned: OwnedTick(fe); break;
	case Phase::LoadPick: LoadPickTick(fe); break;
	case Phase::NewGamePick: NewGamePickTick(fe); break;
	case Phase::NewGameAsk: NewGameAskTick(fe); break;
	case Phase::Dialog: DialogTick(fe); break;
	case Phase::Settle: SettleTick(fe); break;
	case Phase::Close: CloseTick(fe); break;
	}
}

u32 FrontEndButtonMask() {
	return g_mask | g_introMask;
}

bool IntroPlaying() {
	return g_introPlaying.load(std::memory_order_relaxed);
}

void RequestIntroSkip() {
	g_introSkipRequested.store(true, std::memory_order_relaxed);
}

void NoteFileOpen(const char *filename) {
	if (!IsActive() || g_introOver || !filename || !equalsNoCase(filename, kLCSIntroOverFile)) {
		return;
	}
	g_introOver = true;
	g_introPlaying.store(false, std::memory_order_relaxed);
	RaiseCurtain(CurtainKind::Loading);
	// Nothing of ours to wait for: it comes down once the world has run kCurtainSettleFrames.
	g_curtainSequenceDone = true;
	NOTICE_LOG(Log::System, "LCS front end: the intro is over (%s), LOADING until the world runs", filename);
}

bool ScrollGamePage(int notches) {
	if (notches == 0) {
		return false;
	}
	const FeState fe = ReadFe();
	if (!fe.valid || !fe.up || !fe.inPage) {
		return false;
	}
	// Two STATS lines, or one brief, a notch, the way a PC list scrolls: the wheel down brings up what
	// is below. (The game's own down moves STATS the other way, the text down; BRIEF's goes this way.)
	u32 address = 0;
	float step = 0.0f;
	if (fe.page == kLCSPageStats) {
		address = kLCSStatsScroll;
		step = -2.0f * kLCSStatsLine;
	} else if (fe.page == kLCSPageBrief) {
		address = kLCSBriefScroll;
		step = kLCSBriefEntry;
	} else {
		return false;
	}
	const std::optional<float> offset = ReadFloat(address);
	if (offset) {
		WriteFloat(address, *offset + (float)notches * step);
	}
	return true;
}

bool DragMap(float dx, float dy) {
	const FeState fe = ReadFe();
	if (!fe.valid || !fe.up || !fe.inPage || fe.page != kLCSPageMap) {
		return false;
	}
	const std::optional<float> x = ReadFloat(kLCSFrontEnd + kLCSFeMapCentreXOffset);
	const std::optional<float> y = ReadFloat(kLCSFrontEnd + kLCSFeMapCentreYOffset);
	const std::optional<float> size = ReadFloat(kLCSFrontEnd + kLCSFeMapHalfSizeOffset);
	if (!x || !y || !size) {
		return true;
	}
	// The city follows the pointer (a drag right brings it right, the opposite of what a held right
	// does), kept where the game's own scroll keeps it.
	const float reach = std::max(*size - kLCSMapEdge, 0.0f);
	WriteFloat(kLCSFrontEnd + kLCSFeMapCentreXOffset,
		std::clamp(*x + dx * kMapDragUnitsPerCount, kLCSMapCentreX - reach, kLCSMapCentreX + reach));
	WriteFloat(kLCSFrontEnd + kLCSFeMapCentreYOffset,
		std::clamp(*y + dy * kMapDragUnitsPerCount, kLCSMapCentreY - reach, kLCSMapCentreY + reach));
	return true;
}

bool MapPageUp() {
	return g_mapPageUp.load(std::memory_order_relaxed);
}

bool BriefEmpty() {
	return g_briefEmpty.load(std::memory_order_relaxed);
}

bool FrontEndDriving() {
	return g_busy.load(std::memory_order_relaxed) && g_phase != Phase::Owned;
}

void FrontEndReset() {
	if (g_hintsHidden) {
		SetCodeWord(kLCSMenuHintsCall, false);
		g_hintsHidden = false;
	}
	g_request.store((int)Req::None, std::memory_order_relaxed);
	g_menuUp.store(false, std::memory_order_relaxed);
	g_owned.store(false, std::memory_order_relaxed);
	g_ownedPage.store(-1, std::memory_order_relaxed);
	g_busy.store(false, std::memory_order_relaxed);
	g_dialogUp.store(false, std::memory_order_relaxed);
	g_menuAfterClose.store(false, std::memory_order_relaxed);
	g_curtain.store((int)CurtainKind::None, std::memory_order_relaxed);
	g_phase = Phase::Idle;
	g_stepPressed = false;
	g_target = Req::None;
	g_status = "idle";
	g_mask = 0;
	g_pressHold = 0;
	g_pressGap = 0;
	g_haveMission = false;
	g_autoSaveArmed = false;
	g_savePrepared = false;
	g_introOver = false;
	g_introMask = 0;
	g_introSkipLeft = 0;
	g_introPlaying.store(false, std::memory_order_relaxed);
	g_introSkipRequested.store(false, std::memory_order_relaxed);
	g_mapPageUp.store(false, std::memory_order_relaxed);
	g_briefEmpty.store(false, std::memory_order_relaxed);
}

const char *FrontEndStatus() {
	return g_status;
}

void LatestMissionKey(char *out, size_t size) {
	if (!size) {
		return;
	}
	out[0] = 0;
	char key[9]{};
	memcpy(key, &g_missionKey, 8);
	truncate_cpy(out, size, key);
}

int AutoSaveDelayLeft() {
	return g_autoSaveArmed ? g_autoSaveDelay : -1;
}

}  // namespace LCS
