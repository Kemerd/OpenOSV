// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Spherical (360-degree) video metadata for finished MP4 / MOV files.
//
// An equirectangular video only plays as 360 video when its container says
// so.  Two open schemes exist and players read one or the other, so the
// first video track gets both (github.com/google/spatial-media, docs/):
//
//   Spherical Video V1  moov/trak/uuid ffcc8263-f855-4a93-8814-587a02521fdd
//                       holding the GSpherical RDF/XML (Spherical, Stitched,
//                       StitchingSoftware, ProjectionType equirectangular),
//                       appended at the end of the video 'trak' exactly
//                       where Google's Spatial Media Metadata Injector puts
//                       it.  YouTube and older players read this one.
//   Spherical Video V2  'st3d' (monoscopic) and 'sv3d' { 'svhd', 'proj'
//                       { 'prhd' (pose 0/0/0), 'equi' (no cropping) } }
//                       inside every sample entry of the video track
//                       ('hvc1', 'hev1', 'avc1', 'apch', ...), placed before
//                       the optional 'clap' / 'pasp' / 'btrt' boxes as the
//                       RFC asks.  FFmpeg, VR players and 360 editors read it.
//
// The rewrite:
//   * only the 'moov' box is loaded and rebuilt in memory (it is a few MB
//     even for hours of video); the media data is streamed from the source
//     to a temporary file beside the target, so a 50 GB file never has to
//     fit in RAM;
//   * every box on the path moov/trak/mdia/minf/stbl/stsd/<entry> gets its
//     new size; everything else is copied byte for byte;
//   * when 'moov' comes before the media data (a "faststart" file, which is
//     what osvtool's ffmpeg writes) every 'stco' / 'co64' chunk offset of
//     EVERY track that points past the old 'moov' is moved by the number of
//     bytes 'moov' grew; offsets before it (media data first) stay as they
//     are;
//   * the temporary file replaces the target atomically only once it is
//     complete, so a failure (disk full, malformed input) leaves the target
//     exactly as it was;
//   * it is idempotent: an existing spherical tag (ours, or one written by
//     another tool) is replaced rather than duplicated, and a file that
//     already carries exactly these boxes is left untouched.
//
// Every size is checked against its parent and the file; malformed,
// truncated, fragmented or otherwise unsupported input is refused with an
// Error and nothing is written.  Nothing here throws.
#pragma once

#include "osv/core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace osv::io {

/// Extended type of the Spherical Video V1 'uuid' box:
/// ffcc8263-f855-4a93-8814-587a02521fdd.
inline constexpr std::array<std::uint8_t, 16> kSphericalV1Uuid = {0xff, 0xcc, 0x82, 0x63, 0xf8, 0x55, 0x4a, 0x93,
                                                                  0x88, 0x14, 0x58, 0x7a, 0x02, 0x52, 0x1f, 0xdd};

/// Written as the V1 StitchingSoftware and as the V2 'svhd' metadata_source.
inline constexpr const char* kSphericalMetadataSource = "OpenOSV";

/// Largest 'moov' box the injector loads into memory.  Real files are far
/// below it (a 10-hour 60 fps recording has a moov of about 10 MB); the cap
/// only stops a hostile size field from turning into a huge allocation.
inline constexpr std::uint64_t kMaxSphericalMoovBytes = std::uint64_t{512} << 20;

/// What injectSphericalMetadata did.
struct SphericalInjectReport {
    /// False when the file already carried exactly these tags: an in-place
    /// call then writes nothing at all.
    bool changed = false;

    /// True when an earlier spherical tag (V1 'uuid', 'st3d' or 'sv3d') was
    /// removed before the new one was written.
    bool replacedExisting = false;

    /// True when media data follows 'moov' (a faststart file), which is
    /// when chunk offsets have to move.
    bool moovBeforeMdat = false;

    /// Bytes the 'moov' box grew by (negative when a longer, older tag was
    /// replaced; 0 when unchanged).
    std::int64_t moovGrowth = 0;

    /// 'stco' / 'co64' entries rewritten, summed over every track.
    std::uint64_t chunkOffsetsShifted = 0;

    /// track_ID of the tagged video track (0 when its 'tkhd' is missing).
    std::uint32_t videoTrackId = 0;

    /// Sample entries of that track that received 'st3d' + 'sv3d'.
    std::uint32_t sampleEntriesTagged = 0;

    /// Four-character code of the (first) tagged sample entry, e.g. "hvc1".
    std::string sampleEntryType;
};

/// The Spherical Video V1 RDF/XML document written into the 'uuid' box:
/// the same document Google's injector writes for a monoscopic
/// equirectangular video, with StitchingSoftware = kSphericalMetadataSource.
[[nodiscard]] std::string sphericalV1Xml();

/// Tag the MP4 / MOV at `input` as monoscopic equirectangular 360 video
/// (Spherical Video V1 and V2 on its first video track, see the file
/// comment).  The result goes to `output`; an empty `output`, or one that is
/// the same file as `input`, rewrites `input` in place.  Either way the data
/// is written to a temporary file beside the target first and moved over it
/// only once complete.
///
/// Errors: InvalidArgument (empty path), Io (cannot read / write / replace),
/// Malformed or Truncated (not a sound MP4 / MOV), NotFound (no 'moov' or no
/// video track), Unsupported (fragmented MP4, auxiliary sample offsets, a
/// 'moov' above kMaxSphericalMoovBytes, a 32-bit chunk offset that would
/// overflow).  On any error the target is left as it was.
[[nodiscard]] Result<SphericalInjectReport> injectSphericalMetadata(const std::filesystem::path& input,
                                                                    const std::filesystem::path& output = {});

}  // namespace osv::io
