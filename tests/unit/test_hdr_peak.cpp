// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// [WP-HDRPEAK] Tests for the PQ output's HDR peak roll-off
// (osvHdrPeakRolloff in ColorMath.h, setHdrPeak in osv/color/ColorParams.h):
//
//   * 1000 nits - the default - is bit for bit the PQ output that existed
//     before the setting, and no other output ever changes;
//   * the curve is the BT.2408-7 Annex 5 reference EETF: exactly the identity
//     at and below the knee, continuous, monotonic, and it reaches the target
//     peak (in nits, through the PQ EOTF) and never passes it;
//   * the documented knees (464 / 251 / 88 nits) and the documented fate of
//     diffuse white hold;
//   * hostile targets and corrupt blocks cannot produce a curve that bends
//     backwards or divides by zero;
//   * on the sample clip's frame 30 the PQ render's brightest pixel stays at
//     or below each target, and every pixel below the knee is untouched;
//   * CUDA and OpenCL render the roll-off like the CPU reference.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/Look.h"
#include "osv/color/Matrices.h"
#include "osv/color/Transfer.h"
#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"
#include "osv/video/DualStreamReader.h"
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
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::color;
using Catch::Matchers::WithinAbs;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// The targets below the source peak, i.e. every choice that rolls off.
constexpr float kRolledTargets[] = {600.0f, 400.0f, 203.0f};

/// A PQ block for the default Osmo 360 fit with a given target peak.
[[nodiscard]] OsvColorParams pqBlock(float hdrPeakNits, float stops = 0.0f) {
    return makeColorParams(DlogMFit::Osmo360, OutputTransfer::PQ, stops, InputEncoding::DLogM, true, 10, nullptr,
                           kBt2408SceneScale, kDefaultLook, hdrPeakNits);
}

/// A D-Log M code triple through the whole pipeline.
[[nodiscard]] std::array<float, 3> through(const OsvColorParams& p, float r, float g, float b) {
    const float in[3] = {r, g, b};
    std::array<float, 3> out{};
    osvCodeToOutput(&p, in, out.data());
    return out;
}

/// The PQ branch of osvLinearToOutput exactly as it read before the HDR peak
/// existed, spelled out step for step from the same primitives, so "1000 is
/// today's output" is checked against the formula itself rather than against
/// the function under test.
void pqAsBefore(const OsvColorParams& p, const float lin[3], float out[3]) {
    float working[3];
    float tmp[3];
    osvMat3Apply(&p.nativeToWorking, lin[0], lin[1], lin[2], working);
    for (int i = 0; i < 3; ++i) {
        working[i] *= p.exposureGain;
    }
    for (int i = 0; i < 3; ++i) {
        working[i] = fmaxf(working[i] * p.sceneScale, 0.0f);
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
}

/// Bitwise float equality (distinguishes nothing NaN-related here: every
/// value compared is a finite PQ code).
[[nodiscard]] bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

}  // namespace

// -----------------------------------------------------------------------------
//  The default is today's output
// -----------------------------------------------------------------------------
TEST_CASE("1000 nits is bit for bit the PQ output before the setting existed", "[color][hdrpeak]") {
    REQUIRE(kDefaultHdrPeakNits == 1000.0f);
    REQUIRE(kDefaultHdrPeakNits == kDefaultPeakNits);

    // The block: the default argument, an explicit 1000 and anything above
    // the source peak all leave the whole group zeroed, i.e. the block is
    // byte-identical to one built before the fields existed.
    const OsvColorParams def = makeColorParams(DlogMFit::Osmo360, OutputTransfer::PQ, 0.0f);
    const OsvColorParams explicit1000 = pqBlock(1000.0f);
    const OsvColorParams above = pqBlock(4000.0f);
    CHECK(def.hdrPeakNits == 0.0f);
    CHECK(def.hdrPeakSrcCode == 0.0f);
    CHECK(def.hdrPeakMaxLum == 0.0f);
    CHECK(def.hdrPeakKnee == 0.0f);
    CHECK(std::memcmp(&def, &explicit1000, sizeof(OsvColorParams)) == 0);
    CHECK(std::memcmp(&def, &above, sizeof(OsvColorParams)) == 0);
    CHECK(colorParamsValid(def));
    CHECK(hdrPeakNitsOf(def) == 1000.0f);

    // The pixels: every code of a dense neutral and coloured sweep, with and
    // without exposure (so light above the 1000-nit master is included),
    // renders bitwise what the pre-setting PQ formula renders.
    for (const float stops : {0.0f, 2.0f, -1.5f}) {
        const OsvColorParams p = pqBlock(1000.0f, stops);
        int compared = 0;
        for (int i = 0; i <= 256; ++i) {
            const float c = static_cast<float>(i) / 256.0f;
            for (const std::array<float, 3> code : {std::array<float, 3>{c, c, c}, std::array<float, 3>{c, 0.5f * c, 0.2f},
                                                    std::array<float, 3>{0.3f, c, 1.0f - c}}) {
                float lin[3];
                osvCodeToLinear(&p, code.data(), lin);
                float now[3];
                float before[3];
                osvLinearToOutput(&p, lin, now);
                pqAsBefore(p, lin, before);
                for (int k = 0; k < 3; ++k) {
                    INFO("stops " << stops << " code " << code[0] << "," << code[1] << "," << code[2]);
                    REQUIRE(sameBits(now[k], before[k]));
                }
                ++compared;
            }
        }
        CHECK(compared == 3 * 257);
    }
}

TEST_CASE("the HDR peak changes the PQ output only", "[color][hdrpeak]") {
    // Every other transfer builds a block byte-identical to its default one,
    // whatever peak is asked for: HLG is display-relative (its display applies
    // its own peak), Rec.709 / linear / passthrough have no HDR highlights.
    for (const OutputTransfer t :
         {OutputTransfer::HLG, OutputTransfer::Rec709, OutputTransfer::Linear, OutputTransfer::Passthrough}) {
        const OsvColorParams base = makeColorParams(DlogMFit::Osmo360, t, 0.0f);
        for (const float target : kRolledTargets) {
            const OsvColorParams asked = makeColorParams(DlogMFit::Osmo360, t, 0.0f, InputEncoding::DLogM, true, 10,
                                                         nullptr, kBt2408SceneScale, kDefaultLook, target);
            INFO("transfer " << outputTransferName(t) << " target " << target);
            CHECK(std::memcmp(&base, &asked, sizeof(OsvColorParams)) == 0);
            CHECK(asked.hdrPeakNits == 0.0f);
        }
        CHECK(hdrPeakNitsOf(base) == 0.0f);  // no absolute peak outside PQ
    }
    // PQ does carry the target, and the block is valid.
    for (const float target : kRolledTargets) {
        const OsvColorParams p = pqBlock(target);
        INFO("target " << target);
        CHECK(p.hdrPeakNits == target);
        CHECK(hdrPeakNitsOf(p) == target);
        CHECK(colorParamsValid(p));
    }
    // setHdrPeak switches an existing block both ways, for PQ only.
    OsvColorParams p = pqBlock(1000.0f);
    setHdrPeak(p, 400.0f);
    CHECK(p.hdrPeakNits == 400.0f);
    setHdrPeak(p, 1000.0f);
    CHECK(p.hdrPeakNits == 0.0f);
    OsvColorParams hlg = makeColorParams(DlogMFit::Osmo360, OutputTransfer::HLG, 0.0f);
    setHdrPeak(hlg, 400.0f);
    CHECK(hlg.hdrPeakNits == 0.0f);
}

// -----------------------------------------------------------------------------
//  The curve
// -----------------------------------------------------------------------------
TEST_CASE("the roll-off is BT.2408 Annex 5's EETF", "[color][hdrpeak]") {
    for (const float target : kRolledTargets) {
        const OsvColorParams p = pqBlock(target);
        INFO("target " << target);
        // Steps 1 and 2 with the source peak at the OOTF's 1000 nits.
        const double src = ref::pqInverseEotf(1000.0);
        const double maxLum = ref::pqInverseEotf(target) / src;
        CHECK_THAT(p.hdrPeakSrcCode, WithinAbs(src, 1e-7));
        CHECK_THAT(p.hdrPeakMaxLum, WithinAbs(maxLum, 1e-7));
        CHECK_THAT(p.hdrPeakKnee, WithinAbs(1.5 * maxLum - 0.5, 1e-7));
        // Against the double-precision reference EETF over the whole code
        // range, and against the existing float osvBt2390Eetf.
        double worst = 0.0;
        for (int i = 0; i <= 4000; ++i) {
            const float code = static_cast<float>(i) / 4000.0f;
            const float got = osvHdrPeakRolloff(&p, code);
            const double want = ref::bt2390Eetf(code, 1000.0, target);
            worst = std::max(worst, std::fabs(static_cast<double>(got) - want));
            CHECK_THAT(got, WithinAbs(osvBt2390Eetf(code, 1000.0f, target), 2e-6));
        }
        INFO("worst difference to the double reference " << worst);
        CHECK(worst < 1e-6);
        // The closed form the Annex's knee makes of the spline:
        // E2 = KS + (maxLum - KS) (1 - (1 - T)^3).
        for (int i = 1; i < 100; ++i) {
            const double t = i / 100.0;
            const double ks = p.hdrPeakKnee;
            const double e1 = ks + t * (1.0 - ks);
            const float got = osvHdrPeakRolloff(&p, static_cast<float>(e1 * p.hdrPeakSrcCode));
            const double closed = (ks + (maxLum - ks) * (1.0 - std::pow(1.0 - t, 3.0))) * src;
            CHECK_THAT(got, WithinAbs(closed, 2e-6));
        }
    }
}

TEST_CASE("the roll-off is the identity below the knee, bit for bit", "[color][hdrpeak]") {
    for (const float target : kRolledTargets) {
        const OsvColorParams p = pqBlock(target);
        const float kneeCode = p.hdrPeakKnee * p.hdrPeakSrcCode;
        INFO("target " << target << " knee code " << kneeCode);
        // Every representable-ish code up to the knee comes back unchanged.
        for (int i = 0; i <= 20000; ++i) {
            const float code = kneeCode * static_cast<float>(i) / 20000.0f;
            if (code / p.hdrPeakSrcCode > p.hdrPeakKnee) {
                continue;  // the last step may round a hair past the knee
            }
            REQUIRE(sameBits(osvHdrPeakRolloff(&p, code), code));
        }
        // So 18 % grey (26 nits) never moves, whatever the target.
        const float grey = osvPqInverseEotf(26.0f);
        CHECK(sameBits(osvHdrPeakRolloff(&p, grey), grey));
    }
    // Diffuse white (203 nits) is below the 600 and 400 knees: untouched.
    const float white = osvPqInverseEotf(203.0f);
    const OsvColorParams p600 = pqBlock(600.0f);
    const OsvColorParams p400 = pqBlock(400.0f);
    const OsvColorParams p203 = pqBlock(203.0f);
    CHECK(sameBits(osvHdrPeakRolloff(&p600, white), white));
    CHECK(sameBits(osvHdrPeakRolloff(&p400, white), white));
    // At 203 ("SDR-safe") the knee is 88 nits, so diffuse white rolls to 159.
    const float sdrWhite = osvPqEotf(osvHdrPeakRolloff(&p203, white));
    CHECK_THAT(sdrWhite, WithinAbs(159.0, 0.5));
}

TEST_CASE("the roll-off is continuous, monotonic and caps at the target in nits", "[color][hdrpeak]") {
    for (const float target : kRolledTargets) {
        const OsvColorParams p = pqBlock(target);
        const float srcCode = p.hdrPeakSrcCode;
        INFO("target " << target);
        // A dense sweep across the whole code range, past the source peak.
        const int n = 200000;
        float previous = osvHdrPeakRolloff(&p, 0.0f);
        float largestStep = 0.0f;
        for (int i = 1; i <= n; ++i) {
            const float code = static_cast<float>(i) / static_cast<float>(n);
            const float out = osvHdrPeakRolloff(&p, code);
            // Monotonic: never falls (flat at the target above the source
            // peak).
            REQUIRE(out >= previous);
            // Continuous: the output never jumps further than the input
            // stepped (the slope is 1 below the knee and (1 - T)^2 above).
            largestStep = std::max(largestStep, out - previous);
            // Capped: never above the target, in nits.
            REQUIRE(osvPqEotf(out) <= target * 1.0001f);
            previous = out;
        }
        CHECK(largestStep <= 1.0f / static_cast<float>(n) + 1e-6f);
        // Strictly rising all the way up to the source peak, on a sweep
        // coarse enough for float to resolve the flattening shoulder.
        float last = -1.0f;
        for (int i = 0; i <= 990; ++i) {
            const float code = srcCode * static_cast<float>(i) / 1000.0f;
            const float out = osvHdrPeakRolloff(&p, code);
            REQUIRE(out > last);
            last = out;
        }
        // The knee joins with no step: just below and just above it agree.
        const float kneeCode = p.hdrPeakKnee * srcCode;
        CHECK_THAT(osvHdrPeakRolloff(&p, kneeCode * 1.000001f), WithinAbs(kneeCode, 1e-6));
        // The source peak (1000 nits) lands exactly on the target, and so
        // does anything brighter.
        CHECK_THAT(osvPqEotf(osvHdrPeakRolloff(&p, srcCode)), WithinAbs(target, target * 1e-4));
        CHECK_THAT(osvPqEotf(osvHdrPeakRolloff(&p, 1.0f)), WithinAbs(target, target * 1e-4));
    }
}

TEST_CASE("the documented knees are where the roll-off starts", "[color][hdrpeak]") {
    // docs/COLOR.md: 600 -> 464 nits, 400 -> 251 nits, 203 -> 88 nits.
    CHECK_THAT(hdrPeakKneeNits(600.0f), WithinAbs(464.0, 1.0));
    CHECK_THAT(hdrPeakKneeNits(400.0f), WithinAbs(250.9, 1.0));
    CHECK_THAT(hdrPeakKneeNits(203.0f), WithinAbs(87.8, 1.0));
    CHECK(hdrPeakKneeNits(1000.0f) == 1000.0f);  // no roll-off
    CHECK(hdrPeakKneeNits(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(hdrPeakKneeNits(-5.0f) == 0.0f);
    // And the kernel agrees with the query: just below the knee untouched,
    // just above it compressed.
    for (const float target : kRolledTargets) {
        const OsvColorParams p = pqBlock(target);
        const float knee = hdrPeakKneeNits(target);
        const float below = osvPqInverseEotf(knee * 0.99f);
        const float over = osvPqInverseEotf(std::min(knee * 1.2f, 999.0f));
        INFO("target " << target << " knee " << knee);
        CHECK(sameBits(osvHdrPeakRolloff(&p, below), below));
        CHECK(osvHdrPeakRolloff(&p, over) < over);
    }
}

TEST_CASE("hostile targets and corrupt blocks never bend the curve", "[color][hdrpeak]") {
    // Garbage targets - NaN, infinities, zero or negative light - are the
    // default (no roll-off); a positive target below 100 nits is raised to
    // 100.
    for (const float garbage : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(), 0.0f, -400.0f}) {
        INFO("target " << garbage);
        CHECK(pqBlock(garbage).hdrPeakNits == 0.0f);
    }
    CHECK(pqBlock(50.0f).hdrPeakNits == kMinHdrPeakNits);
    CHECK(colorParamsValid(pqBlock(50.0f)));
    // At the lowest target the knee sits just above grey (27 nits).
    CHECK_THAT(hdrPeakKneeNits(kMinHdrPeakNits), WithinAbs(27.7, 1.0));

    // A block whose derived constants are nonsense is left alone by the
    // kernel and refused by the validator.
    const float code = osvPqInverseEotf(800.0f);
    const OsvColorParams good = pqBlock(400.0f);
    const auto corrupt = [&good](auto edit) {
        OsvColorParams p = good;
        edit(p);
        return p;
    };
    const OsvColorParams broken[] = {
        corrupt([](OsvColorParams& p) { p.hdrPeakMaxLum = 1.5f; }),
        corrupt([](OsvColorParams& p) { p.hdrPeakMaxLum = 0.0f; }),
        corrupt([](OsvColorParams& p) { p.hdrPeakSrcCode = 0.0f; }),
        corrupt([](OsvColorParams& p) { p.hdrPeakKnee = 0.99f; }),
        corrupt([](OsvColorParams& p) { p.hdrPeakMaxLum = std::numeric_limits<float>::quiet_NaN(); }),
    };
    for (const OsvColorParams& p : broken) {
        CHECK(sameBits(osvHdrPeakRolloff(&p, code), code));
        CHECK_FALSE(colorParamsValid(p));
    }
    // A roll-off group on a non-PQ block is refused by the validator, and a
    // null block passes the code through.
    OsvColorParams hlg = makeColorParams(DlogMFit::Osmo360, OutputTransfer::HLG, 0.0f);
    hlg.hdrPeakNits = 400.0f;
    CHECK_FALSE(colorParamsValid(hlg));
    CHECK(sameBits(osvHdrPeakRolloff(nullptr, code), code));
    // The pipeline stays finite and in [0, 1] for hostile light.
    const OsvColorParams p = pqBlock(203.0f);
    for (const float v : {-1e6f, -1.0f, 0.0f, 1e-30f, 1.0f, 50.0f, 1e6f, std::numeric_limits<float>::infinity()}) {
        const float lin[3] = {v, v * 0.5f, 1.0f};
        float out[3];
        osvLinearToOutput(&p, lin, out);
        for (const float o : out) {
            INFO("light " << v);
            CHECK(std::isfinite(o));
            CHECK(o >= 0.0f);
            CHECK(osvPqEotf(o) <= 203.0f * 1.0001f);
        }
    }
}

TEST_CASE("HDR peak names parse", "[color][hdrpeak]") {
    float nits = -1.0f;
    for (const float choice : kHdrPeakChoicesNits) {
        REQUIRE(parseHdrPeak(std::to_string(static_cast<int>(choice)), nits));
        CHECK(nits == choice);
    }
    REQUIRE(parseHdrPeak("600 nits", nits));
    CHECK(nits == 600.0f);
    REQUIRE(parseHdrPeak("400NITS", nits));
    CHECK(nits == 400.0f);
    REQUIRE(parseHdrPeak("SDR", nits));
    CHECK(nits == 203.0f);
    nits = 1.0f;
    for (const char* bad : {"", "800", "0", "-600", "600.5", "nits", "hdr", "1000000"}) {
        INFO("'" << bad << "'");
        CHECK_FALSE(parseHdrPeak(bad, nits));
        CHECK(nits == 1.0f);  // untouched on failure
    }
}

TEST_CASE("the roll-off acts per component, so bright colours soften towards white", "[color][hdrpeak]") {
    // A saturated highlight well above the knee: each component is rolled
    // off on its own, so the brightest channel is compressed hardest and the
    // colour loses some saturation - the camera-knee behaviour BT.2408 notes
    // for R'G'B' application - while its hue order is kept.
    const OsvColorParams full = pqBlock(1000.0f, 1.0f);
    const OsvColorParams rolled = pqBlock(400.0f, 1.0f);
    const auto a = through(full, 0.95f, 0.80f, 0.60f);
    const auto b = through(rolled, 0.95f, 0.80f, 0.60f);
    CHECK(b[0] < a[0]);
    // A monotonic curve per component keeps the components' order.
    CHECK((a[0] >= a[1]) == (b[0] >= b[1]));
    CHECK((a[1] >= a[2]) == (b[1] >= b[2]));
    // The spread between the channels shrinks (in display light).
    const double spreadA = osvPqEotf(a[0]) / std::max(osvPqEotf(a[2]), 1e-3f);
    const double spreadB = osvPqEotf(b[0]) / std::max(osvPqEotf(b[2]), 1e-3f);
    CHECK(spreadB < spreadA);
}

// -----------------------------------------------------------------------------
//  The sample clip
// -----------------------------------------------------------------------------
namespace {

/// One decoded frame of the sample with its rig (the calibration the clip
/// records), as test_render.cpp opens it.
struct SampleFrame {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
    video::FramePair pair;
};

Result<SampleFrame> openSampleFrame(std::uint32_t frameIndex) {
    SampleFrame s;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    s.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(s.track, meta::MetadataTrack::load(*s.file));
    OSV_TRY_ASSIGN(s.format, meta::FormatDetector::detect(*s.file, &s.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(s.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(s.format.streamW), static_cast<int>(s.format.streamH),
                                               static_cast<int>(s.format.sensorW), static_cast<int>(s.format.sensorH),
                                               s.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(s.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               s.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleOsv(), s.format));
    OSV_TRY_ASSIGN(s.pair, reader.read(frameIndex));
    return s;
}

/// The largest colour component of a rendered image (the alpha is skipped).
[[nodiscard]] float maxComponent(const render::ImageRGBAf& img) {
    float m = 0.0f;
    for (std::uint32_t y = 0; y < img.h; ++y) {
        const float* row = img.row(y);
        for (std::uint32_t x = 0; x < img.w; ++x) {
            m = std::max({m, row[x * 4], row[x * 4 + 1], row[x * 4 + 2]});
        }
    }
    return m;
}

}  // namespace

TEST_CASE("the sample's brightest pixels stay below each chosen peak", "[color][hdrpeak][sample]") {
    OSV_REQUIRE_SAMPLE();
    // Frame 30: the sunlit white aircraft beside the camera, the sun and its
    // glossy wing - the highlights the setting exists for.
    auto sp = openSampleFrame(30);
    REQUIRE(sp.ok());
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    geom::EquirectMap map;
    map.w = 1024;
    map.h = 512;
    const auto render = [&](float target) {
        auto job = render::RenderParamsBuilder().rig(sp.value().rig).equirect(map).color(pqBlock(target)).build(
            sp.value().pair);
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        return std::move(img).value();
    };

    // The untouched master really has highlights above every knee, or the
    // checks below would prove nothing.
    const render::ImageRGBAf master = render(1000.0f);
    const float masterPeakNits = osvPqEotf(maxComponent(master));
    INFO("1000-nit master's brightest component: " << masterPeakNits << " nits");
    REQUIRE(masterPeakNits > 600.0f);

    for (const float target : kRolledTargets) {
        const render::ImageRGBAf img = render(target);
        const float peakNits = osvPqEotf(maxComponent(img));
        INFO("target " << target << ": brightest component " << peakNits << " nits");
        CHECK(peakNits <= target * 1.0001f);
        // Everything below the knee - every component of the pixel - is
        // exactly the master's pixel: faces, mid-tones and (for 600 and 400)
        // diffuse white do not move by a single bit.
        const float kneeCode = osvPqInverseEotf(hdrPeakKneeNits(target)) * 0.9999f;
        std::size_t below = 0;
        std::size_t moved = 0;
        for (std::size_t i = 0; i + 3 < master.data.size(); i += 4) {
            if (master.data[i] < kneeCode && master.data[i + 1] < kneeCode && master.data[i + 2] < kneeCode) {
                ++below;
                for (int c = 0; c < 3; ++c) {
                    if (!sameBits(master.data[i + static_cast<std::size_t>(c)], img.data[i + static_cast<std::size_t>(c)])) {
                        ++moved;
                    }
                }
            }
        }
        INFO(below << " pixels below the knee, " << moved << " components moved");
        CHECK(below > 0u);
        CHECK(moved == 0u);
    }
}

// -----------------------------------------------------------------------------
//  Backend parity
// -----------------------------------------------------------------------------
namespace {

/// Owns a synthetic 10-bit 4:2:0 lens frame whose luma ramps across the whole
/// code range and whose chroma sweeps hue and saturation, so one render walks
/// the roll-off through its knee and its shoulder in every colour.
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
    // Luma: narrow-range black to white, left to right.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float t = static_cast<float>(x) / static_cast<float>(w - 1);
            base[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(std::lround(64.0f + t * 876.0f));
        }
    }
    // Chroma: hue around the centre, saturation growing outwards.
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

/// The sample clip's verified calibration as a stream-space rig (the constants
/// test_render.cpp and test_look.cpp use), so parity needs no clip on disk.
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

/// Render the sweep through the 400-nit roll-off (with +1 stop, so a good
/// share of it sits on the shoulder) on `gpu` and on the CPU reference, and
/// hold the GPU to the parity bar the stitch kernels meet.
[[maybe_unused]] void checkPeakParity(render::IRenderer& gpu, const char* label) {
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const int W = 600;
    auto rig = sweepRig(W);
    REQUIRE(rig.ok());
    auto slave = makeSweepFrame(static_cast<std::uint32_t>(W), 0.0f);
    auto master = makeSweepFrame(static_cast<std::uint32_t>(W), 1.3f);
    video::FramePair pair;
    pair.lens = {slave.frame, master.frame};

    const OsvColorParams cp = pqBlock(400.0f, 1.0f);
    REQUIRE(cp.hdrPeakNits == 400.0f);
    geom::EquirectMap map;
    map.w = 1024;
    map.h = 512;
    auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(cp).build(pair);
    REQUIRE(job.ok());
    auto ref = cpu.render(job.value());
    auto test = gpu.render(job.value());
    REQUIRE(ref.ok());
    REQUIRE(test.ok());
    // The roll-off really ran: nothing above the target on either backend.
    CHECK(osvPqEotf(maxComponent(ref.value())) <= 400.0f * 1.0001f);
    CHECK(osvPqEotf(maxComponent(test.value())) <= 400.0f * 1.0001f);
    const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
    INFO(label << ": PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes, within2 "
               << stats.fractionWithin2);
    REQUIRE(stats.psnrDb >= 60.0);
    REQUIRE(stats.fractionWithin2 >= 0.9995);
    REQUIRE(stats.maxAbsCode <= 64);
}

}  // namespace

#if defined(OSV_HAVE_CUDA)
TEST_CASE("CUDA renders the HDR peak roll-off like the CPU reference", "[color][hdrpeak][cuda]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto r = render::CudaRenderer::create(0);
    REQUIRE(r.ok());
    checkPeakParity(*r.value(), "cuda");
}
#endif

#if defined(OSV_HAVE_OPENCL)
TEST_CASE("OpenCL renders the HDR peak roll-off like the CPU reference", "[color][hdrpeak][opencl]") {
    std::string reason;
    if (!render::OpenClRenderer::available(&reason)) {
        SKIP("OpenCL unavailable: " << reason);
    }
    auto r = render::OpenClRenderer::create(0);
    if (!r.ok()) {
        FAIL("OpenCL renderer creation failed: " << r.error().message);
    }
    checkPeakParity(*r.value(), "opencl");
}
#endif
