// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MeshWarp.h - ONE correction field for the seam band: a content-preserving
// mesh warp solved as a single sparse least-squares problem.
//
// WHY THIS EXISTS
// ---------------
// Up to 0.5.1 the seam correction was a composition of three different
// things: a 1-D per-column seam table (SeamAnalysis.h), a 2-D flow grid
// (ParallaxWarp.h) and a per-column guard (guardGridWithTable) that switched
// a grid column over to the table where the two disagreed.  Every switch
// between two fields puts a kink into every straight line that crosses the
// band - a roof edge, a hood crease, a lamp pole - and the overlap score the
// guard was tuned on cannot see a kink.  On a car-mounted clip the guard's
// columns sit exactly where the car body crosses the seam, so the roof edge
// came out with a bump a few degrees wide.  And every bucket re-measured
// both fields from scratch, so the bump moved from one bucket to the next.
//
// The literature settled both problems a decade ago with ONE warp solved
// as a sparse quadratic energy:
//
//   * Liao & Li, "Single-Perspective Warps in Natural Image Stitching",
//     IEEE TIP 2019 (arXiv 1802.04645): a mesh warp whose vertices are the
//     unknowns, every sample point a bilinear combination of its four cell
//     vertices (their eq. 9), an alignment term over point matches (eq. 11),
//     line-preserving terms over sampled line segments (eqs. 13, 16) and a
//     shape term, stacked into one sparse linear least-squares system
//     (eq. 17).
//   * Jia, Li, Fan, Zhao, Teng, Ye & Latecki, "Leveraging Line-point
//     Consistence to Preserve Structures for Wide Parallax Image Stitching",
//     CVPR 2021:
//     the same with explicit collinearity of sampled line points (eq. 5), and
//     the evaluation this file's line metric follows (eq. 8: fit a straight
//     line to the warped samples of each line and report the residual).
//   * Jiang & Gu, "Video Stitching with Spatial-Temporal Content-Preserving
//     Warping", CVPR Workshops 2015: the mesh of frame t solved with a
//     temporal term against frame t-1's mesh (their E_gt, eq. 9, weighted
//     more where the scene is static) and a pull toward the pre-warped mesh
//     where no match guides a vertex (E_gs, eq. 4), in the same system.
//   * Ho & Budagavi, "Dual-fisheye lens stitching for 360-degree imaging"
//     (arXiv 1708.08988) and Ho, Schizas, Rao & Budagavi, "360-degree video
//     stitching for dual-fisheye lens cameras based on rigid moving least
//     squares" (ICIP 2017): the interpolation-grid formulation for exactly
//     this kind of camera, with temporal coherence for jitter-free video.
//   * Zaragoza, Chin, Brown & Suter, "As-Projective-As-Possible Image
//     Stitching with Moving DLT" (CVPR 2013) and Chang, Sato & Chuang,
//     "Shape-Preserving Half-Projective Warps for Image Stitching" (CVPR
//     2014): why a warp must decay to a rigid transform away from the
//     matched region - here, to exactly zero at the mesh's latitude edges.
//
// We adopt their ENERGY, not their feature pipelines: the matches come from
// the DIS flow the analysis already computes (dense, forward-backward
// checked, on the two lens bands), and the lines from a small LSD-style
// detector run on the bands themselves (von Gioi, Jakubowicz, Morel &
// Randall, "LSD: a Line Segment Detector", IPOL 2012).
//
// THE UNKNOWNS AND THE OUTPUT
// ---------------------------
// The mesh IS the kernel's grid.  Its vertices are a regular lattice in the
// polar-axis (longitude, latitude) frame of the seam band: meshCols columns
// round the ring (longitude wraps) by rows from +reachDeg to -reachDeg; per
// vertex the unknown is the master lens's (dLon, dLat) displacement, the
// slave taking the negation - exactly a ParallaxWarpGrid.  The kernel samples
// that grid bilinearly (osvWarpSample), and bilinear interpolation of the
// vertices IS the mesh warp of Liao & Li's eq. 9, so the field every energy
// term is evaluated on is the field the kernel renders, bit for bit in the
// geometry.  No rasterisation step stands between the solve and the picture,
// which is why the mesh was not made coarser and rasterised to the old 256 x
// 48 layout: that would interpolate twice and evaluate the line term on a
// field nobody renders.  The cost of that choice is the lattice size, kept to
// under two thousand vertices (128 x 13 at the defaults) so the solve stays
// in the per-bucket budget.
//
// The lattice reaches to +/- 12 degrees, where the kernel's 1-D seam shift
// also ends (OSV_SEAM_SHIFT_ZERO_DEG): one field replacing the table must act
// wherever the table acted, or the table's lift (below) could not be the
// table.  The outermost rows are pinned to zero, so the field meets the
// uncorrected sphere without a step (the grid's decay ring, C0 at its edge;
// the shape term makes it smooth inside).
//
// THE ENERGY (all quadratic; one sparse least-squares system)
// ----------------------------------------------------------
//   E(V) = s E_align + E_line + E_shape + E_anchor + E_temporal
//
//   E_align    Sum over flow matches i:  w_i |B_i V - m_i|^2, where B_i is the
//              bilinear interpolation at the match's band position and m_i
//              the measured half disparity (symmetric DIS flow, plus the
//              band warp the bands were rendered with).  w_i = structure x
//              benefit (x a Cauchy robust weight when asked).  s is the
//              structured gate's strength (parallaxGateStrength): the gate
//              stays as the data term's weight, never as a switch between two
//              fields.  The matches come from up to two measurements (see
//              MeshWarpInputs): the flow on the raw bands and the flow on the
//              bands prewarped by the seam table's lift.
//   E_line     Sum over detected segments, over consecutive sample triples
//              (the second difference D2 V = V(p_k-1) - 2 V(p_k) + V(p_k+1),
//              Liao & Li's eq. 13, third term, on detected lines): the
//              collinearity residual  lambda_l (n . D2 V)^2  - the warped
//              samples stay on a straight line exactly when it vanishes.
//              Measured against the alternatives on the day proxy (frame
//              3423, car window / line RMS): this exact form 0.9907 / 0.037
//              px; the full |D2 V|^2 0.9777 / 0.074 px (it also forces dLon
//              affine along every roof edge, against the data); its separable
//              majorizer alone 0.9832 / 0.063 px.  (The first-difference form
//              of Liao & Li eq. 16 / Jia eq. 5 also forbids ROTATING a line,
//              which a genuine disparity gradient along it does.)
//   E_shape    Membrane + bending (first and second differences, ring-wrapped
//              in longitude) of the DEVIATION V - V0 from the prior field
//              V0, stronger in the decay rings beyond the analysed band; the
//              membrane keeps the field from overshooting across a gap in the
//              data (MeshWarpParams::shapeMembrane).
//   E_anchor   Jiang & Gu's E_gs: a pull of each vertex toward V0, strong
//              where no match supports the vertex and weak where one does,
//              so the field collapses to the prior where nothing is measured.
//   E_temporal Jiang & Gu's E_gt: a pull toward the previous solve's mesh,
//              weighted by the vertex's own data evidence (and a floor) and -
//              like their sigma, larger where the scene is static - by a
//              Cauchy weight on the change the data ask for, so a static
//              scene holds still while a real change (a car passing) is
//              followed within a bucket or two.
//
// V0, the PRIOR, is the seam table lifted into the mesh (liftSeamTable): the
// 1-D shift along the meridian, away from each lens's axis, tapered exactly
// like the kernel's own 1-D path.  With no matches, no lines and no previous
// mesh the solution IS V0, bit for bit - so a frame with nothing to measure
// (sky, fog) still renders exactly one field through exactly one code path.
//
// THE SOLVER
// ----------
// One banded SPD system over both components, interleaved per vertex: the
// ring is ordered by folding (0, W-1, 1, W-2, ...), so every term - which
// couples columns at most two apart - lies within a band of half width
// 2 (5 R - 1) + 1, R the free rows, and the line term's normal, which mixes
// dLon and dLat at an oblique line, stays inside it.  A banded Cholesky
// solves it exactly: about 17 million multiply-adds at the defaults (128 x
// 11 free vertices, half width 109), ~4 ms.  (Two separate blocks with the
// oblique lines' cross part left to conjugate gradients needed 20-130
// iterations per solve - a line is stiff along its normal and free along its
// tangent, which no block preconditioner captures - and the separable
// majorizer of that cross part alone, which needs none, bent the lines
// nearly twice as much.)  IRLS refines the weights, each iteration solved
// exactly: the benefit gate - "a correction must reduce the lens-to-lens
// residual to be trusted" (ParallaxWarpParams::requiredImprovement) - judges
// the first solve once and reweights the matches of every later one; the
// temporal term, left out of the first solve, is weighted by the change that
// solve asked for; and a Cauchy weight on the matches is available.
//
// WHAT THE CALLER OWNS
// --------------------
// The temporal state.  solveMeshWarp takes the previous mesh as an optional
// input and returns the new one; the importer keeps it per clip, osvtool
// render across its range.  The library holds no state between calls and is
// safe to call concurrently on different inputs.

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/FlowBackend.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Line segments on the seam band
// ===========================================================================

/// Tuning of the band line detector (an LSD-style region grower with the
/// a-contrario validation of von Gioi et al., IPOL 2012).
struct LineDetectParams {
    /// Level-line angle tolerance in degrees: LSD's tau.  A pixel joins a
    /// line-support region when its gradient's level-line angle is within
    /// this of the region's.  22.5 is LSD's default and its a-contrario
    /// precision p = tau / 180 = 1/8.
    double angleToleranceDeg = 22.5;
    /// Quantisation noise of the luma, in code values (0..1): LSD's q.  A
    /// pixel whose gradient magnitude is below q / sin(tau) cannot have a
    /// reliable angle and is never a seed or a member.  Two 8-bit code steps,
    /// the structure threshold of the structured gate
    /// (ParallaxWarpParams::minStructureGradient), so "a line" never rests on
    /// less contrast than "structure" does.
    double gradientQuantization = 2.0 / 255.0;
    /// log10 of the largest Number of False Alarms a segment may have and
    /// still be accepted: LSD's epsilon = 1, i.e. 0 here.
    double logEpsilon = 0.0;
    /// Smallest share of a segment's rectangle covered by its aligned region
    /// pixels.  LSD refines a rectangle below 0.7; this detector rejects it
    /// instead (a refused curve is a missing constraint, a refined one could
    /// be a wrong one).
    double minDensity = 0.7;
    /// Segments shorter than this (band pixels) are not reported: at the
    /// line term's sample spacing (8 px) a shorter one holds at most two
    /// sample triples and constrains little a bump could not hide in.
    double minLengthPx = 24.0;
    /// Passes of the luma blur applied before the gradient (a separable
    /// [1 2 1] / 4, normalised over covered pixels), 0 = none.  The band is
    /// a bilinear resampling of a fisheye frame; one blur pass removes the
    /// staircase that would otherwise split a shallow edge into short pieces.
    std::uint32_t blurPasses = 1;
};

/// One straight line segment found on a lens's band, in the band's own
/// polar-axis frame.
///
/// Endpoints are (longitude, latitude) in RADIANS, the same frame as
/// ParallaxWarpGrid, so a segment found on one band (the seam search's taller
/// band, the flow band) can constrain a mesh of any layout.  Longitude is
/// UNWRAPPED along the segment: lon1 may lie beyond +pi when the segment
/// crosses the ring's seam meridian, and lon0 is always in [-pi, pi).
struct SeamLine {
    int lens = 0;              ///< 0 = slave (lens 0), 1 = master (lens 1).
    double lon0Rad = 0.0;      ///< First endpoint, longitude (radians, [-pi, pi)).
    double lat0Rad = 0.0;      ///< First endpoint, latitude (radians).
    double lon1Rad = 0.0;      ///< Second endpoint, longitude (radians, unwrapped).
    double lat1Rad = 0.0;      ///< Second endpoint, latitude (radians).
    double lengthPx = 0.0;     ///< Length in band pixels of the band it was found on.
    double widthPx = 0.0;      ///< Width of its support rectangle (band pixels).
    double logNfa = 0.0;       ///< log10 of its Number of False Alarms (more negative = surer).
    std::uint32_t support = 0; ///< Aligned pixels in its support region.
};

/// What detectSeamLines found, and what it cost.
struct SeamLineSet {
    std::vector<SeamLine> lines;    ///< Both lenses' segments, lens 0 first, each lens in detection order.
    std::uint32_t perLens[2] = {0, 0};
    std::uint32_t regions = 0;      ///< Line-support regions grown (accepted or not).
    std::uint32_t rejected = 0;     ///< Regions refused (too short, too sparse, or not meaningful).
    double ms = 0.0;                ///< Wall time of the detection.
};

/// Detect straight line segments on both lenses' bands.
///
/// Runs on each lens's luma where that lens covers the band (alpha > 0.5):
/// a coverage edge - a rim, an occlusion polygon - never becomes a line,
/// because a gradient is only taken where all four pixels of its 2 x 2
/// window are covered.  Longitude wraps (the band is a ring).  `pool`
/// (optional) runs the two lenses side by side; the result is identical with
/// or without it.
///
/// InvalidArgument for empty or malformed bands or parameters out of range.
[[nodiscard]] Result<SeamLineSet> detectSeamLines(const LensBands& bands, const LineDetectParams& params,
                                                  ThreadPool* pool = nullptr);

// ===========================================================================
//  The mesh warp
// ===========================================================================

/// Tuning of the mesh solve.  The weights are densities over the band's own
/// pixels (band pixels squared), so they mean the same thing whatever the
/// mesh resolution: a fully structured, fully consistent patch of flow has
/// data density alignWeight per band pixel.
struct MeshWarpParams {
    /// The flow band, the flow backend, the structured gate, the benefit gate
    /// (requiredImprovement, minResidual) and the safety clamp
    /// (maxCorrectionDeg) - the measurement side is the grid's own, so the
    /// gate's share and strength are the numbers 0.5.1 logged.
    ParallaxWarpParams parallax;

    // ---- the lattice ----------------------------------------------------------
    /// Columns round the ring (longitude wraps).  128 is 2.8 degrees per
    /// column, which is about the old grid's effective resolution after its
    /// cross-meridian blur (sigma 2 cells of 1.4 degrees).
    std::uint32_t meshCols = 128;
    /// Latitude spacing of the rows, degrees.  The rows run from +reachDeg to
    /// -reachDeg, so 2 x reachDeg must be a whole number of spacings.  2: the
    /// cost of the exact solve grows with the cube of the free rows, and 1.5
    /// (15 free rows against 11) scored within 0.0002 of overlap and 0.01 px
    /// of line straightness on every frame measured at twice the time.
    double rowSpacingDeg = 2.0;
    /// Latitude at which the field is pinned to zero, degrees: the kernel's
    /// 1-D seam shift ends there too (OSV_SEAM_SHIFT_ZERO_DEG).
    double reachDeg = 12.0;

    // ---- the alignment term ----------------------------------------------------
    /// Data density of a fully structured, consistent band pixel.
    double alignWeight = 1.0;
    /// Matches are taken on every matchStride-th band row and column.  A
    /// mesh cell spans ~16 x 8.5 band pixels, so a stride of 2 still gives
    /// it ~34 matches when fully textured.
    std::uint32_t matchStride = 2;
    /// The structure weight rises (smoothstep) from the structured gate's
    /// threshold (parallax.minStructureGradient) to this multiple of it, on
    /// the SMALLER of the two lenses' gradients: the stronger the texture
    /// both lenses see, the better the flow can lock onto it.
    double structureFullFactor = 3.0;
    /// The structure weight AT the threshold: a structured pixel is a real
    /// measurement however faint its texture, so the ramp starts here rather
    /// than at zero (most structured pixels of a D-Log M band sit just above
    /// the threshold, and a ramp from zero discarded them).
    double structureFloor = 0.25;
    /// Weight of a consistent co-visible match WITHOUT structure (relative to
    /// a fully structured one).  0: matches come from the structured pixels
    /// only, whose flow the solver could lock onto.  Measured with the final
    /// solve on the six band-level frames (overlap / car window / line RMS):
    /// 0.25 and 1.0 moved the overlap by at most 0.0007 either way and bent
    /// the lines more (day proxy 3423: 0.016 px at 0, 0.017 at 0.25, 0.030
    /// at 1.0); the grid averaged such pixels in at full weight.
    double unstructuredWeight = 0.0;
    /// Where both measurements matched one pixel, the refined one's share of
    /// that pixel's weight (the raw one takes the rest).  An even split:
    /// all of it to the refined one scored the day car windows 0.001-0.002
    /// higher and the night's 0.005 lower, all of it to the raw one the
    /// reverse.
    double sharedRefinedShare = 0.5;
    /// Scale of the Cauchy robust weight on a match's residual, band pixels
    /// of HALF disparity: w = 1 / (1 + (r / c)^2); 0 = no robust weight.
    /// Off by measurement: at 1 px it discarded exactly the matches a smooth
    /// field reaches last - the near-field car body - and cost the day
    /// proxy's car window 0.94 -> 0.88 (frame 3423); at 3 px it was within
    /// 0.001 of off on every frame.  The forward-backward check and the
    /// benefit gate already remove the outliers it was meant for.
    double robustScalePx = 0.0;

    // ---- the line term ------------------------------------------------------------
    /// Weight of one sample triple's second difference.  A mesh cell holds
    /// ~136 band pixels of data at most (a few at the structured densities
    /// measured); a line crossing it contributes ~2 triples, so a line wins
    /// over the data on a bend by one to three orders of magnitude, while
    /// the affine part along it (a real disparity gradient) costs nothing.
    double lineWeight = 4000.0;
    /// Sample spacing along a segment, band pixels.  Capped at half a mesh
    /// column so a triple never spans more than two columns (the solver's
    /// band width depends on it).
    double lineSampleSpacingPx = 8.0;

    // ---- the shape term ---------------------------------------------------------
    /// Membrane density (first differences of V - V0), dimensionless.  It
    /// is what keeps the field from overshooting across a gap in the data
    /// (harmonic interpolation obeys the maximum principle; bending alone
    /// extrapolates the slopes at the gap's edges - a 100-column gap the
    /// benefit gate cut into wrong data overshot to 7 px at 0.05).  With
    /// shapeBending it crosses over at sqrt(256 / 0.25) = 32 band pixels, two
    /// mesh columns: shorter wiggles are bent, longer gaps are spanned.  On
    /// the clips 0.05 / 0.25 / 1.0 moved no overlap score by more than 0.0007
    /// but the night car window (0.744 / 0.741 / 0.730).
    double shapeMembrane = 0.25;
    /// Bending density (second differences of V - V0), band pixels squared:
    /// (smoothing length in pixels)^4 against a full data density, so 4^4 is
    /// 4 band pixels there - and, at the structured densities measured on the
    /// clips (0.03-0.1 per pixel), 9-12 band pixels (1.6-2.1 degrees), about
    /// the old grid's blur.  8^4 smoothed the near-field car body away (day
    /// proxy frame 3423, raw bands: whole-band overlap 0.9849 at 256 against
    /// 0.9771 at 4096).
    double shapeBending = 256.0;
    /// Multiplier of both shape densities for the cross-meridian component
    /// (dLon).  1 by measurement: the old grid's harder cross-meridian blur
    /// (ParallaxWarpParams::crossMeridianSmooth) as a 16x bending weight cost
    /// 0.001-0.003 of overlap on every frame and changed no line.
    double crossMeridianShapeScale = 1.0;
    /// Multiplier of both shape densities beyond the analysed band (the
    /// decay rings), where nothing is measured and the field must fall to
    /// zero smoothly.
    double decayShapeBoost = 4.0;

    // ---- the anchor (Jiang & Gu's E_gs) ------------------------------------------
    /// Pull toward the prior V0 per band pixel where NO match supports a
    /// vertex; it fades to anchorWeight x anchorFloor where the vertex's data
    /// density reaches anchorDataFull.  Against the shape term's longitude
    /// bending (~0.5 per vertex at the defaults) it sets how far a measured
    /// correction carries into unmeasured columns: at 0.02 less than one
    /// mesh column, which left the thin overlap strip beside a car mount's
    /// zero-overlap arc uncorrected (night drive frame 2000, band columns
    /// 1760-1791: 0.64 against the old grid's 0.93; car window 0.737); at
    /// 0.002 the car window is 0.748, at 0.0005 0.756 - but the airborne
    /// sample's frame 60 then loses 0.004 of overlap (0.9117) where the
    /// field drifts over its featureless strips.
    double anchorWeight = 0.002;
    /// Share of anchorWeight kept under full data support (it keeps the
    /// system positive definite; it must not bias a supported vertex).
    double anchorFloor = 0.01;
    /// Data density (per band pixel, after all weights) at which a vertex
    /// counts as fully supported: a couple of matches near it, like Jiang &
    /// Gu's "a matched feature within 10 pixels".
    double anchorDataFull = 0.02;
    /// Multiplier of the no-data pull where both lenses see a vertex's cells:
    /// there the correction continues smoothly from its measured neighbours
    /// (the lenses still blend there); where only one lens sees them the full
    /// pull holds the prior (a correction would move a picture nothing aligns
    /// it to).  Between, by the co-visible share of the cells' band pixels.
    double anchorCovisibleScale = 0.1;

    // ---- the temporal term (Jiang & Gu's E_gt) -----------------------------------
    /// Pull toward the previous mesh relative to the vertex's own data
    /// evidence: 3 means a static vertex moves a quarter of the way to a new
    /// measurement per solve, which averages measurement noise down to ~0.38
    /// of its per-bucket size.
    double temporalWeight = 3.0;
    /// Pull toward the previous mesh per band pixel regardless of data, so a
    /// vertex with no data does not jump with the prior.
    double temporalFloor = 0.002;
    /// Scale of the temporal term's Cauchy weight, band pixels of half
    /// disparity: the pull toward the previous mesh is multiplied by
    /// 1 / (1 + (change / scale)^2), the change being the one the data ask
    /// for (the first IRLS solve, made without this term).  A change below
    /// it is measurement noise and held (a static vertex moves a quarter of
    /// the way per solve); one of three times it is real and moves ~3/4 of
    /// the way at once.  One pixel is 0.18 degrees of half disparity, about
    /// the flow's own bucket-to-bucket noise on a static scene.
    double temporalScalePx = 1.0;

    // ---- the solver ----------------------------------------------------------------
    /// Solves: the first with structure weights only, each later one with
    /// the benefit gate's weights (judged once, on the first solve), the
    /// temporal Huber weights and - when asked - the Cauchy weights of the
    /// one before.  Two: one leaves the benefit gate out, and on the
    /// airborne sample's wing - where the lenses see different objects -
    /// that cost the window 0.712 -> 0.531; a third changed no score by more
    /// than 0.0005.
    int irlsIterations = 2;
};

/// How a mesh came to be.
enum class MeshWarpMode : std::uint8_t {
    Solved = 0,    ///< The energy was minimised.
    PriorOnly = 1, ///< Nothing to solve (no data, no line, no previous mesh): the prior, exactly.
};

/// The energy terms at one field (weights as of the final IRLS iteration).
struct MeshWarpEnergy {
    double alignment = 0.0;  ///< s E_align.
    double lines = 0.0;      ///< E_line.
    double shape = 0.0;      ///< E_shape.
    double anchor = 0.0;     ///< E_anchor.
    double temporal = 0.0;   ///< E_temporal (0 without a previous mesh).
    [[nodiscard]] double total() const noexcept { return alignment + lines + shape + anchor + temporal; }
};

/// What a solve measured and decided, for logs and osvtool.
struct MeshWarpReport {
    MeshWarpMode mode = MeshWarpMode::PriorOnly;
    double strength = 0.0;            ///< The structured gate's s (the data term's weight).
    double structuredFraction = 0.0;  ///< The share s was judged on.
    std::uint32_t matches = 0;        ///< Matches with a non-zero weight in the final solve.
    std::uint32_t matchesPrimary = 0; ///< Candidate matches from the primary measurement.
    std::uint32_t matchesRefined = 0; ///< Candidate matches from the refined measurement.
    std::uint32_t matchPairs = 0;     ///< Pixels where both measured (each counted half).
    double matchWeight = 0.0;         ///< Their summed weight (band pixels).
    std::uint32_t lines = 0;          ///< Segments that reached the line term.
    std::uint32_t lineTriples = 0;    ///< Collinearity residuals in the line term.
    std::uint32_t lineTriplesDropped = 0;  ///< Triples outside the mesh or too wide for the solver's band.
    std::uint32_t benefitGatedCells = 0;   ///< Band cells the benefit gate switched fully off.
    int irlsIterations = 0;           ///< Solves run.
    bool temporal = false;            ///< A previous mesh took part.
    MeshWarpEnergy before;            ///< At the prior V0.
    MeshWarpEnergy after;             ///< At the solution.
    /// RMS of the line term's collinearity residuals (band pixels) at the
    /// prior and at the solution - the line term's own view of straightness.
    double lineResidualBeforePx = 0.0;
    double lineResidualAfterPx = 0.0;
    double meanAbsChangeFromPreviousDeg = 0.0;  ///< Mean |V - Vprev| over the band rows (degrees, half disparity).
    double matchMs = 0.0;     ///< Structure, gate counts, match extraction and co-visibility.
    double assembleMs = 0.0;  ///< Building the normal equations (all solves).
    double factorMs = 0.0;    ///< Banded Cholesky factorisations and solves (all IRLS iterations).
    double benefitMs = 0.0;   ///< The benefit gate's residual pass.
    /// The extra solve without the temporal term (MeshWarpInputs::solveAlone
    /// with a previous mesh); 0 when none ran.  Included in factorMs too.
    double aloneMs = 0.0;
    double totalMs = 0.0;     ///< The whole of solveMeshWarp.

    /// One human-readable line: mode, strength, matches, lines, residuals, ms.
    [[nodiscard]] std::string summary() const;
};

/// A solved mesh: the grid the kernel samples, and how it came to be.
struct MeshWarpResult {
    /// The field.  Layout = the mesh (meshWarpLayout); uv in radians, HALF
    /// disparity, master +, slave - (ParallaxWarpGrid).  strength carries the
    /// structured gate's s for the logs (the field already includes it
    /// through the data term's weight - it is NOT to be applied again), the
    /// pixel counts are the flow's, untrustedShare is per mesh column.
    ParallaxWarpGrid grid;
    MeshWarpReport report;
    /// With MeshWarpInputs::solveAlone: the same field solved WITHOUT the
    /// temporal term - this measurement on its own, the field a later solve
    /// takes as ITS previous mesh when the temporal prior must not chain
    /// (the plug-ins' rule: bucket b's prior is bucket b - 1 solved alone, so
    /// every bucket depends on two anchors and never on the order frames were
    /// asked for).  With no previous mesh it is a copy of `grid`.  At the
    /// defaults (two IRLS solves, no Cauchy weight on the matches) it is bit
    /// for bit what solveMeshWarp returns with `previous` null: the first
    /// solve and the benefit gate never see the temporal term, and the last
    /// solve is repeated at the same weights without it.  Empty without
    /// solveAlone.
    std::optional<ParallaxWarpGrid> alone;
};

/// The inputs of one solve.  Pointers are borrowed for the call only.
///
/// TWO MEASUREMENTS.  The data may come from two flows on two renders of the
/// same band: the PRIMARY one (normally the raw bands; it also carries the
/// structured gate's counts) and an optional REFINED one (normally the bands
/// prewarped by the seam table's lift, so the flow measures only the residual
/// the table left).  Measured on the clips: on fine texture (the airborne
/// sample's ground) the flow on raw bands is right to a tenth of a pixel and
/// the residual flow on prewarped bands under-measures it; on a near-field
/// car body the raw flow cannot reach the ~14-pixel offset that the table's
/// 1-D search finds, and the residual flow on top of it can.  Where both
/// measured a pixel each candidate counts half (a photometric pick between
/// the two, measured, changed no score by more than 0.001: where they differ
/// by more than noise only one of them is consistent); elsewhere each counts
/// alone.
struct MeshWarpInputs {
    /// The two lens bands the primary flow was measured on (luma + coverage).
    const LensBands* bands = nullptr;
    /// Bidirectional flow on `bands`, forward = lens 0 -> lens 1.  May be
    /// null: then there are no matches (the field is lines + prior +
    /// previous).
    const BidirFlow* flow = nullptr;
    /// The field `bands` were rendered with (renderLensBands' warp), or null
    /// for raw bands.  The flow then measures the RESIDUAL disparity: every
    /// match's value is this field at the match plus the measured half
    /// disparity.
    const ParallaxWarpGrid* bandWarp = nullptr;
    /// The refined measurement (optional): bands of the primary bands'
    /// geometry, the flow on them and the field they were rendered with.
    const LensBands* bands2 = nullptr;
    const BidirFlow* flow2 = nullptr;
    const ParallaxWarpGrid* bandWarp2 = nullptr;
    /// RAW bands of the primary bands' geometry for every photometric
    /// judgement (the benefit gate, the co-visibility of the anchor), or null
    /// to judge on the primary bands as rendered (relative to `bandWarp`).
    const LensBands* verifyBands = nullptr;
    /// Line segments (detectSeamLines on RAW bands - a band rendered with a
    /// warp shows lines as that warp bent them).  May be null or empty.
    const std::vector<SeamLine>* lines = nullptr;
    /// The prior V0 in the mesh layout (liftSeamTable), or null for zero.
    const ParallaxWarpGrid* prior = nullptr;
    /// The previous solve's mesh (the temporal prior), or null for none.
    const ParallaxWarpGrid* previous = nullptr;
    /// Also return the field solved without the temporal term
    /// (MeshWarpResult::alone): one more factorisation (~4 ms) instead of a
    /// second solve, for a caller that keeps both.
    bool solveAlone = false;
    // ---- diagnostics copied onto the grid ------------------------------------
    FlowBackendKind usedBackend = FlowBackendKind::Classical;
    double bandMs = 0.0;
    double flowMs = 0.0;
};

/// The mesh's layout as a zero field: w = meshCols, h = rows, latMinRad =
/// +reach (row 0), latMaxRad = -reach (row h - 1), uv all zero.
/// InvalidArgument for parameters out of range (see checkMeshWarpParams).
[[nodiscard]] Result<ParallaxWarpGrid> meshWarpLayout(const MeshWarpParams& params);

/// Validate every field solveMeshWarp reads; the message names the first bad one.
[[nodiscard]] Status checkMeshWarpParams(const MeshWarpParams& params);

/// The seam table lifted into the mesh: the 1-D shift as a 2-D field.
///
/// The kernel's 1-D path turns a table value T (degrees, full disparity) at
/// a ray's column into a rotation of each lens's sampling direction by T / 2
/// AWAY from that lens's own axis along the meridian through it, in full up
/// to OSV_SEAM_SHIFT_FULL_DEG from the seam and fading (osvSeamShiftTaper)
/// to nothing at OSV_SEAM_SHIFT_ZERO_DEG.  The master's axis is the +90
/// degree pole of the polar-axis band, so for the master that is a move to
/// LOWER latitude: dLat = -T / 2 x taper, dLon = 0, the slave the negation -
/// the grid's own sign rule.  Each vertex takes the table at its longitude
/// through a triangle (hat) filter one mesh column wide each side, which is
/// the table seen at the mesh's resolution; its row takes the kernel's taper
/// at the row's latitude.
///
/// Approximations, both small and stated: the lens axes of a calibrated rig
/// are not exactly the band's poles (the rotation then acts along a
/// meridian tilted by the axis error, a fraction of a degree), and the
/// kernel interpolates the taper linearly between rows.  Non-finite table
/// entries count as 0; an empty table gives the zero field.
[[nodiscard]] Result<ParallaxWarpGrid> liftSeamTable(const std::vector<float>& tableDeg,
                                                     const MeshWarpParams& params);

/// Solve the mesh (see the file comment for the energy).
///
/// Returns InvalidArgument for malformed parameters, a band without luma or
/// coverage of its size, a flow that does not match the band, or a prior /
/// previous mesh of another layout; never Unsupported - a measurement the
/// structured gate does not trust only weakens the data term.  Non-finite
/// flow vectors, luma or prior cells count as missing, never as values.
/// `pool` (optional) parallelises the per-pixel passes and the two blocks'
/// factorisations; the result is identical with or without it.
[[nodiscard]] Result<MeshWarpResult> solveMeshWarp(const MeshWarpInputs& inputs, const MeshWarpParams& params,
                                                   ThreadPool* pool = nullptr);

/// The flows and the solve on bands already rendered: the bidirectional flow
/// on the RAW bands (the primary measurement, also the verification bands)
/// and, when `prewarped` is given, on those bands too (the refined
/// measurement; `prewarp` is the field they were rendered with), each with
/// params.parallax.backend (falling back like computeFlow), then
/// solveMeshWarp.  `lines`, `prior`, `previous` and `solveAlone` are
/// solveMeshWarp's (MeshWarpInputs).
///
/// A flow that FAILS (an allocation, a backend error) does not fail the
/// field: the solve then runs on what is left - the lines, the prior and the
/// previous mesh - exactly as on a band with nothing to match, so a bucket
/// always gets one field through the one code path.  `flowFailure`
/// (optional) receives the flow's message in that case, empty otherwise.
[[nodiscard]] Result<MeshWarpResult> meshWarpFromBands(const LensBands& raw, const LensBands* prewarped,
                                                       const ParallaxWarpGrid* prewarp,
                                                       const std::vector<SeamLine>* lines,
                                                       const ParallaxWarpGrid* prior,
                                                       const ParallaxWarpGrid* previous,
                                                       const MeshWarpParams& params, ThreadPool* pool,
                                                       double bandMs = 0.0, bool solveAlone = false,
                                                       std::string* flowFailure = nullptr);

/// What buildMeshWarp did on the side, for a caller that reports it.
struct MeshWarpBuildInfo {
    SeamLineSet lines;               ///< The segments found on the raw bands.
    bool bandsPrewarped = false;     ///< The flow ran on bands rendered with the table's lift.
    double rawBandMs = 0.0;          ///< Rendering the raw bands (lines, and the flow when not prewarped).
    double warpedBandMs = 0.0;       ///< Rendering the prewarped bands (0 when not prewarped).
    /// Why the flow failed when it did (the field was then solved without
    /// matches, see meshWarpFromBands); empty otherwise.
    std::string flowFailure;
};

/// Everything from a decoded frame pair: render the raw bands, find the lines
/// on them, lift `tableDeg` (optional) into the prior, and - when `prewarp`
/// and a table is given - render the bands once more with the prior as their
/// warp, so a second flow measures only what the table left (coarse to fine:
/// the 1-D search finds large near-field offsets the flow's pyramid cannot
/// reach on a 68-row band).  Then the flows and the solve
/// (meshWarpFromBands).  `previous` is the temporal prior and `solveAlone`
/// asks for MeshWarpResult::alone as well.  `info` (optional) receives the
/// lines and the timings.
[[nodiscard]] Result<MeshWarpResult> buildMeshWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                                   const geom::BlendParams& blend, const MeshWarpParams& params,
                                                   const std::vector<float>* tableDeg, bool prewarp,
                                                   const ParallaxWarpGrid* previous, ThreadPool& pool,
                                                   MeshWarpBuildInfo* info = nullptr, bool solveAlone = false);

/// A borrowed view of a grid for renderLensBands / overlapNcc.
[[nodiscard]] WarpGridView warpGridView(const ParallaxWarpGrid& grid) noexcept;

// ===========================================================================
//  Line straightness: the metric the eye uses
// ===========================================================================

/// How straight the detected segments come out of a correction.
struct LineStraightness {
    std::uint32_t lines = 0;        ///< Segments scored (>= 3 samples each).
    std::uint32_t samples = 0;      ///< Samples scored over all segments.
    double rmsPx = 0.0;             ///< RMS perpendicular residual over all samples (band pixels).
    double maxPx = 0.0;             ///< Largest single residual.
    double meanLineRmsPx = 0.0;     ///< Mean of the per-segment RMS.
    double maxLineRmsPx = 0.0;      ///< Worst segment's RMS.
    std::vector<float> perLineRmsPx; ///< Per segment, in input order (NaN where not scored).
};

/// Warp every segment's samples by a correction and measure how far they
/// leave a straight line.
///
/// Each segment (detected on a RAW band, so straight there) is sampled every
/// `spacingPx` band pixels.  A raw sample q of lens L is shown by the render
/// at the output position y with y + sgn V(y) = q (sgn = +1 for the master,
/// lens 1, which samples at y + V; -1 for the slave), solved by fixed-point
/// iteration.  V is the sum of `grid` (osvWarpSample's bilinear fetch,
/// optional) and the 1-D `tableDeg` (optional, as the kernel's 1-D path
/// moves the ray: the table's column by osvSeamColumn's rule, T / 2 along
/// the meridian, tapered by osvSeamShiftTaper).  A straight line is fitted
/// to each segment's warped samples by total least squares (Jia et al.,
/// CVPR 2021, eq. 8) and the perpendicular residuals reported, in band
/// pixels of a `bandEquirectW`-wide polar map.  With no correction every
/// residual is 0: the samples lie on the detected segment.
[[nodiscard]] Result<LineStraightness> measureLineStraightness(const std::vector<SeamLine>& lines,
                                                               std::uint32_t bandEquirectW,
                                                               const ParallaxWarpGrid* grid,
                                                               const std::vector<float>* tableDeg,
                                                               double spacingPx = 4.0);

/// Mean |a - b| over the cells of two grids of one layout whose latitude
/// lies within +/- maxAbsLatDeg (the analysed band), in DEGREES of the
/// stored half disparity, both components (the 2-D norm per cell).
/// InvalidArgument for mismatched or malformed grids.
[[nodiscard]] Result<double> meanAbsGridChangeDeg(const ParallaxWarpGrid& a, const ParallaxWarpGrid& b,
                                                  double maxAbsLatDeg);

}  // namespace osv::render
