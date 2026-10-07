// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ClipTimeline: a clip's samples placed on a constant-rate timeline at its
// NOMINAL frame rate - what every host (Premiere, Resolve, VEGAS, osvtool's
// MP4 output) needs, because each of them describes video with one fixed
// frame period.
//
// Why this exists
//   The Osmo 360 records at a constant rate, but when it cannot keep up (a
//   long night drive at high ISO, for instance) it DROPS frames and records
//   the gap in the container's time table (stts) as one longer sample: a 50
//   fps clip of 16157 samples then holds 167 samples of 40 ms and 2 of 60 ms.
//   Handing a host "16157 frames at 50 fps" makes the picture 3.4 s shorter
//   than the sound and run ahead of it; "16157 frames at 49.48 fps" spreads
//   the same error over the whole clip.  What actually happened is a 50 fps
//   recording with a few pictures missing, so the timeline is that: frames
//   at the nominal period, each showing the last picture captured at or
//   before its moment, the previous picture HELD over every gap - the
//   picture stays with the audio.
//
// The clock
//   Each sample's moment comes from the camera's own per-frame capture
//   timestamps (djmd timestamp_us) when they are trustworthy: exactly one
//   per sample, strictly increasing, and spanning the same time as the
//   container within kCaptureSpanTolerance.  The container's table is the
//   fallback.  The capture clock wins because the .LRF's table does not
//   follow it: it writes each 60 ms gap as 80 ms, 3.1 s too long over the
//   night clip, while its capture timestamps (and the .OSV's table) match
//   the audio to within milliseconds.
//
// Constant frame rate
//   When every sample lasts the nominal period the timeline IS the sample
//   list: identity() is true, toSample stays empty, and every caller keeps
//   its existing frame-for-sample behaviour byte for byte.  Building it reads
//   nothing but the sample table; the metadata is only read for a clip that
//   really is variable.
//
// The builders are pure functions of their inputs (no files, no globals), so
// the importer, the OpenFX generator, osvtool's probe JSON (and through it
// the VEGAS import) and osvtool's render loop all present a clip the same way.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace osv {
struct TrackInfo;
}  // namespace osv

namespace osv::meta {
class MetadataTrack;
}  // namespace osv::meta

namespace osv::video {

/// The capture clock is trusted only when its span agrees with the
/// container's within this fraction (the night .LRF differs by 0.95 %, its
/// .OSV by 0.003 %; a clock that is off by more is not this clip's clock).
inline constexpr double kCaptureSpanTolerance = 0.03;

/// A timeline may hold at most this many frames per sample; a table that
/// asks for more (a corrupt multi-hour duration) is not presented as
/// variable at all rather than allocating a timeline the size of the gap.
inline constexpr std::uint32_t kMaxTimelineFramesPerSample = 8;

/// Which clock placed the samples on the timeline.
enum class TimelineClock : std::uint8_t {
    Identity = 0,   ///< Constant frame rate: timeline frame k is sample k (nothing was placed).
    Capture = 1,    ///< The camera's per-frame capture timestamps (djmd timestamp_us).
    Container = 2   ///< The container's sample table (stts presentation times).
};

/// Stable lower-case name of a clock ("identity", "capture", "container").
[[nodiscard]] const char* timelineClockName(TimelineClock clock) noexcept;

/// Every sample's moment on one clock, in microseconds after sample 0.
///
/// `us` is strictly increasing and has one entry per sample when the clock
/// is usable; it is empty when neither clock could place the samples (a
/// table of zero durations without trustworthy timestamps).
struct SampleClock {
    TimelineClock clock = TimelineClock::Container;  ///< Capture or Container (never Identity).
    std::vector<std::int64_t> us;                    ///< Per-sample moment, us after sample 0.
    std::string note;                                ///< Why the capture clock was not used (empty when it was).

    /// True when the clock places every sample.
    [[nodiscard]] bool valid() const noexcept { return !us.empty(); }

    /// The sample captured NEAREST to `momentUs` (us after sample 0), a tie
    /// going to the later sample; clamped into [0, size).  0 for an invalid
    /// clock or a NaN moment.
    [[nodiscard]] std::uint32_t nearestSample(double momentUs) const noexcept;
};

/// A clip's samples on its constant-rate timeline (see the file comment).
struct ClipTimeline {
    std::uint32_t timescale = 0;       ///< Ticks per second of the sample table: the rate numerator.
    std::uint32_t nominalTicks = 0;    ///< Nominal frame period in ticks (the most common sample duration): the
                                       ///< rate denominator.  0 when the table has no usable duration.
    std::uint32_t sampleCount = 0;     ///< Samples in the track.
    TimelineClock clock = TimelineClock::Identity;  ///< The clock that placed the samples.
    std::vector<std::uint32_t> toSample;   ///< Timeline frame -> sample; EMPTY for a constant-rate clip.
    std::vector<std::int64_t> sampleUs;    ///< Per-sample moment on `clock` (us after sample 0); empty for identity.
    std::uint32_t heldFrames = 0;      ///< Timeline frames that repeat the previous frame's sample (dropped frames).
    std::uint32_t skippedSamples = 0;  ///< Samples no timeline frame shows (0 on every recording seen so far).
    std::string note;                  ///< Why the capture clock was not used, or why no timeline was built.

    /// True when timeline frame k is sample k (constant frame rate, or a
    /// clip whose variable timing could not be placed - see note).
    [[nodiscard]] bool identity() const noexcept { return toSample.empty(); }

    /// Frames on the timeline: the sample count for identity.
    [[nodiscard]] std::uint32_t frameCount() const noexcept {
        return identity() ? sampleCount : static_cast<std::uint32_t>(toSample.size());
    }

    /// The sample shown at timeline frame `frame`, clamped into the clip
    /// (0 for an empty one).
    [[nodiscard]] std::uint32_t sampleFor(std::uint32_t frame) const noexcept;

    /// Nominal frames per second (timescale / nominalTicks), 0 when unknown.
    [[nodiscard]] double fps() const noexcept;

    /// The timeline's length: frameCount() nominal periods, in seconds.
    [[nodiscard]] double durationSeconds() const noexcept;

    /// The moment timeline frame `frame` shows, in microseconds after sample
    /// 0: frame * nominal period for identity, otherwise the capture (or
    /// container) moment of the sample it shows - so a held frame names the
    /// same moment as the frame before it.
    [[nodiscard]] double frameMomentUs(std::uint32_t frame) const noexcept;
};

/// The nominal frame period: the most common duration between consecutive
/// samples (the last sample's own duration is ignored, many writers make it
/// up), ties going to the shorter period; zero durations never count.  A
/// single-sample table answers its one duration.  NOT sample 0's duration -
/// a long first sample would otherwise halve the rate of the whole clip.
[[nodiscard]] std::uint64_t nominalSampleDuration(std::span<const std::uint64_t> durations) noexcept;

/// True when every sample but the last lasts `nominal` ticks (the clip is
/// constant frame rate as far as its presentation is concerned).  False for
/// a zero nominal.
[[nodiscard]] bool uniformSampleDurations(std::span<const std::uint64_t> durations, std::uint64_t nominal) noexcept;

/// Every sample's moment on the best clock: the capture timestamps when they
/// are one per sample, strictly increasing and within kCaptureSpanTolerance
/// of the container's span, otherwise the container table's presentation
/// times (`durations` accumulated at `timescale`).  Pure.
[[nodiscard]] SampleClock buildSampleClock(std::uint32_t timescale, std::span<const std::uint64_t> durations,
                                           std::span<const std::uint64_t> captureUs);

/// The clip's constant-rate timeline (see the file comment): identity when
/// the table is uniform; otherwise frames at the nominal period, frame k
/// showing the last sample captured at or before (k + 1/2) periods.  Never
/// fails: an input it cannot place (no timescale, no durations, no usable
/// clock, an absurd gap) yields identity with `note` saying why.  Pure.
[[nodiscard]] ClipTimeline buildClipTimeline(std::uint32_t timescale, std::span<const std::uint64_t> durations,
                                             std::span<const std::uint64_t> captureUs);

/// For every frame of `timeline`, the sample of ANOTHER recording of the
/// same moments (an .LRF proxy) captured nearest to the moment that frame
/// shows.  `offsetUs` is `timeline`'s sample 0 on `target`'s clock (the
/// difference of the two first capture timestamps).  Empty when the target
/// clock is invalid or the timeline is empty.  Pure.
[[nodiscard]] std::vector<std::uint32_t> mapTimelineToClock(const ClipTimeline& timeline, const SampleClock& target,
                                                            double offsetUs);

// ---- readers: the inputs above, from an opened file ---------------------------------

/// Every sample's duration (stts delta, timescale ticks) of `track`.
[[nodiscard]] std::vector<std::uint64_t> sampleDurationsOf(const TrackInfo& track);

/// Every frame's capture timestamp (djmd timestamp_us) of `track`; EMPTY
/// when any frame cannot be decoded or carries none, because a partial list
/// cannot be lined up with the samples.  Decodes (and caches) every frame's
/// metadata, so callers only ask for it on a variable-rate clip.
[[nodiscard]] std::vector<std::uint64_t> captureTimestampsOf(const meta::MetadataTrack& track);

/// buildClipTimeline() for an opened video track and its metadata (null
/// when there is none): the metadata is read only when the table is not
/// uniform, so a constant-rate clip costs one pass over its table.
[[nodiscard]] ClipTimeline clipTimelineFor(const TrackInfo& video, const meta::MetadataTrack* track);

/// buildSampleClock() for an opened video track and its metadata (null when
/// there is none).  Reads every frame's metadata when it is given.
[[nodiscard]] SampleClock sampleClockFor(const TrackInfo& video, const meta::MetadataTrack* track);

}  // namespace osv::video
