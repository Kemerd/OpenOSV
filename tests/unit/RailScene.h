// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// RailScene.h - a synthetic near-field overlap band for the flow tests.
//
// The scene a car-body overlap band shows: long horizontal ridges (roof
// rails, panel edges - pure 1-D edges along the band's x), rivets along some
// of them and a sparse scatter of small spots, rendered into a band of the
// analysis size (2048 x 68) with Gaussian sensor noise.  Shifted along the
// band rows by 14 px it reproduces the disparity a car body a metre from the
// lenses shows on the car-mounted clips, which is what the 1-D epipolar
// search of DisFlow exists for.  Written for test_disflow.cpp and shared
// with the CPU / CUDA parity test (test_disflow_cuda.cpp).
#pragma once

#include "osv/render/DisFlow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace osv::testrail {

/// splitmix64 - a tiny PRNG whose sequence is fixed by its definition, unlike
/// std::normal_distribution, whose output differs between standard libraries
/// (the tests must see the same pixels on every platform).
struct SplitMix64 {
    std::uint64_t state = 0;

    std::uint64_t next() noexcept {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// Uniform in (0, 1]: never exactly 0, so log() below is always finite.
    double uniform() noexcept { return (static_cast<double>(next() >> 11) + 1.0) * (1.0 / 9007199254740992.0); }

    /// Standard normal by Box-Muller.
    double gaussian() noexcept {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
};

/// The scene in continuous band coordinates.
struct RailScene {
    /// One horizontal ridge across the whole width.
    struct Rail {
        double y;    ///< Centre row.
        double amp;  ///< Signed height, in [0, 1] intensity.
    };
    /// One small round spot (a rivet, dirt, a reflection).
    struct Spot {
        double x;
        double y;
        double amp;
    };
    std::vector<Rail> rails;
    std::vector<Spot> spots;
};

/// The car-roof scene for a band `w` wide, covering rows -8 .. 96 so both the
/// reference band and a copy shifted by up to 20 rows are fully defined.
///
/// The rails are spaced irregularly (no common period below the band height)
/// and alternate in sign, like the bright rails and dark gaps of a roof, so
/// no whole-pixel offset but the true one lines all of them up.
inline RailScene makeRailScene(std::uint32_t w) {
    RailScene scene;
    const double rows[] = {-3.0, 6.5, 15.0, 27.5, 33.0, 46.5, 58.0, 64.5, 77.0, 86.5, 93.0};
    const double amps[] = {0.22, -0.18, 0.25, -0.20, 0.15, -0.22, 0.20, -0.15, 0.22, -0.18, 0.16};
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        scene.rails.push_back({rows[i], amps[i]});
    }
    SplitMix64 rng{0x5EEDF00Dull};
    // Rivets along every third rail, 20-40 px apart: the only 2-D texture on
    // those rows, as on a real roof rail.
    for (std::size_t i = 0; i < scene.rails.size(); i += 3) {
        for (double x = 8.0 + 20.0 * rng.uniform(); x < static_cast<double>(w); x += 20.0 + 20.0 * rng.uniform()) {
            scene.spots.push_back({x, scene.rails[i].y + 2.5, 0.18});
        }
    }
    // A sparse scatter of small spots (dirt, reflections): one per ~400 px^2.
    const double area = static_cast<double>(w) * 104.0;
    const int count = static_cast<int>(area / 400.0);
    for (int i = 0; i < count; ++i) {
        const double x = rng.uniform() * static_cast<double>(w);
        const double y = -8.0 + 104.0 * rng.uniform();
        const double amp = (rng.uniform() < 0.5 ? -1.0 : 1.0) * (0.08 + 0.10 * rng.uniform());
        scene.spots.push_back({x, y, amp});
    }
    return scene;
}

/// A railing: identical bars every `period` rows and nothing else - the
/// repeated structure a 1-D search can lock onto one period off.
inline RailScene makeRailingScene(double period) {
    RailScene scene;
    if (!(period > 0.5)) {
        return scene;  // a nonsense period draws an empty (flat) scene
    }
    for (double y = -8.0; y < 100.0; y += period) {
        scene.rails.push_back({y, 0.2});
    }
    return scene;
}

/// Render rows [0, h) of `scene` displaced by `dy` rows - pixel (x, y) shows
/// scene point (x, y + dy), so content at row y of a dy = 0 render appears at
/// row y - dy here and the true flow from the dy = 0 render to this one is
/// (0, -dy) - plus Gaussian sensor noise of `noiseCodes` 8-bit codes from the
/// generator seeded with `seed`.  When `splitX` is positive, columns from
/// splitX on are rendered with displacement `dyRight` instead (a near object
/// beside a far one).
inline render::GrayImage renderRailScene(const RailScene& scene, std::uint32_t w, std::uint32_t h, double dy,
                                         double noiseCodes, std::uint64_t seed, std::uint32_t splitX = 0,
                                         double dyRight = 0.0) {
    render::GrayImage img;
    if (w == 0 || h == 0) {
        return img;  // an empty image, which every solver refuses
    }
    img.w = w;
    img.h = h;
    img.data.assign(static_cast<std::size_t>(w) * h, 0.0f);
    std::vector<double> acc(img.data.size(), 0.0);
    for (std::uint32_t x = 0; x < w; ++x) {
        const double shift = (splitX > 0 && x >= splitX) ? dyRight : dy;
        // Rails: a Gaussian ridge across the whole width (sigma 1.2 rows).
        for (std::uint32_t y = 0; y < h; ++y) {
            const double sy = static_cast<double>(y) + shift;
            double value = 0.40;
            for (const RailScene::Rail& r : scene.rails) {
                const double t = (sy - r.y) / 1.2;
                value += r.amp * std::exp(-0.5 * t * t);
            }
            acc[static_cast<std::size_t>(y) * w + x] = value;
        }
    }
    // Spots: splatted within 5 px of their centre (sigma 1.4 px), each with
    // the displacement of the column it is centred on.
    for (const RailScene::Spot& s : scene.spots) {
        const double shift = (splitX > 0 && s.x >= static_cast<double>(splitX)) ? dyRight : dy;
        const double cy = s.y - shift;
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - 5.0)));
        const int y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(std::ceil(cy + 5.0)));
        const int x0 = std::max(0, static_cast<int>(std::floor(s.x - 5.0)));
        const int x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(std::ceil(s.x + 5.0)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double ex = (static_cast<double>(x) - s.x) / 1.4;
                const double ey = (static_cast<double>(y) - cy) / 1.4;
                acc[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] +=
                    s.amp * std::exp(-0.5 * (ex * ex + ey * ey));
            }
        }
    }
    // Sensor noise, independent per image.
    SplitMix64 rng{seed};
    const double sigma = noiseCodes / 255.0;
    for (std::size_t i = 0; i < acc.size(); ++i) {
        img.data[i] = static_cast<float>(acc[i] + sigma * rng.gaussian());
    }
    return img;
}

/// Flow statistics over the pixels ON the rails (within 2 rows of a rail
/// centre) of rows [y0, y1) and columns [x0, x1) of the reference band.
struct RailStats {
    double meanU = 0.0;
    double meanV = 0.0;
    double consistent = 0.0;  ///< Fraction of those pixels the F-B check passed.
    std::uint64_t pixels = 0;
};

inline RailStats railStats(const RailScene& scene, const render::BidirFlow& f, int x0, int x1, int y0, int y1) {
    RailStats s;
    if (!f.valid()) {
        return s;
    }
    // Clamp the window to the field so a caller's typo reads nothing out of
    // bounds.
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(static_cast<int>(f.forward.w), x1);
    y1 = std::min(static_cast<int>(f.forward.h), y1);
    double su = 0.0;
    double sv = 0.0;
    std::uint64_t ok = 0;
    for (int y = y0; y < y1; ++y) {
        bool onRail = false;
        for (const RailScene::Rail& r : scene.rails) {
            onRail = onRail || std::fabs(static_cast<double>(y) - r.y) <= 2.0;
        }
        if (!onRail) {
            continue;
        }
        for (int x = x0; x < x1; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * f.forward.w + static_cast<std::size_t>(x);
            su += f.forward.u[idx];
            sv += f.forward.v[idx];
            ok += f.ok[idx] != 0 ? 1u : 0u;
            ++s.pixels;
        }
    }
    if (s.pixels > 0) {
        s.meanU = su / static_cast<double>(s.pixels);
        s.meanV = sv / static_cast<double>(s.pixels);
        s.consistent = static_cast<double>(ok) / static_cast<double>(s.pixels);
    }
    return s;
}

/// The 0.5.0 solver: every near-field addition switched off.
inline render::DisFlowParams legacyDisParams() {
    render::DisFlowParams p;
    p.tensorTikhonov = 0.0;
    p.epipolarSearchPx = 0.0;
    p.revertOnRunaway = false;
    return p;
}

}  // namespace osv::testrail
