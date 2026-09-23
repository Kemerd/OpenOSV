// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuTestSupport.cpp - see GpuTestSupport.h.

#include "GpuTestSupport.h"

#include "PrSDKVideoSegmentSuite.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace osv::reframe::test {

using mock::MockHost;

// ===========================================================================
//  The sample clip
// ===========================================================================

std::filesystem::path e2eSampleClipPath() {
    // The environment wins so a developer can point one run at another copy
    // without reconfiguring; the configure-time default is only defined when
    // the file existed then.
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        if (env[0] != '\0') {
            return std::filesystem::path(env);
        }
    }
#ifdef OSV_REFRAME_SAMPLE_CLIP
    return std::filesystem::path(OSV_REFRAME_SAMPLE_CLIP);
#else
    return {};
#endif
}

bool e2eSampleClipAvailable() {
    const std::filesystem::path p = e2eSampleClipPath();
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 0;
}

// ===========================================================================
//  Suite access
// ===========================================================================

namespace {

/// A suite acquired from the mock host for the lifetime of the object, so
/// every acquire the tests make is balanced by a release.
template <class SuiteT>
class SuiteRef {
public:
    SuiteRef(MockHost& host, const char* name, int version) : m_host(host), m_name(name), m_version(version) {
        const void* raw = nullptr;
        if (host.basicSuite() && host.basicSuite()->AcquireSuite(name, version, &raw) == kSPNoError && raw) {
            m_suite = static_cast<const SuiteT*>(raw);
        }
    }
    ~SuiteRef() {
        if (m_suite && m_host.basicSuite()) {
            m_host.basicSuite()->ReleaseSuite(m_name, m_version);
        }
    }
    SuiteRef(const SuiteRef&) = delete;
    SuiteRef& operator=(const SuiteRef&) = delete;

    [[nodiscard]] const SuiteT* get() const noexcept { return m_suite; }
    [[nodiscard]] const SuiteT* operator->() const noexcept { return m_suite; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_suite != nullptr; }

private:
    MockHost& m_host;
    const char* m_name;
    int m_version;
    const SuiteT* m_suite = nullptr;
};

/// The device pointer behind a GPU PPix, or null.
[[nodiscard]] void* deviceDataOf(MockHost& host, PPixHand hand) {
    SuiteRef<PrSDKGPUDeviceSuite> gpu(host, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);
    void* data = nullptr;
    if (!gpu || !gpu->GetGPUPPixData || gpu->GetGPUPPixData(hand, &data) != suiteError_NoError) {
        return nullptr;
    }
    return data;
}

}  // namespace

// ===========================================================================
//  GpuEntryScope
// ===========================================================================

GpuEntryScope::GpuEntryScope(MockHost& host) : m_host(host) {
    const GpuEntryFn entry = LoadedPlugin::instance().gpuEntry();
    if (!entry) {
        return;
    }
    csSDK_int32 index = 0;
    m_result = entry(PrSDKGPUFilterInterfaceVersion2, &index, kPrTrue, host.piSuites(), &m_filter, &m_info);
    m_startedUp = (m_result == suiteError_NoError);
}

GpuEntryScope::~GpuEntryScope() {
    if (!m_startedUp) {
        return;
    }
    // Shutdown is what unloads the per-device CUDA modules, while the
    // host's context is still alive.
    csSDK_int32 index = 0;
    PrGPUFilter filter{};
    PrGPUFilterInfo info{};
    LoadedPlugin::instance().gpuEntry()(PrSDKGPUFilterInterfaceVersion2, &index, kPrFalse, m_host.piSuites(), &filter,
                                        &info);
}

// ===========================================================================
//  HostContextScope
// ===========================================================================

HostContextScope::HostContextScope(MockHost& host) {
    SuiteRef<PrSDKGPUDeviceSuite> gpu(host, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);
    if (!gpu || !gpu->GetDeviceInfo) {
        return;
    }
    PrGPUDeviceInfo info{};
    if (gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, 0, &info) != suiteError_NoError) {
        return;
    }
    m_context = static_cast<CUcontext>(info.outContextHandle);
    if (m_context && cuCtxPushCurrent(m_context) == CUDA_SUCCESS) {
        m_pushed = true;
    }
}

HostContextScope::~HostContextScope() {
    if (m_pushed) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
    }
}

bool isPrimaryContext(CUcontext context) {
    if (!context || cuCtxPushCurrent(context) != CUDA_SUCCESS) {
        return false;
    }
    CUdevice device = 0;
    const bool gotDevice = cuCtxGetDevice(&device) == CUDA_SUCCESS;
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
    if (!gotDevice) {
        return false;
    }
    unsigned int flags = 0;
    int active = 0;
    if (cuDevicePrimaryCtxGetState(device, &flags, &active) != CUDA_SUCCESS || !active) {
        return false;  // no primary context alive, so this one cannot be it
    }
    CUcontext primary = nullptr;
    if (cuDevicePrimaryCtxRetain(&primary, device) != CUDA_SUCCESS) {
        return false;
    }
    const bool same = (primary == context);
    cuDevicePrimaryCtxRelease(device);
    return same;
}

// ===========================================================================
//  GpuFrame
// ===========================================================================

GpuFrame::GpuFrame(MockHost& host, int width, int height, bool half)
    : m_host(host), m_width(width), m_height(height), m_half(half) {
    if (width <= 0 || height <= 0) {
        return;
    }
    SuiteRef<PrSDKGPUDeviceSuite> gpu(host, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);
    if (!gpu || !gpu->CreateGPUPPix) {
        return;
    }
    const PrPixelFormat format = half ? PrPixelFormat_GPU_BGRA_4444_16f : PrPixelFormat_GPU_BGRA_4444_32f;
    PPixHand hand = nullptr;
    if (gpu->CreateGPUPPix(0, format, width, height, 1, 1, prFieldsNone, &hand) != suiteError_NoError || !hand) {
        return;
    }
    const auto info = host.inspect(hand);
    if (!info || info->rowBytes <= 0) {
        return;
    }
    m_hand = hand;
    m_rowBytes = info->rowBytes;
}

GpuFrame::~GpuFrame() {
    if (!m_hand) {
        return;
    }
    // GPU PPixes are disposed through the PPix Suite, like host ones.
    SuiteRef<PrSDKPPixSuite> ppix(m_host, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    if (ppix && ppix->Dispose) {
        ppix->Dispose(m_hand);
    }
}

bool GpuFrame::upload(const std::vector<std::uint8_t>& bytes) {
    if (!valid() || bytes.size() != byteSize()) {
        return false;
    }
    void* device = deviceDataOf(m_host, m_hand);
    HostContextScope scope(m_host);
    if (!device || !scope.ok()) {
        return false;
    }
    return cuMemcpyHtoD(reinterpret_cast<CUdeviceptr>(device), bytes.data(), bytes.size()) == CUDA_SUCCESS;
}

bool GpuFrame::fill(const float rgba[4]) {
    if (!valid() || !rgba) {
        return false;
    }
    std::vector<std::uint8_t> bytes(byteSize(), 0u);
    for (int y = 0; y < m_height; ++y) {
        std::uint8_t* row = bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(m_rowBytes);
        for (int x = 0; x < m_width; ++x) {
            // BGRA in memory, straight alpha.
            const float bgra[4] = {rgba[2], rgba[1], rgba[0], rgba[3]};
            if (m_half) {
                auto* px = reinterpret_cast<std::uint16_t*>(row) + static_cast<std::size_t>(x) * 4u;
                for (int c = 0; c < 4; ++c) {
                    px[c] = floatToHalf(bgra[c]);
                }
            } else {
                auto* px = reinterpret_cast<float*>(row) + static_cast<std::size_t>(x) * 4u;
                std::memcpy(px, bgra, sizeof(bgra));
            }
        }
    }
    return upload(bytes);
}

std::vector<float> GpuFrame::downloadRgba() {
    std::vector<float> out;
    if (!valid()) {
        return out;
    }
    void* device = deviceDataOf(m_host, m_hand);
    HostContextScope scope(m_host);
    if (!device || !scope.ok() || cuCtxSynchronize() != CUDA_SUCCESS) {
        return out;
    }
    std::vector<std::uint8_t> bytes(byteSize(), 0u);
    if (cuMemcpyDtoH(bytes.data(), reinterpret_cast<CUdeviceptr>(device), bytes.size()) != CUDA_SUCCESS) {
        return out;
    }
    out.resize(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4u);
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            float* dst = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + x) * 4u;
            if (m_half) {
                readPixelBgra16f(bytes.data(), m_rowBytes, x, y, dst);
            } else {
                readPixelBgra32f(bytes.data(), m_rowBytes, x, y, dst);
            }
        }
    }
    return out;
}

// ===========================================================================
//  FilterInstance
// ===========================================================================

FilterInstance::FilterInstance(const GpuEntryScope& scope, MockHost& host, csSDK_int32 nodeId, PrTimelineID timeline)
    : m_scope(scope) {
    if (scope.result() != suiteError_NoError || !scope.filter().CreateInstance) {
        return;
    }
    m_instance.piSuites = host.piSuites();
    m_instance.inDeviceIndex = 0;
    m_instance.inTimelineID = timeline;
    m_instance.inNodeID = nodeId;
    m_created = scope.filter().CreateInstance(&m_instance);
    m_live = (m_created == suiteError_NoError && m_instance.ioPrivatePluginData != nullptr);
}

FilterInstance::~FilterInstance() { (void)dispose(); }

prSuiteError FilterInstance::dispose() {
    if (!m_live || !m_scope.filter().DisposeInstance) {
        return suiteError_NoError;
    }
    m_live = false;
    return m_scope.filter().DisposeInstance(&m_instance);
}

prSuiteError FilterInstance::render(const GpuFrame& in, GpuFrame& out, PrTime clipTime, PrTime ticksPerFrame) {
    if (!m_live || !m_scope.filter().Render || !in.valid() || !out.valid()) {
        return suiteError_InvalidParms;
    }
    PrGPUFilterRenderParams params{};
    params.inClipTime = clipTime;
    params.inSequenceTime = clipTime;
    params.inQuality = kPrRenderQuality_High;
    params.inDownsampleFactorX = 1.0f;
    params.inDownsampleFactorY = 1.0f;
    params.inRenderWidth = static_cast<csSDK_uint32>(out.width());
    params.inRenderHeight = static_cast<csSDK_uint32>(out.height());
    params.inRenderPARNum = 1;
    params.inRenderPARDen = 1;
    params.inRenderFieldType = prFieldsNone;
    params.inRenderTicksPerFrame = ticksPerFrame;
    const PPixHand inputs[1] = {in.hand()};
    PPixHand output = out.hand();
    return m_scope.filter().Render(&m_instance, &params, inputs, 1, &output);
}

// ===========================================================================
//  Controls
// ===========================================================================

namespace {

void putI32(MockHost& host, csSDK_int32 node, int aeIndex, int value, PrTime time) {
    PrParam p{};
    p.mType = kPrParamType_Int32;
    p.mInt32 = value;
    host.setParam(node, gpuParamIndex(aeIndex), time, p);
}

void putF32(MockHost& host, csSDK_int32 node, int aeIndex, double value, PrTime time) {
    PrParam p{};
    p.mType = kPrParamType_Float32;
    p.mFloat32 = static_cast<float>(value);
    host.setParam(node, gpuParamIndex(aeIndex), time, p);
}

void putF64(MockHost& host, csSDK_int32 node, int aeIndex, double value, PrTime time) {
    PrParam p{};
    p.mType = kPrParamType_Float64;
    p.mFloat64 = value;
    host.setParam(node, gpuParamIndex(aeIndex), time, p);
}

void putBool(MockHost& host, csSDK_int32 node, int aeIndex, bool value, PrTime time) {
    PrParam p{};
    p.mType = kPrParamType_Bool;
    p.mBool = value ? 1 : 0;
    host.setParam(node, gpuParamIndex(aeIndex), time, p);
}

/// True when `aeIndex` is a value-carrying control (ReframeParams.h).
[[nodiscard]] bool isValueControl(int aeIndex) {
    for (int i = 0; i < kValueParamCount; ++i) {
        if (kValueParamAeIndex[i] == aeIndex) {
            return true;
        }
    }
    return false;
}

}  // namespace

void writeVerbatimControls(MockHost& host, csSDK_int32 node, const Controls& c, PrTime time) {
    // The group markers: every AE index the tables do not list as a value
    // control, answering as Bool false like Premiere 26.2.2's did.
    for (int ae = 1; ae <= kParamCount; ++ae) {
        if (!isValueControl(ae)) {
            putBool(host, node, ae, false, time);
        }
    }
    putI32(host, node, kIndexOutputResolution, c.resolution, time);
    putI32(host, node, kIndexPreset, c.preset, time);
    putF32(host, node, kIndexPan, c.pan, time);
    putF32(host, node, kIndexTilt, c.tilt, time);
    putF32(host, node, kIndexRoll, c.roll, time);
    putF64(host, node, kIndexFov, c.fov, time);
    putF64(host, node, kIndexDistortion, c.distortion, time);
    putF32(host, node, kIndexSourcePan, c.sourcePan, time);
    putF32(host, node, kIndexSourceTilt, c.sourceTilt, time);
    putF32(host, node, kIndexSourceRoll, c.sourceRoll, time);
    putBool(host, node, kIndexSmooth, c.smooth, time);
    // [WP-LENSUI] The Lens popup, in the same raw numbering as the others.
    putI32(host, node, kIndexLens, c.lens, time);
    // [WP-EASING] The Keyframe Easing popup, only when the test chose one.
    if (c.easing >= 0) {
        putI32(host, node, kIndexKeyframeEasing, c.easing, time);
    }
    // The count a host with this layout reports, whatever this build added
    // after the controls written here.
    host.setParamCount(node, kParamCount);
}

Settings settingsOf(const Controls& c) {
    Settings s;
    // Learn the numbering from every popup first, then decode each - the
    // order GpuFilter.cpp's readSettings() uses, so a 0-based test and a
    // 1-based one both get the Settings the filter should have read.
    PopupBase base = PopupBase::Unknown;
    (void)decodeHostPopup(c.resolution, OSV_REFRAME_RESOLUTION_COUNT, &base);
    (void)decodeHostPopup(c.preset, OSV_REFRAME_PRESET_COUNT, &base);
    (void)decodeHostPopup(c.lens, OSV_REFRAME_LENS_COUNT, &base);
    if (c.easing >= 0) {
        (void)decodeHostPopup(c.easing, OSV_REFRAME_EASING_COUNT, &base);
    }
    s.resolution = sanitiseResolution(decodeHostPopup(c.resolution, OSV_REFRAME_RESOLUTION_COUNT, &base));
    s.preset = sanitisePreset(decodeHostPopup(c.preset, OSV_REFRAME_PRESET_COUNT, &base));
    s.cameraModel = cameraModelFromLensPopup(decodeHostPopup(c.lens, OSV_REFRAME_LENS_COUNT, &base));
    // The easing is recorded but NOT applied: the angles below are the
    // constants the controls hold, and a test that keyframes them computes
    // the eased values it expects itself.
    s.easing = sanitiseKeyframeEasing(c.easing >= 0 ? decodeHostPopup(c.easing, OSV_REFRAME_EASING_COUNT, &base)
                                                    : OSV_REFRAME_EASING_DEFAULT);
    s.panDeg = c.pan;
    s.tiltDeg = c.tilt;
    s.rollDeg = c.roll;
    s.fovDeg = c.fov;
    s.distortion = c.distortion;
    s.sourcePanDeg = c.sourcePan;
    s.sourceTiltDeg = c.sourceTilt;
    s.sourceRollDeg = c.sourceRoll;
    s.smoothKeyframes = c.smooth;
    return s;
}

// ===========================================================================
//  Image measurements
// ===========================================================================

double fractionOfColour(const std::vector<float>& image, const float rgba[4], float tolerance) {
    const std::size_t pixels = image.size() / 4u;
    if (pixels == 0 || !rgba) {
        return 0.0;
    }
    std::size_t matching = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const float* p = image.data() + i * 4u;
        bool same = true;
        for (int c = 0; c < 4 && same; ++c) {
            same = std::fabs(p[c] - rgba[c]) <= tolerance;
        }
        matching += same ? 1u : 0u;
    }
    return static_cast<double>(matching) / static_cast<double>(pixels);
}

double opaqueFraction(const std::vector<float>& image) {
    const std::size_t pixels = image.size() / 4u;
    if (pixels == 0) {
        return 0.0;
    }
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        opaque += (image[i * 4u + 3u] > 0.999f) ? 1u : 0u;
    }
    return static_cast<double>(opaque) / static_cast<double>(pixels);
}

double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        // A NaN anywhere is a total mismatch, not a zero difference.
        if (!(d <= worst)) {
            worst = std::isfinite(d) ? d : std::numeric_limits<double>::infinity();
        }
    }
    return worst;
}

namespace {

/// A single-channel image with a validity mask.
struct Plane {
    int w = 0;
    int h = 0;
    std::vector<double> v;
    std::vector<std::uint8_t> ok;

    [[nodiscard]] std::size_t at(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    }
};

/// BT.2020 luma of the encoded values; valid where alpha is fully opaque.
[[nodiscard]] Plane lumaPlane(const std::vector<float>& rgba, int w, int h) {
    Plane p;
    p.w = w;
    p.h = h;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    p.v.assign(n, 0.0);
    p.ok.assign(n, 0u);
    for (std::size_t i = 0; i < n && i * 4u + 3u < rgba.size(); ++i) {
        const float* c = rgba.data() + i * 4u;
        const double y = 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2];
        p.v[i] = y;
        p.ok[i] = (c[3] > 0.999f && std::isfinite(y)) ? 1u : 0u;
    }
    return p;
}

/// Separable Gaussian low-pass as a NORMALISED convolution over the valid
/// pixels: invalid pixels contribute nothing and a pixel stays valid only
/// when (almost) its whole support was valid, so the letterbox of an
/// uncovered region cannot bleed into the measurement.
[[nodiscard]] Plane gaussian(const Plane& in, double sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<double> k(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-0.5 * (i * i) / (sigma * sigma));
        k[static_cast<std::size_t>(i + radius)] = v;
        sum += v;
    }
    for (double& v : k) {
        v /= sum;
    }
    const std::size_t n = in.v.size();
    std::vector<double> num(n, 0.0);
    std::vector<double> den(n, 0.0);
    // Horizontal pass on value * mask and on the mask.
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            double a = 0.0;
            double b = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int xx = std::clamp(x + i, 0, in.w - 1);
                const std::size_t j = in.at(xx, y);
                const double wgt = k[static_cast<std::size_t>(i + radius)] * (in.ok[j] ? 1.0 : 0.0);
                a += wgt * in.v[j];
                b += wgt;
            }
            num[in.at(x, y)] = a;
            den[in.at(x, y)] = b;
        }
    }
    // Vertical pass.
    Plane out;
    out.w = in.w;
    out.h = in.h;
    out.v.assign(n, 0.0);
    out.ok.assign(n, 0u);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            double a = 0.0;
            double b = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int yy = std::clamp(y + i, 0, in.h - 1);
                const std::size_t j = in.at(x, yy);
                a += k[static_cast<std::size_t>(i + radius)] * num[j];
                b += k[static_cast<std::size_t>(i + radius)] * den[j];
            }
            const std::size_t o = out.at(x, y);
            if (b > 0.999) {
                out.v[o] = a / b;
                out.ok[o] = 1u;
            }
        }
    }
    return out;
}

/// Bilinear sample; false when any of the four taps is invalid or outside.
[[nodiscard]] bool sample(const Plane& p, double x, double y, double* out) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    if (x0 < 0 || y0 < 0 || x0 + 1 >= p.w || y0 + 1 >= p.h) {
        return false;
    }
    const std::size_t i00 = p.at(x0, y0);
    const std::size_t i10 = p.at(x0 + 1, y0);
    const std::size_t i01 = p.at(x0, y0 + 1);
    const std::size_t i11 = p.at(x0 + 1, y0 + 1);
    if (!p.ok[i00] || !p.ok[i10] || !p.ok[i01] || !p.ok[i11]) {
        return false;
    }
    const double fx = x - x0;
    const double fy = y - y0;
    *out = (p.v[i00] * (1.0 - fx) + p.v[i10] * fx) * (1.0 - fy) + (p.v[i01] * (1.0 - fx) + p.v[i11] * fx) * fy;
    return true;
}

/// One tile's translation d such that b(x + d) ~ a(x), by Lucas-Kanade
/// (Gauss-Newton with a's gradient), after normalising both tiles to zero
/// mean and unit variance so a gain or offset difference cannot pose as a
/// shift.  Returns false for a tile without enough coverage or texture.
[[nodiscard]] bool tileShift(const Plane& a, const Plane& b, int x0, int y0, int x1, int y1, double* dx,
                             double* dy) {
    // ---- statistics over pixels valid in both (at zero shift) -------------
    double sa = 0.0;
    double sb = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    std::size_t n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = a.at(x, y);
            if (!a.ok[i] || !b.ok[i]) {
                continue;
            }
            sa += a.v[i];
            sb += b.v[i];
            saa += a.v[i] * a.v[i];
            sbb += b.v[i] * b.v[i];
            ++n;
        }
    }
    const std::size_t area = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
    if (n < area * 8u / 10u) {
        return false;  // mostly uncovered
    }
    const double ma = sa / static_cast<double>(n);
    const double mb = sb / static_cast<double>(n);
    const double va = saa / static_cast<double>(n) - ma * ma;
    const double vb = sbb / static_cast<double>(n) - mb * mb;
    if (!(va > 1e-10) || !(vb > 1e-10)) {
        return false;  // flat
    }
    const double ka = 1.0 / std::sqrt(va);
    const double kb = 1.0 / std::sqrt(vb);

    // ---- structure tensor of a (constant: the template's gradient) --------
    double h00 = 0.0;
    double h01 = 0.0;
    double h11 = 0.0;
    std::size_t m = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        for (int x = x0 + 1; x < x1 - 1; ++x) {
            if (!a.ok[a.at(x, y)] || !a.ok[a.at(x - 1, y)] || !a.ok[a.at(x + 1, y)] || !a.ok[a.at(x, y - 1)] ||
                !a.ok[a.at(x, y + 1)]) {
                continue;
            }
            const double gx = 0.5 * (a.v[a.at(x + 1, y)] - a.v[a.at(x - 1, y)]) * ka;
            const double gy = 0.5 * (a.v[a.at(x, y + 1)] - a.v[a.at(x, y - 1)]) * ka;
            h00 += gx * gx;
            h01 += gx * gy;
            h11 += gy * gy;
            ++m;
        }
    }
    if (m == 0) {
        return false;
    }
    // Smallest eigenvalue per pixel: an edge-only tile (the aperture
    // problem) or a flat one cannot be localised in both directions.
    const double tr = h00 + h11;
    const double det = h00 * h11 - h01 * h01;
    const double lambdaMin = 0.5 * (tr - std::sqrt(std::max(0.0, tr * tr - 4.0 * det)));
    if (!(lambdaMin / static_cast<double>(m) > 1e-4) || !(std::fabs(det) > 0.0)) {
        return false;
    }

    // ---- Gauss-Newton on the shift ----------------------------------------
    double ux = 0.0;
    double uy = 0.0;
    for (int iter = 0; iter < 30; ++iter) {
        double b0 = 0.0;
        double b1 = 0.0;
        double g00 = 0.0;
        double g01 = 0.0;
        double g11 = 0.0;
        for (int y = y0 + 1; y < y1 - 1; ++y) {
            for (int x = x0 + 1; x < x1 - 1; ++x) {
                if (!a.ok[a.at(x, y)] || !a.ok[a.at(x - 1, y)] || !a.ok[a.at(x + 1, y)] ||
                    !a.ok[a.at(x, y - 1)] || !a.ok[a.at(x, y + 1)]) {
                    continue;
                }
                double bv = 0.0;
                if (!sample(b, x + ux, y + uy, &bv)) {
                    continue;
                }
                const double gx = 0.5 * (a.v[a.at(x + 1, y)] - a.v[a.at(x - 1, y)]) * ka;
                const double gy = 0.5 * (a.v[a.at(x, y + 1)] - a.v[a.at(x, y - 1)]) * ka;
                const double err = (a.v[a.at(x, y)] - ma) * ka - (bv - mb) * kb;
                b0 += gx * err;
                b1 += gy * err;
                g00 += gx * gx;
                g01 += gx * gy;
                g11 += gy * gy;
            }
        }
        const double d = g00 * g11 - g01 * g01;
        if (!(std::fabs(d) > 0.0)) {
            return false;
        }
        const double stepX = (g11 * b0 - g01 * b1) / d;
        const double stepY = (g00 * b1 - g01 * b0) / d;
        ux += stepX;
        uy += stepY;
        // A tile that runs away is not measuring a small misalignment.
        if (std::fabs(ux) > 8.0 || std::fabs(uy) > 8.0) {
            return false;
        }
        if (std::fabs(stepX) < 1e-5 && std::fabs(stepY) < 1e-5) {
            break;
        }
    }
    *dx = ux;
    *dy = uy;
    return true;
}

[[nodiscard]] double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2u;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

}  // namespace

Alignment measureAlignment(const std::vector<float>& a, const std::vector<float>& b, int width, int height,
                           double sigma) {
    Alignment r;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (width < 64 || height < 64 || a.size() != n || b.size() != n || !(sigma > 0.0)) {
        return r;
    }
    const Plane la = gaussian(lumaPlane(a, width, height), sigma);
    const Plane lb = gaussian(lumaPlane(b, width, height), sigma);

    // ---- NCC of the low-passed lumas ----------------------------------------
    {
        double sa = 0.0;
        double sb = 0.0;
        double saa = 0.0;
        double sbb = 0.0;
        double sab = 0.0;
        std::size_t m = 0;
        for (std::size_t i = 0; i < la.v.size(); ++i) {
            if (!la.ok[i] || !lb.ok[i]) {
                continue;
            }
            sa += la.v[i];
            sb += lb.v[i];
            saa += la.v[i] * la.v[i];
            sbb += lb.v[i] * lb.v[i];
            sab += la.v[i] * lb.v[i];
            ++m;
        }
        if (m > 16) {
            const double inv = 1.0 / static_cast<double>(m);
            const double cov = sab * inv - (sa * inv) * (sb * inv);
            const double varA = saa * inv - (sa * inv) * (sa * inv);
            const double varB = sbb * inv - (sb * inv) * (sb * inv);
            if (varA > 0.0 && varB > 0.0) {
                r.ncc = cov / std::sqrt(varA * varB);
            }
        }
    }

    // ---- tile shifts ----------------------------------------------------------
    // A 6 x 4 grid inside a margin that keeps every tile clear of the frame
    // edge (where the blur has no support and the shift would sample outside).
    constexpr int kTilesX = 6;
    constexpr int kTilesY = 4;
    const int margin = static_cast<int>(std::ceil(3.0 * sigma)) + 10;
    const int innerW = width - 2 * margin;
    const int innerH = height - 2 * margin;
    if (innerW < kTilesX * 16 || innerH < kTilesY * 16) {
        return r;
    }
    std::vector<double> dxs;
    std::vector<double> dys;
    for (int ty = 0; ty < kTilesY; ++ty) {
        for (int tx = 0; tx < kTilesX; ++tx) {
            const int x0 = margin + innerW * tx / kTilesX;
            const int x1 = margin + innerW * (tx + 1) / kTilesX;
            const int y0 = margin + innerH * ty / kTilesY;
            const int y1 = margin + innerH * (ty + 1) / kTilesY;
            double dx = 0.0;
            double dy = 0.0;
            if (!tileShift(la, lb, x0, y0, x1, y1, &dx, &dy)) {
                continue;
            }
            dxs.push_back(dx);
            dys.push_back(dy);
            r.maxShiftPx = std::max(r.maxShiftPx, std::sqrt(dx * dx + dy * dy));
        }
    }
    r.tilesUsed = static_cast<int>(dxs.size());
    r.medianDx = median(dxs);
    r.medianDy = median(dys);
    return r;
}

// ===========================================================================
//  The isolated plug-in log
// ===========================================================================

std::filesystem::path reframeLogPath() {
    wchar_t buffer[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, n)) / L"OpenOSV" / L"Open360Reframe.log";
}

std::uintmax_t reframeLogSize() {
    std::error_code ec;
    const std::filesystem::path p = reframeLogPath();
    if (p.empty() || !std::filesystem::exists(p, ec)) {
        return 0;
    }
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    return ec ? 0 : size;
}

std::vector<std::string> reframeLogLinesSince(std::uintmax_t offset) {
    std::vector<std::string> lines;
    const std::filesystem::path p = reframeLogPath();
    if (p.empty()) {
        return lines;
    }
    std::ifstream file(p, std::ios::binary);
    if (!file) {
        return lines;
    }
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file) {
        return lines;
    }
    // Only this process's lines: the directory is per process already, but
    // a pid that recurs within the sweep window could leave an older file.
    const std::string mine = "[pid " + std::to_string(GetCurrentProcessId()) + " ";
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find(mine) != std::string::npos) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool reframeLogContains(const std::string& text) {
    for (const std::string& line : reframeLogLinesSince(0)) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace osv::reframe::test
