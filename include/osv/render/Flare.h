// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Flare.h - sun ghost and veiling-glare removal (WP-FLARE).
//
// WHAT THIS IS FOR
// ----------------
// When the sun is inside a lens's field of view, that lens's frame carries
// stray light the scene never had (docs/research/FLARE.md has the full
// characterisation on the sample clip):
//
//   * GHOSTS: internal reflections between lens surfaces, the sensor stack
//     and the ND filter.  Each is a defocused image of the aperture clipped by
//     a rectangular stop - a soft-edged ROUNDED RECTANGLE of nearly flat
//     brightness sitting near the line from the optical centre through the
//     sun.  On the sample clip the brightest one adds 28 % (fitted plateau;
//     78 % at its peak pixel) to the sky behind it.
//   * VEIL: light scattered everywhere inside the barrel - a near-uniform
//     additive floor over the whole frame of the sun's lens (about +0.045
//     scene-linear on the sample, i.e. +70 % on the deep-blue zenith sky).
//
// Both are ADDITIVE in linear light, so removing them is a subtraction in the
// lens's native scene-linear RGB before the lens gain and the blend.  The
// kernel side lives in osv_kernel.h's [WP-FLARE] regions (OsvFlareLens,
// osvFlareRemove); this header is the host side that measures what to
// subtract.
//
// PIPELINE (per analysed frame, e.g. once per parallax bucket)
// -----------------------------------------------------------
//   1. flareDownsample()     each lens to a factor-4 native-linear RGB image
//                            (GPU when the frames are in VRAM and the CUDA
//                            sampler is installed - see FlareCuda.h).
//   2. analyseLensFlare()    find the sun (largest compact saturated blob),
//                            seed ghost candidates near the sun line on
//                            smooth background, fit a rotated rounded
//                            rectangle + quadratic background to each by
//                            Levenberg-Marquardt (variable projection), and
//                            keep only fits that pass every quality gate.
//   3. applyFlare()          write the model into OsvRenderParams; the
//                            kernel subtracts it with a soft knee that can
//                            never produce negative light.
//
// analyseFlare() runs 1-2 for both lenses of a frame pair.
//
// THE SEAM HOOK
// -------------
// flareCost() is a pure function WP-SEAM's seam finder can call: the
// fraction of the light at a lens pixel that is predicted flare.  Where both
// lenses see a direction (the overlap band), steering the seam toward the
// lens with the lower cost keeps a ghost out of the stitched picture
// altogether - the dual-lens advantage no single-lens method has.
// flareCostBand() evaluates it over a polar-axis band in one call.
//
// WHAT IT NEVER DOES
// ------------------
//   * It never touches a pixel away from a fitted ghost: every ghost's
//     footprint ends exactly at its bounding radius (reach2 in the kernel).
//   * It never makes light negative or reverses a gradient: the kernel's
//     soft subtraction keeps f(x) >= 0.47 x and f'(x) >= 0.16.
//   * The sun disc itself is excluded from the ghost search, so the sun, its
//     star and its glow are left alone.
//
// The veil estimate from the overlap (estimateVeil) is OFF by default and
// must stay off until it is cleared legally: see the note on that function
// and docs/research/FLARE.md ("Patents").

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/LensRig.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/osv_kernel.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace osv::render {

/// Ghosts the kernel carries per lens (OSV_FLARE_MAX_GHOSTS).
inline constexpr int kFlareMaxGhosts = OSV_FLARE_MAX_GHOSTS;

// ===========================================================================
//  Working image
// ===========================================================================

/// A small native-linear RGB image of one lens: the image the analysis runs
/// on.  Pixel (x, y) stands for the lens's `factor` x `factor` block starting
/// at (x * factor, y * factor): the linear mean of a 2 x 2 sample grid
/// centred in it (osvFlareDownsamplePixel).  A continuous coordinate u in
/// this image is u * factor in stream pixels (both with sample centres at
/// +0.5).
struct FlareImage {
    std::uint32_t w = 0;       ///< Width in analysis pixels.
    std::uint32_t h = 0;       ///< Height in analysis pixels.
    std::uint32_t factor = 1;  ///< Stream pixels per analysis pixel.
    std::vector<float> rgb;    ///< Interleaved R, G, B, row-major, w * h * 3 values.

    /// True when the dimensions are sane and the buffer matches them.
    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && factor > 0 && w <= 16384 && h <= 16384 &&
               rgb.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u;
    }
};

/// Analysis size of a stream dimension: ceil(n / factor).
[[nodiscard]] constexpr std::uint32_t flareAnalysisSize(std::uint32_t n, std::uint32_t factor) noexcept {
    return factor == 0 ? 0u : (n + factor - 1u) / factor;
}

/// CPU reference downsample of one lens plane (host pointers) through the
/// shared kernel function osvFlareDownsamplePixel, rows spread over `pool`.
/// Fails with InvalidArgument for a malformed plane or a factor outside
/// [1, 16].
[[nodiscard]] Result<FlareImage> flareDownsample(const OsvPlane& hostPlane, const OsvColorParams& color,
                                                 std::uint32_t factor, ThreadPool& pool);

/// Convenience overload for a decoded host frame.
[[nodiscard]] Result<FlareImage> flareDownsample(const video::PlanarFrame16& frame, const OsvColorParams& color,
                                                 std::uint32_t factor, ThreadPool& pool);

// ===========================================================================
//  GPU sampler hook
// ===========================================================================

/// The GPU twin of flareDownsample() for a plane whose pointers are DEVICE
/// addresses (a keepOnDevice decode).  osv_render_cpu cannot link CUDA, so
/// the CUDA library installs an implementation (installCudaFlareSampler(),
/// FlareCuda.h); without one, analyseFlare() refuses device-only frames.
class FlareDeviceSampler {
public:
    virtual ~FlareDeviceSampler() = default;

    /// Same contract and output as flareDownsample(), from device planes.
    [[nodiscard]] virtual Result<FlareImage> downsample(const OsvPlane& devicePlane, const OsvColorParams& color,
                                                        std::uint32_t factor) = 0;

    /// Backend name for logs ("cuda").
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// Install (or, with nullptr, remove) the process-wide device sampler.
void setFlareDeviceSampler(std::shared_ptr<FlareDeviceSampler> sampler);

/// The installed device sampler, or nullptr.
[[nodiscard]] std::shared_ptr<FlareDeviceSampler> flareDeviceSampler();

// ===========================================================================
//  The model
// ===========================================================================

/// One fitted ghost, in STREAM pixels of its lens.  Its light is the
/// kernel's model (OsvFlareGhost in osv_kernel.h): a plateau `amp`, a caustic
/// rim `rim` on the edge, and a tilt `gradX` / `gradY` across the plateau,
/// clamped to non-negative light per pixel.
struct FlareGhost {
    double cx = 0.0;                     ///< Centre x (continuous stream px).
    double cy = 0.0;                     ///< Centre y.
    double hx = 0.0;                     ///< Half extent along the local x axis (px).
    double hy = 0.0;                     ///< Half extent along the local y axis (px).
    double radius = 0.0;                 ///< Corner radius (px), 0 .. min(hx, hy).
    double angleRad = 0.0;               ///< Rotation of the local x axis.
    double soft = 1.0;                   ///< Half width of the smoothstep edge (px).
    std::array<double, 3> amp{};         ///< Additive native-linear RGB on the plateau (>= 0).
    std::array<double, 3> rim{};         ///< Additive RGB of the edge bump at its crest (may be < 0).
    std::array<double, 3> gradX{};       ///< Plateau tilt along local x, RGB at the +x edge.
    std::array<double, 3> gradY{};       ///< Plateau tilt along local y, RGB at the +y edge.
    double contrast = 0.0;               ///< Plateau luma / local background luma.
    double fitR2 = 0.0;                  ///< Fraction of the local variance the ghost terms explain.

    /// Bounding radius of the footprint (px): the kernel's reach.  Past it
    /// every term is exactly zero (the rim bump ends at 3 * soft).
    [[nodiscard]] double reach() const noexcept;
};

/// Everything measured for one lens.
struct LensFlare {
    bool sunFound = false;               ///< A sun (compact saturated blob) is in this lens.
    double sunX = 0.0;                   ///< Sun centroid (stream px).
    double sunY = 0.0;
    double sunRadiusPx = 0.0;            ///< Equivalent radius of the saturated disc (stream px).
    double sunThetaRad = 0.0;            ///< Angle of the sun from the optical axis.
    std::vector<FlareGhost> ghosts;      ///< Accepted ghosts, most visible first (<= kFlareMaxGhosts).
    std::array<double, 3> veil{};        ///< Uniform additive veil (native linear RGB), 0 unless estimated.
    std::uint32_t candidates = 0;        ///< Candidates seeded (diagnostic).
    std::uint32_t rejected = 0;          ///< Candidates the gates refused (diagnostic).
};

/// Both lenses (index = LensIndex: 0 slave, 1 master).
struct FlareModel {
    std::array<LensFlare, 2> lens{};

    /// True when anything would be subtracted.
    [[nodiscard]] bool any() const noexcept;
};

/// Analysis tuning.  Defaults are the values measured on the sample clip
/// (docs/research/FLARE.md explains each).
struct FlareParams {
    std::uint32_t factor = 4;            ///< Stream px per analysis px (1..16).

    // ---- sun ----------------------------------------------------------------
    double sunLevelFraction = 0.92;      ///< Sun pixels: luma >= this fraction of the frame maximum.
    double sunMinRatioToMedian = 6.0;    ///< The maximum must exceed the median by this much.
    double sunMaxAspect = 1.8;           ///< Bounding-box aspect limit of the sun blob.
    double sunMinFill = 0.5;             ///< Blob area / bounding-box area lower limit.
    double sunMinAreaPx = 3.0;           ///< Minimum blob area (analysis px).

    // ---- candidates ---------------------------------------------------------
    double corridorDeg = 20.0;           ///< Azimuth tolerance about the sun line (both directions).
    double backgroundSigmaPx = 12.0;     ///< Background blur for the relative band-pass (analysis px).
    double seedContrast = 0.03;          ///< Relative band-pass threshold that seeds a candidate.
    double maxTexture = 0.02;            ///< Median local texture around a candidate (relative).
    double minPeakOverTexture = 4.0;     ///< Candidate peak must exceed its surrounding texture this much.
    double sunExclusionRadii = 3.0;      ///< No candidates within this many sun radii of the sun.
    double rimFraction = 0.95;           ///< Only inside this fraction of the image-circle radius.
    /// Largest seed blob considered (analysis px^2).  The sample's ghosts
    /// seed at 300..500; a seed of 2000 (a radius of 25, 100 stream px at
    /// factor 4) is already a sky structure rather than a reflection, and
    /// its fit window is what used to dominate the analysis time.
    double maxCandidateAreaPx = 2000.0;
    /// Largest seed bounding-box side, in STREAM px (so it means the same at
    /// every analysis factor).  The sample's largest ghost seeds at 92 (23
    /// analysis px at factor 4); a seed of 204 - a sky structure at the rim
    /// - was refused anyway and its fit, in the largest window, cost 70 ms
    /// of an 85 ms analysis.  160 keeps 1.7x headroom over every ghost
    /// measured.
    double maxCandidateExtentStreamPx = 160.0;

    // ---- fit and acceptance -------------------------------------------------
    double minContrast = 0.04;           ///< Fitted plateau luma / background luma lower limit.
    double minFitR2 = 0.5;               ///< Local explained-variance lower limit.
    double maxAspect = 3.0;              ///< hx / hy limit either way.
    double maxSoftPx = 6.0;              ///< Edge half width upper bound (analysis px).
    int maxIterations = 30;              ///< Levenberg-Marquardt iterations per candidate.
    int maxGhosts = kFlareMaxGhosts;     ///< Ghosts kept per lens (<= kFlareMaxGhosts).
};

// ===========================================================================
//  Analysis
// ===========================================================================

/// Detect the sun and fit the ghosts of ONE lens from its working image.
/// `lens` supplies the optical centre and the image-circle radius (stream
/// px).  A frame without a sun is not an error: the result has
/// sunFound == false and no ghosts.  Fails with InvalidArgument for an
/// invalid image, lens or parameter set.  `pool` (optional) runs the ghost
/// fits side by side; the result does not depend on it.  Do not pass a pool
/// from inside one of its own jobs (ThreadPool does not nest).
[[nodiscard]] Result<LensFlare> analyseLensFlare(const FlareImage& image, const geom::KannalaBrandt5& lens,
                                                 const FlareParams& params, ThreadPool* pool = nullptr);

/// Downsample and analyse both lenses of a frame pair.  Host frames use the
/// CPU sampler; device-only frames need an installed FlareDeviceSampler.
[[nodiscard]] Result<FlareModel> analyseFlare(const geom::LensRig& rig, const video::FramePair& frames,
                                              const OsvColorParams& color, const FlareParams& params,
                                              ThreadPool& pool);

/// The working image of lens `lens` (0 slave, 1 master) of a frame pair:
/// the host frame through the CPU sampler, else the device frame through the
/// installed FlareDeviceSampler (InvalidArgument when there is none).
[[nodiscard]] Result<FlareImage> flareDownsampleLens(const video::FramePair& frames, int lens,
                                                     const OsvColorParams& color, std::uint32_t factor,
                                                     ThreadPool& pool);

// ===========================================================================
//  The per-frame sun check
// ===========================================================================
//
// A fitted ghost is only where the model says while the sun is where it was
// when the model was measured: a ghost is an image of the sun, and it moves
// when the sun moves across the lens (on the sample clip the brightest one
// by 0.45 px per px of sun motion; how far for other paths is a property of
// the lens that one clip cannot calibrate).  So before a cached model is
// applied to a frame, the frame's own sun is located - a coarse, cheap pass
// (flareSunCheckFactor) - and the model is used only while the sun sits
// within flareSunTolerancePx of where the model saw it.  A panning shot
// therefore needs a measurement per frame; a steady one reuses one per
// bucket.

/// Where the sun is in one lens of one frame.
struct FlareSunFix {
    bool found = false;     ///< A sun (compact saturated blob) is in this lens.
    double x = 0.0;         ///< Centroid (stream px).
    double y = 0.0;
    double radiusPx = 0.0;  ///< Equivalent radius (stream px).
};

/// Both lenses (index = LensIndex).
using FlareSunFixes = std::array<FlareSunFix, 2>;

/// Working-image factor of the sun check for a lens `lensW` px wide (375-750
/// analysis px across: 8 at 6K, 2 for the 1024 px proxy; clamped to 1..16).
[[nodiscard]] std::uint32_t flareSunCheckFactor(std::uint32_t lensW) noexcept;

/// How far (stream px) the sun may be from where a model saw it for the
/// model to still describe the frame: 3 px at 6K, scaled with the lens.
[[nodiscard]] double flareSunTolerancePx(std::uint32_t lensW) noexcept;

/// The sun of one working image, found exactly as analyseLensFlare finds it.
/// No sun (or garbage in) is `found == false`, never an error.
[[nodiscard]] FlareSunFix locateSun(const FlareImage& image, const geom::KannalaBrandt5& lens,
                                    const FlareParams& params) noexcept;

/// The sun check of a frame pair: both lenses at flareSunCheckFactor, host
/// or device frames (see flareDownsampleLens).
[[nodiscard]] Result<FlareSunFixes> locateSuns(const geom::LensRig& rig, const video::FramePair& frames,
                                               const OsvColorParams& color, const FlareParams& params,
                                               ThreadPool& pool);

/// True when two sun checks describe the same sun: present in the same
/// lenses, and each within `tolerancePx` of the other.  Both sides must come
/// from the same kind of check (the importer compares sun checks with sun
/// checks, never with a model's own full-resolution sun).
[[nodiscard]] bool flareSunsMatch(const FlareSunFixes& a, const FlareSunFixes& b, double tolerancePx) noexcept;

// ===========================================================================
//  Veil (OFF by default - read before enabling)
// ===========================================================================

/// Veil estimate from the two lenses' LINEAR luma bands over the overlap
/// (renderLensBands(..., linear = true, ...)).
///
/// The overlap is the one place both lenses see the same scene, so a lens
/// carrying veil reads brighter there by an ADDITIVE offset, while a pure
/// exposure mismatch reads brighter by a RATIO.  Column blocks of different
/// brightness separate the two: fit m_veiled = g * m_clean + v across the
/// smooth blocks and attribute v to the lens that has the sun.  The result
/// is a neutral RGB veil for that lens (zero for the other, and zero for
/// both when neither lens has a sun or the fit is not trustworthy).
///
/// LEGAL: comparing the two images along the stitch line to estimate and
/// subtract flare is the subject of active GoPro patents (US11330208B2 and
/// related, see docs/research/FLARE.md).  This function is not called by
/// analyseFlare(); do not wire it into a shipping path until that has been
/// cleared.
[[nodiscard]] Result<std::array<std::array<double, 3>, 2>> estimateVeil(const LensBands& linearBands,
                                                                        const FlareModel& model);

// ===========================================================================
//  Kernel parameters
// ===========================================================================

/// Write `model` into the [WP-FLARE] fields of `params` (flareEnabled = 1
/// when anything is to be subtracted, 0 otherwise).  Ghosts beyond
/// kFlareMaxGhosts are dropped, non-finite values are refused (the lens is
/// left clean), and every amplitude is clamped to >= 0.
void applyFlare(const FlareModel& model, OsvRenderParams& params) noexcept;

/// Turn the removal off and zero the [WP-FLARE] fields.
void clearFlare(OsvRenderParams& params) noexcept;

/// Blend `current` toward `previous` for temporal stability: ghosts matched
/// by centre distance interpolate every parameter with weight `weight` on
/// the current fit; a new ghost enters at `weight` of its amplitude, a lost
/// one fades out at (1 - weight).  weight = 1 returns `current`.
[[nodiscard]] FlareModel smoothFlare(const FlareModel& previous, const FlareModel& current, double weight);

// ===========================================================================
//  Seam cost hook (for WP-SEAM)
// ===========================================================================

/// Weights of the terms of flareCost().
struct FlareCostParams {
    double ghostWeight = 1.0;            ///< Weight of the fitted ghosts.
    double veilWeight = 1.0;             ///< Weight of the uniform veil.
    double sunGlareWeight = 1.0;         ///< Weight of the glare around the sun disc.
    double sunGlareRadii = 4.0;          ///< Glare term reaches this many sun radii.
    double referenceSignal = 0.18;       ///< Signal assumed when the caller passes none (grey card).
};

/// Fraction of the light at stream pixel (px, py) of the lens described by
/// `lens` that is predicted flare, in [0, 1):
///
///     flare / (flare + signal)
///
/// flare = veil + the ghosts' plateau luma at the pixel + a glare term that
/// is 1 on the sun disc and falls to 0 at sunGlareRadii sun radii.  `signal`
/// is the pixel's own linear luma when the caller has it (it makes the cost
/// honest in dark regions); a non-positive or non-finite value selects
/// params.referenceSignal.  Pure: no state, no allocation, never throws; a
/// non-finite position returns 0.
[[nodiscard]] float flareCost(const LensFlare& lens, double px, double py, double signal = -1.0,
                              const FlareCostParams& params = {}) noexcept;

/// flareCost() for every pixel of rows [row0, row0 + rows) of a polar-axis
/// equirect of mapW x mapH (the band geometry of LensBands, body frame, no
/// stabilisation), for both lenses: cost[i][r * mapW + x].  A band pixel a
/// lens does not see gets cost 1 for that lens.  `signal` (optional) holds
/// per-lens linear luma bands of the same size (LensBands::luma); pass
/// nullptr to use the reference signal.
[[nodiscard]] Status flareCostBand(const FlareModel& model, const geom::LensRig& rig, std::uint32_t mapW,
                                   std::uint32_t mapH, std::uint32_t row0, std::uint32_t rows,
                                   const std::array<const std::vector<float>*, 2>* signal,
                                   std::array<std::vector<float>, 2>& cost, const FlareCostParams& params = {});

/// The flare model as a WP-SEAM penalty source (SeamCarve.h).
///
/// `hook` has exactly the signature of SeamLensPenaltyFn, so it registers
/// as-is, with a FlareSeamPenalty as the `user` pointer:
///
///     static render::FlareSeamPenalty flareSeam;         // outlives every carve
///     render::setSeamPenaltyHook(render::SeamPenaltySlot::Flare,
///                                {&render::FlareSeamPenalty::hook, &flareSeam, 1.0});
///     ...
///     flareSeam.update(model, rig);                      // per analysed bucket
///
/// or, per clip, as SeamCarveParams::penalty with a per-instance object (the
/// slot is process-wide; several open clips each need their own source).
///
/// It fills "cost of SHOWING this lens here" with flareCost() of each band
/// pixel, in [0, 1): ~0.2 on the sample's brightest ghost, ~1 on the sun
/// disc, 0 elsewhere - at weight 1 already decisive against the seam's own
/// costs over a few rows.  Pixels a lens does not see get 0: coverage is the
/// seam's own business.  The veil does NOT steer the seam by default
/// (veilWeight = 0): it is uniform over a lens, so moving the seam cannot
/// remove it, and the subtraction handles it.  The band luma is ignored
/// (its encoding is the seam's choice), so the reference signal is used.
/// Band pixels are evaluated at their own direction, not through the
/// parallax warp the carve applies - an error below one degree, far under a
/// ghost's size.
///
/// Thread-safe: update() swaps an immutable snapshot under a mutex and the
/// hook works on its own copy of the pointer, so a carve on a background
/// thread never sees a half-written model.
class FlareSeamPenalty {
public:
    /// The seam defaults: ghosts and sun glare, no veil.
    [[nodiscard]] static FlareCostParams seamDefaults() noexcept;

    /// Install the model (and the rig it was measured with) for later carves.
    void update(const FlareModel& model, const geom::LensRig& rig, const FlareCostParams& params = seamDefaults());

    /// Forget the model: the hook then contributes nothing.
    void clear() noexcept;

    /// SeamLensPenaltyFn.  Returns false (contributing nothing) with no model,
    /// no sun in either lens, a null `user`, or maps not sized w * h.
    static bool hook(const LensBands& warped, std::vector<float>& penaltySlave, std::vector<float>& penaltyMaster,
                     void* user) noexcept;

private:
    struct Snapshot {
        FlareModel model;
        geom::LensRig rig;
        FlareCostParams params;
    };
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;

    mutable std::mutex m_mutex;
    std::shared_ptr<const Snapshot> m_snapshot;
};

}  // namespace osv::render
