// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SteadyStage: the importer's per-clip analyses - the lens rotation fit
// (Source Settings "Lens Alignment", osv/render/LensAlign.h) and the steady
// seam corrections ("Parallax Grid", osv/render/ClipSteady.h) - measured once
// per clip, off the render thread, and shared by every instance of the clip.
//
// WHEN
// ----
// A clip's first non-draft frame hands the stage its request; a background
// worker then decodes the clip's FIXED sample frames with a short-lived
// decoder of its own and measures:
//
//   1. the lens rotation (Lens Alignment Auto): 3 frames at 10 / 50 / 90 % of
//      the clip, through the calibration rig;
//   2. the clip correction (Parallax Grid Steady / Auto): 9 frames spread over
//      the clip, through the rig with the rotation folded in - the steady
//      grid, seam table and carved seam, and the Auto verdict.
//
// Interactive frames never wait: until a piece is ready they render with what
// exists (the calibration rig, the nearest sample grid measured so far) and
// are marked non-exact.  Exact frames (export, a paused frame, every frame of
// the effect's direct path) wait for it - once per clip, since it is then
// ready for every later frame.  On the sample clip (D3D11VA decode, GPU flow)
// the rotation takes ~0.17 s and the clip correction ~0.35 s.
//
// CACHES
// ------
// Both results depend only on the clip file and on settings, never on which
// frames Premiere asked for, so they are cached process-wide and every
// instance of a clip - the Source monitor's, the new one Premiere opens on
// each Source Settings change, the engine's for the direct path - gets them
// the moment they exist:
//
//   * the rotation, keyed by file identity (path, size, write time) and the
//     calibration rig, in memory and on disk (lens-alignment.tsv next to the
//     plug-in log, like the lens-protector guard's cache): a project reopened
//     next week does not measure again;
//   * the clip correction, keyed by file identity, the rig (with the
//     rotation) and every setting it depends on, in memory (a few hundred
//     KB each, the last 16 kept).
//
// Two instances asking for the same result at once measure it once: the
// second waits for the first (and measures itself if the first is
// cancelled).
//
// LOCKS
// -----
// The stage has its own mutex; the worker takes only it and the process-wide
// cache mutex, never the instance lock, so the instance may stop() it while
// holding its own lock (releaseHeavy does), and an Exact frame may wait on it
// with the instance lock held.
#pragma once

#include "osv/core/Math.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/meta/FormatInfo.h"
#include "osv/render/ClipSteady.h"
#include "osv/render/LensAlign.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace osv::premiere {

/// The lens rotation verdict for one clip file and one calibration rig.
struct LensAlignVerdict {
    bool accepted = false;  ///< `wRad` is the clip's rotation; false: keep the calibration.
    Vec3d wRad;             ///< Body-frame rotation vector for render::applyLensRotation().
    double angleDeg = 0.0;  ///< |w| in degrees.
    double residualDeg = 0.0;  ///< RMS residual of the agreeing cells (full disparity, degrees).
    std::string summary;    ///< The log line's words: the fit, or why there is none.
    bool fromCache = false; ///< Served from the memory or disk cache.
    double millis = 0.0;    ///< Wall time of the measurement (0 for a cache hit).
};

/// A remembered verdict for `path` measured through `baseRig` (memory, then
/// the disk cache, loaded on first use); nullopt when none is known yet.
/// Cheap after the first call; never throws, never measures.
[[nodiscard]] std::optional<LensAlignVerdict> cachedLensAlign(const std::filesystem::path& path,
                                                              const geom::LensRig& baseRig) noexcept;

/// Name of the on-disk rotation cache (inside the plug-in log directory).
inline constexpr const wchar_t* kLensAlignCacheFile = L"lens-alignment.tsv";

/// Everything one clip's per-clip analyses depend on.
struct SteadyRequest {
    std::filesystem::path path;               ///< The clip file.
    meta::FormatInfo format;                  ///< For the stage's own decoder.
    std::uint32_t frameCount = 0;             ///< Frames in the clip.
    std::vector<std::uint32_t> syncFrames;    ///< 0-based sync frames (render::clipSampleFrames).
    geom::LensRig baseRig;                    ///< The calibration rig (after any protector fold), no rotation.
    geom::BlendParams blend;                  ///< The ANALYSIS blend.
    /// The container-sample feed is usable (the importer's own reader's
    /// test): the stage's decoder then numbers frames exactly as it does.
    bool containerSamples = false;
    bool wantRotation = false;                ///< Lens Alignment Auto: fit (or look up) the rotation.
    bool wantClip = false;                    ///< Parallax Grid Steady / Auto: the clip correction.
    /// The clip correction's parameters.  `parallax.requiredImprovement` is
    /// the gate for the calibration rig: the stage replaces it with
    /// render::kAlignedRequiredImprovement when a rotation is folded in, as
    /// the importer's per-bucket grids do.
    render::ClipSteadyParams clip;
};

/// The per-clip analyses of one instance: request, state, worker.
class SteadyStage {
public:
    SteadyStage() = default;
    ~SteadyStage();
    SteadyStage(const SteadyStage&) = delete;
    SteadyStage& operator=(const SteadyStage&) = delete;

    /// Work for `request` from now on.  A request equal to the current one
    /// changes nothing; a different one drops what was published for the
    /// old one (a stale job is cancelled between samples) and starts over.
    /// Serves what the process-wide caches already hold at once, and starts
    /// the worker only for what is missing.  Never blocks on a measurement.
    /// `clipName` is for the log.
    void request(const SteadyRequest& request, const std::string& clipName);

    /// What the current request has produced so far.
    struct Snapshot {
        /// Bumps whenever any published piece changes (a frame cached before
        /// the bump may be stale).
        std::uint64_t serial = 0;
        bool active = false;           ///< A request is in force.
        bool rotationSettled = false;  ///< The rotation question is answered (wantRotation only).
        std::optional<LensAlignVerdict> rotation;  ///< The answer; empty when the measurement failed.
        bool clipSettled = false;      ///< The clip correction is answered (wantClip only).
        std::shared_ptr<const render::ClipSteady> clip;  ///< The answer; null when the measurement failed.
        /// Sample grids measured so far (frame, grid; null = refused), for
        /// stand-ins while the clip correction is not settled.
        std::vector<std::pair<std::uint32_t, std::shared_ptr<const render::ParallaxWarpGrid>>> samples;
        std::string failure;           ///< Why a measurement failed (empty when none did).
    };
    [[nodiscard]] Snapshot snapshot() const;

    /// The serial of the published state (Snapshot::serial), lock-free.
    [[nodiscard]] std::uint64_t serial() const noexcept { return m_serial.load(std::memory_order_acquire); }

    /// Block until the current request's rotation - and, with `clip`, its clip
    /// correction - is settled, a measurement failed, or `timeout` passed.
    /// True when settled.  Safe with the instance lock held.
    bool waitSettled(bool clip, std::chrono::milliseconds timeout);

    /// Forget the current request (the instance's settings changed); the
    /// worker abandons a job for it between samples.
    void reset();

    /// Stop and join the worker, dropping any job not yet started.  May wait
    /// for one sample's measurement (a few tens of milliseconds).  Leaves the
    /// stage ready for a new request.
    void stop() noexcept;

private:
    /// The worker's body; takes only m_mutex and the cache mutex.
    void workerLoop() noexcept;
    /// Run one request (generation `gen`) to completion or cancellation.
    void runJob(const SteadyRequest& job, const std::string& key, std::uint64_t gen, const std::string& clipName);
    /// Publish under m_mutex if `gen` is still current; bumps the serial.
    template <class Fn>
    void publish(std::uint64_t gen, Fn&& fn);
    /// True when generation `gen` is no longer the one worked for.
    [[nodiscard]] bool stale(std::uint64_t gen) const noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_workCv;     ///< Wakes the worker.
    std::condition_variable m_settledCv;  ///< Wakes waitSettled().
    std::thread m_worker;
    /// Set by stop(); polled lock-free by the measurements' cancellation.
    std::atomic<bool> m_stopFlag{false};

    // ---- the request -------------------------------------------------------------
    std::optional<SteadyRequest> m_request;  ///< Current request (null: none).
    std::string m_key;                       ///< Its identity.
    std::string m_clipName;                  ///< For the log.
    std::atomic<std::uint64_t> m_generation{0};  ///< Bumped by every new request / reset.
    bool m_pending = false;                  ///< The worker has work for the current request.
    std::chrono::steady_clock::time_point m_requestedAt;  ///< For the "ready in" log line.

    // ---- published for the current request -----------------------------------------
    Snapshot m_state;
    std::atomic<std::uint64_t> m_serial{0};
};

}  // namespace osv::premiere
