// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the display looks (osv/color/Look.h, osvLookApply in ColorMath.h):
// the DJI Studio Rec.709 look is the default, matches DJI's own measured
// transform within bounds, is monotonic and continuous, never leaves [0, 1],
// leaves every other output transfer and the standard rendering untouched,
// implements exactly the model scripts/fit_look.py fitted, and renders the
// same on the CPU, CUDA and OpenCL backends.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "DjiReference.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/Cube.h"
#include "osv/color/DlogM.h"
#include "osv/color/Look.h"
#include "osv/color/Matrices.h"
#include "osv/color/Transfer.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/Types.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"
#include "osv/video/PlanarFrame.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::color;
using Catch::Matchers::WithinAbs;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// The default (DJI look) Rec.709 block for the Osmo 360 fit.
OsvColorParams djiParams(float stops = 0.0f) {
    return makeColorParams(DlogMFit::Osmo360, OutputTransfer::Rec709, stops);
}

/// The same block with the standard (pre-look) rendering.  [WP-HDRTONE] The
/// HDR outputs keep the default tone style unless another is named, so this
/// differs from the default block in the look alone.
OsvColorParams standardParams(OutputTransfer transfer, float stops = 0.0f, HdrTone tone = kDefaultHdrTone) {
    return makeColorParams(DlogMFit::Osmo360, transfer, stops, InputEncoding::DLogM, true, 10, nullptr,
                           kBt2408SceneScale, Look::Standard, kDefaultHdrPeakNits, tone);
}

/// A D-Log M code triple through the full pipeline.
std::array<float, 3> through(const OsvColorParams& p, float r, float g, float b) {
    const float in[3] = {r, g, b};
    std::array<float, 3> out{};
    osvCodeToOutput(&p, in, out.data());
    return out;
}

/// A working-space (Rec.2020 scene-linear) triple through the output stage.
std::array<float, 3> throughLinear(const OsvColorParams& p, float r, float g, float b) {
    // osvLinearToOutput takes NATIVE light and applies nativeToWorking first;
    // undo that so the caller can speak in working light directly.
    OsvMat3f toNative{};
    REQUIRE(mat3Inverse(p.nativeToWorking, toNative));
    float native[3];
    osvMat3Apply(&toNative, r, g, b, native);
    std::array<float, 3> out{};
    osvLinearToOutput(&p, native, out.data());
    return out;
}

/// Rec.709 signal -> CIELAB, for a BT.1886 (gamma 2.4) display with D65
/// white: the same viewing model scripts/fit_look.py fits in.
std::array<double, 3> signalToLab(const std::array<double, 3>& sig) {
    static constexpr double kToXyz[9] = {0.4123908, 0.3575843, 0.1804808, 0.2126390, 0.7151687,
                                         0.0721923, 0.0193308, 0.1191948, 0.9505322};
    double lin[3];
    for (int c = 0; c < 3; ++c) {
        lin[c] = std::pow(std::clamp(sig[static_cast<std::size_t>(c)], 0.0, 1.0), 2.4);
    }
    double xyz[3];
    double white[3];
    for (int r = 0; r < 3; ++r) {
        xyz[r] = kToXyz[r * 3] * lin[0] + kToXyz[r * 3 + 1] * lin[1] + kToXyz[r * 3 + 2] * lin[2];
        white[r] = kToXyz[r * 3] + kToXyz[r * 3 + 1] + kToXyz[r * 3 + 2];
    }
    const auto f = [](double t) {
        constexpr double e = (6.0 / 29.0) * (6.0 / 29.0) * (6.0 / 29.0);
        return t > e ? std::cbrt(t) : t / (3.0 * (6.0 / 29.0) * (6.0 / 29.0)) + 4.0 / 29.0;
    };
    const double fx = f(xyz[0] / white[0]);
    const double fy = f(xyz[1] / white[1]);
    const double fz = f(xyz[2] / white[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

/// CIEDE2000 colour difference (Sharma, Wu and Dalal 2005).
double deltaE2000(const std::array<double, 3>& lab1, const std::array<double, 3>& lab2) {
    constexpr double kPiRad = 3.14159265358979323846;
    const auto rad = [](double d) { return d * kPiRad / 180.0; };
    const auto deg = [](double r) { return r * 180.0 / kPiRad; };
    const double c1 = std::hypot(lab1[1], lab1[2]);
    const double c2 = std::hypot(lab2[1], lab2[2]);
    const double cb7 = std::pow((c1 + c2) / 2.0, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(cb7 / (cb7 + std::pow(25.0, 7.0))));
    const double a1 = (1.0 + g) * lab1[1];
    const double a2 = (1.0 + g) * lab2[1];
    const double cp1 = std::hypot(a1, lab1[2]);
    const double cp2 = std::hypot(a2, lab2[2]);
    const auto hue = [&](double b, double a) {
        const double h = deg(std::atan2(b, a));
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1 = hue(lab1[2], a1);
    const double h2 = hue(lab2[2], a2);
    double dh = h2 - h1;
    if (cp1 * cp2 == 0.0) {
        dh = 0.0;
    } else if (dh > 180.0) {
        dh -= 360.0;
    } else if (dh < -180.0) {
        dh += 360.0;
    }
    const double dL = lab2[0] - lab1[0];
    const double dC = cp2 - cp1;
    const double dH = 2.0 * std::sqrt(cp1 * cp2) * std::sin(rad(dh / 2.0));
    const double lbar = (lab1[0] + lab2[0]) / 2.0;
    const double cbar = (cp1 + cp2) / 2.0;
    double hbar = h1 + h2;
    if (cp1 * cp2 != 0.0) {
        if (std::fabs(h1 - h2) <= 180.0) {
            hbar /= 2.0;
        } else {
            hbar = (hbar < 360.0) ? (hbar + 360.0) / 2.0 : (hbar - 360.0) / 2.0;
        }
    }
    const double t = 1.0 - 0.17 * std::cos(rad(hbar - 30.0)) + 0.24 * std::cos(rad(2.0 * hbar)) +
                     0.32 * std::cos(rad(3.0 * hbar + 6.0)) - 0.20 * std::cos(rad(4.0 * hbar - 63.0));
    const double dTheta = 30.0 * std::exp(-((hbar - 275.0) / 25.0) * ((hbar - 275.0) / 25.0));
    const double cbar7 = std::pow(cbar, 7.0);
    const double rc = 2.0 * std::sqrt(cbar7 / (cbar7 + std::pow(25.0, 7.0)));
    const double sl = 1.0 + 0.015 * (lbar - 50.0) * (lbar - 50.0) / std::sqrt(20.0 + (lbar - 50.0) * (lbar - 50.0));
    const double sc = 1.0 + 0.045 * cbar;
    const double sh = 1.0 + 0.015 * cbar * t;
    const double rt = -std::sin(rad(2.0 * dTheta)) * rc;
    return std::sqrt((dL / sl) * (dL / sl) + (dC / sc) * (dC / sc) + (dH / sh) * (dH / sh) +
                     rt * (dC / sc) * (dH / sh));
}

/// dE2000 between a rendered float triple and a reference double triple.
double deltaE(const std::array<float, 3>& got, const std::array<double, 3>& want) {
    return deltaE2000(signalToLab(want), signalToLab({got[0], got[1], got[2]}));
}

/// BT.709 luminance of a signal in display-linear light.
double displayLuma(const std::array<float, 3>& sig) {
    const auto lin = [](float v) { return std::pow(std::clamp(static_cast<double>(v), 0.0, 1.0), 2.4); };
    return 0.2126 * lin(sig[0]) + 0.7152 * lin(sig[1]) + 0.0722 * lin(sig[2]);
}

}  // namespace

// -----------------------------------------------------------------------------
//  Selection
// -----------------------------------------------------------------------------
TEST_CASE("the DJI Studio look is the default Rec.709 rendering, and only Rec.709's", "[color][look]") {
    REQUIRE(kDefaultLook == Look::DjiStudio);
    const OsvColorParams dji = djiParams();
    REQUIRE(colorParamsValid(dji));
    REQUIRE(dji.look.id == OSV_LOOK_DJI);
    REQUIRE(lookOf(dji) == Look::DjiStudio);
    REQUIRE(lookApplies(Look::DjiStudio, OutputTransfer::Rec709));
    REQUIRE_FALSE(lookApplies(Look::Standard, OutputTransfer::Rec709));

    // Every other transfer ignores the look entirely: the block carries no
    // look and renders bit-for-bit what the standard block renders.
    for (const OutputTransfer t :
         {OutputTransfer::HLG, OutputTransfer::PQ, OutputTransfer::Linear, OutputTransfer::Passthrough}) {
        REQUIRE_FALSE(lookApplies(Look::DjiStudio, t));
        const OsvColorParams withLook = makeColorParams(DlogMFit::Osmo360, t, 0.0f);
        const OsvColorParams without = standardParams(t);
        REQUIRE(withLook.look.id == OSV_LOOK_STANDARD);
        for (int i = 0; i <= 20; ++i) {
            const float c = static_cast<float>(i) / 20.0f;
            const auto a = through(withLook, c, 0.6f * c, 1.0f - c);
            const auto b = through(without, c, 0.6f * c, 1.0f - c);
            REQUIRE(a == b);
        }
    }

    // The standard Rec.709 rendering is still reachable and is still the HLG
    // signal in Rec.709 primaries (its golden values are pinned in
    // test_color.cpp against scripts/colour_reference.py) - [WP-HDRTONE] the
    // scene-referred HLG signal, i.e. the Neutral style.
    const OsvColorParams standard709 = standardParams(OutputTransfer::Rec709);
    const OsvColorParams hlg = standardParams(OutputTransfer::HLG, 0.0f, HdrTone::Bt2408Neutral);
    REQUIRE(standard709.look.id == OSV_LOOK_STANDARD);
    REQUIRE(lookOf(standard709) == Look::Standard);
    for (int i = 0; i <= 32; ++i) {
        const float c = static_cast<float>(i) / 32.0f;
        REQUIRE_THAT(through(standard709, c, c, c)[1], WithinAbs(through(hlg, c, c, c)[1], 2e-6));
    }

    // setLook switches an existing block both ways, for its own transfer only.
    OsvColorParams p = standard709;
    setLook(p, Look::DjiStudio);
    REQUIRE(p.look.id == OSV_LOOK_DJI);
    setLook(p, Look::Standard);
    REQUIRE(p.look.id == OSV_LOOK_STANDARD);
    OsvColorParams pq = standardParams(OutputTransfer::PQ);
    setLook(pq, Look::DjiStudio);
    REQUIRE(pq.look.id == OSV_LOOK_STANDARD);
    // A corrupt transfer never selects a look.
    OsvColorParams broken = dji;
    broken.transfer = 42;
    setLook(broken, Look::DjiStudio);
    REQUIRE(broken.look.id == OSV_LOOK_STANDARD);
}

TEST_CASE("look names parse and round trip", "[color][look]") {
    for (const Look l : {Look::DjiStudio, Look::Standard}) {
        Look back = (l == Look::DjiStudio) ? Look::Standard : Look::DjiStudio;
        REQUIRE(parseLook(lookName(l), back));
        REQUIRE(back == l);
    }
    Look l = Look::Standard;
    REQUIRE(parseLook("DJI-Studio", l));
    REQUIRE(l == Look::DjiStudio);
    REQUIRE(parseLook("STD", l));
    REQUIRE(l == Look::Standard);
    REQUIRE_FALSE(parseLook("vivid", l));
    REQUIRE(l == Look::Standard);  // untouched on failure
    REQUIRE_FALSE(parseLook("", l));
    REQUIRE(std::string(lookName(static_cast<Look>(77))) == "unknown");
}

// -----------------------------------------------------------------------------
//  The fitted model
// -----------------------------------------------------------------------------
TEST_CASE("the kernel implements exactly the model scripts/fit_look.py fitted", "[color][look]") {
    // Values of fit_look.py's float64 LookModel.forward with the shipped
    // constants - OUR model evaluated at these codes, not DJI data.  The float
    // kernel must reproduce them to float precision, so an edit to
    // osvLookApply that silently changes the look fails here even when it
    // still happens to sit inside the looser DJI bounds below.
    struct Golden {
        std::array<float, 3> code;
        std::array<double, 3> out;
    };
    constexpr std::array<Golden, 10> kModel = {{
        {{0.40f, 0.40f, 0.40f}, {0.377242, 0.377242, 0.377242}},
        {{0.10f, 0.10f, 0.10f}, {0.028170, 0.028170, 0.028170}},
        {{0.95f, 0.95f, 0.95f}, {0.967882, 0.967882, 0.967882}},
        {{0.55f, 0.62f, 0.78f}, {0.490346, 0.597001, 0.883874}},
        {{0.72f, 0.45f, 0.30f}, {0.807919, 0.369412, 0.152910}},
        {{1.00f, 0.72f, 0.50f}, {1.000000, 0.616313, 0.209742}},
        {{1.00f, 0.40f, 0.40f}, {1.000000, 0.092518, 0.241961}},
        {{0.30f, 0.65f, 0.25f}, {0.000899, 0.708094, 0.061218}},
        {{0.02f, 0.05f, 0.30f}, {0.008221, 0.031825, 0.334319}},
        {{0.90f, 0.95f, 1.00f}, {0.858697, 0.944054, 1.000000}},
    }};
    const OsvColorParams p = djiParams();
    for (const Golden& g : kModel) {
        const auto got = through(p, g.code[0], g.code[1], g.code[2]);
        for (int c = 0; c < 3; ++c) {
            INFO("code (" << g.code[0] << ", " << g.code[1] << ", " << g.code[2] << ") channel " << c);
            REQUIRE_THAT(static_cast<double>(got[static_cast<std::size_t>(c)]),
                         WithinAbs(g.out[static_cast<std::size_t>(c)], 2e-5));
        }
    }
}

TEST_CASE("the DJI look matches DJI's measured Osmo 360 transform", "[color][look]") {
    const OsvColorParams dji = djiParams();
    const OsvColorParams standard = standardParams(OutputTransfer::Rec709);
    double sumLook = 0.0;
    double sumStd = 0.0;
    double worstLook = 0.0;
    double worstStd = 0.0;
    int wins = 0;
    for (const osvtest::DjiLookSample& s : osvtest::kDjiLookSamples) {
        const double dl = deltaE(through(dji, s.code[0], s.code[1], s.code[2]), s.rec709);
        const double ds = deltaE(through(standard, s.code[0], s.code[1], s.code[2]), s.rec709);
        sumLook += dl;
        sumStd += ds;
        worstLook = std::max(worstLook, dl);
        worstStd = std::max(worstStd, ds);
        wins += (dl < ds) ? 1 : 0;
    }
    const double n = static_cast<double>(osvtest::kDjiLookSamples.size());
    INFO("DJI look: mean " << sumLook / n << " worst " << worstLook << "; standard: mean " << sumStd / n << " worst "
                           << worstStd << "; look wins " << wins);
    // Measured when the look was fitted: look 1.150 mean / 2.779 worst,
    // standard 2.487 / 12.197, look better on 55 of 72.  Bounds carry a
    // little slack for float evaluation but fail on any real regression.
    REQUIRE(sumLook / n < 1.25);
    REQUIRE(worstLook < 3.0);
    REQUIRE(sumStd / n > 2.3);
    REQUIRE(sumLook / n < 0.5 * sumStd / n);
    REQUIRE(wins >= 50);
}

TEST_CASE("the DJI look's neutral axis is DJI's grey scale", "[color][look]") {
    const OsvColorParams p = djiParams();
    double worstSignal = 0.0;
    double worstDe = 0.0;
    for (std::size_t i = 0; i < osvtest::kOsmo360Rec709Table.size(); ++i) {
        const float c = static_cast<float>(static_cast<double>(i) / 32.0);
        const auto out = through(p, c, c, c);
        // A neutral stays exactly neutral: both matrices have unit row sums
        // and the two colour stages leave equal channels alone.
        REQUIRE_THAT(out[0], WithinAbs(out[1], 1e-5));
        REQUIRE_THAT(out[2], WithinAbs(out[1], 1e-5));
        const double want = osvtest::kOsmo360Rec709Table[i];
        worstDe = std::max(worstDe, deltaE(out, {want, want, want}));
        // Signal error is judged above DJI's crushed toe: below code 0.1 the
        // reference is within 0.012 of black and a signal difference there is
        // invisible (code 1/16: DJI 0.0115, look 0.0010, 0.02 dE2000).
        if (c >= 0.1f) {
            worstSignal = std::max(worstSignal, std::fabs(static_cast<double>(out[1]) - want));
        }
    }
    INFO("worst neutral deviation from DJI: " << worstSignal << " of signal, " << worstDe << " dE2000");
    // Measured 0.0061 of signal (code 0.75) and 0.417 dE2000.
    REQUIRE(worstSignal < 0.0075);
    REQUIRE(worstDe < 0.5);

    // Anchors: black is black, grey sits where DJI puts it, white is reached
    // exactly at code 1.0, and the toe is DJI's crushed one.
    REQUIRE_THAT(through(p, 0.0f, 0.0f, 0.0f)[1], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(through(p, 0.40625f, 0.40625f, 0.40625f)[1], WithinAbs(0.3882, 0.004));
    REQUIRE_THAT(through(p, 1.0f, 1.0f, 1.0f)[1], WithinAbs(1.0, 1e-6));
    REQUIRE(through(p, 0.0625f, 0.0625f, 0.0625f)[1] < 0.02f);

    // Strictly increasing wherever it is below white (4097 codes).
    float prev = -1.0f;
    for (int i = 0; i <= 4096; ++i) {
        const float c = static_cast<float>(i) / 4096.0f;
        const float v = through(p, c, c, c)[1];
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
        if (prev < 1.0f && i > 0) {
            REQUIRE(v > prev);
        }
        prev = v;
    }
}

TEST_CASE("the DJI look rolls highlights off like DJI's", "[color][look]") {
    // DJI's shoulder: the neutral slope over its last grid step (code 31/32 ->
    // 1) is 0.54 of signal per unit code, against 0.83 for the HLG-in-709
    // standard rendering, which is what compresses the sky around the sun.
    const auto slope = [](const OsvColorParams& p) {
        const float a = 31.0f / 32.0f;
        return (through(p, 1.0f, 1.0f, 1.0f)[1] - through(p, a, a, a)[1]) * 32.0f;
    };
    const double dji = (1.0 - osvtest::kOsmo360Rec709Table[31]) * 32.0;
    const float look = slope(djiParams());
    const float standard = slope(standardParams(OutputTransfer::Rec709));
    INFO("top-segment slope: DJI " << dji << ", look " << look << ", standard " << standard);
    REQUIRE(dji < 0.6);
    REQUIRE(standard > 0.8f);
    REQUIRE(look < 0.75f);
    REQUIRE(std::fabs(look - dji) < std::fabs(standard - dji));
}

// -----------------------------------------------------------------------------
//  Monotonic, continuous, in range
// -----------------------------------------------------------------------------
TEST_CASE("the DJI look never darkens as exposure rises", "[color][look]") {
    // A hue scaled up in scene light must never render darker.  400 random
    // hues (including far out-of-gamut ones) from 9 stops under to 5 over grey.
    const OsvColorParams p = djiParams();
    std::mt19937 rng(20260923u);
    std::uniform_real_distribution<float> uni(0.02f, 1.0f);
    double worstDrop = 0.0;
    for (int h = 0; h < 400; ++h) {
        const float base[3] = {uni(rng), uni(rng), uni(rng)};
        double prev = -1.0;
        for (int s = 0; s <= 560; ++s) {
            const float gain = 0.18f * std::exp2(-9.0f + 14.0f * static_cast<float>(s) / 560.0f);
            const double y = displayLuma(throughLinear(p, base[0] * gain, base[1] * gain, base[2] * gain));
            if (prev >= 0.0) {
                worstDrop = std::max(worstDrop, prev - y);
            }
            prev = y;
        }
    }
    INFO("worst luminance drop " << worstDrop);
    // Measured 8.8e-6 of display light: below one 16-bit code, i.e. flat.
    REQUIRE(worstDrop < 2e-5);
}

TEST_CASE("the DJI look is continuous everywhere, including across its joins", "[color][look]") {
    // A 1e-4 nudge of the input light must never move the output by more than
    // a smooth function can: a jump at a knot, at the shaper's continuation
    // below code 0, at the hue blend's onset or at the gamut threshold would
    // show up here as a step far above the slope bound.
    const OsvColorParams p = djiParams();
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> uni(-0.05f, 5.0f);
    double worst = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const float x[3] = {uni(rng), uni(rng), uni(rng)};
        const auto a = throughLinear(p, x[0], x[1], x[2]);
        const auto b = throughLinear(p, x[0] + 1e-4f, x[1] + 1e-4f, x[2] + 1e-4f);
        for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, static_cast<double>(std::fabs(a[static_cast<std::size_t>(c)] -
                                                                  b[static_cast<std::size_t>(c)])));
        }
    }
    INFO("largest output change for a 1e-4 input nudge: " << worst);
    REQUIRE(worst < 0.01);

    // The shaper's continuation below code 0 meets the log section exactly.
    const OsvLookParams& look = p.look;
    const float x0 = look.shaperLin0;
    REQUIRE_THAT(osvLookShaper(&look, x0), WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(osvLookShaper(&look, x0 - 1e-6f), WithinAbs(0.0, 1e-4));
    REQUIRE(osvLookShaper(&look, x0 - 0.01f) < osvLookShaper(&look, x0));
    // And the tone curve passes through its knots and continues linearly.
    for (int k = 0; k < look.knots; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(look.knots - 1);
        REQUIRE_THAT(osvLookTone(&look, u), WithinAbs(look.tone[k], 1e-6));
    }
    REQUIRE_THAT(osvLookTone(&look, 1.1f), WithinAbs(look.tone[look.knots - 1] + 0.1f * look.toneSlope[look.knots - 1], 1e-5));
}

TEST_CASE("the DJI look stays finite and in [0, 1] for hostile input", "[color][look]") {
    const OsvColorParams p = djiParams();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float cases[][3] = {{0.0f, 0.0f, 0.0f},   {-1.0f, -1.0f, -1.0f}, {1e6f, 1e6f, 1e6f},  {1e6f, -1e6f, 0.0f},
                              {-5.0f, 3.0f, 0.1f},  {kInf, 0.0f, 0.0f},    {nan, nan, nan},     {nan, 0.5f, 0.2f},
                              {-kInf, kInf, 1.0f},  {1e-30f, 0.0f, 0.0f},  {100.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 100.0f}};
    for (const auto& c : cases) {
        float out[3] = {-7.0f, -7.0f, -7.0f};
        osvLookApply(&p.look, c, out);
        for (float v : out) {
            INFO("input (" << c[0] << ", " << c[1] << ", " << c[2] << ")");
            REQUIRE(std::isfinite(v));
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
        }
    }
    // Code inputs outside [0, 1] (what an over-ranged decode can hand over).
    for (float c : {-0.5f, 1.5f, 4.0f}) {
        const auto out = through(p, c, 0.5f, 1.0f - c);
        for (float v : out) {
            REQUIRE(std::isfinite(v));
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
        }
    }
    // Null pointers are handled rather than dereferenced.
    float out[3] = {0.0f, 0.0f, 0.0f};
    const float in[3] = {0.3f, 2.0f, -1.0f};
    osvLookApply(nullptr, in, out);
    REQUIRE(out[0] == 0.3f);
    REQUIRE(out[1] == 1.0f);
    REQUIRE(out[2] == 0.0f);
    REQUIRE(osvLookTone(nullptr, 0.25f) == 0.25f);
    REQUIRE(osvLookShaper(nullptr, 0.25f) == 0.25f);
    REQUIRE(osvDlogmToCode(nullptr, 0.25f) == 0.25f);
}

TEST_CASE("exposure acts in scene light before the look's tone curve", "[color][look]") {
    // +1 stop on a code must render exactly like the code whose scene light is
    // twice as bright at 0 stops: the look sees the exposed working value.
    const OsvColorParams p0 = djiParams(0.0f);
    const OsvColorParams p1 = djiParams(1.0f);
    for (float c : {0.2f, 0.35f, 0.5f, 0.65f}) {
        const float brighter = linearToDlogm(kDlogMOsmo360, 2.0f * dlogmToLinear(kDlogMOsmo360, c));
        REQUIRE_THAT(through(p1, c, c, c)[1], WithinAbs(through(p0, brighter, brighter, brighter)[1], 2e-5));
    }
}

TEST_CASE("osvDlogmToCode inverts the D-Log M curves", "[color][look]") {
    for (const OsvDlogMCurve* curve : {&kDlogMOsmo360, &kDlogMDjiRefit, &kDlogMPocket3}) {
        for (int i = 0; i <= 100; ++i) {
            const float code = static_cast<float>(i) / 100.0f;
            const float lin = osvDlogmToLinear(curve, code);
            REQUIRE_THAT(osvDlogmToCode(curve, lin), WithinAbs(code, 2e-4));
        }
    }
    // A degenerate curve returns 0 rather than dividing by (next to) zero.
    // 1e-20 rather than an exact 0 so the compiler's constant folding cannot
    // warn about a division the guard never lets happen.
    OsvDlogMCurve broken = kDlogMOsmo360;
    broken.scale = 1e-20f;
    REQUIRE(osvDlogmToCode(&broken, 0.18f) == 0.0f);
    broken = kDlogMOsmo360;
    broken.slope2 = -1e-20f;
    REQUIRE(osvDlogmToCode(&broken, 0.18f) == 0.0f);
}

// -----------------------------------------------------------------------------
//  Parameter blocks
// -----------------------------------------------------------------------------
TEST_CASE("look parameter blocks are validated", "[color][look]") {
    const OsvLookParams good = makeLookParams(Look::DjiStudio, OutputTransfer::Rec709);
    REQUIRE(good.id == OSV_LOOK_DJI);
    REQUIRE(lookParamsValid(good));
    REQUIRE(good.knots == OSV_LOOK_MAX_KNOTS);
    // The derived values: unit rows kept through the matrix composition,
    // monotone tangents, positive scales.
    for (int r = 0; r < 3; ++r) {
        REQUIRE_THAT(mat3RowSum(good.toLook, r), WithinAbs(1.0, 1e-5));
        REQUIRE_THAT(mat3RowSum(good.display, r), WithinAbs(1.0, 1e-5));
    }
    for (int k = 0; k < good.knots; ++k) {
        REQUIRE(good.toneSlope[k] >= 0.0f);
    }
    // The fit's matrix composed with the camera matrix gives back the fit's
    // native mapping: toLook * nativeToWorking == nativeToLook.
    const OsvMat3f roundTrip = mat3Mul(good.toLook, kNativeToRec2020_Osmo360);
    for (int i = 0; i < 9; ++i) {
        REQUIRE_THAT(roundTrip.m[i], WithinAbs(kLookDjiRec709.nativeToLook.m[i], 1e-5));
    }

    // "No look" is always valid; so is what every non-709 transfer builds.
    REQUIRE(lookParamsValid(makeLookParams(Look::Standard, OutputTransfer::Rec709)));
    REQUIRE(makeLookParams(Look::DjiStudio, OutputTransfer::PQ).id == OSV_LOOK_STANDARD);
    REQUIRE(makeLookParams(static_cast<Look>(9), OutputTransfer::Rec709).id == OSV_LOOK_STANDARD);

    // Corruptions are each rejected.
    const auto rejects = [&](auto mutate) {
        OsvLookParams bad = good;
        mutate(bad);
        return !lookParamsValid(bad);
    };
    REQUIRE(rejects([](OsvLookParams& b) { b.id = 5; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.knots = 1; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.knots = OSV_LOOK_MAX_KNOTS + 1; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.tone[5] = b.tone[4]; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.toneSlope[3] = -0.1f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.toLook.m[0] += 0.01f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.display.m[4] = std::numeric_limits<float>::quiet_NaN(); }));
    REQUIRE(rejects([](OsvLookParams& b) { b.hueWidth = 0.0f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.hueAmount = 1.5f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.gamutThreshold[1] = 1.0f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.gamutScale[2] = 0.0f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.gamutPower = 0.5f; }));
    REQUIRE(rejects([](OsvLookParams& b) { b.shaper.scale = -1.0f; }));

    // A whole colour block carrying a corrupt look fails validation, and a
    // kernel handed one anyway renders the standard picture, not garbage.
    OsvColorParams p = djiParams();
    p.look.id = 7;
    REQUIRE_FALSE(colorParamsValid(p));
    const OsvColorParams standard = standardParams(OutputTransfer::Rec709);
    REQUIRE(through(p, 0.3f, 0.5f, 0.7f) == through(standard, 0.3f, 0.5f, 0.7f));
}

TEST_CASE("monotone tangents keep the tone curve monotonic between knots", "[color][look]") {
    float slopes[OSV_LOOK_MAX_KNOTS];
    REQUIRE(monotoneTangents(kLookDjiRec709.tone, OSV_LOOK_MAX_KNOTS, slopes));
    for (float s : slopes) {
        REQUIRE(s >= 0.0f);
    }
    // Dense evaluation through the kernel's Hermite: strictly increasing.
    const OsvLookParams look = makeLookParams(Look::DjiStudio, OutputTransfer::Rec709);
    float prev = -1.0f;
    for (int i = 0; i <= 8192; ++i) {
        const float v = osvLookTone(&look, static_cast<float>(i) / 8192.0f);
        REQUIRE(v > prev);
        prev = v;
    }
    // A flat stretch gets zero tangents at the plateau (no overshoot), and bad
    // arguments are refused.
    const float plateau[5] = {0.0f, 0.5f, 0.5f, 0.5f, 1.0f};
    float s5[OSV_LOOK_MAX_KNOTS];
    REQUIRE(monotoneTangents(plateau, 5, s5));
    REQUIRE(s5[1] == 0.0f);
    REQUIRE(s5[2] == 0.0f);
    REQUIRE(s5[3] == 0.0f);
    REQUIRE_FALSE(monotoneTangents(plateau, 1, s5));
    REQUIRE_FALSE(monotoneTangents(nullptr, 5, s5));
    REQUIRE_FALSE(monotoneTangents(plateau, 5, nullptr));
    REQUIRE_FALSE(monotoneTangents(plateau, OSV_LOOK_MAX_KNOTS + 1, s5));
}

// -----------------------------------------------------------------------------
//  Against the whole reference file, when it is on this machine
// -----------------------------------------------------------------------------
TEST_CASE("the DJI look against DJI's whole Osmo 360 LUT", "[color][look][dji-lut]") {
    // DJI Studio installs the reference; OSV_DJI_OSMO360_LUT overrides the
    // path.  Nothing from the file is kept - it is read, compared, discarded.
    std::filesystem::path path;
    if (const char* env = std::getenv("OSV_DJI_OSMO360_LUT"); env != nullptr && env[0] != '\0') {
        path = env;
    } else {
        path = "C:/Program Files/DJI Studio/1.0.0.24724/filter/LUT/LOG_Osmo360_DLogM/"
               "DJI Osmo 360 D-Log M to Rec.709 V1.cube";
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        SKIP("DJI's Osmo 360 LUT is not installed (set OSV_DJI_OSMO360_LUT)");
    }
    auto lut = readCube(path);
    REQUIRE(lut.ok());
    REQUIRE(lut.value().size == 33);

    const OsvColorParams dji = djiParams();
    const OsvColorParams standard = standardParams(OutputTransfer::Rec709);
    std::vector<double> look;
    std::vector<double> stdDe;
    const std::uint32_t n = lut.value().size;
    for (std::uint32_t b = 0; b < n; ++b) {
        for (std::uint32_t g = 0; g < n; ++g) {
            for (std::uint32_t r = 0; r < n; ++r) {
                float ref[3];
                lut.value().at(r, g, b, ref);
                const std::array<double, 3> want = {ref[0], ref[1], ref[2]};
                const float cr = static_cast<float>(r) / static_cast<float>(n - 1);
                const float cg = static_cast<float>(g) / static_cast<float>(n - 1);
                const float cb = static_cast<float>(b) / static_cast<float>(n - 1);
                look.push_back(deltaE(through(dji, cr, cg, cb), want));
                stdDe.push_back(deltaE(through(standard, cr, cg, cb), want));
            }
        }
    }
    const auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (double d : v) {
            sum += d;
        }
        return std::array<double, 3>{sum / static_cast<double>(v.size()), v[v.size() * 95 / 100], v.back()};
    };
    const auto l = stats(look);
    const auto s = stats(stdDe);
    INFO("DJI look: mean " << l[0] << " p95 " << l[1] << " max " << l[2] << "; standard: mean " << s[0] << " p95 "
                           << s[1] << " max " << s[2]);
    // Fit report: look 1.231 / 2.798 / 6.117, standard 2.813 / 6.568 / 15.77.
    REQUIRE(l[0] < 1.30);
    REQUIRE(l[1] < 2.9);
    REQUIRE(l[2] < 6.5);
    REQUIRE(s[0] > 2.7);
}

// -----------------------------------------------------------------------------
//  Backend parity
// -----------------------------------------------------------------------------
namespace {

/// Owns a synthetic 10-bit 4:2:0 lens frame: luma ramps left to right across
/// the whole code range, chroma sweeps hue around the frame and saturation
/// from the centre out, so one equirect render walks the look through
/// shadows, highlights, neutrals and far out-of-gamut colours at once.
struct SweepFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

SweepFrame makeSweepFrame(std::uint32_t w, float hueOffset) {
    SweepFrame s;
    const std::uint32_t h = w;
    const std::uint32_t cw = (w + 1) / 2;
    const std::uint32_t ch = (h + 1) / 2;
    const std::size_t lumaN = static_cast<std::size_t>(w) * h;
    const std::size_t chromaN = static_cast<std::size_t>(cw) * ch;
    s.storage = std::make_shared<std::vector<std::uint16_t>>(lumaN + 2 * chromaN);
    std::uint16_t* base = s.storage->data();
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float t = static_cast<float>(x) / static_cast<float>(w - 1);
            base[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(std::lround(64.0f + t * 876.0f));
        }
    }
    for (std::uint32_t y = 0; y < ch; ++y) {
        for (std::uint32_t x = 0; x < cw; ++x) {
            const float dx = static_cast<float>(x) / static_cast<float>(cw) - 0.5f;
            const float dy = static_cast<float>(y) / static_cast<float>(ch) - 0.5f;
            const float sat = std::min(1.0f, 2.0f * std::hypot(dx, dy)) * 0.45f;
            const float ang = std::atan2(dy, dx) + hueOffset;
            const std::size_t i = static_cast<std::size_t>(y) * cw + x;
            base[lumaN + i] = static_cast<std::uint16_t>(std::lround(512.0f + 896.0f * sat * std::cos(ang)));
            base[lumaN + chromaN + i] = static_cast<std::uint16_t>(std::lround(512.0f + 896.0f * sat * std::sin(ang)));
        }
    }
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane = {base, base + lumaN, base + lumaN + chromaN};
    s.frame.strideElems = {w, cw, cw};
    s.frame.bitDepth = 10;
    s.frame.bitShift = 0;
    s.frame.chromaInterleaved = false;
    s.frame.narrowRange = true;
    s.frame.owner = s.storage;
    return s;
}

/// The sample clip's verified calibration as a stream-space rig (the same
/// constants test_render.cpp uses), so parity needs no clip on disk.
Result<geom::LensRig> sweepRig(int streamW) {
    meta::CalibrationSet cal;
    auto fill = [](meta::DewarpParams& d, float fx, float fy, float cx, float cy, const float k[5], float qw, float qx,
                   float qy, float qz) {
        d.fx = fx;
        d.fy = fy;
        d.cx = cx;
        d.cy = cy;
        for (int i = 0; i < 5; ++i) {
            d.k[static_cast<std::size_t>(i)] = k[i];
        }
        d.width = 3840;
        d.height = 3840;
        d.camExtriQ.w = qw;
        d.camExtriQ.x = qx;
        d.camExtriQ.y = qy;
        d.camExtriQ.z = qz;
        d.camExtriQ.present = true;
        for (int b = 1; b <= 11; ++b) {
            d.present.set(static_cast<std::size_t>(b));
        }
        d.present.set(28);
    };
    const float ks[5] = {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f};
    const float km[5] = {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f};
    fill(cal.slave, 1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, ks, 0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f);
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f, -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

/// Render the sweep with the DJI look on `gpu` and on the CPU reference and
/// hold the GPU to the same parity bar the stitch kernels meet.
[[maybe_unused]] void checkLookParity(render::IRenderer& gpu, const char* label) {
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const int W = 600;
    auto rig = sweepRig(W);
    REQUIRE(rig.ok());
    auto slave = makeSweepFrame(static_cast<std::uint32_t>(W), 0.0f);
    auto master = makeSweepFrame(static_cast<std::uint32_t>(W), 1.3f);
    video::FramePair pair;
    pair.lens = {slave.frame, master.frame};

    const OsvColorParams cp = djiParams(0.5f);
    REQUIRE(cp.look.id == OSV_LOOK_DJI);
    geom::EquirectMap map;
    map.w = 1024;
    map.h = 512;
    auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(cp).build(pair);
    REQUIRE(job.ok());
    auto ref = cpu.render(job.value());
    auto test = gpu.render(job.value());
    REQUIRE(ref.ok());
    REQUIRE(test.ok());
    const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
    INFO(label << ": PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes, within2 "
               << stats.fractionWithin2);
    REQUIRE(stats.psnrDb >= 60.0);
    REQUIRE(stats.fractionWithin2 >= 0.9995);
    REQUIRE(stats.maxAbsCode <= 64);
}

}  // namespace

#if defined(OSV_HAVE_CUDA)
TEST_CASE("CUDA renders the DJI look like the CPU reference", "[color][look][cuda]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto r = render::CudaRenderer::create(0);
    REQUIRE(r.ok());
    checkLookParity(*r.value(), "cuda");
}
#endif

#if defined(OSV_HAVE_OPENCL)
TEST_CASE("OpenCL renders the DJI look like the CPU reference", "[color][look][opencl]") {
    std::string reason;
    if (!render::OpenClRenderer::available(&reason)) {
        SKIP("OpenCL unavailable: " << reason);
    }
    auto r = render::OpenClRenderer::create(0);
    if (!r.ok()) {
        FAIL("OpenCL renderer creation failed: " << r.error().message);
    }
    checkLookParity(*r.value(), "opencl");
}
#endif
