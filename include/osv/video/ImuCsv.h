// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CSV export of the per-frame camera / IMU metadata (osvtool extract --imu).
//
// Two layouts are produced:
//
//   per frame (default), one row per video frame:
//     frame,seq,timestamp_us,att_w,att_x,att_y,att_z,acc_x,acc_y,acc_z,
//     iso,exposure_num,exposure_den,wb_cct,sensor_temp
//
//   dense (`dense == true`), one row per fused IMU sample of every frame:
//     frame,sample_index,ts_ticks,q_w,q_x,q_y,q_z
//   where ts_ticks = batch.ts + sample_index * 1006 (the attitude clock runs
//   at 1006 ticks per sample on the Osmo 360, see docs/GEOMETRY.md).
//
// Quaternion components are written in the order the camera stores them
// (w, x, y, z); no convention is applied here so the file can be used to
// probe conventions offline.  Output is plain 7-bit ASCII with "\n" line
// endings and 7 significant digits for floats.
#pragma once

#include "osv/core/Result.h"

#include <filesystem>
#include <ostream>

namespace osv::meta {
class MetadataTrack;
}

namespace osv::video {

/// Write the CSV for every frame of `track` to `out`.  Errors: Io when the
/// stream goes bad, otherwise whatever MetadataTrack::frame() reported for
/// the first frame that failed to decode (frames that fail are skipped with
/// a warning, the error is returned only when NO row could be written).
/// In dense mode a track whose frames carry no IMU batch at all (the
/// stripped second djmd track of the Osmo 360) yields NotFound.
Status writeImuCsv(const meta::MetadataTrack& track, std::ostream& out, bool dense);

/// Convenience: open `osvPath` with the container parser and the metadata
/// decoder, then write `csvPath` (created / truncated).
Status writeImuCsv(const std::filesystem::path& osvPath, const std::filesystem::path& csvPath, bool dense);

}  // namespace osv::video
