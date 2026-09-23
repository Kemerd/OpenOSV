// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensShading.h - the per-lens shading correction ("Lens Shading" in Source
// Settings; docs/research/NEURAL_STITCHING.md, section 9).
//
// ---------------------------------------------------------------------------
//  What is left in the sky after the photometric seam field
// ---------------------------------------------------------------------------
// The photometric seam field (PhotoSeam.h) corrects the RATIO between the two
// lenses where both see the sky.  Anything that belongs to ONE lens and is
// the same in both lenses' view of a direction cancels in that ratio, and
// anything that belongs to one lens but sits where the other lens's rim is
// untrusted is never measured at all.  Measured on the sample clip, through
// the kernel, the soft dark band that remains on the master side of every
// sky seam crossing is exactly such a thing:
//
//   * a ring in the MASTER lens's own image, centred on its axis at ~86 deg
//     (83-89 deg), up to 0.34 stop deep in luma, the same in frames 0, 32
//     and 64 (to 0.02 stop), at the same angle in every azimuth sector;
//   * deepest on the side facing the sun and shallow toward the horizon;
//   * NEUTRAL IN LINEAR LIGHT: R, G and B lose 0.026 / 0.026 / 0.018 of
//     scene-linear light at its deepest, which is 0.51 / 0.31 / 0.10 stop -
//     the signature of an ADDITIVE deficit (a structured veiling glare), not
//     of a multiplicative vignette, which would dim every channel by the
//     same number of stops;
//   * where it lies (the master side of the seam, 1-7 deg from it), the
//     slave lens is past its own usable rim, so the photometric field
//     neither sees it nor hides it: the kernel shows the master lens there,
//     ring and all.
//
// ---------------------------------------------------------------------------
//  The correction
// ---------------------------------------------------------------------------
// Per lens, a table of the scene-linear light to ADD at (theta, phi) - the
// angle from the lens axis and the azimuth around it - over the rim zone
// [anchorDeg, thetaMax - domainBelowThetaMaxDeg], zero at and below the
// anchor and tapered to zero above the domain (the rim itself is the
// photometric field's usable-rim business), plus a per-channel colour
// factor.  The kernel ([WP-VIGNETTE] in osv_kernel.h) adds it to each lens's
// decoded native-linear sample after the flare removal and before every
// gain, from a rank-2 separable form of the table that fits the parameter
// block: no new table, so the correction reaches every renderer and the
// direct path with the parameters themselves.
//
// ---------------------------------------------------------------------------
//  The measurement
// ---------------------------------------------------------------------------
// From the lens's own image, in the sky - the approach of single-image
// vignetting estimation (Zheng et al., CVPR 2006 / PAMI 2009) specialised to
// the one smooth thing a 360 camera almost always sees:
//
//   1. each lens alone over a +-27 deg polar band (renderPhotoBands, native
//      linear, occlusion alpha, no FOV feather), so every band column is a
//      meridian of the lens from 64 deg to its rim;
//   2. per column, a robust log-quadratic of each channel against theta -
//      the sky - fitted to the image plus the current correction, on FLAT
//      pixels only (a texture gate along the ring and a looser one along
//      theta), and the column kept only when its pixel noise (second
//      differences, blind to smooth structure) and the fit's robust scatter
//      both say it is sky.  The fit is Tukey's biweight, which rejects a deep
//      ring outright rather than letting it pull the sky down;
//   3. per (theta knot, azimuth sector) cell, the median linear residual:
//      the light the lens is missing (or has in excess) there.  Cells that
//      fail a plausibility test are dropped: more than maxRelativeAmount of
//      the sky level (a horizon or an object, not shading), or channels that
//      disagree in sign (additive light moves all three the same way);
//   4. back-fitting: the correction is added back and the sky refitted, a
//      few rounds (Hastie & Tibshirani, 1990), so the quadratic does not
//      absorb part of the ring;
//   5. sectors with too little sky stay at zero - no evidence, no
//      correction - and a lens whose table never leaves the noise is zero.
//
// It is estimated per bucket of frames (render::parallaxBucket), like the
// photometric field, with the same temporal filter: an EMA against the
// previous bucket's STORED model and a cross-fade from it within the bucket,
// both frozen when the bucket is stored (LensShadingHistory), because the
// measured structure follows the sun, which moves in lens coordinates when
// the camera turns.  The photometric field and the exposure match are then
// measured on bands with the correction applied, so their estimates see the
// devignetted lenses the kernel will blend.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/osv_kernel.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Mode and parameters
// ===========================================================================

/// Whether the correction runs.  The numeric values match PrefsLensShading.
enum class LensShadingMode : int {
    Off = 0,   ///< Nothing is measured or added.
    Auto = 1,  ///< Measured per bucket and added where the sky shows it.
};

/// Short name for logs and osvtool ("off", "auto").
[[nodiscard]] const char* lensShadingModeName(LensShadingMode mode) noexcept;

/// Every knob of the measurement and of its application.  The defaults are
/// the ones measured on the sample clip (NEURAL_STITCHING.md, section 9).
struct LensShadingParams {
    LensShadingMode mode = LensShadingMode::Auto;
    /// Analysis band: 1024 columns (0.35 deg per pixel) x +-27 deg, so every
    /// column holds each lens from ~63 deg to its rim (the fit window) even
    /// where the lens axis is tilted a degree off the pole.
    BandParams band{1024, 27.0};

    // ---- the per-column sky fit ----------------------------------------------
    double fitLoDeg = 64.0;               ///< Fit window starts at this theta.
    double domainBelowThetaMaxDeg = 4.0;  ///< Fit window and correction end at thetaMax - this (93.6 deg).
    std::uint32_t minColumnPixels = 40;   ///< Flat window pixels a column needs to be fitted.
    double maxColumnNoiseStops = 0.04;    ///< Texture gate: the column's pixel noise (second differences).
    double maxColumnScaleStops = 0.08;    ///< Sky gate: robust scatter about the column's sky model.
    double maxAlongGradient = 0.15;       ///< Flat gate along the ring (longitude), stops per degree.
    double maxRadialGradient = 0.6;       ///< Flat gate along theta, stops per degree (the ring's own
                                          ///< slope is ~0.15 in luma, ~0.3 in red: it must pass).
    int iterations = 3;                   ///< Back-fitting rounds (the first one fits the raw image;
                                          ///< 3, 4 and 8 agree to 0.3 % of the sky on the sample).

    // ---- the table -------------------------------------------------------------
    double anchorDeg = 76.0;           ///< Knot 0; the correction is 0 at and below it.
    double knotDeg = 0.5;              ///< Radial knot spacing (OSV_SHADE_THETA_N knots: 76-95.5 deg).
    double taperDeg = 1.0;             ///< Above the domain the correction falls to 0 over this.
    std::uint32_t minCellPixels = 24;  ///< Residual samples a (knot, sector) cell needs.
    std::uint32_t minCellColumns = 4;  ///< ... from at least this many distinct columns.
    double minSectorCoverage = 0.7;    ///< Fraction of a sector's domain cells that must be valid.
    double maxRelativeAmount = 0.3;    ///< |amount| above this x the cell's sky level is not shading.
    double minPeakRelative = 0.02;     ///< A lens whose |amount| / level never exceeds this is zero.

    // ---- application --------------------------------------------------------------
    double strength = 1.0;        ///< User strength 0..1.
    double temporalAlpha = 0.35;  ///< EMA weight of a new bucket's measurement.
};

// ===========================================================================
//  The model
// ===========================================================================

/// Number of cells of one lens's table.
inline constexpr std::size_t kLensShadingCells =
    static_cast<std::size_t>(OSV_SHADE_THETA_N) * static_cast<std::size_t>(OSV_SHADE_PHI_N);

/// One lens's measured correction, at full table resolution.
struct LensShadingLens {
    /// Per-channel factor of the amount, normalised so that its BT.2020 luma
    /// weighting is 1 (neutral light is 1, 1, 1).
    std::array<float, 3> colour{1.0f, 1.0f, 1.0f};
    /// Scene-linear light to add, kLensShadingCells floats, [knot * PHI_N +
    /// sector]: knot k at anchor + k * knot degrees, sector s centred at
    /// -180 + (s + 0.5) * 360 / PHI_N degrees of lens azimuth.
    std::vector<float> amount;
    /// Which sectors saw enough sky to be measured (PHI_N bytes); the others
    /// hold 0 in `amount`.
    std::vector<std::uint8_t> sectorMeasured;

    // ---- diagnostics ------------------------------------------------------------
    std::uint32_t skyColumns = 0;       ///< Band columns that passed the sky gate (last round).
    std::uint32_t measuredSectors = 0;  ///< Sectors with a measurement.
    double peakAmount = 0.0;            ///< The amount of largest |amount| / sky level, scene-linear.
    double peakStops = 0.0;             ///< The lens's own dip there, luma stops (negative = darker).
    double peakThetaDeg = 0.0;          ///< Where it is.
    double peakPhiDeg = 0.0;

    /// True when every amount is zero.
    [[nodiscard]] bool isZero() const noexcept;
};

/// Both lenses' corrections plus the table geometry.
struct LensShadingModel {
    float theta0Rad = 0.0f;   ///< Knot 0 (the anchor).
    float dThetaRad = 0.0f;   ///< Knot spacing.
    LensShadingLens lens[2];  ///< [0] slave, [1] master, like the rig.
    double bandMs = 0.0;      ///< Band shading (GPU or CPU) and download.
    double statsMs = 0.0;     ///< Everything after the bands.

    /// Geometry positive and finite, both tables the right size and finite.
    [[nodiscard]] bool valid() const noexcept;
    /// valid() and at least one lens with a non-zero amount.
    [[nodiscard]] bool active() const noexcept;
};

/// A model that corrects nothing, with the geometry of `params` (for tests
/// and as the EMA's starting point).
[[nodiscard]] LensShadingModel emptyLensShadingModel(const LensShadingParams& params);

/// `model` with every amount multiplied by `strength` (clamped to [0, 1];
/// non-finite = 0), colours and diagnostics kept: the correction a user
/// strength asks for, as one model the analyses and the kernel both use.
[[nodiscard]] LensShadingModel scaledLensShadingModel(const LensShadingModel& model, double strength);

// ===========================================================================
//  Measurement
// ===========================================================================

/// The per-lens bands the measurement reads: renderPhotoBands over
/// `params.band` with the ANALYSIS blend (occlusion alpha, no FOV feather,
/// native linear light).  Host frames on the CPU, device frames through the
/// installed DeviceBandShader.
[[nodiscard]] Result<RgbLensBands> renderShadingBands(const geom::LensRig& rig, const video::FramePair& frames,
                                                      const geom::BlendParams& blend, const LensShadingParams& params,
                                                      ThreadPool& pool);

/// The model from bands (pure).  `rig` supplies each band pixel's azimuth
/// around each lens axis (the bands carry theta only).  `pool` may be null
/// (single-threaded).  InvalidArgument for bands that do not match the
/// parameters or a rig that does not fit them.
[[nodiscard]] Result<LensShadingModel> lensShadingFromBands(const RgbLensBands& bands, const geom::LensRig& rig,
                                                            const LensShadingParams& params, ThreadPool* pool);

/// renderShadingBands + lensShadingFromBands, with the timings filled in.
[[nodiscard]] Result<LensShadingModel> measureLensShading(const geom::LensRig& rig, const video::FramePair& frames,
                                                          const geom::BlendParams& blend,
                                                          const LensShadingParams& params, ThreadPool& pool);

// ===========================================================================
//  Time
// ===========================================================================

/// from + (to - from) * t on every amount and colour; t is clamped to [0, 1]
/// and the endpoints return `from` / `to` exactly.  A sector counts as
/// measured when either side measured it.  InvalidArgument for a geometry
/// mismatch or a non-finite t.  The result carries `to`'s diagnostics.
[[nodiscard]] Result<LensShadingModel> blendLensShadingModels(const LensShadingModel& from, const LensShadingModel& to,
                                                              double t);

/// The importer's per-clip model cache and temporal filter (and osvtool's),
/// with PhotoSeamHistory's semantics:
///
///   * store(bucket, measured): an EMA against the previous bucket's STORED
///     model (temporalAlpha; a sector the new bucket did not measure decays
///     toward zero, a newly measured one ramps in), or the measurement as it
///     is when the previous bucket was never stored;
///   * modelFor(frame): the bucket's model cross-faded from the previous
///     bucket's (parallaxCrossfadeWeight, exactly like the parallax grid).
///
/// Everything a bucket renders with is fixed when it is STORED, so a frame
/// renders identically every time it is asked for, whatever is rendered in
/// between.  Not thread-safe; the importer holds its instance lock.
class LensShadingHistory {
public:
    /// Record bucket `bucket`'s measurement; nullptr records a refusal (not
    /// measured again; the bucket renders without a correction).
    void store(std::uint32_t bucket, const std::shared_ptr<const LensShadingModel>& measured,
               const LensShadingParams& params);

    /// True once `bucket` was measured (accepted or refused).
    [[nodiscard]] bool measured(std::uint32_t bucket) const;

    /// The model frame `frame` renders with, or nullptr when its bucket was
    /// refused or not measured yet.
    [[nodiscard]] std::shared_ptr<const LensShadingModel> modelFor(std::uint32_t frame,
                                                                   const LensShadingParams& params) const;

    /// Bound the cache: beyond `limit` entries the lowest buckets go (never
    /// `keep`; then the highest).
    void trim(std::size_t limit, std::uint32_t keep);

    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return m_models.size(); }

private:
    /// One bucket as stored.  Both pointers are fixed at store() time.
    struct Entry {
        std::shared_ptr<const LensShadingModel> model;  ///< Its own (filtered) model; null records a refusal.
        std::shared_ptr<const LensShadingModel> from;   ///< The previous bucket's model then, or null: no glide.
    };
    std::map<std::uint32_t, Entry> m_models;
};

// ===========================================================================
//  Evaluation and the kernel block
// ===========================================================================

/// The table itself at (theta, phi) for `lens`, bilinear in (knot, sector)
/// with the kernel's conventions (0 at and below knot 0, held at the last
/// knot, sectors wrapping), in scene-linear luma units.  NaN for an invalid
/// model or lens index.  The reference the kernel's separable form is
/// measured against.
[[nodiscard]] double lensShadingTableAt(const LensShadingModel& model, int lens, double thetaRad,
                                        double phiRad) noexcept;

/// Fill `p`'s [WP-VIGNETTE] block from `model` at `strength` (clamped to
/// [0, 1]): each lens's table reduced to its best rank-OSV_SHADE_RANK
/// separable form (singular value decomposition), the colour factors, the
/// knot geometry.  An inactive or invalid model, or a zero strength, clears
/// the block to all zero (the correction off) and returns false.
bool fillLensShadingBlock(const LensShadingModel& model, double strength, OsvRenderParams& p) noexcept;

/// Largest |separable - table| over every cell of every lens, scene-linear
/// (the price of the rank-2 form; 0 for an inactive model).
[[nodiscard]] double lensShadingSeparableError(const LensShadingModel& model) noexcept;

/// The [WP-VIGNETTE] block of `p` is one a kernel can evaluate: off, or on
/// with a positive finite knot geometry, a strength within [0, 1] and every
/// factor finite.  The direct path refuses a block that is not, rather than
/// render with it (DirectReject::Shading).
[[nodiscard]] bool lensShadingBlockValid(const OsvRenderParams& p) noexcept;

}  // namespace osv::render
