// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CSV export of the per-frame camera / IMU metadata (see ImuCsv.h).
//
// Formatting is done with snprintf into a small stack buffer rather than
// iostream manipulators: 65 frames is nothing, but a one-hour clip has
// 215 000 frames and 3.5 million IMU samples, and the ostream formatting
// machinery is measurably slower than "%.7g" at that scale.

#include "osv/video/ImuCsv.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

namespace osv::video {

namespace {

/// Attitude clock ticks between consecutive fused IMU samples (documented in
/// docs/GEOMETRY.md; measured on the sample clip as 1006 +- 1).
constexpr std::uint64_t kTicksPerImuSample = 1006;

/// Append a float with 7 significant digits (never locale dependent: the C
/// locale is what snprintf uses unless the process changed it, and OpenOSV
/// never does).
void appendFloat(std::string& line, float value) {
    std::array<char, 32> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.7g", static_cast<double>(value));
    if (n > 0) {
        line.append(buf.data(), static_cast<std::size_t>(n < static_cast<int>(buf.size()) ? n : static_cast<int>(buf.size()) - 1));
    } else {
        line.append("0");
    }
}

/// Append an unsigned integer.
void appendUnsigned(std::string& line, std::uint64_t value) { line.append(std::to_string(value)); }

/// Append a signed integer.
void appendSigned(std::string& line, std::int64_t value) { line.append(std::to_string(value)); }

/// One per-frame row.
void appendFrameRow(std::string& line, std::uint32_t index, const meta::FrameMeta& f) {
    const meta::CameraFrame& cam = f.camera;
    appendUnsigned(line, index);
    line.push_back(',');
    appendUnsigned(line, f.seq);
    line.push_back(',');
    appendUnsigned(line, f.timestampUs);
    line.push_back(',');
    // Attitude in stored order (w, x, y, z); absent -> the identity default.
    appendFloat(line, cam.attitude.w);
    line.push_back(',');
    appendFloat(line, cam.attitude.x);
    line.push_back(',');
    appendFloat(line, cam.attitude.y);
    line.push_back(',');
    appendFloat(line, cam.attitude.z);
    line.push_back(',');
    appendFloat(line, cam.acc.x);
    line.push_back(',');
    appendFloat(line, cam.acc.y);
    line.push_back(',');
    appendFloat(line, cam.acc.z);
    line.push_back(',');
    appendFloat(line, cam.iso);
    line.push_back(',');
    // exposure_time is a rational [num, den]; a short vector yields zeros so
    // the column count never changes.
    appendSigned(line, cam.exposureTime.size() >= 1 ? cam.exposureTime[0] : 0);
    line.push_back(',');
    appendSigned(line, cam.exposureTime.size() >= 2 ? cam.exposureTime[1] : 0);
    line.push_back(',');
    appendUnsigned(line, cam.wbCct);
    line.push_back(',');
    appendFloat(line, cam.sensorTemperature);
    line.push_back('\n');
}

/// The dense rows of one frame (one per fused IMU sample).  Returns the
/// number of rows appended (0 when the frame carries no IMU batch).
std::size_t appendDenseRows(std::string& block, std::uint32_t index, const meta::FrameMeta& f) {
    // Key off the data, not the `present` flag: a batch without quaternions
    // has nothing to export whatever the decoder recorded about it.
    if (!f.imu || f.imu->current.q.empty()) {
        return 0;
    }
    const meta::ImuBatch& batch = f.imu->current;
    std::size_t rows = 0;
    for (std::size_t s = 0; s < batch.q.size(); ++s) {
        const meta::Quaternion& q = batch.q[s];
        appendUnsigned(block, index);
        block.push_back(',');
        appendUnsigned(block, s);
        block.push_back(',');
        // 64-bit arithmetic: the 32-bit attitude clock wraps every ~71 min at
        // ~1 MHz, but the sum of a wrapped base plus the offset must not.
        appendUnsigned(block, static_cast<std::uint64_t>(batch.ts) + kTicksPerImuSample * static_cast<std::uint64_t>(s));
        block.push_back(',');
        appendFloat(block, q.w);
        block.push_back(',');
        appendFloat(block, q.x);
        block.push_back(',');
        appendFloat(block, q.y);
        block.push_back(',');
        appendFloat(block, q.z);
        block.push_back('\n');
        ++rows;
    }
    return rows;
}

}  // namespace

// =============================================================================
//  writeImuCsv (stream)
// =============================================================================
Status writeImuCsv(const meta::MetadataTrack& track, std::ostream& out, bool dense) {
    if (!out.good()) {
        return failStatus(ErrorCode::Io, "output stream is not writable");
    }
    const std::uint32_t frameCount = track.frameCount();
    if (frameCount == 0) {
        return failStatus(ErrorCode::NotFound, "metadata track " + std::to_string(track.trackId()) + " has no frames");
    }

    // ---- header ---------------------------------------------------------------
    if (dense) {
        out << "frame,sample_index,ts_ticks,q_w,q_x,q_y,q_z\n";
    } else {
        out << "frame,seq,timestamp_us,att_w,att_x,att_y,att_z,acc_x,acc_y,acc_z,iso,exposure_num,exposure_den,"
               "wb_cct,sensor_temp\n";
    }
    if (!out.good()) {
        return failStatus(ErrorCode::Io, "write failed while emitting the CSV header");
    }

    // ---- rows -----------------------------------------------------------------
    // Rows are accumulated per frame and written in one go; frames whose
    // metadata sample fails to decode are skipped with a warning and the
    // first error is remembered for the "nothing written at all" case.
    std::string block;
    block.reserve(dense ? 2048 : 256);
    std::uint64_t rowsWritten = 0;
    std::uint32_t framesFailed = 0;
    std::uint32_t framesWithoutImu = 0;
    std::optional<Error> firstError;
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        const Result<meta::FrameMeta> frame = track.frame(i);
        if (!frame.ok()) {
            ++framesFailed;
            if (!firstError) {
                firstError = frame.error();
            }
            log::warn("imu csv: frame {} skipped ({})", i, log::safe(frame.error().message));
            continue;
        }
        block.clear();
        if (dense) {
            const std::size_t rows = appendDenseRows(block, i, frame.value());
            if (rows == 0) {
                ++framesWithoutImu;
                continue;
            }
            rowsWritten += rows;
        } else {
            appendFrameRow(block, i, frame.value());
            ++rowsWritten;
        }
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
        if (!out.good()) {
            return failStatus(ErrorCode::Io, "write failed at frame " + std::to_string(i));
        }
    }
    out.flush();
    if (!out.good()) {
        return failStatus(ErrorCode::Io, "flush failed");
    }

    // ---- verdict --------------------------------------------------------------
    if (rowsWritten == 0) {
        if (firstError) {
            return failStatus(firstError->code, "no frame could be written; first failure: " + firstError->message);
        }
        // Every frame decoded but none carried an IMU batch (track 5 of the
        // Osmo 360): the dense layout has nothing to say about this track.
        return failStatus(ErrorCode::NotFound, "metadata track " + std::to_string(track.trackId()) +
                                                   " carries no IMU samples (use the primary djmd track)");
    }
    if (framesFailed > 0) {
        log::warn("imu csv: {} of {} frames could not be decoded and were skipped", framesFailed, frameCount);
    }
    if (dense && framesWithoutImu > 0) {
        log::warn("imu csv: {} of {} frames carry no IMU batch", framesWithoutImu, frameCount);
    }
    log::debug("imu csv: wrote {} rows for {} frames ({})", rowsWritten, frameCount, dense ? "dense" : "per frame");
    return okStatus();
}

// =============================================================================
//  writeImuCsv (paths)
// =============================================================================
Status writeImuCsv(const std::filesystem::path& osvPath, const std::filesystem::path& csvPath, bool dense) {
    if (osvPath.empty() || csvPath.empty()) {
        return failStatus(ErrorCode::InvalidArgument, "input and output paths are required");
    }
    // The metadata track keeps a pointer to the file, so the file must stay
    // alive for the whole export (both live on this stack frame).
    OSV_TRY_ASSIGN(const OsvFile file, OsvFile::open(osvPath));
    OSV_TRY_ASSIGN(const meta::MetadataTrack track, meta::MetadataTrack::load(file));

    std::ofstream out(csvPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return failStatus(ErrorCode::Io, "cannot create " + log::safe(csvPath.string()));
    }
    return writeImuCsv(track, out, dense);
}

}  // namespace osv::video
