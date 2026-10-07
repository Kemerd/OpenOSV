// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ClipTimeline: a variable-frame-rate clip on a constant-rate timeline at its
// nominal rate (see the header for the why).  Everything above the readers
// is a pure function of its arguments; the readers only gather those
// arguments from an opened file.

#include "osv/video/ClipTimeline.h"

#include "osv/container/TrackInfo.h"
#include "osv/core/Log.h"
#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

namespace osv::video {

namespace {

/// `ticks` at `timescale` in whole microseconds, rounded to nearest, without
/// the precision loss a double product would have on a long clip: the whole
/// seconds and the remainder are scaled separately (the remainder is below
/// the timescale, so remainder * 1e6 fits easily in 64 bits).
[[nodiscard]] std::int64_t ticksToUs(std::uint64_t ticks, std::uint32_t timescale) noexcept {
    if (timescale == 0) {
        return 0;
    }
    const std::uint64_t seconds = ticks / timescale;
    const std::uint64_t remainder = ticks % timescale;
    const std::uint64_t fraction = (remainder * 1000000ull + timescale / 2u) / timescale;
    return static_cast<std::int64_t>(seconds * 1000000ull + fraction);
}

/// Accumulated presentation moments of a duration table, us after sample 0;
/// empty when two samples would share a moment (a zero duration).
[[nodiscard]] std::vector<std::int64_t> containerMoments(std::uint32_t timescale,
                                                         std::span<const std::uint64_t> durations) {
    std::vector<std::int64_t> us;
    if (timescale == 0 || durations.empty()) {
        return us;
    }
    us.reserve(durations.size());
    std::uint64_t ticks = 0;
    for (std::size_t i = 0; i < durations.size(); ++i) {
        const std::int64_t moment = ticksToUs(ticks, timescale);
        // Strictly increasing or nothing: a duplicate moment cannot be
        // ordered, and the caller then has no container clock.
        if (!us.empty() && moment <= us.back()) {
            return {};
        }
        us.push_back(moment);
        // Saturate rather than wrap on a corrupt table.
        const std::uint64_t d = durations[i];
        ticks = (d > std::numeric_limits<std::uint64_t>::max() - ticks) ? std::numeric_limits<std::uint64_t>::max()
                                                                         : ticks + d;
    }
    return us;
}

/// Why `captureUs` cannot be the clock of a track whose container moments
/// are `container` (empty when it can).
[[nodiscard]] std::string captureRejection(std::span<const std::uint64_t> captureUs,
                                           const std::vector<std::int64_t>& container) {
    if (captureUs.empty()) {
        return "no per-frame capture timestamps";
    }
    if (captureUs.size() != container.size()) {
        return std::to_string(captureUs.size()) + " capture timestamps for " + std::to_string(container.size()) +
               " samples";
    }
    // Strictly increasing: a repeated or backwards timestamp is not a clock.
    for (std::size_t i = 1; i < captureUs.size(); ++i) {
        if (captureUs[i] <= captureUs[i - 1]) {
            return "capture timestamps do not increase at sample " + std::to_string(i);
        }
    }
    // The same span as the container, within the tolerance.  A single
    // sample has no span and nothing to disagree about.
    if (container.size() >= 2) {
        const double containerSpan = static_cast<double>(container.back());
        const double captureSpan = static_cast<double>(captureUs.back() - captureUs.front());
        if (!(containerSpan > 0.0)) {
            return "the container gives the samples no span to compare with";
        }
        const double disagreement = std::fabs(captureSpan - containerSpan) / containerSpan;
        if (!(disagreement <= kCaptureSpanTolerance)) {
            char buf[160] = {};
            std::snprintf(buf, sizeof(buf), "capture timestamps span %.3f s against the container's %.3f s (%.1f %%)",
                          captureSpan / 1e6, containerSpan / 1e6, disagreement * 100.0);
            return buf;
        }
    }
    return {};
}

}  // namespace

// =============================================================================
//  Names and small accessors
// =============================================================================

const char* timelineClockName(TimelineClock clock) noexcept {
    switch (clock) {
    case TimelineClock::Identity: return "identity";
    case TimelineClock::Capture: return "capture";
    case TimelineClock::Container: return "container";
    }
    return "identity";
}

std::uint32_t SampleClock::nearestSample(double momentUs) const noexcept {
    // Nothing to choose from, or no moment to choose by.
    if (us.empty() || std::isnan(momentUs)) {
        return 0;
    }
    // Clamp outside the recording: the first or the last sample.
    if (momentUs <= static_cast<double>(us.front())) {
        return 0;
    }
    const std::uint32_t last = static_cast<std::uint32_t>(us.size() - 1u);
    if (momentUs >= static_cast<double>(us.back())) {
        return last;
    }
    // The first sample at or after the moment, and the one before it; the
    // nearer one wins, a tie going to the later (floor(x + 0.5)).
    const auto it = std::lower_bound(us.begin(), us.end(), momentUs,
                                     [](std::int64_t value, double m) { return static_cast<double>(value) < m; });
    const std::size_t above = static_cast<std::size_t>(it - us.begin());
    if (above == 0) {
        return 0;
    }
    const double a = static_cast<double>(us[above - 1u]);
    const double b = static_cast<double>(us[above]);
    return static_cast<std::uint32_t>(2.0 * (momentUs - a) >= (b - a) ? above : above - 1u);
}

std::uint32_t ClipTimeline::sampleFor(std::uint32_t frame) const noexcept {
    // An empty clip has only sample 0 to offer.
    if (sampleCount == 0) {
        return 0;
    }
    const std::uint32_t lastSample = sampleCount - 1u;
    if (identity()) {
        return std::min(frame, lastSample);
    }
    // Past the timeline's end: its last frame.  The table never names a
    // sample outside the clip, but a clamp costs nothing.
    const std::size_t k = std::min<std::size_t>(frame, toSample.size() - 1u);
    return std::min(toSample[k], lastSample);
}

double ClipTimeline::fps() const noexcept {
    if (timescale == 0 || nominalTicks == 0) {
        return 0.0;
    }
    return static_cast<double>(timescale) / static_cast<double>(nominalTicks);
}

double ClipTimeline::durationSeconds() const noexcept {
    if (timescale == 0 || nominalTicks == 0) {
        return 0.0;
    }
    return static_cast<double>(frameCount()) * static_cast<double>(nominalTicks) / static_cast<double>(timescale);
}

double ClipTimeline::frameMomentUs(std::uint32_t frame) const noexcept {
    // A placed timeline: the moment of the sample the frame shows.
    if (!identity() && !sampleUs.empty()) {
        const std::uint32_t s = sampleFor(frame);
        return s < sampleUs.size() ? static_cast<double>(sampleUs[s]) : 0.0;
    }
    // Identity: the frame's own place at the nominal period.
    if (timescale == 0 || nominalTicks == 0) {
        return 0.0;
    }
    return static_cast<double>(frame) * static_cast<double>(nominalTicks) * 1e6 / static_cast<double>(timescale);
}

// =============================================================================
//  The nominal period
// =============================================================================

std::uint64_t nominalSampleDuration(std::span<const std::uint64_t> durations) noexcept {
    // Nothing to vote with.
    if (durations.empty()) {
        return 0;
    }
    // One sample: its own duration is all there is.
    if (durations.size() == 1u) {
        return durations.front();
    }
    // Count each duration between consecutive samples (the last sample's
    // own duration is excluded).  Durations come in long runs, so counting
    // runs keeps the map tiny even on a long recording.
    std::map<std::uint64_t, std::uint64_t> votes;
    const std::size_t between = durations.size() - 1u;
    std::size_t i = 0;
    while (i < between) {
        const std::uint64_t d = durations[i];
        std::size_t j = i + 1u;
        while (j < between && durations[j] == d) {
            ++j;
        }
        if (d > 0) {
            votes[d] += static_cast<std::uint64_t>(j - i);
        }
        i = j;
    }
    // The most common one; std::map iterates shortest first, so a strict
    // '>' leaves a tie with the shorter period.
    std::uint64_t best = 0;
    std::uint64_t bestVotes = 0;
    for (const auto& [duration, count] : votes) {
        if (count > bestVotes) {
            best = duration;
            bestVotes = count;
        }
    }
    return best;
}

bool uniformSampleDurations(std::span<const std::uint64_t> durations, std::uint64_t nominal) noexcept {
    if (nominal == 0) {
        return false;
    }
    // Every sample but the last: its own duration shapes no presentation time.
    for (std::size_t i = 0; i + 1u < durations.size(); ++i) {
        if (durations[i] != nominal) {
            return false;
        }
    }
    return true;
}

// =============================================================================
//  The clocks
// =============================================================================

SampleClock buildSampleClock(std::uint32_t timescale, std::span<const std::uint64_t> durations,
                             std::span<const std::uint64_t> captureUs) {
    SampleClock out;
    // The container's moments are both the fallback and the yardstick the
    // capture clock is checked against.
    std::vector<std::int64_t> container = containerMoments(timescale, durations);
    if (container.empty() && !durations.empty()) {
        // No usable table (zero durations or no timescale): the capture
        // clock can still stand alone when it is one per sample and increases.
        if (captureUs.size() == durations.size() && !captureUs.empty()) {
            bool increasing = true;
            for (std::size_t i = 1; i < captureUs.size() && increasing; ++i) {
                increasing = captureUs[i] > captureUs[i - 1];
            }
            if (increasing) {
                out.clock = TimelineClock::Capture;
                out.us.reserve(captureUs.size());
                for (const std::uint64_t t : captureUs) {
                    out.us.push_back(static_cast<std::int64_t>(t - captureUs.front()));
                }
                return out;
            }
        }
        out.note = "neither the sample table nor the capture timestamps order the samples";
        return out;
    }

    // ---- the capture clock, when it passes every check ---------------------------
    out.note = captureRejection(captureUs, container);
    if (out.note.empty()) {
        out.clock = TimelineClock::Capture;
        out.us.reserve(captureUs.size());
        for (const std::uint64_t t : captureUs) {
            out.us.push_back(static_cast<std::int64_t>(t - captureUs.front()));
        }
        return out;
    }

    // ---- otherwise the container's own -------------------------------------------------
    out.clock = TimelineClock::Container;
    out.us = std::move(container);
    return out;
}

// =============================================================================
//  The timeline
// =============================================================================

ClipTimeline buildClipTimeline(std::uint32_t timescale, std::span<const std::uint64_t> durations,
                               std::span<const std::uint64_t> captureUs) {
    ClipTimeline out;
    out.timescale = timescale;
    // A sample count beyond 32 bits is not a clip anyone recorded; the
    // table type cannot even express it.
    out.sampleCount = static_cast<std::uint32_t>(std::min<std::size_t>(durations.size(), 0xFFFFFFFFu));
    if (timescale == 0 || durations.empty()) {
        out.note = "no sample timing";
        return out;
    }

    // ---- the nominal period ------------------------------------------------------------
    const std::uint64_t nominal = nominalSampleDuration(durations);
    if (nominal == 0 || nominal > 0xFFFFFFFFull) {
        out.note = "no usable sample duration";
        return out;
    }
    out.nominalTicks = static_cast<std::uint32_t>(nominal);

    // ---- constant frame rate: the timeline is the sample list --------------------------
    if (uniformSampleDurations(durations, nominal)) {
        return out;
    }

    // ---- variable: place every sample on the best clock --------------------------------
    SampleClock clock = buildSampleClock(timescale, durations, captureUs);
    if (!clock.valid()) {
        out.note = clock.note;
        return out;
    }
    const double periodUs = static_cast<double>(nominal) * 1e6 / static_cast<double>(timescale);
    const double lastUs = static_cast<double>(clock.us.back());
    if (!(periodUs > 0.0) || !std::isfinite(lastUs) || lastUs < 0.0) {
        out.note = "the sample moments do not form a timeline";
        return out;
    }
    // Frames: the last sample's moment in whole periods, plus that frame.
    const double frames = std::floor(lastUs / periodUs + 0.5) + 1.0;
    const double cap = static_cast<double>(out.sampleCount) * kMaxTimelineFramesPerSample;
    if (!(frames >= 1.0) || frames > cap || frames > static_cast<double>(0xFFFFFFFFu)) {
        out.note = "the gaps would need " + std::to_string(static_cast<long long>(frames)) + " frames for " +
                   std::to_string(out.sampleCount) + " samples";
        return out;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(frames);

    // ---- frame k shows the last sample captured by its middle --------------------------
    // One sweep: the samples are in order, so the pointer only moves forward.
    std::vector<std::uint32_t> map;
    map.resize(count);
    const std::size_t n = clock.us.size();
    std::size_t s = 0;
    for (std::uint32_t k = 0; k < count; ++k) {
        const double middle = (static_cast<double>(k) + 0.5) * periodUs;
        while (s + 1u < n && static_cast<double>(clock.us[s + 1u]) <= middle) {
            ++s;
        }
        map[k] = static_cast<std::uint32_t>(s);
    }

    // ---- what the placement did ----------------------------------------------------------
    std::uint32_t held = 0;
    std::uint64_t skipped = map.front();  // samples before the first frame's
    bool isIdentity = count == out.sampleCount && map.front() == 0;
    for (std::uint32_t k = 1; k < count; ++k) {
        if (map[k] == map[k - 1u]) {
            ++held;
        } else if (map[k] > map[k - 1u] + 1u) {
            skipped += map[k] - map[k - 1u] - 1u;
        }
        isIdentity = isIdentity && map[k] == k;
    }
    skipped += (out.sampleCount - 1u) - map.back();  // samples after the last frame's
    if (isIdentity) {
        // The table varies but every sample lands on its own frame: present
        // it exactly like a constant-rate clip.
        return out;
    }
    out.clock = clock.clock;
    out.note = std::move(clock.note);
    out.toSample = std::move(map);
    out.sampleUs = std::move(clock.us);
    out.heldFrames = held;
    out.skippedSamples = static_cast<std::uint32_t>(std::min<std::uint64_t>(skipped, 0xFFFFFFFFu));
    return out;
}

std::vector<std::uint32_t> mapTimelineToClock(const ClipTimeline& timeline, const SampleClock& target,
                                              double offsetUs) {
    std::vector<std::uint32_t> map;
    const std::uint32_t frames = timeline.frameCount();
    if (!target.valid() || frames == 0 || !std::isfinite(offsetUs)) {
        return map;
    }
    // Every frame's moment, moved onto the target's clock, then the target
    // sample captured nearest to it.
    map.resize(frames);
    for (std::uint32_t k = 0; k < frames; ++k) {
        map[k] = target.nearestSample(offsetUs + timeline.frameMomentUs(k));
    }
    return map;
}

// =============================================================================
//  Readers
// =============================================================================

std::vector<std::uint64_t> sampleDurationsOf(const TrackInfo& track) {
    const std::uint32_t count = track.samples.count();
    std::vector<std::uint64_t> durations;
    durations.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        durations.push_back(track.samples.sampleDuration(i));
    }
    return durations;
}

std::vector<std::uint64_t> captureTimestampsOf(const meta::MetadataTrack& track) {
    const std::uint32_t count = track.frameCount();
    std::vector<std::uint64_t> stamps;
    stamps.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto frame = track.frame(i);
        // One unreadable frame and the list no longer lines up with the
        // samples: none at all is the honest answer.
        if (!frame.ok() || frame.value().timestampUs == 0) {
            log::debug("timeline: frame {} of metadata track {} has no capture timestamp{}", i, track.trackId(),
                       frame.ok() ? std::string() : " (" + frame.error().message + ")");
            return {};
        }
        stamps.push_back(frame.value().timestampUs);
    }
    return stamps;
}

ClipTimeline clipTimelineFor(const TrackInfo& video, const meta::MetadataTrack* track) {
    const std::vector<std::uint64_t> durations = sampleDurationsOf(video);
    // A uniform table never needs the metadata: answer it from the table
    // alone, exactly as buildClipTimeline would.
    const std::uint64_t nominal = nominalSampleDuration(durations);
    if (uniformSampleDurations(durations, nominal) || track == nullptr) {
        return buildClipTimeline(video.timescale, durations, {});
    }
    const std::vector<std::uint64_t> capture = captureTimestampsOf(*track);
    return buildClipTimeline(video.timescale, durations, capture);
}

SampleClock sampleClockFor(const TrackInfo& video, const meta::MetadataTrack* track) {
    const std::vector<std::uint64_t> durations = sampleDurationsOf(video);
    std::vector<std::uint64_t> capture;
    if (track != nullptr) {
        capture = captureTimestampsOf(*track);
    }
    return buildSampleClock(video.timescale, durations, capture);
}

}  // namespace osv::video
