// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_flowbackend.cpp - backend selection, fallback, and the neural path.
//
// Two groups:
//
//   [flow]          Run everywhere.  They check the contract FlowBackend.h
//                   promises regardless of what is installed: the factory
//                   never returns null, an unavailable backend says why, and
//                   computeFlow() falls back to the classical solver and
//                   reports that it did.
//
//   [flow][model]   Need the neural model AND a machine that can run it
//                   (ONNX Runtime built in, CUDA present).  They SKIP with
//                   the backend's own reason otherwise, the same way [sample]
//                   tests skip without footage.  Set OSV_REQUIRE_NEURAL=1 to
//                   turn that skip into a failure - on a GPU CI machine a
//                   silent skip would hide a broken loader.
//
// The model is found where production finds it (models/ beside the test
// executable, staged by the build) unless OSV_FLOW_MODEL names a file.
//
// As in test_disflow.cpp, every pair is synthesised with a displacement known
// by construction, so the assertions are against ground truth.  A flow
// network that has been exported with the wrong normalisation or channel
// order still returns a smooth, plausible-looking field; only ground truth
// catches that.
//
// A hidden case prints the neural-vs-classical accuracy table and the
// latency figures quoted in FlowBackendOnnx.cpp.  It has its own tag only,
// because Catch2 runs a hidden test whenever a filter names any of its tags:
//     osv_tests.exe "[flowbench]"

#include "osv/render/FlowBackend.h"

#include "TestSample.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using osv::ErrorCode;
using osv::render::BidirFlow;
using osv::render::computeFlow;
using osv::render::FlowBackend;
using osv::render::FlowBackendKind;
using osv::render::FlowBackendParams;
using osv::render::FlowField;
using osv::render::GrayImage;
using osv::render::haveNeuralFlowBackend;
using osv::render::makeFlowBackend;

namespace {

// ---------------------------------------------------------------------------
//  Synthetic data
// ---------------------------------------------------------------------------

/// A band-limited random texture in [0, 1].
///
/// Random rather than the summed sinusoids test_disflow.cpp uses, on
/// purpose: a learned model has a prior about what real images look like,
/// and strongly periodic patterns are exactly where it is weakest (measured:
/// a 16-px checker defeats it outright).  Real overlap bands are not
/// periodic, so this is the fair test; the periodic weakness is recorded in
/// the benchmark instead.  Deterministic (fixed LCG) so runs are comparable.
GrayImage texture(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    GrayImage img;
    img.w = w;
    img.h = h;
    img.data.resize(static_cast<std::size_t>(w) * h);
    std::uint64_t state = 0x9E3779B97F4A7C15ull ^ seed;
    for (float& v : img.data) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        v = static_cast<float>((state >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
    }
    // Three passes of a 3x3 box blur (clamped), which is close to a Gaussian
    // and leaves structure at a few pixels' scale - enough to constrain the
    // flow everywhere without single-pixel noise.
    std::vector<float> tmp(img.data.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        sum += img.at(static_cast<int>(x) + dx, static_cast<int>(y) + dy);
                    }
                }
                tmp[static_cast<std::size_t>(y) * w + x] = sum / 9.0f;
            }
        }
        img.data.swap(tmp);
    }
    // Stretch back to the full range; blurring compresses it towards 0.5.
    const auto [lo, hi] = std::minmax_element(img.data.begin(), img.data.end());
    const float lov = *lo;
    const float span = std::max(*hi - *lo, 1e-6f);
    for (float& v : img.data) {
        v = (v - lov) / span;
    }
    return img;
}

/// dst(p) = src(p - d): content moves BY (dx, dy), so the true flow from
/// src to dst is exactly (dx, dy) and from dst to src exactly -(dx, dy).
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

/// End-point error statistics over the interior of a field.
struct Epe {
    double mean = 0.0;
    double p95 = 0.0;
    double max = 0.0;
    double medianU = 0.0;
    double medianV = 0.0;
    double okFraction = 0.0;  ///< consistency-mask pass rate over the same region
};

/// Compare `f` against a uniform truth (dx, dy), ignoring `margin` pixels at
/// every border: clamp-to-edge synthesis makes the true motion there
/// undefined for any method.
Epe measure(const FlowField& f, double dx, double dy, int margin, const std::vector<std::uint8_t>* ok = nullptr) {
    Epe e;
    std::vector<double> errs, us, vs;
    std::size_t okCount = 0;
    const int w = static_cast<int>(f.w);
    const int h = static_cast<int>(f.h);
    for (int y = margin; y < h - margin; ++y) {
        for (int x = margin; x < w - margin; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * f.w + static_cast<std::size_t>(x);
            errs.push_back(std::hypot(static_cast<double>(f.u[i]) - dx, static_cast<double>(f.v[i]) - dy));
            us.push_back(f.u[i]);
            vs.push_back(f.v[i]);
            if (ok != nullptr && i < ok->size() && (*ok)[i] != 0) {
                ++okCount;
            }
        }
    }
    if (errs.empty()) {
        return e;
    }
    double sum = 0.0;
    for (double v : errs) {
        sum += v;
        e.max = std::max(e.max, v);
    }
    e.mean = sum / static_cast<double>(errs.size());
    const auto pct = [](std::vector<double>& v, double q) {
        const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    };
    e.p95 = pct(errs, 0.95);
    e.medianU = pct(us, 0.5);
    e.medianV = pct(vs, 0.5);
    e.okFraction = static_cast<double>(okCount) / static_cast<double>(errs.size());
    return e;
}

/// True when no component of either field is NaN or infinite.
bool allFinite(const BidirFlow& f) {
    for (const FlowField* ff : {&f.forward, &f.backward}) {
        for (std::size_t i = 0; i < ff->u.size(); ++i) {
            if (!std::isfinite(ff->u[i]) || !std::isfinite(ff->v[i])) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Model location and the skip guard
// ---------------------------------------------------------------------------

/// Parameters pointing at the model the [model] tests use: OSV_FLOW_MODEL
/// when set, otherwise empty so the backend resolves its own default.
FlowBackendParams modelParams() {
    FlowBackendParams p;
    if (const char* env = std::getenv("OSV_FLOW_MODEL")) {
        p.modelPath = env;
    }
    return p;
}

/// Parameters pointing at a model that certainly does not exist.
FlowBackendParams missingModelParams() {
    FlowBackendParams p;
    p.modelPath = (osvtest::tempDir() / "no_such_flow_model.onnx").string();
    return p;
}

bool neuralRequired() {
    const char* env = std::getenv("OSV_REQUIRE_NEURAL");
    return env != nullptr && env[0] == '1';
}

/// SKIP (or FAIL under OSV_REQUIRE_NEURAL=1) unless `backend` can run.
#define OSV_REQUIRE_NEURAL_BACKEND(backend)                                                                            \
    do {                                                                                                               \
        if (!haveNeuralFlowBackend()) {                                                                                \
            if (neuralRequired()) {                                                                                    \
                FAIL("OSV_REQUIRE_NEURAL=1 but this build has no ONNX Runtime");                                       \
            }                                                                                                          \
            SKIP("built without ONNX Runtime");                                                                        \
        }                                                                                                              \
        if (!(backend) || !(backend)->isAvailable()) {                                                                 \
            const std::string why_ = (backend) ? (backend)->info().detail : std::string("null backend");               \
            if (neuralRequired()) {                                                                                    \
                FAIL("OSV_REQUIRE_NEURAL=1 but the neural backend is unavailable: " << why_);                          \
            }                                                                                                          \
            SKIP("neural flow backend unavailable: " << why_);                                                         \
        }                                                                                                              \
    } while (0)

}  // namespace

// ===========================================================================
//  Contract tests - run everywhere
// ===========================================================================

TEST_CASE("makeFlowBackend never returns null and describes itself", "[render][flow]") {
    const FlowBackendParams params = modelParams();
    const FlowBackendKind kinds[] = {
        FlowBackendKind::Auto,  FlowBackendKind::Classical,       FlowBackendKind::Neural,
        FlowBackendKind::Count, static_cast<FlowBackendKind>(99), static_cast<FlowBackendKind>(-1)};
    for (FlowBackendKind kind : kinds) {
        INFO("kind " << static_cast<int>(kind));
        const std::unique_ptr<FlowBackend> backend = makeFlowBackend(kind, params);
        REQUIRE(backend != nullptr);
        const auto info = backend->info();
        CHECK(info.available == backend->isAvailable());
        CHECK_FALSE(info.detail.empty());
        switch (kind) {
        case FlowBackendKind::Classical:
            CHECK(info.kind == FlowBackendKind::Classical);
            CHECK(info.available);
            CHECK(info.deterministic);
            break;
        case FlowBackendKind::Neural:
            CHECK(info.kind == FlowBackendKind::Neural);
            break;
        default:
            // Auto resolves to something that runs; a corrupt kind falls
            // back to the classical solver rather than refusing.
            CHECK(info.available);
            CHECK((info.kind == FlowBackendKind::Classical || info.kind == FlowBackendKind::Neural));
            break;
        }
    }
}

TEST_CASE("an unavailable neural backend says why and refuses cleanly", "[render][flow]") {
    const FlowBackendParams params = missingModelParams();
    const std::unique_ptr<FlowBackend> backend = makeFlowBackend(FlowBackendKind::Neural, params);
    REQUIRE(backend != nullptr);
    CHECK_FALSE(backend->isAvailable());
    const auto info = backend->info();
    CHECK(info.kind == FlowBackendKind::Neural);
    CHECK_FALSE(info.available);
    INFO("detail: " << info.detail);
    if (haveNeuralFlowBackend()) {
        // The reason must name the file it looked for, not merely fail.
        CHECK(info.detail.find("no_such_flow_model.onnx") != std::string::npos);
    } else {
        CHECK(info.detail.find("ONNX Runtime") != std::string::npos);
    }

    // Asking anyway is a clean Unsupported, not a crash or a half result.
    const GrayImage a = texture(160, 128, 1);
    const GrayImage b = shifted(a, 2.0, 1.0);
    const auto result = backend->compute(a, b, params, nullptr);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::Unsupported);
}

TEST_CASE("a corrupt model file is reported, not crashed on", "[render][flow]") {
    // Garbage with the right extension: exercises the path where the model
    // exists, ONNX Runtime loads, and session creation is what fails.
    const std::filesystem::path bad = osvtest::tempDir() / "corrupt_flow_model.onnx";
    {
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        std::uint32_t state = 12345u;
        for (int i = 0; i < 4096; ++i) {
            state = state * 1664525u + 1013904223u;
            out.put(static_cast<char>(state >> 24));
        }
    }
    FlowBackendParams params;
    params.modelPath = bad.string();
    const std::unique_ptr<FlowBackend> neural = makeFlowBackend(FlowBackendKind::Neural, params);
    REQUIRE(neural != nullptr);
    CHECK_FALSE(neural->isAvailable());
    CHECK_FALSE(neural->info().detail.empty());
    INFO("detail: " << neural->info().detail);

    // Auto must not be sunk by a broken model: it resolves to classical.
    const std::unique_ptr<FlowBackend> automatic = makeFlowBackend(FlowBackendKind::Auto, params);
    REQUIRE(automatic != nullptr);
    CHECK(automatic->isAvailable());
    CHECK(automatic->info().kind == FlowBackendKind::Classical);
}

TEST_CASE("computeFlow falls back to the classical solver and says so", "[render][flow]") {
    const GrayImage a = texture(192, 96, 2);
    const double dx = 2.5;
    const double dy = -1.0;
    const GrayImage b = shifted(a, dx, dy);
    const FlowBackendParams params = missingModelParams();

    for (FlowBackendKind asked : {FlowBackendKind::Neural, FlowBackendKind::Auto, FlowBackendKind::Classical}) {
        INFO("asked for " << osv::render::flowBackendName(asked));
        FlowBackendKind used = FlowBackendKind::Count;  // sentinel: must be overwritten
        const auto result = computeFlow(asked, a, b, params, nullptr, &used);
        REQUIRE(result.ok());
        CHECK(used == FlowBackendKind::Classical);
        const BidirFlow& flow = result.value();
        REQUIRE(flow.valid());
        CHECK(allFinite(flow));
        // The fallback is the real solver, not a zero field.
        const Epe e = measure(flow.forward, dx, dy, 12);
        CHECK(std::abs(e.medianU - dx) < 0.5);
        CHECK(std::abs(e.medianV - dy) < 0.5);
    }

    // usedKind is optional.
    const auto noKind = computeFlow(FlowBackendKind::Neural, a, b, params, nullptr, nullptr);
    CHECK(noKind.ok());
}

// ===========================================================================
//  Neural path - need the model and a GPU
// ===========================================================================

TEST_CASE("computeFlow reports the neural backend when it ran", "[render][flow][model]") {
    const FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> probe = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(probe);

    const GrayImage a = texture(512, 160, 3);
    const GrayImage b = shifted(a, 3.0, 1.0);
    for (FlowBackendKind asked : {FlowBackendKind::Neural, FlowBackendKind::Auto}) {
        FlowBackendKind used = FlowBackendKind::Count;
        const auto result = computeFlow(asked, a, b, params, nullptr, &used);
        REQUIRE(result.ok());
        CHECK(used == FlowBackendKind::Neural);
    }
}

TEST_CASE("the neural backend recovers a known translation", "[render][flow][model]") {
    const FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> backend = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(backend);
    INFO("backend: " << backend->info().detail);

    struct Case {
        std::uint32_t w, h;
        double dx, dy;
    };
    // 2048 x 68 is the shape ParallaxWarp actually hands over (a 2048-wide
    // polar map, +/-6 degrees): short enough that the backend must upscale
    // it before the network can run at all.  Sub-pixel shifts on purpose.
    const Case cases[] = {
        {2048, 68, 2.5, -1.25}, {2048, 68, -6.0, 2.0}, {640, 160, 11.5, 3.0}, {512, 256, -3.25, -4.5}};
    osv::ThreadPool pool(4);
    for (const Case& c : cases) {
        INFO(c.w << "x" << c.h << " shift (" << c.dx << ", " << c.dy << ")");
        const GrayImage a = texture(c.w, c.h, c.w + c.h);
        const GrayImage b = shifted(a, c.dx, c.dy);
        const auto result = backend->compute(a, b, params, &pool);
        REQUIRE(result.ok());
        const BidirFlow& flow = result.value();
        REQUIRE(flow.valid());
        REQUIRE(flow.forward.w == c.w);
        REQUIRE(flow.forward.h == c.h);
        CHECK(allFinite(flow));

        const int margin = static_cast<int>(std::ceil(std::max(std::abs(c.dx), std::abs(c.dy)))) + 8;
        const Epe fwd = measure(flow.forward, c.dx, c.dy, margin, &flow.ok);
        const Epe bwd = measure(flow.backward, -c.dx, -c.dy, margin);
        INFO("forward EPE mean " << fwd.mean << " p95 " << fwd.p95 << " median (" << fwd.medianU << ", " << fwd.medianV
                                 << "); backward EPE mean " << bwd.mean << "; consistent " << fwd.okFraction);
        // A fraction of a pixel, in both directions, and the two directions
        // agree with each other almost everywhere.
        //
        // The bounds are what the network delivers, not what DIS delivers:
        // SEA-RAFT shows a case-dependent sub-pixel bias on synthetic pairs
        // (measured up to 0.22 px on the median of the 512 x 256 case, in
        // one direction only, with no resampling or tiling involved).  Every
        // export defect this test exists to catch - wrong normalisation,
        // channel order, sign or scale - produces errors of whole pixels.
        CHECK(std::abs(fwd.medianU - c.dx) < 0.3);
        CHECK(std::abs(fwd.medianV - c.dy) < 0.3);
        CHECK(fwd.mean < 0.35);
        CHECK(fwd.p95 < 0.6);
        CHECK(bwd.mean < 0.35);
        CHECK(fwd.okFraction > 0.95);
    }
}

TEST_CASE("tiled neural flow has no seams", "[render][flow][model]") {
    FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> backend = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(backend);

    // 4000 x 96 is upscaled to ~10,670 x 256 working pixels: six tiles
    // across at the default 2048 edge, and more at the smaller edge below.
    const double dx = 4.0;
    const double dy = -1.5;
    const GrayImage a = texture(4000, 96, 7);
    const GrayImage b = shifted(a, dx, dy);
    osv::ThreadPool pool(4);

    for (int edge : {2048, 512}) {
        params.maxTileEdge = edge;
        INFO("maxTileEdge " << edge);
        const auto result = backend->compute(a, b, params, &pool);
        REQUIRE(result.ok());
        const FlowField& f = result.value().forward;
        REQUIRE(f.valid());

        // Error per 50-column block: a seam shows up as one bad block even
        // when the band-wide mean looks fine.
        const int margin = 10;
        double worstBlock = 0.0;
        for (std::uint32_t x0 = margin; x0 + 50 < f.w - margin; x0 += 50) {
            double sum = 0.0;
            std::size_t n = 0;
            for (std::uint32_t y = margin; y < f.h - margin; ++y) {
                for (std::uint32_t x = x0; x < x0 + 50; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * f.w + x;
                    sum += std::hypot(static_cast<double>(f.u[i]) - dx, static_cast<double>(f.v[i]) - dy);
                    ++n;
                }
            }
            worstBlock = std::max(worstBlock, sum / static_cast<double>(n));
        }
        INFO("worst 50-column block mean EPE " << worstBlock);
        CHECK(worstBlock < 0.35);
    }
}

TEST_CASE("the neural backend validates its inputs and survives flat content", "[render][flow][model]") {
    const FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> backend = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(backend);

    const GrayImage a = texture(256, 128, 4);
    GrayImage smaller = texture(128, 128, 4);
    GrayImage empty;
    CHECK(backend->compute(a, smaller, params, nullptr).code() == ErrorCode::InvalidArgument);
    CHECK(backend->compute(empty, empty, params, nullptr).code() == ErrorCode::InvalidArgument);
    GrayImage malformed = a;
    malformed.data.resize(10);
    CHECK(backend->compute(malformed, a, params, nullptr).code() == ErrorCode::InvalidArgument);

    // Featureless input has no answer, but it must still produce a finite
    // field of the right size rather than NaNs.
    GrayImage flat;
    flat.w = 300;
    flat.h = 60;
    flat.data.assign(static_cast<std::size_t>(flat.w) * flat.h, 0.5f);
    const auto flatResult = backend->compute(flat, flat, params, nullptr);
    REQUIRE(flatResult.ok());
    CHECK(flatResult.value().valid());
    CHECK(allFinite(flatResult.value()));

    // Out-of-range and non-finite input values are sanitised, not propagated.
    GrayImage hot = a;
    hot.data[10] = std::nanf("");
    hot.data[20] = 7.0f;
    hot.data[30] = -3.0f;
    const auto hotResult = backend->compute(hot, a, params, nullptr);
    REQUIRE(hotResult.ok());
    CHECK(allFinite(hotResult.value()));
}

TEST_CASE("the neural backend is safe to call from several threads", "[render][flow][model]") {
    // Premiere renders frames on many threads at once and each can reach
    // computeFlow(); the shared session must serialise, not corrupt.
    const FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> probe = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(probe);

    const GrayImage a = texture(1024, 96, 5);
    const GrayImage b = shifted(a, 3.0, 0.5);
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            FlowBackendKind used = FlowBackendKind::Count;
            const auto r = computeFlow(FlowBackendKind::Neural, a, b, params, nullptr, &used);
            if (r.ok() && used == FlowBackendKind::Neural && allFinite(r.value())) {
                const Epe e = measure(r.value().forward, 3.0, 0.5, 12);
                if (e.mean < 0.3) {
                    ++good;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(good.load() == 4);
}

// ===========================================================================
//  Benchmark - hidden; prints the numbers quoted in FlowBackendOnnx.cpp
// ===========================================================================

TEST_CASE("neural vs classical flow: accuracy and latency", "[.flowbench]") {
    const FlowBackendParams params = modelParams();
    const std::unique_ptr<FlowBackend> neural = makeFlowBackend(FlowBackendKind::Neural, params);
    OSV_REQUIRE_NEURAL_BACKEND(neural);
    const std::unique_ptr<FlowBackend> classical = makeFlowBackend(FlowBackendKind::Classical, params);
    REQUIRE(classical != nullptr);
    osv::ThreadPool pool(0);

    std::printf("\nneural: %s\n", neural->info().detail.c_str());
    std::printf("\n%-28s %-16s %-30s %-30s\n", "pair", "truth", "neural EPE mean/p95 (ok%)",
                "classical EPE mean/p95 (ok%)");
    struct Acc {
        const char* label;
        std::uint32_t w, h;
        double dx, dy;
        bool periodic;
    };
    const Acc acc[] = {
        {"band 2048x68", 2048, 68, 2.5, -1.25, false},          {"band 2048x68", 2048, 68, -6.0, 2.0, false},
        {"band 2048x68", 2048, 68, 12.0, -3.0, false},          {"strip 1024x256 large", 1024, 256, 18.0, -7.0, false},
        {"strip 1024x256 small", 1024, 256, 0.75, 0.25, false}, {"periodic 1024x256", 1024, 256, 5.0, 2.0, true},
    };
    for (const Acc& c : acc) {
        GrayImage a;
        if (c.periodic) {
            // 16-px checker of sinusoids: every 16-px offset matches equally.
            a.w = c.w;
            a.h = c.h;
            a.data.resize(static_cast<std::size_t>(c.w) * c.h);
            for (std::uint32_t y = 0; y < c.h; ++y) {
                for (std::uint32_t x = 0; x < c.w; ++x) {
                    a.data[static_cast<std::size_t>(y) * c.w + x] = static_cast<float>(
                        0.5 + 0.4 * std::sin(2.0 * 3.14159265 * x / 16.0) * std::sin(2.0 * 3.14159265 * y / 16.0));
                }
            }
        } else {
            a = texture(c.w, c.h, c.w * 7 + c.h);
        }
        const GrayImage b = shifted(a, c.dx, c.dy);
        const int margin = static_cast<int>(std::ceil(std::max(std::abs(c.dx), std::abs(c.dy)))) + 8;
        const auto rn = neural->compute(a, b, params, &pool);
        const auto rc = classical->compute(a, b, params, &pool);
        REQUIRE(rn.ok());
        REQUIRE(rc.ok());
        const Epe en = measure(rn.value().forward, c.dx, c.dy, margin, &rn.value().ok);
        const Epe ec = measure(rc.value().forward, c.dx, c.dy, margin, &rc.value().ok);
        char truth[32];
        std::snprintf(truth, sizeof truth, "(%+.2f,%+.2f)", c.dx, c.dy);
        std::printf("%-28s %-16s %7.3f / %7.3f (%5.1f%%)      %7.3f / %7.3f (%5.1f%%)\n", c.label, truth, en.mean,
                    en.p95, 100.0 * en.okFraction, ec.mean, ec.p95, 100.0 * ec.okFraction);
    }

    // Latency: both directions + consistency mask, i.e. one compute() call,
    // after a warm-up call per size (the first run of a new tile shape pays
    // for cuDNN algorithm selection).
    std::printf("\n%-14s %-24s %-24s\n", "size", "neural median/min ms", "classical median/min ms");
    const std::uint32_t sizes[][2] = {{2048, 68}, {3000, 200}, {7680, 768}};
    for (const auto& s : sizes) {
        const GrayImage a = texture(s[0], s[1], 11);
        const GrayImage b = shifted(a, 3.0, 1.0);
        const auto time = [&](FlowBackend& be, int runs, double& median, double& best) {
            std::vector<double> ms;
            for (int i = 0; i < runs; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto r = be.compute(a, b, params, &pool);
                ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                REQUIRE(r.ok());
            }
            std::sort(ms.begin(), ms.end());
            median = ms[ms.size() / 2];
            best = ms.front();
        };
        const auto w0 = std::chrono::steady_clock::now();
        (void)neural->compute(a, b, params, &pool);  // warm-up
        const double warm = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
        double nm = 0, nb = 0, cm = 0, cb = 0;
        time(*neural, 5, nm, nb);
        time(*classical, s[0] * s[1] > 1000000 ? 2 : 5, cm, cb);
        std::printf("%5ux%-8u %8.1f / %8.1f        %8.1f / %8.1f    (neural first call %.0f ms)\n", s[0], s[1], nm, nb,
                    cm, cb, warm);
    }
}
