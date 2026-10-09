// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ClipSteady.h - one stitch correction per CLIP instead of one per moment:
// the steady parallax grid, seam-shift table and carved seam, the Auto rule
// that decides whether a clip wants them, and the per-clip lens rotation
// measurement (render/LensAlign.h) that runs on the same sample frames.
//
// ===========================================================================
//  WHY
// ===========================================================================
// The importer measures the seam corrections once per bucket of 8 frames and
// glides between buckets (ParallaxWarp.h, the temporal schedule).  On a rigid
// mount - a camera on a wing, a helmet, a car - that is measuring the same
// geometry over and over, and what changes between measurements is noise:
//
//   * the parallax grid glides by up to 2 px per frame at 6K around the
//     sample clip's nacelle (p99 0.88 px; 24 of 64 frames above 1 px) - the
//     "slight movement at the seam" users see (docs/research/AI_STITCHING.md
//     section 3.6);
//   * the carved seam line moves by up to 0.036 deg per frame;
//   * the 1-D seam-shift table steps at every bucket edge.
//
// One correction per clip removes all three motions by construction, and on
// the sample it costs no alignment (ground NCC 0.9036 against 0.9045, wing
// 0.313 against 0.314, research harness).  Handheld footage with near objects
// moving past the seam is the opposite case: there the per-moment correction
// follows real parallax, which is why the choice exists (Auto, below).
//
// ===========================================================================
//  DETERMINISM
// ===========================================================================
// The clip correction is built from a FIXED set of sample frames chosen from
// the clip alone (clipSampleFrames), never from "whatever Premiere happened to
// ask for first", so every frame of a clip renders the same whatever was
// rendered before it, in every instance, on every path.
//
//   grid        ONE correction field (MeshWarp.h): every sample's own mesh -
//               the content-preserving mesh warp solved on the sample alone,
//               its seam table lifted in as the prior - then the per-cell,
//               per-component median of those meshes (a cell the benefit
//               gate held at the prior in most samples stays there), then
//               ONE more solve with no flow at all: the median as the prior
//               and the line segments of EVERY sample in the line term.  A
//               per-cell median of fields that each keep their lines
//               straight need not keep them straight itself (neighbouring
//               cells can take their medians from different samples); the
//               last solve removes exactly the bend along each detected line
//               and nothing else (the shape term spreads it over ~2 mesh
//               columns).  Median, not one joint solve over all samples'
//               matches: a car that passes the seam in one sample must not
//               pull the clip field (a joint least squares averages it in,
//               the median ignores it), while the lines - which are where
//               the eye judges the field - are all honoured.  There is no
//               second field: the 1-D table and the per-column guard of
//               0.5.1 are gone from the clip correction;
//   seam table  the per-column median of the samples' tables, rendered only
//               where there is no clip field (Parallax Grid off, or too few
//               samples solved);
//   seam        each sample's bands carved through the CLIP correction with
//               no temporal prior, then the per-column median of the carves
//               (the median of paths that each move at most N rows per column
//               moves at most N rows per column, so it is still a valid seam).
//
// ===========================================================================
//  AUTO
// ===========================================================================
// Holding the correction still is right exactly when the per-moment
// corrections differ from each other by noise.  "Noise" is judged by effect:
// every sample's own bands are seen with NO correction, through its OWN
// correction and through the CLIP correction (correctBandsForSeam, the
// kernel's sampling), and the overlap NCC is compared per longitude sector.
//
//   * Only sectors where the sample's own correction really ALIGNS something
//     are judged: its NCC reaches minOwnNcc and gains at least minGain over
//     no correction.  That is what a near object both lenses see as one
//     surface looks like (a person, a railing, a wall).  Where even the
//     own correction leaves the lenses disagreeing - the sample's specular,
//     see-through nacelle, which the two lenses see from different sides -
//     its frame-to-frame changes chase reflections, not geometry, and holding
//     them still costs nothing real (AI_STITCHING.md sections 2.3 and 3.3).
//   * A judged sector FAILS when the clip correction loses more than maxLoss
//     NCC against the own one and keeps less than minKeep of the own
//     correction's gain.  A near object that moved past the seam fails (the
//     median of the other samples has no parallax where it now is: the gain
//     kept is ~0); a static one does not (the median carries it).
//   * ... and only where the two corrections really DIFFER: the sector's
//     mean |own - clip| displacement (correctionDisplacement, the full
//     disparity each would render) is at least minFieldDiffDeg.  NCC is an
//     in-sample score: a sample's own mesh is fitted to that frame's flow
//     matches and then scored on the same frame, so wherever the flow
//     locked on to something that is not a surface - the streaks of a
//     spinning propeller, a reflection sliding over a chrome nacelle - the
//     own correction "wins" by a wide NCC margin while moving the picture
//     by no more than the clip correction does.  Such a sector is counted
//     as agreed, not failed: holding the clip correction there changes
//     nothing a viewer could see.  A near object that moved fails as
//     before - its own field differs from the median by its parallax.
//   * Steady when at most maxFailedFraction (5 %) of the judged sectors
//     fail and none of them loses more than maxFailedLoss (0.10 NCC).
//
// On the sample clip (rotation folded, the aligned gate) the worst judged
// sector keeps 74 % of its own gain and none fails, so Auto holds it still;
// the per-sector loss there is at most 0.053 NCC, at the wing root.
//
// The tolerance replaced "steady only when NOTHING fails": a long drive past
// traffic always has a sample with a passing car in some sector, and one
// such sector vetoed the clip correction for the whole clip.  A failure is
// tolerated only while it is rare (a transient) AND small - no larger than
// the losses the keep test already lets through on the sample clip (up to
// 0.093 NCC at the wing root, measured with the 8K focal fix in force).  A
// near object that really moves still fails in many samples and by far more
// (a user's car-mounted 8K clips: 6 of 77 and 11 of 105 judged sectors, up
// to 0.15 and 0.66 NCC lost), so those clips keep following the scene.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/LensAlign.h"
#include "osv/render/LensShading.h"
#include "osv/render/MeshWarp.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Sample frames
// ===========================================================================

/// Sample frames of the clip correction: nine, as many as the sample clip
/// has buckets (the research's median of nine bucket grids).
inline constexpr std::uint32_t kClipSteadySamples = 9;
/// Sample frames of the lens rotation fit: three, at 10 / 50 / 90 % of the
/// clip (AI_STITCHING.md 5.2), combined by their median.
inline constexpr std::uint32_t kLensRotationSamples = 3;
/// A clip at most this many frames long is sampled at its exact targets and
/// decoded straight through (4 s at 60 fps: ~240 hardware decodes, ~2 s);
/// a longer one is sampled at its sync frames, one cheap decode each.
inline constexpr std::uint32_t kClipSequentialFrames = 240;

/// The frames a per-clip analysis measures: `samples` targets spread evenly
/// from `firstFraction` to `lastFraction` of the clip, ascending, duplicates
/// removed.  A clip longer than `sequentialFrames` with a sync table snaps
/// every target to the nearest sync frame (earlier on a tie), so each sample
/// decodes one intra frame instead of walking a GOP.  Depends on nothing but
/// its arguments - the determinism the header promises.  Empty for an empty
/// clip; one frame (the middle target) for samples == 1.
[[nodiscard]] std::vector<std::uint32_t> clipSampleFrames(std::uint32_t frameCount,
                                                          const std::vector<std::uint32_t>& syncFrames,
                                                          std::uint32_t samples, double firstFraction = 0.0,
                                                          double lastFraction = 1.0,
                                                          std::uint32_t sequentialFrames = kClipSequentialFrames);

/// Decoded frames for the per-clip analyses, asked for in ascending order
/// (a substitute for a sample the decoder refused may step back once, see
/// clipSampleAlternates).
using ClipFrameSource = std::function<Result<video::FramePair>(std::uint32_t frame)>;

// ===========================================================================
//  A sample frame the decoder refuses
// ===========================================================================
// One undecodable sample used to abort the whole per-clip measurement: a
// user's night driving clip (variable frame rate, a few dropped frames) lost
// its clip correction to ONE of nine samples, so Steady silently rendered
// per moment.  A sample that cannot be decoded is now replaced by the nearest
// frame that can be (the neighbouring sync frame on a long clip), or skipped,
// and the measurement fails only when fewer than a minimum decode at all.
// Which frames were used, and why, travels with the result (`frames`,
// `sampleNotes`), so the log and the caches say what was measured.

/// Fewest of the clip correction's nine samples that must decode: five keep
/// the per-cell median a majority of the planned samples.
inline constexpr std::uint32_t kClipSteadyMinSamples = 5;
/// Fewest of the lens rotation's three samples that must decode: two, the
/// least combineLensRotations can call an agreement.
inline constexpr std::uint32_t kLensRotationMinSamples = 2;
/// Substitutes tried for one undecodable sample before it is skipped.
inline constexpr std::uint32_t kClipSampleAlternates = 2;

/// Frames to try, in order, in place of sample `frame` when it cannot be
/// decoded (empty: skip it).  Built by clipSampleAlternates.
using ClipFrameAlternates = std::function<std::vector<std::uint32_t>(std::uint32_t frame)>;

/// The substitutes for sample `frame` of the sample set `samples` (as
/// clipSampleFrames returned it): at most `maxAlternates` frames strictly
/// between the neighbouring samples, so the set stays ascending and
/// distinct, nearest first and the later one first on a tie (a forward
/// decoder keeps going forward).  On a clip longer than `sequentialFrames`
/// with a sync table they are the neighbouring SYNC frames (one cheap decode
/// each, like the samples themselves); otherwise the neighbouring frames.
/// Depends on nothing but its arguments.  Empty for a frame outside the
/// clip, a frame that is not one of `samples`, or maxAlternates == 0.
[[nodiscard]] std::vector<std::uint32_t> clipSampleAlternates(std::uint32_t frame, std::uint32_t frameCount,
                                                              const std::vector<std::uint32_t>& syncFrames,
                                                              const std::vector<std::uint32_t>& samples,
                                                              std::uint32_t maxAlternates = kClipSampleAlternates,
                                                              std::uint32_t sequentialFrames = kClipSequentialFrames);

/// Polled between samples: true abandons the measurement (Cancelled).
using ClipCancel = std::function<bool()>;

// ===========================================================================
//  The lens rotation (render/LensAlign.h) over several frames
// ===========================================================================

/// What measureLensRotation found.  A refusal is a RESULT (accepted false,
/// `reason` says why), not an error: a clip of open sky has no rotation to
/// find, and that verdict is as cacheable as a fit.
struct LensRotationMeasurement {
    std::vector<std::uint32_t> frames;      ///< Frames measured (substitutes included, skipped samples left out).
    /// One line per sample that could not be decoded: what replaced it, or
    /// that it was skipped.  Empty when every planned frame decoded.
    std::vector<std::string> sampleNotes;
    std::vector<LensRotationFit> perFrame;  ///< The frames whose own fit was accepted.
    std::vector<std::string> refusals;      ///< Why each other frame's fit was refused.
    bool accepted = false;                  ///< `fit` is the clip's rotation.
    LensRotationFit fit;                    ///< The combined fit (valid when accepted).
    std::string reason;                     ///< Why it was refused (empty when accepted).
    double decodeMs = 0.0;                  ///< Time in the frame source.
    double analysisMs = 0.0;                ///< Bands + flow + fits.
};

/// Measure the rotation on `frames`: each frame's bands through `rig` (the
/// calibration, uncorrected), the flow and grid as the importer builds them
/// (`parallax`), the raw cells fitted (fitLensRotation), and the accepted
/// fits combined (combineLensRotations).  A frame that cannot be decoded is
/// replaced by the first of `alternates(frame)` that can, else skipped (see
/// "A sample frame the decoder refuses"); errors only when fewer than
/// min(`minSamples`, frames.size()) frames decode, for a band render failure,
/// malformed input or cancellation.
[[nodiscard]] Result<LensRotationMeasurement> measureLensRotation(
    const geom::LensRig& rig, const geom::BlendParams& blend, const std::vector<std::uint32_t>& frames,
    const ClipFrameSource& source, const ParallaxWarpParams& parallax, const LensRotationParams& params,
    ThreadPool& pool, const ClipCancel& cancelled = {}, const ClipFrameAlternates& alternates = {},
    std::uint32_t minSamples = kLensRotationMinSamples);

// ===========================================================================
//  Medians
// ===========================================================================

/// The per-cell, per-component median of `grids` (null entries are skipped).
/// Every grid must share one layout.  The diagnostics describe the median
/// (disparities over the whole grid) and the summed cost and pixel counts of
/// the samples; the strength is the median of the samples' strengths (a
/// non-finite one counts as 0), and the untrusted share of each column
/// (ParallaxWarpGrid::untrustedShare) the median of the samples' - empty
/// unless every sample carries one per column.  InvalidArgument for no grid
/// or mismatched layouts.
[[nodiscard]] Result<ParallaxWarpGrid> clipParallaxGrid(const std::vector<const ParallaxWarpGrid*>& grids);

/// The per-column median of 1-D seam tables (null entries skipped; equal
/// lengths required).
[[nodiscard]] Result<std::vector<float>> clipSeamTable(const std::vector<const std::vector<float>*>& tables);

/// The per-column median of carved seams: latitude, feather half width and
/// the near weight separately (null entries skipped; equal column counts
/// required).  The near weight is kept only when every seam has one.
[[nodiscard]] Result<BlendSeam> clipBlendSeam(const std::vector<const BlendSeam*>& seams);

// ===========================================================================
//  Auto
// ===========================================================================

/// Tuning of the Auto rule (see the header).
struct SteadyDecisionParams {
    /// Longitude sectors the ring is judged in: 32 is 11.25 deg each, about
    /// the size of a person at 2 m or of the sample's nacelle crossing.
    std::uint32_t sectors = 32;
    /// Rows judged: |latitude| up to this (degrees), the co-visible heart of
    /// the overlap (the research metric's +-4 deg).
    double latHalfDeg = 4.0;
    /// A sector counts only with this many co-visible pixels ...
    std::uint32_t minPixels = 400;
    /// ... and this much texture (luma standard deviation in BOTH lenses,
    /// code values 0..1): the NCC of a featureless sector is noise.
    double minStd = 0.006;
    /// A sector is judged only where the sample's own correction reaches
    /// this NCC (it aligns one surface both lenses see) ...
    double minOwnNcc = 0.7;
    /// ... and gains at least this much NCC over no correction (there is
    /// parallax for the correction to follow).
    double minGain = 0.05;
    /// A judged sector fails when the clip correction loses more than this
    /// NCC against the own one ...
    double maxLoss = 0.02;
    /// ... AND keeps less than this fraction of the own correction's gain.
    /// On the sample the worst judged sector keeps 0.59-0.86 of it across
    /// gate settings (0.74 with the rotation folded); a near object that
    /// moved keeps ~0.
    double minKeep = 0.4;
    /// The verdict's tolerance (see "Steady when" in the header): at most
    /// this fraction of the judged sectors may fail ...
    double maxFailedFraction = 0.05;
    /// ... and none of those failures may lose more than this NCC: the
    /// largest loss the keep test already passes on the sample clip (0.093
    /// at frame 32, the wing root, keeping 73 % of its gain), rounded up.
    double maxFailedLoss = 0.10;
    /// A sector can fail only where the sample's own correction and the
    /// clip correction differ by at least this much (mean full disparity
    /// over the sector's judged pixels, degrees; see the header).  Two
    /// mesh fields solved on different frames of a static scene differ by
    /// their measurement noise - about a band pixel, 0.18 deg, the scale
    /// the mesh's temporal term names (MeshWarp.h) - and by what the flow
    /// made of texture that is not a surface: on the sample clip the
    /// propeller sectors reach 0.32 deg (frame 32, lon +152).  Half a
    /// degree sits above both and below a near object's parallax (0.7 deg
    /// at 2 m, 1.4 deg at 1 m), which is what a moving one fails by.
    double minFieldDiffDeg = 0.5;
};

/// One sample as the Auto rule sees it: its uncorrected bands (as
/// measureParallaxBands renders them) and the correction it would render
/// with on its own (its mesh field, else its table, else none).
struct SteadySample {
    std::uint32_t frame = 0;
    const LensBands* bands = nullptr;
    SeamCorrection own;
};

/// One judged (sample, sector) pair: the overlap NCC with no correction,
/// the sample's own and the clip correction.
struct SteadySectorScore {
    std::uint32_t frame = 0;   ///< Sample frame.
    std::uint32_t sector = 0;  ///< Longitude sector (0 = -180 deg).
    double none = 0.0;         ///< NCC with no correction at all.
    double own = 0.0;          ///< NCC through the sample's own correction.
    double clip = 0.0;         ///< NCC through the clip correction.
    /// Mean |own - clip| displacement over the sector's judged pixels (full
    /// disparity, degrees): how differently the two corrections MOVE the
    /// picture there, whatever the NCC says.
    double fieldDiffDeg = 0.0;
};

/// The Auto rule's verdict.
struct SteadyDecision {
    bool steady = true;             ///< Hold the correction still.
    std::uint32_t textured = 0;     ///< (sample, sector) pairs with the texture to score at all.
    std::uint32_t judged = 0;       ///< ... of which the own correction aligned something (judged).
    std::uint32_t failed = 0;       ///< ... of which the clip correction lost it (see the header).
    /// ... and of which the clip correction lost the NCC but moves the
    /// picture the same way (the fields differ by less than
    /// minFieldDiffDeg): the own score was texture, not geometry.
    std::uint32_t agreed = 0;
    double worstFailedLoss = 0.0;   ///< The largest NCC loss of a failed pair (0 when none failed).
    double worstFieldDiffDeg = 0.0; ///< The field difference of the worst-keep pair (below).
    double meanLoss = 0.0;          ///< Mean NCC loss (own - clip) over the textured pairs.
    /// The judged pair that kept the least of its own correction's gain
    /// (1 when nothing was judged).
    double worstKeep = 1.0;
    double worstLoss = 0.0;         ///< ... its NCC loss (own - clip).
    std::uint32_t worstFrame = 0;   ///< ... its sample frame
    double worstLonDeg = 0.0;       ///< ... the centre longitude of its sector (polar-axis layout, degrees)
    double worstNoneNcc = 0.0;      ///< ... its NCC with no correction,
    double worstOwnNcc = 0.0;       ///< ... through its own correction
    double worstClipNcc = 0.0;      ///< ... and through the clip correction.
    /// Every textured pair, for reports (osvtool seam --steady) and tests.
    std::vector<SteadySectorScore> scores;
};

/// Judge the clip correction against every sample's own (see the header).
/// With nothing textured to judge, holding the correction still cannot
/// cost anything measurable, so the verdict is steady.  InvalidArgument for
/// malformed input.
[[nodiscard]] Result<SteadyDecision> decideSteady(const std::vector<SteadySample>& samples,
                                                  const SeamCorrection& clip, const SteadyDecisionParams& params,
                                                  ThreadPool* pool = nullptr);

/// The verdict's rule on its own (pure): steady when nothing failed, or when
/// the failures are at most floor(maxFailedFraction x judged) AND none lost
/// more than maxFailedLoss NCC.  decideSteady sets SteadyDecision::steady
/// with it; exposed so the tolerance is pinned by tests without footage.
[[nodiscard]] bool steadyWithinTolerance(std::uint32_t judged, std::uint32_t failed, double worstFailedLoss,
                                         const SteadyDecisionParams& params) noexcept;

/// "steady: worst sector loses 0.012 NCC (frame 32, 158 deg), 180 judged" -
/// the log line's words for a verdict.
[[nodiscard]] std::string describeSteadyDecision(const SteadyDecision& decision);

// ===========================================================================
//  The clip correction
// ===========================================================================

/// What the clip correction is built from and with.
struct ClipSteadyParams {
    /// The per-sample measurement block (band, flow backend, structured and
    /// benefit gates), exactly as the importer measures a bucket.  It is the
    /// mesh's own measurement block: measureClipSteady copies it into
    /// `mesh.parallax`, so the two can never disagree.
    ParallaxWarpParams parallax;
    /// The mesh solve (MeshWarp.h) of every sample and of the clip field;
    /// its `parallax` member is replaced by `parallax` above.
    MeshWarpParams mesh;
    bool parallaxOn = true;       ///< Solve the mesh at all.
    /// The seam: every sample's table (the mesh's prior and the bands its
    /// refined flow is measured on, and the correction where there is no
    /// clip field) and the carve.  Off leaves all of it out (the importer's
    /// Seam Search off: the meshes are then solved without a prior).
    bool seamOn = true;
    SeamSearchParams seamSearch;  ///< The per-sample seam-shift table.
    SeamCarveParams carve;        ///< The carve (the importer's widths; its penalty is not used).
    /// Steer each sample's carve off the lenses' usable rims, as the
    /// importer's per-bucket carve does with its photometric field.
    bool rimCost = false;
    PhotoSeamParams photo;        ///< The field the rim comes from (rimCost only).
    /// Measure that field on shading-corrected lenses, as the importer's
    /// per-bucket field is (each sample's own model, scaled by
    /// `shading.strength`); rimCost only.
    bool shadingOn = false;
    LensShadingParams shading;
    SteadyDecisionParams decision;
    /// Solved sample meshes needed for a clip field: fewer is a clip whose
    /// samples mostly failed to measure, which the seam table then serves.
    /// (A mesh never refuses content - open sky gives the table's lift - so
    /// this counts measurements that could not be made at all.)
    std::uint32_t minGrids = 3;
    /// Sample frames that must decode (substitutes count); fewer is an
    /// error.  Capped at the number of planned frames.
    std::uint32_t minSamples = kClipSteadyMinSamples;
};

/// The clip correction and how it was measured.
struct ClipSteady {
    /// Sample frames measured: the planned ones, a substitute where one could
    /// not be decoded, a skipped one left out.
    std::vector<std::uint32_t> frames;
    /// One line per planned sample that could not be decoded (what replaced
    /// it, or that it was skipped); empty when every one decoded.
    std::vector<std::string> sampleNotes;
    /// The clip field as it renders - the WHOLE correction, the kernel's 1-D
    /// seam shift carrying nothing under it; null when not measured.  The
    /// per-cell median of the samples' meshes, then kept line-straight over
    /// every sample's segments (see the header).  Its strength is the median
    /// of the samples' structured-gate strengths, for the logs (each mesh
    /// already carries its own as the weight of its data).
    std::shared_ptr<const ParallaxWarpGrid> grid;
    std::uint32_t acceptedGrids = 0;  ///< Samples whose own mesh was solved.
    /// The line pass over the median: segments of all samples that reached
    /// the line term, and the RMS of their collinearity residuals (band
    /// pixels) before (the median itself) and after (the clip field).
    std::uint32_t lines = 0;
    double lineResidualBeforePx = 0.0;
    double lineResidualAfterPx = 0.0;
    /// Clip seam table where there is no clip field (Parallax Grid off, or
    /// fewer than minGrids samples solved): the per-column median of the
    /// samples' tables; null when none was needed or found.
    std::shared_ptr<const std::vector<float>> seamTable;
    std::shared_ptr<const BlendSeam> seam;               ///< Clip carved seam; null when seamOn is off or it failed.
    SteadyDecision decision;                             ///< The Auto rule's verdict.
    double decodeMs = 0.0;                               ///< Time in the frame source.
    double measureMs = 0.0;                              ///< Bands, flow, tables, rims.
    double finishMs = 0.0;                               ///< Medians, carves, the verdict.
};

/// Progress: each sample's own mesh field (null = not measurable) as soon as
/// it exists, for stand-ins while the rest is measured.  Called on the
/// measuring thread.
using ClipSampleGridFn = std::function<void(std::uint32_t frame, std::shared_ptr<const ParallaxWarpGrid> grid)>;

/// Measure the clip correction on `frames` through `rig` (which already
/// carries any lens rotation), in one pass over the frames.  A frame that
/// cannot be decoded is replaced by the first of `alternates(frame)` that
/// can, else skipped (see "A sample frame the decoder refuses").  Errors only
/// when fewer than min(params.minSamples, frames.size()) frames decode, for a
/// band render failure, malformed input or cancellation; an analysis the
/// content refuses (no grid, no table) is a result with that piece null.
[[nodiscard]] Result<ClipSteady> measureClipSteady(const geom::LensRig& rig, const geom::BlendParams& blend,
                                                   const std::vector<std::uint32_t>& frames,
                                                   const ClipFrameSource& source, const ClipSteadyParams& params,
                                                   ThreadPool& pool, const ClipSampleGridFn& onSampleGrid = {},
                                                   const ClipCancel& cancelled = {},
                                                   const ClipFrameAlternates& alternates = {});

}  // namespace osv::render
