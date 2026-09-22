// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ParallaxWarp.cpp - measure the 2-D parallax across the overlap band and
// reduce it to the angular grid the kernel samples.
//
// The interesting steps, each explained at its site below:
//
//   1. the flow is HALVED, because both lenses move toward each other;
//   2. band pixels become angles through the polar-axis map's own scale, so
//      no new convention is introduced;
//   3. the cross-meridian component is trusted less than the along-meridian
//      one (anisotropic regularization);
//   4. the field is decayed to exactly zero at the grid's latitude edges, so
//      the correction cannot tear where the overlap ends.

#include "osv/render/ParallaxWarp.h"

#include "osv/core/Log.h"

#include <algorithm>
#include <cmath>

namespace osv::render {

namespace {

/// Smoothstep on [0, 1], clamped outside it.  Used for the decay ring:
/// a linear ramp to zero has a slope discontinuity at both of its ends, and
/// that shows as a faint band edge on smooth content - which is most of what
/// sits at the top and bottom of an overlap band.
[[nodiscard]] double smoothstep01(double t) noexcept {
    const double c = std::clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

/// Separable Gaussian blur of one interleaved component of the grid.
///
/// Longitude wraps and latitude clamps, matching how the kernel samples the
/// same table - if the smoothing used different addressing than the fetch,
/// the two would disagree exactly at the wrap meridian.
void blurComponent(std::vector<float>& uv, std::uint32_t w, std::uint32_t h, int comp, double sigma) {
    if (sigma <= 0.0 || w == 0 || h == 0) {
        return;
    }
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    if (radius < 1) {
        return;
    }
    std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
    double ksum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double k = std::exp(-0.5 * static_cast<double>(i * i) / (sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = k;
        ksum += k;
    }
    if (!(ksum > 0.0)) {
        return;
    }

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> tmp(n, 0.0f);

    // Horizontal pass: longitude wraps.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int xx = ((static_cast<int>(x) + i) % static_cast<int>(w) + static_cast<int>(w)) %
                               static_cast<int>(w);
                acc += static_cast<double>(uv[(static_cast<std::size_t>(y) * w + static_cast<std::uint32_t>(xx)) * 2u +
                                              static_cast<std::size_t>(comp)]) *
                       kernel[static_cast<std::size_t>(i + radius)];
            }
            tmp[static_cast<std::size_t>(y) * w + x] = static_cast<float>(acc / ksum);
        }
    }

    // Vertical pass: latitude clamps, because the band genuinely ends.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(h) - 1);
                acc += static_cast<double>(tmp[static_cast<std::size_t>(yy) * w + x]) *
                       kernel[static_cast<std::size_t>(i + radius)];
            }
            uv[(static_cast<std::size_t>(y) * w + x) * 2u + static_cast<std::size_t>(comp)] =
                static_cast<float>(acc / ksum);
        }
    }
}

/// Validate the tuning block once, so every later step can assume it is sane.
[[nodiscard]] Status checkParams(const ParallaxWarpParams& p) {
    if (p.gridW < 8 || p.gridW > 8192) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: gridW out of range");
    }
    if (p.gridRows < 4 || p.gridRows > 4096) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: gridRows out of range");
    }
    if (p.decayRows > 4096) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: decayRows out of range");
    }
    if (!(p.crossMeridianScale >= 0.0) || p.crossMeridianScale > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: crossMeridianScale must be within 0..1");
    }
    if (!(p.maxCorrectionDeg > 0.0) || p.maxCorrectionDeg > 45.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: maxCorrectionDeg out of range");
    }
    if (!(p.minConsistentFraction >= 0.0) || p.minConsistentFraction > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: minConsistentFraction must be within 0..1");
    }
    if (!std::isfinite(p.crossMeridianSmooth) || p.crossMeridianSmooth < 0.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: crossMeridianSmooth must be >= 0");
    }
    return okStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
//  Flow field -> angular grid
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> gridFromFlow(const LensBands& bands, const BidirFlow& flow,
                                      const ParallaxWarpParams& params) {
    OSV_TRY(checkParams(params));
    if (bands.w == 0 || bands.h == 0 || bands.mapH == 0) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: empty band"};
    }
    if (!flow.valid()) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: invalid flow field"};
    }
    if (flow.forward.w != bands.w || flow.forward.h != bands.h) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: flow size does not match the band"};
    }
    if (bands.luma[0].size() != static_cast<std::size_t>(bands.w) * bands.h ||
        bands.alpha[0].size() != bands.luma[0].size() || bands.alpha[1].size() != bands.luma[0].size()) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: band planes are the wrong size"};
    }

    ParallaxWarpGrid grid;
    grid.w = params.gridW;
    grid.h = params.gridRows + 2u * params.decayRows;
    if (grid.h < 2) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: grid needs at least two rows"};
    }
    grid.uv.assign(static_cast<std::size_t>(grid.w) * grid.h * 2u, 0.0f);

    // ---- band geometry -> angles ------------------------------------------
    // The band came from a polar-axis equirect map: its columns ARE longitude
    // and its rows ARE latitude, linearly.  So one band pixel is a fixed
    // angle in each direction and no new convention enters here.
    const double radPerRow = osv::kPi / static_cast<double>(bands.mapH);
    const double radPerCol = osv::kTwoPi / static_cast<double>(bands.w);

    // Latitude of the measured rows.  Row r of the band is equirect row
    // (rowOffset + r), and the polar-axis map puts latitude +90 at row 0.
    const auto latOfBandRow = [&](double r) {
        const double equirectRow = static_cast<double>(bands.rowOffset) + r;
        return osv::kHalfPi - equirectRow * radPerRow;
    };
    const double latTop = latOfBandRow(0.0);
    const double latBottom = latOfBandRow(static_cast<double>(bands.h) - 1.0);

    // The grid spans the band PLUS the decay ring on each side, so the
    // correction has somewhere to fall to zero that is not inside the
    // measured region.  Rows run top (max latitude) to bottom, matching the
    // band, and the kernel maps latitude onto rows with the same span.
    const double bandLatSpan = latTop - latBottom;  // positive: top is larger
    const double latPerGridRow = params.gridRows > 1 ? bandLatSpan / static_cast<double>(params.gridRows - 1) : 0.0;
    const double gridLatTop = latTop + latPerGridRow * static_cast<double>(params.decayRows);
    const double gridLatBottom = latBottom - latPerGridRow * static_cast<double>(params.decayRows);
    // latMinRad is row 0's latitude and latMaxRad row (h - 1)'s, per the
    // kernel's own documentation; rows descend in latitude, so min > max
    // numerically and the kernel's span subtraction handles the sign.
    grid.latMinRad = static_cast<float>(gridLatTop);
    grid.latMaxRad = static_cast<float>(gridLatBottom);

    const double maxRad = params.maxCorrectionDeg * osv::kPi / 180.0;

    // ---- accumulate the flow into the grid cells ---------------------------
    // Each grid cell averages the flow over the band pixels that fall in it,
    // weighted by nothing more than co-visibility: a pixel only one lens can
    // see carries no disparity information at all, and a pixel whose flow
    // failed the consistency check carries a repaired guess that must not
    // outvote a measured neighbour.
    std::vector<double> accU(static_cast<std::size_t>(grid.w) * params.gridRows, 0.0);
    std::vector<double> accV(accU.size(), 0.0);
    std::vector<double> accW(accU.size(), 0.0);

    std::uint64_t consistent = 0;
    std::uint64_t total = 0;

    for (std::uint32_t r = 0; r < bands.h; ++r) {
        // Band row -> grid row, within the measured (non-decay) rows.
        const double rowFrac = bands.h > 1 ? static_cast<double>(r) / static_cast<double>(bands.h - 1) : 0.0;
        int gr = static_cast<int>(std::lround(rowFrac * static_cast<double>(params.gridRows - 1)));
        gr = std::clamp(gr, 0, static_cast<int>(params.gridRows) - 1);

        for (std::uint32_t c = 0; c < bands.w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
            ++total;
            // Only co-visible pixels say anything about parallax.
            if (!(bands.alpha[0][i] > 0.5f) || !(bands.alpha[1][i] > 0.5f)) {
                continue;
            }
            const std::uint8_t ok = i < flow.ok.size() ? flow.ok[i] : 0u;
            if (ok) {
                ++consistent;
            } else {
                // A repaired vector is plausible but not measured; it is
                // allowed to fill a hole and not to dominate one.
                continue;
            }
            const float fu = flow.forward.u[i];
            const float fv = flow.forward.v[i];
            if (!std::isfinite(fu) || !std::isfinite(fv)) {
                continue;
            }

            int gc = static_cast<int>(std::floor((static_cast<double>(c) / static_cast<double>(bands.w)) *
                                                 static_cast<double>(grid.w)));
            gc = ((gc % static_cast<int>(grid.w)) + static_cast<int>(grid.w)) % static_cast<int>(grid.w);

            const std::size_t gi = static_cast<std::size_t>(gr) * grid.w + static_cast<std::size_t>(gc);
            accU[gi] += static_cast<double>(fu);
            accV[gi] += static_cast<double>(fv);
            accW[gi] += 1.0;
        }
    }

    grid.consistentPixels = consistent;
    grid.totalPixels = total;

    // ---- pixels -> half-corrections in radians -----------------------------
    // HALVED, because the kernel moves BOTH lenses: lens 0 by +delta and
    // lens 1 by -delta, so the two close a gap of 2 * delta.  Storing the
    // half here keeps the kernel's inner loop free of the factor and makes
    // the stored number mean exactly "how far each lens moves".
    //
    // The ANISOTROPY enters here too.  The v (along-meridian) component is
    // the epipolar direction and is taken at face value; the u
    // (cross-meridian) component is scaled down, because on a back-to-back
    // rig a large perpendicular disparity is more often a patch mismatch
    // than real geometry.
    std::vector<float> measured(static_cast<std::size_t>(grid.w) * params.gridRows * 2u, 0.0f);
    for (std::size_t gi = 0; gi < accW.size(); ++gi) {
        if (!(accW[gi] > 0.0)) {
            continue;
        }
        const double inv = 1.0 / accW[gi];
        const double u = (accU[gi] * inv) * radPerCol * 0.5 * params.crossMeridianScale;
        const double v = (accV[gi] * inv) * radPerRow * 0.5;
        measured[gi * 2u + 0u] = static_cast<float>(std::clamp(u, -maxRad, maxRad));
        measured[gi * 2u + 1u] = static_cast<float>(std::clamp(v, -maxRad, maxRad));
    }

    // Fill cells no co-visible pixel reached, from their nearest filled
    // neighbour along longitude (which wraps).  An empty cell left at zero is
    // a claim that there is no parallax there, and a zero island inside a
    // corrected region tears exactly like an unwarped one.
    for (std::uint32_t gr = 0; gr < params.gridRows; ++gr) {
        const std::size_t rowBase = static_cast<std::size_t>(gr) * grid.w;
        // Is anything in this row filled at all?
        bool any = false;
        for (std::uint32_t gc = 0; gc < grid.w && !any; ++gc) {
            any = accW[rowBase + gc] > 0.0;
        }
        if (!any) {
            continue;  // nothing to spread; the row stays at zero
        }
        for (std::uint32_t gc = 0; gc < grid.w; ++gc) {
            if (accW[rowBase + gc] > 0.0) {
                continue;
            }
            for (std::uint32_t d = 1; d < grid.w; ++d) {
                const std::uint32_t left = (gc + grid.w - d) % grid.w;
                const std::uint32_t right = (gc + d) % grid.w;
                if (accW[rowBase + left] > 0.0) {
                    measured[(rowBase + gc) * 2u + 0u] = measured[(rowBase + left) * 2u + 0u];
                    measured[(rowBase + gc) * 2u + 1u] = measured[(rowBase + left) * 2u + 1u];
                    break;
                }
                if (accW[rowBase + right] > 0.0) {
                    measured[(rowBase + gc) * 2u + 0u] = measured[(rowBase + right) * 2u + 0u];
                    measured[(rowBase + gc) * 2u + 1u] = measured[(rowBase + right) * 2u + 1u];
                    break;
                }
            }
        }
    }

    // ---- copy into the full grid, inside the decay ring --------------------
    for (std::uint32_t gr = 0; gr < params.gridRows; ++gr) {
        const std::uint32_t dst = gr + params.decayRows;
        for (std::uint32_t gc = 0; gc < grid.w; ++gc) {
            const std::size_t s = (static_cast<std::size_t>(gr) * grid.w + gc) * 2u;
            const std::size_t d = (static_cast<std::size_t>(dst) * grid.w + gc) * 2u;
            grid.uv[d + 0u] = measured[s + 0u];
            grid.uv[d + 1u] = measured[s + 1u];
        }
    }

    // ---- anisotropic smoothing --------------------------------------------
    // The cross-meridian component is smoothed harder than the along-meridian
    // one, for the same reason it was scaled down: it is the less trustworthy
    // of the two, and forcing it to agree with its neighbours is how that
    // distrust is expressed spatially rather than only in amplitude.
    if (params.crossMeridianSmooth > 0.0) {
        blurComponent(grid.uv, grid.w, grid.h, 0, params.crossMeridianSmooth);
    }
    // A light blur on v as well: the grid cells are independent averages and
    // cell-to-cell noise becomes a visible ripple once it is a rotation.
    blurComponent(grid.uv, grid.w, grid.h, 1, 1.0);

    // ---- boundary decay ----------------------------------------------------
    // The header's central point: the correction must reach EXACTLY zero at
    // the grid's latitude edges, because the kernel returns zero for any ray
    // outside them.  Anything else is a step discontinuity at the band edge.
    //
    // Note this runs AFTER the smoothing, not before.  Smoothing a field that
    // was already zeroed at the edge would pull the zero inward and leave a
    // small non-zero value at the boundary row - which is precisely the tear
    // the ring exists to prevent.
    if (params.decayRows > 0) {
        for (std::uint32_t gr = 0; gr < grid.h; ++gr) {
            double scale = 1.0;
            if (gr < params.decayRows) {
                // Row 0 is fully outside (scale 0); the first measured row is
                // fully inside.  +1 in the denominator keeps row 0 at exactly
                // zero rather than at one step above it.
                scale = smoothstep01(static_cast<double>(gr) / static_cast<double>(params.decayRows));
            } else if (gr >= params.decayRows + params.gridRows) {
                const std::uint32_t into = gr - (params.decayRows + params.gridRows) + 1u;
                scale = smoothstep01(1.0 - static_cast<double>(into) / static_cast<double>(params.decayRows));
            }
            if (scale >= 1.0) {
                continue;
            }
            for (std::uint32_t gc = 0; gc < grid.w; ++gc) {
                const std::size_t d = (static_cast<std::size_t>(gr) * grid.w + gc) * 2u;
                grid.uv[d + 0u] = static_cast<float>(grid.uv[d + 0u] * scale);
                grid.uv[d + 1u] = static_cast<float>(grid.uv[d + 1u] * scale);
            }
        }
    }

    // ---- diagnostics --------------------------------------------------------
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    std::size_t counted = 0;
    for (std::uint32_t gr = params.decayRows; gr < params.decayRows + params.gridRows; ++gr) {
        for (std::uint32_t gc = 0; gc < grid.w; ++gc) {
            const std::size_t d = (static_cast<std::size_t>(gr) * grid.w + gc) * 2u;
            const double mag = std::sqrt(static_cast<double>(grid.uv[d]) * grid.uv[d] +
                                         static_cast<double>(grid.uv[d + 1u]) * grid.uv[d + 1u]);
            sumAbs += mag;
            maxAbs = std::max(maxAbs, mag);
            ++counted;
        }
    }
    const double degPerRad = 180.0 / osv::kPi;
    grid.meanAbsCorrectionDeg = counted ? (sumAbs / static_cast<double>(counted)) * degPerRad : 0.0;
    grid.maxAbsCorrectionDeg = maxAbs * degPerRad;
    return grid;
}

// ---------------------------------------------------------------------------
//  Full measurement
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> buildParallaxWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                           const geom::BlendParams& blend, const ParallaxWarpParams& params,
                                           const std::vector<float>* seamTable, ThreadPool& pool) {
    OSV_TRY(checkParams(params));

    // Render the two per-lens bands.  Passing the seam table through means
    // the flow measures the RESIDUAL parallax the 1-D correction left behind,
    // so the two mechanisms compose instead of both claiming the same
    // disparity and over-correcting it.
    OSV_TRY_ASSIGN(LensBands bands, renderLensBands(rig, frames, blend, params.band, false, seamTable, pool));
    if (bands.w == 0 || bands.h == 0) {
        return Error{ErrorCode::Internal, "buildParallaxWarp: band render produced nothing"};
    }

    // The band luma planes are already exactly the shape the flow solver
    // wants, so there is no conversion here beyond wrapping them.
    GrayImage a;
    a.w = bands.w;
    a.h = bands.h;
    a.data = bands.luma[0];
    GrayImage b;
    b.w = bands.w;
    b.h = bands.h;
    b.data = bands.luma[1];
    if (!a.valid() || !b.valid()) {
        return Error{ErrorCode::Internal, "buildParallaxWarp: band planes are malformed"};
    }

    FlowBackendKind used = params.backend;
    OSV_TRY_ASSIGN(BidirFlow flow, computeFlow(params.backend, a, b, params.flow, &pool, &used));

    OSV_TRY_ASSIGN(ParallaxWarpGrid grid, gridFromFlow(bands, flow, params));
    grid.usedBackend = used;

    // Refuse to act on a measurement that mostly failed its own consistency
    // check.  On featureless content - open sky, water - the solver has
    // nothing to lock onto and most of the field is repaired guesses;
    // applying that is worse than applying nothing.
    if (grid.consistentFraction() < params.minConsistentFraction) {
        return Error{ErrorCode::Unsupported, "parallax flow too inconsistent to use"};
    }
    return grid;
}

}  // namespace osv::render
