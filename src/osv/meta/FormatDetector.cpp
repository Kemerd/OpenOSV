// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FormatDetector implementation.  Metadata first, container second, and a
// note for everything that had to be inferred.

#include "osv/meta/FormatDetector.h"

#include "osv/core/Log.h"

#include <cmath>
#include <format>

namespace osv::meta {

Mode FormatDetector::modeFromWidth(std::uint32_t width, bool sideBySide) noexcept {
    if (sideBySide) {
        return Mode::Lrf;
    }
    switch (width) {
    case 1920: return Mode::K4;
    case 3000: return Mode::K6;
    case 3840: return Mode::K8;
    default: return Mode::Unknown;
    }
}

Result<FormatInfo> FormatDetector::detect(const OsvFile& file, const MetadataTrack* meta) {
    FormatInfo info;

    // ---- lens <-> track mapping (also tells us about side-by-side) ----------
    const StreamMeta* stream = (meta && meta->hasStream()) ? &meta->stream() : nullptr;
    const ClipMeta* clip = (meta && meta->hasClip()) ? &meta->clip() : nullptr;
    OSV_TRY_ASSIGN(MetadataTrack::LensStreamOrder order, MetadataTrack::lensStreamOrderFor(file, stream));
    info.videoTrackIds = {order.slaveTrackId, order.masterTrackId};
    info.sideBySideProxy = order.sideBySide;
    info.dualFisheye = order.sideBySide || order.slaveTrackId != order.masterTrackId;
    info.notes.push_back("lens order: " + order.source);
    if (order.fromFallback) {
        info.notes.push_back("lens order could not be verified from the container flags; documented default assumed");
    }

    const TrackInfo* slaveTrack = file.track(order.slaveTrackId);
    if (!slaveTrack) {
        return Error{ErrorCode::Internal, "lens order names a track that does not exist"};
    }

    // ---- stream geometry -----------------------------------------------------
    // Sample entry (stsd) size first, tkhd presentation size second.
    const std::uint32_t containerW = slaveTrack->codedWidth() ? slaveTrack->codedWidth()
                                                              : static_cast<std::uint32_t>(std::lround(slaveTrack->width));
    const std::uint32_t containerH = slaveTrack->codedHeight() ? slaveTrack->codedHeight()
                                                               : static_cast<std::uint32_t>(std::lround(slaveTrack->height));
    if (stream && stream->video.width != 0 && stream->video.height != 0) {
        info.streamW = stream->video.width;
        info.streamH = stream->video.height;
        if (containerW != 0 && (containerW != info.streamW || containerH != info.streamH)) {
            info.notes.push_back(std::format("metadata stream size {}x{} differs from container {}x{}", info.streamW,
                                             info.streamH, containerW, containerH));
        }
    } else {
        info.streamW = containerW;
        info.streamH = containerH;
        info.notes.push_back("stream size taken from the container (no StreamMeta)");
    }
    info.mode = modeFromWidth(info.streamW, info.sideBySideProxy);
    if (info.mode == Mode::Unknown) {
        info.notes.push_back(std::format("unrecognised stream width {}; mode unknown", info.streamW));
    }
    if (info.mode == Mode::K4) {
        info.notes.push_back("4K crop/scale rule is unverified (derived from the 6K clip); check --focal-source");
    }

    // ---- frame rate ------------------------------------------------------------
    const double containerFps = slaveTrack->frameRate();
    const double metaFps = stream ? static_cast<double>(stream->video.fps) : 0.0;
    if (containerFps > 0.0) {
        info.fps = containerFps;
        if (metaFps > 0.0 && std::fabs(metaFps - containerFps) > 0.05) {
            info.notes.push_back(std::format("metadata fps {:.3f} differs from container fps {:.3f}", metaFps, containerFps));
        }
    } else if (metaFps > 0.0) {
        info.fps = metaFps;
        info.notes.push_back("frame rate taken from StreamMeta (container sample table has no timing)");
    } else {
        info.notes.push_back("frame rate unknown");
    }

    // ---- colour / lens mode / bit depth --------------------------------------
    if (stream && stream->present.test(4) && stream->colorMode != ColorMode::Unknown) {
        info.colorMode = stream->colorMode;
        info.colorModeFromMetadata = true;
    } else {
        info.colorMode = ColorMode::Unknown;
        info.colorModeFromMetadata = false;
        info.notes.push_back("colour mode not present in metadata; use the histogram auto-detect (osv_color) or --input");
    }
    if (stream) {
        info.lensMode = stream->extriLensMode;
    } else {
        info.notes.push_back("lens accessory mode unknown (no StreamMeta); assuming native lenses");
    }

    if (stream && stream->video.bitDepthValid && stream->video.bitDepth != 0) {
        info.bitDepth = stream->video.bitDepth;
    } else if (const HevcConfig* hevc = slaveTrack->hevc()) {
        info.bitDepth = hevc->bitDepthLuma();
        info.notes.push_back("bit depth taken from hvcC");
    } else if (slaveTrack->avc()) {
        // avc1 in an LRF is always 8-bit Baseline/Main/High.
        info.bitDepth = 8;
        info.notes.push_back("bit depth assumed 8 for an AVC track");
    } else {
        info.notes.push_back("bit depth unknown");
    }

    // ---- clip level ------------------------------------------------------------
    if (clip) {
        info.cameraModel = clip->header.productName;
        info.sensorW = clip->sensorW;
        info.sensorH = clip->sensorH;
        info.digitalFocalLength = clip->digitalFocalLength;
        if (info.digitalFocalLength <= 0.0f) {
            info.notes.push_back("digital focal length missing; stream scaling must be derived from the calibration");
        }
    } else {
        info.notes.push_back("no ClipMeta: sensor size and digital focal length unknown");
    }
    if (info.cameraModel.empty()) {
        // udta (c)too carries "Osmo 360" as well.
        if (!file.movie().udta.tool.empty()) {
            info.cameraModel = file.movie().udta.tool;
            info.notes.push_back("camera model taken from udta (c)too");
        } else {
            info.cameraModel = "Unknown";
        }
    }
    info.metaTrackId = meta ? meta->trackId() : 0;
    if (!meta) {
        const std::vector<const TrackInfo*> djmd = file.tracksOfKind(TrackKind::Djmd);
        if (!djmd.empty() && djmd.front()) {
            info.metaTrackId = djmd.front()->trackId;
        }
        info.notes.push_back("detected from the container only (no metadata track loaded)");
    }

    log::debug("meta: format {} {}x{} @ {:.3f} fps, colour {}, lens {}, tracks {}/{}", modeName(info.mode), info.streamW,
               info.streamH, info.fps, colorModeName(info.colorMode), extriLensModeName(info.lensMode),
               info.videoTrackIds[0], info.videoTrackIds[1]);
    return info;
}

}  // namespace osv::meta
