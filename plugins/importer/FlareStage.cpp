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

namespace osv::premiere {

namespace {

/// Reasons a frame goes without removal, one log line each per reset.
enum Reason : int {
    kReasonOff = 0,       ///< Switched off in Source Settings.
    kReasonNoSun = 1,     ///< No sun in either lens.
    kReasonCheck = 2,     ///< The sun check itself failed.
    kReasonAnalysis = 3,  ///< The analysis failed.
    kReasonPassthrough = 4,  ///< D-Log M passthrough output: the kernel cannot remove.
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
    m_models[bucket].frames[frame] = entry;
    trimBuckets(m_models, kMaxBuckets, bucket);
}

Result<render::FlareModel> FlareStage::analyse(const std::array<render::FlareImage, 2>& images,
                                               const std::array<geom::KannalaBrandt5, 2>& lenses,
                                               const render::FlareParams& params, ThreadPool* pool) {
    render::FlareModel model;
    for (std::size_t i = 0; i < 2; ++i) {
        OSV_TRY_ASSIGN(model.lens[i], render::analyseLensFlare(images[i], lenses[i], params, pool));
    }
    return model;
}

void FlareStage::logModelOnce(const std::string& clip, std::uint32_t frame, const render::FlareModel& model) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_loggedModel) {
                return;
            }
            m_loggedModel = true;
        }
        PluginLog::info("flare: '{}' frame {}: {}; {}", clip, frame, describeLens(model.lens[1], 1),
                        describeLens(model.lens[0], 0));
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

FlareStage::Outcome FlareStage::apply(std::uint32_t index, const video::FramePair& pair, const geom::LensRig& rig,
                                      const OsvColorParams& color, bool enabled, bool draft, bool exactWanted,
                                      ThreadPool& pool, render::RenderParamsBuilder& builder,
                                      const std::string& clip) noexcept {
    Outcome out;
    try {
        // ---- wanted at all? ------------------------------------------------
        if (!enabled || draft) {
            // A frame without removal must not steer the seam either.
            m_penalty.clear();
            if (!enabled) {
                logReasonOnce(kReasonOff, std::format("flare: '{}': sun ghost removal is off in Source Settings",
                                                      clip));
            }
            return out;
        }
        // The D-Log M passthrough output blends in log code, where the kernel
        // has no linear light to subtract from (osv_kernel.h skips the
        // removal there), so a measurement would cost an analysis per sun
        // position and change nothing.
        if (color.transfer == OSV_TRANSFER_PASSTHROUGH) {
            m_penalty.clear();
            logReasonOnce(kReasonPassthrough,
                          std::format("flare: '{}': the D-Log M passthrough output is not treated (it blends in log "
                                      "code); rendering without ghost removal",
                                      clip));
            return out;
        }
        const render::FlareParams params;
        const std::uint32_t bucket = render::parallaxBucket(index);

        // ---- a frame already answered keeps its answer -------------------------
        {
            std::optional<EntryPtr> known;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (const auto it = m_models.find(bucket); it != m_models.end()) {
                    if (const auto f = it->second.frames.find(index); f != it->second.frames.end()) {
                        known = f->second;
                    }
                }
            }
            if (known) {
                if (*known && (*known)->model.any()) {
                    builder.flare((*known)->model);
                    m_penalty.update((*known)->model, rig);
                    out.applied = true;
                } else {
                    m_penalty.clear();  // no sun in this frame
                }
                return out;
            }
        }

        // ---- where is the sun in this frame? --------------------------------
        const auto tCheck = std::chrono::steady_clock::now();
        auto checked = render::locateSuns(rig, pair, color, params, pool);
        if (!checked.ok()) {
            m_penalty.clear();
            logReasonOnce(kReasonCheck,
                          std::format("flare: '{}': the sun check failed ({}); rendering without ghost removal", clip,
                                      checked.error().message));
            return out;
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
            m_penalty.clear();
            logReasonOnce(kReasonNoSun,
                          std::format("flare: '{}': no sun in either lens at frame {}; nothing to remove", clip, index));
            return out;
        }
        const double tolerance = render::flareSunTolerancePx(static_cast<std::uint32_t>(std::max(rig.streamW, 0)));

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
            if (const auto it = m_models.find(bucket); it != m_models.end()) {
                own = bestMatch(it->second.entries, suns, tolerance);
            }
            for (std::uint32_t d = 1; d <= kBorrowBuckets && !own; ++d) {
                for (const std::uint32_t b : {bucket >= d ? bucket - d : bucket, bucket + d}) {
                    if (b == bucket || own) {
                        continue;
                    }
                    if (const auto it = m_models.find(b); it != m_models.end()) {
                        own = bestMatch(it->second.entries, suns, tolerance);
                    }
                }
            }
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
                auto image = render::flareDownsampleLens(pair, i, color, params.factor, pool);
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
                auto model = analyse(images, {rig.lens[0], rig.lens[1]}, params, &pool);
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
                    logModelOnce(clip, index, entry->model);
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
        std::optional<render::FlareModel> chosen;
        if (own) {
            chosen = own->model;
        } else {
            // Nothing measured for this sun yet, and an Interactive request
            // does not wait: the frame goes without removal and is not final.
            out.exact = false;
        }

        // ---- hand it over ---------------------------------------------------------
        if (chosen && chosen->any()) {
            builder.flare(*chosen);
            m_penalty.update(*chosen, rig);
            out.applied = true;
        } else {
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
            auto model = analyse(job.images, job.lenses, job.params, nullptr);
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
                logModelOnce(job.clip, job.frame, entry->model);
            }
        } else {
            PluginLog::debug("flare: bucket {} (frame {}) failed in the background after {:.0f} ms ({})", job.bucket,
                             job.frame, ms, refusal);
        }
    }
}

}  // namespace osv::premiere
