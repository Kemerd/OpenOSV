// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareStage.h - sun ghost removal inside the importer (WP-FLARE).
//
// WHAT IT DOES
// ------------
// One per ImporterInstance.  For every frame the importer stitches - on the
// equirect path (renderFrame) and on the direct GPU path (directFrame), both
// through ImporterInstance::applyAnalyses - it decides which fitted ghost
// model (osv/render/Flare.h) to subtract, and hands it to the frame's
// RenderParamsBuilder.  From there the model travels inside OsvRenderParams,
// so every backend and the direct kernel subtract it with no further
// plumbing.
//
// THE SCHEDULE (the parallax grid's, plus one rule of its own)
// ---------------------------------------------------------
//   * Models are cached per BUCKET (render::parallaxBucket) and measured on
//     the frame of the bucket that is asked for first.
//   * Its own rule: a model is USABLE for a frame only while that frame's sun
//     is where the model saw it (render::locateSuns / flareSunsMatch, within
//     flareSunTolerancePx).  A ghost is an image of the sun and moves with
//     it; subtracting a model at a stale position would cut a dark copy of
//     the ghost into clean sky.  Conversely a usable model is usable in ANY
//     bucket: a frame whose own bucket has none adopts the nearest usable
//     neighbour's (within kBorrowBuckets) into its bucket.  A steady shot
//     therefore measures once; a pan measures wherever the sun moves on.
//   * An Exact request (export, a paused frame, and every direct-path frame)
//     with no usable model measures it now, on the render pool, and waits.
//   * An Interactive request (playback, scrubbing) NEVER waits: without a
//     usable model it renders without removal and is marked non-exact, and
//     the measurement goes to a background worker (one slot, latest wins)
//     that never takes the instance lock.
//   * No glide between buckets: a blend depends on which neighbours happen to
//     be cached, and an Exact frame must be the same whatever was rendered
//     before it (the direct path relies on that).  render::smoothFlare stays
//     available for a caller that can afford the dependence.
//
// WHEN IT DOES NOT LOOK AT ALL
// ----------------------------
//   * A scene the camera metered too dark for the sun to be in view
//     (render::flareSceneTooDark on the frame's EV100, sceneEv100()) is
//     answered before the sun check: at night every street lamp passes the
//     image tests for "the sun", and fitting ghosts around lamps removed
//     reflections, lane markings and headlights on ~3 % of a night drive's
//     frames, one frame at a time.  The answer depends only on the frame's
//     own metadata, so it is as final as any other.
//   * The sun check is held to one sun (render::resolveOneSun), and only a
//     lens the check found that sun in is analysed: a lens whose check found
//     none, or whose blob the rule dropped, gets no working image and no
//     fits.  This is deliberately wider than dropping the rule's rejects.
//     Before the rule the other lens was analysed at the finer factor
//     whenever either lens had a sun, and there detectSun could crown a blob
//     the check never saw - a glint, or a sliver of a sun mostly outside the
//     usable circle - and fit ghosts around it with nothing holding it to
//     the one sun.  The check is now the one judge of where the sun is, as
//     it already is for model reuse (flareSunsMatch).  For the same reason
//     the finer suns of two analysed lenses are not held to one sun again:
//     both lenses are analysed only when their checks already agree on it.
//
// COST ON THE RENDER THREAD
// -------------------------
// Every wanted frame pays the sun check once (both lenses at ~375 px:
// measured 5.4-5.8 ms on the CPU pool, 2.8-3.4 ms from device frames); a
// repeat render of an answered frame skips it.  An Interactive miss additionally
// pays the working images it hands to the worker; the fits (the tens to
// hundreds of milliseconds) run only on the worker.  An Exact miss pays the
// whole analysis, as an export should.
//
// LOCKS
// -----
// The stage has its own mutex.  apply() runs under the instance lock and
// takes the stage mutex briefly; the worker takes ONLY the stage mutex, so
// stop() can join it while the instance lock is held (releaseHeavy does).

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/LensRig.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/Flare.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace osv::premiere {

class FlareStage {
public:
    /// What apply() did to the frame.
    struct Outcome {
        /// False when an Interactive frame went without removal because no
        /// model fits its sun yet (one is being measured in the background);
        /// an Exact request must never reuse such a frame.
        bool exact = true;
        /// A model was handed to the builder.
        bool applied = false;
    };

    FlareStage() = default;
    /// Stops and joins the worker.
    ~FlareStage();
    FlareStage(const FlareStage&) = delete;
    FlareStage& operator=(const FlareStage&) = delete;

    /// Decide - measuring when it must - the removal for frame `index` and
    /// give it to `builder`.
    ///
    /// `enabled` is the Source Settings switch (PrefsBlob::flareRemoval);
    /// `draft` a draft request, which never pays for the analysis (not even
    /// the sun check) and only subtracts a model already fitted for the
    /// frame - non-final without one;
    /// `exactWanted` the request's purpose.  `color` is the clip's block:
    /// the analysis reads only its input decode (it works in native linear
    /// light), and a passthrough output (D-Log M) switches the removal off,
    /// because the kernel blends that output in log code.  `sceneEv100` is
    /// the frame's metered scene brightness (sceneEv100()): below
    /// FlareParams::minSceneEv100 the frame is answered with nothing to
    /// remove before any image is looked at; NaN (not recorded) keeps the
    /// full check.  `clip` names the file in the log.  The caller holds the
    /// instance lock.  Never fails a frame: every problem is logged and
    /// leaves the frame without removal.
    Outcome apply(std::uint32_t index, const video::FramePair& pair, const geom::LensRig& rig,
                  const OsvColorParams& color, double sceneEv100, bool enabled, bool draft, bool exactWanted,
                  ThreadPool& pool, render::RenderParamsBuilder& builder, const std::string& clip) noexcept;

    /// Scene exposure value (render::flareSceneEv100) of frame `index` of a
    /// clip, from what the camera recorded: the frame's ISO and shutter and
    /// the clip's aperture.  NaN when any of them is missing or unusable, or
    /// the frame's metadata cannot be read.  Reads one cached metadata
    /// sample: cheap enough for every frame.
    [[nodiscard]] static double sceneEv100(const meta::MetadataTrack& track, std::uint32_t index) noexcept;

    /// The hook for WP-SEAM's carve (SeamCarveParams::penalty): it makes
    /// the lens showing the model applied by the latest apply() expensive
    /// where its ghosts and sun are, and contributes nothing when that frame
    /// had no model.  Valid for this stage's lifetime.  For a seam carved on
    /// the frame itself only (the stand-in lane): a bucket's ANCHORED seam
    /// takes its anchor's model (anchorSeamPenalty), whichever frame carves it.
    [[nodiscard]] render::SeamPenaltyHook seamPenalty() noexcept;

    /// @brief The removal of a bucket's ANCHOR frame, for the carve of that
    /// bucket's anchored seam: installed into `penalty` (cleared when the
    /// anchor has nothing to remove).
    ///
    /// WHY.  A bucket's anchored seam is carved once and then reused by every
    /// frame of the bucket, in playback and parked alike.  Steered by the
    /// model of whichever frame carved it (seamPenalty(), the latest apply()),
    /// it depended on which frame of the bucket happened to be asked first -
    /// and an Interactive frame without a model yet carved it with none at
    /// all.  Steered by the anchor's own answer it is the seam a sequential
    /// render carves (there the anchor is the first frame of its bucket),
    /// whichever frame asks.
    ///
    /// With `pair` - the ANCHOR's frames, never another frame's - the anchor
    /// is answered as an Exact apply() would answer it: the cached answer, a
    /// usable model adopted, or one measured now.  Without, it is only looked
    /// up: its own answer, or a model that now fits the sun an earlier
    /// Interactive render of it saw (recorded then); nothing is decoded or
    /// measured.  Arguments as for apply(); `sceneEv100` is the ANCHOR's.
    ///
    /// @return true when the anchor's answer is settled (`penalty` then holds
    ///         it - possibly nothing: no sun, too dark, removal off, a D-Log M
    ///         passthrough output, a failed sun check); false when it is not
    ///         known yet (a look-up with no answer, or an analysis that
    ///         failed), `penalty` then cleared.  Never throws.
    [[nodiscard]] bool anchorSeamPenalty(std::uint32_t anchor, const video::FramePair* pair,
                                         const geom::LensRig& rig, const OsvColorParams& color, double sceneEv100,
                                         bool enabled, ThreadPool& pool, const std::string& clip,
                                         render::FlareSeamPenalty& penalty) noexcept;

    /// Forget every model, drop the pending job and bump the generation, so
    /// a measurement still running for the old settings is discarded
    /// (prefs change, quiet).  Also re-arms the one-line-per-clip log.
    void reset() noexcept;

    /// Stop and join the worker, dropping any job not yet started.  May wait
    /// for one in-flight analysis.  Safe with the instance lock held; the
    /// worker restarts lazily on the next Interactive miss.
    void stop() noexcept;

private:
    /// One measured model and the sun check of the frame it was measured on.
    struct Entry {
        render::FlareModel model;
        render::FlareSunFixes suns{};
        std::uint32_t frame = 0;
    };
    using EntryPtr = std::shared_ptr<const Entry>;

    /// Background work: the frame's working images, OWNED, so the decoded
    /// frame is released as soon as they exist.
    struct Job {
        std::uint32_t bucket = 0;
        std::uint32_t frame = 0;
        std::uint64_t generation = 0;
        render::FlareSunFixes suns{};
        std::array<render::FlareImage, 2> images;
        std::array<geom::KannalaBrandt5, 2> lenses;
        render::FlareParams params;
        std::string clip;
    };

    /// The model measured on the frame whose sun check `suns` matches best
    /// among `entries` (nullptr when none is within `tolerancePx`).
    [[nodiscard]] static EntryPtr bestMatch(const std::vector<EntryPtr>& entries, const render::FlareSunFixes& suns,
                                            double tolerancePx) noexcept;

    /// One bucket: the models measured in (or adopted into) it, and the
    /// model each of its frames was given - nullptr for a frame with no sun.
    /// A frame keeps its first answer, so it renders the same every time
    /// whatever is measured later (the direct path relies on that), and a
    /// repeat render skips the sun check altogether.
    struct Bucket {
        std::vector<EntryPtr> entries;
        std::map<std::uint32_t, EntryPtr> frames;
        /// The sun check of frames checked but not answered yet (an
        /// Interactive miss: no model fitted, one being measured), so a
        /// look-up (anchorSeamPenalty without frames) can adopt a model that
        /// fits them once one lands.  At most one per frame of the bucket;
        /// a frame's entry goes when it is answered (assignLocked).
        std::map<std::uint32_t, render::FlareSunFixes> checked;
    };

    /// What one frame's removal is, and whether that answer is final.
    struct Decision {
        /// exact: the answer is settled (false: an Interactive miss, a
        /// look-up with no answer, a failed analysis).  applied: unused here
        /// (apply() sets it when it hands a model over).
        Outcome outcome;
        /// The model to subtract (null: nothing to remove).
        EntryPtr entry;
    };

    /// The schedule of apply() without its two side effects (the builder and
    /// the carve's latest-model hook): the answer for frame `index`.  `pair`
    /// null makes it a look-up (see anchorSeamPenalty).  Never throws.
    [[nodiscard]] Decision decide(std::uint32_t index, const video::FramePair* pair, const geom::LensRig& rig,
                                  const OsvColorParams& color, double sceneEv100, bool enabled, bool draft,
                                  bool exactWanted, ThreadPool& pool, const std::string& clip) noexcept;

    /// A usable model for a frame of `bucket` whose sun check is `suns`: the
    /// bucket's own models first, then its neighbours' (nearest first,
    /// earlier first, within kBorrowBuckets).  Caller holds m_mutex.
    [[nodiscard]] EntryPtr findUsableLocked(std::uint32_t bucket, const render::FlareSunFixes& suns,
                                            double tolerancePx) const;

    /// Store `entry` under `bucket` (caller holds m_mutex), bounded per
    /// bucket and in buckets.  A model already there is not stored twice.
    void storeLocked(std::uint32_t bucket, const EntryPtr& entry);

    /// Record that frame `frame` of `bucket` uses `entry` (nullptr: no sun).
    /// The caller holds m_mutex.
    void assignLocked(std::uint32_t bucket, std::uint32_t frame, const EntryPtr& entry);

    /// Analyse the working images of the lenses the sun check `suns` (already
    /// held to one sun) has the sun in, serially when `pool` is null.  A lens
    /// without it is left empty and its image is not read (it may be empty):
    /// a lamp or a glint there is not the sun, and ghosts fitted around it
    /// would cut real scene.
    [[nodiscard]] static Result<render::FlareModel> analyse(const std::array<render::FlareImage, 2>& images,
                                                            const std::array<geom::KannalaBrandt5, 2>& lenses,
                                                            const render::FlareSunFixes& suns,
                                                            const render::FlareParams& params, ThreadPool* pool);

    /// Log the first measured model of the clip (once per reset), with the
    /// scene brightness the frame was judged by (`sceneEv100`, NaN when not
    /// recorded) - the one place the log shows that the importer handed the
    /// stage the frame's metered exposure.
    void logModelOnce(const std::string& clip, std::uint32_t frame, const render::FlareModel& model,
                      double sceneEv100) noexcept;

    /// Log why a frame has no removal (once per reason per reset).
    void logReasonOnce(int reason, const std::string& text) noexcept;

    /// The worker's body.  Takes only m_mutex.
    void workerLoop() noexcept;

    // ---- guarded by m_mutex -------------------------------------------------
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;  ///< Started lazily; joined by stop().
    bool m_stop = false;
    std::optional<Job> m_pending;
    /// Bucket and sun check of the job being analysed right now.
    std::optional<std::pair<std::uint32_t, render::FlareSunFixes>> m_busy;
    std::uint64_t m_generation = 0;
    std::map<std::uint32_t, Bucket> m_models;
    bool m_loggedModel = false;
    std::array<bool, 6> m_loggedReason{};  ///< One per Reason (FlareStage.cpp).

    /// Latest model for WP-SEAM's carve (thread-safe on its own).
    render::FlareSeamPenalty m_penalty;

    /// Models kept per bucket: one per frame of a bucket covers a pan.
    static constexpr std::size_t kMaxModelsPerBucket = 8;
    /// Buckets kept (lowest dropped first, never the one being stored).
    static constexpr std::size_t kMaxBuckets = 64;
    /// How far (in buckets) a frame looks for a model that fits its sun
    /// before it needs its own measurement (the parallax grid's range).
    static constexpr std::uint32_t kBorrowBuckets = 4;
};

}  // namespace osv::premiere
