// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_color: transfer-function anchors, both D-Log M curves, the
// float kernel math against the float64 golden (tests/golden/colour_reference.json
// from scripts/colour_reference.py), makeColorParams pipelines, YCbCr
// expansion and the histogram auto-detector.  None of these need the sample
// clip.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/color/AutoDetect.h"
#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/DlogM.h"
#include "osv/color/Matrices.h"
#include "osv/color/Transfer.h"
#include "osv/video/PlanarFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::color;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// The 64 DJI-matched neutral-axis measurements (code i/63 -> HLG signal).
constexpr std::array<double, 64> kDjiHlgTable = {
    0.0118, 0.0157, 0.0157, 0.0196, 0.0235, 0.0275, 0.0353, 0.0444, 0.0549, 0.0627, 0.0706, 0.0863, 0.1020,
    0.1137, 0.1294, 0.1451, 0.1647, 0.1804, 0.2013, 0.2248, 0.2484, 0.2745, 0.2980, 0.3216, 0.3503, 0.3752,
    0.4013, 0.4248, 0.4484, 0.4719, 0.4954, 0.5163, 0.5425, 0.5621, 0.5817, 0.5974, 0.6131, 0.6288, 0.6484,
    0.6601, 0.6758, 0.6915, 0.7072, 0.7229, 0.7386, 0.7503, 0.7660, 0.7778, 0.7935, 0.8052, 0.8183, 0.8327,
    0.8418, 0.8601, 0.8719, 0.8850, 0.8967, 0.9124, 0.9242, 0.9373, 0.9490, 0.9647, 0.9791, 0.9922};

/// Run a grey code through the full pipeline of a parameter block.
float greyThrough(const OsvColorParams& p, float code) {
    const float in[3] = {code, code, code};
    float out[3] = {0.0f, 0.0f, 0.0f};
    osvCodeToOutput(&p, in, out);
    return out[0];
}

/// Curve sampled densely; returns the linear values.
std::vector<double> denseCurve(const OsvDlogMCurve& c, int n) {
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] = dlogmToLinearD(c, static_cast<double>(i) / (n - 1));
    }
    return v;
}

/// Largest "excess" step: step_i minus the mean of its neighbours.  A smooth
/// curve gives ~h^2 * f'' (1e-5 here); a C0 discontinuity gives the jump.
double maxExcessStep(const std::vector<double>& v) {
    double worst = 0.0;
    for (std::size_t i = 2; i + 1 < v.size(); ++i) {
        const double prev = v[i - 1] - v[i - 2];
        const double cur = v[i] - v[i - 1];
        const double next = v[i + 1] - v[i];
        worst = std::max(worst, cur - 0.5 * (prev + next));
    }
    return worst;
}

/// Owning synthetic planar 4:2:0 frame for the auto-detect tests.
struct SyntheticFrame {
    std::shared_ptr<std::vector<std::uint16_t>> y;
    std::shared_ptr<std::vector<std::uint16_t>> c;
    video::PlanarFrame16 frame;
};

/// Build a frame whose luma histogram has the requested shape: a band of
/// mid values around `median` with a spread, a small fraction of highlights
/// at `highlight`, and a border of "outside the image circle" pixels at raw
/// code 70 (what the dual-fisheye corners look like).
SyntheticFrame makeFrame(std::uint32_t w, std::uint32_t h, float median, float halfWidth, float highlight,
                         float highlightFraction, bool p010) {
    SyntheticFrame s;
    const std::uint32_t cw = (w + 1) / 2;
    const std::uint32_t ch = (h + 1) / 2;
    const std::size_t strideY = w + 16;  // padding proves the stride is honoured
    s.y = std::make_shared<std::vector<std::uint16_t>>(strideY * h, std::uint16_t{0});
    s.c = std::make_shared<std::vector<std::uint16_t>>(static_cast<std::size_t>(cw) * ch * 2, std::uint16_t{512});
    std::mt19937 rng(1234u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (std::uint32_t yy = 0; yy < h; ++yy) {
        for (std::uint32_t xx = 0; xx < w; ++xx) {
            float code;
            // Corner region: outside the fisheye circle.
            const float dx = static_cast<float>(xx) - static_cast<float>(w) * 0.5f;
            const float dy = static_cast<float>(yy) - static_cast<float>(h) * 0.5f;
            if (dx * dx + dy * dy > 0.25f * static_cast<float>(w) * static_cast<float>(w)) {
                code = (70.0f - 64.0f) / 876.0f;
            } else if (uni(rng) < highlightFraction) {
                code = highlight;
            } else {
                code = median + (uni(rng) * 2.0f - 1.0f) * halfWidth;
            }
            code = std::clamp(code, 0.0f, 1.0f);
            const std::uint16_t raw = static_cast<std::uint16_t>(std::lround(64.0f + code * 876.0f));
            (*s.y)[yy * strideY + xx] = static_cast<std::uint16_t>(p010 ? (raw << 6) : raw);
        }
    }
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane[0] = s.y->data();
    s.frame.strideElems[0] = strideY;
    if (p010) {
        s.frame.plane[1] = s.c->data();
        s.frame.plane[2] = s.c->data() + 1;
        s.frame.strideElems[1] = s.frame.strideElems[2] = static_cast<std::size_t>(cw) * 2;
        s.frame.chromaInterleaved = true;
        s.frame.bitShift = 6;
    } else {
        s.frame.plane[1] = s.c->data();
        s.frame.plane[2] = s.c->data() + static_cast<std::size_t>(cw) * ch;
        s.frame.strideElems[1] = s.frame.strideElems[2] = cw;
        s.frame.chromaInterleaved = false;
        s.frame.bitShift = 0;
    }
    s.frame.bitDepth = 10;
    s.frame.narrowRange = true;
    s.frame.owner = s.y;
    return s;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Transfer function anchors
// -----------------------------------------------------------------------------
TEST_CASE("HLG OETF anchors", "[color]") {
    REQUIRE_THAT(hlgOetf(1.0f / 12.0f), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(hlgOetf(1.0f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(hlgOetf(0.0f), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(hlgOetf(-1.0f), WithinAbs(0.0, 1e-9));  // negative light clamps
    REQUIRE_THAT(hlgInverseOetf(0.75f), WithinAbs(0.26496, 5e-4));
    REQUIRE_THAT(hlgInverseOetf(0.5f), WithinAbs(1.0 / 12.0, 1e-6));
    // BT.2408: 18 % grey at 38 % signal relative to 75 % reference white.
    const double ratio = static_cast<double>(hlgInverseOetf(0.38f)) / static_cast<double>(hlgInverseOetf(0.75f));
    REQUIRE_THAT(ratio, WithinAbs(0.1816, 2e-3));
    // Double reference agrees with the float kernel.
    for (int i = 0; i <= 100; ++i) {
        const double e = i / 100.0;
        REQUIRE_THAT(static_cast<double>(hlgOetf(static_cast<float>(e))), WithinAbs(ref::hlgOetf(e), 2e-6));
        REQUIRE_THAT(static_cast<double>(hlgInverseOetf(static_cast<float>(e))), WithinAbs(ref::hlgInverseOetf(e), 2e-6));
        // Round trip.
        REQUIRE_THAT(static_cast<double>(hlgOetf(hlgInverseOetf(static_cast<float>(e)))), WithinAbs(e, 2e-6));
    }
}

TEST_CASE("PQ EOTF anchors and round trip", "[color]") {
    REQUIRE_THAT(pqInverseEotf(203.0f), WithinAbs(0.5807, 5e-4));
    REQUIRE_THAT(pqInverseEotf(100.0f), WithinAbs(0.5081, 5e-4));
    REQUIRE_THAT(pqInverseEotf(1000.0f), WithinAbs(0.7518, 5e-4));
    REQUIRE_THAT(pqInverseEotf(26.0f), WithinAbs(0.380, 5e-4));
    REQUIRE_THAT(pqInverseEotf(10000.0f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(pqInverseEotf(0.0f), WithinAbs(0.0, 5e-4));
    REQUIRE_THAT(pqInverseEotf(20000.0f), WithinAbs(1.0, 1e-6));  // clamps above peak
    REQUIRE_THAT(pqEotf(1.0f), WithinRel(10000.0, 1e-5));
    // Round trip over 64 log-spaced luminances: relative error 1e-4.
    for (int i = 0; i < 64; ++i) {
        const float nits = std::pow(10.0f, -1.0f + 5.0f * static_cast<float>(i) / 63.0f);  // 0.1 .. 10000
        const float back = pqEotf(pqInverseEotf(nits));
        REQUIRE_THAT(static_cast<double>(back), WithinRel(static_cast<double>(nits), 1e-4));
        // The float PQ curve ends in powf(..., 78.84), which amplifies one
        // ulp of the base into ~5e-6 of code; 1e-5 is still 1/100 of a 10-bit step.
        REQUIRE_THAT(static_cast<double>(pqInverseEotf(nits)), WithinAbs(ref::pqInverseEotf(nits), 1e-5));
    }
}

TEST_CASE("Rec.709 OETF anchors", "[color]") {
    REQUIRE_THAT(rec709Oetf(0.018f), WithinAbs(0.081, 1e-3));
    REQUIRE_THAT(rec709Oetf(1.0f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(rec709Oetf(0.0f), WithinAbs(0.0, 1e-9));
    for (int i = 0; i <= 50; ++i) {
        const float v = static_cast<float>(i) / 50.0f;
        REQUIRE_THAT(static_cast<double>(rec709Oetf(rec709InverseOetf(v))), WithinAbs(v, 2e-6));
        REQUIRE_THAT(static_cast<double>(rec709Oetf(v)), WithinAbs(ref::rec709Oetf(v), 2e-6));
    }
}

TEST_CASE("BT.2390 EETF behaves per the report", "[color]") {
    const float pq1000 = pqInverseEotf(1000.0f);
    const float pq100 = pqInverseEotf(100.0f);
    // No compression when the target can show the source peak.
    REQUIRE(bt2390Eetf(0.6f, 1000.0f, 1000.0f) == 0.6f);
    REQUIRE(bt2390Eetf(0.6f, 100.0f, 1000.0f) == 0.6f);
    // Source peak lands exactly on the target peak.
    REQUIRE_THAT(bt2390Eetf(pq1000, 1000.0f, 100.0f), WithinAbs(pq100, 1e-5));
    // Below the knee (KS = 1.5 * maxLum - 0.5 = 0.514 normalised) values pass
    // through: 26 nit grey is at 0.380 / 0.7518 = 0.505.
    const float pq26 = pqInverseEotf(26.0f);
    REQUIRE_THAT(bt2390Eetf(pq26, 1000.0f, 100.0f), WithinAbs(pq26, 1e-6));
    // Monotone and bounded on a fine grid, matches the double reference.
    float prev = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float x = static_cast<float>(i) / 200.0f;
        const float y = bt2390Eetf(x, 1000.0f, 100.0f);
        REQUIRE(y >= prev - 1e-7f);
        REQUIRE(y <= pq100 + 1e-6f);
        REQUIRE_THAT(static_cast<double>(y), WithinAbs(ref::bt2390Eetf(x, 1000.0, 100.0), 3e-6));
        prev = y;
    }
}

// -----------------------------------------------------------------------------
//  D-Log M curves
// -----------------------------------------------------------------------------
TEST_CASE("Pocket 3 D-Log M curve anchors and intersection cut", "[color]") {
    REQUIRE(dlogmCurveValid(kDlogMPocket3));
    REQUIRE_THAT(dlogmToLinear(kDlogMPocket3, 0.40f), WithinAbs(0.18, 2e-3));
    REQUIRE_THAT(dlogmToLinear(kDlogMPocket3, 1.0f), WithinAbs(2.4735, 1e-3));
    // The intersection rule reproduces the published cut to float precision.
    REQUIRE_THAT(dlogmCut(kDlogMPocket3), WithinAbs(0.6034245, 1e-6));
    REQUIRE_THAT(dlogmCutD(kDlogMPocket3), WithinAbs(0.6034245, 1e-6));
    // The kernel function and the C++ wrapper are the same math.
    REQUIRE(osvDlogmToLinear(&kDlogMPocket3, 0.4f) == dlogmToLinear(kDlogMPocket3, 0.4f));
}

TEST_CASE("DJI refit D-Log M curve matches the DJI HLG placement", "[color]") {
    REQUIRE(dlogmCurveValid(kDlogMDjiRefit));
    REQUIRE_THAT(dlogmToLinear(kDlogMDjiRefit, 0.40f), WithinAbs(0.18, 1e-4));
    REQUIRE_THAT(dlogmToLinear(kDlogMDjiRefit, 0.0f), WithinAbs(0.0, 1e-4));
    const auto hlgOf = [](float code) { return hlgOetf(kBt2408SceneScale * dlogmToLinear(kDlogMDjiRefit, code)); };
    REQUIRE_THAT(hlgOf(0.400f), WithinAbs(0.380, 0.006));
    REQUIRE_THAT(hlgOf(0.714f), WithinAbs(0.750, 0.015));
    REQUIRE_THAT(hlgOf(1.000f), WithinAbs(0.990, 0.015));

    // Full pipeline (makeColorParams) for grey inputs vs the 64-point table.
    const OsvColorParams p = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f);
    REQUIRE(colorParamsValid(p));
    double worst = 0.0;
    for (int i = 0; i < 64; ++i) {
        const double code = i / 63.0;
        if (code < 0.30) {
            continue;  // the reference toe is crushed 8-bit data
        }
        const double got = greyThrough(p, static_cast<float>(code));
        worst = std::max(worst, std::fabs(got - kDjiHlgTable[static_cast<std::size_t>(i)]));
    }
    INFO("worst |pipeline - DJI table| for code >= 0.30: " << worst);
    REQUIRE(worst < 0.02);
}

TEST_CASE("Both D-Log M curves are strictly increasing and continuous", "[color]") {
    for (const OsvDlogMCurve* c : {&kDlogMPocket3, &kDlogMDjiRefit}) {
        const std::vector<double> v = denseCurve(*c, 4096);
        double maxStep = 0.0;
        for (std::size_t i = 1; i < v.size(); ++i) {
            REQUIRE(v[i] > v[i - 1]);
            maxStep = std::max(maxStep, v[i] - v[i - 1]);
        }
        // Slope of the curve is ~10 linear per unit code at the top end, so
        // adjacent samples differ by a few 1e-3; the continuity check is the
        // second difference, which a C0 discontinuity would blow up.
        REQUIRE(maxStep < 5e-3);
        REQUIRE(maxExcessStep(v) < 1e-4);
    }
    // Negative control: a wrong given cut creates a visible jump the check catches.
    OsvDlogMCurve broken = kDlogMDjiRefit;
    broken.cutMode = OSV_DLOGM_CUT_GIVEN;
    broken.cut = 0.5f;
    REQUIRE(maxExcessStep(denseCurve(broken, 4096)) > 1e-3);
    // And a given cut equal to the intersection is indistinguishable.
    OsvDlogMCurve given = kDlogMDjiRefit;
    given.cutMode = OSV_DLOGM_CUT_GIVEN;
    given.cut = dlogmCut(kDlogMDjiRefit);
    REQUIRE(maxExcessStep(denseCurve(given, 4096)) < 1e-4);
}

TEST_CASE("D-Log M inverse round trip", "[color]") {
    for (const OsvDlogMCurve* c : {&kDlogMPocket3, &kDlogMDjiRefit}) {
        for (int i = 0; i <= 1000; ++i) {
            const double code = i / 1000.0;
            const double lin = dlogmToLinearD(*c, code);
            // Double round trip is exact to 1e-6 (closed form).
            REQUIRE_THAT(linearToDlogmD(*c, lin), WithinAbs(code, 1e-6));
            // Float round trip: the float forward curve then the inverse.
            const float linF = dlogmToLinear(*c, static_cast<float>(code));
            REQUIRE_THAT(static_cast<double>(linearToDlogm(*c, linF)), WithinAbs(code, 2e-5));
        }
        // Garbage in: NaN and negative light do not produce NaN out.
        REQUIRE(std::isfinite(linearToDlogm(*c, std::numeric_limits<float>::quiet_NaN())));
        REQUIRE(linearToDlogm(*c, -5.0f) >= 0.0f);
        REQUIRE(std::isfinite(linearToDlogm(*c, 1e6f)));
    }
    // Invalid curve: inverse refuses rather than dividing by zero.
    OsvDlogMCurve bad = kDlogMPocket3;
    bad.slope = 0.0f;
    REQUIRE_FALSE(dlogmCurveValid(bad));
    REQUIRE(linearToDlogm(bad, 0.5f) == 0.0f);
}

// -----------------------------------------------------------------------------
//  Matrices
// -----------------------------------------------------------------------------
TEST_CASE("Colour matrices preserve white", "[color]") {
    for (const OsvMat3f* m : {&kNativeToRec2020_Pocket3, &kRec2020ToRec709, &kRec709ToRec2020, &kIdentity3}) {
        for (int row = 0; row < 3; ++row) {
            REQUIRE_THAT(static_cast<double>(mat3RowSum(*m, row)), WithinAbs(1.0, 2e-6));
        }
    }
    // 2020 -> 709 -> 2020 is (nearly) identity.
    const OsvMat3f rt = mat3Mul(kRec709ToRec2020, kRec2020ToRec709);
    for (int i = 0; i < 9; ++i) {
        REQUIRE_THAT(static_cast<double>(rt.m[i]), WithinAbs(kIdentity3.m[i], 5e-6));
    }
    // YCbCr matrices: luma weights sum to one through the chroma coefficients.
    float rgb[3];
    osvMat3Apply(&kYuvToRgb709Narrow, 0.5f, 0.0f, 0.0f, rgb);
    REQUIRE_THAT(rgb[0], WithinAbs(0.5, 1e-7));
    REQUIRE_THAT(rgb[1], WithinAbs(0.5, 1e-7));
    REQUIRE_THAT(rgb[2], WithinAbs(0.5, 1e-7));
    // Null matrix is identity (defensive path).
    osvMat3Apply(nullptr, 0.1f, 0.2f, 0.3f, rgb);
    REQUIRE(rgb[2] == 0.3f);
}

// -----------------------------------------------------------------------------
//  Pipelines
// -----------------------------------------------------------------------------
TEST_CASE("Exposure of +1 stop doubles linear output", "[color]") {
    const OsvColorParams p0 = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Linear, 0.0f);
    const OsvColorParams p1 = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Linear, 1.0f);
    const float in[3] = {0.55f, 0.35f, 0.30f};
    float a[3], b[3];
    osvCodeToOutput(&p0, in, a);
    osvCodeToOutput(&p1, in, b);
    for (int i = 0; i < 3; ++i) {
        REQUIRE_THAT(static_cast<double>(b[i]), WithinRel(2.0 * static_cast<double>(a[i]), 1e-5));
    }
    // Linear output of grey 0.40 is 0.18 (no scene scale on the linear path).
    REQUIRE_THAT(greyThrough(p0, 0.40f), WithinAbs(0.18, 2e-4));
}

TEST_CASE("PQ, HLG and Rec.709 grey pipelines", "[color]") {
    const OsvColorParams pq = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::PQ, 0.0f);
    REQUIRE_THAT(greyThrough(pq, 0.40f), WithinAbs(0.380, 0.01));
    const OsvColorParams hlg = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f);
    REQUIRE_THAT(greyThrough(hlg, 0.40f), WithinAbs(0.380, 0.006));
    // Rec.709 output is monotone and in [0,1] across the grey axis.
    const OsvColorParams sdr = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Rec709, 0.0f);
    float prev = -1.0f;
    for (int i = 0; i <= 256; ++i) {
        const float v = greyThrough(sdr, static_cast<float>(i) / 256.0f);
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
        REQUIRE(v >= prev);
        prev = v;
    }
    REQUIRE(greyThrough(sdr, 1.0f) > 0.95f);
    // Passthrough returns the code unchanged; a disabled block too.
    const OsvColorParams pass = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Passthrough, 0.0f);
    REQUIRE(greyThrough(pass, 0.4321f) == 0.4321f);
    const OsvColorParams off = makeDisabledColorParams();
    REQUIRE(greyThrough(off, 0.4321f) == 0.4321f);
}

TEST_CASE("HLG input encoding round trips through the pipeline", "[color]") {
    const OsvColorParams p = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f, InputEncoding::HLG);
    REQUIRE(p.inputEncoding == OSV_INPUT_HLG);
    REQUIRE_THAT(greyThrough(p, 0.75f), WithinAbs(0.75, 1e-5));
    REQUIRE_THAT(greyThrough(p, 0.38f), WithinAbs(0.38, 1e-5));
    // Linear out of an HLG 0.38 grey is 0.18 (scene scale undone on input).
    const OsvColorParams lin = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Linear, 0.0f, InputEncoding::HLG);
    REQUIRE_THAT(greyThrough(lin, 0.38f), WithinAbs(0.18, 1e-3));
    // Rec.709 "Normal" input: 709 grey signal -> linear 0.18-ish.
    const OsvColorParams sdrIn = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::Linear, 0.0f,
                                                 InputEncoding::Rec709Normal);
    REQUIRE_THAT(greyThrough(sdrIn, rec709Oetf(0.18f)), WithinAbs(0.18, 1e-3));
}

TEST_CASE("makeColorParams is defensive about its inputs", "[color]") {
    // Garbage stops / bit depth / scene scale are sanitised, not propagated.
    const OsvColorParams p = makeColorParams(DlogMFit::Pocket3, OutputTransfer::PQ,
                                             std::numeric_limits<float>::quiet_NaN(), InputEncoding::DLogM, true, 99,
                                             nullptr, -1.0f);
    REQUIRE(colorParamsValid(p));
    REQUIRE(p.exposureGain == 1.0f);
    REQUIRE(p.bitDepth == 16);
    REQUIRE(p.sceneScale == kBt2408SceneScale);
    // Curve override wins when valid, is ignored when broken.
    OsvDlogMCurve custom = kDlogMPocket3;
    custom.scale = 6.0f;
    const OsvColorParams o = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f, InputEncoding::DLogM,
                                             true, 10, &custom);
    REQUIRE(o.curve.scale == 6.0f);
    custom.scale = -1.0f;
    const OsvColorParams o2 = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f, InputEncoding::DLogM,
                                              true, 10, &custom);
    REQUIRE(o2.curve.scale == kDlogMDjiRefit.scale);
    // Enum names and parsing round trip.
    for (const DlogMFit f : {DlogMFit::DjiRefit, DlogMFit::Pocket3}) {
        DlogMFit back = DlogMFit::Pocket3;
        REQUIRE(parseDlogMFit(dlogMFitName(f), back));
        REQUIRE(back == f);
    }
    for (const OutputTransfer t : {OutputTransfer::HLG, OutputTransfer::PQ, OutputTransfer::Rec709,
                                   OutputTransfer::Linear, OutputTransfer::Passthrough}) {
        OutputTransfer back = OutputTransfer::HLG;
        REQUIRE(parseOutputTransfer(outputTransferName(t), back));
        REQUIRE(back == t);
    }
    for (const InputEncoding e : {InputEncoding::DLogM, InputEncoding::HLG, InputEncoding::Rec709Normal}) {
        InputEncoding back = InputEncoding::HLG;
        REQUIRE(parseInputEncoding(inputEncodingName(e), back));
        REQUIRE(back == e);
    }
    OutputTransfer t = OutputTransfer::HLG;
    REQUIRE(parseOutputTransfer("PQ", t));
    REQUIRE(t == OutputTransfer::PQ);
    REQUIRE_FALSE(parseOutputTransfer("nonsense", t));
    REQUIRE(t == OutputTransfer::PQ);  // untouched on failure
}

// -----------------------------------------------------------------------------
//  YCbCr expansion
// -----------------------------------------------------------------------------
TEST_CASE("Narrow-range 10-bit YCbCr expands to code", "[color]") {
    const OsvColorParams p = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f);
    float rgb[3];
    osvYuvToCode(&p, 64.0f, 512.0f, 512.0f, rgb);
    REQUIRE_THAT(rgb[0], WithinAbs(0.0, 1e-6));
    osvYuvToCode(&p, 940.0f, 512.0f, 512.0f, rgb);
    REQUIRE_THAT(rgb[1], WithinAbs(1.0, 1e-6));
    osvYuvToCode(&p, 64.0f + 0.4f * 876.0f, 512.0f, 512.0f, rgb);
    REQUIRE_THAT(rgb[2], WithinAbs(0.4, 1e-5));
    // Below black / above white clamp.
    osvYuvToCode(&p, 0.0f, 512.0f, 512.0f, rgb);
    REQUIRE(rgb[0] == 0.0f);
    osvYuvToCode(&p, 1023.0f, 512.0f, 512.0f, rgb);
    REQUIRE(rgb[0] == 1.0f);
    // Forward-encode a colour with BT.709 and decode it back.
    const float r = 0.7f, g = 0.3f, b = 0.5f;
    const float yy = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float cb = (b - yy) / 1.8556f;
    const float cr = (r - yy) / 1.5748f;
    osvYuvToCode(&p, 64.0f + yy * 876.0f, 512.0f + cb * 896.0f, 512.0f + cr * 896.0f, rgb);
    REQUIRE_THAT(rgb[0], WithinAbs(r, 1e-4));
    REQUIRE_THAT(rgb[1], WithinAbs(g, 1e-4));
    REQUIRE_THAT(rgb[2], WithinAbs(b, 1e-4));
    // Full range 8-bit.
    const OsvColorParams full = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f, InputEncoding::DLogM,
                                                false, 8);
    osvYuvToCode(&full, 255.0f, 128.0f, 128.0f, rgb);
    REQUIRE_THAT(rgb[0], WithinAbs(1.0, 1e-6));
    osvYuvToCode(&full, 0.0f, 128.0f, 128.0f, rgb);
    REQUIRE_THAT(rgb[0], WithinAbs(0.0, 1e-6));
    // Null params: zeros, no crash.
    osvYuvToCode(nullptr, 500.0f, 512.0f, 512.0f, rgb);
    REQUIRE(rgb[0] == 0.0f);
}

// -----------------------------------------------------------------------------
//  Golden comparison against the float64 reference
// -----------------------------------------------------------------------------
TEST_CASE("Kernel math matches the float64 golden reference", "[color]") {
    const nlohmann::json j = osvtest::loadGolden("colour_reference.json");

    // Transfer functions.
    for (const auto& pr : j.at("hlg_oetf")) {
        REQUIRE_THAT(static_cast<double>(hlgOetf(pr[0].get<float>())), WithinAbs(pr[1].get<double>(), 2e-6));
    }
    for (const auto& pr : j.at("hlg_inverse_oetf")) {
        REQUIRE_THAT(static_cast<double>(hlgInverseOetf(pr[0].get<float>())), WithinAbs(pr[1].get<double>(), 2e-6));
    }
    for (const auto& pr : j.at("pq_inverse_eotf")) {
        REQUIRE_THAT(static_cast<double>(pqInverseEotf(pr[0].get<float>())), WithinAbs(pr[1].get<double>(), 1e-5));
    }
    for (const auto& pr : j.at("pq_eotf")) {
        REQUIRE_THAT(static_cast<double>(pqEotf(pr[0].get<float>())), WithinRel(pr[1].get<double>(), 1e-4));
    }
    for (const auto& pr : j.at("rec709_oetf")) {
        REQUIRE_THAT(static_cast<double>(rec709Oetf(pr[0].get<float>())), WithinAbs(pr[1].get<double>(), 2e-6));
    }
    for (const auto& pr : j.at("bt2390_eetf_1000_to_100")) {
        REQUIRE_THAT(static_cast<double>(bt2390Eetf(pr[0].get<float>(), 1000.0f, 100.0f)),
                     WithinAbs(pr[1].get<double>(), 3e-6));
    }

    // Curves: the constants in the header equal the ones the script parsed.
    const auto checkCurve = [&](const char* key, const OsvDlogMCurve& c) {
        const nlohmann::json& d = j.at("dlogm").at(key);
        const nlohmann::json& k = d.at("constants");
        REQUIRE_THAT(static_cast<double>(c.xShift), WithinRel(k.at("xShift").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.yShift), WithinRel(k.at("yShift").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.scale), WithinRel(k.at("scale").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.slope), WithinRel(k.at("slope").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.slope2), WithinRel(k.at("slope2").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.intercept), WithinRel(k.at("intercept").get<double>(), 1e-6));
        REQUIRE_THAT(static_cast<double>(c.midGrayScaling), WithinRel(k.at("midGrayScaling").get<double>(), 1e-6));
        REQUIRE(c.cutMode == k.at("cutMode").get<int>());
        REQUIRE_THAT(dlogmCutD(c), WithinRel(d.at("cut").get<double>(), 1e-6));
        REQUIRE(d.at("monotonic_4097").get<bool>());
        for (const auto& pr : d.at("samples")) {
            const double want = pr[1].get<double>();
            const double got = dlogmToLinear(c, pr[0].get<float>());
            REQUIRE_THAT(got, WithinAbs(want, 1e-6 + 4e-6 * std::fabs(want)));
        }
    };
    checkCurve("pocket3", kDlogMPocket3);
    checkCurve("dji_refit", kDlogMDjiRefit);

    // Grey pipelines and RGB spot checks through makeColorParams.
    const auto checkGrey = [&](const char* key, DlogMFit fit, const char* transferKey, OutputTransfer transfer) {
        const OsvColorParams p = makeColorParams(fit, transfer, 0.0f);
        for (const auto& entry : j.at("pipeline_grey").at(key).at(transferKey)) {
            const float code = entry[0].get<float>();
            const float in[3] = {code, code, code};
            float out[3];
            osvCodeToOutput(&p, in, out);
            for (int ch = 0; ch < 3; ++ch) {
                REQUIRE_THAT(static_cast<double>(out[ch]), WithinAbs(entry[1][static_cast<std::size_t>(ch)].get<double>(), 1e-4));
            }
        }
    };
    for (const auto& [key, fit] : {std::pair{"pocket3", DlogMFit::Pocket3}, std::pair{"dji_refit", DlogMFit::DjiRefit}}) {
        checkGrey(key, fit, "hlg", OutputTransfer::HLG);
        checkGrey(key, fit, "pq", OutputTransfer::PQ);
        checkGrey(key, fit, "rec709", OutputTransfer::Rec709);
    }
    for (const auto& spot : j.at("pipeline_rgb_dji_refit")) {
        const float in[3] = {spot.at("code")[0].get<float>(), spot.at("code")[1].get<float>(),
                             spot.at("code")[2].get<float>()};
        for (const auto& [name, transfer] : {std::pair{"hlg", OutputTransfer::HLG}, std::pair{"pq", OutputTransfer::PQ},
                                             std::pair{"rec709", OutputTransfer::Rec709},
                                             std::pair{"linear", OutputTransfer::Linear}}) {
            const OsvColorParams p = makeColorParams(DlogMFit::DjiRefit, transfer, 0.0f);
            float out[3];
            osvCodeToOutput(&p, in, out);
            for (int ch = 0; ch < 3; ++ch) {
                const double want = spot.at(name)[static_cast<std::size_t>(ch)].get<double>();
                REQUIRE_THAT(static_cast<double>(out[ch]), WithinAbs(want, 2e-4 + 1e-4 * std::fabs(want)));
            }
        }
    }
}

// -----------------------------------------------------------------------------
//  Auto-detect
// -----------------------------------------------------------------------------
TEST_CASE("Colour-mode auto-detect from luma statistics", "[color]") {
    // Rule evaluation on synthetic percentiles.
    // With unmasked fisheye corners p001 is ~0, so spread ~= p999 and the
    // D-Log M rule effectively needs p999 <= 0.70.
    REQUIRE(classifyLumaStats(0.01f, 0.42f, 0.68f, 1000).guess == meta::ColorMode::DLogM);
    REQUIRE(classifyLumaStats(0.01f, 0.42f, 0.75f, 1000).guess == meta::ColorMode::Normal);  // spread 0.74 > 0.70
    REQUIRE(classifyLumaStats(0.01f, 0.20f, 0.95f, 1000).guess == meta::ColorMode::HLG);
    REQUIRE(classifyLumaStats(0.01f, 0.45f, 0.98f, 1000).guess == meta::ColorMode::Normal);
    REQUIRE(classifyLumaStats(0.01f, 0.42f, 0.68f, 0).guess == meta::ColorMode::Unknown);
    REQUIRE(classifyLumaStats(std::numeric_limits<float>::quiet_NaN(), 0.4f, 0.7f, 10).guess ==
            meta::ColorMode::Unknown);
    REQUIRE(classifyLumaStats(0.01f, 0.42f, 0.68f, 1000).confidence > 0.5f);

    // D-Log M shaped frame: mid-heavy, highlights capped near 0.66.
    const SyntheticFrame dlog = makeFrame(320, 240, 0.42f, 0.12f, 0.66f, 0.02f, false);
    const AutoDetectResult rd = detectColorMode(dlog.frame, 2);
    INFO("dlog p001 " << rd.p001 << " p50 " << rd.p50 << " p999 " << rd.p999);
    REQUIRE(rd.guess == meta::ColorMode::DLogM);
    REQUIRE(rd.samples > 0);
    REQUIRE(rd.p50 > 0.30f);
    REQUIRE(rd.p50 < 0.55f);
    REQUIRE(rd.p999 < 0.70f);

    // HLG shaped frame: dark-heavy with bright specular highlights.
    const SyntheticFrame hlg = makeFrame(320, 240, 0.22f, 0.10f, 0.97f, 0.03f, false);
    const AutoDetectResult rh = detectColorMode(hlg.frame, 2);
    INFO("hlg p001 " << rh.p001 << " p50 " << rh.p50 << " p999 " << rh.p999);
    REQUIRE(rh.guess == meta::ColorMode::HLG);

    // Rec.709 shaped frame: full range, mid median (P010 layout, subsample 1).
    const SyntheticFrame sdr = makeFrame(320, 240, 0.50f, 0.30f, 0.99f, 0.05f, true);
    const AutoDetectResult rs = detectColorMode(sdr.frame, 1);
    INFO("sdr p001 " << rs.p001 << " p50 " << rs.p50 << " p999 " << rs.p999);
    REQUIRE(rs.guess == meta::ColorMode::Normal);
    REQUIRE(rs.samples == 320u * 240u);

    // Invalid frame: Unknown, no samples, no crash.  Subsample 0 is treated as 1.
    video::PlanarFrame16 empty;
    const AutoDetectResult re = detectColorMode(empty, 8);
    REQUIRE(re.guess == meta::ColorMode::Unknown);
    REQUIRE(re.samples == 0);
    REQUIRE(detectColorMode(dlog.frame, 0).samples == 320u * 240u);
}
