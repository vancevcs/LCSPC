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
#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/Data/Color/RGBAUtil.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Log.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/Render/Text/Font.h"
#include "Common/System/Display.h"
#include "Common/System/OSD.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/Root.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSFrontEnd.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSInput.h"
#include "Core/LCS/LCSSettings.h"
#include "UI/BackgroundAudio.h"
#include "UI/PauseScreen.h"
#include "UI/LCSMenuScreen.h"

// The palette is the game's own pause menu, sampled from it: rows light blue, the selected one
// white, headings red in Pricedown, the hint strip near-black with a lighter top edge. The layout,
// the blocks and the behaviour are the VCS fork's menu, which carries the selection in colour
// alone - no highlight shape behind the row.
static const uint32_t kItemColor = COLOR(0x76B0DC);
static const uint32_t kItemSelectedColor = COLOR(0xFFFFFF);
static const uint32_t kTitleColor = COLOR(0xAE0000);
static const uint32_t kSliderEmptyColor = COLOR(0x1C3044);
static const uint32_t kHintColor = COLOR(0xE8E8E8);
// The strip under the game's backdrop, and its edge (Tools/lcsmenuart.py prints both).
static const uint32_t kBarColor = COLOR(0x0C0B10);
static const uint32_t kBarEdgeColor = COLOR(0x1D171F);
// The backdrop's own base: what shows if assets/lcs is missing, so a missing file reads as a
// plain dark menu rather than a broken one.
static const uint32_t kBackgroundColor = COLOR(0x010001);

// A placeholder row is dimmed but not greyed out like a row whose switch is off: it is still a
// stop for the keyboard, so its help line can say what is coming.
static constexpr float kPlaceholderAlpha = 0.45f;
static constexpr float kDisabledAlpha = 0.35f;

// Layout, in dp. Rows are full screen width so "centred" means centred on the screen.
static constexpr float kRowHeight = 76.0f;
static constexpr float kRowMinHeight = 50.0f;
static constexpr float kTitleLeft = 58.0f;
static constexpr float kTitleTop = 24.0f;
static constexpr float kTitleHeight = 104.0f;
static constexpr float kBottomBarHeight = 38.0f;
// Gap from the screen centre to the value column: labels right-aligned into the centre, values
// left-aligned past it.
static constexpr float kValueGap = 100.0f;
static constexpr int kSliderBlocks = 10;
// How far left of centre the save list starts.
static constexpr float kSaveListInset = 240.0f;

// Real font sizes rather than a scaled theme font: the text drawer rasterises at the style's size
// and a scale only stretches the pixels.
static const FontStyle kItemFont(FontFamily::Display, 38, FontStyleFlags::Default);
// The game's headings are lowercase Pricedown, and so are these - drawn as text, where the VCS
// menu had to bake its brush-script titles to images.
static const FontStyle kTitleFont(FontFamily::Display, 68, FontStyleFlags::Default);
static const FontStyle kConfirmFont(FontFamily::SansSerif, 28, FontStyleFlags::Default);
static const FontStyle kConfirmDetailFont(FontFamily::SansSerif, 20, FontStyleFlags::Default);

// The controls card is a table, set in the UI sans: Pricedown sets four words across a screen
// well and twenty rows badly.
static const FontStyle kListFont(FontFamily::SansSerif, 25, FontStyleFlags::Default);
static const FontStyle kListHeaderFont(FontFamily::SansSerif, 18, FontStyleFlags::Default);
static constexpr float kListRowHeight = 40.0f;
static constexpr float kListRowMinHeight = 28.0f;
static constexpr float kListTop = 150.0f;
static constexpr float kListPadding = 22.0f;
static constexpr float kListHeaderHeight = 46.0f;
static constexpr float kListPanelInsetFrac = 0.023f;
static constexpr float kListNameFrac = 0.047f;
static constexpr float kListDeviceFrac[] = { 0.42f, 0.74f };
static const char *const kListHeaders[] = { "ACTION", "KEYBOARD AND MOUSE", "PSP" };
static_assert(ARRAY_SIZE(kListHeaders) == ARRAY_SIZE(kListDeviceFrac) + 1, "a heading per column");
static const char *const kListKeySeparator = " / ";
static const uint32_t kListPanelColor = 0x40000000;
static const uint32_t kListNameColor = COLOR(0x9BB0C8);
static const uint32_t kListKeyColor = COLOR(0xF0F2F8);
static const uint32_t kListRowColor = 0x50DCB076;  // the rows' blue, translucent (ABGR)
static const uint32_t kListHeaderColor = COLOR(0xFDB336);  // the game's tab orange
static const uint32_t kListRuleColor = 0x40FFFFFF;

// Lines for the bottom bar that are true of a whole page rather than of the selected row.
static const char *const kBindingsNote = "Rebinding keys from here comes later. These are the keys as they are.";
static const char *const kCheatNote = "Save first - some cheats cannot be turned off again.";
static const char *const kPlaceholderPrefix = "Coming later. ";

// --- The cheats ---
//
// LCS has no cheat screen: a cheat is eight presses typed during play. A row queues its presses on
// the PSP pad (LCS::QueuePadTaps, the same queue as Debug -> LCS -> Pad), and closing the menu is
// the last step of entering it, because the emulator is paused while this is up. Typing the
// combination gets everything the game does for a cheat - its message, its flags - for free.
//
// The sequences are the published PSP ones (grandtheftwiki.com); the three weapon sets are
// confirmed against the running game (free-aim.md).
struct LCSCheat {
	LCSMenuPage group;
	const char *name;
	const char *help;
	const char *sequence;
};

static const LCSCheat kCheats[] = {
	{ LCSMenuPage::CheatsPlayer, "WEAPON SET 1", "Brass knuckles, pistol, shotgun, Uzi, rifle and more.", "Up Square Square Down Left Square Square Right" },
	{ LCSMenuPage::CheatsPlayer, "WEAPON SET 2", "The second weapon set.", "Up Circle Circle Down Left Circle Circle Right" },
	{ LCSMenuPage::CheatsPlayer, "WEAPON SET 3", "The third weapon set.", "Up Cross Cross Down Left Cross Cross Right" },
	{ LCSMenuPage::CheatsPlayer, "FULL HEALTH", "", "L R Cross L R Square L R" },
	{ LCSMenuPage::CheatsPlayer, "FULL ARMOUR", "", "L R Circle L R Cross L R" },
	{ LCSMenuPage::CheatsPlayer, "$250,000", "", "L R Triangle L R Circle L R" },
	{ LCSMenuPage::CheatsPlayer, "RAISE WANTED LEVEL", "Two more stars.", "L R Square L R Triangle L R" },
	{ LCSMenuPage::CheatsPlayer, "NEVER WANTED", "The police lose interest for good. Cannot be turned off.", "L L Triangle R R Cross Square Circle" },
	{ LCSMenuPage::CheatsPlayer, "CHANGE OUTFIT", "Play as a random pedestrian.", "L L Left L L Right Square Triangle" },
	{ LCSMenuPage::CheatsPlayer, "MEDIA ATTENTION", "Show the media attention meter.", "L Up Right R Triangle Square Down Cross" },
	{ LCSMenuPage::CheatsVehicles, "SPAWN RHINO", "A tank, in front of you.", "L L Left L L Right Triangle Circle" },
	{ LCSMenuPage::CheatsVehicles, "SPAWN TRASHMASTER", "A garbage truck, in front of you.", "Triangle Circle Down Triangle Circle Up L L" },
	{ LCSMenuPage::CheatsVehicles, "DESTROY ALL CARS", "Every car nearby blows up.", "L L Left L L Right Cross Square" },
	{ LCSMenuPage::CheatsVehicles, "PERFECT TRACTION", "Cars grip the road; Down hops.", "L Up Left R Triangle Circle Down Cross" },
	{ LCSMenuPage::CheatsVehicles, "CARS DRIVE ON WATER", "", "Circle Cross Down Circle Cross Up L L" },
	{ LCSMenuPage::CheatsVehicles, "CHROME CARS", "", "Triangle R L Down Down R R Triangle" },
	{ LCSMenuPage::CheatsVehicles, "WHITE TRAFFIC", "", "Cross Cross R Circle Circle L Triangle Triangle" },
	{ LCSMenuPage::CheatsVehicles, "BLACK TRAFFIC", "", "Circle Circle R Triangle Triangle L Square Square" },
	{ LCSMenuPage::CheatsVehicles, "AGGRESSIVE DRIVERS", "", "Square Square R Cross Cross L Circle Circle" },
	{ LCSMenuPage::CheatsVehicles, "ALL GREEN LIGHTS", "", "Triangle Triangle R Square Square L Cross Cross" },
	{ LCSMenuPage::CheatsVehicles, "BIKE TYRE SIZE", "Bigger wheels on bikes.", "Circle Right Cross Up Right Cross L Square" },
	{ LCSMenuPage::CheatsPedestrians, "PEDESTRIANS RIOT", "Cannot be turned off.", "L L R L L R Left Square" },
	{ LCSMenuPage::CheatsPedestrians, "PEDESTRIANS ATTACK YOU", "Cannot be turned off.", "L L R L L R Up Triangle" },
	{ LCSMenuPage::CheatsPedestrians, "PEDESTRIANS ARMED", "", "R R L R R L Right Circle" },
	{ LCSMenuPage::CheatsPedestrians, "PEDESTRIANS FOLLOW YOU", "", "Down Down Down Triangle Triangle Circle L R" },
	{ LCSMenuPage::CheatsPedestrians, "BIG HEADS", "", "Down Down Down Circle Circle Cross L R" },
	{ LCSMenuPage::CheatsPedestrians, "PASSENGER", "The nearest pedestrian gets into your car.", "Cross Square Down Cross Square Up R R" },
	{ LCSMenuPage::CheatsWorld, "SUNNY", "", "L L Circle R R Square Triangle Cross" },
	{ LCSMenuPage::CheatsWorld, "CLEAR", "", "Up Down Circle Up Down Square L R" },
	{ LCSMenuPage::CheatsWorld, "OVERCAST", "", "Up Down Cross Up Down Triangle L R" },
	{ LCSMenuPage::CheatsWorld, "RAIN", "", "Up Down Square Up Down Circle L R" },
	{ LCSMenuPage::CheatsWorld, "FOG", "", "Up Down Triangle Up Down Cross L R" },
	{ LCSMenuPage::CheatsWorld, "FASTER CLOCK", "", "L L Left L L Right Circle Cross" },
	{ LCSMenuPage::CheatsWorld, "FASTER GAMEPLAY", "", "R R L R R L Down Cross" },
	{ LCSMenuPage::CheatsWorld, "SLOWER GAMEPLAY", "", "R Triangle Cross R Square Circle Left Right" },
};

// --- Art ---

// Centre of the screen, in the same dp space as a view's bounds; asked directly rather than from
// a row's bounds, which are only the screen's width if the layout gave it every pixel.
static float ScreenCenterX() {
	return g_display.dp_xres * 0.5f;
}

// Textures for the life of the screen, loaded on the first draw (the first time a DrawContext is
// in hand). A miss is cached as null, so a missing file is not re-read every frame.
struct LCSMenuArt {
	~LCSMenuArt() {
		Release();
	}

	void Release() {
		for (auto &pair : images) {
			if (pair.second) {
				pair.second->Release();
			}
		}
		images.clear();
	}

	Draw::Texture *Image(UIContext &dc, const char *path) {
		// Between deviceLost and deviceRestored there is nothing to make a texture on.
		if (deviceLost) {
			return nullptr;
		}
		auto iter = images.find(path);
		if (iter != images.end()) {
			return iter->second;
		}
		Draw::Texture *tex = nullptr;
		size_t size = 0;
		if (uint8_t *data = g_VFS.ReadFile(path, &size)) {
			tex = CreateTextureFromFileData(dc.GetDrawContext(), data, size, ImageFileType::DETECT, false, path);
			delete[] data;
		}
		images[path] = tex;
		return tex;
	}

	std::map<std::string, Draw::Texture *> images;
	bool deviceLost = false;
};

static void DrawTexture(UIContext &dc, Draw::Texture *tex, const Bounds &bounds, uint32_t color) {
	dc.Flush();
	dc.Begin();
	dc.GetDrawContext()->BindTexture(0, tex);
	dc.Draw()->DrawTexRect(bounds, 0.0f, 0.0f, 1.0f, 1.0f, color);
	dc.Flush();
	dc.RebindTexture();
}

// --- Sounds ---

// Straight into the effect mixer, not UI::PlayUISound: that is gated on g_Config.bUISound, which
// ships off. A UI sound is mixed after the game's audio, so the emulator's volume and its sound
// switch are applied here or not at all.
static void PlayMenuSound(UI::UISound sound) {
	if (!g_Config.bEnableSound) {
		return;
	}
	g_BackgroundAudio.SFX().Play(sound,
		Volume100ToMultiplier(std::clamp(g_Config.iGameVolume, VOLUME_OFF, VOLUMEHI_FULL)));
}

// The blip as the selection moves: hover and the keyboard are different paths and meet in
// FocusChanged. A page focusing its first row, or a click forcing focus on its row, is not a move.
static void PlayHighlightIfMoved(UI::FocusFlags focusFlags) {
	if (!(focusFlags & UI::FocusFlags::GOT_FOCUS)) {
		return;
	}
	if (focusFlags & (UI::FocusFlags::CAUSE_FOCUS_MOVE | UI::FocusFlags::CAUSE_OTHER)) {
		PlayMenuSound(UI::UISound::LCS_HIGHLIGHT);
	}
}

// --- A row ---

// A value shown as blocks rather than a number: anything that is a position within a range.
static bool ShowsBlocks(const LCS::Option &opt) {
	return (opt.type == LCS::OptionType::Float || opt.type == LCS::OptionType::Int) && !opt.format;
}

// Ten, unless the row's range is fewer steps than that (brightness has eight).
static int BlockCount(const LCS::Option *opt) {
	return opt && opt->blocks > 0 ? opt->blocks : kSliderBlocks;
}

LCSMenuItem::LCSMenuItem(std::string_view label, UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), label_(label) {}

LCSMenuItem::LCSMenuItem(const LCS::Option *option, UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), label_(option->label), option_(option) {}

bool LCSMenuItem::IsPlaceholder() const {
	return placeholder_ || (option_ && option_->placeholder);
}

std::string LCSMenuItem::help() const {
	const std::string base = option_ ? std::string(option_->help) : help_;
	return IsPlaceholder() ? kPlaceholderPrefix + base : base;
}

void LCSMenuItem::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 100.0f;  // The row fills the screen; the text is placed from the centre.
	h = kRowHeight;
}

Bounds LCSMenuItem::BlockStripBounds() const {
	const float blockH = std::min(bounds_.h, kRowHeight) * 0.34f;
	const float gap = blockH * 0.28f;
	const int blocks = BlockCount(option_);
	const float width = blocks * blockH + (blocks - 1) * gap;
	return Bounds(ScreenCenterX() + kValueGap, bounds_.centerY() - blockH * 0.5f, width, blockH);
}

float LCSMenuItem::ValueFractionAt(float x) const {
	const Bounds strip = BlockStripBounds();
	return strip.w > 0.0f ? std::clamp((x - strip.x) / strip.w, 0.0f, 1.0f) : 0.0f;
}

void LCSMenuItem::Adjust(int direction) {
	if (!option_ || IsPlaceholder()) {
		return;
	}
	switch (option_->type) {
	case LCS::OptionType::Bool:
		LCS::SetBool(*option_, !*option_->boolValue);
		break;
	case LCS::OptionType::Float:
		// One block per press, so a press visibly moves exactly one square.
		LCS::SetNormalized(*option_, LCS::GetNormalized(*option_) + (1.0f / BlockCount(option_)) * (float)direction);
		break;
	case LCS::OptionType::Int:
		LCS::SetInt(*option_, *option_->intValue + (option_->stepInt > 0 ? option_->stepInt : 1) * direction);
		break;
	case LCS::OptionType::Choice:
		LCS::SetInt(*option_, *option_->intValue + direction);
		break;
	}
}

// A click moves the value on by one and wraps at the end, where an arrow key stops: a click has
// one direction, and a setting that could be raised and never lowered would be a trap.
void LCSMenuItem::Cycle() {
	if (!option_ || !IsEnabled() || IsPlaceholder()) {
		return;
	}
	switch (option_->type) {
	case LCS::OptionType::Bool:
		LCS::SetBool(*option_, !*option_->boolValue);
		break;
	case LCS::OptionType::Choice:
		LCS::SetInt(*option_, *option_->intValue + 1 >= option_->numChoices ? 0 : *option_->intValue + 1);
		break;
	case LCS::OptionType::Int: {
		const int step = option_->stepInt > 0 ? option_->stepInt : 1;
		LCS::SetInt(*option_, *option_->intValue + step > option_->maxInt ? option_->minInt : *option_->intValue + step);
		break;
	}
	case LCS::OptionType::Float: {
		const float stride = 1.0f / (float)BlockCount(option_);
		const float next = LCS::GetNormalized(*option_) + stride;
		LCS::SetNormalized(*option_, next > 1.0f - stride * 0.5f ? 0.0f : next);
		break;
	}
	}
}

bool LCSMenuItem::Key(const KeyInput &input) {
	if (option_ && IsEnabled() && HasFocus() && (input.flags & KeyInputFlags::DOWN)) {
		if (input.keyCode == NKCODE_DPAD_LEFT || input.keyCode == NKCODE_DPAD_RIGHT) {
			if (!IsPlaceholder()) {
				Adjust(input.keyCode == NKCODE_DPAD_LEFT ? -1 : 1);
			}
			return true;
		}
	}
	return UI::ClickableItem::Key(input);
}

Bounds LCSMenuItem::HitBounds() const {
	return hitBoundsValid_ ? hitBounds_ : bounds_;
}

// Take focus on a press, forced. Clickable::Touch takes it unforced, and TouchEvent then unfocuses
// whatever has focus on every press - which, with hover always focusing a row, cleared down_ and
// the click was lost. `force` is there for exactly this.
void LCSMenuItem::ClaimFocus() {
	UI::SetFocusedView(this, UI::FocusFlags::CAUSE_FORCED, true);
}

void LCSMenuItem::FocusChanged(UI::FocusFlags focusFlags) {
	UI::ClickableItem::FocusChanged(focusFlags);
	PlayHighlightIfMoved(focusFlags);
}

bool LCSMenuItem::Touch(const TouchInput &input) {
	const bool contains = HitBounds().Contains(input.x, input.y);

	// Hover moves the selection, which PPSSPP's UI has no notion of and a PC menu needs. Only on a
	// buttonless move, so it cannot fire in the middle of dragging a value.
	if ((input.flags & TouchInputFlags::MOVE) && input.buttons == 0 && contains && !HasFocus()) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_OTHER);
	}

	// Dragging along the blocks sets the value directly.
	if (option_ && IsEnabled() && !IsPlaceholder() && ShowsBlocks(*option_)) {
		const Bounds strip = BlockStripBounds().Expand(0.0f, 14.0f);
		if ((input.flags & TouchInputFlags::DOWN) && strip.Contains(input.x, input.y)) {
			draggingValue_ = true;
			ClaimFocus();
		}
		if (draggingValue_) {
			if (input.flags & (TouchInputFlags::DOWN | TouchInputFlags::MOVE)) {
				LCS::SetNormalized(*option_, ValueFractionAt(input.x));
			}
			if (input.flags & TouchInputFlags::UP) {
				draggingValue_ = false;
			}
			return true;
		}
	}

	// Clickable::Touch, against the glyphs' box instead of bounds_ (which is the screen's width).
	if (!IsEnabled()) {
		down_ = false;
		dragging_ = false;
		return contains;
	}
	if ((input.flags & TouchInputFlags::MOUSE) && (input.buttons & 1) == 0) {
		return contains;
	}
	if (input.flags & TouchInputFlags::DOWN) {
		// Before the flags: SetFocusedView tells the outgoing view, even this one, and that
		// clears down_.
		if (contains) {
			ClaimFocus();
		}
		down_ = contains;
		dragging_ = contains;
	} else if ((input.flags & TouchInputFlags::MOVE) && dragging_) {
		down_ = contains;
	}
	if (input.flags & TouchInputFlags::UP) {
		if ((input.flags & TouchInputFlags::CANCEL) == 0 && dragging_ && contains) {
			ClickInternal();
		}
		down_ = false;
		dragging_ = false;
	}
	return contains;
}

void LCSMenuItem::ClickInternal() {
	// A placeholder is a stop and nothing more: no sound, no event.
	if (IsPlaceholder()) {
		return;
	}
	Cycle();
	// Every click, mouse and keyboard, comes through here.
	PlayMenuSound(UI::UISound::LCS_SELECT);
	UI::ClickableItem::ClickInternal();
}

std::string LCSMenuItem::DescribeText() const {
	return option_ ? label_ + ": " + LCS::ValueText(*option_) : label_;
}

void LCSMenuItem::Draw(UIContext &dc) {
	const bool selected = HasFocus() || down_;
	uint32_t color = selected ? kItemSelectedColor : kItemColor;
	if (!IsEnabled()) {
		color = colorAlpha(kItemColor, kDisabledAlpha);
	} else if (IsPlaceholder()) {
		color = colorAlpha(color, selected ? kPlaceholderAlpha + 0.2f : kPlaceholderAlpha);
	}

	dc.SetFontStyle(kItemFont);
	// A little slack around the glyphs, so the pointer does not have to be dead on a letter.
	const float slack = 14.0f;
	float labelW = 0.0f, labelH = 0.0f;
	dc.MeasureText(kItemFont, 1.0f, 1.0f, label_, &labelW, &labelH);

	if (!option_) {
		if (leftAlignX_ >= 0.0f) {
			dc.DrawTextShadow(label_, leftAlignX_, bounds_.centerY(), color, ALIGN_VCENTER | ALIGN_LEFT);
			hitBounds_ = Bounds(leftAlignX_ - slack, bounds_.y, labelW + slack * 2.0f, bounds_.h);
		} else {
			// An action row - RESUME, BACK - centred on the screen with nothing beside it.
			dc.DrawTextShadow(label_, ScreenCenterX(), bounds_.centerY(), color, ALIGN_VCENTER | ALIGN_HCENTER);
			hitBounds_ = Bounds(ScreenCenterX() - labelW * 0.5f - slack, bounds_.y, labelW + slack * 2.0f, bounds_.h);
		}
		hitBoundsValid_ = true;
		return;
	}

	// A setting row is two columns: the label right-aligned into the centre, the value a fixed gap
	// past it, so every value on the page lines up.
	dc.DrawTextShadow(label_ + ":", ScreenCenterX(), bounds_.centerY(), color, ALIGN_VCENTER | ALIGN_RIGHT);

	float valueW = 0.0f, valueH = 0.0f;
	if (ShowsBlocks(*option_)) {
		const Bounds strip = BlockStripBounds();
		const float blockH = strip.h;
		const float gap = blockH * 0.28f;
		const int blocks = BlockCount(option_);
		const int filled = (int)(LCS::GetNormalized(*option_) * blocks + 0.5f);
		const uint32_t empty = IsEnabled() && !IsPlaceholder() ? kSliderEmptyColor : colorAlpha(kSliderEmptyColor, kPlaceholderAlpha);
		dc.Flush();
		dc.BeginNoTex();
		for (int i = 0; i < blocks; i++) {
			dc.Draw()->Rect(strip.x + i * (blockH + gap), strip.y, blockH, blockH, i < filled ? color : empty);
		}
		dc.Flush();
		dc.Begin();
		valueW = strip.w;
	} else {
		const std::string value = LCS::ValueText(*option_);
		dc.DrawTextShadow(value, ScreenCenterX() + kValueGap, bounds_.centerY(), color, ALIGN_VCENTER | ALIGN_LEFT);
		dc.MeasureText(kItemFont, 1.0f, 1.0f, value, &valueW, &valueH);
	}

	const float left = ScreenCenterX() - labelW - slack;
	const float right = ScreenCenterX() + kValueGap + valueW + slack;
	hitBounds_ = Bounds(left, bounds_.y, right - left, bounds_.h);
	hitBoundsValid_ = true;
}

// --- A line of the controls card ---

LCSBindingRow::LCSBindingRow(const LCS::KeyListingRow &row, UI::LayoutParams *layoutParams)
	: UI::ClickableItem(layoutParams), name_(row.action) {
	std::string keys;
	for (const std::string &key : row.keys) {
		if (!keys.empty()) {
			keys += kListKeySeparator;
		}
		keys += key;
	}
	cells_.push_back(keys);
	cells_.push_back(row.psp);
}

void LCSBindingRow::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 100.0f;
	h = kListRowHeight;
}

void LCSBindingRow::FocusChanged(UI::FocusFlags focusFlags) {
	UI::ClickableItem::FocusChanged(focusFlags);
	PlayHighlightIfMoved(focusFlags);
}

bool LCSBindingRow::Touch(const TouchInput &input) {
	// The highlight is the full width of the panel, so the band the pointer has to be in is too.
	const bool contains = bounds_.Contains(input.x, input.y);
	if ((input.flags & TouchInputFlags::MOVE) && input.buttons == 0 && contains && !HasFocus()) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_OTHER);
	}
	if ((input.flags & TouchInputFlags::DOWN) && contains) {
		UI::SetFocusedView(this, UI::FocusFlags::CAUSE_FORCED, true);
	}
	// A reference card: a click does nothing rather than something surprising.
	return contains;
}

std::string LCSBindingRow::DescribeText() const {
	std::string text = name_;
	bool first = true;
	for (const std::string &cell : cells_) {
		if (!cell.empty()) {
			text += first ? ": " : ", ";
			text += cell;
			first = false;
		}
	}
	return text;
}

void LCSBindingRow::Draw(UIContext &dc) {
	if (HasFocus()) {
		const float inset = g_display.dp_xres * kListPanelInsetFrac;
		dc.FillRect(UI::Drawable(kListRowColor), Bounds(inset, bounds_.y, g_display.dp_xres - inset * 2.0f, bounds_.h));
	}
	dc.SetFontStyle(kListFont);
	dc.DrawTextShadow(name_, g_display.dp_xres * kListNameFrac, bounds_.centerY(), kListNameColor, ALIGN_VCENTER | ALIGN_LEFT);
	for (size_t i = 0; i < cells_.size() && i < ARRAY_SIZE(kListDeviceFrac); i++) {
		if (!cells_[i].empty()) {
			dc.DrawTextShadow(cells_[i], g_display.dp_xres * kListDeviceFrac[i], bounds_.centerY(), kListKeyColor, ALIGN_VCENTER | ALIGN_LEFT);
		}
	}
}

// --- The saves on the memory stick ---

struct LCSSaveSlot {
	int index;
	std::string title;   // the mission the game named it after
	std::string detail;  // place, day, money, completion, as the game wrote them
	int64_t written = 0; // PARAM.SFO's modification time: the newest gets a mark
};

static Path SaveDir(int slot) {
	char dir[32];
	snprintf(dir, sizeof(dir), "%sS%d", LCS::kLCSDiscID, slot);
	return GetSysDirectory(DIRECTORY_SAVEDATA) / dir;
}

// ULUS10041S0, S1, ...: what PARAM.SFO says about each save that is there. Read, never written.
static std::vector<LCSSaveSlot> EnumerateSaves() {
	std::vector<LCSSaveSlot> slots;
	for (int i = 0; i < 10; i++) {
		std::string data;
		const Path sfoPath = SaveDir(i) / "PARAM.SFO";
		if (!File::ReadBinaryFileToString(sfoPath, &data)) {
			continue;
		}
		ParamSFOData sfo;
		if (!sfo.ReadSFO((const u8 *)data.data(), data.size())) {
			continue;
		}
		std::string detail = sfo.GetValueString("SAVEDATA_DETAIL");
		std::replace(detail.begin(), detail.end(), '\n', ' ');
		File::FileInfo info;
		const int64_t written = File::GetFileInfo(sfoPath, &info) ? (int64_t)info.mtime : 0;
		// The game names a save after the last mission passed; before the first there is no name.
		std::string title = sfo.GetValueString("SAVEDATA_TITLE");
		if (title.empty()) {
			title = "SAVED GAME";
		}
		slots.push_back({ i, title, detail, written });
	}
	return slots;
}

// A save is a directory on the memory stick, and the game lists the stick again every time it
// opens a list, so deleting one is removing it here rather than walking the game's DELETE SAVE DATA
// and a third firmware dialog to do the same.
static bool DeleteSave(int slot) {
	const Path dir = SaveDir(slot);
	if (!File::IsDirectory(dir)) {
		return false;
	}
	const bool ok = File::DeleteDirRecursively(dir);
	NOTICE_LOG(Log::System, "LCS menu: deleted save %d (%s)", slot, ok ? "ok" : "failed");
	return ok;
}

// The root row MAP, STATS or BRIEF was chosen from, so the menu that comes back after the game's
// page has gone opens on it. -1 for the usual first row.
static int g_rootReturnRow = -1;

// --- The screen ---

LCSMenuScreen::LCSMenuScreen(const Path &gamePath, bool bootPending)
	: UIBaseDialogScreen(gamePath), bootPending_(bootPending), art_(new LCSMenuArt()) {
	LCS::DropPendingMouse();
}

LCSMenuScreen::~LCSMenuScreen() {
	// Raw mouse deltas kept arriving while this was up, and would turn the camera on resume.
	LCS::DropPendingMouse();
	// Every way out comes through here, which is why the save is here rather than on a row.
	LCS::SaveSettings();
}

void LCSMenuScreen::deviceLost() {
	art_->Release();
	art_->deviceLost = true;
	UIBaseDialogScreen::deviceLost();
}

void LCSMenuScreen::deviceRestored(Draw::DrawContext *draw) {
	art_->deviceLost = false;
	UIBaseDialogScreen::deviceRestored(draw);
}

// The page table's back-link, and the only place the shape of the menu is written down.
LCSMenuPage LCSMenuScreen::ParentPage(LCSMenuPage page) const {
	switch (page) {
	case LCSMenuPage::Settings: return LCSMenuPage::Root;
	case LCSMenuPage::Controls: return LCSMenuPage::Settings;
	case LCSMenuPage::Mouse: return LCSMenuPage::Controls;
	case LCSMenuPage::Controller: return LCSMenuPage::Controls;
	case LCSMenuPage::Aiming: return LCSMenuPage::Controls;
	case LCSMenuPage::Bindings: return LCSMenuPage::Controls;
	case LCSMenuPage::KeysOnFoot: return LCSMenuPage::Bindings;
	case LCSMenuPage::KeysVehicle: return LCSMenuPage::Bindings;
	case LCSMenuPage::KeysMenu: return LCSMenuPage::Bindings;
	case LCSMenuPage::Audio: return LCSMenuPage::Settings;
	case LCSMenuPage::Graphics: return LCSMenuPage::Settings;
	case LCSMenuPage::GraphicsAdvanced: return LCSMenuPage::Graphics;
	case LCSMenuPage::Gameplay: return LCSMenuPage::Settings;
	case LCSMenuPage::Game: return LCSMenuPage::Root;
	case LCSMenuPage::LoadGame: return LCSMenuPage::Game;
	case LCSMenuPage::DeleteGame: return LCSMenuPage::Game;
	// Back out of a question and you are where you asked it from.
	case LCSMenuPage::Confirm: return confirmParent_;
	// Cheats hang off the root: they are not a setting, and nothing about them persists.
	case LCSMenuPage::Cheats: return LCSMenuPage::Root;
	case LCSMenuPage::CheatsPlayer:
	case LCSMenuPage::CheatsVehicles:
	case LCSMenuPage::CheatsPedestrians:
	case LCSMenuPage::CheatsWorld: return LCSMenuPage::Cheats;
	default: return LCSMenuPage::Root;
	}
}

bool LCSMenuScreen::IsOptionPage(LCSMenuPage page) {
	switch (page) {
	case LCSMenuPage::Mouse:
	case LCSMenuPage::Controller:
	case LCSMenuPage::Aiming:
	case LCSMenuPage::Audio:
	case LCSMenuPage::Graphics:
	case LCSMenuPage::GraphicsAdvanced:
	case LCSMenuPage::Gameplay:
		return true;
	default:
		return false;
	}
}

LCS::OptionPage LCSMenuScreen::ToOptionPage(LCSMenuPage page) {
	switch (page) {
	case LCSMenuPage::Controller: return LCS::OptionPage::Controller;
	case LCSMenuPage::Aiming: return LCS::OptionPage::Aiming;
	case LCSMenuPage::Audio: return LCS::OptionPage::Audio;
	case LCSMenuPage::Graphics: return LCS::OptionPage::Graphics;
	case LCSMenuPage::GraphicsAdvanced: return LCS::OptionPage::GraphicsAdvanced;
	case LCSMenuPage::Gameplay: return LCS::OptionPage::Gameplay;
	default: return LCS::OptionPage::Mouse;
	}
}

bool LCSMenuScreen::IsKeyListPage(LCSMenuPage page) {
	return page == LCSMenuPage::KeysOnFoot || page == LCSMenuPage::KeysVehicle || page == LCSMenuPage::KeysMenu;
}

LCS::InputContext LCSMenuScreen::ToKeyContext(LCSMenuPage page) {
	switch (page) {
	case LCSMenuPage::KeysVehicle: return LCS::InputContext::InVehicle;
	case LCSMenuPage::KeysMenu: return LCS::InputContext::Menu;
	default: return LCS::InputContext::OnFoot;
	}
}

bool LCSMenuScreen::IsCheatPage(LCSMenuPage page) {
	return page == LCSMenuPage::CheatsPlayer || page == LCSMenuPage::CheatsVehicles ||
		page == LCSMenuPage::CheatsPedestrians || page == LCSMenuPage::CheatsWorld;
}

// The heading, lowercase as the game sets them ("game", "map", "brief").
const char *LCSMenuScreen::PageTitle(LCSMenuPage page) const {
	switch (page) {
	case LCSMenuPage::Settings: return "options";
	case LCSMenuPage::Controls: return "controller setup";
	case LCSMenuPage::Mouse: return "mouse settings";
	case LCSMenuPage::Controller: return "controller";
	case LCSMenuPage::Aiming: return "aiming";
	case LCSMenuPage::Audio: return "audio setup";
	case LCSMenuPage::Graphics: return "display setup";
	case LCSMenuPage::GraphicsAdvanced: return "advanced";
	case LCSMenuPage::Gameplay: return "gameplay";
	case LCSMenuPage::Game: return "game";
	case LCSMenuPage::LoadGame: return "load game";
	case LCSMenuPage::DeleteGame: return "delete game";
	// Whatever the page that asked was showing, so the heading does not jump.
	case LCSMenuPage::Confirm: return confirmTitle_;
	case LCSMenuPage::Bindings: return "key bindings";
	case LCSMenuPage::KeysOnFoot: return "on foot";
	case LCSMenuPage::KeysVehicle: return "in vehicle";
	case LCSMenuPage::KeysMenu: return "menus";
	case LCSMenuPage::Cheats: return "cheats";
	case LCSMenuPage::CheatsPlayer: return "player";
	case LCSMenuPage::CheatsVehicles: return "vehicles";
	case LCSMenuPage::CheatsPedestrians: return "pedestrians";
	case LCSMenuPage::CheatsWorld: return "world";
	default: return "paused";
	}
}

void LCSMenuScreen::GoToPage(LCSMenuPage page) {
	for (size_t i = 0; i < rows_.size(); i++) {
		if (rows_[i]->HasFocus()) {
			lastFocus_[page_] = (int)i;
		}
	}
	// Going down a level starts at the top; coming back up finds the row it left from.
	if (page != ParentPage(page_)) {
		lastFocus_.erase(page);
	}
	page_ = page;
	RecreateViews();
}

bool LCSMenuScreen::GoBack() {
	PlayMenuSound(UI::UISound::LCS_BACK);
	if (page_ != LCSMenuPage::Root) {
		GoToPage(ParentPage(page_));
		return true;
	}
	return false;
}

void LCSMenuScreen::GoToConfirm(const char *title, std::string question, std::string detail,
	std::function<void()> action) {
	confirmTitle_ = title;
	confirmQuestion_ = std::move(question);
	confirmDetail_ = std::move(detail);
	confirmAction_ = std::move(action);
	confirmParent_ = page_;
	GoToPage(LCSMenuPage::Confirm);
}

bool LCSMenuScreen::key(const KeyInput &key) {
	if ((key.flags & KeyInputFlags::DOWN) && !(key.flags & KeyInputFlags::IS_REPEAT)) {
		// PPSSPP's own pause screen (savestates, its settings), for working on the fork. The root
		// page's bar says so.
		if (key.keyCode == NKCODE_F1 && page_ == LCSMenuPage::Root) {
			pendingEmulatorMenu_ = true;
			return true;
		}
		// Back is up one level, not "close the menu": the page table's parent link. Backspace
		// too, as the controls card lists it. Out of the root it resumes, as Escape always has.
		if (UI::IsEscapeKey(key) || key.keyCode == NKCODE_DEL) {
			if (!GoBack()) {
				TriggerFinish(DR_CANCEL);
			}
			return true;
		}
	}
	return UIBaseDialogScreen::key(key);
}

LCSMenuItem *LCSMenuScreen::AddRow(UI::ViewGroup *parent, const char *label, const char *help) {
	LCSMenuItem *row = parent->Add(new LCSMenuItem(label, new UI::LinearLayoutParams(UI::FILL_PARENT, kRowHeight)));
	if (help) {
		row->SetHelp(help);
	}
	rows_.push_back(row);
	return row;
}

void LCSMenuScreen::AddPageRow(UI::ViewGroup *parent, const char *label, LCSMenuPage target) {
	AddRow(parent, label)->OnClick.Add([this, target](UI::EventParams &e) {
		GoToPage(target);
	});
}

// A page of the game's own front end: asked for, then out of the way, because the walk to it presses
// the game's buttons and this screen pauses the game. Back from it comes here again, on this row.
void LCSMenuScreen::AddGamePageRow(UI::ViewGroup *parent, const char *label, const char *help, LCS::GamePage page) {
	LCSMenuItem *row = AddRow(parent, label, help);
	const int index = (int)rows_.size() - 1;
	row->OnClick.Add([this, page, index](UI::EventParams &e) {
		g_rootReturnRow = index;
		LCS::RequestGamePage(page);
		TriggerFinish(DR_CANCEL);
	});
}

void LCSMenuScreen::AddPlaceholderRow(UI::ViewGroup *parent, const char *label, const char *help) {
	AddRow(parent, label, help)->SetPlaceholder();
}

void LCSMenuScreen::AddBackRow(UI::ViewGroup *parent) {
	// page_ is read at click time, so one lambda serves every page.
	AddRow(parent, "BACK")->OnClick.Add([this](UI::EventParams &e) {
		GoToPage(ParentPage(page_));
	});
}

static void AddGap(UI::ViewGroup *parent) {
	// A blank half row, the way the game separates actions from settings.
	parent->Add(new UI::Spacer(0.0f, new UI::LinearLayoutParams(UI::FILL_PARENT, kRowHeight * 0.5f)));
}

void LCSMenuScreen::AddOptionRows(UI::ViewGroup *parent, LCSMenuPage page) {
	// A row standing in for part of a PPSSPP setting shows what PPSSPP has now.
	LCS::RefreshOptionProxies();
	const LCS::OptionPage optionPage = ToOptionPage(page);
	for (const LCS::Option &option : LCS::Options()) {
		if (option.page != optionPage) {
			continue;
		}
		LCSMenuItem *row = parent->Add(new LCSMenuItem(&option, new UI::LinearLayoutParams(UI::FILL_PARENT, kRowHeight)));
		if (option.enabledBy) {
			// A pointer, so the row greys the moment its switch on the same page is flipped.
			row->SetEnabledPtr(option.enabledBy);
		}
		rows_.push_back(row);
	}
	// The last row of the settings rather than an action: it leads to more of them.
	if (page == LCSMenuPage::Graphics) {
		AddPageRow(parent, "ADVANCED", LCSMenuPage::GraphicsAdvanced);
		rows_.back()->SetHelp("Texture quality, anti-aliasing, filtering and world memory.");
	}
	AddGap(parent);
	AddRow(parent, "RESTORE DEFAULTS")->OnClick.Handle(this, &LCSMenuScreen::OnRestoreDefaults);
	AddBackRow(parent);
}

void LCSMenuScreen::AddSaveRows(UI::ViewGroup *parent, bool deleting) {
	const std::vector<LCSSaveSlot> slots = EnumerateSaves();
	int64_t newest = 0;
	for (const LCSSaveSlot &slot : slots) {
		newest = std::max(newest, slot.written);
	}
	const int autoSlot = LCS::Settings().autoSaveSlot;
	for (const LCSSaveSlot &slot : slots) {
		// The number is the game's own slot; a * is the newest, the one carrying on would load.
		char label[160];
		snprintf(label, sizeof(label), "%d.  %s%s%s", slot.index + 1, slot.title.c_str(),
			slot.index == autoSlot ? "  (AUTO)" : "", slot.written == newest && slots.size() > 1 ? "  *" : "");
		LCSMenuItem *row = AddRow(parent, label);
		row->SetHelp(slot.detail);
		row->SetLeftAligned(ScreenCenterX() - kSaveListInset);
		const int index = slot.index;
		const std::string title = slot.title;
		if (deleting) {
			row->OnClick.Add([this, index, title](UI::EventParams &e) {
				GoToConfirm("delete game", "Delete " + title + "? It can't be brought back.", "",
					[this, index]() {
						pendingDelete_ = index;
					});
			});
		} else {
			row->OnClick.Add([this, index, title](UI::EventParams &e) {
				GoToConfirm("load game", "All unsaved progress in your current game will be lost. Load " + title + "?", "",
					[this, index]() {
						pendingLoad_ = index;
					});
			});
		}
	}
	if (slots.empty()) {
		LCSMenuItem *row = AddRow(parent, "NO SAVES", "Nothing saved on the memory stick yet.");
		row->SetEnabled(false);
	}
	AddBackRow(parent);
}

void LCSMenuScreen::AddBindingRows(UI::ViewGroup *parent, LCSMenuPage page) {
	const std::vector<LCS::KeyListingRow> listing = LCS::KeyListing(ToKeyContext(page));
	listRowCount_ = (int)listing.size();
	// Rows give up height to keep the whole card on the screen; there is no scrolling here.
	const float available = g_display.dp_yres - kBottomBarHeight - kListTop - kListHeaderHeight - kListPadding * 4.0f - kRowHeight;
	listRowHeight_ = listRowCount_ > 0 ? std::clamp(available / (float)listRowCount_, kListRowMinHeight, kListRowHeight) : kListRowHeight;
	for (const LCS::KeyListingRow &row : listing) {
		bindingRows_.push_back(parent->Add(new LCSBindingRow(row, new UI::LinearLayoutParams(UI::FILL_PARENT, listRowHeight_))));
	}
	parent->Add(new UI::Spacer(0.0f, new UI::LinearLayoutParams(UI::FILL_PARENT, kListPadding * 2.0f)));
	AddBackRow(parent);
}

void LCSMenuScreen::AddCheatRows(UI::ViewGroup *parent, LCSMenuPage page) {
	for (const LCSCheat &cheat : kCheats) {
		if (cheat.group != page) {
			continue;
		}
		const char *sequence = cheat.sequence;
		AddRow(parent, cheat.name, cheat.help)->OnClick.Add([this, sequence](UI::EventParams &e) {
			std::vector<u32> taps;
			std::string bad;
			if (!LCS::ParsePadSequence(sequence, &taps, &bad)) {
				ERROR_LOG(Log::System, "LCS menu: cheat '%s' has an unknown button '%s'", sequence, bad.c_str());
				return;
			}
			// Queued, then out of the way: the presses play on the running game, and closing the
			// menu is what lets it run. DR_CANCEL is EmuScreen's "continue".
			LCS::QueuePadTaps(taps);
			NOTICE_LOG(Log::System, "LCS menu: cheat queued (%s)", sequence);
			TriggerFinish(DR_CANCEL);
		});
	}
	AddBackRow(parent);
}

// Rows give up height, down to a floor, when a page is taller than the room between the heading
// and the bar. The list is centred, so the room is the screen less the bar at both ends.
void LCSMenuScreen::FitRows(UI::LinearLayout *list) {
	float total = 0.0f;
	for (int i = 0; i < list->GetNumSubviews(); i++) {
		total += list->GetViewByIndex(i)->GetLayoutParams()->height;
	}
	const float room = g_display.dp_yres - (kBottomBarHeight + 12.0f) * 2.0f;
	if (total <= room || total <= 0.0f) {
		return;
	}
	const float scale = std::max(room / total, kRowMinHeight / kRowHeight);
	for (int i = 0; i < list->GetNumSubviews(); i++) {
		UI::View *view = list->GetViewByIndex(i);
		view->ReplaceLayoutParams(new UI::LinearLayoutParams(UI::FILL_PARENT, view->GetLayoutParams()->height * scale));
	}
}

Bounds LCSMenuScreen::ListPanel() const {
	const float inset = g_display.dp_xres * kListPanelInsetFrac;
	const float height = kListHeaderHeight + (float)listRowCount_ * listRowHeight_ + kListPadding;
	return Bounds(inset, kListTop, g_display.dp_xres - inset * 2.0f, height);
}

void LCSMenuScreen::CreateViews() {
	using namespace UI;

	rows_.clear();
	bindingRows_.clear();
	listRowCount_ = 0;
	listRowHeight_ = kListRowHeight;

	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));

	// Vertically centred (both anchors NONE), except the controls card: a table under a header
	// band, whose panel is drawn at a fixed top.
	const bool listing = IsKeyListPage(page_);
	LinearLayout *list = root_->Add(new LinearLayout(ORIENT_VERTICAL,
		new AnchorLayoutParams(FILL_PARENT, WRAP_CONTENT, 0.0f, listing ? kListTop + kListHeaderHeight : NONE, 0.0f, NONE)));
	list->SetSpacing(0.0f);
	// Named per page. RecreateViews hands focus back to the view at the same place in the tree,
	// and with every page's rows at the same place a new page opened on the old page's row number.
	char tag[32];
	snprintf(tag, sizeof(tag), "lcsmenu%d", (int)page_);
	list->SetTag(tag);

	switch (page_) {
	case LCSMenuPage::Root:
		AddRow(list, "RESUME")->OnClick.Handle(this, &LCSMenuScreen::OnResume);
		AddPageRow(list, "START NEW GAME", LCSMenuPage::Game);
		AddGamePageRow(list, "MAP", "The city map, with your position and the places you have found.", LCS::GamePage::Map);
		AddGamePageRow(list, "STATS", "Everything the game has been counting.", LCS::GamePage::Stats);
		AddGamePageRow(list, "BRIEF", "What you were last told to do.", LCS::GamePage::Brief);
		AddPageRow(list, "CHEATS", LCSMenuPage::Cheats);
		AddPageRow(list, "OPTIONS", LCSMenuPage::Settings);
		AddRow(list, "QUIT GAME")->OnClick.Handle(this, &LCSMenuScreen::OnExitGame);
		break;
	case LCSMenuPage::Settings:
		AddPageRow(list, "CONTROLLER SETUP", LCSMenuPage::Controls);
		AddPageRow(list, "AUDIO SETUP", LCSMenuPage::Audio);
		AddPageRow(list, "DISPLAY SETUP", LCSMenuPage::Graphics);
		AddPageRow(list, "GAMEPLAY", LCSMenuPage::Gameplay);
		AddBackRow(list);
		break;
	case LCSMenuPage::Controls:
		AddPageRow(list, "MOUSE SETTINGS", LCSMenuPage::Mouse);
		AddPageRow(list, "CONTROLLER", LCSMenuPage::Controller);
		AddPageRow(list, "AIMING", LCSMenuPage::Aiming);
		AddPageRow(list, "KEY BINDINGS", LCSMenuPage::Bindings);
		AddBackRow(list);
		break;
	case LCSMenuPage::Bindings:
		AddPageRow(list, "ON FOOT", LCSMenuPage::KeysOnFoot);
		AddPageRow(list, "IN VEHICLE", LCSMenuPage::KeysVehicle);
		AddPageRow(list, "MENUS", LCSMenuPage::KeysMenu);
		AddGap(list);
		AddPlaceholderRow(list, "RESET KEYS", "Put every key back where it started.");
		AddBackRow(list);
		break;
	case LCSMenuPage::Game:
		// The game's own Game tab, rebuilt: the same three things, in the same order.
		AddRow(list, "NEW GAME", "Start the story again from the beginning.")->OnClick.Add([this](UI::EventParams &e) {
			GoToConfirm("new game", "Are you sure you want to start a new game? All progress since the last save game will be lost. Proceed?", "",
				[this]() {
					pendingNewGame_ = true;
				});
		});
		AddPageRow(list, "LOAD GAME", LCSMenuPage::LoadGame);
		rows_.back()->SetHelp("The saves on the memory stick. * is the newest.");
		AddPageRow(list, "DELETE GAME", LCSMenuPage::DeleteGame);
		AddBackRow(list);
		break;
	case LCSMenuPage::LoadGame:
		AddSaveRows(list, false);
		break;
	case LCSMenuPage::DeleteGame:
		AddSaveRows(list, true);
		break;
	case LCSMenuPage::Confirm: {
		// NO first, and focused: the answer to a question nobody has read yet should change nothing.
		AddRow(list, "NO", "Go back without changing anything.")->OnClick.Add([this](UI::EventParams &e) {
			GoToPage(confirmParent_);
		});
		LCSMenuItem *yes = AddRow(list, "YES");
		yes->SetHelp(confirmDetail_);
		yes->OnClick.Add([this](UI::EventParams &e) {
			// Records what to do; update() does it.
			if (confirmAction_) {
				confirmAction_();
			}
		});
		break;
	}
	case LCSMenuPage::Cheats:
		AddPageRow(list, "PLAYER", LCSMenuPage::CheatsPlayer);
		AddPageRow(list, "VEHICLES", LCSMenuPage::CheatsVehicles);
		AddPageRow(list, "PEDESTRIANS", LCSMenuPage::CheatsPedestrians);
		AddPageRow(list, "WORLD", LCSMenuPage::CheatsWorld);
		AddBackRow(list);
		break;
	default:
		if (IsCheatPage(page_)) {
			AddCheatRows(list, page_);
		} else if (IsKeyListPage(page_)) {
			AddBindingRows(list, page_);
		} else if (IsOptionPage(page_)) {
			AddOptionRows(list, page_);
		}
		break;
	}

	if (!listing) {
		FitRows(list);
	}

	// A card has no menu rows above its lines, so its first line takes focus; otherwise the row
	// this page was left on, or the first, which on a question is NO.
	if (!bindingRows_.empty()) {
		UI::SetFocusedView(bindingRows_[0], UI::FocusFlags::CAUSE_SCREEN_CHANGE);
	} else if (!rows_.empty()) {
		auto it = lastFocus_.find(page_);
		int index = it != lastFocus_.end() && it->second < (int)rows_.size() ? it->second : 0;
		// Coming back from one of the game's pages: the row it was opened from.
		if (page_ == LCSMenuPage::Root && g_rootReturnRow >= 0 && g_rootReturnRow < (int)rows_.size()) {
			index = g_rootReturnRow;
			g_rootReturnRow = -1;
		}
		UI::SetFocusedView(rows_[index], UI::FocusFlags::CAUSE_SCREEN_CHANGE);
	}
}

void LCSMenuScreen::update() {
	// Pending actions, done here rather than in the click that recorded them: a handler that
	// rebuilds or finishes the screen it runs in is not safe.
	if (pendingExit_) {
		pendingExit_ = false;
		// What PPSSPP's own pause screen does with Exit: the app, or back to the game list.
		if (g_Config.bPauseMenuExitsEmulator) {
			System_ExitApp();
		} else {
			TriggerFinish(DR_OK);
		}
		return;
	}
	if (pendingEmulatorMenu_) {
		pendingEmulatorMenu_ = false;
		screenManager()->switchScreen(new GamePauseScreen(gamePath_, bootPending_));
		return;
	}
	// Loading and a new game go through the game's own menu, which needs the game running: asked
	// for, and this screen gets out of the way.
	if (pendingLoad_ >= 0) {
		LCS::RequestLoadSlot(pendingLoad_);
		pendingLoad_ = -1;
		TriggerFinish(DR_CANCEL);
		return;
	}
	if (pendingNewGame_) {
		pendingNewGame_ = false;
		LCS::RequestNewGame();
		TriggerFinish(DR_CANCEL);
		return;
	}
	if (pendingDelete_ >= 0) {
		const int slot = pendingDelete_;
		pendingDelete_ = -1;
		if (!DeleteSave(slot)) {
			g_OSD.Show(OSDType::MESSAGE_ERROR, "That save couldn't be deleted", 3.0f, "lcs_menu");
		}
		page_ = LCSMenuPage::DeleteGame;
		lastFocus_.erase(LCSMenuPage::DeleteGame);
		RecreateViews();
		return;
	}

	// A screen that pauses the game has to say so: without it the UI state stays INGAME and the
	// mouse pointer never comes back (CorrectCursor in Windows/MainWindow.cpp), among others.
	UpdateUIState(UISTATE_PAUSEMENU);
	UIBaseDialogScreen::update();
}

void LCSMenuScreen::DrawBackground(UIContext &dc) {
	const Bounds &bounds = dc.GetBounds();

	// Opaque, as the game's own is: the paused world showing through would fight the text. The
	// backdrop is stretched over the screen, as the game stretches it over the PSP's - it is drawn
	// for that, so "cover" cropping would cut the bridge's deck off.
	dc.FillRect(UI::Drawable(kBackgroundColor), bounds);
	if (Draw::Texture *backdrop = art_->Image(dc, "lcs/background.png")) {
		DrawTexture(dc, backdrop, Bounds(bounds.x, bounds.y, bounds.w, bounds.h - kBottomBarHeight), 0xFFFFFFFF);
	}

	// The heading, top left.
	dc.SetFontStyle(kTitleFont);
	dc.DrawTextShadow(PageTitle(page_), kTitleLeft, kTitleTop + kTitleHeight * 0.5f, kTitleColor, ALIGN_VCENTER | ALIGN_LEFT);

	// The question, under the heading against the same margin, wrapped in a box so a long one fits.
	if (page_ == LCSMenuPage::Confirm) {
		const Bounds box(kTitleLeft, kTitleTop + kTitleHeight + 8.0f, bounds.w - kTitleLeft * 2.0f, 120.0f);
		dc.SetFontStyle(kConfirmFont);
		dc.DrawTextShadowRect(confirmQuestion_, box, kHintColor, ALIGN_TOP | ALIGN_LEFT | FLAG_WRAP_TEXT);
		if (!confirmDetail_.empty()) {
			dc.SetFontStyle(kConfirmDetailFont);
			dc.DrawTextShadowRect(confirmDetail_, Bounds(box.x, box.y + 84.0f, box.w, 60.0f),
				colorAlpha(kHintColor, 0.7f), ALIGN_TOP | ALIGN_LEFT | FLAG_WRAP_TEXT);
		}
	}

	// The card's panel, sized to its rows, with the column names in the band along its top.
	if (IsKeyListPage(page_)) {
		const Bounds panel = ListPanel();
		dc.FillRect(UI::Drawable(kListPanelColor), panel);
		dc.SetFontStyle(kListHeaderFont);
		const float headerY = panel.y + kListHeaderHeight * 0.5f;
		dc.DrawText(kListHeaders[0], g_display.dp_xres * kListNameFrac, headerY, kListHeaderColor, ALIGN_VCENTER | ALIGN_LEFT);
		for (size_t i = 0; i < ARRAY_SIZE(kListDeviceFrac); i++) {
			dc.DrawText(kListHeaders[i + 1], g_display.dp_xres * kListDeviceFrac[i], headerY, kListHeaderColor, ALIGN_VCENTER | ALIGN_LEFT);
		}
		dc.FillRect(UI::Drawable(kListRuleColor), Bounds(panel.x, panel.y + kListHeaderHeight - 2.0f, panel.w, 1.0f));
	}

	// The bar along the bottom, in the game's strip colours: what the selected row means on the
	// left, how to work it on the right.
	const Bounds bar(bounds.x, bounds.y2() - kBottomBarHeight, bounds.w, kBottomBarHeight);
	dc.FillRect(UI::Drawable(kBarColor), bar);
	dc.FillRect(UI::Drawable(kBarEdgeColor), Bounds(bar.x, bar.y, bar.w, 2.0f));

	const LCSMenuItem *focused = nullptr;
	for (const LCSMenuItem *row : rows_) {
		if (row->HasFocus()) {
			focused = row;
			break;
		}
	}

	dc.SetFontStyle(dc.GetTheme().uiFontSmall);
	// A page-wide note outranks the selected row's help: it is true of everything on the page.
	std::string note;
	if (IsKeyListPage(page_)) {
		note = kBindingsNote;
	} else if (page_ == LCSMenuPage::Cheats) {
		note = kCheatNote;
	} else if (focused) {
		note = focused->help();
	}
	if (!note.empty()) {
		dc.DrawText(note, bar.x + kTitleLeft, bar.centerY(), colorAlpha(kHintColor, 0.85f), ALIGN_VCENTER | ALIGN_LEFT);
	}

	const char *hint = "ENTER / LMB - SELECT     ESC - BACK";
	if (page_ == LCSMenuPage::Root) {
		hint = "ENTER / LMB - SELECT     ESC - RESUME     F1 - PPSSPP MENU";
	} else if (IsKeyListPage(page_)) {
		hint = "ESC - BACK";
	} else if (focused && focused->option() && !focused->IsPlaceholder()) {
		hint = focused->option()->type == LCS::OptionType::Bool
			? "ENTER / LMB - TOGGLE     ESC - BACK"
			: "LEFT / RIGHT - ADJUST     ESC - BACK";
	}
	dc.DrawText(hint, bar.x2() - kTitleLeft, bar.centerY(), kHintColor, ALIGN_VCENTER | ALIGN_RIGHT);

	dc.Flush();
}

void LCSMenuScreen::OnResume(UI::EventParams &e) {
	// DR_CANCEL is EmuScreen's "continue".
	TriggerFinish(DR_CANCEL);
}

void LCSMenuScreen::OnExitGame(UI::EventParams &e) {
	GoToConfirm("quit", "Are you sure you want to quit? All progress since the last save game will be lost. Proceed?", "",
		[this]() {
			pendingExit_ = true;
		});
}

void LCSMenuScreen::OnRestoreDefaults(UI::EventParams &e) {
	LCS::ResetPage(ToOptionPage(page_));
}

UIScreen *CreatePauseScreen(const Path &gamePath, bool bootPending) {
	if (!LCS::IsActive()) {
		return new GamePauseScreen(gamePath, bootPending);
	}
	// While a sequence walks the game's menu, or the firmware's save list is up (it has its own
	// Back), a pause would stop the thing it would interrupt half way.
	if (LCS::ActiveCurtain() != LCS::CurtainKind::None || LCS::FrontEndBusy() || LCS::SaveDialogUp()) {
		return nullptr;
	}
	// With the game's own menu up, the pause key puts it away. On a page this fork opened, this
	// fork's menu comes back afterwards (LCSMenuDue); the game's own menu, opened with its Start,
	// just closes.
	if (LCS::GameMenuUp()) {
		LCS::RequestCloseGameMenu(LCS::BridgeOwnsMenu());
		return nullptr;
	}
	return new LCSMenuScreen(gamePath, bootPending);
}

bool LCSMenuDue() {
	if (!LCS::IsActive() || LCS::GameMenuUp() || LCS::FrontEndBusy()) {
		return false;
	}
	if (!LCS::TakeMenuAfterClose()) {
		return false;
	}
	// The curtain that covered the frames between the game's menu going and this one coming: this
	// one is opaque, and the tick that would take it down stops while it is up.
	LCS::DropCurtain();
	return true;
}

// --- Over the game: the curtain, and the hint line on the game's own pages ---
//
// Drawn by EmuScreen rather than pushed as a screen: a screen pauses the emulator, and what these
// cover is the game working. Their art is their own, made on the first draw and released with the
// device.

static std::unique_ptr<LCSMenuArt> g_overlayArt;

static const FontStyle kCurtainFont(FontFamily::Display, 44, FontStyleFlags::Default);
static const FontStyle kStripFont(FontFamily::SansSerif, 22, FontStyleFlags::Default);
static constexpr float kCurtainFadeSeconds = 0.5f;

void ReleaseLCSOverlayArt() {
	g_overlayArt.reset();
}

// The menu's backdrop, the whole screen, and one word bottom left: the VCS fork's loading screen
// over LCS's own pause-menu backdrop. No word for a menu transition (the menu the player was
// looking at is about to be back). The dots are there so a still screen doesn't read as a hang.
static void DrawCurtain(UIContext &dc, float alpha, const char *word) {
	if (alpha <= 0.0f) {
		return;
	}
	alpha = std::min(alpha, 1.0f);
	if (!g_overlayArt) {
		g_overlayArt.reset(new LCSMenuArt());
	}
	const Bounds &bounds = dc.GetBounds();
	// The flat fill first and always: a build without assets/lcs gets a plain dark loading screen,
	// not a transparent one with the walk showing through.
	dc.FillRect(UI::Drawable(colorAlpha(kBackgroundColor, alpha)), bounds);
	if (Draw::Texture *backdrop = g_overlayArt->Image(dc, "lcs/background.png")) {
		DrawTexture(dc, backdrop, bounds, colorAlpha(0xFFFFFFFF, alpha));
	}
	if (word) {
		const int dots = (int)(time_now_d() * 2.0) % 4;
		char text[32];
		snprintf(text, sizeof(text), "%s%.*s", word, dots, "...");
		dc.SetFontStyle(kCurtainFont);
		dc.DrawTextShadow(text, bounds.x + kTitleLeft, bounds.y2() - kTitleTop,
			colorAlpha(kItemColor, alpha), ALIGN_LEFT | ALIGN_BOTTOM);
	}
	dc.Flush();
}

// What works on the game's page, where its own button hints were: the keys of this fork, in the
// strip along the bottom of the PSP's picture (the bottom 48 of its 272 lines).
static void DrawPageHints(UIContext &dc, int page) {
	const char *hint = "ESC - BACK";
	if (page == LCS::kLCSPageMap) {
		hint = "WASD - SCROLL     WHEEL / Q E - ZOOM     F - MARKER     ENTER - LEGEND     ESC - BACK";
	} else if (page == LCS::kLCSPageBrief || page == LCS::kLCSPageStats) {
		hint = "W / S - SCROLL     ESC - BACK";
	}
	const DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
	const FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, g_display.pixel_xres, g_display.pixel_yres);
	FRect rc;
	CalculateDisplayOutputRect(config, &rc, 480.0f, 272.0f, frame, config.iInternalScreenRotation);
	// Pixels to the UI's units.
	const float sx = g_display.pixel_xres > 0 ? g_display.dp_xres / (float)g_display.pixel_xres : 1.0f;
	const float sy = g_display.pixel_yres > 0 ? g_display.dp_yres / (float)g_display.pixel_yres : 1.0f;
	const float stripTop = (rc.y + rc.h * (224.0f / 272.0f)) * sy;
	const float stripBottom = (rc.y + rc.h) * sy;
	const float right = (rc.x + rc.w) * sx - kTitleLeft * 0.5f;
	dc.SetFontStyle(kStripFont);
	dc.DrawTextShadow(hint, right, (stripTop + stripBottom) * 0.5f, kHintColor, ALIGN_VCENTER | ALIGN_RIGHT);
	dc.Flush();
}

// The curtain's fade-out, after the sequence has let it go: wall clock, so it takes the same time on
// a machine that isn't keeping up.
static bool g_curtainWasUp = false;
static double g_curtainFadeStart = 0.0;
static const char *g_curtainWord = nullptr;

bool LCSOverlayVisible() {
	return LCS::IsActive() && (g_curtainWasUp || LCS::ActiveCurtain() != LCS::CurtainKind::None || LCS::OwnedGamePage() >= 0);
}

void DrawLCSOverlay(UIContext &dc) {
	bool &wasUp = g_curtainWasUp;
	double &fadeStart = g_curtainFadeStart;
	const char *&word = g_curtainWord;
	if (!LCS::IsActive()) {
		wasUp = false;
		return;
	}
	const LCS::CurtainKind kind = LCS::ActiveCurtain();
	if (kind != LCS::CurtainKind::None) {
		// Remembered for the fade, which runs after the sequence has let go of the kind.
		word = kind == LCS::CurtainKind::Saving ? "SAVING" : kind == LCS::CurtainKind::Loading ? "LOADING" : nullptr;
		wasUp = true;
		fadeStart = 0.0;
		DrawCurtain(dc, 1.0f, word);
		return;
	}
	const int page = LCS::OwnedGamePage();
	if (page >= 0) {
		DrawPageHints(dc, page);
	}
	if (!wasUp) {
		return;
	}
	const double now = time_now_d();
	if (fadeStart == 0.0) {
		fadeStart = now;
	}
	const float t = (float)(now - fadeStart) / kCurtainFadeSeconds;
	if (t >= 1.0f || page >= 0) {
		// A page the bridge opened is what the player asked to look at: no fade over it.
		wasUp = false;
		return;
	}
	DrawCurtain(dc, 1.0f - t, word);
}
