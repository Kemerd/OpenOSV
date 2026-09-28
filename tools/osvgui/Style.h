// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Style.h - OpenOSV Studio's look: the two palettes (dark, light), the
// ImGui style built from them, and the system fonts it draws with.
//
// The palettes follow the system colours of Apple's Human Interface
// Guidelines: grouped surfaces on a slightly darker (or, in light mode,
// greyer) window, three levels of label contrast, and one accent - system
// blue - for what can be clicked and what is in progress.  Green, red and
// orange only ever mean done, failed and "look here".
//
// No font file ships with the program.  The platform's own UI face is
// loaded at run time (Segoe UI Variable / Segoe UI on Windows, SF Pro on
// macOS), with ImGui's built-in font as the last resort.
#pragma once

#include <imgui.h>

namespace osvgui::style {

/// Every colour the UI uses.
struct Palette {
    ImVec4 window;       ///< Window background.
    ImVec4 surface;      ///< Cards / grouped sections.
    ImVec4 surfaceAlt;   ///< Rows on hover, secondary surfaces.
    ImVec4 border;       ///< Hairlines around cards.
    ImVec4 field;        ///< Controls at rest (buttons, combos, text fields).
    ImVec4 fieldHover;   ///< Controls under the pointer.
    ImVec4 popup;        ///< Menus and popups.
    ImVec4 text;         ///< Primary label.
    ImVec4 text2;        ///< Secondary label.
    ImVec4 text3;        ///< Tertiary label, placeholders, disabled.
    ImVec4 accent;       ///< System blue.
    ImVec4 accentHover;  ///< Accent under the pointer.
    ImVec4 accentPress;  ///< Accent while pressed.
    ImVec4 onAccent;     ///< Text on the accent.
    ImVec4 success;      ///< Done.
    ImVec4 danger;       ///< Failed, destructive.
    ImVec4 warning;      ///< Needs attention.
    ImVec4 trackOff;     ///< A switch's track when off, a progress bar's track.
    ImVec4 knob;         ///< A switch's knob.
    ImVec4 shadow;       ///< Soft shadows under knobs and pills.
    ImVec4 selection;    ///< Selected rows, text selection.
    bool dark = true;
};

/// The palette in force.
[[nodiscard]] const Palette& palette() noexcept;

/// Switch palette and rebuild ImGui's style for `uiScale` (1 = 96 dpi on
/// Windows; macOS keeps 1 and lets the framebuffer scale do the rest).
void apply(bool dark, float uiScale) noexcept;

/// The scale apply() was last called with.
[[nodiscard]] float scale() noexcept;

/// A colour as ImU32, its alpha multiplied by `alpha`.
[[nodiscard]] ImU32 u32(const ImVec4& colour, float alpha = 1.0f) noexcept;

/// Mix of two colours (t = 0: a, t = 1: b).
[[nodiscard]] ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) noexcept;

// ---- fonts ------------------------------------------------------------------------

/// The faces (null until loadFonts ran; `bold` / `mono` fall back to `ui`).
struct Fonts {
    ImFont* ui = nullptr;    ///< Body text.
    ImFont* bold = nullptr;  ///< Titles, names.
    ImFont* mono = nullptr;  ///< The command line and the log.
};

/// Unscaled sizes (ImGui applies the DPI scale on top).
inline constexpr float kBodySize = 15.0f;
inline constexpr float kSmallSize = 13.0f;
inline constexpr float kTinySize = 11.5f;
inline constexpr float kTitleSize = 21.0f;
inline constexpr float kHeadingSize = 16.0f;
inline constexpr float kMonoSize = 13.0f;

/// The loaded faces.
[[nodiscard]] const Fonts& fonts() noexcept;

/// Load the system faces into ImGui's atlas (once, after CreateContext).
/// Always leaves usable fonts behind.
void loadFonts() noexcept;

}  // namespace osvgui::style
