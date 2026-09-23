// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaDisFlow.cpp - host side of the CUDA Dense Inverse Search solver, and the
// FlowBackendKind::ClassicalCuda backend built on it.
//
// The kernels live in CudaDisKernel.cu; this file plans the pyramid and the
// patch grids exactly as DisFlow.cpp does, builds the Gaussian taps with
// DisFlow.cpp's own arithmetic, validates inputs with DisFlow.cpp's rules,
// and runs the stages in DisFlow.cpp's order:
//
//   upload + intensity scale -> pyramid (pre-blur + 2x2 box) of BOTH images
//   for each level, coarse to fine:
//       gradients -> tensors -> seeded solve -> densify -> smooth
//   forward-backward consistency -> download
//
// Two differences from the CPU are structural, not numerical.  The CPU
// solver builds each image's pyramid twice (once per direction); here both
// directions share one pyramid per image, because it is the same pyramid.
// And every stage processes both directions in one launch, since the whole
// problem is small enough that launches, not arithmetic, set the pace.

#include "osv/render/CudaAnalysis.h"

#include "CudaAnalysisInternal.h"
#include "CudaAnalysisLaunch.h"
#include "CudaWorkspace.h"

#include "osv/core/Log.h"
#include "osv/render/FlowBackend.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace osv::render {

namespace {

using gpu::DisBlurTaps;
using gpu::DisGrid;
using gpu::DisPair;
using gpu::DisPairW;
using gpu::DisPatch;
using gpu::DisPatchPair;
using gpu::DisPlaneBatch;
using gpu::DisSolveConsts;
using gpu::Workspace;
using gpu::WorkspaceLease;
using gpu::cudaMessage;

/// DisFlow.cpp's kMaxEdge: past this the patch-grid arithmetic overflows.
constexpr std::uint32_t kMaxEdge = 1u << 16;

/// DisFlow.cpp's kMaxLevels: a hard ceiling on pyramid depth.
constexpr int kMaxLevels = 8;

/// DisFlow.cpp halfScale's pre-blur sigma for a 2x decimation.
constexpr double kPyramidSigma = 0.8;

/// One pyramid level: size and offset of its plane inside the pyramid block.
struct Level {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::size_t offset = 0;  ///< In floats from the start of the pyramid block.

    [[nodiscard]] std::size_t pixels() const noexcept { return static_cast<std::size_t>(w) * h; }
};

/// What building a Gaussian for one plane size came to.
enum class TapsOutcome {
    Ok,       ///< Blur with the taps.
    Skip,     ///< DisFlow.cpp's blurPlane would return without blurring.
    TooWide,  ///< Needs more taps than a launch carries; refuse the solve.
};

/// DisFlow.cpp blurPlane's kernel construction, statement for statement -
/// the same double exp, the same float taps, the same float multiply by the
/// float reciprocal of the double sum - so both backends blur with identical
/// weights.  Also applies blurPlane's two early-outs for this plane size.
TapsOutcome buildTaps(double sigma, std::uint32_t w, std::uint32_t h, DisBlurTaps& taps) {
    taps = DisBlurTaps{};
    if (sigma <= 0.0 || w == 0 || h == 0) {
        return TapsOutcome::Skip;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    if (static_cast<std::uint32_t>(radius) >= w && static_cast<std::uint32_t>(radius) >= h) {
        return TapsOutcome::Skip;  // kernel wider than the image: the CPU leaves it untouched
    }
    if (radius > gpu::kDisMaxBlurRadius) {
        return TapsOutcome::TooWide;
    }
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double weight = std::exp(-static_cast<double>(i) * static_cast<double>(i) * inv2s2);
        taps.k[i + radius] = static_cast<float>(weight);
        sum += weight;
    }
    if (!(sum > 0.0)) {
        return TapsOutcome::Skip;  // cannot normalise; the CPU leaves the plane untouched
    }
    const float invSum = static_cast<float>(1.0 / sum);
    for (int i = 0; i < 2 * radius + 1; ++i) {
        taps.k[i] *= invSum;
    }
    taps.radius = radius;
    return TapsOutcome::Ok;
}

/// DisFlow.cpp buildPyramid's level sizes: halve until a level would drop
/// below kMinPyramidEdge on either axis, at most min(levels, kMaxLevels).
std::vector<Level> planPyramid(std::uint32_t w, std::uint32_t h, int levels) {
    std::vector<Level> out;
    out.push_back(Level{w, h, 0});
    const int wanted = std::min(std::max(1, levels), kMaxLevels);
    for (int level = 1; level < wanted; ++level) {
        const Level& prev = out.back();
        if (prev.w / 2 < kMinPyramidEdge || prev.h / 2 < kMinPyramidEdge) {
            break;
        }
        out.push_back(Level{prev.w / 2, prev.h / 2, prev.offset + prev.pixels()});
    }
    return out;
}

/// DisFlow.cpp precomputeTensors's grid layout for a w x h level; cols and
/// rows stay 0 when the level cannot hold a single patch.
DisGrid planGrid(std::uint32_t w, std::uint32_t h, const DisFlowParams& params) {
    DisGrid grid;
    grid.ps = std::max(2, params.patchSize);
    grid.half = grid.ps / 2;
    grid.stride = std::max(1, params.patchStridePx);
    grid.cols = 0;
    grid.rows = 0;
    if (w < static_cast<std::uint32_t>(grid.ps) || h < static_cast<std::uint32_t>(grid.ps)) {
        return grid;
    }
    const int lastX = static_cast<int>(w) - grid.half - 1;
    const int lastY = static_cast<int>(h) - grid.half - 1;
    if (lastX < grid.half || lastY < grid.half) {
        return grid;
    }
    grid.cols = (lastX - grid.half) / grid.stride + 1;
    grid.rows = (lastY - grid.half) / grid.stride + 1;
    return grid;
}

/// Turn a launch status into a Status naming the stage.
Status launched(cudaError_t err, const char* stage) {
    if (err != cudaSuccess) {
        return failStatus(ErrorCode::Gpu, cudaMessage((std::string("cuda dis: ") + stage).c_str(), err));
    }
    return okStatus();
}

/// Where the two input images come from.
struct DisInputs {
    const GrayImage* hostA = nullptr;  ///< Host path: images to upload.
    const GrayImage* hostB = nullptr;
    const float* devA = nullptr;       ///< Device path: caller's planes.
    const float* devB = nullptr;
    std::size_t devPitchFloats = 0;    ///< Device path: row pitch in floats.
};

/// Validation shared by both entry points - DisFlow.cpp disFlow()'s checks
/// first (same codes, so a caller cannot tell the backends apart by what they
/// refuse), then the GPU's own limits as Unsupported, which computeFlow()
/// answers by falling back to the CPU solver.
Status validate(std::uint32_t w, std::uint32_t h, const DisFlowParams& params) {
    if (w > kMaxEdge || h > kMaxEdge) {
        return failStatus(ErrorCode::InvalidArgument, "cuda dis: image edge beyond the supported maximum");
    }
    if (params.patchSize < 2 || params.iterations < 1) {
        return failStatus(ErrorCode::InvalidArgument, "cuda dis: patchSize must be >= 2 and iterations >= 1");
    }
    if (params.patchSize > gpu::kDisMaxPatchSize) {
        return failStatus(ErrorCode::Unsupported, "cuda dis: patch side " + std::to_string(params.patchSize) +
                                                      " exceeds the GPU limit of " +
                                                      std::to_string(gpu::kDisMaxPatchSize));
    }
    return okStatus();
}

/// The whole bidirectional solve on one leased workspace.
Result<BidirFlow> runDis(const WorkspaceLease& lease, const DisInputs& in, std::uint32_t w, std::uint32_t h,
                         const DisFlowParams& params) {
    Workspace& ws = lease.ws();
    const cudaStream_t s = lease.stream();
    const std::size_t n0 = static_cast<std::size_t>(w) * h;

    // ---- plan everything on the host before touching the device ----------
    const std::vector<Level> levels = planPyramid(w, h, params.levels);
    const std::size_t pyramidFloats = levels.back().offset + levels.back().pixels();

    // The flow smoothing taps for every level that will produce a field; a
    // sigma needing more taps than a launch carries is refused up front, so
    // no work is queued for a solve that cannot finish.
    std::vector<DisBlurTaps> smoothTaps(levels.size());
    std::vector<TapsOutcome> smoothOutcome(levels.size(), TapsOutcome::Skip);
    for (std::size_t l = 0; l < levels.size(); ++l) {
        smoothOutcome[l] = buildTaps(params.smoothSigmaPx, levels[l].w, levels[l].h, smoothTaps[l]);
        if (smoothOutcome[l] == TapsOutcome::TooWide && planGrid(levels[l].w, levels[l].h, params).cols > 0) {
            return Error{ErrorCode::Unsupported, "cuda dis: the flow smoothing sigma needs more than " +
                                                     std::to_string(gpu::kDisMaxBlurRadius) + " taps"};
        }
    }
    const DisGrid finest = planGrid(w, h, params);
    const std::size_t maxPatches =
        std::max<std::size_t>(1, static_cast<std::size_t>(finest.cols) * static_cast<std::size_t>(finest.rows));

    // ---- buffers (grown once, then reused call after call) -----------------
    for (int i = 0; i < 2; ++i) {
        OSV_TRY(ws.pyramid[i].ensure(pyramidFloats * sizeof(float), "pyramid"));
        OSV_TRY(ws.blurTmp[i].ensure(n0 * sizeof(float), "blur scratch"));
        OSV_TRY(ws.blurred[i].ensure(n0 * sizeof(float), "blurred level"));
        OSV_TRY(ws.gradX[i].ensure(n0 * sizeof(float), "x gradient"));
        OSV_TRY(ws.gradY[i].ensure(n0 * sizeof(float), "y gradient"));
        OSV_TRY(ws.patches[i].ensure(maxPatches * sizeof(DisPatch), "patch grid"));
        OSV_TRY(ws.flowU[i].ensure(n0 * sizeof(float), "flow u"));
        OSV_TRY(ws.flowV[i].ensure(n0 * sizeof(float), "flow v"));
    }
    for (int i = 0; i < 4; ++i) {
        OSV_TRY(ws.flowTmp[i].ensure(n0 * sizeof(float), "flow smoothing scratch"));
    }
    OSV_TRY(ws.okMask.ensure(n0, "consistency mask"));
    OSV_TRY(ws.okCount.ensure(sizeof(unsigned long long), "consistency count"));
    // Download layout: four float planes, the mask, then the 8-byte count on
    // an 8-byte boundary.
    const std::size_t maskOffset = 4u * n0 * sizeof(float);
    const std::size_t countOffset = (maskOffset + n0 + 7u) & ~static_cast<std::size_t>(7u);
    OSV_TRY(ws.down.ensure(countOffset + sizeof(unsigned long long), "download staging"));

    float* pyr[2] = {ws.pyramid[0].as<float>(), ws.pyramid[1].as<float>()};

    // ---- 1. inputs onto the pyramid base, scaled to DJI's 8-bit range ------
    // Done once at the base so every level, gradient, tensor and residual is
    // on the scale DJI's absolute thresholds assume (DisFlowParams).
    const bool applyScale = params.intensityScale != 1.0 && std::isfinite(params.intensityScale) &&
                            params.intensityScale > 0.0;
    const float scale = static_cast<float>(params.intensityScale);
    if (in.hostA != nullptr) {
        // Host images: stage through pinned memory so the upload is DMA on
        // this stream, then scale in place.
        OSV_TRY(ws.up.ensure(2u * n0 * sizeof(float), "upload staging"));
        float* up = ws.up.as<float>();
        std::memcpy(up, in.hostA->data.data(), n0 * sizeof(float));
        std::memcpy(up + n0, in.hostB->data.data(), n0 * sizeof(float));
        for (int i = 0; i < 2; ++i) {
            const cudaError_t err = cudaMemcpyAsync(pyr[i], up + static_cast<std::size_t>(i) * n0, n0 * sizeof(float),
                                                    cudaMemcpyHostToDevice, s);
            if (err != cudaSuccess) {
                return Error{ErrorCode::Gpu, cudaMessage("cuda dis: upload", err)};
            }
        }
        OSV_TRY(launched(gpu::disLaunchScale(pyr[0], w, pyr[1], w, pyr[0], pyr[1], static_cast<int>(w),
                                             static_cast<int>(h), scale, applyScale ? 1 : 0, s),
                         "intensity scale"));
    } else {
        // Device images: read the caller's planes, write the pyramid base.
        OSV_TRY(launched(gpu::disLaunchScale(in.devA, in.devPitchFloats, in.devB, in.devPitchFloats, pyr[0], pyr[1],
                                             static_cast<int>(w), static_cast<int>(h), scale, applyScale ? 1 : 0, s),
                         "intensity scale"));
    }

    // ---- 2. the pyramids of both images ------------------------------------
    for (std::size_t l = 1; l < levels.size(); ++l) {
        const Level& src = levels[l - 1];
        const Level& dst = levels[l];
        DisBlurTaps taps;
        const TapsOutcome outcome = buildTaps(kPyramidSigma, src.w, src.h, taps);
        DisPlaneBatch decimate;
        decimate.count = 2;
        if (outcome == TapsOutcome::Ok) {
            // Pre-blur: rows into the scratch plane, columns into `blurred`.
            DisPlaneBatch rows;
            rows.count = 2;
            DisPlaneBatch cols;
            cols.count = 2;
            for (int i = 0; i < 2; ++i) {
                rows.src[i] = pyr[i] + src.offset;
                rows.dst[i] = ws.blurTmp[i].as<float>();
                cols.src[i] = ws.blurTmp[i].as<float>();
                cols.dst[i] = ws.blurred[i].as<float>();
                decimate.src[i] = ws.blurred[i].as<float>();
                decimate.dst[i] = pyr[i] + dst.offset;
            }
            OSV_TRY(launched(gpu::disLaunchBlurRows(rows, static_cast<int>(src.w), static_cast<int>(src.h), taps, s),
                             "pyramid blur (rows)"));
            OSV_TRY(launched(gpu::disLaunchBlurCols(cols, static_cast<int>(src.w), static_cast<int>(src.h), taps, s),
                             "pyramid blur (columns)"));
        } else {
            // blurPlane would have been a no-op: decimate the level as is.
            for (int i = 0; i < 2; ++i) {
                decimate.src[i] = pyr[i] + src.offset;
                decimate.dst[i] = pyr[i] + dst.offset;
            }
        }
        OSV_TRY(launched(gpu::disLaunchDecimate(decimate, static_cast<int>(src.w), static_cast<int>(src.h),
                                                static_cast<int>(dst.w), static_cast<int>(dst.h), s),
                         "pyramid decimation"));
    }

    // ---- 3. coarse to fine -------------------------------------------------
    const DisSolveConsts consts{std::max(1, params.iterations), static_cast<float>(params.stepScale), params.minStepPx,
                                params.maxDisplacementPx, params.maxPatchSsd};
    DisPairW gx{{ws.gradX[0].as<float>(), ws.gradX[1].as<float>()}};
    DisPairW gy{{ws.gradY[0].as<float>(), ws.gradY[1].as<float>()}};
    DisPatchPair patches{{ws.patches[0].as<DisPatch>(), ws.patches[1].as<DisPatch>()}};
    DisPairW flowU{{ws.flowU[0].as<float>(), ws.flowU[1].as<float>()}};
    DisPairW flowV{{ws.flowV[0].as<float>(), ws.flowV[1].as<float>()}};
    bool haveField = false;  // DisFlow.cpp's flow.valid(): a coarser level produced a field
    int fieldW = 0;
    int fieldH = 0;

    for (std::size_t level = levels.size(); level-- > 0;) {
        const Level& lv = levels[level];
        const int lw = static_cast<int>(lv.w);
        const int lh = static_cast<int>(lv.h);
        const DisPair img{{pyr[0] + lv.offset, pyr[1] + lv.offset}};

        // Gradients of both images.  (The CPU computes them before it knows
        // whether the level has a grid; so does this, for symmetry - it is
        // the cheapest pass of the level.)
        OSV_TRY(launched(gpu::disLaunchGradients(img, gx, gy, lw, lh, s), "gradients"));

        const DisGrid grid = planGrid(lv.w, lv.h, params);
        if (grid.cols <= 0 || grid.rows <= 0) {
            continue;  // level too small for a grid; the finer ones still run
        }
        const DisPair gxr{{gx.p[0], gx.p[1]}};
        const DisPair gyr{{gy.p[0], gy.p[1]}};
        OSV_TRY(launched(gpu::disLaunchTensors(gxr, gyr, lw, lh, grid, params.minTensorDet, patches, s),
                         "structure tensors"));

        // Direction 0 solves A -> B from A's tensors, direction 1 B -> A
        // from B's.  The seed is the previous level's field, read before
        // densify overwrites the same planes (stream order guarantees it).
        const DisPair from{{img.p[0], img.p[1]}};
        const DisPair to{{img.p[1], img.p[0]}};
        const DisPair coarseU{{flowU.p[0], flowU.p[1]}};
        const DisPair coarseV{{flowV.p[0], flowV.p[1]}};
        OSV_TRY(launched(gpu::disLaunchSolve(from, to, gxr, gyr, lw, lh, grid, patches, coarseU, coarseV, fieldW,
                                             fieldH, haveField ? 1 : 0, consts, ws.multiprocessors, s),
                         "patch solve"));
        OSV_TRY(launched(gpu::disLaunchDensify(patches, grid, lw, lh, flowU, flowV, s), "densify"));

        // Smooth at every level, not only the last: the field seeds the next.
        if (smoothOutcome[level] == TapsOutcome::Ok) {
            DisPlaneBatch rows;
            DisPlaneBatch cols;
            rows.count = 4;
            cols.count = 4;
            float* planes[4] = {flowU.p[0], flowV.p[0], flowU.p[1], flowV.p[1]};
            for (int i = 0; i < 4; ++i) {
                rows.src[i] = planes[i];
                rows.dst[i] = ws.flowTmp[i].as<float>();
                cols.src[i] = ws.flowTmp[i].as<float>();
                cols.dst[i] = planes[i];
            }
            OSV_TRY(launched(gpu::disLaunchBlurRows(rows, lw, lh, smoothTaps[level], s), "flow smoothing (rows)"));
            OSV_TRY(launched(gpu::disLaunchBlurCols(cols, lw, lh, smoothTaps[level], s), "flow smoothing (columns)"));
        }
        haveField = true;
        fieldW = lw;
        fieldH = lh;
    }
    if (!haveField) {
        return Error{ErrorCode::Internal, "cuda dis: no pyramid level produced a field"};
    }

    // ---- 4. forward-backward consistency -----------------------------------
    const double tol = std::max(0.0, params.consistencyTolPx);
    const double tol2 = tol * tol;
    cudaError_t err = cudaMemsetAsync(ws.okCount.get(), 0, sizeof(unsigned long long), s);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("cuda dis: clear count", err)};
    }
    OSV_TRY(launched(gpu::disLaunchConsistency(flowU.p[0], flowV.p[0], flowU.p[1], flowV.p[1], static_cast<int>(w),
                                               static_cast<int>(h), tol2, ws.okMask.as<std::uint8_t>(),
                                               ws.okCount.as<unsigned long long>(), s),
                     "consistency"));

    // ---- 5. download, then wait for exactly this stream --------------------
    auto* down = ws.down.as<unsigned char>();
    const float* sources[4] = {flowU.p[0], flowV.p[0], flowU.p[1], flowV.p[1]};
    for (int i = 0; i < 4; ++i) {
        err = cudaMemcpyAsync(down + static_cast<std::size_t>(i) * n0 * sizeof(float), sources[i], n0 * sizeof(float),
                              cudaMemcpyDeviceToHost, s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda dis: download field", err)};
        }
    }
    err = cudaMemcpyAsync(down + maskOffset, ws.okMask.get(), n0, cudaMemcpyDeviceToHost, s);
    if (err == cudaSuccess) {
        err = cudaMemcpyAsync(down + countOffset, ws.okCount.get(), sizeof(unsigned long long), cudaMemcpyDeviceToHost,
                              s);
    }
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("cuda dis: download mask", err)};
    }
    err = cudaStreamSynchronize(s);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("cuda dis: execution", err)};
    }

    // Only now, with every stage known to have succeeded, is the result
    // built - the caller never sees a partially written field.
    BidirFlow out;
    const auto* planesOut = reinterpret_cast<const float*>(down);
    out.forward.w = w;
    out.forward.h = h;
    out.forward.u.assign(planesOut, planesOut + n0);
    out.forward.v.assign(planesOut + n0, planesOut + 2u * n0);
    out.backward.w = w;
    out.backward.h = h;
    out.backward.u.assign(planesOut + 2u * n0, planesOut + 3u * n0);
    out.backward.v.assign(planesOut + 3u * n0, planesOut + 4u * n0);
    out.ok.assign(down + maskOffset, down + maskOffset + n0);
    unsigned long long consistent = 0;
    std::memcpy(&consistent, down + countOffset, sizeof(consistent));
    out.consistent = consistent;
    return out;
}

/// Name of the device behind the current context, cached per ordinal.
std::string deviceLabel() {
    const Result<int> dev = gpu::currentDevice();
    if (!dev.ok()) {
        return "unknown device";
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, dev.value()) != cudaSuccess) {
        return "device " + std::to_string(dev.value());
    }
    return std::string(prop.name) + " (sm_" + std::to_string(prop.major) + std::to_string(prop.minor) + ")";
}

/// Dense Inverse Search on the GPU, behind the common FlowBackend interface.
///
/// Holds no state of its own: computeFlow() builds a backend per call, so
/// everything expensive (streams, buffers) lives in the per-context
/// workspace pool and this object is free to create.
class CudaDisFlowBackend final : public FlowBackend {
public:
    CudaDisFlowBackend() = default;

    [[nodiscard]] FlowBackendInfo info() const override {
        FlowBackendInfo out;
        out.kind = FlowBackendKind::ClassicalCuda;
        std::string reason;
        out.available = cudaAnalysesAvailable(&reason);
        out.detail = out.available ? "Dense Inverse Search (CUDA, " + deviceLabel() + ")" : reason;
        // Every patch is solved sequentially by one thread, every pixel sums
        // its terms in a fixed order, and the only atomic is an integer
        // count: two runs agree bit for bit.
        out.deterministic = true;
        return out;
    }

    [[nodiscard]] bool isAvailable() const override { return cudaAnalysesAvailable(nullptr); }

    [[nodiscard]] Result<BidirFlow> compute(const GrayImage& a, const GrayImage& b, const FlowBackendParams& params,
                                            ThreadPool* pool) override {
        (void)pool;  // the GPU needs no CPU workers
        // The consistency tolerance lives on FlowBackendParams so it applies
        // uniformly to every backend, exactly as the CPU backend copies it.
        DisFlowParams dis = params.dis;
        dis.consistencyTolPx = params.consistencyTolPx;
        return cudaDisFlowBidirectional(a, b, dis, nullptr);
    }
};

}  // namespace

namespace gpu {

std::unique_ptr<FlowBackend> makeCudaDisFlowBackend(const FlowBackendParams& params) {
    (void)params;  // nothing to resolve up front: availability is checked per use
    return std::make_unique<CudaDisFlowBackend>();
}

void clearStaleCudaError() noexcept { (void)cudaGetLastError(); }

}  // namespace gpu

// ---------------------------------------------------------------------------
//  Public entry points
// ---------------------------------------------------------------------------
Result<BidirFlow> cudaDisFlowBidirectional(const GrayImage& a, const GrayImage& b, const DisFlowParams& params,
                                           void* stream) {
    // Nothing may escape into a host: allocation failure is the one realistic
    // throw left once the inputs are validated.
    try {
        if (!a.valid() || !b.valid()) {
            return Error{ErrorCode::InvalidArgument, "cuda dis: an input image is empty or malformed"};
        }
        if (a.w != b.w || a.h != b.h) {
            return Error{ErrorCode::InvalidArgument,
                         "cuda dis: size mismatch, " + std::to_string(a.w) + "x" + std::to_string(a.h) + " vs " +
                             std::to_string(b.w) + "x" + std::to_string(b.h)};
        }
        OSV_TRY(validate(a.w, a.h, params));
        gpu::clearStaleCudaError();
        OSV_TRY_ASSIGN(WorkspaceLease lease, gpu::acquireWorkspace(stream));
        DisInputs in;
        in.hostA = &a;
        in.hostB = &b;
        return runDis(lease, in, a.w, a.h, params);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "cuda dis: out of host memory"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("cuda dis: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "cuda dis: unknown exception"};
    }
}

Result<BidirFlow> cudaDisFlowBidirectionalDevice(const float* a, const float* b, std::uint32_t w, std::uint32_t h,
                                                 std::size_t pitchBytes, const DisFlowParams& params, void* stream) {
    try {
        if (a == nullptr || b == nullptr) {
            return Error{ErrorCode::InvalidArgument, "cuda dis: null device image"};
        }
        if (w == 0 || h == 0) {
            return Error{ErrorCode::InvalidArgument, "cuda dis: an input image is empty"};
        }
        if (pitchBytes % sizeof(float) != 0 || pitchBytes < static_cast<std::size_t>(w) * sizeof(float)) {
            return Error{ErrorCode::InvalidArgument, "cuda dis: row pitch is not a whole row of floats"};
        }
        OSV_TRY(validate(w, h, params));
        gpu::clearStaleCudaError();
        OSV_TRY_ASSIGN(WorkspaceLease lease, gpu::acquireWorkspace(stream));
        DisInputs in;
        in.devA = a;
        in.devB = b;
        in.devPitchFloats = pitchBytes / sizeof(float);
        return runDis(lease, in, w, h, params);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "cuda dis: out of host memory"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("cuda dis: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "cuda dis: unknown exception"};
    }
}

}  // namespace osv::render
