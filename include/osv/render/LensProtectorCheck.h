// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensProtectorCheck: score the lens-protector correction on real pixels.
//
// DJI's own software applies the protector correction in one direction
// (geom::ProtectorDirection::Forward).  We still have no footage
// shot through a protector, so this check is the runtime guard the importer
// runs once per clip before it trusts the correction: it renders the seam
// overlap band of one frame pair three ways - no correction, forward,
// inverse - and measures how well the two lenses agree in each.
//
// The comparison is made on ONE pixel set: the band pixels both lenses cover
// under all three rigs.  The corrections move the usable FOV (by about
// 1.5 deg per lens), so each rig alone would score a slightly different
// strip, and a wider strip of worse-matching rim pixels would bias the
// comparison against whichever rig sees furthest.
//
// Decision rule (guard, not a vote): the preferred direction stays unless it
// scores CLEARLY worse than no correction (kProtectorClearlyWorse), in which
// case the footage evidently is not behind a protector and the correction is
// switched off.  A frame with too little structure to judge (every score
// below kProtectorMinReliableNcc, or too few co-visible pixels) keeps the
// preferred direction: no evidence is not evidence against it.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensProtector.h"
#include "osv/geom/LensRig.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <string>

namespace osv::render {

/// NCC margin by which the preferred direction must lose to "none" before
/// the guard switches the correction off.  On the bare-lens sample clip the
/// forward correction loses by far more than this; see the unit tests.
inline constexpr double kProtectorClearlyWorse = 0.02;

/// Below this NCC for every variant the frame is too flat to judge.
inline constexpr double kProtectorMinReliableNcc = 0.25;

/// Fewer co-visible band pixels than this and the frame is not judged.
inline constexpr std::uint64_t kProtectorMinSamples = 4096;

/// What the guard measured and decided.
struct ProtectorScores {
    /// Overlap NCC indexed by geom::ProtectorDirection (None, Forward, Inverse).
    std::array<double, 3> ncc{};
    std::uint64_t samples = 0;  ///< Co-visible band pixels scored (same set for all three).
    bool reliable = false;      ///< False when the frame was too flat or too occluded to judge.
    geom::ProtectorDirection preferred = geom::ProtectorDirection::Forward;  ///< The order DJI's own software applies.
    geom::ProtectorDirection pick = geom::ProtectorDirection::Forward;       ///< What to apply.
    std::string summary;        ///< "none 0.8253, forward 0.7011, inverse 0.6934 -> none (...)".
};

/// Score the three directions on `frames` (host planes) with `baseRig` /
/// `baseBlend` as the bare-lens geometry, and decide per the rule above.
/// InvalidArgument for device-only frames or a bad rig; any band-render
/// failure is returned as is.
[[nodiscard]] Result<ProtectorScores> scoreLensProtector(const geom::LensRig& baseRig,
                                                        const geom::BlendParams& baseBlend,
                                                        const video::FramePair& frames, ThreadPool& pool,
                                                        geom::ProtectorDirection preferred =
                                                            geom::ProtectorDirection::Forward);

}  // namespace osv::render
