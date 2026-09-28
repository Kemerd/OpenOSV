// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Icon.cpp - the procedural app icon (see Icon.h).
//
// Every pixel is supersampled on a regular grid: each sample decides
// whether it is inside the rounded square and on one of the globe's
// strokes, the samples are averaged in premultiplied alpha, and the result
// is converted back to straight alpha.  Distances to the ellipses use the
// first-order estimate f / |grad f| of the implicit form, which is exact on
// the curve and plenty for a few pixels either side of it.

#include "Icon.h"

#include <algorithm>
#include <cmath>

namespace osvgui {

namespace {

/// Straight-alpha colour in 0..1.
struct Rgba {
    double r = 0, g = 0, b = 0, a = 0;
};

/// Linear interpolation.
[[nodiscard]] double mix(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

/// Coverage 0..1 of a stroke of half width `halfWidth` at signed distance
/// `d` from its centre line, with a one-sample soft edge.
[[nodiscard]] double strokeCoverage(double d, double halfWidth, double soft) noexcept {
    return std::clamp((halfWidth - std::abs(d)) / soft + 0.5, 0.0, 1.0);
}

/// Signed distance estimate to the ellipse (x/rx)^2 + (y/ry)^2 = 1.
[[nodiscard]] double ellipseDistance(double x, double y, double rx, double ry) noexcept {
    const double f = (x * x) / (rx * rx) + (y * y) / (ry * ry) - 1.0;
    const double gx = 2.0 * x / (rx * rx);
    const double gy = 2.0 * y / (ry * ry);
    const double g = std::sqrt(gx * gx + gy * gy);
    return g > 1e-9 ? f / g : 1e9;
}

/// Porter-Duff "over" of straight-alpha `top` onto premultiplied `dst`.
void over(Rgba& dst, const Rgba& top) noexcept {
    dst.r = top.r * top.a + dst.r * (1.0 - top.a);
    dst.g = top.g * top.a + dst.g * (1.0 - top.a);
    dst.b = top.b * top.a + dst.b * (1.0 - top.a);
    dst.a = top.a + dst.a * (1.0 - top.a);
}

/// The icon's premultiplied colour at (u, v) in [0, 1]^2 (v down), drawn
/// for an icon `size` pixels wide (small sizes get thicker strokes).
[[nodiscard]] Rgba sample(double u, double v, int size) noexcept {
    Rgba out;  // transparent, premultiplied

    // ---- the rounded square: a superellipse, like a modern app tile -----------
    const double half = 0.44;  // half extent: a 6 % margin all round
    const double dx = (u - 0.5) / half;
    const double dy = (v - 0.5) / half;
    const double n = 5.0;
    const double shape = std::pow(std::abs(dx), n) + std::pow(std::abs(dy), n);
    if (shape > 1.0) {
        return out;
    }

    // ---- the tile: a diagonal blue -> indigo gradient, a soft top glow ----------
    const double t = std::clamp((u + v) * 0.5, 0.0, 1.0);
    Rgba tile;
    tile.r = mix(0.19, 0.36, t);
    tile.g = mix(0.55, 0.26, t);
    tile.b = mix(1.00, 0.90, t);
    tile.a = 1.0;
    const double glowDist = std::hypot(u - 0.36, v - 0.18);
    const double glow = std::clamp(1.0 - glowDist / 0.62, 0.0, 1.0) * 0.16;
    tile.r = mix(tile.r, 1.0, glow);
    tile.g = mix(tile.g, 1.0, glow);
    tile.b = mix(tile.b, 1.0, glow);
    over(out, tile);

    // ---- the globe: an outline, its equator and one meridian ----------------------
    const double x = u - 0.5;
    const double y = v - 0.5;
    const double radius = 0.255;
    // At least ~1.35 px wide, so the 16 px icon still reads as a globe.
    const double strokeHalf = std::max(0.021, 0.68 / static_cast<double>(size));
    const double soft = 1.0 / static_cast<double>(size);
    const double r = std::hypot(x, y);

    // Outline.
    Rgba white{1.0, 1.0, 1.0, 0.0};
    white.a = strokeCoverage(r - radius, strokeHalf, soft);
    over(out, white);

    // The inner curves, only inside the sphere and a little lighter.
    if (r < radius) {
        const double inner = 0.88;
        white.a = inner * strokeCoverage(ellipseDistance(x, y, radius, radius * 0.38), strokeHalf * 0.9, soft);
        over(out, white);
        white.a = inner * strokeCoverage(ellipseDistance(x, y, radius * 0.40, radius), strokeHalf * 0.9, soft);
        over(out, white);
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> renderAppIcon(int size) {
    size = std::clamp(size, 8, 1024);
    // More samples for the small sizes, where each pixel matters most.
    const int ss = size <= 64 ? 6 : 4;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u, 0);
    const double inv = 1.0 / static_cast<double>(size);
    const double step = 1.0 / static_cast<double>(ss);

    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            // ---- average the premultiplied samples ------------------------------
            Rgba acc;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const double u = (static_cast<double>(px) + (static_cast<double>(sx) + 0.5) * step) * inv;
                    const double v = (static_cast<double>(py) + (static_cast<double>(sy) + 0.5) * step) * inv;
                    const Rgba s = sample(u, v, size);
                    acc.r += s.r;
                    acc.g += s.g;
                    acc.b += s.b;
                    acc.a += s.a;
                }
            }
            const double n = static_cast<double>(ss * ss);
            acc.r /= n;
            acc.g /= n;
            acc.b /= n;
            acc.a /= n;

            // ---- back to straight alpha, 8 bits -----------------------------------
            const std::size_t o =
                (static_cast<std::size_t>(py) * static_cast<std::size_t>(size) + static_cast<std::size_t>(px)) * 4u;
            const auto byte = [](double c) {
                return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
            };
            if (acc.a > 1e-6) {
                rgba[o + 0] = byte(acc.r / acc.a);
                rgba[o + 1] = byte(acc.g / acc.a);
                rgba[o + 2] = byte(acc.b / acc.a);
            }
            rgba[o + 3] = byte(acc.a);
        }
    }
    return rgba;
}

}  // namespace osvgui
