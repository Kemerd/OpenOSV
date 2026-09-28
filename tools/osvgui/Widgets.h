// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Widgets.h - the controls OpenOSV Studio is built from, drawn on top of
// Dear ImGui: springs for motion, vector icons, switches, segmented
// controls, buttons, progress bars, cards and setting rows.
//
// Motion is physical, not timed: every animated value is a damped spring
// (stiffness, damping ratio), so a switch flicked twice mid-animation turns
// round smoothly instead of restarting.  Icons are drawn from primitives at
// the size asked for, so they are sharp at any scale and on any platform
// without an icon font.
#pragma once

#include "Settings.h"  // Choice

#include <imgui.h>

#include <span>
#include <string>
#include <string_view>

namespace osvgui::ui {

// ===========================================================================
//  Springs
// ===========================================================================

/// Call once at the start of every frame.
void beginFrame() noexcept;

/// The value of the spring `id` moving towards `target`, advanced once per
/// frame.  A new spring starts AT its target (nothing animates on first
/// sight).  `stiffness` in 1/s^2, `dampingRatio` 1 = critically damped,
/// below 1 a little overshoot.
float spring(ImGuiID id, float target, float stiffness = 280.0f, float dampingRatio = 0.9f) noexcept;

/// Some spring is still moving: the loop should keep drawing frames.
[[nodiscard]] bool animating() noexcept;

/// Ask for frames for a moment (e.g. an indeterminate bar is on screen).
void keepAnimating() noexcept;

// ===========================================================================
//  Icons
// ===========================================================================

enum class Icon {
    None,
    Plus,
    Folder,
    Play,
    Pause,
    Stop,
    Close,
    Check,
    Alert,
    Copy,
    ChevronDown,
    ChevronRight,
    Sun,
    Moon,
    Terminal,
    Tray,
    Reveal,
    Retry,
    Trash,
    Skip,
    Sparkle,
};

/// Draw `icon` centred on `center`, `size` pixels across, in `colour`.
/// `background` is the colour under it (the moon's cut-out).
void drawIcon(ImDrawList* list, Icon icon, ImVec2 center, float size, ImU32 colour, ImU32 background = 0) noexcept;

// ===========================================================================
//  Controls
// ===========================================================================

/// How a button presents itself.
enum class ButtonKind {
    Primary,      ///< Filled with the accent: the one thing to do next.
    Secondary,    ///< Quiet fill: everything else.
    Destructive,  ///< Tinted red: stops or removes.
    Plain,        ///< No fill until hovered: toolbar and inline actions.
};

/// A button with an optional icon.  `width` 0 = fit the label; `height` 0
/// = the frame height.  Returns true when clicked.
bool button(const char* label, Icon icon = Icon::None, ButtonKind kind = ButtonKind::Secondary, float width = 0.0f,
            float height = 0.0f, bool enabled = true);

/// The width button() gives `label` when it fits it (for right-aligning a
/// row of buttons before drawing them).
[[nodiscard]] float buttonWidth(const char* label, Icon icon = Icon::None, ButtonKind kind = ButtonKind::Secondary);

/// A square icon-only button with a tooltip.
bool iconButton(const char* id, Icon icon, const char* tooltip, bool enabled = true, float size = 0.0f);

/// An iOS / macOS style switch.  Returns true when it changed.
bool toggleSwitch(const char* id, bool* value, bool enabled = true);

/// A settings row: `label` on the left, a switch on the right, the whole
/// row clickable.  Returns true when it changed.
bool toggleRow(const char* label, bool* value, bool enabled = true);

/// A macOS-style slider: a thin track, a round knob, the value on the
/// right.  A range that spans zero fills from the centre (pan, tilt, roll).
/// Double-click resets it to `resetTo`.  `formatValue` turns the value into
/// its label.  Returns true when the value changed.
bool slider(const char* id, float* value, float min, float max, float resetTo, float width,
            std::string (*formatValue)(float), bool enabled = true);

/// A segmented control over `labels`; the selection slides between them.
/// Returns true when the selection changed.
bool segmented(const char* id, const char* const* labels, int count, int* index, float width, bool enabled = true);

/// A segmented control over choice tokens.
bool segmentedChoice(const char* id, std::span<const Choice> choices, std::string& token, float width,
                     bool enabled = true);

/// A drop-down over choice tokens.
bool comboChoice(const char* id, std::span<const Choice> choices, std::string& token, float width, bool enabled = true);

/// ImGui's BeginCombo with the app's look (a quiet chevron instead of the
/// arrow box).  Call ImGui::EndCombo() when it returns true, as usual.
bool beginCombo(const char* id, const char* preview, float width, bool enabled = true);

/// A rounded progress bar; the fill glides to `fraction`.  `indeterminate`
/// shows a moving sheen instead (work started, amount not known yet).
void progressBar(const char* id, float fraction, float width, float height, ImU32 fill, bool indeterminate = false);

/// The same bar drawn at `pos` without taking layout space (inside a row
/// that is itself one item).  `id` keys its fill spring.
void drawProgress(ImDrawList* list, ImVec2 pos, ImVec2 size, ImGuiID id, float fraction, ImU32 fill,
                  bool indeterminate) noexcept;

/// A rounded rectangle outline in dashes (a drop zone).
void dashedRect(ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness, float dash,
                float gap) noexcept;

/// A spinning arc (work in progress), centred on `center`.
void spinner(ImDrawList* list, ImVec2 center, float radius, ImU32 colour, float thickness) noexcept;

// ===========================================================================
//  Layout
// ===========================================================================

/// A small, quiet heading above a card ("OUTPUT").
void sectionHeader(const char* text);

/// A grouped card; content goes between begin and end.  Always pair them.
void beginCard(const char* id);
void endCard();

/// A settings row's label.  Returns the width left for the control, which
/// the caller draws next (on the same line, or below on narrow cards).
float rowLabel(const char* label, bool enabled = true);

/// Secondary explanatory text, wrapped to the available width.
void caption(const char* text);

/// A thin separator inside a card.
void hairline();

/// `text` cut to `maxWidth` pixels in the current font with "..." in the
/// middle (paths keep their start and their file name) or at the end.
[[nodiscard]] std::string ellipsize(std::string_view text, float maxWidth, bool middle);

/// A delayed tooltip on the last item.
void tooltip(const char* text);

/// Scaled length (a design length in points times the UI scale).
[[nodiscard]] float px(float points) noexcept;

}  // namespace osvgui::ui
