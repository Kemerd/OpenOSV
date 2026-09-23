// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensProtectorCheck implementation (see the header for the rule).

#include "osv/render/LensProtectorCheck.h"

#include "osv/core/Log.h"
#include "osv/render/SeamAnalysis.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace osv::render {

namespace {

/// The three variants, in ProtectorScores::ncc order.
constexpr std::array<geom::ProtectorDirection, 3> kVariants = {
    geom::ProtectorDirection::None, geom::ProtectorDirection::Forward, geom::ProtectorDirection::Inverse};

/// Normalised cross-correlation of the two lenses' luma over `mask`.
/// 0 when the set is empty or either side is flat.
double maskedNcc(const LensBands& b, const std::vector<std::uint8_t>& mask) noexcept {
    const std::size_t n = mask.size();
    double sa = 0.0, sb = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (mask[i]) {
            sa += b.luma[0][i];
            sb += b.luma[1][i];
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    const double ma = sa / static_cast<double>(count);
    const double mb = sb / static_cast<double>(count);
    double num = 0.0, da = 0.0, db = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!mask[i]) {
            continue;
        }
        const double x = b.luma[0][i] - ma;
        const double y = b.luma[1][i] - mb;
        num += x * y;
        da += x * x;
        db += y * y;
    }
    return (da > 0.0 && db > 0.0) ? num / std::sqrt(da * db) : 0.0;
}

}  // namespace

Result<ProtectorScores> scoreLensProtector(const geom::LensRig& baseRig, const geom::BlendParams& baseBlend,
                                           const video::FramePair& frames, ThreadPool& pool,
                                           geom::ProtectorDirection preferred) {
    // ---- inputs ---------------------------------------------------------------
    if (!frames.valid()) {
        return Error{ErrorCode::InvalidArgument, "lens protector check: needs host frames for both lenses"};
    }
    if (!baseRig.lens[0].isValid() || !baseRig.lens[1].isValid()) {
        return Error{ErrorCode::InvalidArgument, "lens protector check: the base rig is invalid"};
    }
    if (preferred != geom::ProtectorDirection::Forward && preferred != geom::ProtectorDirection::Inverse) {
        return Error{ErrorCode::InvalidArgument, "lens protector check: the preferred direction must be a correction"};
    }

    // ---- render the band three ways ------------------------------------------
    // +/- 4 deg around the seam plane: inside every variant's usable FOV
    // (the forward one ends at ~96.1 deg, 6 deg past the seam).
    BandParams band;
    band.equirectW = 2048;
    band.bandHalfDeg = 4.0;
    std::array<LensBands, 3> bands{};
    for (std::size_t v = 0; v < kVariants.size(); ++v) {
        geom::LensRig rig = baseRig;
        geom::BlendParams blend = baseBlend;
        OSV_TRY_ASSIGN(const geom::ProtectorRigFold fold,
                       geom::applyLensProtector(rig, kVariants[v], baseBlend.lensFovDeg));
        blend.lensFovDeg = fold.lensFovDeg;
        OSV_TRY_ASSIGN(bands[v], renderLensBands(rig, frames, blend, band, false, nullptr, pool));
    }
    const std::size_t n = bands[0].luma[0].size();
    for (const LensBands& b : bands) {
        if (b.luma[0].size() != n || b.luma[1].size() != n || b.alpha[0].size() != n || b.alpha[1].size() != n) {
            return Error{ErrorCode::Internal, "lens protector check: the three bands differ in size"};
        }
    }

    // ---- one pixel set for all three: co-visible under every rig --------------
    std::vector<std::uint8_t> mask(n, 1);
    std::uint64_t samples = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (const LensBands& b : bands) {
            if (!(b.alpha[0][i] > 0.5f && b.alpha[1][i] > 0.5f)) {
                mask[i] = 0;
                break;
            }
        }
        samples += mask[i];
    }

    ProtectorScores s;
    s.samples = samples;
    s.preferred = preferred;
    for (std::size_t v = 0; v < kVariants.size(); ++v) {
        s.ncc[v] = maskedNcc(bands[v], mask);
    }

    // ---- decide: keep the preferred direction unless it clearly loses ---------
    const double none = s.ncc[static_cast<std::size_t>(geom::ProtectorDirection::None)];
    const double pref = s.ncc[static_cast<std::size_t>(preferred)];
    const double best = std::max({s.ncc[0], s.ncc[1], s.ncc[2]});
    s.reliable = samples >= kProtectorMinSamples && best >= kProtectorMinReliableNcc;
    std::string why;
    if (!s.reliable) {
        s.pick = preferred;
        why = std::format("frame too flat or occluded to judge ({} px, best {:.4f}); keeping {}", samples, best,
                          geom::protectorDirectionName(preferred));
    } else if (pref < none - kProtectorClearlyWorse) {
        s.pick = geom::ProtectorDirection::None;
        why = std::format("{} is clearly worse than no correction ({:+.4f}); the footage does not look like it was "
                          "shot through a protector",
                          geom::protectorDirectionName(preferred), pref - none);
    } else {
        s.pick = preferred;
        why = std::format("{} is not worse than no correction ({:+.4f})", geom::protectorDirectionName(preferred),
                          pref - none);
    }
    s.summary = std::format("none {:.4f}, forward {:.4f}, inverse {:.4f} over {} px -> {} ({})", s.ncc[0], s.ncc[1],
                            s.ncc[2], samples, geom::protectorDirectionName(s.pick), why);
    log::debug("lens protector check: {}", s.summary);
    return s;
}

}  // namespace osv::render
