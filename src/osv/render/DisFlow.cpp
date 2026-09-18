// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DisFlow.cpp - Dense Inverse Search, the solver.
//
// The structure mirrors the paper (Kroeger et al., ECCV 2016), split into
// the same stages DJI's stitcher uses, so the constants it shares with DJI
// map onto named parameters here rather than onto magic numbers:
//
//   buildPyramid            Gaussian half-scale pyramid of both images
//   precomputeTensors       per-patch inverse structure tensor
//   solvePatches            mean-normalised inverse search per patch
//   densify                 quality-weighted scatter to pixels
//   smoothFlow              separable Gaussian on the field
//   repairFlow              hole filling from neighbours
//
// Numerical conventions, fixed once so every stage agrees:
//   * a flow vector (u, v) at pixel p means the content at p in `from` is at
//     p + (u, v) in `to`;
//   * pixel centres are at integer coordinates, so a bilinear sample at an
//     integer lands exactly on one pixel with no interpolation;
//   * everything is float, and every divide is guarded - a structure tensor
//     on flat sky is singular by nature, not by accident.

#include "osv/render/DisFlow.h"

#include "osv/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::render {

namespace {

/// Largest image edge the solver will accept.  A band is a few thousand
/// pixels wide at most; anything past this is a corrupt size that would
/// overflow the patch-grid arithmetic below.
constexpr std::uint32_t kMaxEdge = 1u << 16;

/// Hard ceiling on pyramid levels, independent of the requested count, so a
/// nonsense parameter cannot allocate an unbounded number of images.
constexpr int kMaxLevels = 8;

/// One patch's solved state.
struct Patch {
    float x = 0.0f;       ///< Patch centre in level pixels.
    float y = 0.0f;
    float u = 0.0f;       ///< Solved displacement.
    float v = 0.0f;
    float quality = 0.0f; ///< 1 / (1 + mean abs residual); 0 = rejected.
    // The inverse structure tensor, symmetric, stored as its three distinct
    // entries already inverted so the solve loop does no division.
    float iHxx = 0.0f;
    float iHxy = 0.0f;
    float iHyy = 0.0f;
    bool usable = false;  ///< False when the tensor was singular.
};

/// Separable Gaussian blur of a single plane, clamp-to-edge.
void blurPlane(std::vector<float>& plane, std::uint32_t w, std::uint32_t h, double sigma) {
    if (sigma <= 0.0 || w == 0 || h == 0) {
        return;
    }
    // Three sigma each side captures >99.7% of the kernel; a radius of zero
    // would make the loop a no-op copy, so it is floored at one.
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    if (static_cast<std::uint32_t>(radius) >= w && static_cast<std::uint32_t>(radius) >= h) {
        return;  // kernel wider than the image: blurring it says nothing
    }
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double weight = std::exp(-static_cast<double>(i) * static_cast<double>(i) * inv2s2);
        kernel[static_cast<std::size_t>(i + radius)] = static_cast<float>(weight);
        sum += weight;
    }
    if (!(sum > 0.0)) {
        return;  // cannot normalise; leave the plane untouched
    }
    const float invSum = static_cast<float>(1.0 / sum);
    for (float& k : kernel) {
        k *= invSum;
    }

    std::vector<float> tmp(plane.size(), 0.0f);

    // Horizontal pass.
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w;
        for (std::uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                const int sx = std::clamp(static_cast<int>(x) + i, 0, static_cast<int>(w) - 1);
                acc += plane[row + static_cast<std::size_t>(sx)] * kernel[static_cast<std::size_t>(i + radius)];
            }
            tmp[row + x] = acc;
        }
    }
    // Vertical pass.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                const int sy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(h) - 1);
                acc += tmp[static_cast<std::size_t>(sy) * w + x] * kernel[static_cast<std::size_t>(i + radius)];
            }
            plane[static_cast<std::size_t>(y) * w + x] = acc;
        }
    }
}

/// Half-scale an image with a 2x2 box filter after a light Gaussian.
///
/// The pre-blur matters: decimating without it aliases high-frequency detail
/// into the coarse level, and the coarse level is what seeds every finer one,
/// so the alias would propagate all the way up as a confident wrong answer.
GrayImage halfScale(const GrayImage& src) {
    GrayImage dst;
    if (!src.valid() || src.w < 2 || src.h < 2) {
        return dst;
    }
    GrayImage blurred = src;
    blurPlane(blurred.data, blurred.w, blurred.h, 0.8);  // sigma for a 2x decimation

    dst.w = src.w / 2;
    dst.h = src.h / 2;
    dst.data.assign(static_cast<std::size_t>(dst.w) * dst.h, 0.0f);
    for (std::uint32_t y = 0; y < dst.h; ++y) {
        for (std::uint32_t x = 0; x < dst.w; ++x) {
            const std::uint32_t sx = x * 2;
            const std::uint32_t sy = y * 2;
            const float a = blurred.at(static_cast<int>(sx), static_cast<int>(sy));
            const float b = blurred.at(static_cast<int>(sx + 1), static_cast<int>(sy));
            const float c = blurred.at(static_cast<int>(sx), static_cast<int>(sy + 1));
            const float d = blurred.at(static_cast<int>(sx + 1), static_cast<int>(sy + 1));
            dst.data[static_cast<std::size_t>(y) * dst.w + x] = 0.25f * (a + b + c + d);
        }
    }
    return dst;
}

/// Build the coarse-to-fine pyramid.  Index 0 is the FINEST (the input).
std::vector<GrayImage> buildPyramid(const GrayImage& base, int levels) {
    std::vector<GrayImage> pyramid;
    pyramid.reserve(static_cast<std::size_t>(std::clamp(levels, 1, kMaxLevels)));
    pyramid.push_back(base);
    for (int level = 1; level < std::min(levels, kMaxLevels); ++level) {
        const GrayImage& prev = pyramid.back();
        // Stop before a level too small to hold a patch grid: below this the
        // solve has fewer than two patches per axis and says nothing.
        if (prev.w / 2 < kMinPyramidEdge || prev.h / 2 < kMinPyramidEdge) {
            break;
        }
        GrayImage next = halfScale(prev);
        if (!next.valid()) {
            break;
        }
        pyramid.push_back(std::move(next));
    }
    return pyramid;
}

/// Central-difference gradients of one level, same size as the image.
void gradients(const GrayImage& img, std::vector<float>& gx, std::vector<float>& gy) {
    gx.assign(img.data.size(), 0.0f);
    gy.assign(img.data.size(), 0.0f);
    for (std::uint32_t y = 0; y < img.h; ++y) {
        for (std::uint32_t x = 0; x < img.w; ++x) {
            const int xi = static_cast<int>(x);
            const int yi = static_cast<int>(y);
            // 0.5 * (I(x+1) - I(x-1)), clamped at the edges by at().
            gx[static_cast<std::size_t>(y) * img.w + x] = 0.5f * (img.at(xi + 1, yi) - img.at(xi - 1, yi));
            gy[static_cast<std::size_t>(y) * img.w + x] = 0.5f * (img.at(xi, yi + 1) - img.at(xi, yi - 1));
        }
    }
}

/// Lay out the patch grid for a level and precompute each patch's inverse
/// structure tensor.
///
/// The structure tensor of a patch is the 2x2 sum over its pixels of the
/// outer product of the gradient with itself:
///
///     H = sum [ gx*gx  gx*gy ]
///             [ gx*gy  gy*gy ]
///
/// Inverse search needs H^-1 once per patch, which is the whole trick: the
/// per-iteration cost drops to one image sample plus a 2x2 multiply.  A patch
/// whose H is near-singular is marked unusable here rather than producing a
/// huge displacement later - that is the flat-sky case, and it is the common
/// case in this application, where most of the overlap band is empty blue.
std::vector<Patch> precomputeTensors(const GrayImage& img, const std::vector<float>& gx, const std::vector<float>& gy,
                                     const DisFlowParams& params) {
    std::vector<Patch> patches;
    const int ps = std::max(2, params.patchSize);
    const int half = ps / 2;
    // Stride from the overlap fraction; at least one pixel so the loop ends.
    const int stride = std::max(1, static_cast<int>(std::lround(static_cast<double>(ps) *
                                                               std::clamp(params.patchStrideFraction, 0.1, 1.0))));
    if (img.w < static_cast<std::uint32_t>(ps) || img.h < static_cast<std::uint32_t>(ps)) {
        return patches;
    }

    const int lastX = static_cast<int>(img.w) - half - 1;
    const int lastY = static_cast<int>(img.h) - half - 1;
    for (int cy = half; cy <= lastY; cy += stride) {
        for (int cx = half; cx <= lastX; cx += stride) {
            Patch p;
            p.x = static_cast<float>(cx);
            p.y = static_cast<float>(cy);

            double hxx = 0.0;
            double hxy = 0.0;
            double hyy = 0.0;
            for (int dy = -half; dy < ps - half; ++dy) {
                for (int dx = -half; dx < ps - half; ++dx) {
                    const int sx = std::clamp(cx + dx, 0, static_cast<int>(img.w) - 1);
                    const int sy = std::clamp(cy + dy, 0, static_cast<int>(img.h) - 1);
                    const std::size_t idx = static_cast<std::size_t>(sy) * img.w + static_cast<std::size_t>(sx);
                    const double a = gx[idx];
                    const double b = gy[idx];
                    hxx += a * a;
                    hxy += a * b;
                    hyy += b * b;
                }
            }

            // Invertibility test on a SCALE-FREE quantity: det / trace^2.  A
            // raw determinant threshold would reject a low-contrast patch
            // that is nonetheless well conditioned, and accept a
            // high-contrast edge patch that is singular along the edge.
            const double det = hxx * hyy - hxy * hxy;
            const double trace = hxx + hyy;
            if (trace > 0.0 && det / (trace * trace) > params.minTensorDet) {
                const double invDet = 1.0 / det;
                p.iHxx = static_cast<float>(hyy * invDet);
                p.iHxy = static_cast<float>(-hxy * invDet);
                p.iHyy = static_cast<float>(hxx * invDet);
                p.usable = true;
            }
            patches.push_back(p);
        }
    }
    return patches;
}

/// Solve one patch by inverse search, starting from (u, v).
///
/// Each iteration measures the residual between the template (the patch in
/// `from`) and the target sampled at the current displacement, projects it
/// onto the gradients, and applies the precomputed inverse tensor.  The
/// mean-normalisation - subtracting each window's own mean before comparing -
/// is what makes this robust to the exposure difference between the two
/// lenses, and it is why DJI's stitcher also mean-normalises rather than
/// using a plain SSD.
void solvePatch(Patch& patch, const GrayImage& from, const GrayImage& to, const std::vector<float>& gx,
                const std::vector<float>& gy, const DisFlowParams& params) {
    if (!patch.usable) {
        patch.quality = 0.0f;
        return;
    }
    const int ps = std::max(2, params.patchSize);
    const int half = ps / 2;
    const int cx = static_cast<int>(patch.x);
    const int cy = static_cast<int>(patch.y);

    // The template and its mean, computed once - the template never moves.
    std::vector<float> tmpl(static_cast<std::size_t>(ps) * static_cast<std::size_t>(ps), 0.0f);
    double tmplSum = 0.0;
    for (int dy = -half; dy < ps - half; ++dy) {
        for (int dx = -half; dx < ps - half; ++dx) {
            const float value = from.at(cx + dx, cy + dy);
            tmpl[static_cast<std::size_t>(dy + half) * ps + static_cast<std::size_t>(dx + half)] = value;
            tmplSum += value;
        }
    }
    const float tmplMean = static_cast<float>(tmplSum / (static_cast<double>(ps) * ps));

    float u = patch.u;
    float v = patch.v;
    double lastResidual = 0.0;

    for (int iter = 0; iter < std::max(1, params.iterations); ++iter) {
        // Target window at the current displacement, and its own mean.
        double targetSum = 0.0;
        for (int dy = -half; dy < ps - half; ++dy) {
            for (int dx = -half; dx < ps - half; ++dx) {
                targetSum += to.sample(static_cast<float>(cx + dx) + u, static_cast<float>(cy + dy) + v);
            }
        }
        const float targetMean = static_cast<float>(targetSum / (static_cast<double>(ps) * ps));

        // Project the mean-normalised residual onto the template gradients.
        double bx = 0.0;
        double by = 0.0;
        double absResidual = 0.0;
        for (int dy = -half; dy < ps - half; ++dy) {
            for (int dx = -half; dx < ps - half; ++dx) {
                const int sx = std::clamp(cx + dx, 0, static_cast<int>(from.w) - 1);
                const int sy = std::clamp(cy + dy, 0, static_cast<int>(from.h) - 1);
                const std::size_t gidx = static_cast<std::size_t>(sy) * from.w + static_cast<std::size_t>(sx);
                const float t = tmpl[static_cast<std::size_t>(dy + half) * ps + static_cast<std::size_t>(dx + half)] -
                                tmplMean;
                const float s = to.sample(static_cast<float>(cx + dx) + u, static_cast<float>(cy + dy) + v) -
                                targetMean;
                const float residual = s - t;
                absResidual += std::fabs(static_cast<double>(residual));
                bx += static_cast<double>(residual) * gx[gidx];
                by += static_cast<double>(residual) * gy[gidx];
            }
        }
        lastResidual = absResidual / (static_cast<double>(ps) * ps);

        // The inverse-tensor step.  Negated because b was accumulated as
        // (target - template): we move the displacement to REDUCE it.
        const float du = -(patch.iHxx * static_cast<float>(bx) + patch.iHxy * static_cast<float>(by));
        const float dv = -(patch.iHxy * static_cast<float>(bx) + patch.iHyy * static_cast<float>(by));
        if (!std::isfinite(du) || !std::isfinite(dv)) {
            patch.usable = false;
            patch.quality = 0.0f;
            return;
        }
        u += du;
        v += dv;

        // A patch that has wandered further than any real disparity has
        // locked onto the wrong feature; stop and disown it rather than
        // letting it drag the densified field.
        if (std::fabs(static_cast<double>(u)) > params.maxDisplacementPx ||
            std::fabs(static_cast<double>(v)) > params.maxDisplacementPx) {
            patch.usable = false;
            patch.quality = 0.0f;
            return;
        }
        if (std::fabs(static_cast<double>(du)) < params.minStepPx &&
            std::fabs(static_cast<double>(dv)) < params.minStepPx) {
            break;  // converged
        }
    }

    patch.u = u;
    patch.v = v;
    // Quality falls off with the residual, so densify() prefers patches that
    // actually matched.  The +1 keeps it bounded in (0, 1].
    patch.quality = static_cast<float>(1.0 / (1.0 + lastResidual));
}

/// Scatter the solved patches into a per-pixel field, weighted by quality.
///
/// Every pixel is covered by several patches (they overlap by construction),
/// and this is the paper's densification: a weighted mean, with the weight
/// being match quality times a spatial falloff from the patch centre.  The
/// spatial term stops a patch from imposing its displacement on pixels near
/// its edge that a better-centred neighbour describes.
FlowField densify(const std::vector<Patch>& patches, std::uint32_t w, std::uint32_t h, const DisFlowParams& params) {
    FlowField flow;
    flow.resize(w, h);
    if (w == 0 || h == 0) {
        return flow;
    }

    std::vector<float> weight(static_cast<std::size_t>(w) * h, 0.0f);
    const int ps = std::max(2, params.patchSize);
    const int half = ps / 2;
    // Spatial falloff scaled to the patch: sigma of half the patch means the
    // weight is ~0.6 at the patch edge, so overlapping patches blend rather
    // than one winning outright.
    const double sigma = std::max(1.0, static_cast<double>(half));
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);

    for (const Patch& p : patches) {
        if (!p.usable || !(p.quality > 0.0f)) {
            continue;
        }
        const int cx = static_cast<int>(p.x);
        const int cy = static_cast<int>(p.y);
        for (int dy = -half; dy < ps - half; ++dy) {
            const int y = cy + dy;
            if (y < 0 || y >= static_cast<int>(h)) {
                continue;
            }
            for (int dx = -half; dx < ps - half; ++dx) {
                const int x = cx + dx;
                if (x < 0 || x >= static_cast<int>(w)) {
                    continue;
                }
                const double r2 = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
                const float wgt = p.quality * static_cast<float>(std::exp(-r2 * inv2s2));
                const std::size_t idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);
                flow.u[idx] += p.u * wgt;
                flow.v[idx] += p.v * wgt;
                weight[idx] += wgt;
            }
        }
    }

    // Normalise.  A pixel no usable patch reached keeps zero flow, which is
    // the honest answer: nothing measured it.  repairFlow() can fill those
    // from neighbours when the caller wants a dense field.
    for (std::size_t i = 0; i < flow.u.size(); ++i) {
        if (weight[i] > 0.0f) {
            const float inv = 1.0f / weight[i];
            flow.u[i] *= inv;
            flow.v[i] *= inv;
        }
    }
    return flow;
}

}  // namespace

// ---------------------------------------------------------------------------
//  GrayImage / FlowField
// ---------------------------------------------------------------------------
float GrayImage::at(int x, int y) const noexcept {
    if (data.empty() || w == 0 || h == 0) {
        return 0.0f;
    }
    const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
    return data[static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)];
}

float GrayImage::sample(float x, float y) const noexcept {
    if (data.empty() || w == 0 || h == 0) {
        return 0.0f;
    }
    // A non-finite coordinate can only come from a diverged solve; returning
    // 0 keeps the caller's arithmetic finite so it can detect and reject it.
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return 0.0f;
    }
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const float tx = x - fx;
    const float ty = y - fy;
    const float a = at(x0, y0);
    const float b = at(x0 + 1, y0);
    const float c = at(x0, y0 + 1);
    const float d = at(x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

void FlowField::resize(std::uint32_t width, std::uint32_t height) {
    w = width;
    h = height;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    u.assign(n, 0.0f);
    v.assign(n, 0.0f);
}

float FlowField::atU(int x, int y) const noexcept {
    if (u.empty() || w == 0 || h == 0) {
        return 0.0f;
    }
    const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
    return u[static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)];
}

float FlowField::atV(int x, int y) const noexcept {
    if (v.empty() || w == 0 || h == 0) {
        return 0.0f;
    }
    const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
    return v[static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)];
}

// ---------------------------------------------------------------------------
//  Flow post-processing
// ---------------------------------------------------------------------------
void smoothFlow(FlowField& flow, double sigmaPx) {
    if (!flow.valid() || sigmaPx <= 0.0) {
        return;
    }
    blurPlane(flow.u, flow.w, flow.h, sigmaPx);
    blurPlane(flow.v, flow.w, flow.h, sigmaPx);
}

std::uint64_t repairFlow(FlowField& flow, const std::vector<std::uint8_t>& ok) {
    if (!flow.valid() || ok.size() != flow.u.size()) {
        return 0;
    }
    std::vector<std::uint8_t> filled = ok;
    std::uint64_t remaining = 0;
    for (const std::uint8_t value : filled) {
        if (value == 0) {
            ++remaining;
        }
    }
    if (remaining == 0 || remaining == filled.size()) {
        // Nothing to do, or nothing to borrow FROM - an all-invalid field
        // cannot be repaired and saying so is better than inventing zeros.
        return remaining;
    }

    // Iterative dilation.  Each sweep fills only pixels adjacent to
    // already-valid ones, so the values genuinely propagate outward from
    // measured data instead of being smeared across the whole hole at once.
    const int w = static_cast<int>(flow.w);
    const int h = static_cast<int>(flow.h);
    for (int sweep = 0; sweep < w + h && remaining > 0; ++sweep) {
        std::vector<std::uint8_t> next = filled;
        std::uint64_t fixedThisSweep = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * flow.w + static_cast<std::size_t>(x);
                if (filled[idx] != 0) {
                    continue;
                }
                double su = 0.0;
                double sv = 0.0;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                            continue;
                        }
                        const std::size_t nidx =
                            static_cast<std::size_t>(ny) * flow.w + static_cast<std::size_t>(nx);
                        if (filled[nidx] == 0) {
                            continue;
                        }
                        su += flow.u[nidx];
                        sv += flow.v[nidx];
                        ++count;
                    }
                }
                if (count > 0) {
                    flow.u[idx] = static_cast<float>(su / count);
                    flow.v[idx] = static_cast<float>(sv / count);
                    next[idx] = 1;
                    ++fixedThisSweep;
                }
            }
        }
        if (fixedThisSweep == 0) {
            break;  // no progress is possible; stop rather than spin
        }
        filled.swap(next);
        remaining -= fixedThisSweep;
    }
    return remaining;
}

// ---------------------------------------------------------------------------
//  The solver
// ---------------------------------------------------------------------------
Result<FlowField> disFlow(const GrayImage& from, const GrayImage& to, const DisFlowParams& params, ThreadPool* pool) {
    (void)pool;  // The per-level solve is sequential; see the note below.

    if (!from.valid() || !to.valid()) {
        return Error{ErrorCode::InvalidArgument, "disFlow: an input image is empty or malformed"};
    }
    if (from.w != to.w || from.h != to.h) {
        return Error{ErrorCode::InvalidArgument,
                     std::string("disFlow: size mismatch, ") + std::to_string(from.w) + "x" +
                         std::to_string(from.h) + " vs " + std::to_string(to.w) + "x" + std::to_string(to.h)};
    }
    if (from.w > kMaxEdge || from.h > kMaxEdge) {
        return Error{ErrorCode::InvalidArgument, "disFlow: image edge beyond the supported maximum"};
    }
    if (params.patchSize < 2 || params.iterations < 1) {
        return Error{ErrorCode::InvalidArgument, "disFlow: patchSize must be >= 2 and iterations >= 1"};
    }

    const std::vector<GrayImage> pyrFrom = buildPyramid(from, std::max(1, params.levels));
    const std::vector<GrayImage> pyrTo = buildPyramid(to, std::max(1, params.levels));
    if (pyrFrom.empty() || pyrTo.empty() || pyrFrom.size() != pyrTo.size()) {
        return Error{ErrorCode::Internal, "disFlow: pyramid construction failed"};
    }

    // Coarse to fine: start at the smallest level and carry the field up.
    FlowField flow;
    for (std::size_t level = pyrFrom.size(); level-- > 0;) {
        const GrayImage& imgFrom = pyrFrom[level];
        const GrayImage& imgTo = pyrTo[level];

        std::vector<float> gx;
        std::vector<float> gy;
        gradients(imgFrom, gx, gy);

        std::vector<Patch> patches = precomputeTensors(imgFrom, gx, gy, params);
        if (patches.empty()) {
            continue;  // level too small for a grid; the finer ones still run
        }

        // Seed each patch from the field carried up from the coarser level,
        // doubling the displacement because this level is twice the size.
        if (flow.valid()) {
            for (Patch& p : patches) {
                const int sx = static_cast<int>(p.x * 0.5f);
                const int sy = static_cast<int>(p.y * 0.5f);
                p.u = flow.atU(sx, sy) * 2.0f;
                p.v = flow.atV(sx, sy) * 2.0f;
            }
        }

        // The patch solve is embarrassingly parallel - every patch reads the
        // two images and writes only itself - but it is left sequential here
        // deliberately.  The band this runs on is a few hundred rows, the
        // whole solve is milliseconds, and ThreadPool::parallelFor has a
        // per-call synchronisation cost that dominates at that size.  The
        // pool parameter is kept in the signature so the decision can be
        // revisited for full-frame use without an API change.
        for (Patch& p : patches) {
            solvePatch(p, imgFrom, imgTo, gx, gy, params);
        }

        flow = densify(patches, imgFrom.w, imgFrom.h, params);
        // Smooth at every level, not only the last: the field is the seed for
        // the next level, and an unsmoothed seed propagates each patch's
        // blockiness into the finer solve.
        smoothFlow(flow, params.smoothSigmaPx);
    }

    if (!flow.valid()) {
        return Error{ErrorCode::Internal, "disFlow: no pyramid level produced a field"};
    }
    return flow;
}

Result<BidirFlow> disFlowBidirectional(const GrayImage& a, const GrayImage& b, const DisFlowParams& params,
                                       ThreadPool* pool) {
    BidirFlow out;

    OSV_TRY_ASSIGN(out.forward, disFlow(a, b, params, pool));
    OSV_TRY_ASSIGN(out.backward, disFlow(b, a, params, pool));
    if (out.forward.w != out.backward.w || out.forward.h != out.backward.h) {
        return Error{ErrorCode::Internal, "disFlowBidirectional: the two directions disagree on size"};
    }

    // Forward-backward consistency.  Follow the forward flow from p to q,
    // then the backward flow from q, and see whether it lands back at p.  A
    // mismatch means the two solves disagree about what moved where, which
    // happens at occlusions, at the band edges and on textureless regions -
    // exactly the places a warp must not trust the field.
    out.ok.assign(out.forward.u.size(), 0u);
    const int w = static_cast<int>(out.forward.w);
    const int h = static_cast<int>(out.forward.h);
    const double tol = std::max(0.0, params.consistencyTolPx);
    const double tol2 = tol * tol;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * out.forward.w + static_cast<std::size_t>(x);
            const float fu = out.forward.u[idx];
            const float fv = out.forward.v[idx];
            if (!std::isfinite(fu) || !std::isfinite(fv)) {
                continue;
            }
            // Nearest-pixel lookup of the backward field; the tolerance is
            // more than a pixel, so bilinear here would add cost for nothing.
            const int qx = static_cast<int>(std::lround(static_cast<double>(x) + fu));
            const int qy = static_cast<int>(std::lround(static_cast<double>(y) + fv));
            const float bu = out.backward.atU(qx, qy);
            const float bv = out.backward.atV(qx, qy);
            const double ex = static_cast<double>(fu) + bu;
            const double ey = static_cast<double>(fv) + bv;
            if (ex * ex + ey * ey <= tol2) {
                out.ok[idx] = 1u;
                ++out.consistent;
            }
        }
    }

    return out;
}

}  // namespace osv::render
