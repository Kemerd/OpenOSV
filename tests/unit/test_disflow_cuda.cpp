// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_disflow_cuda.cpp - the GPU analyses (docs/DIRECT_GPU.md, WP-B) against
// the CPU reference they replace.
//
// Three groups:
//
//   [flow][cuda]           The CUDA DIS solver against DisFlow.cpp on
//                          synthetic pairs whose motion is known by
//                          construction, its input validation, the
//                          ClassicalCuda backend's wiring through
//                          makeFlowBackend / computeFlow, determinism and
//                          thread safety.
//
//   [seam][cuda][sample]   GPU band shading against the CPU band path on the
//                          sample clip, from frames NVDEC left in VRAM
//                          (keepOnDevice) - and the three analyses that sit
//                          on it (seam search, gain estimate, parallax bands)
//                          working from such frames at all.
//
//   [parallax][cuda][sample]  The parallax grid built from GPU flow against
//                          the grid built from CPU flow, on real bands from
//                          frames 0, 32 and 64.
//
// The parity bars are the design document's: mean endpoint difference below
// 0.05 px over pixels both solvers call consistent, consistency masks agreeing
// on at least 98 % of pixels, gated grid cells within 2 %, mean correction
// within 5 %, and PSNR >= 60 dB after 16-bit quantisation for the shading.
// The port mirrors the CPU arithmetic closely enough that the measured
// differences are far below those bars; each test prints what it measured.
//
// Everything SKIPs cleanly on a machine without a usable CUDA device, and the
// [sample] cases without the clip (OSV_SAMPLE_FILE).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/DisFlow.h"
#include "osv/render/FlowBackend.h"

#if defined(OSV_HAVE_CUDA)

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/CudaAnalysis.h"
#include "osv/render/DeviceBandShader.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace osv;
using render::BidirFlow;
using render::DisFlowParams;
using render::FlowBackendKind;
using render::FlowField;
using render::GrayImage;

namespace {

// ---------------------------------------------------------------------------
//  Guards
// ---------------------------------------------------------------------------

/// SKIP the current test when the GPU analyses cannot run here.
#define OSV_REQUIRE_CUDA_ANALYSES()                                                                                    \
    do {                                                                                                               \
        std::string osvCudaWhy_;                                                                                       \
        if (!::osv::render::cudaAnalysesAvailable(&osvCudaWhy_)) {                                                     \
            SKIP("CUDA analyses unavailable: " << osvCudaWhy_);                                                       \
        }                                                                                                              \
    } while (0)

/// Installs the GPU analyses for one test and removes them again, so no
/// other test in the process sees different hooks than it expects.
class InstalledAnalyses {
public:
    InstalledAnalyses() : m_status(render::installCudaAnalyses()) {}
    ~InstalledAnalyses() { render::uninstallCudaAnalyses(); }
    InstalledAnalyses(const InstalledAnalyses&) = delete;
    InstalledAnalyses& operator=(const InstalledAnalyses&) = delete;

    [[nodiscard]] const Status& status() const noexcept { return m_status; }

private:
    Status m_status;
};

// ---------------------------------------------------------------------------
//  Synthetic data (the same constructions as test_disflow.cpp)
// ---------------------------------------------------------------------------

/// Summed incommensurate sinusoids: textured in both axes everywhere.
GrayImage textured(std::uint32_t w, std::uint32_t h, double phase = 0.0) {
    GrayImage img;
    img.w = w;
    img.h = h;
    img.data.resize(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const double fx = static_cast<double>(x);
            const double fy = static_cast<double>(y);
            const double value = 0.5 + 0.18 * std::sin(0.21 * fx + phase) + 0.14 * std::sin(0.13 * fy + 0.7 * phase) +
                                 0.10 * std::sin(0.071 * (fx + fy)) + 0.06 * std::sin(0.037 * (fx - 1.7 * fy));
            img.data[static_cast<std::size_t>(y) * w + x] = static_cast<float>(value);
        }
    }
    return img;
}

/// dst(p) = src(p - d): the true flow from src to dst is exactly (dx, dy).
GrayImage shifted(const GrayImage& src, double dx, double dy) {
    GrayImage dst;
    dst.w = src.w;
    dst.h = src.h;
    dst.data.resize(src.data.size());
    for (std::uint32_t y = 0; y < src.h; ++y) {
        for (std::uint32_t x = 0; x < src.w; ++x) {
            dst.data[static_cast<std::size_t>(y) * src.w + x] = src.sample(
                static_cast<float>(static_cast<double>(x) - dx), static_cast<float>(static_cast<double>(y) - dy));
        }
    }
    return dst;
}

/// A band-shaped pair (wide and short, like the overlap bands) whose motion
/// varies across it, so overlapping patches disagree and densify's summation
/// order matters - test_disflow.cpp's bit-identity case.
void bandPair(GrayImage& a, GrayImage& b) {
    a = textured(1500, 120);
    b = shifted(a, 1.7, -0.6);
    for (std::uint32_t y = 0; y < b.h; ++y) {
        for (std::uint32_t x = b.w / 2; x < b.w; ++x) {
            const double dx = 3.0 + 2.0 * std::sin(0.01 * static_cast<double>(x));
            b.data[static_cast<std::size_t>(y) * b.w + x] =
                a.sample(static_cast<float>(static_cast<double>(x) - dx), static_cast<float>(y) + 0.8f);
        }
    }
}

// ---------------------------------------------------------------------------
//  Comparison
// ---------------------------------------------------------------------------

/// How one bidirectional result differs from another.
struct FlowParity {
    double meanEpeBothOk = 0.0;    ///< Mean |forward difference| over pixels both call consistent.
    double maxEpeBothOk = 0.0;     ///< Largest such difference.
    double maskAgreement = 1.0;    ///< Fraction of pixels whose ok flags agree.
    std::uint64_t bothOk = 0;      ///< Pixels both call consistent.
    std::uint64_t differingValues = 0;  ///< Float components (u, v, both directions) not bit-equal.
    bool identical = false;        ///< Every component and every flag bit-equal.
};

FlowParity compareFlows(const BidirFlow& ref, const BidirFlow& test) {
    FlowParity p;
    const std::size_t n = ref.forward.u.size();
    REQUIRE(ref.valid());
    REQUIRE(test.valid());
    REQUIRE(test.forward.w == ref.forward.w);
    REQUIRE(test.forward.h == ref.forward.h);
    double sum = 0.0;
    std::uint64_t agree = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (ref.ok[i] == test.ok[i]) {
            ++agree;
        }
        if (ref.ok[i] != 0 && test.ok[i] != 0) {
            const double du = static_cast<double>(ref.forward.u[i]) - test.forward.u[i];
            const double dv = static_cast<double>(ref.forward.v[i]) - test.forward.v[i];
            const double e = std::hypot(du, dv);
            sum += e;
            p.maxEpeBothOk = std::max(p.maxEpeBothOk, e);
            ++p.bothOk;
        }
        p.differingValues += (ref.forward.u[i] != test.forward.u[i]) + (ref.forward.v[i] != test.forward.v[i]) +
                             (ref.backward.u[i] != test.backward.u[i]) + (ref.backward.v[i] != test.backward.v[i]);
    }
    p.meanEpeBothOk = p.bothOk ? sum / static_cast<double>(p.bothOk) : 0.0;
    p.maskAgreement = n ? static_cast<double>(agree) / static_cast<double>(n) : 1.0;
    p.identical = p.differingValues == 0 && agree == n && ref.consistent == test.consistent;
    return p;
}

/// The design document's flow parity bar.
void checkFlowParity(const FlowParity& p) {
    INFO("mean EPE over both-consistent pixels " << p.meanEpeBothOk << " px (max " << p.maxEpeBothOk
                                                 << "), masks agree on " << 100.0 * p.maskAgreement << " %, "
                                                 << p.differingValues << " components differ, bit-identical "
                                                 << (p.identical ? "yes" : "no"));
    CHECK(p.meanEpeBothOk < 0.05);
    CHECK(p.maskAgreement >= 0.98);
}

/// Mean flow over an interior region.
void meanInterior(const FlowField& flow, int margin, double& outU, double& outV) {
    double su = 0.0;
    double sv = 0.0;
    int count = 0;
    for (int y = margin; y < static_cast<int>(flow.h) - margin; ++y) {
        for (int x = margin; x < static_cast<int>(flow.w) - margin; ++x) {
            su += flow.atU(x, y);
            sv += flow.atV(x, y);
            ++count;
        }
    }
    outU = count > 0 ? su / count : 0.0;
    outV = count > 0 ? sv / count : 0.0;
}

/// Pack two same-sized planes as an RGBA image (plane, plane, plane, alpha)
/// so compareImages16 - the renderer parity metric - can judge them.
render::ImageRGBAf packPlanes(const std::vector<float>& luma, const std::vector<float>& alpha, std::uint32_t w,
                              std::uint32_t h) {
    auto img = render::ImageRGBAf::create(w, h);
    REQUIRE(img.ok());
    render::ImageRGBAf out = std::move(img).value();
    REQUIRE(luma.size() == static_cast<std::size_t>(w) * h);
    REQUIRE(alpha.size() == luma.size());
    for (std::size_t i = 0; i < luma.size(); ++i) {
        out.data[i * 4 + 0] = luma[i];
        out.data[i * 4 + 1] = luma[i];
        out.data[i * 4 + 2] = luma[i];
        out.data[i * 4 + 3] = alpha[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Sample clip
// ---------------------------------------------------------------------------

struct Sample {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
};

Result<Sample> openSample() {
    Sample s;
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
    return s;
}

/// A reader keeping frames on the GPU, or the reason there is none.
Result<video::DualStreamReader> deviceReader(const Sample& s) {
    video::DecoderOptions opt;
    opt.hw = video::HwAccel::Cuda;
    opt.keepOnDevice = true;
    return video::DualStreamReader::open(osvtest::sampleOsv(), s.format, opt);
}

}  // namespace

// ===========================================================================
//  The solver
// ===========================================================================

TEST_CASE("CUDA DIS matches the CPU solver on synthetic pairs", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    DisFlowParams params;

    SECTION("pure translations, sub-pixel to coarse-level-only") {
        const GrayImage a = textured(128, 96);
        const double cases[][2] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {2.5, 1.5}, {-3.0, 2.0}, {5.0, -4.0}};
        for (const auto& c : cases) {
            INFO("translation (" << c[0] << ", " << c[1] << ")");
            const GrayImage b = shifted(a, c[0], c[1]);
            const auto cpu = render::disFlowBidirectional(a, b, params, nullptr);
            const auto gpu = render::cudaDisFlowBidirectional(a, b, params);
            REQUIRE(cpu.ok());
            REQUIRE(gpu.ok());
            checkFlowParity(compareFlows(cpu.value(), gpu.value()));
        }
    }

    SECTION("an exposure difference between the two images") {
        const GrayImage a = textured(128, 96);
        GrayImage b = shifted(a, 2.0, -1.0);
        for (float& value : b.data) {
            value = value * 1.30f + 0.08f;
        }
        const auto cpu = render::disFlowBidirectional(a, b, params, nullptr);
        const auto gpu = render::cudaDisFlowBidirectional(a, b, params);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        checkFlowParity(compareFlows(cpu.value(), gpu.value()));
    }

    SECTION("a band-shaped pair with spatially varying motion") {
        GrayImage a;
        GrayImage b;
        bandPair(a, b);
        const auto cpu = render::disFlowBidirectional(a, b, params, nullptr);
        const auto gpu = render::cudaDisFlowBidirectional(a, b, params);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        const FlowParity p = compareFlows(cpu.value(), gpu.value());
        checkFlowParity(p);
        CHECK(gpu.value().consistent > 0);
    }

    SECTION("an unrelated pair, where most of the field is rejected") {
        const GrayImage a = textured(96, 96, 0.0);
        const GrayImage b = textured(96, 96, 2.4);
        DisFlowParams strict = params;
        strict.consistencyTolPx = 0.25;
        const auto cpu = render::disFlowBidirectional(a, b, strict, nullptr);
        const auto gpu = render::cudaDisFlowBidirectional(a, b, strict);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        checkFlowParity(compareFlows(cpu.value(), gpu.value()));
    }

    SECTION("non-default parameters: another patch side, more levels, no smoothing") {
        GrayImage a;
        GrayImage b;
        bandPair(a, b);
        DisFlowParams other = params;
        other.patchSize = 6;       // the general (non-unrolled) solve path
        other.patchStridePx = 3;
        other.levels = 6;
        other.smoothSigmaPx = 0.0;
        other.iterations = 20;
        const auto cpu = render::disFlowBidirectional(a, b, other, nullptr);
        const auto gpu = render::cudaDisFlowBidirectional(a, b, other);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        checkFlowParity(compareFlows(cpu.value(), gpu.value()));
    }
}

TEST_CASE("CUDA DIS recovers a known translation", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    // Ground truth, not just agreement with the CPU: a solver that matched a
    // broken reference would pass the parity test above.
    const GrayImage a = textured(128, 96);
    DisFlowParams params;
    const double cases[][2] = {{2.5, 1.5}, {-3.0, 2.0}};
    for (const auto& c : cases) {
        const GrayImage b = shifted(a, c[0], c[1]);
        const auto flow = render::cudaDisFlowBidirectional(a, b, params);
        REQUIRE(flow.ok());
        double mu = 0.0;
        double mv = 0.0;
        meanInterior(flow.value().forward, 16, mu, mv);
        INFO("true (" << c[0] << ", " << c[1] << ") recovered (" << mu << ", " << mv << ")");
        CHECK_THAT(mu, Catch::Matchers::WithinAbs(c[0], 0.5));
        CHECK_THAT(mv, Catch::Matchers::WithinAbs(c[1], 0.5));
        // And the backward field is its negation.
        meanInterior(flow.value().backward, 16, mu, mv);
        CHECK_THAT(mu, Catch::Matchers::WithinAbs(-c[0], 0.5));
        CHECK_THAT(mv, Catch::Matchers::WithinAbs(-c[1], 0.5));
    }
}

TEST_CASE("CUDA DIS reports no motion on a featureless image", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    GrayImage flat;
    flat.w = 64;
    flat.h = 64;
    flat.data.assign(static_cast<std::size_t>(64) * 64, 0.5f);
    const auto flow = render::cudaDisFlowBidirectional(flat, flat, DisFlowParams{});
    REQUIRE(flow.ok());
    REQUIRE(flow.value().valid());
    for (std::size_t i = 0; i < flow.value().forward.u.size(); ++i) {
        REQUIRE(flow.value().forward.u[i] == 0.0f);
        REQUIRE(flow.value().forward.v[i] == 0.0f);
        REQUIRE(flow.value().backward.u[i] == 0.0f);
        REQUIRE(flow.value().backward.v[i] == 0.0f);
    }
}

TEST_CASE("CUDA DIS keeps non-finite pixels out of the field, as the CPU does", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    // A NaN or infinity in a band (a corrupt decode, a division upstream)
    // must not become a NaN vector: every stage that divides guards it, and
    // the GPU port must guard it in the same places.
    GrayImage a = textured(128, 96);
    GrayImage b = shifted(a, 1.5, -0.5);
    a.data[40 * 128 + 60] = std::numeric_limits<float>::quiet_NaN();
    b.data[20 * 128 + 90] = std::numeric_limits<float>::infinity();
    const auto cpu = render::disFlowBidirectional(a, b, DisFlowParams{}, nullptr);
    const auto gpu = render::cudaDisFlowBidirectional(a, b, DisFlowParams{});
    REQUIRE(cpu.ok());
    REQUIRE(gpu.ok());
    for (const FlowField* f : {&gpu.value().forward, &gpu.value().backward}) {
        for (std::size_t i = 0; i < f->u.size(); ++i) {
            REQUIRE(std::isfinite(f->u[i]));
            REQUIRE(std::isfinite(f->v[i]));
        }
    }
    checkFlowParity(compareFlows(cpu.value(), gpu.value()));
}

TEST_CASE("CUDA DIS rejects what the CPU solver rejects, and says what it cannot do", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    const GrayImage good = textured(64, 64);
    const DisFlowParams params;

    SECTION("empty, malformed and mismatched images are InvalidArgument") {
        const GrayImage empty;
        CHECK(render::cudaDisFlowBidirectional(empty, good, params).code() == ErrorCode::InvalidArgument);
        CHECK(render::cudaDisFlowBidirectional(good, empty, params).code() == ErrorCode::InvalidArgument);
        GrayImage bad = good;
        bad.data.pop_back();
        CHECK(render::cudaDisFlowBidirectional(bad, good, params).code() == ErrorCode::InvalidArgument);
        const GrayImage other = textured(48, 64);
        CHECK(render::cudaDisFlowBidirectional(good, other, params).code() == ErrorCode::InvalidArgument);
    }

    SECTION("parameters that describe no solvable problem are InvalidArgument") {
        DisFlowParams bad = params;
        bad.patchSize = 1;
        CHECK(render::cudaDisFlowBidirectional(good, good, bad).code() == ErrorCode::InvalidArgument);
        bad = params;
        bad.iterations = 0;
        CHECK(render::cudaDisFlowBidirectional(good, good, bad).code() == ErrorCode::InvalidArgument);
    }

    SECTION("beyond the GPU's limits is Unsupported, so computeFlow falls back") {
        DisFlowParams big = params;
        big.patchSize = 17;
        CHECK(render::cudaDisFlowBidirectional(good, good, big).code() == ErrorCode::Unsupported);
        DisFlowParams wide = params;
        wide.smoothSigmaPx = 40.0;  // radius 120 taps
        const GrayImage large = textured(512, 256);
        CHECK(render::cudaDisFlowBidirectional(large, large, wide).code() == ErrorCode::Unsupported);
    }

    SECTION("the device entry point checks its pointers and pitch") {
        CHECK(render::cudaDisFlowBidirectionalDevice(nullptr, nullptr, 64, 64, 256, params).code() ==
              ErrorCode::InvalidArgument);
        float* dev = nullptr;
        REQUIRE(cudaMalloc(&dev, 64 * 64 * sizeof(float)) == cudaSuccess);
        CHECK(render::cudaDisFlowBidirectionalDevice(dev, dev, 64, 64, 100, params).code() ==
              ErrorCode::InvalidArgument);  // shorter than a row
        CHECK(render::cudaDisFlowBidirectionalDevice(dev, dev, 64, 64, 258, params).code() ==
              ErrorCode::InvalidArgument);  // not whole floats
        CHECK(render::cudaDisFlowBidirectionalDevice(dev, dev, 0, 64, 256, params).code() ==
              ErrorCode::InvalidArgument);
        cudaFree(dev);
    }
}

TEST_CASE("the device-pointer entry point gives the host entry point's field", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    GrayImage a;
    GrayImage b;
    bandPair(a, b);
    const DisFlowParams params;
    const auto host = render::cudaDisFlowBidirectional(a, b, params);
    REQUIRE(host.ok());

    // Pitched device copies, as a decoder or a band kernel would hold them.
    float* devA = nullptr;
    float* devB = nullptr;
    std::size_t pitchA = 0;
    std::size_t pitchB = 0;
    REQUIRE(cudaMallocPitch(reinterpret_cast<void**>(&devA), &pitchA, a.w * sizeof(float), a.h) == cudaSuccess);
    REQUIRE(cudaMallocPitch(reinterpret_cast<void**>(&devB), &pitchB, b.w * sizeof(float), b.h) == cudaSuccess);
    REQUIRE(pitchA == pitchB);

    // On an explicit caller stream, too: the result must not depend on it.
    // The uploads are ordered on that same stream - the contract is stream
    // order, and a pageable cudaMemcpy2D on the legacy stream may return
    // before its DMA has landed, which a non-blocking stream does not wait
    // for.
    cudaStream_t stream = nullptr;
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    REQUIRE(cudaMemcpy2DAsync(devA, pitchA, a.data.data(), a.w * sizeof(float), a.w * sizeof(float), a.h,
                              cudaMemcpyHostToDevice, stream) == cudaSuccess);
    REQUIRE(cudaMemcpy2DAsync(devB, pitchB, b.data.data(), b.w * sizeof(float), b.w * sizeof(float), b.h,
                              cudaMemcpyHostToDevice, stream) == cudaSuccess);
    const auto dev = render::cudaDisFlowBidirectionalDevice(devA, devB, a.w, a.h, pitchA, params, stream);
    cudaStreamDestroy(stream);
    cudaFree(devA);
    cudaFree(devB);
    REQUIRE(dev.ok());
    const FlowParity p = compareFlows(host.value(), dev.value());
    INFO(p.differingValues << " components differ; consistent " << host.value().consistent << " vs "
                           << dev.value().consistent << "; mask agreement " << p.maskAgreement);
    CHECK(p.identical);
}

TEST_CASE("the ClassicalCuda backend is reached through the factory and falls back cleanly", "[render][flow][cuda]") {
    GrayImage a;
    GrayImage b;
    bandPair(a, b);
    render::FlowBackendParams params;
    params.modelPath = "no_such_flow_model.onnx";  // keep the neural path out of Auto

    SECTION("not installed: unavailable with a reason, and computeFlow uses the CPU") {
        render::uninstallCudaAnalyses();
        const auto backend = render::makeFlowBackend(FlowBackendKind::ClassicalCuda, params);
        REQUIRE(backend != nullptr);
        CHECK_FALSE(backend->isAvailable());
        CHECK(backend->info().kind == FlowBackendKind::ClassicalCuda);
        INFO("detail: " << backend->info().detail);
        CHECK(backend->info().detail.find("installCudaAnalyses") != std::string::npos);
        CHECK(backend->compute(a, b, params, nullptr).code() == ErrorCode::Unsupported);

        FlowBackendKind used = FlowBackendKind::Count;
        const auto flow = render::computeFlow(FlowBackendKind::ClassicalCuda, a, b, params, nullptr, &used);
        REQUIRE(flow.ok());
        CHECK(used == FlowBackendKind::Classical);
    }

    SECTION("installed: it runs, it says so, and it matches the direct call") {
        OSV_REQUIRE_CUDA_ANALYSES();
        InstalledAnalyses installed;
        REQUIRE(installed.status().ok());
        CHECK(render::cudaAnalysesInstalled());
        CHECK(std::string(render::flowBackendName(FlowBackendKind::ClassicalCuda)) == "classical-cuda");

        const auto backend = render::makeFlowBackend(FlowBackendKind::ClassicalCuda, params);
        REQUIRE(backend != nullptr);
        REQUIRE(backend->isAvailable());
        const render::FlowBackendInfo info = backend->info();
        INFO("detail: " << info.detail);
        CHECK(info.kind == FlowBackendKind::ClassicalCuda);
        CHECK(info.available);
        CHECK(info.deterministic);

        FlowBackendKind used = FlowBackendKind::Count;
        const auto viaFactory = render::computeFlow(FlowBackendKind::ClassicalCuda, a, b, params, nullptr, &used);
        REQUIRE(viaFactory.ok());
        CHECK(used == FlowBackendKind::ClassicalCuda);

        // The backend copies the shared tolerance into the solver's params,
        // exactly as the CPU backend does.
        DisFlowParams dis = params.dis;
        dis.consistencyTolPx = params.consistencyTolPx;
        const auto direct = render::cudaDisFlowBidirectional(a, b, dis);
        REQUIRE(direct.ok());
        CHECK(compareFlows(direct.value(), viaFactory.value()).identical);

        // Auto is untouched by the install: it never picks the GPU solver.
        used = FlowBackendKind::Count;
        const auto automatic = render::computeFlow(FlowBackendKind::Auto, a, b, params, nullptr, &used);
        REQUIRE(automatic.ok());
        CHECK(used != FlowBackendKind::ClassicalCuda);
    }

    // Whatever happened above, nothing stays installed for later tests.
    CHECK_FALSE(render::cudaAnalysesInstalled());
}

TEST_CASE("CUDA DIS is deterministic and safe to call from several threads", "[render][flow][cuda]") {
    OSV_REQUIRE_CUDA_ANALYSES();
    GrayImage a;
    GrayImage b;
    bandPair(a, b);
    const DisFlowParams params;
    const auto reference = render::cudaDisFlowBidirectional(a, b, params);
    REQUIRE(reference.ok());

    // More threads than one context has workspaces, so some callers queue
    // for a lease: every one of them must still get the reference field.
    constexpr int kThreads = 6;
    constexpr int kRunsEach = 4;
    std::atomic<int> failures{0};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int r = 0; r < kRunsEach; ++r) {
                const auto flow = render::cudaDisFlowBidirectional(a, b, params);
                if (!flow.ok()) {
                    ++failures;
                    continue;
                }
                const BidirFlow& f = flow.value();
                const BidirFlow& ref = reference.value();
                if (f.forward.u != ref.forward.u || f.forward.v != ref.forward.v || f.backward.u != ref.backward.u ||
                    f.backward.v != ref.backward.v || f.ok != ref.ok || f.consistent != ref.consistent) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(failures.load() == 0);
    CHECK(mismatches.load() == 0);
}

// ===========================================================================
//  Band shading from frames in VRAM
// ===========================================================================

TEST_CASE("without the GPU analyses installed, device frames are still refused", "[render][seam][sample]") {
    // The pre-existing behaviour must survive for any process that never
    // installs the GPU path: a clean InvalidArgument that says what to do,
    // never a read of device memory from the CPU.  The "device" pointers
    // below are host addresses that are never dereferenced - which is the
    // point: if the CPU band path touched them, this test would see garbage
    // or crash rather than a refusal.
    OSV_REQUIRE_SAMPLE();
    render::uninstallCudaAnalyses();
    auto sample = openSample();
    REQUIRE(sample.ok());
    const std::uint32_t w = sample.value().format.lensW();
    const std::uint32_t h = sample.value().format.lensH();
    std::vector<std::uint16_t> fake(16);
    video::FramePair pair;
    for (std::size_t i = 0; i < 2; ++i) {
        pair.lens[i].width = w;
        pair.lens[i].height = h;
        video::DeviceFrameRef& d = pair.device[i];
        d.yDevice = fake.data();
        d.uvDevice = fake.data();
        d.pitchBytes = static_cast<std::size_t>(w) * 2u;
        d.width = w;
        d.height = h;
    }
    ThreadPool pool(2);
    const auto bands = render::renderLensBands(sample.value().rig, pair, geom::BlendParams{}, render::BandParams{},
                                               false, nullptr, pool);
    REQUIRE_FALSE(bands.ok());
    CHECK(bands.error().code == ErrorCode::InvalidArgument);
    INFO(bands.error().message);
    CHECK(bands.error().message.find("installCudaAnalyses") != std::string::npos);
    const auto gain = render::estimateGain(sample.value().rig, pair, geom::BlendParams{}, render::BandParams{}, pool);
    REQUIRE_FALSE(gain.ok());
    CHECK(gain.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("GPU band shading from NVDEC frames matches the CPU band path", "[render][seam][cuda][sample][hwaccel]") {
    OSV_REQUIRE_SAMPLE();
    OSV_REQUIRE_CUDA_ANALYSES();
    auto sample = openSample();
    REQUIRE(sample.ok());
    auto hostReader = video::DualStreamReader::open(osvtest::sampleOsv(), sample.value().format);
    REQUIRE(hostReader.ok());
    auto hostPair = hostReader.value().read(0);
    REQUIRE(hostPair.ok());
    auto devReader = deviceReader(sample.value());
    if (!devReader.ok()) {
        SKIP("NVDEC unavailable: " << devReader.error().message);
    }
    auto devPair = devReader.value().read(0);
    REQUIRE(devPair.ok());
    REQUIRE(devPair.value().onDevice());

    InstalledAnalyses installed;
    REQUIRE(installed.status().ok());
    ThreadPool pool;
    const geom::BlendParams blend;
    const geom::LensRig& rig = sample.value().rig;

    SECTION("luma and coverage bands (seam search, parallax)") {
        const render::BandParams band{2048, 6.0};
        const auto cpu = render::renderLensBands(rig, hostPair.value(), blend, band, false, nullptr, pool);
        const auto gpu = render::renderLensBands(rig, devPair.value(), blend, band, false, nullptr, pool);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        REQUIRE(gpu.value().w == cpu.value().w);
        REQUIRE(gpu.value().h == cpu.value().h);
        REQUIRE(gpu.value().rowOffset == cpu.value().rowOffset);
        for (int lens = 0; lens < 2; ++lens) {
            const auto a = packPlanes(cpu.value().luma[lens], cpu.value().alpha[lens], cpu.value().w, cpu.value().h);
            const auto b = packPlanes(gpu.value().luma[lens], gpu.value().alpha[lens], gpu.value().w, gpu.value().h);
            const render::ImageDiffStats stats = render::compareImages16(a, b);
            INFO("lens " << lens << ": PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }

    SECTION("full RGBA rows with a seam table and a warp grid in force") {
        // Exercise the table uploads: a non-trivial seam table and a small
        // warp grid, the same on both sides.
        geom::EquirectMap map;
        map.layout = geom::EquirectLayout::PolarAxis;
        map.w = 1024;
        map.h = 512;
        std::vector<float> seam(static_cast<std::size_t>(map.w));
        for (std::size_t c = 0; c < seam.size(); ++c) {
            seam[c] = 0.2f * std::sin(0.01f * static_cast<float>(c));
        }
        const std::uint32_t gw = 64;
        const std::uint32_t gh = 12;
        std::vector<float> warp(static_cast<std::size_t>(gw) * gh * 2u);
        for (std::size_t i = 0; i < warp.size(); ++i) {
            warp[i] = 0.002f * std::cos(0.37f * static_cast<float>(i));
        }
        const OsvColorParams cp =
            color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
        const auto build = [&](const video::FramePair& pair, int lens) {
            render::RenderParamsBuilder builder;
            builder.rig(rig).equirect(map).blend(blend, true).color(cp).lensEnabled(1 - lens, false).seam(seam);
            builder.warp(warp, gw, gh, 0.12f, -0.12f);
            return builder.build(pair);
        };
        const std::uint32_t row0 = 220;
        const std::uint32_t row1 = 292;
        for (int lens = 0; lens < 2; ++lens) {
            auto hostJob = build(hostPair.value(), lens);
            auto devJob = build(devPair.value(), lens);
            REQUIRE(hostJob.ok());
            REQUIRE(devJob.ok());
            REQUIRE(devJob.value().planesOnDevice[0]);
            // CPU reference: the full map, then the same rows (shadeRows is
            // documented as bit-identical to a full-map render's rows).
            render::CpuRenderer cpuRenderer(pool);
            auto full = cpuRenderer.render(hostJob.value());
            REQUIRE(full.ok());
            auto rows = render::cudaShadeBandRgba(devJob.value(), row0, row1);
            REQUIRE(rows.ok());
            auto ref = render::ImageRGBAf::create(static_cast<std::uint32_t>(map.w), row1 - row0);
            auto got = render::ImageRGBAf::create(static_cast<std::uint32_t>(map.w), row1 - row0);
            REQUIRE(ref.ok());
            REQUIRE(got.ok());
            const std::size_t rowFloats = static_cast<std::size_t>(map.w) * 4u;
            std::copy_n(full.value().data.begin() + static_cast<std::ptrdiff_t>(row0 * rowFloats),
                        (row1 - row0) * rowFloats, ref.value().data.begin());
            REQUIRE(rows.value().size() == (row1 - row0) * rowFloats);
            std::copy(rows.value().begin(), rows.value().end(), got.value().data.begin());
            const render::ImageDiffStats stats = render::compareImages16(ref.value(), got.value());
            INFO("lens " << lens << " RGBA rows: PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode
                         << " codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }
}

TEST_CASE("seam search, gain estimate and parallax bands work from device-only frames",
          "[render][seam][parallax][cuda][sample][hwaccel]") {
    OSV_REQUIRE_SAMPLE();
    OSV_REQUIRE_CUDA_ANALYSES();
    auto sample = openSample();
    REQUIRE(sample.ok());
    auto hostReader = video::DualStreamReader::open(osvtest::sampleOsv(), sample.value().format);
    REQUIRE(hostReader.ok());
    auto hostPair = hostReader.value().read(0);
    REQUIRE(hostPair.ok());
    auto devReader = deviceReader(sample.value());
    if (!devReader.ok()) {
        SKIP("NVDEC unavailable: " << devReader.error().message);
    }
    auto devPair = devReader.value().read(0);
    REQUIRE(devPair.ok());
    // Device-only: the frames have a size and device planes, no host planes.
    REQUIRE(devPair.value().onDevice());
    REQUIRE_FALSE(devPair.value().lens[0].valid());

    InstalledAnalyses installed;
    REQUIRE(installed.status().ok());
    ThreadPool pool;
    const geom::BlendParams blend;
    const geom::LensRig& rig = sample.value().rig;

    SECTION("searchSeam") {
        render::SeamSearchParams sp;
        const auto cpu = render::searchSeam(rig, hostPair.value(), blend, sp, pool);
        const auto gpu = render::searchSeam(rig, devPair.value(), blend, sp, pool);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        REQUIRE(gpu.value().columns == cpu.value().columns);
        double sumDiff = 0.0;
        double maxDiff = 0.0;
        for (std::size_t c = 0; c < cpu.value().shiftDeg.size(); ++c) {
            const double d = std::fabs(static_cast<double>(cpu.value().shiftDeg[c]) - gpu.value().shiftDeg[c]);
            sumDiff += d;
            maxDiff = std::max(maxDiff, d);
        }
        const double meanDiff = sumDiff / static_cast<double>(cpu.value().shiftDeg.size());
        INFO("accepted " << cpu.value().acceptedColumns << " / " << gpu.value().acceptedColumns << " columns, mean NCC "
                         << cpu.value().meanNcc << " / " << gpu.value().meanNcc << ", seam table mean |diff| "
                         << meanDiff << " deg (max " << maxDiff << ")");
        CHECK(std::abs(static_cast<long long>(cpu.value().acceptedColumns) -
                       static_cast<long long>(gpu.value().acceptedColumns)) <=
              static_cast<long long>(cpu.value().columns / 100));
        CHECK_THAT(gpu.value().meanNcc, Catch::Matchers::WithinAbs(cpu.value().meanNcc, 1e-3));
        CHECK(meanDiff < 0.01);
    }

    SECTION("estimateGain") {
        const render::BandParams band{1024, 4.0};
        const auto cpu = render::estimateGain(rig, hostPair.value(), blend, band, pool);
        const auto gpu = render::estimateGain(rig, devPair.value(), blend, band, pool);
        REQUIRE(cpu.ok());
        REQUIRE(gpu.ok());
        INFO("samples " << cpu.value().samples << " / " << gpu.value().samples);
        CHECK(gpu.value().samples == cpu.value().samples);
        for (int lens = 0; lens < 2; ++lens) {
            const Vec3d& c = cpu.value().gain[lens];
            const Vec3d& g = gpu.value().gain[lens];
            const double cv[3] = {c.x, c.y, c.z};
            const double gv[3] = {g.x, g.y, g.z};
            for (int k = 0; k < 3; ++k) {
                INFO("lens " << lens << " channel " << k << ": " << cv[k] << " vs " << gv[k]);
                CHECK_THAT(gv[k], Catch::Matchers::WithinRel(cv[k], 1e-4));
            }
        }
    }

    SECTION("measureParallaxBands, then the GPU flow") {
        render::ParallaxWarpParams pw;
        pw.backend = FlowBackendKind::ClassicalCuda;
        const auto cpuBands = render::measureParallaxBands(rig, hostPair.value(), blend, pw, nullptr, pool);
        const auto gpuBands = render::measureParallaxBands(rig, devPair.value(), blend, pw, nullptr, pool);
        REQUIRE(cpuBands.ok());
        REQUIRE(gpuBands.ok());
        for (int lens = 0; lens < 2; ++lens) {
            const auto a = packPlanes(cpuBands.value().luma[lens], cpuBands.value().alpha[lens], cpuBands.value().w,
                                      cpuBands.value().h);
            const auto b = packPlanes(gpuBands.value().luma[lens], gpuBands.value().alpha[lens], gpuBands.value().w,
                                      gpuBands.value().h);
            const render::ImageDiffStats stats = render::compareImages16(a, b);
            INFO("lens " << lens << ": PSNR " << stats.psnrDb << " dB");
            CHECK(stats.psnrDb >= 60.0);
        }
        const auto grid = render::parallaxFromBands(gpuBands.value(), pw, nullptr);
        REQUIRE(grid.ok());
        CHECK(grid.value().usedBackend == FlowBackendKind::ClassicalCuda);
        CHECK(grid.value().valid());
    }
}

// ===========================================================================
//  Parallax grid from GPU flow
// ===========================================================================

TEST_CASE("the parallax grid from GPU flow matches the CPU grid on frames 0, 32 and 64",
          "[render][parallax][cuda][sample]") {
    OSV_REQUIRE_SAMPLE();
    OSV_REQUIRE_CUDA_ANALYSES();
    auto sample = openSample();
    REQUIRE(sample.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), sample.value().format);
    REQUIRE(reader.ok());
    InstalledAnalyses installed;
    REQUIRE(installed.status().ok());
    ThreadPool pool;
    const geom::BlendParams blend;

    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        INFO("frame " << frame);
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());

        render::ParallaxWarpParams cpuParams;
        cpuParams.backend = FlowBackendKind::Classical;
        render::ParallaxWarpParams gpuParams = cpuParams;
        gpuParams.backend = FlowBackendKind::ClassicalCuda;

        const auto bands = render::measureParallaxBands(sample.value().rig, pair.value(), blend, cpuParams, nullptr,
                                                        pool);
        REQUIRE(bands.ok());

        // The flow itself, on the real bands.
        GrayImage a;
        a.w = bands.value().w;
        a.h = bands.value().h;
        a.data = bands.value().luma[0];
        GrayImage b = a;
        b.data = bands.value().luma[1];
        FlowBackendKind usedCpu = FlowBackendKind::Count;
        FlowBackendKind usedGpu = FlowBackendKind::Count;
        const auto cpuFlow = render::computeFlow(FlowBackendKind::Classical, a, b, cpuParams.flow, &pool, &usedCpu);
        const auto gpuFlow = render::computeFlow(FlowBackendKind::ClassicalCuda, a, b, gpuParams.flow, &pool, &usedGpu);
        REQUIRE(cpuFlow.ok());
        REQUIRE(gpuFlow.ok());
        REQUIRE(usedCpu == FlowBackendKind::Classical);
        REQUIRE(usedGpu == FlowBackendKind::ClassicalCuda);
        checkFlowParity(compareFlows(cpuFlow.value(), gpuFlow.value()));

        // And the grid built from each.
        const auto cpuGrid = render::parallaxFromBands(bands.value(), cpuParams, &pool);
        const auto gpuGrid = render::parallaxFromBands(bands.value(), gpuParams, &pool);
        REQUIRE(cpuGrid.ok());
        REQUIRE(gpuGrid.ok());
        const render::ParallaxWarpGrid& c = cpuGrid.value();
        const render::ParallaxWarpGrid& g = gpuGrid.value();
        CHECK(g.usedBackend == FlowBackendKind::ClassicalCuda);
        INFO("gated cells " << c.gatedCells << " / " << g.gatedCells << " of " << c.measuredCells << " / "
                            << g.measuredCells << "; mean |correction| " << c.meanAbsCorrectionDeg << " / "
                            << g.meanAbsCorrectionDeg << " deg; consistent " << c.consistentPixels << " / "
                            << g.consistentPixels);
        const double gatedTol = std::max(1.0, 0.02 * static_cast<double>(c.gatedCells));
        CHECK(std::fabs(static_cast<double>(g.gatedCells) - static_cast<double>(c.gatedCells)) <= gatedTol);
        CHECK_THAT(g.meanAbsCorrectionDeg,
                   Catch::Matchers::WithinRel(c.meanAbsCorrectionDeg, 0.05) ||
                       Catch::Matchers::WithinAbs(c.meanAbsCorrectionDeg, 1e-6));
    }
}

#endif  // OSV_HAVE_CUDA
