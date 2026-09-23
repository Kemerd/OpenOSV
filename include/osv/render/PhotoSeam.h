// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PhotoSeam.h - the photometric seam fix for the sky band between the two
// lenses (docs/research/NEURAL_STITCHING.md, section 8).
//
// ---------------------------------------------------------------------------
//  What is wrong with the sky seam
// ---------------------------------------------------------------------------
// Measured on the sample clip, the visible band in the sky along the seam is
// photometric, not geometric:
//
//   1. lens 0's usable rim ends near 92.8 deg in the open-sky longitudes, not
//      at the calibrated 97.59 deg, yet the production feather still gives it
//      0.67 of the blend at 95 deg - a dark line along the overlap edge;
//   2. the two lenses disagree by a gain that changes with direction (0.71
//      stop in the sky, 0.31 on the ground, mostly R and G), which one global
//      gain per lens can only compromise on;
//   3. a tone step where lens 0's occlusion mask ends, which disappears once
//      the first two are fixed.
//
// ---------------------------------------------------------------------------
//  Stage 1 (this part): a render-only blend inset
// ---------------------------------------------------------------------------
// The kernel's FOV feather comes from geom::BlendParams.  Narrowing the RENDER
// blend so every lens's weight reaches zero a few degrees inside the
// calibrated rim removes most of the dark line without touching the kernel.
// It must stay render-only: the analyses (parallax, seam search, gain) keep
// the full calibrated FOV, because shrinking the overlap they measure in
// costs parallax quality (NCC 0.92 -> 0.75-0.86 on the ground, measured).
//
// ---------------------------------------------------------------------------
//  Stage 2: the photometric seam field
// ---------------------------------------------------------------------------
// A fixed inset is one clip's compromise.  Stage 2 measures, per bucket of
// frames, what the overlap itself says:
//
//   * a per-longitude USABLE RIM for each lens - the angle where the lens's
//     own ratio to the other lens departs 0.25 stop from its core - and the
//     kernel's FOV feather ends there instead of at thetaMax;
//   * a 2-D LOG-GAIN FIELD log2(master / slave) over the overlap, from
//     TRUSTED pixels only (co-valid, flat, inside both usable rims), split
//     half and half between the lenses and decayed to zero over 20 deg
//     beyond the overlap (chroma twice as fast).
//
// It is what DJI's learned colour model outputs (a low-resolution gain field
// for the seam strip), estimated directly from the overlap statistics.  The
// single most important detail: the gain is NEVER estimated from all
// co-visible pixels - a lens's dark rim then reads as "the other lens is too
// bright" and the correction paints a wide dark band (section 1.4).
//
// Time: one measurement per bucket (render::parallaxBucket), an EMA against
// the previous bucket, a cross-fade from the previous bucket's field inside
// each bucket (exactly like the parallax grid), and a per-clip running
// median of the rim (the rim is a property of the lens, not of the frame).
//
// The same header also carries the section 1.4 sky metrics, implemented
// exactly as research/neural/photo_grid_experiments.py:metrics(), so the
// quality claims are measured in C++ on bands rendered through the real
// kernel rather than in a Python re-blend.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/RenderJob.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Stage 1: the render-only seam edge inset
// ===========================================================================

/// Default inset of the render blend below the calibrated half FOV, degrees.
/// 97.59 - 2.6 = 94.99 deg: the research's "blend ends at 95 deg" fallback,
/// where lens 0 is still within one stop of its core on the sample clip.
inline constexpr double kDefaultSeamInsetDeg = 2.6;

/// Largest inset the UI offers, degrees.  Beyond ~6 deg the two render
/// feathers no longer overlap on a 195 deg lens pair (2 x (97.59 - 6) = 183
/// deg of coverage leaves only +-1.6 deg of blend), so a larger value would
/// trade the dark line for a hard seam.
inline constexpr double kMaxSeamInsetDeg = 6.0;

/// FOV feather width of an inset render blend, degrees.  Narrower than the
/// analysis feather (4 deg) so the blend still ends near the usable rim.
inline constexpr double kSeamInsetFeatherDeg = 3.0;

/// The render-only blend for a seam edge inset.
///
/// @param analysis    the blend the analyses use (the calibrated FOV and its
///                    feather; ImporterInstance::m_blend, osvtool's
///                    --lens-fov / --feather).
/// @param insetDeg    how far inside the calibrated half FOV the render
///                    weight reaches zero, degrees, clamped to
///                    [0, kMaxSeamInsetDeg].
/// @param featherDeg  FOV feather of the inset blend, degrees (> 0).
/// @return `analysis` UNCHANGED - every field bit for bit - when the inset is
///         zero, non-finite or negative, so "no inset" renders exactly as
///         before this function existed; otherwise `analysis` with
///         lensFovDeg reduced by 2 x inset and featherDeg replaced.
[[nodiscard]] geom::BlendParams insetRenderBlend(const geom::BlendParams& analysis, double insetDeg,
                                                 double featherDeg = kSeamInsetFeatherDeg) noexcept;

// ===========================================================================
//  Rendering band rows through the real kernel
// ===========================================================================

/// Shade rows [row0, row1) of `job` on the CPU into tightly packed RGBA
/// floats, with the job's seam table, warp grid and everything else it
/// carries - the same per-pixel call CpuRenderer makes, at the same ABSOLUTE
/// row index, so each row equals the matching row of a full render.
///
/// Host frames only (a device job is refused with InvalidArgument).  Used by
/// the quality measurements, which need a ±30 deg band of a full-resolution
/// polar map without paying for the rest of it.
[[nodiscard]] Result<std::vector<float>> shadeJobRows(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                                      ThreadPool& pool);

// ===========================================================================
//  Section 1.4 sky metrics
// ===========================================================================

/// RGBA bands of a polar-axis equirect (lens axes at the poles, the seam on
/// the equator), rendered through the kernel in scene-linear light: the
/// blended picture and each lens alone (with whatever correction the variant
/// under test applies to it).
struct MetricBands {
    std::uint32_t w = 0;          ///< Columns (the whole 360 deg of longitude).
    std::uint32_t h = 0;          ///< Rows.
    std::uint32_t rowOffset = 0;  ///< First map row of the band.
    std::uint32_t mapH = 0;       ///< Height of the whole polar map (w / 2).
    std::vector<float> blend;     ///< Blended RGBA, w * h * 4.
    std::vector<float> lens[2];   ///< Each lens alone, RGBA, w * h * 4 (alpha = its weight).

    /// Latitude of band row `r`, degrees (+ towards lens 1, the master).
    [[nodiscard]] double latDeg(std::uint32_t r) const noexcept;
};

/// The four seam-visibility numbers of NEURAL_STITCHING.md table 1.4, in
/// millistops (lower is better).
struct SkySeamMetrics {
    double line = 0.0;   ///< RMS(profile - 1.5 deg blur), |lat| <= 10: thin lines.
    double band = 0.0;   ///< RMS(0.5 deg blur - 4 deg blur), |lat| <= 10: band-scale bumps.
    double broad = 0.0;  ///< RMS(2 deg blur - 10 deg blur), |lat| <= 25: soft bands, decay ramps.
    double dE = 0.0;     ///< RMS log2 chroma (R/G, B/G) difference of the two lenses on trusted pixels.
    std::uint64_t trustedPixels = 0;  ///< Pixels the dE term was averaged over.
};

/// Geometry of the metric bands (see renderMetricBands).
struct MetricBandRequest {
    std::uint32_t mapW = 2048;  ///< Polar map width (columns per 360 deg).
    double halfDeg = 30.0;      ///< Half height of the band around the seam, degrees.
};

/// Render the metric bands of one variant: a polar-axis map `request.mapW`
/// wide, rows within `request.halfDeg` of the seam, blended and per lens, all
/// through the shared kernel on the CPU.
///
/// `builder` is copied and completed here: the polar equirect map, a
/// scene-linear colour block and alpha coverage are forced; everything else -
/// rig, blend, gains, seam table, warp grid, photometric field - is the
/// caller's variant.  Host frames only.
[[nodiscard]] Result<MetricBands> renderMetricBands(const RenderParamsBuilder& builder,
                                                    const video::FramePair& frames,
                                                    const MetricBandRequest& request, ThreadPool& pool);

/// Compute the metrics over longitude columns [colBegin, colEnd) of the band
/// (fractions of the width, 0.20 / 0.44 = the sample's open sky).
///
/// `trust` (w * h, nonzero = trusted) selects the pixels the colour term is
/// averaged over; the research uses "co-valid and inside both usable rims".
/// Exactly photo_grid_experiments.py:metrics(): log2 luma (BT.2020 weights)
/// averaged over blocks of w/128 columns, Gaussian blurs along latitude only
/// with a 4-sigma radius and replicated borders.
///
/// Errors: InvalidArgument for mismatched sizes, an empty column range or a
/// band without the +-10 deg rows the thin-line terms need.
[[nodiscard]] Result<SkySeamMetrics> skySeamMetrics(const MetricBands& bands, const std::vector<std::uint8_t>& trust,
                                                    double colBeginFrac, double colEndFrac);

/// The simplest trust mask for skySeamMetrics: both lenses' alpha >= 0.99
/// (production weights: unoccluded and clear of the FOV feather) and both
/// finite.  Take it from the UNCORRECTED variant's bands so every variant of
/// one frame is scored on the same pixels.
[[nodiscard]] std::vector<std::uint8_t> coValidTrustMask(const MetricBands& bands);

// ===========================================================================
//  Stage 2: the photometric seam field
// ===========================================================================

/// What the field corrects.  The numeric values match PrefsPhotoSeam.
enum class PhotoSeamMode : int {
    Off = 0,         ///< Nothing (the stage-1 inset and global gain still apply).
    RimOnly = 1,     ///< Per-longitude usable rim only; the global gain stays.
    RimAndGain = 2,  ///< Rim plus the 2-D gain field (replaces the global gain).
};

/// Short name for logs and osvtool ("off", "rim", "full").
[[nodiscard]] const char* photoSeamModeName(PhotoSeamMode mode) noexcept;

/// Every knob of the measurement and of its application.  The defaults are
/// the research's (NEURAL_STITCHING.md 8.3), re-measured through the kernel.
struct PhotoSeamParams {
    PhotoSeamMode mode = PhotoSeamMode::RimAndGain;
    /// Analysis band: 1536 columns (0.23 deg per pixel) x +-9 deg (the
    /// +-7.59 deg overlap, the lens axes' tilt off +-Y and the gradient
    /// step).  Measured on the sample through the kernel, 1536 scores the
    /// same as 2048 (line / band / broad / dE 0.46 / 0.48 / 0.59 / 0.19 vs
    /// 0.47 / 0.49 / 0.60 / 0.18) at 56 % of the pixels; 1024 leaves under
    /// one row per 0.25 deg rim bin and the rim search fails.
    BandParams band{1536, 9.0};
    std::uint32_t gridW = 256;       ///< Field columns over 360 deg of longitude (wraps).
    std::uint32_t gridH = 16;        ///< Field rows across the OVERLAP only (the decay is analytic).

    // ---- usable rim --------------------------------------------------------
    double rimDropStops = 0.25;      ///< Rim = first bin whose lens/other ratio departs this far from its core.
    double rimCoreLoDeg = 88.5;      ///< The lens's own core ratio is taken over [lo, hi) of its theta.
    double rimCoreHiDeg = 91.0;
    double rimBinDeg = 0.25;         ///< Theta bin width of the rim search.
    double rimBlockDeg = 2.8125;     ///< Longitude block of one rim measurement (16 of 2048 columns).
    std::uint32_t rimMinCore = 20;   ///< Core pixels a block needs to be measured at all.
    std::uint32_t rimMinBin = 8;     ///< Pixels a bin needs to count.
    double rimMinHalfDeg = 4.2;      ///< Conservative running minimum over +-this along longitude.
    double rimSmoothDeg = 2.1;       ///< Then a Gaussian of this sigma along longitude.
    double rimFeatherDeg = 3.0;      ///< Kernel feather below the usable rim.
    double rimMaxTrimDeg = 6.0;      ///< Never place the rim below thetaMax - this.

    // ---- trusted pixels ------------------------------------------------------
    double trustAlpha = 0.99;        ///< Both lenses' occlusion factor at least this (DJI's rule).
    double trustMarginDeg = 0.5;     ///< Statistics use pixels this far inside both usable rims.
    /// Texture gate, stops per degree.  RIM statistics use only pixels whose
    /// |d log2 L / d lon| in BOTH lenses and |d log2 L / d lat| in the OTHER
    /// lens are below this (never the tested lens's own latitude gradient:
    /// its radial fall-off IS the signal).  GAIN statistics weight every
    /// trusted pixel by 1 / (1 + (g / flat)^2) with g its largest gradient,
    /// so flat sky dominates wherever it exists while a fully textured
    /// overlap (the ground) still yields an unbiased mean.
    double flatLog2PerDeg = 0.1;
    double gradStepDeg = 0.5;        ///< Central-difference half step of those gradients.
    /// False reproduces the research's negative result (statistics from every
    /// co-visible pixel, rim included) - for the guard test only.
    bool trustMask = true;

    // ---- gain field ------------------------------------------------------------
    double sigmaLonDeg = 4.2;        ///< Normalised-convolution sigma along longitude.
    double sigmaLatDeg = 1.5;        ///< ... and along latitude.
    double fillSigmaLonDeg = 8.4;    ///< Columns with no trusted support: wide longitude fill.
    /// Fraction of a cell's kernel mass that must be trusted for the cell to
    /// count; weaker cells take the nearest counted row's value.  0.25, not
    /// the research's 0.05: a cell on the grid's edge row that only the
    /// Gaussian's tail reaches was the one cell that moved more than 0.02 stop
    /// per frame on the sample (0.024, section 8.4 test 10).
    double minSupport = 0.25;
    double maxAbsLog2Gain = 1.5;     ///< Clamp per cell, stops.
    double minTrustedFraction = 0.02;///< Refuse the field below this fraction of trusted band pixels.

    // ---- application ------------------------------------------------------------
    double decayDeg = 20.0;          ///< Luma correction: raised cosine to 0 over this beyond the overlap.
    double chromaDecayScale = 0.5;   ///< Chroma ratios decay over this fraction of decayDeg.
    double strength = 1.0;           ///< User strength 0..1 (the gain only; the rim is all or nothing).
    double temporalAlpha = 0.35;     ///< EMA weight of a new bucket's measurement.
};

/// The field the kernel samples, plus what the importer needs to carry it
/// through time.  Layout (PhotoSeamField::kernelTable()):
///   gain: w * h * 3 floats, log2(L_master / L_slave) per channel (the FULL
///         ratio; the kernel applies +half to the slave, -half to the master),
///         row-major, rows from latMinRad to latMaxRad = the overlap;
///   rim:  w * 2 floats, usable rim angle (radians) per column, [x * 2 + lens].
/// Column j sits at longitude -pi + j * 2pi / w in the polar-axis band frame
/// (lon = atan2(x, z), lat = asin(y) of the body ray).
struct PhotoSeamField {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    float latMinRad = 0.0f;
    float latMaxRad = 0.0f;
    float thetaMaxRad[2] = {0.0f, 0.0f};  ///< Each lens's calibrated half FOV (the rim clamp).
    std::vector<float> gain;              ///< w * h * 3, log2 master / slave.
    std::vector<float> rim;               ///< w * 2, radians.
    /// w * 2 raw per-column rim measurements in radians, NaN where the
    /// column was not measured (too few flat pixels) - what the per-clip rim
    /// accumulator consumes.
    std::vector<float> rimMeasured;

    // ---- diagnostics -----------------------------------------------------------
    std::uint64_t trustedPixels = 0;      ///< Band pixels that passed the trust test.
    std::uint64_t bandPixels = 0;         ///< All band pixels.
    double medianLog2Gain[3] = {};        ///< Median of the gain cells per channel, stops.
    double rimMedianDeg[2] = {};          ///< Median usable rim per lens, degrees.
    double bandMs = 0.0;                  ///< Band shading (GPU or CPU) and download.
    double statsMs = 0.0;                 ///< Everything after the bands.

    /// Sizes consistent, every value finite, rows span a real latitude range.
    [[nodiscard]] bool valid() const noexcept;
    /// The kernel's photo table: gain, then rim.  Empty when !valid().
    [[nodiscard]] std::vector<float> kernelTable() const;
};

/// Per-lens RGB bands shaded with the ANALYSIS blend's occlusion but no FOV
/// feather (featherDeg = 0), so alpha is the occlusion factor alone and rim
/// decisions are not blurred by the production feather.
struct RgbLensBands {
    std::uint32_t w = 0;          ///< Columns (360 deg).
    std::uint32_t h = 0;          ///< Rows.
    std::uint32_t rowOffset = 0;  ///< First map row of the band.
    std::uint32_t mapH = 0;       ///< Full polar map height.
    std::vector<float> rgba[2];   ///< Scene-linear RGBA per lens, w * h * 4.
    /// Each pixel's angle from each lens's axis, w * h radians.  Shared and
    /// immutable: it depends only on the rig and the band geometry, so every
    /// bucket of a clip reads the same table.
    std::shared_ptr<const std::vector<float>> thetaRad[2];
    float thetaMaxRad[2] = {0.0f, 0.0f};  ///< The analysis blend's half FOV per lens.
};

/// Shade the per-lens bands: host frames on the CPU (shadeJobRows), device
/// frames through the installed DeviceBandShader (the GPU band hook
/// installCudaAnalyses() provides).  `blend` is the ANALYSIS blend.
[[nodiscard]] Result<RgbLensBands> renderPhotoBands(const geom::LensRig& rig, const video::FramePair& frames,
                                                    const geom::BlendParams& blend, const PhotoSeamParams& params,
                                                    ThreadPool& pool);

/// The field from bands (pure).  `pool` may be null (single-threaded).
/// Unsupported when fewer than minTrustedFraction of the band's pixels are
/// trusted (a fully occluded or blank overlap): render without the field.
[[nodiscard]] Result<PhotoSeamField> photoSeamFromBands(const RgbLensBands& bands, const PhotoSeamParams& params,
                                                        ThreadPool* pool);

/// renderPhotoBands + photoSeamFromBands, with the timings filled in.
[[nodiscard]] Result<PhotoSeamField> measurePhotoSeam(const geom::LensRig& rig, const video::FramePair& frames,
                                                      const geom::BlendParams& blend, const PhotoSeamParams& params,
                                                      ThreadPool& pool);

/// from + (to - from) * t on the gain and the rim; t is clamped to [0, 1]
/// and the endpoints return `from` / `to` exactly.  InvalidArgument for a
/// layout mismatch (size or latitude span) or a non-finite t.  The result
/// carries `to`'s diagnostics.
[[nodiscard]] Result<PhotoSeamField> blendPhotoSeamFields(const PhotoSeamField& from, const PhotoSeamField& to,
                                                          double t);

/// Log code units per stop of `color`'s input decode around mid grey, for
/// passthrough output (the kernel then applies the gain as a code offset).
/// 0 when the block is disabled or the curve is degenerate.
[[nodiscard]] float photoCodePerStop(const OsvColorParams& color) noexcept;

/// Usable rim of `lens` at polar-axis longitude `lonRad`, degrees (linear
/// in longitude, wraps).  NaN for an invalid field or lens index.
///
/// Also the seam COST WP-SEAM can use: a seam must not run where a lens is
/// past its usable rim, i.e. where that lens's theta exceeds this angle.
[[nodiscard]] double photoRimDegAt(const PhotoSeamField& field, int lens, double lonRad) noexcept;

/// The rim resampled to the `columns` columns of a polar band (column c at
/// lon = -pi + (c + 0.5) 2pi / columns), degrees.  Empty for an invalid
/// field, lens index or column count.
[[nodiscard]] std::vector<float> photoRimColumnsDeg(const PhotoSeamField& field, int lens, std::uint32_t columns);

/// Usable-rim estimate over a run of buckets: per column and lens, the
/// running median of the last kHistory accepted measurements (the rim is a
/// static property of the lens and its mount, section 1.2).  Columns never
/// measured inherit the median of the measured ones.  PhotoSeamHistory feeds
/// it the contiguous run of buckets stored before the one being stored.
class PhotoRimAccumulator {
public:
    static constexpr std::size_t kHistory = 15;

    /// Add one bucket's raw measurements (PhotoSeamField::rimMeasured).  A
    /// field of a different width restarts the history.
    void add(const PhotoSeamField& bucket);

    /// Replace `field.rim` with the accumulated rim, post-processed exactly
    /// like a single measurement (fill, running minimum, smoothing, clamp).
    /// Leaves the field untouched while nothing has been accumulated or the
    /// widths differ.
    void apply(PhotoSeamField& field, const PhotoSeamParams& params) const;

    /// Buckets added since the last clear().
    [[nodiscard]] std::uint32_t buckets() const noexcept { return m_buckets; }
    void clear() noexcept;

private:
    std::uint32_t m_w = 0;
    std::uint32_t m_buckets = 0;
    std::vector<std::deque<float>> m_history;  ///< w * 2 histories, radians.
};

/// The importer's per-clip field cache and temporal filter (and osvtool's,
/// so an osvtool A/B shows the picture a user gets).
///
///   * store(bucket, measured): an EMA against the previous bucket's STORED
///     field (temporalAlpha), and the rim replaced by the running median
///     (PhotoRimAccumulator) over the contiguous run of buckets stored
///     before this one plus this one;
///   * fieldFor(frame): the bucket's field cross-faded from the previous
///     bucket's (parallaxCrossfadeWeight, exactly like the parallax grid),
///     gain and rim alike, so neither steps at a bucket edge.
///
/// Determinism: everything a bucket's frames render with - its EMA, its rim
/// and the field it glides from - is fixed when the bucket is STORED, from
/// what was stored before it at that moment.  A bucket measured later never
/// changes it.  So a frame renders identically every time it is asked for,
/// whatever is rendered in between (the direct path's Exact contract, and
/// two instances of one clip rendering the same frame agree).  A clip-wide
/// accumulator applied at render time broke exactly that: its median
/// depended on every bucket any earlier request happened to measure.  In
/// sequential playback - buckets stored in order - the two are the same.
///
/// Not thread-safe; the importer holds its instance lock around it.
class PhotoSeamHistory {
public:
    /// How far back store() walks the contiguous run for the rim median, in
    /// buckets: four times the median's depth, so a column measured only
    /// every few buckets (texture gating) still gathers a full history.
    static constexpr std::uint32_t kRimChainBuckets = 4u * static_cast<std::uint32_t>(PhotoRimAccumulator::kHistory);

    /// Record bucket `bucket`'s measurement; nullptr records a refusal (the
    /// bucket is not measured again, and renders without a field).
    void store(std::uint32_t bucket, const std::shared_ptr<const PhotoSeamField>& measured,
               const PhotoSeamParams& params);

    /// True once `bucket` was measured (accepted or refused).
    [[nodiscard]] bool measured(std::uint32_t bucket) const;

    /// The field frame `frame` renders with, or nullptr when its bucket was
    /// refused or not measured yet.
    [[nodiscard]] std::shared_ptr<const PhotoSeamField> fieldFor(std::uint32_t frame,
                                                                 const PhotoSeamParams& params) const;

    /// The nearest ACCEPTED bucket's stored field (with its accumulated rim)
    /// within `maxBuckets` of `bucket`, earlier first.
    [[nodiscard]] std::shared_ptr<const PhotoSeamField> nearest(std::uint32_t bucket, std::uint32_t maxBuckets,
                                                                const PhotoSeamParams& params) const;

    /// Bound the cache: beyond `limit` entries the lowest buckets go (never
    /// `keep`; then the highest).  A kept bucket keeps the field it glides
    /// from even when that bucket's own entry goes.
    void trim(std::size_t limit, std::uint32_t keep);

    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return m_fields.size(); }

private:
    /// One bucket as stored.  Both pointers are fixed at store() time.
    struct Entry {
        std::shared_ptr<const PhotoSeamField> field;  ///< Its own field; null records a refusal.
        std::shared_ptr<const PhotoSeamField> from;   ///< The previous bucket's field then, or null: no glide.
    };
    std::map<std::uint32_t, Entry> m_fields;
};

/// The research's trust mask for skySeamMetrics's colour term: both lenses'
/// alpha >= 0.99 in `bands` (production weights: unoccluded, clear of the
/// FOV feather) AND at least `marginDeg` inside both usable rims of `field`
/// (theta from the rig's own geometry).  Build it from the UNCORRECTED
/// variant's bands so every variant of a frame is scored on the same pixels.
[[nodiscard]] std::vector<std::uint8_t> rimTrustMask(const MetricBands& bands, const geom::LensRig& rig,
                                                     const PhotoSeamField& field, double marginDeg = 0.5);

// ===========================================================================
//  The usable rim as a seam cost (SeamCarve.h, SeamPenaltySlot::Rim)
// ===========================================================================

/// Weight the Rim slot is installed with.  The penalty is 1 per band row a
/// lens would be shown on past its usable rim (0.1 after this weight), where
/// the seam's own disagreement cost is ~0.05 per row: decisive, but no
/// stronger than it needs to be.
inline constexpr double kPhotoRimPenaltyWeight = 0.1;

/// Per-lens penalty over a seam-carve band (pure): 0 until `rampDeg` below
/// the lens's usable rim (photoRimDegAt), rising linearly to 1 at the rim and
/// beyond.  Theta is each band pixel's angle from the lens axis through the
/// rig (bandPixelDirection).  The maps must already be bands.w * bands.h
/// long; false (maps untouched) for any size mismatch or an invalid field.
[[nodiscard]] bool photoRimPenalty(const LensBands& bands, const geom::LensRig& rig, const PhotoSeamField& field,
                                   std::vector<float>& penaltySlave, std::vector<float>& penaltyMaster,
                                   double rampDeg = 0.5) noexcept;

/// Install the process-wide Rim hook (once; later calls do nothing).  The
/// hook judges a carve by the rig and field of the PhotoRimPenaltyScope
/// active on the CALLING thread - carves run synchronously on the thread
/// that asked for them - and contributes nothing without one, so clips
/// without a measured rim, other callers and other threads are unaffected.
void installPhotoRimPenaltyHook() noexcept;

/// Makes a rig and a field the Rim hook's reference for this thread until
/// the scope ends (scopes nest; the previous one is restored).  A null rig
/// or field, or an invalid field, installs "no penalty".
class PhotoRimPenaltyScope {
public:
    PhotoRimPenaltyScope(const geom::LensRig* rig, std::shared_ptr<const PhotoSeamField> field) noexcept;
    ~PhotoRimPenaltyScope();
    PhotoRimPenaltyScope(const PhotoRimPenaltyScope&) = delete;
    PhotoRimPenaltyScope& operator=(const PhotoRimPenaltyScope&) = delete;

    /// The context the hook sees (null when none); an implementation detail
    /// exposed so the hook in PhotoSeam.cpp can reach it.
    struct Context {
        const geom::LensRig* rig = nullptr;
        std::shared_ptr<const PhotoSeamField> field;
    };

private:
    Context m_context;
    const Context* m_previous = nullptr;
};

}  // namespace osv::render
