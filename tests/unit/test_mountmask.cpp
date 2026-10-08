// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Hide Mount Auto (osv/render/MountMask.h): the per-window correlation on
// synthetic bands, the release rule, the dilation, the choice of the lens
// that does not image the mount, and the cache text form.  CPU only; the
// geometry half (rebuilding a rig's polygons) is in test_geom.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "osv/core/Math.h"
#include "osv/render/MountMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::render;

namespace {

// =============================================================================
//  Synthetic bands
// =============================================================================

/// Bands as measureMountMask renders them: the polar-axis ring of `params`
/// columns, rows |phi| <= rowsHalfDeg + searchAlongDeg, both lenses fully
/// covered.  Luma is filled by `fill(lens, row, column)`.
template <class Fill>
LensBands makeBands(const MountMaskParams& params, Fill fill) {
    LensBands b;
    b.w = params.equirectW;
    b.mapH = params.equirectW / 2;
    const double rowsPerDeg = static_cast<double>(b.mapH) / 180.0;
    const auto halfRows =
        static_cast<std::uint32_t>(std::lround((params.rowsHalfDeg + params.searchAlongDeg) * rowsPerDeg));
    b.rowOffset = b.mapH / 2 - halfRows;
    b.h = 2 * halfRows;
    for (int lens = 0; lens < 2; ++lens) {
        b.luma[lens].assign(static_cast<std::size_t>(b.w) * b.h, 0.0f);
        b.alpha[lens].assign(b.luma[lens].size(), 1.0f);
        for (std::uint32_t r = 0; r < b.h; ++r) {
            for (std::uint32_t c = 0; c < b.w; ++c) {
                b.luma[lens][static_cast<std::size_t>(r) * b.w + c] = fill(lens, static_cast<int>(r),
                                                                           static_cast<int>(c));
            }
        }
    }
    return b;
}

/// A deterministic texture: smooth noise of amplitude ~0.1 around 0.5.
float texture(int r, int c, std::uint32_t seed) {
    const auto hash = [&](int y, int x) {
        std::uint32_t h = static_cast<std::uint32_t>(y) * 73856093u ^ static_cast<std::uint32_t>(x) * 19349663u ^
                          seed * 83492791u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;
    };
    // 2 x 2 box of hashed cells: texture with structure at every scale the
    // window sees, never flat.
    return 0.4f + 0.1f * (hash(r / 2, c / 2) + hash(r / 3 + 7, c / 3 + 11));
}

/// An arc covering columns [first, first + count) of `params`'s ring.
MountArc makeArc(const MountMaskParams& params, std::uint32_t first, std::uint32_t count) {
    MountArc arc;
    arc.columns = params.equirectW;
    arc.inArc.assign(params.equirectW, 0);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t c = (first + k) % params.equirectW;
        arc.inArc[c] = 1;
        ++arc.arcColumns;
    }
    return arc;
}

/// The windows of an arc (every window with an arc column), as measureMountMask lists them.
std::vector<std::uint32_t> windowsOf(const MountArc& arc, const MountMaskParams& params) {
    std::vector<std::uint32_t> starts;
    for (std::uint32_t c0 = 0; c0 < params.equirectW; c0 += params.windowCols) {
        for (std::uint32_t c = 0; c < params.windowCols; ++c) {
            if (arc.inArc[c0 + c]) {
                starts.push_back(c0);
                break;
            }
        }
    }
    return starts;
}

/// One frame of scores, every window `ncc` with the given sigmas and halves.
std::vector<MountWindowScore> frameOf(std::size_t windows, float ncc, float sigma, float upper, float lower) {
    MountWindowScore s;
    s.ncc = ncc;
    s.upper = upper;
    s.lower = lower;
    s.sigma0 = sigma;
    s.sigma1 = sigma;
    return std::vector<MountWindowScore>(windows, s);
}

}  // namespace

// =============================================================================
//  Parameters and the cache's text form
// =============================================================================

TEST_CASE("Hide Mount Auto: the shipped rule's parameters are valid and garbage is not", "[mountmask]") {
    const MountMaskParams shipped;
    REQUIRE(validateMountMaskParams(shipped).ok());
    // The numbers the rule is defined by.
    CHECK(shipped.windowCols == 16u);
    CHECK(shipped.releaseNcc == 0.8);
    CHECK(shipped.flatSigma == 0.01);
    CHECK(shipped.dilateCols == 12u);
    CHECK(shipped.rowsHalfDeg == 5.0);
    CHECK(shipped.searchAlongDeg == 8.0);
    CHECK(shipped.searchAcrossDeg == 2.0);
    CHECK(shipped.minFrames == kClipSteadyMinSamples);
    CHECK(shipped.cleanMarginNcc == 0.1);
    CHECK(shipped.cleanMinNcc == 0.6);

    const auto bad = [&](auto mutate) {
        MountMaskParams p;
        mutate(p);
        return !validateMountMaskParams(p).ok();
    };
    CHECK(bad([](MountMaskParams& p) { p.equirectW = 63; }));
    CHECK(bad([](MountMaskParams& p) { p.windowCols = 15; }));  // does not divide 2048
    CHECK(bad([](MountMaskParams& p) { p.releaseNcc = std::nan(""); }));
    CHECK(bad([](MountMaskParams& p) { p.releaseNcc = 1.5; }));
    CHECK(bad([](MountMaskParams& p) { p.rowsHalfDeg = 40.0; }));  // 40 + 8 > 45
    CHECK(bad([](MountMaskParams& p) { p.minCovalidFraction = 0.0; }));
    CHECK(bad([](MountMaskParams& p) { p.minFrames = 0; }));
    CHECK(bad([](MountMaskParams& p) { p.clampThetaDeg = std::nan(""); }));
    CHECK(bad([](MountMaskParams& p) { p.dilateCols = 5000; }));
    CHECK(bad([](MountMaskParams& p) { p.cleanMarginNcc = 0.0; }));  // would name a lens on noise
    CHECK(bad([](MountMaskParams& p) { p.cleanMarginNcc = std::nan(""); }));
    CHECK(bad([](MountMaskParams& p) { p.cleanMinNcc = 1.5; }));
    CHECK(bad([](MountMaskParams& p) { p.cleanMinNcc = std::nan(""); }));
}

TEST_CASE("Hide Mount Auto: the cache's column text round-trips and refuses anything partial", "[mountmask]") {
    std::vector<std::uint8_t> state(2048, kMountKeepClean0);
    for (std::uint32_t c = 300; c < 352; ++c) {
        state[c] = kMountRelease;
    }
    for (std::uint32_t c = 1000; c < 1100; ++c) {
        state[c] = kMountKeepNoClamp;  // kept as the calibration drew it
    }
    for (std::uint32_t c = 1800; c < 2048; ++c) {
        state[c] = kMountKeepClean1;
    }
    const std::string text = encodeMountColumns(state);
    CHECK(text == "0:0-299,2:300-351,0:352-999,3:1000-1099,0:1100-1799,1:1800-2047");
    auto back = decodeMountColumns(text, 2048);
    REQUIRE(back.ok());
    CHECK(back.value() == state);
    // Anything that does not cover the ring exactly once, in order, with a
    // known state, is refused (a damaged cache line is only re-measured).
    for (const char* broken : {"", "0:0-2046", "0:0-2048", "0:1-2047", "0:0-10,0:12-2047", "4:0-2047",
                               "0:0-10;0:11-2047", "x:0-2047", "0:0-1000,0:900-2047", "0:0-2047,"}) {
        INFO(broken);
        CHECK_FALSE(decodeMountColumns(broken, 2048).ok());
    }
    CHECK_FALSE(decodeMountColumns(text, 0).ok());
}

// =============================================================================
//  The correlation
// =============================================================================

TEST_CASE("Hide Mount Auto: a window both lenses see agrees at the right shift, a mount does not",
          "[mountmask]") {
    const MountMaskParams params;
    // Lens 1 sees the scene shifted 10 rows along the meridian and 3 columns
    // across (a near object), except over columns 496..575, where lens 1
    // sees something lens 0 does not (a mount in front of the scene) - wide
    // enough that no searched shift of windows 512 and 528 reaches the scene.
    const int dy = 10;
    const int dx = 3;
    LensBands bands = makeBands(params, [&](int lens, int r, int c) {
        if (lens == 0) {
            return texture(r, c, 1u);
        }
        if (c >= 496 && c < 576) {
            return texture(r, c, 99u);  // unrelated content
        }
        return texture(r - dy, c - dx, 1u);
    });
    const std::vector<std::uint32_t> windows{256, 512, 528};
    auto scores = scoreMountBands(bands, windows, params, nullptr);
    REQUIRE(scores.ok());
    REQUIRE(scores.value().size() == 3u);
    const MountWindowScore& scene = scores.value()[0];
    INFO("scene ncc " << scene.ncc << " upper " << scene.upper << " lower " << scene.lower);
    CHECK(scene.ncc > 0.99f);
    CHECK(scene.upper > 0.99f);
    CHECK(scene.lower > 0.99f);
    CHECK(scene.sigma0 > 0.01f);
    for (std::size_t w = 1; w < 3; ++w) {
        INFO("mount window " << windows[w] << " ncc " << scores.value()[w].ncc);
        CHECK(scores.value()[w].ncc < 0.5f);
    }
    // A pool gives the same numbers (each window is scored on its own).
    ThreadPool pool(2);
    auto pooled = scoreMountBands(bands, windows, params, &pool);
    REQUIRE(pooled.ok());
    for (std::size_t w = 0; w < 3; ++w) {
        CHECK(pooled.value()[w].ncc == scores.value()[w].ncc);
    }
}

TEST_CASE("Hide Mount Auto: flat bands have no correlation and a low sigma; bad bands are refused",
          "[mountmask]") {
    const MountMaskParams params;
    LensBands flat = makeBands(params, [](int, int, int) { return 0.5f; });
    auto scores = scoreMountBands(flat, {0, 1024}, params, nullptr);
    REQUIRE(scores.ok());
    for (const MountWindowScore& s : scores.value()) {
        CHECK(std::isnan(s.ncc));  // no variance: nothing to correlate
        CHECK(s.sigma0 < 0.01f);
        CHECK(s.sigma1 < 0.01f);
    }
    // Uncovered lens 1: no shift has enough co-visible pixels.
    LensBands blind = makeBands(params, [](int lens, int r, int c) { return texture(r, c, lens == 0 ? 1u : 2u); });
    std::fill(blind.alpha[1].begin(), blind.alpha[1].end(), 0.0f);
    auto none = scoreMountBands(blind, {0}, params, nullptr);
    REQUIRE(none.ok());
    CHECK(std::isnan(none.value()[0].ncc));
    CHECK(std::isnan(none.value()[0].sigma1));
    // Malformed input is an error, never a crash.
    LensBands wrongWidth = flat;
    wrongWidth.w = 1024;
    CHECK_FALSE(scoreMountBands(wrongWidth, {0}, params, nullptr).ok());
    LensBands shortPlane = flat;
    shortPlane.luma[1].resize(10);
    CHECK_FALSE(scoreMountBands(shortPlane, {0}, params, nullptr).ok());
    CHECK_FALSE(scoreMountBands(flat, {4096}, params, nullptr).ok());
}

// =============================================================================
//  The decision
// =============================================================================

TEST_CASE("Hide Mount Auto: textured windows are released on their median agreement, then the kept ones dilate",
          "[mountmask]") {
    const MountMaskParams params;
    // An arc of 8 windows: columns 1600..1727.
    const MountArc arc = makeArc(params, 1600, 128);
    const std::vector<std::uint32_t> windows = windowsOf(arc, params);
    REQUIRE(windows.size() == 8u);
    // Windows 0..3 agree on most frames (one frame of nonsense each), 4..7
    // agree on one frame only.
    std::vector<std::vector<MountWindowScore>> frames;
    for (int f = 0; f < 9; ++f) {
        auto frame = frameOf(windows.size(), 0.3f, 0.05f, 0.3f, 0.3f);
        for (std::size_t w = 0; w < 4; ++w) {
            frame[w].ncc = f == 2 ? 0.1f : 0.9f;
        }
        for (std::size_t w = 4; w < 8; ++w) {
            frame[w].ncc = f == 5 ? 0.95f : 0.4f;
        }
        frames.push_back(frame);
    }
    auto decided = decideMountMask(arc, windows, frames, params);
    REQUIRE(decided.ok());
    const MountMask& mask = decided.value();
    REQUIRE(mask.valid());
    for (std::size_t w = 0; w < 4; ++w) {
        CHECK(mask.windows[w].released);
        CHECK_THAT(mask.windows[w].agreement, Catch::Matchers::WithinAbs(0.9, 1e-6));
        CHECK(mask.windows[w].frames == 9u);
    }
    for (std::size_t w = 4; w < 8; ++w) {
        CHECK_FALSE(mask.windows[w].released);
    }
    // Released: 1600..1663, minus the 12 columns the first kept window
    // (1664..1679) dilates back over: 1600..1651.
    for (std::uint32_t c = 1600; c < 1652; ++c) {
        INFO(c);
        CHECK(mask.state[c] == kMountRelease);
    }
    for (std::uint32_t c = 1652; c < 1728; ++c) {
        INFO(c);
        CHECK(mask.state[c] != kMountRelease);
    }
    CHECK(mask.releasedColumns == 52u);
    CHECK(mask.arcColumns == 128u);
    // Columns outside the arc are never released, and nothing there is
    // rebuilt (no polygon reaches them).
    CHECK(mask.state[0] == kMountKeepNoClamp);
    CHECK(mask.state[1599] == kMountKeepNoClamp);
    CHECK(describeMountMask(mask).find("released 52 of 128 arc columns") != std::string::npos);
}

TEST_CASE("Hide Mount Auto: a flat window is released only between two released textured windows",
          "[mountmask]") {
    MountMaskParams params;
    params.dilateCols = 0;  // the windows' own verdicts, undiluted
    const MountArc arc = makeArc(params, 0, 7 * 16);
    const std::vector<std::uint32_t> windows = windowsOf(arc, params);
    REQUIRE(windows.size() == 7u);
    // T = textured and agreeing, F = flat (agrees with anything), K = textured, disagreeing:
    //   T F T  F F T  K
    //   0 1 2  3 4 5  6
    const char* pattern = "TFTFFTK";
    std::vector<std::vector<MountWindowScore>> frames;
    for (int f = 0; f < 9; ++f) {
        std::vector<MountWindowScore> frame(windows.size());
        for (std::size_t w = 0; w < windows.size(); ++w) {
            const char kind = pattern[w];
            frame[w].ncc = kind == 'K' ? 0.2f : 0.95f;
            frame[w].upper = frame[w].lower = frame[w].ncc;
            frame[w].sigma0 = frame[w].sigma1 = kind == 'F' ? 0.004f : 0.05f;
        }
        frames.push_back(frame);
    }
    auto decided = decideMountMask(arc, windows, frames, params);
    REQUIRE(decided.ok());
    const MountMask& mask = decided.value();
    CHECK(mask.windows[0].released);   // textured, agrees
    CHECK(mask.windows[1].flat);
    CHECK(mask.windows[1].released);   // between two released textured windows
    CHECK(mask.windows[2].released);
    CHECK(mask.windows[3].released);  // a run of flat windows bounded by released textured ones
    CHECK(mask.windows[4].released);
    CHECK(mask.windows[5].released);
    CHECK_FALSE(mask.windows[6].released);  // disagrees: the mount
    // The same run with its right bound disagreeing (T F T F F K K): the
    // run ends at a kept textured window, so it stays kept.
    {
        std::vector<std::vector<MountWindowScore>> bounded = frames;
        for (auto& frame : bounded) {
            frame[5].ncc = frame[5].upper = frame[5].lower = 0.2f;
        }
        auto decidedBounded = decideMountMask(arc, windows, bounded, params);
        REQUIRE(decidedBounded.ok());
        CHECK(decidedBounded.value().windows[1].released);
        CHECK_FALSE(decidedBounded.value().windows[3].released);
        CHECK_FALSE(decidedBounded.value().windows[4].released);
        CHECK_FALSE(decidedBounded.value().windows[5].released);
    }
    // The arc's first window has no arc neighbour on its left: a flat window
    // there stays kept, and so does a flat run that reaches the arc's edge.
    // Make window 0 flat and check.
    for (auto& frame : frames) {
        frame[0].sigma0 = frame[0].sigma1 = 0.004f;
    }
    auto again = decideMountMask(arc, windows, frames, params);
    REQUIRE(again.ok());
    CHECK_FALSE(again.value().windows[0].released);
    CHECK_FALSE(again.value().windows[1].released);  // its run now reaches the arc's edge
    CHECK(again.value().windows[3].released);        // still bounded by windows 2 and 5
}

TEST_CASE("Hide Mount Auto: too few scored frames keep the window, and the clean lens follows the halves",
          "[mountmask]") {
    const MountMaskParams params;
    const MountArc arc = makeArc(params, 2000, 96);  // wraps past column 0
    const std::vector<std::uint32_t> windows = windowsOf(arc, params);
    REQUIRE(windows.size() == 6u);
    // Every window agrees, but only on 4 of 9 frames (5 needed).
    std::vector<std::vector<MountWindowScore>> frames;
    for (int f = 0; f < 9; ++f) {
        // Lens 0's far side (upper) agrees better than lens 1's (lower):
        // lens 0 does not image the mount.
        auto frame = frameOf(windows.size(), f < 4 ? 0.95f : std::nanf(""), 0.05f, 0.9f, 0.4f);
        frames.push_back(frame);
    }
    auto decided = decideMountMask(arc, windows, frames, params);
    REQUIRE(decided.ok());
    const MountMask& mask = decided.value();
    CHECK(mask.releasedColumns == 0u);
    for (const MountWindow& w : mask.windows) {
        CHECK_FALSE(w.released);
        CHECK(std::isnan(w.agreement));
        CHECK(w.frames == 4u);
    }
    // Every window across the wrap names lens 0 the clean lens.
    for (const MountWindow& w : mask.windows) {
        CHECK_THAT(w.cleanDiff, Catch::Matchers::WithinAbs(0.5, 1e-6));
        CHECK(w.keep == kMountKeepClean0);
    }
    for (std::uint32_t k = 0; k < 96; ++k) {
        const std::uint32_t c = (2000 + k) % 2048;
        INFO(c);
        CHECK(mask.state[c] == kMountKeepClean0);
    }
    // Swap the halves: lens 1 becomes the clean lens.
    for (auto& frame : frames) {
        for (auto& s : frame) {
            std::swap(s.upper, s.lower);
        }
    }
    auto swapped = decideMountMask(arc, windows, frames, params);
    REQUIRE(swapped.ok());
    CHECK(swapped.value().state[2000] == kMountKeepClean1);
    CHECK(swapped.value().state[40] == kMountKeepClean1);
    // No halves at all: the data cannot tell which lens images the mount,
    // so neither is clamped - the calibration's polygons stay (a clamp of
    // the wrong lens would paint the mount at full weight).
    for (auto& frame : frames) {
        for (auto& s : frame) {
            s.upper = s.lower = std::nanf("");
        }
    }
    auto blind = decideMountMask(arc, windows, frames, params);
    REQUIRE(blind.ok());
    for (std::uint32_t k = 0; k < 96; ++k) {
        const std::uint32_t c = (2000 + k) % 2048;
        INFO(c);
        CHECK(blind.value().state[c] == kMountKeepNoClamp);
    }
    // Mismatched inputs are refused.
    auto shortFrame = frames;
    shortFrame[3].pop_back();
    CHECK_FALSE(decideMountMask(arc, windows, shortFrame, params).ok());
    CHECK_FALSE(decideMountMask(arc, {5}, frames, params).ok());  // not on the window grid
}

TEST_CASE("Hide Mount Auto: one kept run whose windows disagree gets a clean lens per window, or none",
          "[mountmask]") {
    // A car clip's kept run reaches from the roof over the nadir: along the
    // roof lens 1's far side matches lens 0's near side (lens 1 does not see
    // the mount there), at the nadir it is the other way round, and in
    // between the halves cannot tell.  One verdict for the whole run would
    // clamp the lens that sees the suction cup on one of the two stretches.
    const MountMaskParams params;
    const MountArc arc = makeArc(params, 1888, 192);  // 12 windows, wrapping past column 0
    const std::vector<std::uint32_t> windows = windowsOf(arc, params);
    REQUIRE(windows.size() == 12u);
    // Ring order of the windows (1888, 1904, ..., 2032, 0, 16, ..., 64).
    std::vector<std::size_t> ring;
    for (std::uint32_t k = 0; k < 12; ++k) {
        const std::uint32_t c0 = (1888 + 16 * k) % 2048;
        ring.push_back(static_cast<std::size_t>(std::find(windows.begin(), windows.end(), c0) - windows.begin()));
    }
    // Halves per ring position: R = lens 1 clean (roof), A = ambiguous,
    // N = lens 0 clean (nadir), W = lens 0 clean but its own half too weak.
    //   R R R R  A A  N N N N  W  N
    //   0 1 2 3  4 5  6 7 8 9 10 11
    const char* pattern = "RRRRAANNNNWN";
    std::vector<std::vector<MountWindowScore>> frames;
    for (int f = 0; f < 9; ++f) {
        std::vector<MountWindowScore> frame(windows.size());
        for (std::size_t k = 0; k < 12; ++k) {
            MountWindowScore& s = frame[ring[k]];
            s.ncc = 0.4f;  // kept: no window agrees
            s.sigma0 = s.sigma1 = 0.05f;
            switch (pattern[k]) {
            case 'R':
                s.upper = 0.30f;
                s.lower = 0.85f;
                break;
            case 'A':
                s.upper = 0.70f;
                s.lower = 0.72f;
                break;
            case 'N':
                s.upper = 0.88f;
                s.lower = 0.35f;
                break;
            default:  // 'W': lens 0's far side says more than lens 1's, but matches nothing well
                s.upper = 0.55f;
                s.lower = 0.20f;
                break;
            }
        }
        frames.push_back(frame);
    }
    auto decided = decideMountMask(arc, windows, frames, params);
    REQUIRE(decided.ok());
    const MountMask& mask = decided.value();
    CHECK(mask.releasedColumns == 0u);
    // Per window: the roof names lens 1, the nadir lens 0; the ambiguous
    // pair (smoothed |diff| 0.02) and the weak window (lens 0's own half
    // 0.55 < 0.6) clamp nothing.
    const std::uint8_t expected[12] = {kMountKeepClean1,  kMountKeepClean1,  kMountKeepClean1,  kMountKeepClean1,
                                       kMountKeepNoClamp, kMountKeepNoClamp, kMountKeepClean0,  kMountKeepClean0,
                                       kMountKeepClean0,  kMountKeepClean0,  kMountKeepNoClamp, kMountKeepClean0};
    for (std::size_t k = 0; k < 12; ++k) {
        INFO("ring window " << k << " (col " << windows[ring[k]] << ")");
        CHECK(mask.windows[ring[k]].keep == expected[k]);
        // Every column of the window carries its window's verdict.
        for (std::uint32_t c = 0; c < params.windowCols; ++c) {
            CHECK(mask.state[windows[ring[k]] + c] == expected[k]);
        }
    }
    // Outside the arc nothing is named.
    CHECK(mask.state[1000] == kMountKeepNoClamp);

    // A window whose own halves name the OTHER lens vetoes its neighbours'
    // majority: ring position 7 sees the mount in lens 0 (lens 1 clean)
    // between two windows that name lens 0.  The median would name lens 0
    // there; the window itself says that lens images the mount, so no clamp.
    for (auto& frame : frames) {
        frame[ring[7]].upper = 0.30f;
        frame[ring[7]].lower = 0.90f;
    }
    auto vetoed = decideMountMask(arc, windows, frames, params);
    REQUIRE(vetoed.ok());
    INFO("position 7 smoothed difference " << vetoed.value().windows[ring[7]].cleanDiff);
    CHECK(vetoed.value().windows[ring[7]].cleanDiff > 0.1);  // the neighbours' median names lens 0 ...
    CHECK(vetoed.value().windows[ring[7]].keep == kMountKeepNoClamp);  // ... the window vetoes it
    CHECK(vetoed.value().windows[ring[9]].keep == kMountKeepClean0);   // unaffected further along
    CHECK(describeMountMask(vetoed.value()).find("undecided") != std::string::npos);
}

TEST_CASE("Hide Mount Auto: the measurement scores no frame when no polygon reaches the image", "[mountmask]") {
    // A rig without polygons: nothing to keep or release, and the frame
    // source is never asked for a frame.
    geom::LensRig rig;
    for (int i = 0; i < 2; ++i) {
        geom::KannalaBrandt5& L = rig.lens[static_cast<std::size_t>(i)];
        L.fx = L.fy = 800.0;
        L.cx = L.cy = 1500.0;
        L.updateRMax();
    }
    rig.bodyToLens[0] = Mat3d::identity();
    rig.bodyToLens[1] = Mat3d::identity();
    rig.streamW = rig.streamH = 3000;
    int asked = 0;
    const ClipFrameSource source = [&](std::uint32_t) -> Result<video::FramePair> {
        ++asked;
        return Error{ErrorCode::Decoder, "not expected"};
    };
    ThreadPool pool(1);
    auto mask = measureMountMask(rig, geom::BlendParams{}, {0, 10, 20}, source, MountMaskParams{}, pool);
    REQUIRE(mask.ok());
    CHECK(asked == 0);
    CHECK(mask.value().valid());
    CHECK(mask.value().releasedColumns == 0u);
    CHECK(mask.value().arcColumns == 0u);
    // A missing frame source is an error.
    CHECK_FALSE(measureMountMask(rig, geom::BlendParams{}, {0}, ClipFrameSource{}, MountMaskParams{}, pool).ok());
}
