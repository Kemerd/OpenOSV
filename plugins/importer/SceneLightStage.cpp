// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SceneLightStage implementation: the request key, the process-wide answer
// cache, the stage's own short-lived decoder and the background job.  The
// policy is in the header.

#include "SceneLightStage.h"

#include "PluginLog.h"

#include "osv/core/ThreadPool.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <system_error>
#include <utility>

namespace osv::premiere {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t`.
[[nodiscard]] double msSince(Clock::time_point t) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// =============================================================================
//  The request's identity
// =============================================================================

/// 64-bit FNV-1a over the exact bytes of everything the cap depends on.
/// Doubles hash by bit pattern: two rigs that differ in the last bit are two
/// requests (a measurement through one is not the other's).
class Hasher {
public:
    void bytes(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            m_h ^= p[i];
            m_h *= 1099511628211ull;
        }
    }
    void u64(std::uint64_t v) noexcept { bytes(&v, sizeof(v)); }
    void f64(double v) noexcept {
        // -0.0 and +0.0 are the same value; one bit pattern for both.
        if (v == 0.0) {
            v = 0.0;
        }
        bytes(&v, sizeof(v));
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return m_h; }

private:
    std::uint64_t m_h = 1469598103934665603ull;
};

/// The request's identity: the clip FILE (absolute path, size, write time -
/// an edited file is another clip), the frames and their up directions, the
/// rig, the blend, the colour decode and the cap parameters.  Empty when the
/// file cannot be inspected (then nothing is cached and the job still runs).
[[nodiscard]] std::string requestKey(const SceneLightRequest& r) noexcept {
    try {
        Hasher h;
        // ---- the file ------------------------------------------------------------
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(r.path, ec);
        if (ec) {
            return {};
        }
        std::u8string u8 = abs.u8string();
        for (char8_t& c : u8) {
            // Windows paths ignore case; one key per file.
            if (c >= u8'A' && c <= u8'Z') {
                c = static_cast<char8_t>(c - u8'A' + u8'a');
            }
        }
        h.bytes(u8.data(), u8.size());
        const std::uintmax_t size = std::filesystem::file_size(abs, ec);
        if (ec) {
            return {};
        }
        const auto mtime = std::filesystem::last_write_time(abs, ec);
        if (ec) {
            return {};
        }
        h.u64(static_cast<std::uint64_t>(size));
        h.u64(static_cast<std::uint64_t>(mtime.time_since_epoch().count()));
        h.u64(r.containerSamples ? 1u : 0u);

        // ---- the frames and how they are levelled -----------------------------------
        h.u64(r.frames.size());
        for (const std::uint32_t f : r.frames) {
            h.u64(f);
        }
        for (const Vec3d& u : r.upBody) {
            h.f64(u.x);
            h.f64(u.y);
            h.f64(u.z);
        }

        // ---- the rig and the blend ------------------------------------------------------
        for (std::size_t i = 0; i < 2; ++i) {
            const geom::KannalaBrandt5& L = r.rig.lens[i];
            for (const double v : {L.fx, L.fy, L.cx, L.cy, L.thetaMaxRad}) {
                h.f64(v);
            }
            for (const double k : L.k) {
                h.f64(k);
            }
            for (const double m : r.rig.bodyToLens[i].m) {
                h.f64(m);
            }
            h.u64(r.rig.occlusionPolyStream[i].size());
        }
        h.u64(static_cast<std::uint64_t>(r.rig.streamW));
        h.u64(static_cast<std::uint64_t>(r.rig.streamH));
        for (const double v : {r.blend.lensFovDeg, r.blend.featherDeg, r.blend.occlusionFeatherPx, r.blend.seamShiftDeg}) {
            h.f64(v);
        }
        h.u64(r.blend.useOcclusionMask ? 1u : 0u);

        // ---- the colour decode and the cap's parameters ----------------------------------
        // A plain C block of ints and floats, filled field by field from a
        // zeroed block by makeColorParams: its bytes are its value.
        h.bytes(&r.linearColor, sizeof(r.linearColor));
        const render::SkyCapParams& p = r.params;
        h.u64(p.equirectW);
        for (const double v : {p.capMinElevationDeg, p.greyLinear, p.sourceStopsAboveGrey, p.sourceExclusionDeg,
                               p.flatNoiseMultiple, p.flatFloorStops, p.minFlatFraction, p.minUsableFraction,
                               p.minAlpha}) {
            h.f64(v);
        }
        return std::format("s1|{:016x}", h.value());
    } catch (...) {
        return {};
    }
}

// =============================================================================
//  Process-wide answers
// =============================================================================

/// Answers kept in memory: a handful of doubles each, so a long session's
/// worth costs nothing; the bound only stops a pathological project growing
/// it forever.
constexpr std::size_t kMaxCachedAnswers = 256;

/// Everything shared between instances.  `measure` is held for a whole
/// measurement, so two instances of the same clip decode it once (the second
/// finds the first's answer when it gets the mutex).
struct Global {
    std::mutex cacheMutex;
    std::map<std::string, SceneLightStage::Snapshot> answers;
    std::mutex measure;
};

Global& global() {
    static Global g;
    return g;
}

/// The cached answer for `key`, if any.
[[nodiscard]] std::optional<SceneLightStage::Snapshot> cachedAnswer(const std::string& key) {
    if (key.empty()) {
        return std::nullopt;
    }
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.cacheMutex);
    const auto it = g.answers.find(key);
    if (it == g.answers.end()) {
        return std::nullopt;
    }
    SceneLightStage::Snapshot s = it->second;
    s.fromCache = true;
    s.millis = 0.0;
    return s;
}

/// Remember `answer` for `key` (a full cache drops its oldest-keyed entry).
void storeAnswer(const std::string& key, const SceneLightStage::Snapshot& answer) {
    if (key.empty()) {
        return;
    }
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.cacheMutex);
    if (g.answers.size() >= kMaxCachedAnswers && g.answers.find(key) == g.answers.end()) {
        g.answers.erase(g.answers.begin());
    }
    g.answers[key] = answer;
}

// =============================================================================
//  The stage's own decoder
// =============================================================================

/// A short-lived host-frame reader for the sample frames: D3D11VA copied back
/// (two decoder threads), else software; a hardware failure on a frame is
/// retried once in software.  It never touches the instance's own decoders,
/// so it can run beside them.
class JobReader {
public:
    JobReader(std::filesystem::path path, meta::FormatInfo format, bool containerSamples)
        : m_path(std::move(path)), m_format(std::move(format)), m_containerSamples(containerSamples) {}

    [[nodiscard]] Result<video::FramePair> read(std::uint32_t frame) {
        if (!m_reader) {
            OSV_TRY(open(false));
        }
        auto pair = m_reader->read(frame);
        if (pair.ok() || m_software) {
            return pair;
        }
        // A hardware failure: software can still decode the frame.
        PluginLog::debug("scene light: hardware decode of frame {} failed ({}); retrying in software", frame,
                         pair.error().message);
        m_reader.reset();
        OSV_TRY(open(true));
        return m_reader->read(frame);
    }

private:
    [[nodiscard]] Status open(bool softwareOnly) {
        Error last{ErrorCode::Decoder, "no decoder could be opened"};
        for (const video::HwAccel hw : {video::kHostFrameHwAccel, video::HwAccel::None}) {
            if (softwareOnly && hw != video::HwAccel::None) {
                continue;
            }
            // Container samples first (frame index == sample index, exactly
            // the importer's own reader), libavformat second.
            for (const bool samples : {m_containerSamples, false}) {
                video::DecoderOptions opt;
                opt.hw = hw;
                opt.threads = hw == video::HwAccel::None ? 0 : 2;
                opt.keepOnDevice = false;  // the cap is shaded on the CPU from host planes
                opt.useContainerSamples = samples;
                opt.shareHwDevice = true;
                opt.deferFirstFrame = true;
                auto reader = video::DualStreamReader::open(m_path, m_format, opt);
                if (reader.ok()) {
                    m_reader = std::make_unique<video::DualStreamReader>(std::move(reader).value());
                    m_software = hw == video::HwAccel::None;
                    return okStatus();
                }
                last = reader.error();
                if (!samples) {
                    break;
                }
            }
        }
        return last;
    }

    std::filesystem::path m_path;
    meta::FormatInfo m_format;
    bool m_containerSamples = false;
    bool m_software = false;
    std::unique_ptr<video::DualStreamReader> m_reader;
};

/// Threads of the job's private pool: a background task should not take the
/// whole machine from the frame renders it exists to stay out of the way of.
[[nodiscard]] unsigned jobThreads() noexcept {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw / 4u, 2u, 8u);
}

}  // namespace

// =============================================================================
//  SceneLightStage
// =============================================================================
SceneLightStage::~SceneLightStage() { stop(); }

void SceneLightStage::request(const SceneLightRequest& request, const std::string& clipName) noexcept {
    try {
        // ---- validate: one up direction per frame, at least one frame ------------
        if (request.frames.empty() || request.frames.size() != request.upBody.size()) {
            PluginLog::warn("scene light: '{}': a request without matching frames and up directions; ignored",
                            clipName);
            return;
        }
        const std::string key = requestKey(request);

        // ---- the same request: nothing to do ------------------------------------------
        std::thread previous;
        std::uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state.active && !key.empty() && key == m_key) {
                return;
            }
            // A new request: the old job (if any) is abandoned between frames.
            gen = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_key = key;
            m_state = Snapshot{};
            m_state.active = true;
            previous = std::move(m_worker);
        }
        // Joined outside the lock: the old job publishes under it.
        if (previous.joinable()) {
            previous.join();
        }

        // ---- a cached answer: publish it at once ---------------------------------------
        if (std::optional<Snapshot> hit = cachedAnswer(key)) {
            publish(gen, *hit);
            return;
        }

        // ---- otherwise measure in the background -----------------------------------------
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stale(gen)) {
            return;  // stop() or another request came in meanwhile
        }
        m_worker = std::thread(&SceneLightStage::runJob, this, request, key, gen, clipName);
    } catch (const std::exception& e) {
        // A thread that cannot start, or no memory for the request copy: the
        // clip stays on the day profile, which is today's behaviour.
        PluginLog::warn("scene light: '{}': the sky measurement could not start ({}); day profile", clipName,
                        e.what());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.settled = true;
        m_state.failure = e.what();
        m_settledCv.notify_all();
    } catch (...) {
        PluginLog::warn("scene light: '{}': the sky measurement could not start; day profile", clipName);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.settled = true;
        m_state.failure = "the measurement could not start";
        m_settledCv.notify_all();
    }
}

SceneLightStage::Snapshot SceneLightStage::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool SceneLightStage::waitSettled(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    // Nothing requested is "settled" in the only sense a caller cares about:
    // there is nothing to wait for.
    m_settledCv.wait_for(lock, timeout, [this] { return !m_state.active || m_state.settled; });
    return !m_state.active || m_state.settled;
}

void SceneLightStage::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        worker = std::move(m_worker);
        m_key.clear();
        m_state = Snapshot{};
    }
    m_settledCv.notify_all();
    if (worker.joinable()) {
        try {
            worker.join();
        } catch (...) {
            // join only throws for a thread that is not joinable, checked above.
        }
    }
}

bool SceneLightStage::stale(std::uint64_t gen) const noexcept {
    return m_generation.load(std::memory_order_acquire) != gen;
}

void SceneLightStage::publish(std::uint64_t gen, const Snapshot& state) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stale(gen)) {
            return;
        }
        m_state = state;
        m_state.active = true;
        m_state.settled = true;
    }
    m_settledCv.notify_all();
}

void SceneLightStage::runJob(SceneLightRequest job, std::string key, std::uint64_t gen,
                             std::string clipName) noexcept {
    try {
        const auto t0 = Clock::now();
        Snapshot result;
        {
            // ---- one measurement at a time, process-wide ------------------------------------
            std::unique_lock<std::mutex> measureLock(global().measure);
            if (stale(gen)) {
                return;
            }
            // Another instance of the clip may have measured it while this
            // job waited for the mutex.
            if (std::optional<Snapshot> hit = cachedAnswer(key)) {
                measureLock.unlock();
                publish(gen, *hit);
                return;
            }

            // ---- decode and measure each sample frame -----------------------------------------
            ThreadPool pool(jobThreads());
            JobReader reader(job.path, job.format, job.containerSamples);
            std::vector<render::SkyCap> caps;
            caps.reserve(job.frames.size());
            std::string lastFailure;
            for (std::size_t i = 0; i < job.frames.size(); ++i) {
                if (stale(gen)) {
                    return;  // abandoned between frames; nothing cached
                }
                const std::uint32_t frame = job.frames[i];
                auto pair = reader.read(frame);
                if (!pair.ok()) {
                    lastFailure = std::format("frame {} could not be decoded ({})", frame, pair.error().message);
                    PluginLog::debug("scene light: '{}': {}", clipName, lastFailure);
                    continue;
                }
                auto cap = render::measureSkyCap(job.rig, pair.value(), job.blend, job.upBody[i], job.linearColor,
                                                 pool, job.params);
                if (!cap.ok()) {
                    lastFailure = std::format("frame {}: {}", frame, cap.error().message);
                    PluginLog::debug("scene light: '{}': {}", clipName, lastFailure);
                    continue;
                }
                const render::SkyCap& c = cap.value();
                PluginLog::debug("scene light: '{}': frame {}: sky {:+.2f} stops, R/G {:+.2f}, B/G {:+.2f}, usable "
                                 "{:.0f} %, flat {:.0f} %, around lights {:.0f} %",
                                 clipName, frame, c.stopsVsGrey, c.log2RG, c.log2BG, 100.0 * c.usableFraction,
                                 100.0 * c.flatFraction, 100.0 * c.sourceFraction);
                caps.push_back(c);
            }

            // ---- the clip's answer: the median over the frames that measured ---------------------
            const render::SkyCap combined = render::combineSkyCaps(caps);
            result.active = true;
            result.settled = true;
            result.cap = combined;
            if (!combined.valid) {
                result.failure = lastFailure.empty() ? std::string("no sample frame could be measured") : lastFailure;
            }
            result.millis = msSince(t0);
            storeAnswer(key, result);
        }
        if (result.cap && result.cap->valid) {
            PluginLog::info("scene light: '{}': sky {:+.2f} stops against metered grey (B/G {:+.2f}) from {} of {} "
                            "sample frames in {:.0f} ms",
                            clipName, result.cap->stopsVsGrey, result.cap->log2BG, result.cap->frames,
                            job.frames.size(), result.millis);
        } else {
            PluginLog::info("scene light: '{}': the sky could not be measured ({}) in {:.0f} ms", clipName,
                            result.failure, result.millis);
        }
        publish(gen, result);
    } catch (const std::exception& e) {
        PluginLog::warn("scene light: '{}': the sky measurement failed ({}); day profile", clipName, e.what());
        Snapshot failed;
        failed.failure = e.what();
        try {
            publish(gen, failed);
        } catch (...) {
            // Publishing copies a short string; nothing more can be done.
        }
    } catch (...) {
        PluginLog::warn("scene light: '{}': the sky measurement failed; day profile", clipName);
        try {
            // Settle it anyway, so an Exact frame waiting on it does not sit
            // out its whole timeout.
            Snapshot failed;
            failed.failure = "the measurement failed";
            publish(gen, failed);
        } catch (...) {
            // Nothing more can be done; the waiter's timeout ends the wait.
        }
    }
}

}  // namespace osv::premiere
