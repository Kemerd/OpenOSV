// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FormatDetector: derive a FormatInfo from an opened file and (optionally)
// its primary metadata track.  Metadata wins whenever it is present; the
// container is the fallback so a file with a damaged djmd track still yields
// a usable description (with notes saying what was guessed).
#pragma once

#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"

namespace osv::meta {

class FormatDetector {
public:
    /// Detect the format of `file`.  `meta` may be null (container-only
    /// detection).  NotFound when the file has no video track at all.
    [[nodiscard]] static Result<FormatInfo> detect(const OsvFile& file, const MetadataTrack* meta);

    /// Map a per-lens stream width to a Mode (1920 -> K4, 3000 -> K6,
    /// 3840 -> K8, anything else -> Unknown).  `sideBySide` forces Lrf.
    [[nodiscard]] static Mode modeFromWidth(std::uint32_t width, bool sideBySide) noexcept;
};

}  // namespace osv::meta
