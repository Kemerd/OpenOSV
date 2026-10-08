// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareStage.cpp - the importer's sun ghost removal schedule (FlareStage.h).

#include "FlareStage.h"

#include "PluginLog.h"

#include "osv/render/ParallaxWarp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace osv::premiere {

namespace {

/// Reasons a frame goes without removal, one log line each per reset.
enum Reason : int {
    kReasonOff = 0,       ///< Switched off in Source Settings.
    kReasonNoSun = 1,     ///< No sun in either lens.
    kReasonCheck = 2,     ///< The sun check itself failed.
    kReasonAnalysis = 3,  ///< The analysis failed.
    kReasonPassthrough = 4,  ///< D-Log M passthrough output: the kernel cannot remove.
    kReasonDark = 5,      ///< The scene metered too dark for the sun to be in view.
};

/// "master" / "slave" for a lens index.
[[nodiscard]] const char* lensName(std::size_t i) noexcept { return i == 1 ? "master" : "slave"; }

/// One lens of a model for the log: where the sun is and what was removed.
[[nodiscard]] std::string describeLens(const render::LensFlare& lf, std::size_t i) {
    if (!lf.sunFound) {
        return std::format("{} lens: no sun", lensName(i));
    }
    std::string s = std::format("{} lens: sun {:.1f} deg off axis, ", lensName(i), lf.sunThetaRad * 180.0 / kPi);
    if (lf.ghosts.empty()) {
        s += std::format("no reflection passed the checks ({} candidates)", lf.candidates);
        return s;
    }
    s += std::format("{} ghost{} removed (", lf.ghosts.size(), lf.ghosts.size() == 1 ? "" : "s");
    for (std::size_t k = 0; k < lf.ghosts.size(); ++k) {
        const render::FlareGhost& g = lf.ghosts[k];
        s += std::format("{}+{:.0f}% at ({:.0f}, {:.0f})", k ? ", " : "", 100.0 * g.contrast, g.cx, g.cy);
    }
    s += ")";
    return s;
}

/// Keep at most `limit` buckets, dropping the lowest keys first but never
/// `keep` (the bucket just written) - the same policy as the importer's
/// other analysis caches.
template <class MapT>
void trimBuckets(MapT& cache, std::size_t limit, std::uint32_t keep) {
    while (cache.size() > limit && !cache.empty()) {
        auto victim = cache.begin();
        if (victim->first == keep) {
            victim = std::prev(cache.end());
            if (victim->first == keep) {
                break;
            }
        }
        cache.erase(victim);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Lifetime
// ---------------------------------------------------------------------------

FlareStage::~FlareStage() { stop(); }

void FlareStage::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_pending.reset();
    }
    m_cv.notify_all();
    // The worker never takes the instance lock, so joining under it is safe.
    // It may wait for one in-flight analysis: the fit has no cancellation
    // point, and abandoning the thread would leave it touching this object.
    if (m_worker.joinable()) {
        try {
            m_worker.join();
        } catch (...) {
            // join() only throws for a non-joinable thread or a self-join;
            // neither can happen here, and a noexcept function must not
            // let it escape if the library disagrees.
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = false;
}

void FlareStage::reset() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_models.clear();
        m_pending.reset();
        ++m_generation;
        m_loggedModel = false;
        m_loggedReason.fill(false);
    }
    m_penalty.clear();
}

render::SeamPenaltyHook FlareStage::seamPenalty() noexcept {
    render::SeamPenaltyHook hook;
    hook.fn = &render::FlareSeamPenalty::hook;
    hook.user = &m_penalty;
    hook.weight = 1.0;
    return hook;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

FlareStage::EntryPtr FlareStage::bestMatch(const std::vector<EntryPtr>& entries, const render::FlareSunFixes& suns,
                                           double tolerancePx) noexcept {
    EntryPtr best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const EntryPtr& e : entries) {
        if (!e || !render::flareSunsMatch(e->suns, suns, tolerancePx)) {
            continue;
        }
        // Among the usable ones, the closest sun: summed over the lenses.
        double d = 0.0;
        for (std::size_t i = 0; i < 2; ++i) {
            if (suns[i].found) {
                d += std::hypot(e->suns[i].x - suns[i].x, e->suns[i].y - suns[i].y);
            }
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = e;
        }
    }
    return best;
}

void FlareStage::storeLocked(std::uint32_t bucket, const EntryPtr& entry) {
    if (!entry) {
        return;
    }
    std::vector<EntryPtr>& list = m_models[bucket].entries;
    if (std::find(list.begin(), list.end(), entry) != list.end()) {
        return;  // adopted before
    }
    list.push_back(entry);
    // The oldest measurement of the bucket goes first: a pan that has moved
    // on will not come back to it soon.
    while (list.size() > kMaxModelsPerBucket) {
        list.erase(list.begin());
    }
    trimBuckets(m_models, kMaxBuckets, bucket);
}

void FlareStage::assignLocked(std::uint32_t bucket, std::uint32_t frame, const EntryPtr& entry) {
    Bucket& b = m_models[bucket];
    b.frames[frame] = entry;
    b.checked.erase(frame);  // answered: its pending sun check is no longer needed
    trimBuckets(m_models, kMaxBuckets, bucket);
}

FlareStage::EntryPtr FlareStage::findUsableLocked(std::uint32_t bucket, const render::FlareSunFixes& suns,
                                                  double tolerancePx) const {
    // A model describes every frame whose sun is where the model's frame had
    // it, whichever bucket that frame is in: this bucket's own models first,
    // then its neighbours' (nearest first, earlier first).
    EntryPtr own;
    if (const auto it = m_models.find(bucket); it != m_models.end()) {
        own = bestMatch(it->second.entries, suns, tolerancePx);
    }
    for (std::uint32_t d = 1; d <= kBorrowBuckets && !own; ++d) {
        for (const std::uint32_t b : {bucket >= d ? bucket - d : bucket, bucket + d}) {
            if (b == bucket || own) {
                continue;
            }
            if (const auto it = m_models.find(b); it != m_models.end()) {
                own = bestMatch(it->second.entries, suns, tolerancePx);
            }
        }
    }
    return own;
}

Result<render::FlareModel> FlareStage::analyse(const std::array<render::FlareImage, 2>& images,
                                               const std::array<geom::KannalaBrandt5, 2>& lenses,
                                               const render::FlareSunFixes& suns, const render::FlareParams& params,
                                               ThreadPool* pool) {
    render::FlareModel model;
    for (std::size_t i = 0; i < 2; ++i) {
        // Only where the (one) sun is: a lens the sun check found none in,
        // or whose blob the one-sun rule dropped, stays empty.
        if (!suns[i].found) {
            continue;
        }
        OSV_TRY_ASSIGN(model.lens[i], render::analyseLensFlare(images[i], lenses[i], params, pool));
    }
    return model;
}

double FlareStage::sceneEv100(const meta::MetadataTrack& track, std::uint32_t index) noexcept {
    constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();
    try {
        // ---- the clip's aperture: a [num, den] rational ([19, 10] = f/1.9) --
        const std::vector<std::uint32_t>& fn = track.clip().fNumber;
        if (fn.size() < 2 || fn[0] == 0 || fn[1] == 0) {
            return kUnknown;
        }
        // ---- the frame's ISO and shutter (a [num, den] rational in seconds) --
        const Result<meta::FrameMeta> frame = track.frame(index);
        if (!frame.ok()) {
            return kUnknown;
        }
        const meta::CameraFrame& camera = frame.value().camera;
        const std::vector<std::int32_t>& et = camera.exposureTime;
        if (et.size() < 2 || et[0] <= 0 || et[1] <= 0) {
            return kUnknown;
        }
        // flareSceneEv100 refuses anything not finite and positive.
        return render::flareSceneEv100(static_cast<double>(fn[0]) / static_cast<double>(fn[1]),
                                       static_cast<double>(et[0]) / static_cast<double>(et[1]),
                                       static_cast<double>(camera.iso));
    } catch (...) {
        // Copying the frame's metadata can only fail on allocation: unknown.
        return kUnknown;
    }
}

void FlareStage::logModelOnce(const std::string& clip, std::uint32_t frame, const render::FlareModel& model,
                              double sceneEv100) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_loggedModel) {
                return;
            }
            m_loggedModel = true;
        }
        // The brightness last: the lens descriptions keep their place in the
        // line, and an unknown value says so rather than printing "nan".
        PluginLog::info("flare: '{}' frame {}: {}; {}; {}", clip, frame, describeLens(model.lens[1], 1),
                        describeLens(model.lens[0], 0),
                        std::isfinite(sceneEv100) ? std::format("scene metered at EV100 {:.1f}", sceneEv100)
                                                  : std::string("scene brightness not recorded"));
    } catch (...) {
        // Formatting can only fail on allocation; the log line is optional.
    }
}

void FlareStage::logReasonOnce(int reason, const std::string& text) noexcept {
    if (reason < 0 || reason >= static_cast<int>(m_loggedReason.size())) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_loggedReason[static_cast<std::size_t>(reason)]) {
            return;
        }
        m_loggedReason[static_cast<std::size_t>(reason)] = true;
    }
    PluginLog::info("{}", text);
}

// ---------------------------------------------------------------------------
//  Per frame
// ---------------------------------------------------------------------------

FlareStage::Decision FlareStage::decide(std::uint32_t index, const video::FramePair* pair, const geom::LensRig& rig,
                                        const OsvColorParams& color, double sceneEv100, bool enabled, bool draft,
                                        bool exactWanted, ThreadPool& pool, const std::string& clip) noexcept {
    // The answer starts as "settled, nothing to remove"; every early return
    // below keeps it (apply() then clears the carve's hook, as before).
    Decision d;
    Outcome& out = d.outcome;
    try {
        // ---- wanted at all? ------------------------------------------------
        if (!enabled || draft) {
            // A frame without removal must not steer the seam either.
            if (!enabled) {
                logReasonOnce(kReasonOff, std::format("flare: '{}': sun ghost removal is off in Source Settings",
                                                      clip));
            }
            return d;
        }
        // The D-Log M passthrough output blends in log code, where the kernel
        // has no linear light to subtract from (osv_kernel.h skips the
        // removal there), so a measurement would cost an analysis per sun
        // position and change nothing.
        if (color.transfer == OSV_TRANSFER_PASSTHROUGH) {
            logReasonOnce(kReasonPassthrough,
                          std::format("flare: '{}': the D-Log M passthrough output is not treated (it blends in log "
                                      "code); rendering without ghost removal",
                                      clip));
            return d;
        }
        // The frame's exposure travels with the parameters, so the sun check
        // and the fits (here or on the worker) also hold the image's own
        // median to it (FlareParams::minSceneLuminance).
        const render::FlareParams params = [sceneEv100] {
            render::FlareParams p;
            p.sceneEv100 = sceneEv100;
            return p;
        }();

        // ---- can the sun be in view at all? ------------------------------------
        // The camera's exposure says how much light the scene had; a night
        // street's clipped lamps pass every image test for "the sun", this
        // one they cannot.  Before the cache and the sun check, so a dark
        // frame costs nothing and nothing measured earlier reaches it.  The
        // value is the frame's own metadata, so the answer is final (exact).
        if (render::flareSceneTooDark(sceneEv100, params)) {
            logReasonOnce(kReasonDark,
                          std::format("flare: '{}': scene metered at EV100 {:.1f} at frame {}, too dark for the sun "
                                      "to be in view; nothing to remove",
                                      clip, sceneEv100, index));
            return d;
        }
        const std::uint32_t bucket = render::parallaxBucket(index);
        const double tolerance = render::flareSunTolerancePx(static_cast<std::uint32_t>(std::max(rig.streamW, 0)));

        // ---- a frame already answered keeps its answer -------------------------
        // A look-up without the frame (pair null) may also answer it from the
        // sun check an earlier Interactive render of it recorded: a model
        // that fits that sun now is adopted exactly as that render would
        // adopt it on its next request.
        {
            std::optional<EntryPtr> known;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (const auto it = m_models.find(bucket); it != m_models.end()) {
                    if (const auto f = it->second.frames.find(index); f != it->second.frames.end()) {
                        known = f->second;
                    } else if (pair == nullptr) {
                        if (const auto c = it->second.checked.find(index); c != it->second.checked.end()) {
                            const render::FlareSunFixes recorded = c->second;
                            if (EntryPtr fit = findUsableLocked(bucket, recorded, tolerance)) {
                                // Adopt it into this bucket and give it to the frame for good.
                                storeLocked(bucket, fit);
                                assignLocked(bucket, index, fit);
                                known = fit;
                            }
                        }
                    }
                }
            }
            if (known) {
                d.entry = *known;  // null: no sun in this frame
                return d;
            }
        }
        if (pair == nullptr) {
            // A look-up with no answer: not settled, nothing decoded or measured.
            out.exact = false;
            return d;
        }

        // ---- where is the sun in this frame? --------------------------------
        const auto tCheck = std::chrono::steady_clock::now();
        auto checked = render::locateSuns(rig, *pair, color, params, pool);
        if (!checked.ok()) {
            logReasonOnce(kReasonCheck,
                          std::format("flare: '{}': the sun check failed ({}); rendering without ghost removal", clip,
                                      checked.error().message));
            return d;
        }
        const render::FlareSunFixes suns = checked.value();
        const double checkMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCheck).count();
        if (!suns[0].found && !suns[1].found) {
            // Nothing can have a ghost: the frame is final as it is.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                assignLocked(bucket, index, nullptr);
            }
            logReasonOnce(kReasonNoSun,
                          std::format("flare: '{}': no sun in either lens at frame {}; nothing to remove", clip, index));
            return d;
        }

        // ---- what is known ------------------------------------------------------
        // A model describes every frame whose sun is where the model's frame
        // had it, whichever bucket that frame is in.  So this bucket's own
        // models first, then its neighbours' (nearest first, earlier first);
        // a neighbour's model that fits is adopted INTO this bucket, so every
        // later render of this bucket - Interactive or Exact - subtracts the
        // same thing.  A steady shot therefore measures once, not once per
        // bucket; a pan measures wherever the sun has moved on.
        EntryPtr own;
        bool inHand = false;  // the worker already has this bucket at this sun
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            own = findUsableLocked(bucket, suns, tolerance);
            if (own) {
                // Adopt it into this bucket and give it to this frame for good.
                storeLocked(bucket, own);
                assignLocked(bucket, index, own);
            }
            inHand = (m_pending && m_pending->bucket == bucket &&
                      render::flareSunsMatch(m_pending->suns, suns, tolerance)) ||
                     (m_busy && m_busy->first == bucket && render::flareSunsMatch(m_busy->second, suns, tolerance));
        }

        // ---- measure now (Exact) or hand to the worker (Interactive) -----------
        if (!own && (exactWanted || !inHand)) {
            const auto t0 = std::chrono::steady_clock::now();
            std::array<render::FlareImage, 2> images;
            bool imagesOk = true;
            for (int i = 0; i < 2 && imagesOk; ++i) {
                // A lens without the sun is not analysed, so it needs no
                // working image (analyse() never reads it).
                if (!suns[static_cast<std::size_t>(i)].found) {
                    continue;
                }
                auto image = render::flareDownsampleLens(*pair, i, color, params.factor, pool);
                if (image.ok()) {
                    images[static_cast<std::size_t>(i)] = std::move(image).value();
                } else {
                    imagesOk = false;
                    logReasonOnce(kReasonAnalysis,
                                  std::format("flare: '{}': the working image failed ({}); rendering without ghost "
                                              "removal",
                                              clip, image.error().message));
                }
            }
            const double sampleMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (imagesOk && exactWanted) {
                // Exact: the whole analysis here, on the render pool.
                auto model = analyse(images, {rig.lens[0], rig.lens[1]}, suns, params, &pool);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (model.ok()) {
                    auto entry = std::make_shared<Entry>();
                    entry->model = std::move(model).value();
                    entry->suns = suns;
                    entry->frame = index;
                    PluginLog::debug("flare: frame {} (bucket {}): measured in {:.0f} ms (sun check {:.1f}, working "
                                     "images {:.1f}); {} + {} ghosts",
                                     index, bucket, ms + checkMs, checkMs, sampleMs,
                                     entry->model.lens[1].ghosts.size(), entry->model.lens[0].ghosts.size());
                    logModelOnce(clip, index, entry->model, params.sceneEv100);
                    own = entry;
                    std::lock_guard<std::mutex> lock(m_mutex);
                    storeLocked(bucket, own);
                    assignLocked(bucket, index, own);
                } else {
                    logReasonOnce(kReasonAnalysis,
                                  std::format("flare: '{}': the analysis failed at frame {} ({}); rendering without "
                                              "ghost removal",
                                              clip, index, model.error().message));
                }
            } else if (imagesOk) {
                // Interactive: the images to the worker, and move on.  One
                // slot, latest wins - while scrubbing only where the user
                // stops matters.
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    Job job;
                    job.bucket = bucket;
                    job.frame = index;
                    job.generation = m_generation;
                    job.suns = suns;
                    job.images = std::move(images);
                    job.lenses = {rig.lens[0], rig.lens[1]};
                    job.params = params;
                    job.clip = clip;
                    m_pending = std::move(job);
                    // Started lazily, under the instance lock that stop()
                    // also runs under, so start and stop never race.
                    if (!m_worker.joinable()) {
                        try {
                            m_worker = std::thread(&FlareStage::workerLoop, this);
                        } catch (const std::exception& e) {
                            m_pending.reset();
                            PluginLog::warn("flare: could not start the background worker ({}); interactive frames "
                                            "render without ghost removal",
                                            e.what());
                        }
                    }
                }
                m_cv.notify_one();
                PluginLog::debug("flare: frame {} (bucket {}): queued for the worker (sun check {:.1f} ms, working "
                                 "images {:.1f} ms)",
                                 index, bucket, checkMs, sampleMs);
            }
        }

        // ---- choose what to subtract ----------------------------------------------
        // No glide between buckets (render::smoothFlare): what a glide
        // blends in depends on which neighbours happen to be cached, and the
        // direct path relies on an Exact frame being the same whatever was
        // rendered before it.  One model per sun position is also what keeps
        // a steady shot's ghosts from shimmering between bucket fits.
        if (own) {
            d.entry = own;
        } else {
            // Nothing measured for this sun yet, and an Interactive request
            // does not wait: the frame goes without removal and is not final.
            out.exact = false;
            if (!exactWanted) {
                // Its sun check, for a look-up that may answer it once a model
                // fitting this sun lands (anchorSeamPenalty).
                std::lock_guard<std::mutex> lock(m_mutex);
                m_models[bucket].checked[index] = suns;
                trimBuckets(m_models, kMaxBuckets, bucket);
            }
        }
        return d;
    } catch (const std::exception& e) {
        // A frame never fails because of the removal.
        PluginLog::warn("flare: frame {}: {}; rendering without ghost removal", index, e.what());
        return Decision{};
    } catch (...) {
        return Decision{};
    }
}

FlareStage::Outcome FlareStage::apply(std::uint32_t index, const video::FramePair& pair, const geom::LensRig& rig,
                                      const OsvColorParams& color, double sceneEv100, bool enabled, bool draft,
                                      bool exactWanted, ThreadPool& pool, render::RenderParamsBuilder& builder,
                                      const std::string& clip) noexcept {
    // The schedule decides; this hands its answer to the frame and to the
    // carve of a seam on this frame (the latest-model hook).
    const Decision d = decide(index, &pair, rig, color, sceneEv100, enabled, draft, exactWanted, pool, clip);
    Outcome out = d.outcome;
    try {
        if (d.entry && d.entry->model.any()) {
            builder.flare(d.entry->model);
            m_penalty.update(d.entry->model, rig);
            out.applied = true;
        } else {
            // No sun, nothing measured yet, or removal not wanted: a frame
            // without removal must not steer the seam either.
            m_penalty.clear();
        }
        return out;
    } catch (const std::exception& e) {
        // A frame never fails because of the removal.
        m_penalty.clear();
        PluginLog::warn("flare: frame {}: {}; rendering without ghost removal", index, e.what());
        return Outcome{};
    } catch (...) {
        m_penalty.clear();
        return Outcome{};
    }
}

bool FlareStage::anchorSeamPenalty(std::uint32_t anchor, const video::FramePair* pair, const geom::LensRig& rig,
                                   const OsvColorParams& color, double sceneEv100, bool enabled, ThreadPool& pool,
                                   const std::string& clip, render::FlareSeamPenalty& penalty) noexcept {
    // With the anchor's frames: answered as an Exact request answers it
    // (measured now when nothing fits).  Without: a look-up only.  Never a
    // draft - the carve does not run for one.
    const Decision d = decide(anchor, pair, rig, color, sceneEv100, enabled, /*draft=*/false,
                              /*exactWanted=*/pair != nullptr, pool, clip);
    try {
        if (d.outcome.exact && d.entry && d.entry->model.any()) {
            penalty.update(d.entry->model, rig);
        } else {
            penalty.clear();  // nothing to steer around, or not settled yet
        }
    } catch (...) {
        // Copying the model or the rig can only fail on allocation: carve
        // without the ghosts rather than with a half-installed model.
        penalty.clear();
    }
    return d.outcome.exact;
}

// ---------------------------------------------------------------------------
//  Background worker
// ---------------------------------------------------------------------------

void FlareStage::workerLoop() noexcept {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || m_pending.has_value(); });
            if (m_stop) {
                return;
            }
            job = std::move(*m_pending);
            m_pending.reset();
            m_busy = std::make_pair(job.bucket, job.suns);
        }

        // The expensive half, with no lock held and no pool: the render pool
        // is shared with the frame renders this worker exists to stay out of
        // the way of (ThreadPool serialises whole jobs).
        std::shared_ptr<Entry> entry;
        std::string refusal;
        double ms = 0.0;
        try {
            const auto t0 = std::chrono::steady_clock::now();
            auto model = analyse(job.images, job.lenses, job.suns, job.params, nullptr);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (model.ok()) {
                entry = std::make_shared<Entry>();
                entry->model = std::move(model).value();
                entry->suns = job.suns;
                entry->frame = job.frame;
            } else {
                refusal = model.error().message;
            }
        } catch (const std::exception& e) {
            refusal = std::string("exception: ") + e.what();
        } catch (...) {
            refusal = "unknown exception";
        }

        bool stored = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_busy.reset();
            // A model measured under settings that have since changed (or a
            // clip that has since been quieted) describes nothing current.
            if (entry && job.generation == m_generation && !m_stop) {
                storeLocked(job.bucket, entry);
                stored = true;
            }
        }
        if (entry) {
            PluginLog::debug("flare: bucket {} (frame {}) measured in the background in {:.0f} ms{}", job.bucket,
                             job.frame, ms, stored ? "" : " - discarded, settings changed");
            if (stored) {
                logModelOnce(job.clip, job.frame, entry->model, job.params.sceneEv100);
            }
        } else {
            PluginLog::debug("flare: bucket {} (frame {}) failed in the background after {:.0f} ms ({})", job.bucket,
                             job.frame, ms, refusal);
        }
    }
}

}  // namespace osv::premiere
