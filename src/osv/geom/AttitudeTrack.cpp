// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AttitudeTrack: orientation samples from the djmd metadata with slerp
// lookup and the IMU clock fit.

#include "osv/geom/AttitudeTrack.h"

#include "osv/core/Log.h"
#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace osv::geom {

// -----------------------------------------------------------------------------
//  Names and convention helpers
// -----------------------------------------------------------------------------
const char* attitudeSenseName(AttitudeSense sense) noexcept {
    switch (sense) {
    case AttitudeSense::WorldToBody: return "WorldToBody";
    case AttitudeSense::BodyToWorld: return "BodyToWorld";
    }
    return "Unknown";
}

const char* worldUpName(WorldUp up) noexcept {
    switch (up) {
    case WorldUp::Y: return "Y";
    case WorldUp::Z: return "Z";
    case WorldUp::NegY: return "-Y";
    case WorldUp::NegZ: return "-Z";
    }
    return "Unknown";
}

std::string attitudeConventionName(const AttitudeConvention& convention) {
    return std::format("{}/{}/{}", quatOrderName(convention.order), attitudeSenseName(convention.sense),
                       worldUpName(convention.up));
}

Vec3d worldUpVector(WorldUp up) noexcept {
    switch (up) {
    case WorldUp::Y: return Vec3d{0.0, 1.0, 0.0};
    case WorldUp::Z: return Vec3d{0.0, 0.0, 1.0};
    case WorldUp::NegY: return Vec3d{0.0, -1.0, 0.0};
    case WorldUp::NegZ: return Vec3d{0.0, 0.0, -1.0};
    }
    return Vec3d{0.0, 0.0, 1.0};
}

Quatd worldFromBodyQuat(const meta::Quaternion& q, const AttitudeConvention& convention) noexcept {
    // Reuse the extrinsic reader for the component order / sanitising.
    const Quatd raw = extrinsicQuat(q, convention.order);
    // A world->body rotation is inverted so the track always stores body->world.
    return (convention.sense == AttitudeSense::WorldToBody) ? raw.conj() : raw;
}

Mat3d bodyFromWorldMatrix(const meta::Quaternion& q, const AttitudeConvention& convention) noexcept {
    // worldFromBody^T == bodyFromWorld for a rotation.
    return worldFromBodyQuat(q, convention).toMatrix().transposed();
}

// -----------------------------------------------------------------------------
//  Construction
// -----------------------------------------------------------------------------
namespace {

/// Sort by time, drop non-finite entries and duplicates (keep the first).
void tidySamples(std::vector<AttitudeTrack::Sample>& samples) {
    // Remove anything that cannot be interpolated.
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [](const AttitudeTrack::Sample& s) {
                                     return !std::isfinite(s.tUs) || !s.worldFromBody.isFinite();
                                 }),
                  samples.end());
    std::stable_sort(samples.begin(), samples.end(),
                     [](const AttitudeTrack::Sample& a, const AttitudeTrack::Sample& b) { return a.tUs < b.tUs; });
    // Strictly increasing times are required by the binary search.
    samples.erase(std::unique(samples.begin(), samples.end(),
                              [](const AttitudeTrack::Sample& a, const AttitudeTrack::Sample& b) {
                                  return !(a.tUs < b.tUs) && !(b.tUs < a.tUs);
                              }),
                  samples.end());
    // Normalise every quaternion once so lookups never do it.
    for (AttitudeTrack::Sample& s : samples) {
        s.worldFromBody = s.worldFromBody.normalized();
    }
}

}  // namespace

Result<AttitudeTrack> AttitudeTrack::fromSamples(std::vector<Sample> samples, const Options& options) {
    tidySamples(samples);
    if (samples.empty()) {
        return Error{ErrorCode::InvalidArgument, "AttitudeTrack: no usable samples"};
    }
    AttitudeTrack track;
    track.m_samples = std::move(samples);
    track.m_options = options;
    return track;
}

Result<AttitudeTrack> AttitudeTrack::build(const meta::MetadataTrack& track, const Options& options) {
    const std::uint32_t frameCount = track.frameCount();
    if (frameCount == 0) {
        return Error{ErrorCode::NotFound, "AttitudeTrack: metadata track has no frames"};
    }
    if (options.batchAnchorIndex < 0) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("AttitudeTrack: negative batch anchor {}", options.batchAnchorIndex)};
    }
    if (!std::isfinite(options.extraOffsetUs)) {
        return Error{ErrorCode::InvalidArgument, "AttitudeTrack: extra offset is not finite"};
    }

    // Dense placement needs the IMU rate to space the batch samples.
    double samplePeriodUs = 0.0;
    if (options.dense) {
        const std::uint32_t rate = track.clip().imuSamplingRate;
        if (rate == 0) {
            return Error{ErrorCode::NotFound, "AttitudeTrack: imu_sampling_rate missing (needed for dense mode)"};
        }
        samplePeriodUs = 1.0e6 / static_cast<double>(rate);
    }

    std::vector<Sample> samples;
    std::vector<ClockObservation> clock;
    samples.reserve(options.dense ? frameCount * 17 : frameCount);
    clock.reserve(frameCount);
    std::size_t skipped = 0;

    for (std::uint32_t i = 0; i < frameCount; ++i) {
        const Result<meta::FrameMeta> frameResult = track.frame(i);
        if (!frameResult.ok()) {
            ++skipped;
            continue;
        }
        const meta::FrameMeta& frame = frameResult.value();
        const double frameTs = static_cast<double>(frame.timestampUs);

        // The IMU batch feeds both the clock fit and (dense mode) the samples.
        if (frame.imu.has_value() && frame.imu->current.present && !frame.imu->current.q.empty()) {
            const meta::ImuBatch& batch = frame.imu->current;
            clock.push_back(ClockObservation{frameTs, static_cast<double>(batch.ts), batch.q.size()});
            if (options.dense) {
                for (std::size_t s = 0; s < batch.q.size(); ++s) {
                    if (!batch.q[s].present) {
                        continue;
                    }
                    const double t = frameTs + (static_cast<double>(s) - options.batchAnchorIndex) * samplePeriodUs +
                                     options.extraOffsetUs;
                    samples.push_back(Sample{t, worldFromBodyQuat(batch.q[s], options.conv)});
                }
                continue;
            }
        } else if (options.dense) {
            // Dense mode without a batch: fall back to the frame attitude so
            // the track has no hole.
            if (frame.camera.attitude.present) {
                samples.push_back(Sample{frameTs + options.extraOffsetUs,
                                         worldFromBodyQuat(frame.camera.attitude, options.conv)});
            } else {
                ++skipped;
            }
            continue;
        }

        // Sparse mode: one sample per frame at the frame timestamp.
        if (frame.camera.attitude.present) {
            samples.push_back(Sample{frameTs, worldFromBodyQuat(frame.camera.attitude, options.conv)});
        } else {
            ++skipped;
        }
    }

    if (skipped > 0) {
        log::debug("AttitudeTrack: {} of {} frames had no usable attitude", skipped, frameCount);
    }
    OSV_TRY_ASSIGN(AttitudeTrack result, fromSamples(std::move(samples), options));
    result.m_clockFit = fitClock(clock);
    return result;
}

// -----------------------------------------------------------------------------
//  Lookup
// -----------------------------------------------------------------------------
Quatd AttitudeTrack::worldFromBody(double tUs) const noexcept {
    if (m_samples.empty() || !std::isfinite(tUs)) {
        return Quatd::identity();
    }
    // Clamp outside the covered range.
    if (tUs <= m_samples.front().tUs) {
        return m_samples.front().worldFromBody;
    }
    if (tUs >= m_samples.back().tUs) {
        return m_samples.back().worldFromBody;
    }
    // First sample strictly after t; its predecessor is at or before t.
    const auto upper = std::upper_bound(m_samples.begin(), m_samples.end(), tUs,
                                        [](double t, const Sample& s) { return t < s.tUs; });
    if (upper == m_samples.begin() || upper == m_samples.end()) {
        return m_samples.back().worldFromBody;
    }
    const Sample& b = *upper;
    const Sample& a = *(upper - 1);
    const double span = b.tUs - a.tUs;
    if (!(span > 0.0)) {
        return a.worldFromBody;
    }
    const double f = clampd((tUs - a.tUs) / span, 0.0, 1.0);
    return Quatd::slerp(a.worldFromBody, b.worldFromBody, f);
}

double AttitudeTrack::beginUs() const noexcept { return m_samples.empty() ? 0.0 : m_samples.front().tUs; }

double AttitudeTrack::endUs() const noexcept { return m_samples.empty() ? 0.0 : m_samples.back().tUs; }

Vec3d AttitudeTrack::worldUp() const noexcept { return worldUpVector(m_options.conv.up); }

// -----------------------------------------------------------------------------
//  Clock fit
// -----------------------------------------------------------------------------
AttitudeTrack::ClockFit AttitudeTrack::fitClock(const std::vector<ClockObservation>& observations) noexcept {
    ClockFit fit;
    // Keep only finite observations; the fit needs at least two of them.
    std::vector<ClockObservation> obs;
    obs.reserve(observations.size());
    for (const ClockObservation& o : observations) {
        if (std::isfinite(o.frameTimeUs) && std::isfinite(o.batchTicks)) {
            obs.push_back(o);
        }
    }
    if (obs.size() < 2) {
        return fit;
    }

    // Centre the data before the least-squares slope to avoid catastrophic
    // cancellation on the large absolute timestamps.
    double meanT = 0.0;
    double meanTicks = 0.0;
    for (const ClockObservation& o : obs) {
        meanT += o.frameTimeUs;
        meanTicks += o.batchTicks;
    }
    meanT /= static_cast<double>(obs.size());
    meanTicks /= static_cast<double>(obs.size());
    double sxx = 0.0;
    double sxy = 0.0;
    for (const ClockObservation& o : obs) {
        const double dt = o.frameTimeUs - meanT;
        sxx += dt * dt;
        sxy += dt * (o.batchTicks - meanTicks);
    }
    if (!(sxx > 0.0)) {
        return fit;
    }
    fit.ticksPerUs = sxy / sxx;
    fit.offsetTicks = meanTicks - fit.ticksPerUs * meanT;
    fit.n = obs.size();

    // RMS residual in microseconds (ticks / slope).
    double sumSq = 0.0;
    for (const ClockObservation& o : obs) {
        const double predicted = fit.ticksPerUs * o.frameTimeUs + fit.offsetTicks;
        const double residualTicks = o.batchTicks - predicted;
        sumSq += residualTicks * residualTicks;
    }
    if (fit.ticksPerUs != 0.0 && std::isfinite(fit.ticksPerUs)) {
        fit.rmsUs = std::sqrt(sumSq / static_cast<double>(obs.size())) / std::fabs(fit.ticksPerUs);
    }

    // Ticks per IMU sample: total tick advance between consecutive batches
    // divided by the number of samples that advance covers.  Batches are
    // assumed to be consecutive in time (they are, one per frame); pairs
    // whose ticks go backwards (counter wrap) are skipped.
    double tickSum = 0.0;
    double sampleSum = 0.0;
    for (std::size_t i = 1; i < obs.size(); ++i) {
        const double dTicks = obs[i].batchTicks - obs[i - 1].batchTicks;
        if (!(dTicks > 0.0) || obs[i - 1].batchSamples == 0) {
            continue;
        }
        tickSum += dTicks;
        sampleSum += static_cast<double>(obs[i - 1].batchSamples);
    }
    fit.ticksPerSample = (sampleSum > 0.0) ? tickSum / sampleSum : 0.0;
    return fit;
}

}  // namespace osv::geom
