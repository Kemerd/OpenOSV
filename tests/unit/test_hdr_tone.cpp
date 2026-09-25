// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// [WP-HDRTONE] Tests for the HDR outputs' transfer function styles
// (osvHdrToneApply in ColorMath.h, setHdrTone in osv/color/ColorParams.h):
//
//   * BT.2408 - Neutral, and every zeroed or unusable tone group, is bit for
//     bit the PQ / HLG output that existed before the setting;
//   * only D-Log M to PQ / HLG ever carries a style, everything else is
//     byte-identical whatever is asked for;
//   * the approved constants, names and labels, and their parsing;
//   * the neutral-axis anchors of each style (grey, diffuse white, the
//     sensor clip), in the float kernel, in the double reference and back
//     through the HLG display's own OOTF;
//   * monotonic, capped at each style's ceiling, finite for hostile input;
//   * luminance mode keeps the scene's chromaticity, per channel does not;
//   * the HDR peak roll-off applies after every style exactly as before;
//   * agreement with the float64 reference renders' own 65^3 PQ tables, spot by spot.
//
// Everything here runs on the CPU; the GPU backends compile the very same
// ColorMath.h and are covered by the existing CPU / CUDA / OpenCL parity
// suites.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/Cube.h"
#include "osv/color/DlogM.h"
#include "osv/color/Transfer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace osv;
using namespace osv::color;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// Every style, in enum (and popup) order.
constexpr HdrTone kAllTones[] = {HdrTone::Aces2Bright, HdrTone::Aces2Detailed, HdrTone::Bt2408Natural,
                                 HdrTone::Bt2408Punchy, HdrTone::Bt2408Neutral};

/// The four tone-scale styles (every one but Neutral).
constexpr HdrTone kCurveTones[] = {HdrTone::Aces2Bright, HdrTone::Aces2Detailed, HdrTone::Bt2408Natural,
                                   HdrTone::Bt2408Punchy};

/// A D-Log M block for the default Osmo 360 fit with a style.
[[nodiscard]] OsvColorParams toneBlock(OutputTransfer transfer, HdrTone tone, float stops = 0.0f,
                                       float hdrPeakNits = kDefaultHdrPeakNits) {
    return makeColorParams(DlogMFit::Osmo360, transfer, stops, InputEncoding::DLogM, true, 10, nullptr,
                           kBt2408SceneScale, kDefaultLook, hdrPeakNits, tone);
}

/// Scene-linear NATIVE light through the output stage.  A neutral native
/// triple is the same neutral in the working space (the camera matrix's rows
/// sum to 1), so a neutral input is the tone scale's own x.
[[nodiscard]] std::array<float, 3> fromLinear(const OsvColorParams& p, float r, float g, float b) {
    const float lin[3] = {r, g, b};
    std::array<float, 3> out{};
    osvLinearToOutput(&p, lin, out.data());
    return out;
}

/// The PQ and HLG branches of osvLinearToOutput exactly as they read before
/// the transfer function styles existed, spelled out from the same
/// primitives, so "Neutral is today's output" is checked against the
/// formula itself rather than against the function under test.  (The HDR
/// peak roll-off, which also predates the styles, follows the PQ encode.)
void asBefore(const OsvColorParams& p, const float lin[3], float out[3]) {
    float working[3];
    float tmp[3];
    osvMat3Apply(&p.nativeToWorking, lin[0], lin[1], lin[2], working);
    for (int i = 0; i < 3; ++i) {
        working[i] *= p.exposureGain;
    }
    for (int i = 0; i < 3; ++i) {
        working[i] = fmaxf(working[i] * p.sceneScale, 0.0f);
    }
    if (p.transfer == OSV_TRANSFER_HLG) {
        osvMat3Apply(&p.workingToOutput, working[0], working[1], working[2], tmp);
        for (int i = 0; i < 3; ++i) {
            out[i] = osvSaturatef(osvHlgOetf(fmaxf(tmp[i], 0.0f)));
        }
        return;
    }
    const float ys = OSV_BT2020_LUMA_R * working[0] + OSV_BT2020_LUMA_G * working[1] + OSV_BT2020_LUMA_B * working[2];
    const float mult = osvHlgOotfScale(ys, p.peakNits, p.ootfGamma);
    float nits[3];
    for (int i = 0; i < 3; ++i) {
        nits[i] = fmaxf(mult * working[i], 0.0f);
    }
    osvMat3Apply(&p.workingToOutput, nits[0], nits[1], nits[2], tmp);
    for (int i = 0; i < 3; ++i) {
        out[i] = osvSaturatef(osvPqInverseEotf(fmaxf(tmp[i], 0.0f)));
    }
    if (p.hdrPeakNits > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            out[i] = osvHdrPeakRolloff(&p, out[i]);
        }
    }
}

/// Bitwise float equality.
[[nodiscard]] bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

/// Display light of a PQ code, in nits (double precision decode).
[[nodiscard]] double pqNits(float code) { return ref::pqEotf(static_cast<double>(code)); }

/// Display light an HLG display at 1000 nits (gamma 1.2) shows for a neutral
/// HLG signal: the inverse OETF, then the BT.2100 OOTF on its own luminance.
[[nodiscard]] double hlgNeutralNits(float signal) {
    const double e = ref::hlgInverseOetf(static_cast<double>(signal));
    return ref::hlgOotfScale(e, 1000.0, 1.2) * e;
}

/// The D-Log M sensor clip (code 1.0) on the osmo360 curve, scene-linear.
[[nodiscard]] float sensorClip() { return dlogmToLinear(kDlogMOsmo360, 1.0f); }

/// BT.2408 diffuse white on the osmo360 curve (code 0.714).
[[nodiscard]] float diffuseWhite() { return dlogmToLinear(kDlogMOsmo360, 0.714f); }

// -----------------------------------------------------------------------------
//  Golden spots: entries of the float64 reference renders' own 65^3 D-Log M -> PQ
//  tables (the float64 reference the styles were designed and signed off
//  with), at grid index (r, g, b), for ACES 2 Bright, ACES 2 Detailed,
//  BT.2408 Natural and BT.2408 Punchy.  Six decimals, as in the files.
// -----------------------------------------------------------------------------
struct GoldenSpot {
    int r, g, b;
    float out[4][3];
};

constexpr GoldenSpot kGoldenSpots[] = {
    {0, 0, 0, {{0.000001f, 0.000001f, 0.000001f}, {0.000001f, 0.000001f, 0.000001f}, {0.000001f, 0.000001f, 0.000001f}, {0.000001f, 0.000001f, 0.000001f}}},
    {16, 16, 16, {{0.264337f, 0.264337f, 0.264337f}, {0.212143f, 0.212143f, 0.212143f}, {0.256662f, 0.256662f, 0.256662f}, {0.256662f, 0.256662f, 0.256662f}}},
    {26, 26, 26, {{0.384149f, 0.384149f, 0.384149f}, {0.329822f, 0.329822f, 0.329822f}, {0.384459f, 0.384459f, 0.384459f}, {0.384459f, 0.384459f, 0.384459f}}},
    {32, 32, 32, {{0.442477f, 0.442477f, 0.442477f}, {0.387701f, 0.387701f, 0.387701f}, {0.447607f, 0.447607f, 0.447607f}, {0.447607f, 0.447607f, 0.447607f}}},
    {46, 46, 46, {{0.563795f, 0.563795f, 0.563795f}, {0.508714f, 0.508714f, 0.508714f}, {0.583568f, 0.583568f, 0.583568f}, {0.583568f, 0.583568f, 0.583568f}}},
    {56, 56, 56, {{0.641294f, 0.641294f, 0.641294f}, {0.587634f, 0.587634f, 0.587634f}, {0.677532f, 0.677532f, 0.677532f}, {0.677532f, 0.677532f, 0.677532f}}},
    {64, 64, 64, {{0.696294f, 0.696294f, 0.696294f}, {0.645303f, 0.645303f, 0.645303f}, {0.751827f, 0.751827f, 0.751827f}, {0.751827f, 0.751827f, 0.751827f}}},
    {64, 0, 0, {{0.677180f, 0.368207f, 0.000001f}, {0.625051f, 0.314018f, 0.000001f}, {0.702934f, 0.403955f, 0.000001f}, {0.724941f, 0.367334f, 0.000001f}}},
    {0, 64, 0, {{0.506380f, 0.695484f, 0.000001f}, {0.451239f, 0.644440f, 0.000001f}, {0.545716f, 0.745092f, 0.000001f}, {0.518150f, 0.750659f, 0.000001f}}},
    {0, 0, 64, {{0.360855f, 0.000001f, 0.696294f}, {0.306734f, 0.000001f, 0.654367f}, {0.375850f, 0.000001f, 0.710399f}, {0.359451f, 0.000001f, 0.751827f}}},
    {64, 64, 0, {{0.692721f, 0.696294f, 0.000001f}, {0.641498f, 0.648357f, 0.000001f}, {0.746574f, 0.751827f, 0.000001f}, {0.746695f, 0.751827f, 0.000001f}}},
    {0, 64, 64, {{0.531581f, 0.692464f, 0.696294f}, {0.476399f, 0.641224f, 0.646188f}, {0.570862f, 0.742439f, 0.748256f}, {0.546560f, 0.746327f, 0.751827f}}},
    {64, 0, 64, {{0.681591f, 0.205214f, 0.696294f}, {0.629702f, 0.156616f, 0.653573f}, {0.712041f, 0.278332f, 0.739855f}, {0.731027f, 0.194456f, 0.751827f}}},
    {45, 36, 30, {{0.543644f, 0.484904f, 0.413977f}, {0.488476f, 0.429856f, 0.359414f}, {0.552391f, 0.497330f, 0.434077f}, {0.560317f, 0.494233f, 0.416642f}}},
    {26, 36, 48, {{0.422001f, 0.470021f, 0.586584f}, {0.367377f, 0.415058f, 0.531718f}, {0.435758f, 0.478276f, 0.588684f}, {0.425336f, 0.477793f, 0.610345f}}},
    {30, 44, 20, {{0.452209f, 0.547415f, 0.189636f}, {0.397364f, 0.492257f, 0.142524f}, {0.471492f, 0.559982f, 0.259792f}, {0.458241f, 0.564642f, 0.178235f}}},
    {60, 20, 10, {{0.650047f, 0.401716f, 0.000001f}, {0.596696f, 0.347248f, 0.000001f}, {0.667926f, 0.429231f, 0.000001f}, {0.688791f, 0.403389f, 0.000001f}}},
    {10, 20, 60, {{0.355715f, 0.070072f, 0.678536f}, {0.301645f, 0.045028f, 0.626480f}, {0.366095f, 0.152732f, 0.671294f}, {0.353946f, 0.058985f, 0.726805f}}},
    {50, 50, 58, {{0.599116f, 0.592868f, 0.660080f}, {0.544431f, 0.538086f, 0.607133f}, {0.625519f, 0.619122f, 0.691198f}, {0.625331f, 0.617834f, 0.701923f}}},
    {38, 30, 28, {{0.485269f, 0.428568f, 0.400800f}, {0.430219f, 0.373895f, 0.346339f}, {0.486440f, 0.436051f, 0.412186f}, {0.494638f, 0.432466f, 0.402399f}}},
    {64, 48, 32, {{0.682614f, 0.592041f, 0.396196f}, {0.630783f, 0.537248f, 0.341772f}, {0.721589f, 0.622229f, 0.438439f}, {0.732448f, 0.616846f, 0.397433f}}},
    {8, 4, 2, {{0.083761f, 0.048065f, 0.017339f}, {0.055081f, 0.029588f, 0.009801f}, {0.057246f, 0.042996f, 0.026872f}, {0.071924f, 0.038822f, 0.012609f}}},
    {48, 18, 55, {{0.567128f, 0.267072f, 0.642826f}, {0.512070f, 0.214774f, 0.589217f}, {0.570110f, 0.310433f, 0.648121f}, {0.587450f, 0.259558f, 0.679490f}}},
    {60, 59, 57, {{0.667936f, 0.663517f, 0.646844f}, {0.615343f, 0.610721f, 0.593376f}, {0.711641f, 0.706536f, 0.687707f}, {0.712391f, 0.706482f, 0.684652f}}},
    {2, 13, 23, {{0.114569f, 0.201217f, 0.360421f}, {0.078846f, 0.152975f, 0.306304f}, {0.140732f, 0.192298f, 0.296480f}, {0.101897f, 0.190285f, 0.358986f}}},
    {5, 52, 45, {{0.426767f, 0.608293f, 0.549392f}, {0.372107f, 0.553772f, 0.494241f}, {0.458528f, 0.631394f, 0.572286f}, {0.430509f, 0.636439f, 0.566914f}}},
    {22, 43, 29, {{0.406723f, 0.537652f, 0.389881f}, {0.352216f, 0.482474f, 0.335507f}, {0.429299f, 0.548291f, 0.414840f}, {0.408796f, 0.553470f, 0.390629f}}},
    {31, 59, 64, {{0.540045f, 0.657592f, 0.696294f}, {0.484870f, 0.604540f, 0.648741f}, {0.574009f, 0.695929f, 0.745992f}, {0.556201f, 0.698643f, 0.751827f}}},
    {61, 8, 55, {{0.659515f, 0.284031f, 0.642296f}, {0.606543f, 0.231181f, 0.588670f}, {0.680815f, 0.334219f, 0.661621f}, {0.701176f, 0.277536f, 0.678813f}}},
    {8, 12, 53, {{0.291600f, 0.000001f, 0.628710f}, {0.238547f, 0.000001f, 0.574669f}, {0.301765f, 0.000001f, 0.597043f}, {0.285573f, 0.000001f, 0.661629f}}},
    {3, 11, 59, {{0.327311f, 0.000001f, 0.671943f}, {0.273578f, 0.000001f, 0.619545f}, {0.340502f, 0.000001f, 0.659781f}, {0.323590f, 0.000001f, 0.717798f}}},
    {37, 56, 4, {{0.529039f, 0.641355f, 0.000001f}, {0.473856f, 0.587697f, 0.000001f}, {0.558657f, 0.672893f, 0.000001f}, {0.543675f, 0.677610f, 0.000001f}}},
};

}  // namespace

// -----------------------------------------------------------------------------
//  Neutral is today's output
// -----------------------------------------------------------------------------
TEST_CASE("BT.2408 Neutral is bit for bit the HDR output before the setting existed", "[color][hdrtone]") {
    for (const OutputTransfer t : {OutputTransfer::PQ, OutputTransfer::HLG}) {
        // The Neutral block carries a zeroed group: byte-identical to a
        // block from before the fields existed.
        const OsvColorParams neutral = toneBlock(t, HdrTone::Bt2408Neutral);
        CHECK(neutral.hdrToneMode == OSV_HDR_TONE_OFF);
        CHECK(neutral.hdrToneM2 == 0.0f);
        CHECK(neutral.hdrToneS2 == 0.0f);
        CHECK(neutral.hdrToneG == 0.0f);
        CHECK(neutral.hdrToneT1 == 0.0f);
        CHECK(neutral.hdrToneCapNits == 0.0f);
        CHECK(colorParamsValid(neutral));
        CHECK_FALSE(osvHdrToneActive(&neutral));
        // A styled block differs from it in the tone group alone.
        const OsvColorParams bright = toneBlock(t, HdrTone::Aces2Bright);
        OsvColorParams zeroed = bright;
        zeroed.hdrToneMode = OSV_HDR_TONE_OFF;
        zeroed.hdrToneM2 = zeroed.hdrToneS2 = zeroed.hdrToneG = zeroed.hdrToneT1 = zeroed.hdrToneCapNits = 0.0f;
        CHECK(std::memcmp(&zeroed, &neutral, sizeof(OsvColorParams)) == 0);

        // The pixels: a dense neutral and coloured sweep at three exposures
        // (so light past the sensor clip is included), bitwise against the
        // pre-setting formula - for Neutral and for a hand-zeroed group, and
        // with the PQ roll-off on as well.
        for (const float stops : {0.0f, 2.0f, -1.5f}) {
            for (const float peak : {kDefaultHdrPeakNits, 400.0f}) {
                const OsvColorParams p = toneBlock(t, HdrTone::Bt2408Neutral, stops, peak);
                int compared = 0;
                for (int i = 0; i <= 256; ++i) {
                    const float c = static_cast<float>(i) / 256.0f;
                    for (const std::array<float, 3> code :
                         {std::array<float, 3>{c, c, c}, std::array<float, 3>{c, 0.5f * c, 0.2f},
                          std::array<float, 3>{0.3f, c, 1.0f - c}}) {
                        float lin[3];
                        osvCodeToLinear(&p, code.data(), lin);
                        float now[3];
                        float before[3];
                        osvLinearToOutput(&p, lin, now);
                        asBefore(p, lin, before);
                        for (int k = 0; k < 3; ++k) {
                            INFO(outputTransferName(t) << " stops " << stops << " peak " << peak << " code "
                                                       << code[0] << "," << code[1] << "," << code[2]);
                            REQUIRE(sameBits(now[k], before[k]));
                        }
                        ++compared;
                    }
                }
                CHECK(compared == 3 * 257);
            }
        }
    }
}

TEST_CASE("only D-Log M to PQ and HLG carries a style", "[color][hdrtone]") {
    // Rec.709, linear and the passthrough have no HDR transfer function: the
    // block is byte-identical whatever style is asked for.
    for (const OutputTransfer t : {OutputTransfer::Rec709, OutputTransfer::Linear, OutputTransfer::Passthrough}) {
        const OsvColorParams base = toneBlock(t, HdrTone::Bt2408Neutral);
        for (const HdrTone tone : kAllTones) {
            const OsvColorParams asked = toneBlock(t, tone);
            INFO(outputTransferName(t) << " " << hdrToneName(tone));
            CHECK(std::memcmp(&base, &asked, sizeof(OsvColorParams)) == 0);
            CHECK(asked.hdrToneMode == OSV_HDR_TONE_OFF);
        }
    }
    // An HLG or Normal clip is already a display rendering of its own: its
    // HDR outputs keep it, whatever style is asked for.
    for (const InputEncoding in : {InputEncoding::HLG, InputEncoding::Rec709Normal}) {
        for (const OutputTransfer t : {OutputTransfer::PQ, OutputTransfer::HLG}) {
            const OsvColorParams base = makeColorParams(DlogMFit::Osmo360, t, 0.0f, in, true, 10, nullptr,
                                                        kBt2408SceneScale, kDefaultLook, kDefaultHdrPeakNits,
                                                        HdrTone::Bt2408Neutral);
            for (const HdrTone tone : kAllTones) {
                const OsvColorParams asked = makeColorParams(DlogMFit::Osmo360, t, 0.0f, in, true, 10, nullptr,
                                                             kBt2408SceneScale, kDefaultLook, kDefaultHdrPeakNits,
                                                             tone);
                INFO(inputEncodingName(in) << " -> " << outputTransferName(t) << " " << hdrToneName(tone));
                CHECK(std::memcmp(&base, &asked, sizeof(OsvColorParams)) == 0);
            }
        }
    }
    // D-Log M to PQ / HLG carries the style's constants, and every styled
    // block is valid.
    for (const OutputTransfer t : {OutputTransfer::PQ, OutputTransfer::HLG}) {
        for (const HdrTone tone : kCurveTones) {
            const OsvColorParams p = toneBlock(t, tone);
            const HdrToneCurve curve = hdrToneCurve(tone);
            INFO(outputTransferName(t) << " " << hdrToneName(tone));
            CHECK(p.hdrToneMode == curve.mode);
            CHECK(p.hdrToneM2 == static_cast<float>(curve.m2));
            CHECK(p.hdrToneS2 == static_cast<float>(curve.s2));
            CHECK(p.hdrToneG == static_cast<float>(curve.g));
            CHECK(p.hdrToneT1 == static_cast<float>(curve.t1));
            CHECK(p.hdrToneCapNits == static_cast<float>(curve.capNits));
            CHECK(osvHdrToneActive(&p));
            CHECK(colorParamsValid(p));
        }
    }
    // The default is ACES 2 Bright, and setHdrTone switches a block both ways.
    REQUIRE(kDefaultHdrTone == HdrTone::Aces2Bright);
    const OsvColorParams def = makeColorParams(DlogMFit::Osmo360, OutputTransfer::PQ, 0.0f);
    const OsvColorParams bright = toneBlock(OutputTransfer::PQ, HdrTone::Aces2Bright);
    CHECK(std::memcmp(&def, &bright, sizeof(OsvColorParams)) == 0);
    OsvColorParams p = def;
    setHdrTone(p, HdrTone::Bt2408Neutral);
    CHECK(p.hdrToneMode == OSV_HDR_TONE_OFF);
    setHdrTone(p, HdrTone::Bt2408Natural);
    CHECK(p.hdrToneMode == OSV_HDR_TONE_LUMINANCE);
    // An out-of-range value is the default style, in both entry points.
    setHdrTone(p, static_cast<HdrTone>(99));
    CHECK(std::memcmp(&p, &def, sizeof(OsvColorParams)) == 0);
    const OsvColorParams corrupt = toneBlock(OutputTransfer::PQ, static_cast<HdrTone>(-3));
    CHECK(std::memcmp(&corrupt, &def, sizeof(OsvColorParams)) == 0);
    // setHdrTone never puts a style on a block that cannot carry one.
    OsvColorParams sdr = toneBlock(OutputTransfer::Rec709, HdrTone::Bt2408Neutral);
    setHdrTone(sdr, HdrTone::Aces2Detailed);
    CHECK(sdr.hdrToneMode == OSV_HDR_TONE_OFF);
}

// -----------------------------------------------------------------------------
//  Constants, names, labels
// -----------------------------------------------------------------------------
TEST_CASE("the styles carry the approved constants and names", "[color][hdrtone]") {
    // The reference renders' constants, to the digit (docs/COLOR.md).
    struct Expected {
        HdrTone tone;
        int mode;
        double m2, s2, g, t1, cap;
    };
    constexpr Expected kExpected[] = {
        {HdrTone::Aces2Bright, OSV_HDR_TONE_PER_CHANNEL, 24.9763073, 9.93461981, 1.09839444, 0.0448692404, 600.0},
        {HdrTone::Aces2Detailed, OSV_HDR_TONE_PER_CHANNEL, 19.8623327, 13.2779859, 1.09839444, 0.0448692404, 1000.0},
        {HdrTone::Bt2408Natural, OSV_HDR_TONE_LUMINANCE, 1641.6635, 300.45295, 1.16036774, 0.0448692404, 1000.0},
        {HdrTone::Bt2408Punchy, OSV_HDR_TONE_PER_CHANNEL, 1641.6635, 300.45295, 1.16036774, 0.0448692404, 1000.0},
        {HdrTone::Bt2408Neutral, OSV_HDR_TONE_OFF, 0.0, 0.0, 0.0, 0.0, 0.0},
    };
    for (const Expected& e : kExpected) {
        const HdrToneCurve c = hdrToneCurve(e.tone);
        INFO(hdrToneName(e.tone));
        CHECK(c.mode == e.mode);
        CHECK(c.m2 == e.m2);
        CHECK(c.s2 == e.s2);
        CHECK(c.g == e.g);
        CHECK(c.t1 == e.t1);
        CHECK(c.capNits == e.cap);
    }
    // The enum is persisted: its values never move.
    CHECK(static_cast<int>(HdrTone::Aces2Bright) == 0);
    CHECK(static_cast<int>(HdrTone::Aces2Detailed) == 1);
    CHECK(static_cast<int>(HdrTone::Bt2408Natural) == 2);
    CHECK(static_cast<int>(HdrTone::Bt2408Punchy) == 3);
    CHECK(static_cast<int>(HdrTone::Bt2408Neutral) == 4);
    CHECK(kHdrToneCount == 5);

    // Names and labels, and every name parses back to its style.
    const char* const kNames[] = {"aces-bright", "aces-detailed", "bt2408-natural", "bt2408-punchy", "bt2408-neutral"};
    const char* const kLabels[] = {"ACES 2 - Bright (outdoor)", "ACES 2 - Detailed (indoor)",
                                   "BT.2408 - Deep Blacks + Natural", "BT.2408 - Deep Blacks + Punchy",
                                   "BT.2408 - Neutral"};
    for (int i = 0; i < kHdrToneCount; ++i) {
        const HdrTone tone = kAllTones[i];
        CHECK(std::string(hdrToneName(tone)) == kNames[i]);
        CHECK(std::string(hdrToneLabel(tone)) == kLabels[i]);
        HdrTone back = HdrTone::Bt2408Punchy;
        CHECK(parseHdrTone(hdrToneName(tone), back));
        CHECK(back == tone);
    }
    // The short names and aliases, case-insensitive.
    const std::pair<const char*, HdrTone> kAliases[] = {
        {"bright", HdrTone::Aces2Bright},     {"ACES-Bright", HdrTone::Aces2Bright},
        {"detailed", HdrTone::Aces2Detailed}, {"natural", HdrTone::Bt2408Natural},
        {"punchy", HdrTone::Bt2408Punchy},    {"neutral", HdrTone::Bt2408Neutral},
        {"standard", HdrTone::Bt2408Neutral}, {"BT2408-NEUTRAL", HdrTone::Bt2408Neutral},
    };
    for (const auto& [text, want] : kAliases) {
        HdrTone got = HdrTone::Bt2408Punchy;
        INFO(text);
        CHECK(parseHdrTone(text, got));
        CHECK(got == want);
    }
    // Anything else is refused and leaves the output alone.
    for (const char* bad : {"", "aces", "aces 2", "bright ", "hlg", "0", "neutrals"}) {
        HdrTone got = HdrTone::Aces2Detailed;
        INFO("'" << bad << "'");
        CHECK_FALSE(parseHdrTone(bad, got));
        CHECK(got == HdrTone::Aces2Detailed);
    }
    CHECK(std::string(hdrToneName(static_cast<HdrTone>(42))) == "unknown");
    CHECK(std::string(hdrToneLabel(static_cast<HdrTone>(42))) == "unknown");
}

// -----------------------------------------------------------------------------
//  Anchors
// -----------------------------------------------------------------------------
TEST_CASE("each style puts grey, diffuse white and the sensor clip where it was designed to", "[color][hdrtone]") {
    const float clip = sensorClip();
    const float white = diffuseWhite();
    REQUIRE_THAT(static_cast<double>(clip), WithinAbs(3.7647, 5e-4));
    REQUIRE_THAT(static_cast<double>(white), WithinAbs(0.95775, 5e-4));

    // The neutral-axis design points in nits: grey, diffuse white, clip.
    struct Anchor {
        HdrTone tone;
        double grey, white, clip;
    };
    constexpr Anchor kAnchors[] = {
        {HdrTone::Aces2Bright, 26.0, 168.51, 600.0},
        {HdrTone::Aces2Detailed, 13.81, 98.16, 373.74},
        {HdrTone::Bt2408Natural, 26.0, 203.0, 1000.0},
        {HdrTone::Bt2408Punchy, 26.0, 203.0, 1000.0},
    };
    for (const Anchor& a : kAnchors) {
        INFO(hdrToneName(a.tone));
        const HdrToneCurve curve = hdrToneCurve(a.tone);
        // The double reference.
        CHECK_THAT(hdrToneNits(curve, 0.18), WithinAbs(a.grey, 0.01));
        CHECK_THAT(hdrToneNits(curve, static_cast<double>(white)), WithinAbs(a.white, 0.1));
        CHECK_THAT(std::min(hdrToneNits(curve, static_cast<double>(clip)), curve.capNits), WithinAbs(a.clip, 0.1));
        // The float kernel through PQ, and through HLG as an HLG display at
        // 1000 nits (gamma 1.2) shows it: the inverse OOTF undoes exactly the
        // display's own OOTF, so both land on the same light.
        const OsvColorParams pq = toneBlock(OutputTransfer::PQ, a.tone);
        const OsvColorParams hlg = toneBlock(OutputTransfer::HLG, a.tone);
        for (const auto& [x, want] : {std::pair{0.18f, a.grey}, std::pair{white, a.white}, std::pair{clip, a.clip}}) {
            const auto viaPq = fromLinear(pq, x, x, x);
            const auto viaHlg = fromLinear(hlg, x, x, x);
            INFO("scene " << x);
            // Neutral in, neutral out (the camera matrix's float row sums can
            // move a channel by an ulp, nothing more).
            CHECK_THAT(static_cast<double>(viaPq[0]), WithinAbs(static_cast<double>(viaPq[1]), 1e-6));
            CHECK_THAT(static_cast<double>(viaPq[2]), WithinAbs(static_cast<double>(viaPq[1]), 1e-6));
            CHECK_THAT(pqNits(viaPq[1]), WithinRel(want, 1e-3));
            CHECK_THAT(hlgNeutralNits(viaHlg[1]), WithinRel(want, 2e-3));
        }
    }
    // Neutral keeps BT.2408's scene-referred placement: grey at HLG 0.38.
    const OsvColorParams neutralHlg = toneBlock(OutputTransfer::HLG, HdrTone::Bt2408Neutral);
    CHECK_THAT(static_cast<double>(fromLinear(neutralHlg, 0.18f, 0.18f, 0.18f)[1]), WithinAbs(0.380, 2e-3));
    // Bright and the BT.2408 styles keep grey where Neutral has it (26 nits,
    // PQ 0.380), so switching between them never moves faces and mid-tones.
    for (const HdrTone tone : {HdrTone::Aces2Bright, HdrTone::Bt2408Natural, HdrTone::Bt2408Punchy}) {
        CHECK_THAT(static_cast<double>(fromLinear(toneBlock(OutputTransfer::PQ, tone), 0.18f, 0.18f, 0.18f)[1]),
                   WithinAbs(0.380, 1e-3));
    }
}

TEST_CASE("the float kernel follows the double reference", "[color][hdrtone]") {
    for (const HdrTone tone : kCurveTones) {
        const HdrToneCurve curve = hdrToneCurve(tone);
        const OsvColorParams p = toneBlock(OutputTransfer::PQ, tone);
        double worst = 0.0;
        // Twenty stops of scene light, down to 18 % grey / 2^14.
        for (int i = 0; i <= 400; ++i) {
            const double x = 0.18 * std::exp2(-14.0 + 20.0 * i / 400.0);
            const double want = hdrToneNits(curve, x);
            const double got = osvHdrToneCurve(&p, static_cast<float>(x));
            worst = std::max(worst, std::fabs(got - want) / std::max(want, 1e-3));
        }
        INFO(hdrToneName(tone) << ": worst relative error " << worst);
        CHECK(worst < 2e-5);
    }
    // The reference refuses what the kernel refuses.
    CHECK(hdrToneNits(hdrToneCurve(HdrTone::Bt2408Neutral), 0.18) == 0.0);
    CHECK(hdrToneNits(hdrToneCurve(HdrTone::Aces2Bright), std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(hdrToneNits(hdrToneCurve(HdrTone::Aces2Bright), -1.0) == 0.0);
}

// -----------------------------------------------------------------------------
//  Shape
// -----------------------------------------------------------------------------
TEST_CASE("every style is monotonic and stays under its ceiling", "[color][hdrtone]") {
    for (const OutputTransfer t : {OutputTransfer::PQ, OutputTransfer::HLG}) {
        for (const HdrTone tone : kAllTones) {
            for (const float stops : {0.0f, 2.0f}) {
                const OsvColorParams p = toneBlock(t, tone, stops);
                INFO(outputTransferName(t) << " " << hdrToneName(tone) << " stops " << stops);
                // The neutral axis, densely: never decreasing, never outside
                // the signal range.
                float prev = -1.0f;
                for (int i = 0; i <= 4096; ++i) {
                    const float c = static_cast<float>(i) / 4096.0f;
                    const float in[3] = {c, c, c};
                    float out[3];
                    osvCodeToOutput(&p, in, out);
                    REQUIRE(out[1] >= prev);
                    REQUIRE(out[1] >= 0.0f);
                    REQUIRE(out[1] <= 1.0f);
                    prev = out[1];
                }
                // Each channel on its own, for the per-channel styles (a
                // luminance style moves every channel with Y, by design).
                if (p.hdrToneMode == OSV_HDR_TONE_PER_CHANNEL && t == OutputTransfer::PQ) {
                    for (int ch = 0; ch < 3; ++ch) {
                        float prevCh = -1.0f;
                        for (int i = 0; i <= 512; ++i) {
                            float in[3] = {0.35f, 0.45f, 0.30f};
                            in[ch] = static_cast<float>(i) / 512.0f;
                            float out[3];
                            osvCodeToOutput(&p, in, out);
                            REQUIRE(out[ch] >= prevCh);
                            prevCh = out[ch];
                        }
                    }
                }
            }
        }
        // The ceiling: no PQ code of a style ever passes PQ(cap), over the
        // whole cube and with light pushed two stops past the sensor clip.
        if (t == OutputTransfer::PQ) {
            for (const HdrTone tone : kCurveTones) {
                const OsvColorParams p = toneBlock(t, tone, 2.0f);
                const float ceiling = osvPqInverseEotf(p.hdrToneCapNits);
                float highest = 0.0f;
                for (int b = 0; b <= 16; ++b) {
                    for (int g = 0; g <= 16; ++g) {
                        for (int r = 0; r <= 16; ++r) {
                            const float in[3] = {r / 16.0f, g / 16.0f, b / 16.0f};
                            float out[3];
                            osvCodeToOutput(&p, in, out);
                            highest = std::max({highest, out[0], out[1], out[2]});
                        }
                    }
                }
                INFO(hdrToneName(tone) << ": highest " << highest << " ceiling " << ceiling);
                CHECK(highest <= ceiling);
                CHECK(highest >= ceiling - 1e-6f);  // and the clip, boosted, reaches it
                CHECK(hdrPeakNitsOf(p) == p.hdrToneCapNits);
            }
        }
    }
    // HLG has no absolute peak to report.
    CHECK(hdrPeakNitsOf(toneBlock(OutputTransfer::HLG, HdrTone::Aces2Bright)) == 0.0f);
}

TEST_CASE("luminance mode keeps the scene's chromaticity, per channel adds chroma", "[color][hdrtone]") {
    const OsvColorParams natural = toneBlock(OutputTransfer::PQ, HdrTone::Bt2408Natural);
    const OsvColorParams punchy = toneBlock(OutputTransfer::PQ, HdrTone::Bt2408Punchy);
    // A mid-saturation sky blue and a skin tone, well under the ceiling.
    for (const std::array<float, 3> lin : {std::array<float, 3>{0.10f, 0.16f, 0.30f},
                                           std::array<float, 3>{0.30f, 0.20f, 0.15f}}) {
        // The working value the tone stage sees (camera matrix, no exposure).
        float working[3];
        osvMat3Apply(&natural.nativeToWorking, lin[0], lin[1], lin[2], working);
        const auto n = fromLinear(natural, lin[0], lin[1], lin[2]);
        const auto c = fromLinear(punchy, lin[0], lin[1], lin[2]);
        // Natural: display light in the scene's own ratios.
        const double r0 = pqNits(n[0]) / pqNits(n[1]);
        const double r2 = pqNits(n[2]) / pqNits(n[1]);
        CHECK_THAT(r0, WithinRel(static_cast<double>(working[0] / working[1]), 1e-3));
        CHECK_THAT(r2, WithinRel(static_cast<double>(working[2] / working[1]), 1e-3));
        // Punchy spreads the channels further apart than the scene does.
        const double spreadNatural = std::fabs(std::log(pqNits(n[0]) / pqNits(n[2])));
        const double spreadPunchy = std::fabs(std::log(pqNits(c[0]) / pqNits(c[2])));
        CHECK(spreadPunchy > spreadNatural);
    }
    // On the neutral axis the two are the same curve.
    for (int i = 0; i <= 64; ++i) {
        const float x = 0.18f * std::exp2(-6.0f + 10.0f * static_cast<float>(i) / 64.0f);
        CHECK_THAT(static_cast<double>(fromLinear(natural, x, x, x)[1]),
                   WithinAbs(static_cast<double>(fromLinear(punchy, x, x, x)[1]), 2e-6));
    }
}

// -----------------------------------------------------------------------------
//  Hostile input and corrupt blocks
// -----------------------------------------------------------------------------
TEST_CASE("hostile light and corrupt tone groups stay finite and safe", "[color][hdrtone]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float hostile[] = {nan, inf, -inf, -1.0f, 0.0f, 1e-30f, 1e30f, 3.4e38f, -3.4e38f};
    // Scene light the pipeline can never produce, into every style.
    for (const OutputTransfer t : {OutputTransfer::PQ, OutputTransfer::HLG}) {
        for (const HdrTone tone : kAllTones) {
            const OsvColorParams p = toneBlock(t, tone);
            for (const float a : hostile) {
                for (const float b : {0.18f, nan, -2.0f}) {
                    const auto out = fromLinear(p, a, b, 0.5f);
                    for (const float v : out) {
                        INFO(outputTransferName(t) << " " << hdrToneName(tone) << " in " << a << ", " << b);
                        REQUIRE(std::isfinite(v));
                        REQUIRE(v >= 0.0f);
                        REQUIRE(v <= 1.0f);
                    }
                }
            }
        }
    }
    // The curve itself is finite and never negative for anything, and holds
    // +inf at its own ceiling.
    const OsvColorParams bright = toneBlock(OutputTransfer::PQ, HdrTone::Aces2Bright);
    for (const float x : hostile) {
        const float nits = osvHdrToneCurve(&bright, x);
        INFO("x " << x);
        CHECK(std::isfinite(nits));
        CHECK(nits >= 0.0f);
    }
    CHECK(osvHdrToneCurve(&bright, inf) == osvHdrToneCurve(&bright, 1e30f));
    CHECK(osvHdrToneCurve(&bright, nan) == 0.0f);
    CHECK(osvHdrToneCurve(nullptr, 0.18f) == 0.0f);
    CHECK_FALSE(osvHdrToneActive(nullptr));
    {
        float out[3] = {9.0f, 9.0f, 9.0f};
        const float w[3] = {0.18f, 0.18f, 0.18f};
        osvHdrToneApply(nullptr, w, out);
        CHECK((out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f));
    }

    // A corrupt group is not a style: the kernel renders it as Neutral, bit
    // for bit, and colorParamsValid refuses the block.
    const OsvColorParams neutral = toneBlock(OutputTransfer::PQ, HdrTone::Bt2408Neutral);
    const auto corrupt = [&bright](auto&& damage) {
        OsvColorParams p = bright;
        damage(p);
        return p;
    };
    const OsvColorParams broken[] = {
        corrupt([](OsvColorParams& p) { p.hdrToneMode = 7; }),
        corrupt([](OsvColorParams& p) { p.hdrToneMode = -1; }),
        corrupt([nan](OsvColorParams& p) { p.hdrToneM2 = nan; }),
        corrupt([](OsvColorParams& p) { p.hdrToneS2 = 0.0f; }),
        corrupt([](OsvColorParams& p) { p.hdrToneG = -1.1f; }),
        corrupt([](OsvColorParams& p) { p.hdrToneT1 = -0.01f; }),
        corrupt([](OsvColorParams& p) { p.hdrToneCapNits = -600.0f; }),
        corrupt([inf](OsvColorParams& p) { p.hdrToneCapNits = inf; }),
        corrupt([](OsvColorParams& p) { p.inputEncoding = OSV_INPUT_HLG; }),
    };
    for (std::size_t k = 0; k < std::size(broken); ++k) {
        const OsvColorParams& p = broken[k];
        INFO("corruption " << k);
        CHECK_FALSE(colorParamsValid(p));
        if (p.inputEncoding != OSV_INPUT_DLOGM || !(p.hdrToneCapNits < inf)) {
            continue;  // those render through other paths or are only refused
        }
        CHECK_FALSE(osvHdrToneActive(&p));
        for (int i = 0; i <= 32; ++i) {
            const float c = static_cast<float>(i) / 32.0f;
            const float in[3] = {c, 0.7f * c, 0.4f};
            float got[3];
            float want[3];
            osvCodeToOutput(&p, in, got);
            osvCodeToOutput(&neutral, in, want);
            for (int ch = 0; ch < 3; ++ch) {
                REQUIRE(sameBits(got[ch], want[ch]));
            }
        }
    }
    // A cap above the block's own mastering peak is refused too.
    CHECK_FALSE(colorParamsValid(corrupt([](OsvColorParams& p) { p.hdrToneCapNits = 4000.0f; })));
    // A tone group on a transfer that has none is refused.
    OsvColorParams sdr = toneBlock(OutputTransfer::Rec709, HdrTone::Bt2408Neutral);
    sdr.hdrToneMode = bright.hdrToneMode;
    sdr.hdrToneM2 = bright.hdrToneM2;
    sdr.hdrToneS2 = bright.hdrToneS2;
    sdr.hdrToneG = bright.hdrToneG;
    sdr.hdrToneT1 = bright.hdrToneT1;
    sdr.hdrToneCapNits = bright.hdrToneCapNits;
    CHECK_FALSE(colorParamsValid(sdr));
    CHECK_FALSE(osvHdrToneActive(&sdr));
    // A Neutral group with stray constants is refused (zeroed means zeroed).
    OsvColorParams stray = neutral;
    stray.hdrToneS2 = 1.0f;
    CHECK_FALSE(colorParamsValid(stray));
}

// -----------------------------------------------------------------------------
//  The HDR peak after every style
// -----------------------------------------------------------------------------
TEST_CASE("the HDR peak roll-off applies after every style exactly as before", "[color][hdrtone][hdrpeak]") {
    for (const HdrTone tone : kAllTones) {
        for (const float target : {600.0f, 400.0f, 203.0f}) {
            const OsvColorParams full = toneBlock(OutputTransfer::PQ, tone, 1.0f);
            const OsvColorParams rolled = toneBlock(OutputTransfer::PQ, tone, 1.0f, target);
            REQUIRE(rolled.hdrPeakNits == target);
            REQUIRE(colorParamsValid(rolled));
            CHECK(hdrPeakNitsOf(rolled) == std::min(target, full.hdrToneCapNits > 0.0f ? full.hdrToneCapNits : 1000.0f));
            // PQ(target), with a float ulp of slack: the roll-off lands on
            // maxLum * PQ(1000) computed in double on the host.
            const float ceiling = osvPqInverseEotf(target) + 1e-6f;
            for (int i = 0; i <= 128; ++i) {
                const float c = static_cast<float>(i) / 128.0f;
                const float in[3] = {c, 0.8f * c, 0.3f + 0.5f * c};
                float a[3];
                float b[3];
                osvCodeToOutput(&full, in, a);
                osvCodeToOutput(&rolled, in, b);
                for (int ch = 0; ch < 3; ++ch) {
                    INFO(hdrToneName(tone) << " target " << target << " code " << c << " ch " << ch);
                    // The same roll-off on the same style's output, bitwise.
                    REQUIRE(sameBits(b[ch], osvHdrPeakRolloff(&rolled, a[ch])));
                    REQUIRE(b[ch] <= ceiling);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
//  The reference renders
// -----------------------------------------------------------------------------
TEST_CASE("each style reproduces its reference render's PQ table", "[color][hdrtone][cube]") {
    // The same evaluation writeCube uses (osvtool lut --tone X), at the same
    // grid points: every spot within 1e-4 PQ code of the float64 original.
    // (On the full 65^3 tables the largest difference is 1.4e-5.)
    const HdrTone kOrder[] = {HdrTone::Aces2Bright, HdrTone::Aces2Detailed, HdrTone::Bt2408Natural,
                              HdrTone::Bt2408Punchy};
    CubeOptions options;
    options.size = 65;
    const float step = 1.0f / 64.0f;
    for (int s = 0; s < 4; ++s) {
        const OsvColorParams p = toneBlock(OutputTransfer::PQ, kOrder[s]);
        double worst = 0.0;
        for (const GoldenSpot& spot : kGoldenSpots) {
            const float in[3] = {static_cast<float>(spot.r) * step, static_cast<float>(spot.g) * step,
                                 static_cast<float>(spot.b) * step};
            float out[3];
            evaluateCubeEntry(p, options, in, out);
            for (int ch = 0; ch < 3; ++ch) {
                worst = std::max(worst, static_cast<double>(std::fabs(out[ch] - spot.out[s][ch])));
            }
        }
        INFO(hdrToneName(kOrder[s]) << ": worst " << worst);
        CHECK(worst < 1e-4);
    }
}
