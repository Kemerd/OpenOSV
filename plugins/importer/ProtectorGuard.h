// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProtectorGuard: the once-per-clip runtime check behind the lens-protector
// correction.
//
// The direction of the correction is the one DJI's own software applies
// (geom::ProtectorDirection::Forward).  We have never seen footage
// shot through a protector, so before trusting it the importer scores the
// seam overlap of frame 0 with no correction, forward and inverse
// (render::scoreLensProtector) and keeps forward unless it scores clearly
// worse than no correction.
//
// Cost and caching
// ----------------
// The check needs one decoded frame pair, which the rig build (open time)
// does not have, so it opens a short-lived decoder of its own: D3D11VA with
// a software fallback, frame 0 only (a sync sample, so no GOP walk).  That
// is a few hundred milliseconds, paid ONCE per clip:
//
//   * in memory, keyed by file identity (absolute path, size, last write
//     time), so the engine's instance of the same clip, quiet / unquiet
//     cycles and Source Settings changes never measure again;
//   * on disk, as one line per clip in
//     %LOCALAPPDATA%\OpenOSV\lens-protector-guard.tsv, so a project reopened
//     in a new session does not measure again either.  A changed file (size
//     or time) is a different key, so an edited clip is re-measured.
//
// Measurements run one at a time under a process-wide mutex, so two
// instances of the same clip opening together measure it once.
//
// Failure handling: when no frame can be decoded the static direction
// (forward) is used, logged as unverified, and NOT cached, so a later open
// can still verify it.
#pragma once

#include "osv/geom/Blend.h"
#include "osv/geom/LensProtector.h"
#include "osv/geom/LensRig.h"
#include "osv/meta/FormatInfo.h"

#include <array>
#include <filesystem>
#include <string>

namespace osv::premiere {

/// What the guard decided for one clip.
struct ProtectorGuardResult {
    geom::ProtectorDirection direction = geom::ProtectorDirection::Forward;  ///< What to apply.
    bool measured = false;        ///< The direction was checked on decoded pixels (now or earlier).
    bool fromCache = false;       ///< Served from the memory or disk cache (no decode this time).
    std::array<double, 3> ncc{};  ///< None / forward / inverse overlap NCC (0 when not measured).
    double millis = 0.0;          ///< Wall time of this call's measurement (0 for a cache hit).
    std::string summary;          ///< One human line for the log and the Properties notes.
};

/// Decide the protector direction for `path`.  `format` describes the clip
/// (for the decoder), `baseRig` / `baseBlend` are the bare-lens geometry the
/// correction will be applied to.  Never throws; never fails - the worst
/// case is the unverified static direction.
[[nodiscard]] ProtectorGuardResult resolveProtectorGuard(const std::filesystem::path& path,
                                                         const meta::FormatInfo& format,
                                                         const geom::LensRig& baseRig,
                                                         const geom::BlendParams& baseBlend) noexcept;

/// Name of the on-disk cache file (inside the plug-in log directory).
inline constexpr const wchar_t* kProtectorGuardCacheFile = L"lens-protector-guard.tsv";

}  // namespace osv::premiere
