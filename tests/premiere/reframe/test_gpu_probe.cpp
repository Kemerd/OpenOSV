// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_gpu_probe.cpp - the GPU filter's parameter probe (GpuFilter.cpp
// probeParams) against the host parameter lists Premiere has really shown.
//
// Premiere does not hand a GPU filter one fixed parameter index space.  Three
// have been logged in the field:
//
//   * COMPACT, 8 entries: the group markers dropped and the collapsed
//     "Source" group missing (covered by test_gpu.cpp's 8-parameter tests);
//   * VERBATIM, 15 entries, markers as Bool - Premiere Pro 26.2.2:
//       [0]i32:0 [1]bool:0 [2]i32:3 [3]f32:0.000 [4]f32:0.000 [5]f32:0.000
//       [6]f64:120.000 [7]f64:15.000 [8]bool:0 [9]bool:0 [10]f32:0.000
//     with entries 11..14 not answering at t = 0 on a fresh node.  The probe
//     logged "the host reports 15 parameters and their types do not match"
//     for it and fell back to the static mapping - which happened to be the
//     right one - and retried on every render;
//   * VERBATIM with markers that refuse to answer (an earlier session).  The
//     old probe read only the first 11 entries and its FOV/Distortion anchor
//     then laid the controls down at one constant offset, which is wrong past
//     the first group marker: Source Roll came from Source Pan's entry, and
//     Output Resolution, Source Pan, Source Tilt and Smooth Keyframes were
//     never read.
//
// The observable proof that the probe identified a list at CreateInstance is
// that Render never probes again: a Render that reads any parameter at t = 0
// (with smoothing off, every control is read at the render time only) is
// re-running a probe that failed.  The mock host records every GetParam, so
// that is checked directly; the rendered frame is checked against a CPU
// render of the settings the list describes.

#include "GpuTestSupport.h"

#include "PrSDKVideoSegmentSuite.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using osv::premiere::mock::MockHost;
using osv::premiere::mock::ParamReadRecord;

namespace {

/// The node and timeline every case uses.  No sequence is configured, so
/// "Match Sequence" resolves to the frame itself on both the GPU and the CPU
/// side (resolveOutputSize's documented fallback).
constexpr csSDK_int32 kNode = 5150;
constexpr PrTimelineID kTimeline = 0x5151;

/// The render time: frame 10 of a 30 fps sequence, clear of t = 0 so a probe
/// re-run (which reads at t = 0) cannot hide among the render's own reads.
constexpr PrTime kFrame30 = osv::premiere::mock::kTicksPerSecond / 30;
constexpr PrTime kRenderTime = 10 * kFrame30;

/// The labelled panorama the renders sample.
[[nodiscard]] const Panorama& panorama() {
    static const Panorama p = makePanorama(1024, 512);
    return p;
}

/// The host index of every group marker (ReframeParams.h: an AE index that
/// is not a value control).
[[nodiscard]] std::vector<int> markerHostIndices() {
    std::vector<int> out;
    for (int ae = 1; ae <= kParamCount; ++ae) {
        bool isValue = false;
        for (int i = 0; i < kValueParamCount; ++i) {
            isValue = isValue || kValueParamAeIndex[i] == ae;
        }
        if (!isValue) {
            out.push_back(gpuParamIndex(ae));
        }
    }
    return out;
}

/// Reads made at exactly t = 0.
[[nodiscard]] std::size_t readsAtZero(const std::vector<ParamReadRecord>& reads) {
    std::size_t n = 0;
    for (const ParamReadRecord& r : reads) {
        n += (r.nodeId == kNode && r.time == 0) ? 1u : 0u;
    }
    return n;
}

/// PSNR of a GPU render against the CPU render of `s` from the same panorama.
[[nodiscard]] double psnrAgainstCpu(const std::vector<float>& gpu, const Settings& s, int w, int h) {
    const Panorama& p = panorama();
    const std::vector<std::uint8_t> srcBytes = packBgra32f(p, p.width * 16);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = p.width * 16;
    src.width = p.width;
    src.height = p.height;
    src.layout = PixelLayout::Bgra32f;
    src.topDown = true;
    const KernelSetup setup = buildParams(s, src, w, h, SizePx{});
    if (!setup.valid) {
        return 0.0;
    }
    std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u, 0u);
    FrameView dst;
    dst.base = cpuBytes.data();
    dst.rowBytes = w * 16;
    dst.width = w;
    dst.height = h;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    if (!renderCpu(setup, src, dst, nullptr)) {
        return 0.0;
    }
    std::vector<float> cpu(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            readPixelBgra32f(cpuBytes.data(), dst.rowBytes, x, y,
                             cpu.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u);
        }
    }
    return psnr(gpu, cpu);
}

/// The frames and the entry scope every case needs.
struct ProbeFixture {
    MockHost host;
    int w = 640;
    int h = 360;

    ProbeFixture(int width, int height) : w(width), h(height) {}

    /// An input frame holding the panorama, or an invalid one when there is
    /// no device (the caller skips).
    [[nodiscard]] bool upload(GpuFrame& in) {
        const Panorama& p = panorama();
        return in.valid() && in.upload(packBgra32f(p, in.rowBytes()));
    }
};

#define PROBE_REQUIRE_GPU(host)                                                                                \
    do {                                                                                                       \
        if (!(host).gpuAvailable()) {                                                                          \
            SKIP("no CUDA device: " << (host).gpuFailureReason());                                             \
        }                                                                                                      \
    } while (0)

}  // namespace

TEST_CASE("the probe identifies Premiere 26.2.2's verbatim list, markers and unreadable tail included",
          "[reframe][gpu][probe][cuda]") {
    ProbeFixture f(640, 360);
    PROBE_REQUIRE_GPU(f.host);

    // ---- the host, exactly as its dump read -------------------------------------
    Controls c;
    c.resolution = 0;  // [0]i32:0
    c.preset = 3;      // [2]i32:3
    c.fov = 120.0;     // [6]f64:120
    c.distortion = 15.0;  // [7]f64:15
    // The trailing entries did not answer at t = 0, so the dump cannot say
    // what they hold; distinctive values prove they are read at render time.
    c.sourceTilt = 20.0;   // host index 11
    c.sourceRoll = -10.0;  // host index 12
    writeVerbatimControls(f.host, kNode, c);
    for (int hostIndex = 11; hostIndex < kParamCount; ++hostIndex) {
        f.host.setParamReadError(kNode, hostIndex, suiteError_Fail, PrTime{0});
    }

    const std::uintmax_t logStart = reframeLogSize();
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));

    f.host.clearParamReads();
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    // The probe looked at the whole list, the tail included.
    bool readLast = false;
    for (const ParamReadRecord& r : f.host.paramReads()) {
        readLast = readLast || (r.nodeId == kNode && r.time == 0 && r.index == kParamCount - 1);
    }
    CHECK(readLast);

    // ---- identified: Render never probes again ------------------------------------
    f.host.clearParamReads();
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    CHECK(readsAtZero(f.host.paramReads()) == 0u);

    // ---- and every control, the unreadable-at-t=0 ones too, is read right ----------
    const std::vector<float> gpu = out.downloadRgba();
    REQUIRE(!gpu.empty());
    const double db = psnrAgainstCpu(gpu, settingsOf(c), f.w, f.h);
    INFO("GPU vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);

    // ---- no false error ----------------------------------------------------------
    // (The outcome lines are logged once per process, so they are only there
    // when this is the first probe of the process - as it is under ctest.)
    for (const std::string& line : reframeLogLinesSince(logStart)) {
        CHECK(line.find("do not match") == std::string::npos);
        if (line.find("probed the host parameter mapping") != std::string::npos) {
            CHECK(line.find("verbatim") != std::string::npos);
        }
    }
    CHECK(instance.dispose() == suiteError_NoError);
}

TEST_CASE("a verbatim list whose group markers refuse to answer maps every control to its own entry",
          "[reframe][gpu][probe][cuda]") {
    // 640 x 480 against a 1920 x 1080 Output Resolution, so the resolution
    // control changes the framing (a cover-fit) and a probe that did not map
    // it would render a different picture.
    ProbeFixture f(640, 480);
    PROBE_REQUIRE_GPU(f.host);

    Controls c;
    c.resolution = static_cast<int>(Resolution::Fhd1920x1080);
    c.preset = static_cast<int>(Preset::Custom);
    c.pan = 40.0;
    c.tilt = -12.0;
    c.roll = 6.0;
    c.fov = 95.0;
    c.distortion = 25.0;
    c.sourcePan = 35.0;
    c.sourceTilt = -18.0;
    c.sourceRoll = 11.0;
    c.smooth = true;
    writeVerbatimControls(f.host, kNode, c);
    // Pan keyframed around the render time, so Smooth Keyframes has an
    // average to take: (10 + 40 + 100) / 3 = 50.
    {
        PrParam p{};
        p.mType = kPrParamType_Float32;
        p.mFloat32 = 10.0f;
        f.host.setParam(kNode, gpuParamIndex(kIndexPan), kRenderTime - kFrame30, p);
        p.mFloat32 = 40.0f;
        f.host.setParam(kNode, gpuParamIndex(kIndexPan), kRenderTime, p);
        p.mFloat32 = 100.0f;
        f.host.setParam(kNode, gpuParamIndex(kIndexPan), kRenderTime + kFrame30, p);
    }
    // The markers answer nothing at all.
    for (const int hostIndex : markerHostIndices()) {
        f.host.setParamReadError(kNode, hostIndex, suiteError_InvalidParms);
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    f.host.clearParamReads();
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    // Smoothing reads t - 1 frame = 9 frames, never 0: any t = 0 read is a
    // probe re-run.
    CHECK(readsAtZero(f.host.paramReads()) == 0u);

    const std::vector<float> gpu = out.downloadRgba();
    REQUIRE(!gpu.empty());
    Settings expected = settingsOf(c);
    expected.panDeg = (10.0 + 40.0 + 100.0) / 3.0;
    const double db = psnrAgainstCpu(gpu, expected, f.w, f.h);
    INFO("GPU vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);

    // The same frame WITHOUT the Source angles and the smoothing is a
    // different picture - the controls the old probe dropped are visible.
    Settings dropped = expected;
    dropped.sourcePanDeg = 0.0;
    dropped.sourceTiltDeg = 0.0;
    dropped.sourceRollDeg = 0.0;
    CHECK(psnrAgainstCpu(gpu, dropped, f.w, f.h) < 40.0);
    CHECK(instance.dispose() == suiteError_NoError);
}

TEST_CASE("a 15-entry list that contradicts the signature keeps the static mapping and is probed again",
          "[reframe][gpu][probe][cuda]") {
    ProbeFixture f(320, 180);
    PROBE_REQUIRE_GPU(f.host);

    // FOV answering as an integer: not this effect's list.
    writeVerbatimControls(f.host, kNode, Controls{});
    PrParam p{};
    p.mType = kPrParamType_Int32;
    p.mInt32 = 90;
    f.host.setParam(kNode, gpuParamIndex(kIndexFov), 0, p);

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    f.host.clearParamReads();
    // Still renders (with the static mapping and FOV's default)...
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    // ...and, unidentified, tries again on the next frame.
    CHECK(readsAtZero(f.host.paramReads()) > 0u);
    CHECK(instance.dispose() == suiteError_NoError);
}

TEST_CASE("a list unreadable at CreateInstance is identified by the first Render that can read it",
          "[reframe][gpu][probe][cuda]") {
    ProbeFixture f(320, 180);
    PROBE_REQUIRE_GPU(f.host);

    writeVerbatimControls(f.host, kNode, Controls{});
    // A node Premiere has only just built: nothing answers yet.
    for (int hostIndex = 0; hostIndex < kParamCount; ++hostIndex) {
        f.host.setParamReadError(kNode, hostIndex, suiteError_Fail);
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    // The node is ready by the time a frame is rendered.
    for (int hostIndex = 0; hostIndex < kParamCount; ++hostIndex) {
        f.host.setParamReadError(kNode, hostIndex, suiteError_NoError);
    }
    f.host.clearParamReads();
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    CHECK(readsAtZero(f.host.paramReads()) > 0u);  // the retry

    f.host.clearParamReads();
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    CHECK(readsAtZero(f.host.paramReads()) == 0u);  // identified by it
    CHECK(instance.dispose() == suiteError_NoError);
}

TEST_CASE("the compact 8-entry list is still identified at CreateInstance", "[reframe][gpu][probe][cuda]") {
    ProbeFixture f(320, 180);
    PROBE_REQUIRE_GPU(f.host);

    // Premiere 26.2's first layout: no markers, no Source group.
    auto put = [&](int index, int type, double value) {
        PrParam p{};
        p.mType = static_cast<PrParamType>(type);
        switch (type) {
            case kPrParamType_Int32: p.mInt32 = static_cast<int>(value); break;
            case kPrParamType_Float32: p.mFloat32 = static_cast<float>(value); break;
            case kPrParamType_Float64: p.mFloat64 = value; break;
            default: p.mBool = value != 0.0 ? 1 : 0; break;
        }
        f.host.setParam(kNode, index, 0, p);
    };
    put(0, kPrParamType_Int32, 1);
    put(1, kPrParamType_Int32, 1);
    put(2, kPrParamType_Float32, 10.0);
    put(3, kPrParamType_Float32, 0.0);
    put(4, kPrParamType_Float32, 0.0);
    put(5, kPrParamType_Float64, 90.0);
    put(6, kPrParamType_Float64, 0.0);
    put(7, kPrParamType_Bool, 0.0);

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    f.host.clearParamReads();
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    CHECK(readsAtZero(f.host.paramReads()) == 0u);
    CHECK(instance.dispose() == suiteError_NoError);
}

TEST_CASE("a host that reports no parameters keeps the static mapping without failing", "[reframe][gpu][probe][cuda]") {
    ProbeFixture f(320, 180);
    PROBE_REQUIRE_GPU(f.host);
    writeVerbatimControls(f.host, kNode, Controls{});
    f.host.setParamCount(kNode, 0);

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(f.host, panorama().width, panorama().height, false);
    GpuFrame out(f.host, f.w, f.h, false);
    REQUIRE(f.upload(in));
    FilterInstance instance(scope, f.host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);
    // The static mapping IS the verbatim layout, so the frame is still right.
    REQUIRE(instance.render(in, out, kRenderTime, kFrame30) == suiteError_NoError);
    const std::vector<float> gpu = out.downloadRgba();
    REQUIRE(!gpu.empty());
    CHECK(psnrAgainstCpu(gpu, settingsOf(Controls{}), f.w, f.h) >= 60.0);
    CHECK(instance.dispose() == suiteError_NoError);
}
