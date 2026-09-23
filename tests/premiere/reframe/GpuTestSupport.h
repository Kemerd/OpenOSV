// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuTestSupport.h - driving Open360Reframe.aex's GPU filter the way
// Premiere does, for the end-to-end tests (test_direct_e2e.cpp) and the
// parameter-probe tests (test_gpu_probe.cpp).
//
// Everything goes through the LOADED module's xGPUFilterEntry and through the
// mock host's suites - GPU frames are created with the GPU Device Suite and
// disposed with the PPix Suite, instances are created and disposed through
// the PrGPUFilter table - so what these helpers prove is what Premiere would
// see.  The only thing the test does itself is move pixels in and out of
// device memory, with the host's context pushed around it.
//
// The one piece of Premiere-specific knowledge in here is the VERBATIM
// parameter layout (writeVerbatimControls): the list Premiere Pro 26.2.2 was
// seen to hand a GPU filter - this effect's AE parameter list minus the
// input layer, host index = AE index - 1, with the group markers present as
// Bool entries.
#pragma once

#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "MockHost.h"

#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKGPUFilter.h"
#include "PrSDKPPixSuite.h"

#include <cuda.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace osv::reframe::test {

/// The mock host's namespace, spelled short.
namespace mock = osv::premiere::mock;

/// Premiere ticks per frame at 59.94 fps (254016000000 * 1001 / 60000) - the
/// sample clip's rate.
constexpr PrTime kTicksPerFrame5994 = 4237833600LL;

/// Path of the sample clip: OSV_SAMPLE_FILE from the environment first, the
/// configure-time default (OSV_REFRAME_SAMPLE_CLIP) next, empty otherwise.
[[nodiscard]] std::filesystem::path e2eSampleClipPath();
/// True when that file exists and is not empty.
[[nodiscard]] bool e2eSampleClipAvailable();

// ---------------------------------------------------------------------------
//  The GPU entry and the host context
// ---------------------------------------------------------------------------

/// xGPUFilterEntry startup on construction, shutdown on destruction.  The
/// module keeps a per-device CUDA module cache that shutdown releases, so the
/// scope must end while the host's context is still alive (declare it AFTER
/// the host, or the harness owning it).
class GpuEntryScope {
public:
    explicit GpuEntryScope(mock::MockHost& host);
    ~GpuEntryScope();
    GpuEntryScope(const GpuEntryScope&) = delete;
    GpuEntryScope& operator=(const GpuEntryScope&) = delete;

    [[nodiscard]] prSuiteError result() const noexcept { return m_result; }
    [[nodiscard]] const PrGPUFilter& filter() const noexcept { return m_filter; }

private:
    mock::MockHost& m_host;
    PrGPUFilter m_filter{};
    PrGPUFilterInfo m_info{};
    prSuiteError m_result = suiteError_Fail;
    bool m_startedUp = false;
};

/// Push the mock host's CUDA context on the test thread for the scope.  The
/// test thread is not a render thread, so without this every driver call the
/// test makes itself fails with CUDA_ERROR_INVALID_CONTEXT.
class HostContextScope {
public:
    explicit HostContextScope(mock::MockHost& host);
    ~HostContextScope();
    HostContextScope(const HostContextScope&) = delete;
    HostContextScope& operator=(const HostContextScope&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_pushed; }
    [[nodiscard]] CUcontext context() const noexcept { return m_context; }

private:
    CUcontext m_context = nullptr;
    bool m_pushed = false;
};

/// True when `context` is its device's primary context.  Never creates a
/// primary context that was not already alive (cuDevicePrimaryCtxGetState
/// first), so asking costs nothing.
[[nodiscard]] bool isPrimaryContext(CUcontext context);

// ---------------------------------------------------------------------------
//  GPU frames
// ---------------------------------------------------------------------------

/// A BGRA 32f / 16f GPU frame created through the GPU Device Suite, top-left
/// origin, and disposed through the PPix Suite when it goes out of scope.
class GpuFrame {
public:
    GpuFrame(mock::MockHost& host, int width, int height, bool half);
    ~GpuFrame();
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_hand != nullptr && m_rowBytes > 0; }
    [[nodiscard]] PPixHand hand() const noexcept { return m_hand; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] std::int32_t rowBytes() const noexcept { return m_rowBytes; }
    [[nodiscard]] bool isHalf() const noexcept { return m_half; }
    /// Bytes of the whole frame (rowBytes * height).
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return static_cast<std::size_t>(m_rowBytes) * static_cast<std::size_t>(m_height);
    }

    /// Copy `bytes` (exactly byteSize(), rows at rowBytes()) into the frame.
    [[nodiscard]] bool upload(const std::vector<std::uint8_t>& bytes);
    /// Fill every pixel with one straight RGBA colour.
    [[nodiscard]] bool fill(const float rgba[4]);
    /// The frame as RGBA floats, row 0 = top, width * height * 4 values;
    /// empty on failure.  Synchronises the context first.
    [[nodiscard]] std::vector<float> downloadRgba();

private:
    mock::MockHost& m_host;
    PPixHand m_hand = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::int32_t m_rowBytes = 0;
    bool m_half = false;
};

// ---------------------------------------------------------------------------
//  Filter instances
// ---------------------------------------------------------------------------

/// One PrGPUFilter instance on (node, timeline, device 0), disposed on
/// destruction.  Check created() before rendering.
class FilterInstance {
public:
    FilterInstance(const GpuEntryScope& scope, mock::MockHost& host, csSDK_int32 nodeId, PrTimelineID timeline);
    ~FilterInstance();
    FilterInstance(const FilterInstance&) = delete;
    FilterInstance& operator=(const FilterInstance&) = delete;

    /// What CreateInstance returned.
    [[nodiscard]] prSuiteError created() const noexcept { return m_created; }
    /// Render `in` into `out` at `clipTime` (the sequence time is set equal).
    [[nodiscard]] prSuiteError render(const GpuFrame& in, GpuFrame& out, PrTime clipTime, PrTime ticksPerFrame);
    /// DisposeInstance now (idempotent; the destructor calls it too).
    prSuiteError dispose();

private:
    const GpuEntryScope& m_scope;
    PrGPUFilterInstance m_instance{};
    prSuiteError m_created = suiteError_Fail;
    bool m_live = false;
};

// ---------------------------------------------------------------------------
//  The controls, in Premiere's verbatim layout
// ---------------------------------------------------------------------------

/// Every value-carrying control of the effect, as a host would store it.
///
/// The three popups hold RAW host values: 1-based by default (After Effects'
/// numbering, which the mock serves), 0-based when a test sets them the way
/// Premiere 26.2.2 numbers popups on the GPU side.  A test that uses 0-based
/// values sets every popup it writes 0-based, the Lens included.
struct Controls {
    int resolution = static_cast<int>(Resolution::MatchSequence);  ///< Popup value.
    int preset = static_cast<int>(Preset::Custom);                 ///< Popup value.
    double pan = 0.0;
    double tilt = 0.0;
    double roll = 0.0;
    double fov = 90.0;
    double distortion = 0.0;
    double sourcePan = 0.0;
    double sourceTilt = 0.0;
    double sourceRoll = 0.0;
    bool smooth = false;
    /// [WP-LENSUI] The Lens popup.  Classic by default, because FOV and
    /// Distortion above are Classic numbers and every GPU test written before
    /// the popup existed describes a Classic camera; the EFFECT's default is
    /// DJI.  In 1-based numbering "Classic" is 2, the popup's entry count, so
    /// it also settles the host's numbering as 1-based.
    int lens = static_cast<int>(LensPopup::Classic);
};

/// Write `c` onto `node` at `time` the way Premiere 26.2.2 serves it: host
/// index = AE index - 1 for every control, the group markers (every AE index
/// that is not a value control, per ReframeParams.h) as Bool false, and
/// GetParamCount pinned to OSV_REFRAME_PARAM_COUNT.  The DJI block is not
/// written (it reads its defaults) except the Lens popup.
void writeVerbatimControls(mock::MockHost& host, csSDK_int32 node, const Controls& c, PrTime time = 0);

/// The Settings the effect should read from `c` (for CPU references): the
/// popups decoded exactly as the GPU filter decodes them - the numbering
/// learned from the three raw values together (decodeHostPopup), then each
/// one translated.
[[nodiscard]] Settings settingsOf(const Controls& c);

// ---------------------------------------------------------------------------
//  Image measurements
// ---------------------------------------------------------------------------

/// Fraction of pixels whose RGBA is within `tolerance` of `rgba` in every
/// channel.  1.0 for a frame the equirect path rendered from a solid input
/// of that colour; ~0 for a frame rendered from the fisheyes.
[[nodiscard]] double fractionOfColour(const std::vector<float>& image, const float rgba[4], float tolerance);

/// Fraction of pixels with alpha above 0.999.
[[nodiscard]] double opaqueFraction(const std::vector<float>& image);

/// Largest absolute difference over every channel of two equal-size images
/// (infinity when the sizes differ).
[[nodiscard]] double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b);

/// Geometric agreement of two renders of the same view, measured the way a
/// framing difference would show: both lumas are low-passed (a Gaussian of
/// `sigma` px, which removes the sharpness difference between a one-step and
/// a two-step resampling), the frame is cut into tiles, and each tile's
/// translation is estimated to sub-pixel precision with Lucas-Kanade.
struct Alignment {
    int tilesUsed = 0;        ///< Tiles with enough texture and coverage to measure.
    double maxShiftPx = 0.0;  ///< Largest tile displacement, in output pixels.
    double medianDx = 0.0;    ///< Median tile displacement in x (b relative to a).
    double medianDy = 0.0;    ///< Median tile displacement in y.
    double ncc = 0.0;         ///< Normalised cross-correlation of the low-passed lumas.
};
[[nodiscard]] Alignment measureAlignment(const std::vector<float>& a, const std::vector<float>& b, int width,
                                         int height, double sigma = 2.0);

// ---------------------------------------------------------------------------
//  The isolated plug-in log
// ---------------------------------------------------------------------------

/// %LOCALAPPDATA%\OpenOSV\Open360Reframe.log - which TestMain has pointed at
/// a per-process temporary directory (TestLogIsolation.h).
[[nodiscard]] std::filesystem::path reframeLogPath();
/// The log's size now, to read only what a test adds after this point.
[[nodiscard]] std::uintmax_t reframeLogSize();
/// This process's log lines written after byte `offset`.
[[nodiscard]] std::vector<std::string> reframeLogLinesSince(std::uintmax_t offset);
/// True when any of this process's log lines contains `text`.
[[nodiscard]] bool reframeLogContains(const std::string& text);

}  // namespace osv::reframe::test
