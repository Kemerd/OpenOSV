// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SceneLightStage: the importer's one per-clip pixel measurement behind
// Source Settings "Scene Light: Auto" - the zenith cap of a clip whose
// metered light says dark (osv/render/SceneLight.h has the policy and the
// numbers).
//
// WHEN
// ----
// Only for a clip whose camera metering is dark: a day clip never gets here,
// so it costs nothing and renders exactly as before.  The clip's first
// non-draft frame hands the stage its request; a background worker then
// decodes the clip's FIXED sample frames (the three the lens rotation fit
// uses, 10 / 50 / 90 % of the clip, snapped to sync frames) with a
// short-lived decoder of its own, shades the levelled zenith cap of each on
// the CPU (a 1024 x ~170 strip) and publishes their median.  Interactive
// frames never wait: until the answer lands they render with the day profile
// and are marked non-exact.  Exact frames (export, a paused frame, osvtool)
// wait for it - once per clip.
//
// CACHE
// -----
// The answer depends only on the clip file and on the request (frames, up
// directions, rig, blend, colour decode), never on which frame a host asked
// for first, so it is cached process-wide: the Source monitor's instance,
// the new one Premiere opens on each Source Settings change and the engine's
// instance for the direct path all share one measurement.  A request being
// measured is marked in flight process-wide, so a second instance of the same
// clip waits for that answer (polling its own cancellation) instead of
// decoding the clip twice; different clips measure side by side, so one
// stuck decode never holds up another clip.  Only real answers are cached: a
// measured cap, or a sky that every sample frame showed to be unusable - a
// frame that would not decode is measured again by the next request.  A file
// whose size or write time cannot be read is still recognised as the same
// request by its own instance, but shares nothing.  No disk cache: the
// measurement is a few hundred milliseconds once per session, for night
// clips only.
//
// LOCKS
// -----
// The stage has its own mutex; the worker takes only it and the process-wide
// cache mutex, never the instance lock, so an Exact frame may wait on it with
// the instance lock held and the instance may destroy the stage while
// holding its own lock.
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/core/Math.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/meta/FormatInfo.h"
#include "osv/render/SceneLight.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace osv::premiere {

/// Everything one clip's sky cap measurement depends on.
struct SceneLightRequest {
    std::filesystem::path path;           ///< The clip file.
    meta::FormatInfo format;              ///< For the stage's own decoder.
    /// The container-sample feed is usable (the importer's own reader's
    /// test): the stage's decoder then numbers frames exactly as it does.
    bool containerSamples = false;
    std::vector<std::uint32_t> frames;    ///< The fixed sample frames, ascending.
    std::vector<Vec3d> upBody;            ///< Gravity-up in body coordinates, one per frame.
    geom::LensRig rig;                    ///< The calibration rig (no lens rotation needed for a cap).
    geom::BlendParams blend;              ///< The analysis blend.
    OsvColorParams linearColor{};         ///< The clip's decode to scene-linear Rec.2020 (transfer Linear).
    render::SkyCapParams params;          ///< How the cap is measured.
};

/// The per-clip sky cap measurement of one instance: request, state, worker.
class SceneLightStage {
public:
    SceneLightStage() = default;
    ~SceneLightStage();
    SceneLightStage(const SceneLightStage&) = delete;
    SceneLightStage& operator=(const SceneLightStage&) = delete;

    /// Work for `request` from now on.  A request equal to the current one
    /// changes nothing; a different one cancels the old job (between sample
    /// frames) and starts over.  Serves a process-wide cached answer at once
    /// and starts the worker only when there is none.  `clipName` is for the
    /// log.  Never throws.
    void request(const SceneLightRequest& request, const std::string& clipName) noexcept;

    /// What the current request has produced so far.
    struct Snapshot {
        bool active = false;             ///< A request is in force.
        bool settled = false;            ///< The measurement finished (successfully or not).
        std::optional<render::SkyCap> cap;  ///< The clip's cap; empty or invalid when nothing could be measured.
        std::string failure;             ///< Why nothing could be measured (empty when the cap is valid).
        double millis = 0.0;             ///< Wall time of the measurement (0 for a cached answer).
        bool fromCache = false;          ///< Served from the process-wide cache.
    };
    [[nodiscard]] Snapshot snapshot() const;

    /// Block until the current request is settled or `timeout` passed.  True
    /// when settled.  Safe with the instance lock held.
    bool waitSettled(std::chrono::milliseconds timeout);

    /// Stop and join the worker.  May wait for one sample frame's measurement
    /// (a worker waiting on another instance's measurement leaves within
    /// 50 ms).  Leaves the stage ready for a new request; called by the
    /// destructor and by the instance's quiet (releaseHeavy).
    void stop() noexcept;

private:
    /// The worker's body for one request (generation `gen`).
    void runJob(SceneLightRequest job, std::string key, std::uint64_t gen, std::string clipName) noexcept;
    /// Publish under m_mutex if `gen` is still current.
    void publish(std::uint64_t gen, const Snapshot& state);
    /// True when generation `gen` is no longer the one worked for.
    [[nodiscard]] bool stale(std::uint64_t gen) const noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_settledCv;  ///< Wakes waitSettled().
    std::thread m_worker;                 ///< The running (or finished, unjoined) job.
    std::atomic<std::uint64_t> m_generation{0};  ///< Bumped by every new request and by stop().
    std::string m_key;                    ///< The current request's identity.
    Snapshot m_state;                     ///< Published for the current request.
};

}  // namespace osv::premiere
