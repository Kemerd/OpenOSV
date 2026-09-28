// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Style.cpp - palettes, the ImGui style and the system fonts (see Style.h).

#include "Style.h"

#include "Queue.h"  // pathToUtf8

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace osvgui::style {

namespace fs = std::filesystem;

namespace {

/// 0xRRGGBB (+ alpha) as a colour.
constexpr ImVec4 rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

/// Dark: near-black window, raised grey cards, system blue.
Palette darkPalette() {
    Palette p;
    p.window = rgb(0x121214);
    p.surface = rgb(0x1C1C1F);
    p.surfaceAlt = rgb(0x26262A);
    p.border = rgb(0xFFFFFF, 0.07f);
    p.field = rgb(0x2C2C30);
    p.fieldHover = rgb(0x36363B);
    p.popup = rgb(0x242428);
    p.text = rgb(0xF5F5F7);
    p.text2 = rgb(0xA1A1A8);
    p.text3 = rgb(0x6C6C73);
    p.accent = rgb(0x0A84FF);
    p.accentHover = rgb(0x3395FF);
    p.accentPress = rgb(0x0071E3);
    p.onAccent = rgb(0xFFFFFF);
    p.success = rgb(0x30D158);
    p.danger = rgb(0xFF453A);
    p.warning = rgb(0xFF9F0A);
    p.trackOff = rgb(0x39393E);
    p.knob = rgb(0xFFFFFF);
    p.shadow = rgb(0x000000, 0.45f);
    p.selection = rgb(0x0A84FF, 0.22f);
    p.dark = true;
    return p;
}

/// Light: the grouped-background grey, white cards, system blue.
Palette lightPalette() {
    Palette p;
    p.window = rgb(0xF2F2F7);
    p.surface = rgb(0xFFFFFF);
    p.surfaceAlt = rgb(0xF4F4F7);
    p.border = rgb(0x000000, 0.07f);
    p.field = rgb(0xEDEDF0);
    p.fieldHover = rgb(0xE3E3E8);
    p.popup = rgb(0xFFFFFF);
    p.text = rgb(0x1C1C1E);
    p.text2 = rgb(0x6C6C70);
    p.text3 = rgb(0xA3A3A8);
    p.accent = rgb(0x007AFF);
    p.accentHover = rgb(0x1A88FF);
    p.accentPress = rgb(0x0062CC);
    p.onAccent = rgb(0xFFFFFF);
    p.success = rgb(0x28A745);
    p.danger = rgb(0xFF3B30);
    p.warning = rgb(0xF08C00);
    p.trackOff = rgb(0xE2E2E7);
    p.knob = rgb(0xFFFFFF);
    p.shadow = rgb(0x000000, 0.18f);
    p.selection = rgb(0x007AFF, 0.16f);
    p.dark = false;
    return p;
}

Palette g_palette = darkPalette();
float g_scale = 1.0f;
Fonts g_fonts;

/// The first of `candidates` that exists, as UTF-8 (empty when none).
std::string firstExisting(const std::vector<fs::path>& candidates) {
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return pathToUtf8(candidate);
        }
    }
    return {};
}

/// The system font folder.
fs::path systemFontFolder() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const UINT got = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    if (got > 0 && got < MAX_PATH) {
        return fs::path(std::wstring(buffer, got)) / L"Fonts";
    }
    return fs::path(L"C:\\Windows\\Fonts");
#elif defined(__APPLE__)
    return fs::path("/System/Library/Fonts");
#else
    return fs::path("/usr/share/fonts");
#endif
}

/// Add a font file, or return null when it is missing or unreadable.
ImFont* addFont(const std::string& file, float size) {
    if (file.empty()) {
        return nullptr;
    }
    ImFontConfig config;
    // A touch of horizontal oversampling keeps small text crisp on 96 dpi.
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = false;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(file.c_str(), size, &config);
}

}  // namespace

const Palette& palette() noexcept {
    return g_palette;
}

float scale() noexcept {
    return g_scale;
}

ImU32 u32(const ImVec4& colour, float alpha) noexcept {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x, colour.y, colour.z, colour.w * alpha));
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void apply(bool dark, float uiScale) noexcept {
    g_palette = dark ? darkPalette() : lightPalette();
    g_scale = std::clamp(uiScale, 0.5f, 4.0f);
    const Palette& p = g_palette;

    // ---- geometry: generous spacing, soft corners -----------------------------------
    ImGuiStyle style;  // fresh, so a scale change never compounds
    // Popups, tooltips and menus pad their content; the main window pushes
    // its own zero padding (Ui.cpp) and lays its regions out by hand.
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(11.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 14.0f;
    style.WindowRounding = 12.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.DisabledAlpha = 0.42f;
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    // ---- colours ----------------------------------------------------------------------
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.text3;
    c[ImGuiCol_WindowBg] = p.window;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = p.popup;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.field;
    c[ImGuiCol_FrameBgHovered] = p.fieldHover;
    c[ImGuiCol_FrameBgActive] = p.fieldHover;
    c[ImGuiCol_TitleBg] = p.window;
    c[ImGuiCol_TitleBgActive] = p.window;
    c[ImGuiCol_TitleBgCollapsed] = p.window;
    c[ImGuiCol_MenuBarBg] = p.window;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(p.text3.x, p.text3.y, p.text3.z, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(p.text3.x, p.text3.y, p.text3.z, 0.60f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(p.text3.x, p.text3.y, p.text3.z, 0.80f);
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentPress;
    c[ImGuiCol_Button] = p.field;
    c[ImGuiCol_ButtonHovered] = p.fieldHover;
    c[ImGuiCol_ButtonActive] = p.selection;
    c[ImGuiCol_Header] = p.selection;
    c[ImGuiCol_HeaderHovered] = p.fieldHover;
    c[ImGuiCol_HeaderActive] = p.selection;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, dark ? 0.55f : 0.30f);
    c[ImGuiCol_Tab] = p.field;
    c[ImGuiCol_TabHovered] = p.fieldHover;
    c[ImGuiCol_TabSelected] = p.surface;

    // ---- fonts: one base size, the DPI factor applied by ImGui ---------------------------
    style.FontSizeBase = kBodySize;
    style.ScaleAllSizes(g_scale);
    style.FontScaleDpi = g_scale;
    ImGui::GetStyle() = style;
}

const Fonts& fonts() noexcept {
    return g_fonts;
}

void loadFonts() noexcept {
    try {
        ImGuiIO& io = ImGui::GetIO();
        const fs::path dir = systemFontFolder();

#if defined(_WIN32)
        // Segoe UI Variable is Windows 11's UI face; Segoe UI everywhere else.
        const std::string ui = firstExisting({dir / L"SegUIVar.ttf", dir / L"segoeui.ttf"});
        const std::string bold = firstExisting({dir / L"seguisb.ttf", dir / L"segoeuib.ttf"});
        const std::string mono = firstExisting({dir / L"CascadiaMono.ttf", dir / L"consola.ttf", dir / L"cour.ttf"});
#elif defined(__APPLE__)
        // SF Pro (the system face), with Helvetica as the fallback.
        const std::string ui = firstExisting({dir / "SFNS.ttf", dir / "Helvetica.ttc"});
        const std::string bold = firstExisting({dir / "SFNS.ttf", dir / "Helvetica.ttc"});
        const std::string mono = firstExisting({dir / "SFNSMono.ttf", dir / "Menlo.ttc", dir / "Monaco.ttf"});
#else
        const std::string ui = firstExisting({dir / "truetype/dejavu/DejaVuSans.ttf"});
        const std::string bold = firstExisting({dir / "truetype/dejavu/DejaVuSans-Bold.ttf"});
        const std::string mono = firstExisting({dir / "truetype/dejavu/DejaVuSansMono.ttf"});
#endif
        // The first font added is ImGui's default.
        g_fonts.ui = addFont(ui, kBodySize);
        if (!g_fonts.ui) {
            g_fonts.ui = io.Fonts->AddFontDefault();
        }
        g_fonts.bold = addFont(bold, kBodySize);
        if (!g_fonts.bold) {
            g_fonts.bold = g_fonts.ui;
        }
        g_fonts.mono = addFont(mono, kMonoSize);
        if (!g_fonts.mono) {
            g_fonts.mono = g_fonts.ui;
        }
        io.FontDefault = g_fonts.ui;
    } catch (...) {
        // Whatever happened, leave ImGui with a font to draw with.
        if (!g_fonts.ui) {
            g_fonts.ui = ImGui::GetIO().Fonts->AddFontDefault();
            g_fonts.bold = g_fonts.ui;
            g_fonts.mono = g_fonts.ui;
        }
    }
}

}  // namespace osvgui::style
