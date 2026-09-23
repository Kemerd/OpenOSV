// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_gpu.cpp - the xGPUFilterEntry side of Open360Reframe.aex.
//
// The mock GPU Device Suite hands out a REAL CUDA driver-API context, stream
// and device buffers, so these tests exercise the same code path Premiere
// does: cuCtxPushCurrent around the driver calls, cuModuleLoadFatBinary on
// the embedded fatbin, cuModuleGetFunction by name, cuLaunchKernel on the
// host's stream.  Nothing here is simulated except the host itself.
//
// Everything that needs a device is tagged [cuda]; Catch2's SKIP() plus
// SKIP_RETURN_CODE 4 in the CMake makes ctest report "Not Run" rather than a
// failure on a machine with no NVIDIA GPU.

#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "MockHost.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKGPUFilter.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKVideoSegmentSuite.h"

#include <cuda.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using Catch::Approx;
using osv::premiere::mock::MockHost;

namespace {

/// The resolution entry the parity tests render with.  These tests used to
/// select the "Full Frame" aspect; that entry went with the letterbox, and
/// its exact equivalent is "Match Sequence" whenever no sequence size is
/// known - resolveOutputSize() then falls back to the frame and the cover-fit
/// factor is exactly 1.  The GPU fixture's timeline is never given a
/// SequenceConfig (except by the one Match Sequence test, which says so), so
/// GetFrameRect fails and GpuFilter takes that fallback; the CPU mirrors pass
/// SizePx{} to buildParams() for the same answer.
constexpr Resolution kFillFrame = Resolution::MatchSequence;

/// Horizontal angle (deg) from the frame centre to the right-hand edge pixel
/// of the centre row, decoded from the labelled panorama.  With a level
/// rectilinear camera this is exactly atan(dx / focal).
double edgeAngleDeg(const std::vector<std::uint8_t>& bytes, std::int32_t rowBytes, int w, int h) {
    float centre[4];
    float edge[4];
    readPixelBgra32f(bytes.data(), rowBytes, w / 2, h / 2, centre);
    readPixelBgra32f(bytes.data(), rowBytes, w - 1, h / 2, edge);
    double d = std::fmod(Panorama::decodeLongitude(edge) - Panorama::decodeLongitude(centre) + 540.0, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    return std::fabs(d - 180.0);
}

/// atan(dx / focal) in degrees for the right-hand edge pixel of a `w`-wide
/// frame.
double expectedEdgeAngleDeg(int w, double focal) {
    const double dx = (w - 1) + 0.5 - w / 2.0;
    return std::atan(dx / focal) * 180.0 / 3.14159265358979323846;
}

/// Number of pixels whose alpha is not fully opaque.
int countNonOpaque(const std::vector<std::uint8_t>& bytes, std::int32_t rowBytes, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float rgba[4];
            readPixelBgra32f(bytes.data(), rowBytes, x, y, rgba);
            if (!(rgba[3] > 0.999f)) {
                ++n;
            }
        }
    }
    return n;
}

/// The panorama the GPU tests reframe, shared with the CPU comparison.
const Panorama& panorama() {
    static const Panorama p = makePanorama(1024, 512);
    return p;
}

/// RAII around the GPU entry's startup / shutdown pair.
///
/// The module holds a per-device CUDA module cache that startup fills and
/// shutdown releases, so every test that talks to the GPU entry must bracket
/// itself this way - exactly as Premiere does around a session.
class GpuEntryScope {
public:
    explicit GpuEntryScope(MockHost& host) : m_host(host) {
        csSDK_int32 index = 0;
        m_result = LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrTrue,
                                                       host.piSuites(), &m_filter, &m_info);
        m_startedUp = (m_result == suiteError_NoError);
    }
    ~GpuEntryScope() {
        if (m_startedUp) {
            csSDK_int32 index = 0;
            PrGPUFilter filter{};
            PrGPUFilterInfo info{};
            LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrFalse, m_host.piSuites(),
                                                &filter, &info);
        }
    }
    GpuEntryScope(const GpuEntryScope&) = delete;
    GpuEntryScope& operator=(const GpuEntryScope&) = delete;

    [[nodiscard]] prSuiteError result() const noexcept { return m_result; }
    [[nodiscard]] const PrGPUFilter& filter() const noexcept { return m_filter; }
    [[nodiscard]] const PrGPUFilterInfo& info() const noexcept { return m_info; }

private:
    MockHost& m_host;
    PrGPUFilter m_filter{};
    PrGPUFilterInfo m_info{};
    prSuiteError m_result = suiteError_Fail;
    bool m_startedUp = false;
};

/// Skip the rest of a test case when this machine has no usable device.
#define REQUIRE_CUDA_DEVICE(host)                                                                      \
    do {                                                                                               \
        if (!(host).gpuAvailable()) {                                                                  \
            SKIP("no CUDA device: " << (host).gpuFailureReason());                                     \
        }                                                                                              \
    } while (0)

/// The GPU Device Suite, acquired the way the plug-in acquires it (so a test
/// that allocates frames uses the same table the module does).
const PrSDKGPUDeviceSuite* gpuSuite(MockHost& host) {
    const void* raw = nullptr;
    if (host.basicSuite()->AcquireSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion, &raw) != kSPNoError) {
        return nullptr;
    }
    return static_cast<const PrSDKGPUDeviceSuite*>(raw);
}

/// RAII push/pop of the host's CUDA context.
///
/// Premiere guarantees its context is current on the RENDER thread, but the
/// test thread is not a render thread: without this every driver call the
/// test makes itself (cuMemcpy, cuCtxSynchronize) fails with
/// CUDA_ERROR_INVALID_CONTEXT.  The plug-in does the same thing internally,
/// which is why it works either way.
class TestContextScope {
public:
    explicit TestContextScope(const PrSDKGPUDeviceSuite* gpu) {
        if (!gpu || !gpu->GetDeviceInfo) {
            return;
        }
        PrGPUDeviceInfo info{};
        if (gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, 0, &info) != suiteError_NoError) {
            return;
        }
        auto* context = static_cast<CUcontext>(info.outContextHandle);
        if (context && cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            m_pushed = true;
        }
    }
    ~TestContextScope() {
        if (m_pushed) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
    }
    TestContextScope(const TestContextScope&) = delete;
    TestContextScope& operator=(const TestContextScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return m_pushed; }

private:
    bool m_pushed = false;
};

/// Upload a host buffer into a GPU PPix created through the suite.
bool uploadToPPix(const PrSDKGPUDeviceSuite* gpu, PPixHand hand, const std::vector<std::uint8_t>& bytes) {
    void* device = nullptr;
    if (!gpu || gpu->GetGPUPPixData(hand, &device) != suiteError_NoError || !device) {
        return false;
    }
    TestContextScope scope(gpu);
    if (!scope.ok()) {
        return false;
    }
    return cuMemcpyHtoD(reinterpret_cast<CUdeviceptr>(device), bytes.data(), bytes.size()) == CUDA_SUCCESS;
}

/// Download a GPU PPix back into a host buffer.  Synchronises first, because
/// the plug-in launches asynchronously on the host's stream and a real host
/// would sequence its own read the same way.
bool downloadFromPPix(const PrSDKGPUDeviceSuite* gpu, PPixHand hand, std::vector<std::uint8_t>& bytes) {
    void* device = nullptr;
    if (!gpu || gpu->GetGPUPPixData(hand, &device) != suiteError_NoError || !device) {
        return false;
    }
    TestContextScope scope(gpu);
    if (!scope.ok()) {
        return false;
    }
    if (cuCtxSynchronize() != CUDA_SUCCESS) {
        return false;
    }
    return cuMemcpyDtoH(bytes.data(), reinterpret_cast<CUdeviceptr>(device), bytes.size()) == CUDA_SUCCESS;
}

/// A full GPU render harness: the suites, the frames and the instance.
struct GpuRenderFixture {
    MockHost host;
    const PrSDKGPUDeviceSuite* gpu = nullptr;
    PPixHand inFrame = nullptr;
    PPixHand outFrame = nullptr;
    std::int32_t inRowBytes = 0;
    std::int32_t outRowBytes = 0;
    int outW = 0;
    int outH = 0;
    bool isHalf = false;
    csSDK_int32 nodeId = 4242;
    PrTimelineID timelineId = 0x5150;

    GpuRenderFixture(int width, int height, bool half) : outW(width), outH(height), isHalf(half) {}

    /// Allocate the two frames and upload the panorama.  Returns false when
    /// there is no device (the caller then skips).
    bool prepare() {
        gpu = gpuSuite(host);
        if (!gpu || !host.gpuAvailable()) {
            return false;
        }
        const PrPixelFormat format = isHalf ? PrPixelFormat_GPU_BGRA_4444_16f : PrPixelFormat_GPU_BGRA_4444_32f;
        const Panorama& p = panorama();
        if (gpu->CreateGPUPPix(0, format, p.width, p.height, 1, 1, prFieldsNone, &inFrame) != suiteError_NoError) {
            return false;
        }
        if (gpu->CreateGPUPPix(0, format, outW, outH, 1, 1, prFieldsNone, &outFrame) != suiteError_NoError) {
            return false;
        }
        const auto inInfo = host.inspect(inFrame);
        const auto outInfo = host.inspect(outFrame);
        if (!inInfo || !outInfo) {
            return false;
        }
        inRowBytes = inInfo->rowBytes;
        outRowBytes = outInfo->rowBytes;

        const std::vector<std::uint8_t> packed = isHalf ? packBgra16f(p, inRowBytes) : packBgra32f(p, inRowBytes);
        return uploadToPPix(gpu, inFrame, packed);
    }

    ~GpuRenderFixture() {
        // PPixes are disposed through the PPix Suite, which the GPU suite's
        // header points at for GPU frames too.
        const void* raw = nullptr;
        if (host.basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &raw) == kSPNoError && raw) {
            const auto* ppix = static_cast<const PrSDKPPixSuite*>(raw);
            if (ppix->Dispose) {
                if (inFrame) {
                    ppix->Dispose(inFrame);
                }
                if (outFrame) {
                    ppix->Dispose(outFrame);
                }
            }
            host.basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        }
    }
    GpuRenderFixture(const GpuRenderFixture&) = delete;
    GpuRenderFixture& operator=(const GpuRenderFixture&) = delete;

    /// Set one keyframed parameter on the node the GPU instance reads from.
    /// The index is the AE index; the mock stores it the way the Video
    /// Segment Suite serves it, i.e. at index - 1.
    void setFloat32(int aeIndex, float value) {
        PrParam p{};
        p.mType = kPrParamType_Float32;
        p.mFloat32 = value;
        host.setParam(nodeId, gpuParamIndex(aeIndex), 0, p);
    }
    void setFloat64(int aeIndex, double value) {
        PrParam p{};
        p.mType = kPrParamType_Float64;
        p.mFloat64 = value;
        host.setParam(nodeId, gpuParamIndex(aeIndex), 0, p);
    }
    void setInt32(int aeIndex, int value) {
        PrParam p{};
        p.mType = kPrParamType_Int32;
        p.mInt32 = value;
        host.setParam(nodeId, gpuParamIndex(aeIndex), 0, p);
    }
    void setBool(int aeIndex, bool value) {
        PrParam p{};
        p.mType = kPrParamType_Bool;
        p.mBool = value ? 1 : 0;
        host.setParam(nodeId, gpuParamIndex(aeIndex), 0, p);
    }

    /// The Settings the same parameters would produce on the CPU side, for
    /// the parity comparison.
    Settings mirrorSettings(Resolution resolution, double pan, double tilt, double roll, double fov,
                            double distortion) const {
        Settings s;
        s.resolution = resolution;
        s.preset = Preset::Custom;
        s.panDeg = pan;
        s.tiltDeg = tilt;
        s.rollDeg = roll;
        s.fovDeg = fov;
        s.distortion = distortion;
        return s;
    }
};

}  // namespace

// ===========================================================================
//  Startup / shutdown and the match name
// ===========================================================================
TEST_CASE("xGPUFilterEntry startup fills the whole function table", "[reframe][gpu]") {
    MockHost host;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    // All five callbacks must be present: the host calls Render and the two
    // lifetime hooks unconditionally, and GetFrameDependencies / Precompute
    // whenever it wants to ask (they answer NotImplemented).
    CHECK(scope.filter().CreateInstance != nullptr);
    CHECK(scope.filter().DisposeInstance != nullptr);
    CHECK(scope.filter().GetFrameDependencies != nullptr);
    CHECK(scope.filter().Precompute != nullptr);
    CHECK(scope.filter().Render != nullptr);

    CHECK(scope.info().outInterfaceVersion == PrSDKGPUFilterInterfaceVersion2);
}

TEST_CASE("xGPUFilterEntry leaves the match name null so the host binds it to this module's PiPL",
          "[reframe][gpu]") {
    MockHost host;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    // A non-null outMatchName "must be equal to a registered software
    // filter" (PrSDKGPUFilter.h), and Premiere registers an AE-API effect
    // as "AE." + its PiPL match name.  Reporting the bare PiPL string named
    // no registered filter at all, so Premiere loaded the GPU entry and never
    // created an instance - the effect rendered on the CPU in every real
    // session.  NULL means "the software effect of this module's PiPL",
    // which is exactly this effect, whatever prefix the host adds.
    const PrSDKString& name = scope.info().outMatchName;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&name);
    bool allZero = true;
    for (std::size_t i = 0; i < sizeof(PrSDKString); ++i) {
        allZero = allZero && bytes[i] == 0;
    }
    CHECK(allZero);
}

TEST_CASE("xGPUFilterEntry declines a host interface older than 2", "[reframe][gpu]") {
    MockHost host;
    csSDK_int32 index = 0;
    PrGPUFilter filter{};
    PrGPUFilterInfo info{};

    // Version 1 is the CS7 interface; this module implements 2 and says so
    // rather than filling a table the host would misread.
    CHECK(LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion1, &index, kPrTrue, host.piSuites(),
                                              &filter, &info) != suiteError_NoError);
    CHECK(filter.Render == nullptr);
}

TEST_CASE("xGPUFilterEntry declines a non-zero filter index", "[reframe][gpu]") {
    MockHost host;
    csSDK_int32 index = 1;  // the host asking for a second filter
    PrGPUFilter filter{};
    PrGPUFilterInfo info{};

    // This module holds exactly one GPU filter, so it never increments
    // ioIndex and must refuse to answer for index 1.
    CHECK(LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrTrue, host.piSuites(),
                                              &filter, &info) != suiteError_NoError);
}

TEST_CASE("xGPUFilterEntry survives null arguments", "[reframe][gpu]") {
    MockHost host;
    csSDK_int32 index = 0;
    PrGPUFilter filter{};
    PrGPUFilterInfo info{};

    // Defensive: a null out pointer must be an error return, never a fault
    // inside the host's plug-in loader.
    CHECK(LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, nullptr, kPrTrue, host.piSuites(),
                                              &filter, &info) != suiteError_NoError);
    CHECK(LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrTrue, host.piSuites(),
                                              nullptr, &info) != suiteError_NoError);
    index = 0;
    CHECK(LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrTrue, host.piSuites(),
                                              &filter, nullptr) != suiteError_NoError);
}

TEST_CASE("the optional callbacks report NotImplemented", "[reframe][gpu]") {
    MockHost host;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    PrGPUFilterInstance instance{};
    PrGPUFilterRenderParams params{};
    csSDK_int32 queryIndex = 0;
    PrGPUFilterFrameDependency dependency{};

    // The reframe needs only the frame at the current time (the "Smooth
    // Keyframes" average samples PARAMETERS, not pixels), so the host must
    // be told there are no extra dependencies - and that is what
    // suiteError_NotImplemented means here.
    CHECK(scope.filter().GetFrameDependencies(&instance, &params, &queryIndex, &dependency) ==
          suiteError_NotImplemented);
    CHECK(scope.filter().Precompute(&instance, &params, 0, nullptr) == suiteError_NotImplemented);
}

// ===========================================================================
//  CreateInstance / DisposeInstance
// ===========================================================================
TEST_CASE("CreateInstance declines a host with no GPU", "[reframe][gpu]") {
    MockHost host;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    // Hide the GPU Device Suite entirely: that is what a host without
    // acceleration looks like.  The documented answer is an error, which
    // makes Premiere render the node in software.
    host.setSuiteAvailable(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion, false);

    PrGPUFilterInstance instance{};
    instance.piSuites = host.piSuites();
    instance.inDeviceIndex = 0;
    CHECK(scope.filter().CreateInstance(&instance) != suiteError_NoError);
    CHECK(instance.ioPrivatePluginData == nullptr);

    host.setSuiteAvailable(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion, true);
}

TEST_CASE("CreateInstance declines when the Video Segment Suite is missing", "[reframe][gpu]") {
    MockHost host;
    REQUIRE_CUDA_DEVICE(host);
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    // Without GetParam the effect cannot read a single control, so rendering
    // would silently produce the defaults.  Declining sends the frame to the
    // CPU path, which reads the parameters through AE instead.
    for (const int version : {9, 8, 7, 6}) {
        host.setSuiteAvailable(kPrSDKVideoSegmentSuite, version, false);
    }

    PrGPUFilterInstance instance{};
    instance.piSuites = host.piSuites();
    instance.inDeviceIndex = 0;
    CHECK(scope.filter().CreateInstance(&instance) != suiteError_NoError);

    for (const int version : {9, 8, 7, 6}) {
        host.setSuiteAvailable(kPrSDKVideoSegmentSuite, version, true);
    }
}

TEST_CASE("CreateInstance falls back through the Video Segment Suite versions", "[reframe][gpu]") {
    MockHost host;
    REQUIRE_CUDA_DEVICE(host);
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    // Premiere 22 may not have v9.  Only GetParam is used and it is a v4
    // member, so an older table must be accepted rather than refused.
    host.setSuiteAvailable(kPrSDKVideoSegmentSuite, 9, false);
    host.setSuiteAvailable(kPrSDKVideoSegmentSuite, 8, false);

    PrGPUFilterInstance instance{};
    instance.piSuites = host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = 1;
    CHECK(scope.filter().CreateInstance(&instance) == suiteError_NoError);
    CHECK(instance.ioPrivatePluginData != nullptr);
    CHECK(scope.filter().DisposeInstance(&instance) == suiteError_NoError);

    host.setSuiteAvailable(kPrSDKVideoSegmentSuite, 9, true);
    host.setSuiteAvailable(kPrSDKVideoSegmentSuite, 8, true);
}

TEST_CASE("CreateInstance and DisposeInstance are balanced and leak-free", "[reframe][gpu][cuda]") {
    MockHost host;
    REQUIRE_CUDA_DEVICE(host);
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    const int suitesBefore = host.totalSuiteRefs();

    // Many instances in a row: Premiere creates a new one on EVERY parameter
    // change, so a leak of one suite reference or one allocation per
    // instance would be fatal in a keyframing session.
    for (int i = 0; i < 32; ++i) {
        PrGPUFilterInstance instance{};
        instance.piSuites = host.piSuites();
        instance.inDeviceIndex = 0;
        instance.inNodeID = 100 + i;
        instance.inTimelineID = 0x900;
        REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);
        REQUIRE(instance.ioPrivatePluginData != nullptr);
        // The effect promises realtime playback; the host colours the
        // timeline segment from this.
        CHECK(instance.outIsRealtime == kPrTrue);
        REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
        CHECK(instance.ioPrivatePluginData == nullptr);
    }

    CHECK(host.totalSuiteRefs() == suitesBefore);
}

TEST_CASE("DisposeInstance tolerates being called twice or with nothing to dispose", "[reframe][gpu]") {
    MockHost host;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);

    PrGPUFilterInstance instance{};
    instance.piSuites = host.piSuites();
    // Never created: there is nothing to free, which is not an error.
    CHECK(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
    CHECK(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
    CHECK(scope.filter().DisposeInstance(nullptr) != suiteError_NoError);
}

// ===========================================================================
//  Render
// ===========================================================================
TEST_CASE("the GPU render matches the CPU render in 32f", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 640;
    constexpr int kH = 360;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    // The fixture owns its own MockHost; use it for everything so the frames
    // and the instance see the same suites.
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    // The controls, as the host would serve them: angles float32 degrees,
    // sliders float64, popups int32, the checkbox bool.
    const double pan = 42.0;
    const double tilt = -15.0;
    const double roll = 7.0;
    const double fov = 100.0;
    const double distortion = 30.0;
    f.setInt32(kIndexOutputResolution, static_cast<int>(kFillFrame));
    f.setInt32(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat32(kIndexPan, static_cast<float>(pan));
    f.setFloat32(kIndexTilt, static_cast<float>(tilt));
    f.setFloat32(kIndexRoll, static_cast<float>(roll));
    f.setFloat64(kIndexFov, fov);
    f.setFloat64(kIndexDistortion, distortion);
    f.setFloat32(kIndexSourcePan, 0.0f);
    f.setFloat32(kIndexSourceTilt, 0.0f);
    f.setFloat32(kIndexSourceRoll, 0.0f);
    f.setBool(kIndexSmooth, false);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inClipTime = 0;
    renderParams.inSequenceTime = 0;
    renderParams.inQuality = kPrRenderQuality_High;
    renderParams.inDownsampleFactorX = 1.0f;
    renderParams.inDownsampleFactorY = 1.0f;
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderPARNum = 1;
    renderParams.inRenderPARDen = 1;
    renderParams.inRenderFieldType = prFieldsNone;
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;

    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;
    REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);

    // The launch is asynchronous on the host's stream; the host would
    // sequence its own download, so the test does the same by synchronising
    // the context before reading back.
    std::vector<std::uint8_t> gpuBytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
    REQUIRE(downloadFromPPix(f.gpu, f.outFrame, gpuBytes));

    // The CPU reference, from the same source bytes and the same settings.
    const Panorama& p = panorama();
    const std::vector<std::uint8_t> srcBytes = packBgra32f(p, p.width * 16);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = p.width * 16;
    src.width = p.width;
    src.height = p.height;
    src.layout = PixelLayout::Bgra32f;
    src.topDown = true;

    const Settings settings = f.mirrorSettings(kFillFrame, pan, tilt, roll, fov, distortion);
    const KernelSetup setup = buildParams(settings, src, kW, kH, SizePx{});
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst;
    dst.base = cpuBytes.data();
    dst.rowBytes = kW * 16;
    dst.width = kW;
    dst.height = kH;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src, dst, nullptr));

    // Compare as flat float vectors so PSNR is meaningful.
    std::vector<float> gpuSamples;
    std::vector<float> cpuSamples;
    gpuSamples.reserve(static_cast<std::size_t>(kW) * kH * 4u);
    cpuSamples.reserve(static_cast<std::size_t>(kW) * kH * 4u);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            float a[4];
            float b[4];
            readPixelBgra32f(gpuBytes.data(), f.outRowBytes, x, y, a);
            readPixelBgra32f(cpuBytes.data(), dst.rowBytes, x, y, b);
            gpuSamples.insert(gpuSamples.end(), a, a + 4);
            cpuSamples.insert(cpuSamples.end(), b, b + 4);
        }
    }

    const double db = psnr(gpuSamples, cpuSamples);
    INFO("GPU vs CPU PSNR (32f): " << db << " dB");
    // The kernel is compiled with -fmad=false and no fast math, exactly like
    // the CPU build, so the only divergence is the transcendental libraries.
    CHECK(db >= 60.0);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("the GPU render matches the CPU render in 16f", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 512;
    constexpr int kH = 288;
    GpuRenderFixture f(kW, kH, /*half=*/true);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    const double pan = -70.0;
    const double tilt = 25.0;
    f.setInt32(kIndexOutputResolution, static_cast<int>(kFillFrame));
    f.setInt32(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat32(kIndexPan, static_cast<float>(pan));
    f.setFloat32(kIndexTilt, static_cast<float>(tilt));
    f.setFloat32(kIndexRoll, 0.0f);
    f.setFloat64(kIndexFov, 120.0);
    f.setFloat64(kIndexDistortion, 15.0);
    f.setBool(kIndexSmooth, false);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;
    renderParams.inQuality = kPrRenderQuality_High;
    renderParams.inDownsampleFactorX = 1.0f;
    renderParams.inDownsampleFactorY = 1.0f;

    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;
    REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);
    std::vector<std::uint8_t> gpuBytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
    REQUIRE(downloadFromPPix(f.gpu, f.outFrame, gpuBytes));

    // The CPU reference reads the SAME half source, so the comparison
    // isolates the kernel, not the input quantisation.
    const Panorama& p = panorama();
    const std::vector<std::uint8_t> srcBytes = packBgra16f(p, p.width * 8);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = p.width * 8;
    src.width = p.width;
    src.height = p.height;
    src.layout = PixelLayout::Bgra16f;
    src.topDown = true;

    const Settings settings = f.mirrorSettings(kFillFrame, pan, tilt, 0.0, 120.0, 15.0);
    const KernelSetup setup = buildParams(settings, src, kW, kH, SizePx{});
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(kW) * kH * 8u, 0u);
    FrameView dst;
    dst.base = cpuBytes.data();
    dst.rowBytes = kW * 8;
    dst.width = kW;
    dst.height = kH;
    dst.layout = PixelLayout::Bgra16f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src, dst, nullptr));

    std::vector<float> gpuSamples;
    std::vector<float> cpuSamples;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            float a[4];
            float b[4];
            readPixelBgra16f(gpuBytes.data(), f.outRowBytes, x, y, a);
            readPixelBgra16f(cpuBytes.data(), dst.rowBytes, x, y, b);
            gpuSamples.insert(gpuSamples.end(), a, a + 4);
            cpuSamples.insert(cpuSamples.end(), b, b + 4);
        }
    }

    const double db = psnr(gpuSamples, cpuSamples);
    INFO("GPU vs CPU PSNR (16f): " << db << " dB");
    // Half storage costs ~11 bits of mantissa, so the budget is looser than
    // the 32f one; docs/PREMIERE.md sets it at 45 dB.
    CHECK(db >= 45.0);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("the GPU render fills every pixel and cover-fits a fixed resolution", "[reframe][gpu][cuda]") {
    // This replaced "the GPU render writes transparent black outside the
    // letterbox".  The letterbox was removed on purpose - the reframe must
    // always fill the frame - so the same situation (a shape unlike the
    // frame's) must now paint EVERY pixel and crop the overflow.  Checking
    // every pixel's alpha is stricter than the old four sampled bar rows, and
    // it still proves the GPU writes the whole frame rather than relying on
    // the buffer arriving zeroed (a zeroed pixel has alpha 0 and would fail).
    //
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 401;
    constexpr int kH = 401;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    f.setInt32(kIndexOutputResolution, static_cast<int>(Resolution::Qhd2560x1440));
    f.setInt32(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat64(kIndexFov, 90.0);
    f.setFloat64(kIndexDistortion, 0.0);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;
    renderParams.inQuality = kPrRenderQuality_High;

    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;
    REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);
    std::vector<std::uint8_t> gpuBytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
    REQUIRE(downloadFromPPix(f.gpu, f.outFrame, gpuBytes));

    CHECK(countNonOpaque(gpuBytes, f.outRowBytes, kW, kH) == 0);

    // 16:9 on a square frame: the height binds and the sides are cropped by
    // (2560 * 401) / (1440 * 401) = 16/9, so the edge pixel sits at
    // atan(200 / (200.5 * 16/9)) ~ 29.3 degrees instead of ~44.9.
    const double coverFocal = (kW / 2.0) * (2560.0 * kH) / (1440.0 * kW);
    CHECK(edgeAngleDeg(gpuBytes, f.outRowBytes, kW, kH) ==
          Approx(expectedEdgeAngleDeg(kW, coverFocal)).margin(0.5));

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("Render refuses arguments it cannot use", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost (see above).
    GpuRenderFixture f(320, 180, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = 320;
    renderParams.inRenderHeight = 180;
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;
    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;

    // Each of these would be a fault if the code trusted its arguments.
    CHECK(scope.filter().Render(nullptr, &renderParams, inputs, 1, &output) != suiteError_NoError);
    CHECK(scope.filter().Render(&instance, nullptr, inputs, 1, &output) != suiteError_NoError);
    CHECK(scope.filter().Render(&instance, &renderParams, nullptr, 1, &output) != suiteError_NoError);
    CHECK(scope.filter().Render(&instance, &renderParams, inputs, 0, &output) != suiteError_NoError);
    CHECK(scope.filter().Render(&instance, &renderParams, inputs, 1, nullptr) != suiteError_NoError);

    // A null PPix handle inside a valid array.
    const PPixHand nullInputs[1] = {nullptr};
    CHECK(scope.filter().Render(&instance, &renderParams, nullInputs, 1, &output) != suiteError_NoError);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("Render after DisposeInstance is refused rather than executed", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost (see above).
    GpuRenderFixture f(160, 90, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);
    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = 160;
    renderParams.inRenderHeight = 90;
    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;

    // DisposeInstance nulls ioPrivatePluginData, so this must be caught by
    // the null check rather than by dereferencing freed memory.
    CHECK(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) != suiteError_NoError);
}

TEST_CASE("the GPU path reads a keyframed angle at the render time", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 161;
    constexpr int kH = 161;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    const PrTime ticksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;

    f.setInt32(kIndexOutputResolution, static_cast<int>(kFillFrame));
    f.setInt32(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat64(kIndexFov, 90.0);
    f.setFloat64(kIndexDistortion, 0.0);
    f.setBool(kIndexSmooth, false);

    // Two keyframes on Pan: 0 degrees at frame 0, 90 degrees at frame 10.
    PrParam start{};
    start.mType = kPrParamType_Float32;
    start.mFloat32 = 0.0f;
    f.host.setParam(f.nodeId, gpuParamIndex(kIndexPan), 0, start);
    PrParam end{};
    end.mType = kPrParamType_Float32;
    end.mFloat32 = 90.0f;
    f.host.setParam(f.nodeId, gpuParamIndex(kIndexPan), ticksPerFrame * 10, end);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    auto renderAt = [&](PrTime clipTime) {
        PrGPUFilterRenderParams renderParams{};
        renderParams.inClipTime = clipTime;
        renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
        renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
        renderParams.inRenderTicksPerFrame = ticksPerFrame;
        renderParams.inQuality = kPrRenderQuality_High;
        const PPixHand inputs[1] = {f.inFrame};
        PPixHand output = f.outFrame;
        REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
        REQUIRE(downloadFromPPix(f.gpu, f.outFrame, bytes));
        float centre[4];
        readPixelBgra32f(bytes.data(), f.outRowBytes, kW / 2, kH / 2, centre);
        // The label decodes to a longitude; the camera's pan is its negation.
        return -Panorama::decodeLongitude(centre);
    };

    // The mock interpolates float keyframes linearly, which is what a real
    // host does for an angle between two linear keyframes.
    CHECK(std::fabs(renderAt(0) - 0.0) < 1.5);
    CHECK(std::fabs(renderAt(ticksPerFrame * 10) - 90.0) < 1.5);
    CHECK(std::fabs(renderAt(ticksPerFrame * 5) - 45.0) < 1.5);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("the GPU path honours Match Sequence through the Sequence Info Suite", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 400;
    constexpr int kH = 400;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    // On the GPU side the timeline id comes straight from the instance, not
    // from PF_UtilitySuite - a different code path from the CPU test.
    //
    // The sequence used to be 1:2, which showed up as a PILLARBOX.  With the
    // letterbox gone a relatively TALLER sequence cover-fits with a factor of
    // exactly 1 - indistinguishable from the fallback, so the test would pass
    // even if the suite were never asked.  A 2:1 sequence is relatively WIDER
    // on this square frame, which crops the sides by exactly 2 and can only
    // come from the suite's answer.
    osv::premiere::mock::SequenceConfig config = f.host.sequence(f.timelineId);
    prSetRect(&config.frameRect, 0, 0, 2000, 1000);  // a 2:1 sequence
    f.host.setSequence(f.timelineId, config);

    f.setInt32(kIndexOutputResolution, static_cast<int>(Resolution::MatchSequence));
    f.setInt32(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat64(kIndexFov, 90.0);
    f.setFloat64(kIndexDistortion, 0.0);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;
    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;
    REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
    REQUIRE(downloadFromPPix(f.gpu, f.outFrame, bytes));

    // No bars anywhere, and the sides cropped by the 2:1 factor.
    CHECK(countNonOpaque(bytes, f.outRowBytes, kW, kH) == 0);
    const double measured = edgeAngleDeg(bytes, f.outRowBytes, kW, kH);
    CHECK(measured == Approx(expectedEdgeAngleDeg(kW, (kW / 2.0) * 2.0)).margin(0.5));
    // ...and NOT the frame fallback, which would put the edge ~18 degrees
    // further out.
    CHECK(measured < expectedEdgeAngleDeg(kW, kW / 2.0) - 10.0);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}

TEST_CASE("two concurrent instances render independently", "[reframe][gpu][cuda]") {
    // GpuRenderFixture owns the MockHost: only one may exist at a time
    // (the suite functions reach the instance through MockHost::current()).
    constexpr int kW = 161;
    constexpr int kH = 161;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }
    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    // "Separate instances may be called concurrently" (PrSDKGPUFilter.h).
    // Two nodes with different parameters must not see each other's values,
    // which is what a shared module cache could get wrong.
    const csSDK_int32 nodeA = 7001;
    const csSDK_int32 nodeB = 7002;
    for (const csSDK_int32 node : {nodeA, nodeB}) {
        PrParam popup{};
        popup.mType = kPrParamType_Int32;
        popup.mInt32 = static_cast<csSDK_int32>(kFillFrame);
        f.host.setParam(node, gpuParamIndex(kIndexOutputResolution), 0, popup);
        popup.mInt32 = static_cast<csSDK_int32>(Preset::Custom);
        f.host.setParam(node, gpuParamIndex(kIndexPreset), 0, popup);
        PrParam slider{};
        slider.mType = kPrParamType_Float64;
        slider.mFloat64 = 90.0;
        f.host.setParam(node, gpuParamIndex(kIndexFov), 0, slider);
        slider.mFloat64 = 0.0;
        f.host.setParam(node, gpuParamIndex(kIndexDistortion), 0, slider);
    }
    PrParam pan{};
    pan.mType = kPrParamType_Float32;
    pan.mFloat32 = 0.0f;
    f.host.setParam(nodeA, gpuParamIndex(kIndexPan), 0, pan);
    pan.mFloat32 = 120.0f;
    f.host.setParam(nodeB, gpuParamIndex(kIndexPan), 0, pan);

    PrGPUFilterInstance a{};
    a.piSuites = f.host.piSuites();
    a.inDeviceIndex = 0;
    a.inNodeID = nodeA;
    a.inTimelineID = f.timelineId;
    PrGPUFilterInstance b = a;
    b.inNodeID = nodeB;
    REQUIRE(scope.filter().CreateInstance(&a) == suiteError_NoError);
    REQUIRE(scope.filter().CreateInstance(&b) == suiteError_NoError);
    // Two live instances must be two different objects.
    CHECK(a.ioPrivatePluginData != b.ioPrivatePluginData);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;
    const PPixHand inputs[1] = {f.inFrame};

    auto renderWith = [&](PrGPUFilterInstance& instance) {
        PPixHand output = f.outFrame;
        REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
        REQUIRE(downloadFromPPix(f.gpu, f.outFrame, bytes));
        float centre[4];
        readPixelBgra32f(bytes.data(), f.outRowBytes, kW / 2, kH / 2, centre);
        return -Panorama::decodeLongitude(centre);
    };

    CHECK(std::fabs(renderWith(a) - 0.0) < 1.5);
    CHECK(std::fabs(renderWith(b) - 120.0) < 1.5);

    REQUIRE(scope.filter().DisposeInstance(&a) == suiteError_NoError);
    REQUIRE(scope.filter().DisposeInstance(&b) == suiteError_NoError);
}

// ===========================================================================
//  The Premiere parameter index space, end to end
//
//  Everything above populates the mock at "AE index - 1", which is what the
//  SDK sample does and what this effect assumed.  Real Premiere Pro 26.2
//  does something else: it drops the four group markers AND hides the three
//  controls inside the START_COLLAPSED "Source" group, so GetParamCount
//  answers 8 and every control after the first group sits at a lower index
//  than the static rule expects.
//
//  Under the old mapping FOV was read from host index 6 - which holds
//  Distortion - the value failed buildParams()'s focal-length check and the
//  render bailed out: the effect did nothing at all.  This test builds that
//  exact host and proves the probe recovers the right values.
// ===========================================================================
namespace {

/// Populate a node the way Premiere Pro 26.2 does: eight entries, no group
/// markers, no Source angles.  The values are deliberately distinctive so a
/// misread lands on an obviously wrong number rather than on a plausible one.
struct PremiereParamLayout {
    static constexpr int kResolution = 0;
    static constexpr int kPreset = 1;
    static constexpr int kPan = 2;
    static constexpr int kTilt = 3;
    static constexpr int kRoll = 4;
    static constexpr int kFov = 5;
    static constexpr int kDistortion = 6;
    static constexpr int kSmooth = 7;
    static constexpr int kCount = 8;

    double pan = 42.0;
    double tilt = -15.0;
    double roll = 7.0;
    double fov = 100.0;
    double distortion = 30.0;

    void apply(MockHost& host, csSDK_int32 nodeId) const {
        auto i32 = [&](int index, int value) {
            PrParam p{};
            p.mType = kPrParamType_Int32;
            p.mInt32 = value;
            host.setParam(nodeId, index, 0, p);
        };
        auto f32 = [&](int index, double value) {
            PrParam p{};
            p.mType = kPrParamType_Float32;
            p.mFloat32 = static_cast<float>(value);
            host.setParam(nodeId, index, 0, p);
        };
        auto f64 = [&](int index, double value) {
            PrParam p{};
            p.mType = kPrParamType_Float64;
            p.mFloat64 = value;
            host.setParam(nodeId, index, 0, p);
        };
        auto boolean = [&](int index, bool value) {
            PrParam p{};
            p.mType = kPrParamType_Bool;
            p.mBool = value ? 1 : 0;
            host.setParam(nodeId, index, 0, p);
        };

        i32(kResolution, static_cast<int>(kFillFrame));
        i32(kPreset, static_cast<int>(Preset::Custom));
        f32(kPan, pan);
        f32(kTilt, tilt);
        f32(kRoll, roll);
        f64(kFov, fov);
        f64(kDistortion, distortion);
        boolean(kSmooth, false);
    }
};

}  // namespace

TEST_CASE("the mock host can reproduce Premiere's 8-parameter index space", "[reframe][gpu]") {
    // A cheap guard on the fixture itself: if the mock stopped reporting 8
    // the end-to-end test below would be testing the wrong thing and would
    // still pass, which is the worst kind of green.
    MockHost host;
    const csSDK_int32 nodeId = 7777;
    const PremiereParamLayout layout;
    layout.apply(host, nodeId);

    const void* raw = nullptr;
    REQUIRE(host.basicSuite()->AcquireSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9, &raw) ==
            kSPNoError);
    REQUIRE(raw != nullptr);
    const auto* segment = static_cast<const PrSDKVideoSegmentSuite*>(raw);

    csSDK_int32 count = 0;
    REQUIRE(segment->GetParamCount(nodeId, &count) == suiteError_NoError);
    CHECK(count == PremiereParamLayout::kCount);
    CHECK(count != OSV_REFRAME_PARAM_COUNT);  // the whole point: 8, not 15

    // The type signature the probe identifies the controls by.
    PrParam p{};
    REQUIRE(segment->GetParam(nodeId, PremiereParamLayout::kFov, 0, &p) == suiteError_NoError);
    CHECK(p.mType == kPrParamType_Float64);
    CHECK(p.mFloat64 == Catch::Approx(layout.fov));

    // And the proof that the STATIC rule reads the wrong control here: AE
    // index 7 (FOV) minus one is host index 6, which holds Distortion.
    REQUIRE(segment->GetParam(nodeId, gpuParamIndex(kIndexFov), 0, &p) == suiteError_NoError);
    CHECK(p.mFloat64 == Catch::Approx(layout.distortion));
    CHECK(p.mFloat64 != Catch::Approx(layout.fov));

    host.basicSuite()->ReleaseSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9);
}

TEST_CASE("the GPU path reads the right controls from an 8-parameter host", "[reframe][gpu][cuda]") {
    // THE end-to-end regression test for the blocker.  The host is Premiere's
    // real index space; the rendered frame must match a CPU render built from
    // the settings those eight entries describe.  Before the probe existed
    // this could not even produce a frame: the misread FOV made buildParams()
    // fail and Render() returned an error.
    constexpr int kW = 640;
    constexpr int kH = 360;
    GpuRenderFixture f(kW, kH, /*half=*/false);
    if (!f.host.gpuAvailable()) {
        SKIP("no CUDA device: " << f.host.gpuFailureReason());
    }
    if (!f.prepare()) {
        SKIP("could not allocate GPU frames: " << f.host.gpuFailureReason());
    }

    GpuEntryScope scope(f.host);
    REQUIRE(scope.result() == suiteError_NoError);

    // Premiere's layout, on the node the instance will read from.  Note that
    // NOTHING is written at the AE-minus-one indices the other tests use.
    const PremiereParamLayout layout;
    layout.apply(f.host, f.nodeId);

    PrGPUFilterInstance instance{};
    instance.piSuites = f.host.piSuites();
    instance.inDeviceIndex = 0;
    instance.inNodeID = f.nodeId;
    instance.inTimelineID = f.timelineId;
    REQUIRE(scope.filter().CreateInstance(&instance) == suiteError_NoError);

    PrGPUFilterRenderParams renderParams{};
    renderParams.inClipTime = 0;
    renderParams.inSequenceTime = 0;
    renderParams.inQuality = kPrRenderQuality_High;
    renderParams.inDownsampleFactorX = 1.0f;
    renderParams.inDownsampleFactorY = 1.0f;
    renderParams.inRenderWidth = static_cast<csSDK_uint32>(kW);
    renderParams.inRenderHeight = static_cast<csSDK_uint32>(kH);
    renderParams.inRenderPARNum = 1;
    renderParams.inRenderPARDen = 1;
    renderParams.inRenderFieldType = prFieldsNone;
    renderParams.inRenderTicksPerFrame = osv::premiere::mock::kTicksPerSecond / 30;

    const PPixHand inputs[1] = {f.inFrame};
    PPixHand output = f.outFrame;
    REQUIRE(scope.filter().Render(&instance, &renderParams, inputs, 1, &output) == suiteError_NoError);

    std::vector<std::uint8_t> gpuBytes(static_cast<std::size_t>(f.outRowBytes) * static_cast<std::size_t>(kH), 0u);
    REQUIRE(downloadFromPPix(f.gpu, f.outFrame, gpuBytes));

    // The CPU reference from the settings those eight entries describe.  The
    // three Source angles are zero because the host does not expose them,
    // which is exactly right: a control the user cannot see is a control they
    // cannot have changed.
    const Panorama& p = panorama();
    const std::vector<std::uint8_t> srcBytes = packBgra32f(p, p.width * 16);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = p.width * 16;
    src.width = p.width;
    src.height = p.height;
    src.layout = PixelLayout::Bgra32f;
    src.topDown = true;

    const Settings settings =
        f.mirrorSettings(kFillFrame, layout.pan, layout.tilt, layout.roll, layout.fov, layout.distortion);
    const KernelSetup setup = buildParams(settings, src, kW, kH, SizePx{});
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst;
    dst.base = cpuBytes.data();
    dst.rowBytes = kW * 16;
    dst.width = kW;
    dst.height = kH;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src, dst, nullptr));

    std::vector<float> gpuSamples;
    std::vector<float> cpuSamples;
    gpuSamples.reserve(static_cast<std::size_t>(kW) * kH * 4u);
    cpuSamples.reserve(static_cast<std::size_t>(kW) * kH * 4u);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            float a[4];
            float b[4];
            readPixelBgra32f(gpuBytes.data(), f.outRowBytes, x, y, a);
            readPixelBgra32f(cpuBytes.data(), dst.rowBytes, x, y, b);
            gpuSamples.insert(gpuSamples.end(), a, a + 4);
            cpuSamples.insert(cpuSamples.end(), b, b + 4);
        }
    }

    const double db = psnr(gpuSamples, cpuSamples);
    INFO("8-parameter host, GPU vs CPU PSNR (32f): " << db << " dB");
    CHECK(db >= 60.0);

    // The frame must also be something other than a uniform field: a probe
    // that mapped every control to nothing would still hit the PSNR budget
    // against a CPU render of the same wrong settings, so check the picture
    // actually varies.
    bool varies = false;
    for (std::size_t i = 4; i < gpuSamples.size() && !varies; ++i) {
        varies = (gpuSamples[i] != gpuSamples[i % 4]);
    }
    CHECK(varies);

    REQUIRE(scope.filter().DisposeInstance(&instance) == suiteError_NoError);
}
