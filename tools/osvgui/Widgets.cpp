// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Widgets.cpp - springs, icons and the custom controls (see Widgets.h).

#include "Widgets.h"

#include "Style.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace osvgui::ui {

namespace {

constexpr float kPi = 3.14159265358979f;

// ===========================================================================
//  Spring store
// ===========================================================================

struct SpringState {
    float value = 0.0f;
    float velocity = 0.0f;
    int frame = -1;  ///< The frame it was last advanced in.
};

std::unordered_map<ImGuiID, SpringState> g_springs;
bool g_animating = false;

/// ImVec2 arithmetic, kept local (ImGui's operators live in imgui_internal).
inline ImVec2 add(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

/// The palette, shortened.
inline const style::Palette& pal() {
    return style::palette();
}

/// The end of the visible part of an ImGui label ("Text##id" -> "Text").
const char* labelEnd(const char* label) {
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) {
        ++end;
    }
    return end;
}

/// Draw a switch at `pos`, `t` = 0 off .. 1 on (animated).
void drawSwitch(ImDrawList* dl, ImVec2 pos, float w, float h, float t, bool enabled) {
    const style::Palette& p = pal();
    const float alpha = enabled ? 1.0f : 0.45f;
    // Track: grey to accent.
    dl->AddRectFilled(pos, add(pos, ImVec2(w, h)), style::u32(style::mix(p.trackOff, p.accent, t), alpha), h * 0.5f);
    // Knob with a soft shadow, sliding across.
    const float r = h * 0.5f - px(2.0f);
    const float cx = pos.x + h * 0.5f + std::clamp(t, -0.1f, 1.1f) * (w - h);
    const float cy = pos.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, cy + px(1.0f)), r + px(0.6f), style::u32(p.shadow, 0.55f * alpha), 32);
    dl->AddCircleFilled(ImVec2(cx, cy), r, style::u32(p.knob, alpha), 32);
}

}  // namespace

float px(float points) noexcept {
    return points * style::scale();
}

// ===========================================================================
//  Springs
// ===========================================================================

void beginFrame() noexcept {
    g_animating = false;
    // Forget springs nobody asked about for ten seconds' worth of frames,
    // so the store stays the size of what is on screen.
    const int frame = ImGui::GetFrameCount();
    if (frame % 600 == 0) {
        for (auto it = g_springs.begin(); it != g_springs.end();) {
            it = (frame - it->second.frame > 600) ? g_springs.erase(it) : std::next(it);
        }
    }
}

float spring(ImGuiID id, float target, float stiffness, float dampingRatio) noexcept {
    try {
        const int frame = ImGui::GetFrameCount();
        auto [it, inserted] = g_springs.try_emplace(id);
        SpringState& s = it->second;
        if (inserted) {
            s.value = target;
            s.velocity = 0.0f;
            s.frame = frame;
            return target;
        }
        if (s.frame != frame) {
            // Semi-implicit Euler in 240 Hz sub-steps: stable for any frame
            // time, and a long pause (the loop sleeps when idle) is clamped
            // so a value never teleports.
            const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 20.0f);
            const float damping = 2.0f * dampingRatio * std::sqrt(stiffness);
            const int steps = std::max(1, static_cast<int>(std::ceil(dt * 240.0f)));
            const float h = dt / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                const float accel = -stiffness * (s.value - target) - damping * s.velocity;
                s.velocity += accel * h;
                s.value += s.velocity * h;
            }
            s.frame = frame;
        }
        // At rest when both the distance and the speed are negligible.
        const float eps = 0.0015f * std::max(1.0f, std::fabs(target));
        if (std::fabs(s.value - target) < eps && std::fabs(s.velocity) < eps * 20.0f) {
            s.value = target;
            s.velocity = 0.0f;
        } else {
            g_animating = true;
        }
        return s.value;
    } catch (...) {
        return target;
    }
}

bool animating() noexcept {
    return g_animating;
}

void keepAnimating() noexcept {
    g_animating = true;
}

// ===========================================================================
//  Icons
// ===========================================================================

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col, ImU32 background) noexcept {
    if (!dl || size <= 0.0f || icon == Icon::None) {
        return;
    }
    const float s = size;
    const float t = std::max(1.2f, s * 0.095f);  // stroke width
    const auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };

    switch (icon) {
    case Icon::Plus:
        dl->AddLine(P(-0.36f, 0.0f), P(0.36f, 0.0f), col, t);
        dl->AddLine(P(0.0f, -0.36f), P(0.0f, 0.36f), col, t);
        break;
    case Icon::Close:
        dl->AddLine(P(-0.27f, -0.27f), P(0.27f, 0.27f), col, t);
        dl->AddLine(P(0.27f, -0.27f), P(-0.27f, 0.27f), col, t);
        break;
    case Icon::Check: {
        const ImVec2 pts[3] = {P(-0.33f, 0.02f), P(-0.10f, 0.25f), P(0.34f, -0.25f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t * 1.15f);
        break;
    }
    case Icon::Play:
        // Nudged right: a triangle's visual centre is left of its box centre.
        dl->AddTriangleFilled(P(-0.22f, -0.34f), P(0.38f, 0.0f), P(-0.22f, 0.34f), col);
        break;
    case Icon::Pause:
        dl->AddRectFilled(P(-0.30f, -0.34f), P(-0.08f, 0.34f), col, s * 0.06f);
        dl->AddRectFilled(P(0.08f, -0.34f), P(0.30f, 0.34f), col, s * 0.06f);
        break;
    case Icon::Stop:
        dl->AddRectFilled(P(-0.29f, -0.29f), P(0.29f, 0.29f), col, s * 0.09f);
        break;
    case Icon::Folder: {
        dl->PathClear();
        dl->PathLineTo(P(-0.42f, 0.32f));
        dl->PathLineTo(P(-0.42f, -0.30f));
        dl->PathLineTo(P(-0.12f, -0.30f));
        dl->PathLineTo(P(-0.03f, -0.19f));
        dl->PathLineTo(P(0.42f, -0.19f));
        dl->PathLineTo(P(0.42f, 0.32f));
        dl->PathStroke(col, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Alert:
        dl->AddLine(P(0.0f, -0.30f), P(0.0f, 0.08f), col, t * 1.2f);
        dl->AddCircleFilled(P(0.0f, 0.28f), t * 0.75f, col, 12);
        break;
    case Icon::Copy:
        dl->AddRect(P(-0.14f, -0.40f), P(0.40f, 0.14f), col, s * 0.08f, 0, t);
        if (background) {
            dl->AddRectFilled(P(-0.40f, -0.14f), P(0.14f, 0.40f), background, s * 0.08f);
        }
        dl->AddRect(P(-0.40f, -0.14f), P(0.14f, 0.40f), col, s * 0.08f, 0, t);
        break;
    case Icon::ChevronDown: {
        const ImVec2 pts[3] = {P(-0.28f, -0.12f), P(0.0f, 0.16f), P(0.28f, -0.12f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::ChevronRight: {
        const ImVec2 pts[3] = {P(-0.12f, -0.28f), P(0.16f, 0.0f), P(-0.12f, 0.28f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::Sun:
        dl->AddCircle(c, s * 0.19f, col, 24, t);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            const ImVec2 d(std::cos(a), std::sin(a));
            dl->AddLine(ImVec2(c.x + d.x * s * 0.31f, c.y + d.y * s * 0.31f),
                        ImVec2(c.x + d.x * s * 0.43f, c.y + d.y * s * 0.43f), col, t);
        }
        break;
    case Icon::Moon:
        dl->AddCircleFilled(c, s * 0.34f, col, 32);
        if (background) {
            dl->AddCircleFilled(P(0.17f, -0.15f), s * 0.29f, background, 32);
        }
        break;
    case Icon::Terminal: {
        dl->AddRect(P(-0.43f, -0.34f), P(0.43f, 0.34f), col, s * 0.10f, 0, t);
        const ImVec2 pts[3] = {P(-0.24f, -0.12f), P(-0.08f, 0.02f), P(-0.24f, 0.16f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
        dl->AddLine(P(0.02f, 0.17f), P(0.23f, 0.17f), col, t);
        break;
    }
    case Icon::Tray: {
        const ImVec2 tray[4] = {P(-0.42f, 0.06f), P(-0.42f, 0.38f), P(0.42f, 0.38f), P(0.42f, 0.06f)};
        dl->AddPolyline(tray, 4, col, ImDrawFlags_None, t);
        dl->AddLine(P(0.0f, -0.42f), P(0.0f, 0.14f), col, t);
        const ImVec2 head[3] = {P(-0.17f, -0.04f), P(0.0f, 0.14f), P(0.17f, -0.04f)};
        dl->AddPolyline(head, 3, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::Reveal: {
        dl->AddLine(P(-0.26f, 0.26f), P(0.30f, -0.30f), col, t);
        const ImVec2 head[3] = {P(-0.05f, -0.30f), P(0.30f, -0.30f), P(0.30f, 0.05f)};
        dl->AddPolyline(head, 3, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::Retry: {
        const float r = s * 0.31f;
        const float a0 = -kPi * 0.30f;
        const float a1 = kPi * 1.30f;
        dl->PathClear();
        dl->PathArcTo(c, r, a0, a1, 28);
        dl->PathStroke(col, ImDrawFlags_None, t);
        // Arrowhead at the start of the arc, pointing along it.
        const ImVec2 tip(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r);
        dl->AddTriangleFilled(ImVec2(tip.x - s * 0.17f, tip.y - s * 0.02f),
                              ImVec2(tip.x + s * 0.06f, tip.y - s * 0.20f),
                              ImVec2(tip.x + s * 0.07f, tip.y + s * 0.09f), col);
        break;
    }
    case Icon::Trash: {
        dl->AddLine(P(-0.36f, -0.25f), P(0.36f, -0.25f), col, t);
        const ImVec2 handle[4] = {P(-0.10f, -0.25f), P(-0.10f, -0.37f), P(0.10f, -0.37f), P(0.10f, -0.25f)};
        dl->AddPolyline(handle, 4, col, ImDrawFlags_None, t);
        const ImVec2 body[4] = {P(-0.26f, -0.25f), P(-0.20f, 0.38f), P(0.20f, 0.38f), P(0.26f, -0.25f)};
        dl->AddPolyline(body, 4, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::Skip: {
        const ImVec2 arrow[3] = {P(-0.28f, -0.30f), P(0.10f, 0.0f), P(-0.28f, 0.30f)};
        dl->AddPolyline(arrow, 3, col, ImDrawFlags_None, t);
        dl->AddLine(P(0.28f, -0.30f), P(0.28f, 0.30f), col, t);
        break;
    }
    case Icon::Sparkle: {
        const ImVec2 star[8] = {P(0.0f, -0.42f), P(0.09f, -0.09f), P(0.42f, 0.0f),  P(0.09f, 0.09f),
                                P(0.0f, 0.42f),  P(-0.09f, 0.09f), P(-0.42f, 0.0f), P(-0.09f, -0.09f)};
        dl->AddConcavePolyFilled(star, 8, col);
        break;
    }
    case Icon::None:
        break;
    }
}

// ===========================================================================
//  Controls
// ===========================================================================

bool button(const char* label, Icon icon, ButtonKind kind, float width, float height, bool enabled) {
    if (!label) {
        return false;
    }
    const style::Palette& p = pal();
    const char* textEnd = labelEnd(label);
    const bool hasText = textEnd != label;
    const ImVec2 textSize = hasText ? ImGui::CalcTextSize(label, textEnd) : ImVec2(0, 0);
    const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
    const float iconSize = std::round(ImGui::GetFontSize() * 0.92f);
    const float gap = (icon != Icon::None && hasText) ? px(7.0f) : 0.0f;
    const float contentW = (icon != Icon::None ? iconSize : 0.0f) + gap + textSize.x;
    const float padX = kind == ButtonKind::Plain ? px(9.0f) : px(15.0f);
    const float w = width > 0.0f ? width : contentW + 2.0f * padX;

    // ---- interaction ---------------------------------------------------------------
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const ImGuiID id = ImGui::GetItemID();
    const float hover = spring(id, hovered && enabled ? 1.0f : 0.0f, 420.0f, 1.0f);
    const float press = spring(id ^ 0x5bd1e995u, held && enabled ? 1.0f : 0.0f, 700.0f, 1.0f);

    // ---- colours by kind --------------------------------------------------------------
    ImVec4 fill, fillHover, fillPress, ink;
    switch (kind) {
    case ButtonKind::Primary:
        fill = p.accent;
        fillHover = p.accentHover;
        fillPress = p.accentPress;
        ink = p.onAccent;
        break;
    case ButtonKind::Destructive:
        fill = ImVec4(p.danger.x, p.danger.y, p.danger.z, 0.14f);
        fillHover = ImVec4(p.danger.x, p.danger.y, p.danger.z, 0.24f);
        fillPress = ImVec4(p.danger.x, p.danger.y, p.danger.z, 0.32f);
        ink = p.danger;
        break;
    case ButtonKind::Plain:
        fill = ImVec4(p.field.x, p.field.y, p.field.z, 0.0f);
        fillHover = p.field;
        fillPress = p.fieldHover;
        ink = style::mix(p.text2, p.text, hover);
        break;
    case ButtonKind::Secondary:
    default:
        fill = p.field;
        fillHover = p.fieldHover;
        fillPress = style::mix(p.fieldHover, p.text3, 0.25f);
        ink = p.text;
        break;
    }
    ImVec4 colour = style::mix(style::mix(fill, fillHover, hover), fillPress, press);
    const float alpha = enabled ? 1.0f : 0.42f;

    // ---- draw: a press sinks the button by a pixel all round -------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inset = press * px(1.0f);
    const float rounding = std::min(h * 0.5f, px(9.0f));
    const ImVec2 a(pos.x + inset, pos.y + inset);
    const ImVec2 b(pos.x + w - inset, pos.y + h - inset);
    if (colour.w > 0.001f) {
        dl->AddRectFilled(a, b, style::u32(colour, alpha), rounding);
    }
    const ImU32 inkU32 = style::u32(ink, alpha);
    float x = pos.x + (w - contentW) * 0.5f;
    const float cy = pos.y + h * 0.5f;
    if (icon != Icon::None) {
        // What shows through the button (for the moon's cut-out): its fill
        // over the window colour.
        const ImVec4 under = style::mix(p.window, ImVec4(colour.x, colour.y, colour.z, 1.0f), colour.w);
        drawIcon(dl, icon, ImVec2(x + iconSize * 0.5f, cy), iconSize, inkU32, style::u32(under, 1.0f));
        x += iconSize + gap;
    }
    if (hasText) {
        dl->AddText(ImVec2(std::round(x), std::round(cy - textSize.y * 0.5f)), inkU32, label, textEnd);
    }
    return pressed && enabled;
}

float buttonWidth(const char* label, Icon icon, ButtonKind kind) {
    if (!label) {
        return 0.0f;
    }
    // The same arithmetic as button(), without drawing.
    const char* textEnd = labelEnd(label);
    const bool hasText = textEnd != label;
    const float textW = hasText ? ImGui::CalcTextSize(label, textEnd).x : 0.0f;
    const float iconSize = std::round(ImGui::GetFontSize() * 0.92f);
    const float gap = (icon != Icon::None && hasText) ? px(7.0f) : 0.0f;
    const float padX = kind == ButtonKind::Plain ? px(9.0f) : px(15.0f);
    return (icon != Icon::None ? iconSize : 0.0f) + gap + textW + 2.0f * padX;
}

bool iconButton(const char* id, Icon icon, const char* tip, bool enabled, float size) {
    const float s = size > 0.0f ? size : ImGui::GetFrameHeight();
    const bool clicked = button(id, icon, ButtonKind::Plain, s, s, enabled);
    if (tip && *tip) {
        tooltip(tip);
    }
    return clicked;
}

bool toggleSwitch(const char* id, bool* value, bool enabled) {
    if (!value) {
        return false;
    }
    const float h = std::round(ImGui::GetFrameHeight() * 0.80f);
    const float w = std::round(h * 1.66f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImGui::EndDisabled();
    bool changed = false;
    if (clicked && enabled) {
        *value = !*value;
        changed = true;
    }
    const float t = spring(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 380.0f, 0.78f);
    drawSwitch(ImGui::GetWindowDrawList(), pos, w, h, t, enabled);
    return changed;
}

bool toggleRow(const char* label, bool* value, bool enabled) {
    if (!label || !value) {
        return false;
    }
    const style::Palette& p = pal();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float switchH = std::round(ImGui::GetFrameHeight() * 0.80f);
    const float switchW = std::round(switchH * 1.66f);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // The whole row is the hit target, as on macOS and iOS.
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(std::max(avail, switchW), h));
    ImGui::EndDisabled();
    bool changed = false;
    if (clicked && enabled) {
        *value = !*value;
        changed = true;
    }
    const float t = spring(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 380.0f, 0.78f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* end = labelEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, end);
    const std::string shown = ellipsize(std::string_view(label, static_cast<std::size_t>(end - label)),
                                        std::max(0.0f, avail - switchW - px(12.0f)), false);
    dl->AddText(ImVec2(pos.x, std::round(pos.y + (h - textSize.y) * 0.5f)), style::u32(enabled ? p.text : p.text3),
                shown.c_str());
    drawSwitch(dl, ImVec2(pos.x + std::max(avail, switchW) - switchW, pos.y + (h - switchH) * 0.5f), switchW, switchH,
               t, enabled);
    return changed;
}

bool slider(const char* id, float* value, float min, float max, float resetTo, float width,
            std::string (*formatValue)(float), bool enabled) {
    if (!id || !value || !(max > min) || width <= 0.0f) {
        return false;
    }
    const style::Palette& p = pal();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // ---- the value's label on the right, sized for the widest value --------------------
    const std::string widest =
        formatValue ? std::max(formatValue(min), formatValue(max),
                               [](const std::string& a, const std::string& b) { return a.size() < b.size(); })
                    : std::string("0000");
    const float labelW = std::min(width * 0.45f, ImGui::CalcTextSize(widest.c_str()).x + px(14.0f));
    const float trackW = std::max(px(40.0f), width - labelW);
    const float knobR = std::round(h * 0.34f);
    const float x0 = pos.x + knobR;
    const float x1 = pos.x + trackW - knobR;

    // ---- interaction: press or drag anywhere on the track; double-click resets --------
    ImGui::BeginDisabled(!enabled);
    ImGui::InvisibleButton(id, ImVec2(trackW, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const ImGuiID gid = ImGui::GetItemID();
    bool changed = false;
    float v = std::clamp(*value, min, max);
    if (enabled && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        v = std::clamp(resetTo, min, max);
        changed = true;
    } else if (enabled && active && x1 > x0) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        const float next = min + t * (max - min);
        if (next != v) {
            v = next;
            changed = true;
        }
    }
    if (changed) {
        *value = v;
    }

    // ---- drawing ----------------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.0f : 0.45f;
    const float cy = pos.y + h * 0.5f;
    const float thickness = px(4.0f);
    dl->AddRectFilled(ImVec2(x0 - thickness * 0.5f, cy - thickness * 0.5f),
                      ImVec2(x1 + thickness * 0.5f, cy + thickness * 0.5f), style::u32(p.trackOff, alpha),
                      thickness * 0.5f);
    // The knob glides to a clicked value rather than jumping (a drag follows
    // the pointer closely: the spring is stiff).
    const float t = (v - min) / (max - min);
    const float shown = spring(gid, t, active ? 2400.0f : 420.0f, 1.0f);
    const float kx = x0 + std::clamp(shown, 0.0f, 1.0f) * (x1 - x0);
    // Fill: from the zero point when the range spans it, else from the start.
    const float zeroT = (min < 0.0f && max > 0.0f) ? (-min) / (max - min) : 0.0f;
    const float zx = x0 + zeroT * (x1 - x0);
    const float fa = std::min(zx, kx);
    const float fb = std::max(zx, kx);
    if (fb - fa > 0.5f) {
        dl->AddRectFilled(ImVec2(fa, cy - thickness * 0.5f), ImVec2(fb, cy + thickness * 0.5f),
                          style::u32(p.accent, alpha), thickness * 0.5f);
    }
    const float grow = spring(gid ^ 0x7F4A7C15u, (hovered || active) && enabled ? 1.0f : 0.0f, 420.0f, 1.0f);
    const float r = knobR * (0.86f + 0.14f * grow);
    dl->AddCircleFilled(ImVec2(kx, cy + px(1.0f)), r + px(0.6f), style::u32(p.shadow, 0.5f * alpha), 32);
    dl->AddCircleFilled(ImVec2(kx, cy), r, style::u32(p.knob, alpha), 32);
    if (!p.dark) {
        dl->AddCircle(ImVec2(kx, cy), r, style::u32(p.border, 2.0f * alpha), 32, 1.0f);
    }

    // The label, right-aligned in its column.
    const std::string text = formatValue ? formatValue(v) : std::string();
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    dl->AddText(ImVec2(pos.x + width - size.x, cy - size.y * 0.5f), style::u32(enabled ? p.text2 : p.text3),
                text.c_str());

    // Reserve the label's space.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(std::max(0.0f, width - trackW), h));
    return changed;
}

bool segmented(const char* id, const char* const* labels, int count, int* index, float width, bool enabled) {
    if (!id || !labels || !index || count <= 0 || width <= 0.0f) {
        return false;
    }
    const style::Palette& p = pal();
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float segW = width / static_cast<float>(count);
    const int current = std::clamp(*index, 0, count - 1);
    const float alpha = enabled ? 1.0f : 0.45f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- the track and the sliding selection ---------------------------------------------
    const float rounding = px(9.0f);
    dl->AddRectFilled(pos, add(pos, ImVec2(width, h)), style::u32(p.field, alpha), rounding);
    const float x = spring(ImGui::GetID("##pill"), static_cast<float>(current) * segW, 420.0f, 0.86f);
    const float inset = px(2.5f);
    const ImVec2 a(pos.x + x + inset, pos.y + inset);
    const ImVec2 b(pos.x + x + segW - inset, pos.y + h - inset);
    const ImVec4 pill = p.dark ? style::mix(p.fieldHover, p.text, 0.10f) : p.surface;
    dl->AddRectFilled(ImVec2(a.x, a.y + px(1.0f)), ImVec2(b.x, b.y + px(1.0f)), style::u32(p.shadow, 0.35f * alpha),
                      rounding - inset);
    dl->AddRectFilled(a, b, style::u32(pill, alpha), rounding - inset);

    // ---- segments: hit targets and labels ------------------------------------------------------
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + static_cast<float>(i) * segW, pos.y));
        ImGui::PushID(i);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::InvisibleButton("##segment", ImVec2(segW, h)) && enabled && i != current) {
            *index = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        ImGui::PopID();

        const char* text = labels[i] ? labels[i] : "";
        const std::string shown = ellipsize(text, segW - px(12.0f), false);
        const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
        const ImVec4 ink = (i == current || hovered) ? p.text : p.text2;
        dl->AddText(ImVec2(std::round(pos.x + static_cast<float>(i) * segW + (segW - size.x) * 0.5f),
                           std::round(pos.y + (h - size.y) * 0.5f)),
                    style::u32(ink, alpha), shown.c_str());
    }
    // Reserve the control's space for the layout.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
    return changed;
}

bool segmentedChoice(const char* id, std::span<const Choice> choices, std::string& token, float width, bool enabled) {
    if (choices.empty()) {
        return false;
    }
    std::vector<const char*> labels;
    labels.reserve(choices.size());
    for (const Choice& c : choices) {
        labels.push_back(c.label);
    }
    int index = std::max(0, choiceIndex(choices, token));
    if (segmented(id, labels.data(), static_cast<int>(labels.size()), &index, width, enabled)) {
        token = choices[static_cast<std::size_t>(index)].token;
        return true;
    }
    return false;
}

bool beginCombo(const char* id, const char* preview, float width, bool enabled) {
    // The frame's place, taken before BeginCombo opens its popup (after
    // which the "last item" is the popup, not the frame).
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetNextItemWidth(width);
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
    // A small chevron at the right edge, like a macOS pop-up button.
    const float s = std::round(h * 0.40f);
    const style::Palette& p = pal();
    drawIcon(dl, Icon::ChevronDown, ImVec2(pos.x + width - s * 0.5f - px(10.0f), pos.y + h * 0.5f + px(1.0f)), s,
             style::u32(enabled ? p.text2 : p.text3));
    return open;
}

bool comboChoice(const char* id, std::span<const Choice> choices, std::string& token, float width, bool enabled) {
    if (choices.empty()) {
        return false;
    }
    bool changed = false;
    ImGui::BeginDisabled(!enabled);
    if (beginCombo(id, choiceLabel(choices, token), width, enabled)) {
        for (const Choice& c : choices) {
            const bool selected = token == c.token;
            if (ImGui::Selectable(c.label, selected)) {
                if (!selected) {
                    token = c.token;
                    changed = true;
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    return changed;
}

void progressBar(const char* id, float fraction, float width, float height, ImU32 fill, bool indeterminate) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImGuiID gid = ImGui::GetID(id);
    ImGui::Dummy(ImVec2(width, height));
    drawProgress(ImGui::GetWindowDrawList(), pos, ImVec2(width, height), gid, fraction, fill, indeterminate);
}

void drawProgress(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImGuiID id, float fraction, ImU32 fill,
                  bool indeterminate) noexcept {
    if (!dl || size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const float r = size.y * 0.5f;
    dl->AddRectFilled(pos, add(pos, size), style::u32(pal().trackOff), r);
    if (indeterminate) {
        // A sheen sweeping left to right: work has started, the amount is
        // not known yet.
        keepAnimating();
        const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.75, 1.0));
        const float seg = size.x * 0.30f;
        const float x = -seg + t * (size.x + seg);
        dl->PushClipRect(pos, add(pos, size), true);
        dl->AddRectFilled(ImVec2(pos.x + x, pos.y), ImVec2(pos.x + x + seg, pos.y + size.y), fill, r);
        dl->PopClipRect();
        return;
    }
    // A soft spring: progress arrives in steps of ten frames, and the fill
    // glides between them instead of jumping.
    const float f = std::clamp(spring(id, std::clamp(fraction, 0.0f, 1.0f), 90.0f, 1.0f), 0.0f, 1.0f);
    if (f > 0.0005f) {
        const float w = std::max(size.y, f * size.x);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + size.y), fill, r);
    }
}

void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness, float dash,
                float gap) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y || dash <= 0.0f) {
        return;
    }
    const float r = std::min(rounding, std::min(b.x - a.x, b.y - a.y) * 0.5f);
    // Corners: solid quarter arcs.
    const ImVec2 corners[4] = {ImVec2(a.x + r, a.y + r), ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r),
                               ImVec2(a.x + r, b.y - r)};
    const float starts[4] = {kPi, kPi * 1.5f, 0.0f, kPi * 0.5f};
    for (int i = 0; i < 4; ++i) {
        dl->PathClear();
        dl->PathArcTo(corners[i], r, starts[i], starts[i] + kPi * 0.5f, 8);
        dl->PathStroke(colour, ImDrawFlags_None, thickness);
    }
    // Edges: dashes, centred so both ends look the same.
    const auto edge = [&](ImVec2 from, ImVec2 to) {
        const float len = std::hypot(to.x - from.x, to.y - from.y);
        if (len <= 0.0f) {
            return;
        }
        const ImVec2 dir((to.x - from.x) / len, (to.y - from.y) / len);
        const float period = dash + gap;
        const int n = std::max(1, static_cast<int>((len + gap) / period));
        const float used = static_cast<float>(n) * period - gap;
        float t = (len - used) * 0.5f;
        for (int i = 0; i < n; ++i, t += period) {
            const float t1 = std::min(len, t + dash);
            dl->AddLine(ImVec2(from.x + dir.x * t, from.y + dir.y * t),
                        ImVec2(from.x + dir.x * t1, from.y + dir.y * t1), colour, thickness);
        }
    };
    edge(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y));
    edge(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
    edge(ImVec2(b.x - r, b.y), ImVec2(a.x + r, b.y));
    edge(ImVec2(a.x, b.y - r), ImVec2(a.x, a.y + r));
}

void spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 colour, float thickness) noexcept {
    if (!dl || radius <= 0.0f) {
        return;
    }
    keepAnimating();
    const float a0 = static_cast<float>(std::fmod(ImGui::GetTime() * 5.5, 2.0 * kPi));
    dl->PathClear();
    dl->PathArcTo(center, radius, a0, a0 + kPi * 1.35f, 24);
    dl->PathStroke(colour, ImDrawFlags_None, thickness);
}

// ===========================================================================
//  Layout
// ===========================================================================

void sectionHeader(const char* text) {
    if (!text) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(4.0f));
    ImGui::PushFont(style::fonts().bold, style::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, pal().text2);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void beginCard(const char* id) {
    const style::Palette& p = pal();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(14.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10.0f), px(12.0f)));
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
}

void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

float rowLabel(const char* label, bool enabled) {
    const style::Palette& p = pal();
    const float avail = ImGui::GetContentRegionAvail().x;
    // Narrow card: the label sits above its control.
    if (avail < px(300.0f)) {
        ImGui::PushStyleColor(ImGuiCol_Text, enabled ? p.text2 : p.text3);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        return avail;
    }
    const float labelW = std::clamp(avail * 0.40f, px(110.0f), px(180.0f));
    const float startX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? p.text : p.text3);
    const std::string shown = ellipsize(label, labelW - px(8.0f), false);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(startX + labelW);
    return avail - labelW;
}

void caption(const char* text) {
    if (!text || !*text) {
        return;
    }
    ImGui::PushFont(nullptr, style::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, pal().text2);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void hairline() {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + w, pos.y), style::u32(pal().border), 1.0f);
    ImGui::Dummy(ImVec2(w, 1.0f));
}

std::string ellipsize(std::string_view text, float maxWidth, bool middle) {
    try {
        const char* begin = text.data();
        const char* end = text.data() + text.size();
        if (text.empty() || ImGui::CalcTextSize(begin, end).x <= maxWidth) {
            return std::string(text);
        }
        static constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026
        const float ellW = ImGui::CalcTextSize(kEllipsis).x;
        if (maxWidth <= ellW) {
            return kEllipsis;
        }
        // Character starts, so a cut never splits a UTF-8 sequence.
        std::vector<std::size_t> starts;
        starts.reserve(text.size() + 1);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) {
                starts.push_back(i);
            }
        }
        starts.push_back(text.size());
        const std::size_t chars = starts.size() - 1;
        const auto width = [&](std::size_t from, std::size_t to) {
            return ImGui::CalcTextSize(begin + starts[from], begin + starts[to]).x;
        };
        // Binary search the number of characters kept.
        std::size_t lo = 0;
        std::size_t hi = chars;
        std::string best = kEllipsis;
        while (lo <= hi) {
            const std::size_t keep = (lo + hi) / 2;
            std::size_t head = keep;
            std::size_t tail = 0;
            if (middle) {
                // Paths: keep more of the end, where the file name is.
                tail = std::min(chars, (keep * 3) / 5);
                head = keep - tail;
            }
            const float w = width(0, head) + ellW + (tail > 0 ? width(chars - tail, chars) : 0.0f);
            if (w <= maxWidth) {
                best = std::string(text.substr(0, starts[head])) + kEllipsis +
                       (tail > 0 ? std::string(text.substr(starts[chars - tail])) : std::string());
                lo = keep + 1;
            } else {
                if (keep == 0) {
                    break;
                }
                hi = keep - 1;
            }
        }
        return best;
    } catch (...) {
        return std::string(text);
    }
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

}  // namespace osvgui::ui
