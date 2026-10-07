// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FrameRowCheck: a cheap look at a frame the importer has just handed to the
// host - did every row of it come out, or is a band of rows transparent or
// black?
//
// Why it exists.  A field report showed an .LRF frame whose bottom three
// eighths were black: a flat, full-width cut, which no mock-host or osvtool
// render reproduced at any size, format, intent or renderer.  Every frame
// path in the importer either writes every row of the PPix or fails the whole
// frame (and then disposes it), so the band most likely came from outside the
// plug-in - but "it cannot happen" is not evidence.  This check turns the
// user's own log into that evidence: run on the delivered PPix, it either
// names the rows that came out empty, together with the size the host asked
// for and the size it got, or stays silent.
//
// What counts.  kSampleColumns evenly spaced columns of EVERY row are read
// (64 x 3840 = 245 760 reads for an 8K frame, a few milliseconds at most, so
// the importer runs it on the first kFramesPerGeometry frames of every size,
// format and render quality it delivers - FrameBudget - and on every frame
// only at Trace level).  A row is
//
//   * transparent  when the mean of its sampled alphas is below 0.5 - the
//                  coverage alpha a straight-alpha host composites over
//                  black, so it LOOKS black on the timeline;
//   * black        when every sampled B, G and R is exactly zero - real
//                  pixels never are, across a whole 360-degree row, except
//                  in a scene with the black level crushed at a pole.
//
// The longest run of rows that are either is reported in PICTURE rows (row 0
// = the top of the frame), whatever the layout's own row order is, so "rows
// 640..1023 of 1024" reads as "the bottom 37.5 %" without any arithmetic.
//
// Which frames and requests.  Two small, allocation-free memos decide what
// the importer looks at, so the diagnostics cost nothing in steady playback:
//
//   * FrameBudget - the first kFramesPerGeometry rendered frames of every
//                   delivered size, layout and quality (draft or full).  A
//                   host asks one clip at several sizes and for several
//                   purposes (bin thumbnail, Source Monitor, sequence,
//                   export), and the frame a band would appear in is often
//                   the LAST kind it asks for, so one budget per clip would
//                   be spent on thumbnails before the frame that matters;
//   * SeenSizes   - the requested sizes whose "delivering the nearest size"
//                   line was already offered to the log, so a repeat of the
//                   same mismatched request costs a few loads and no string.
//
// SDK-free on purpose: the three host layouts are pixelcopy::HostPixelFormat,
// so the unit tests can hand it a buffer they built themselves.
#pragma once

#include "PixelCopy.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace osv::premiere::rowcheck {

/// Columns sampled in every row (all of them for a frame narrower than this).
inline constexpr std::uint32_t kSampleColumns = 64;

/// Alpha below which a sample counts as transparent, and below which a row's
/// mean sampled alpha makes the row transparent.
inline constexpr float kTransparentAlpha = 0.5f;

/// What one scan of a host frame found.
///
/// Every row number is a PICTURE row: 0 is the top scanline, height - 1 the
/// bottom one.  A default-constructed scan (scanned == false) is what a frame
/// that could not be read safely yields: null pixels, a zero pitch, a pitch
/// narrower than a row, an absurd size or a layout outside the enum.
struct RowScan {
    bool scanned = false;               ///< False when the frame was refused (nothing below is meaningful).
    std::uint32_t width = 0;            ///< Frame width in pixels.
    std::uint32_t height = 0;           ///< Frame height in pixels (= rows scanned).
    std::uint32_t columns = 0;          ///< Samples per row: min(kSampleColumns, width).

    std::uint32_t transparentRows = 0;  ///< Rows whose mean sampled alpha is below kTransparentAlpha.
    std::uint32_t blackRows = 0;        ///< Rows whose sampled B, G and R are all exactly zero.
    std::uint32_t badRows = 0;          ///< Rows that are transparent, black or both.

    /// The longest run of consecutive bad rows (the first one on a tie):
    /// picture rows runFirst..runLast inclusive, runLength of them; 0 = no
    /// bad row at all.
    std::uint32_t runFirst = 0;
    std::uint32_t runLast = 0;
    std::uint32_t runLength = 0;
    std::uint32_t runTransparentRows = 0;  ///< Rows of that run that are transparent.
    std::uint32_t runBlackRows = 0;        ///< Rows of that run that are black.

    /// The row with the fewest opaque samples (alpha >= kTransparentAlpha; the
    /// first one on a tie) and that count, so a test can demand "every row
    /// opaque over at least 95 % of its samples" with one comparison.
    std::uint32_t worstRow = 0;
    std::uint32_t worstRowOpaque = 0;

    /// Samples with a NaN or infinite channel (32f only; integer codes are
    /// always finite).  A non-finite alpha counts as transparent.
    std::uint64_t nonFiniteSamples = 0;

    /// True when the frame was scanned and at least one row came out empty.
    [[nodiscard]] bool defective() const noexcept { return scanned && runLength > 0; }

    /// Fraction of the worst row's samples that are opaque (0 when nothing
    /// was scanned).
    [[nodiscard]] double worstRowOpaqueFraction() const noexcept {
        return (scanned && columns > 0) ? static_cast<double>(worstRowOpaque) / static_cast<double>(columns) : 0.0;
    }

    /// The longest run's share of the frame height, in percent (0 when there
    /// is no run).
    [[nodiscard]] double runPercentOfHeight() const noexcept {
        return (scanned && height > 0) ? 100.0 * static_cast<double>(runLength) / static_cast<double>(height) : 0.0;
    }
};

/// Scan a BOTTOM-LEFT host frame (the importer's PPix layout: host row 0 is
/// the bottom scanline, the pitch may be negative) in `format`.
///
/// Reads kSampleColumns evenly spaced pixels of every row - nothing else, no
/// allocation - and never writes.  A frame that fails ConstHostFrame::valid()
/// for the layout's pixel size, or a layout outside the enum, is refused and
/// yields a scan with scanned == false; that is the only failure.
[[nodiscard]] RowScan scanHostFrame(const pixelcopy::ConstHostFrame& frame,
                                    pixelcopy::HostPixelFormat format) noexcept;

/// What the longest run is made of: "transparent", "black" or "transparent
/// and black" (rows of both kinds, or rows that are both); "clean" when the
/// scan found nothing and "unscanned" when the frame was refused.
[[nodiscard]] const char* runKindName(const RowScan& scan) noexcept;

/// The run as one log phrase: "rows 640..1023 (37.5% of 1024) came out
/// black".  Empty when the scan found nothing (or was refused), and when the
/// text cannot be built (an allocation failure never escapes).
[[nodiscard]] std::string describeRun(const RowScan& scan) noexcept;

// ---------------------------------------------------------------------------
//  Which frames to check, which request sizes to log
// ---------------------------------------------------------------------------

/// Rendered frames self-checked per delivered geometry (width, height, host
/// layout and draft or full quality) when the log is below Trace level.
/// Three covers the first landing of a size plus the first frames played or
/// exported at it.
inline constexpr std::uint32_t kFramesPerGeometry = 3;

/// Delivered geometries one FrameBudget tells apart.  A clip advertises
/// three sizes (native, half, quarter); a host uses one or two of the three
/// layouts, in drafts and full frames - a dozen at most, and a change of
/// Output Size mid-session brings new ones, which a full table still admits.
inline constexpr std::size_t kBudgetGeometries = 16;

/// Request sizes one SeenSizes remembers.  A host asks a clip at a handful of
/// sizes at most (thumbnail, Source Monitor, sequence, export).
inline constexpr std::size_t kSeenSizeSlots = 8;

static_assert(kFramesPerGeometry >= 1, "every new geometry must get at least one checked frame");
static_assert(kBudgetGeometries >= 1 && kSeenSizeSlots >= 1, "the memos need at least one slot");

/// Which of one clip's rendered frames to self-check: the first
/// kFramesPerGeometry of EVERY delivered geometry, not of the clip.
///
/// The budget restarts for each new (width, height, layout, draft), so a
/// size the host first asks for late - the sequence or export size, after the
/// bin thumbnail and the Source Monitor have been served - is still checked,
/// and so is the first full-quality frame at a size whose budget draft
/// thumbnails or scrubbing already spent (a draft skips the seam search, so
/// it is a different render of the same size).  Steady playback at a
/// geometry already checked pays one short table walk.  Once all
/// kBudgetGeometries slots are in use a new geometry takes the oldest slot
/// (round robin), so the memo never stops admitting new geometries; one
/// evicted that way gets a fresh budget if it returns.
///
/// No allocation, no lock of its own and NOT thread-safe: the importer keeps
/// one per clip and calls it under that clip's instance lock.
class FrameBudget {
public:
    /// True when this frame is within its geometry's budget, and count it.
    /// `draft` is the request's quality (the importer's isDraftRequest()).
    /// False for a geometry already checked kFramesPerGeometry times, an
    /// empty size, or a layout outside the enum (nothing could be scanned).
    [[nodiscard]] bool admit(std::uint32_t width, std::uint32_t height, pixelcopy::HostPixelFormat format,
                             bool draft) noexcept;

private:
    /// One remembered geometry and the frames of it admitted so far.
    struct Slot {
        std::uint32_t width = 0;   ///< 0 marks an unused slot (admit() never stores an empty size).
        std::uint32_t height = 0;  ///< Delivered height in pixels.
        pixelcopy::HostPixelFormat format = pixelcopy::HostPixelFormat::Bgra32f;  ///< Delivered layout.
        bool draft = false;        ///< Draft (no seam search) or full-quality frames.
        std::uint32_t frames = 0;  ///< Frames of this geometry admitted so far (<= kFramesPerGeometry).
    };
    std::array<Slot, kBudgetGeometries> m_slots{};  ///< The remembered geometries, oldest first until they wrap.
    std::size_t m_next = 0;                         ///< Slot the next new geometry takes.
};

/// The request sizes already offered to a once-per-size log line, remembered
/// without a lock or an allocation, so a repeat costs a few relaxed loads.
///
/// Thread-safe: Premiere calls imGetSourceVideo for one clip from several
/// threads, and the importer consults this BEFORE it takes the instance lock.
/// A size is claimed with one compare-exchange on the first empty slot, and
/// slots only ever go from empty to full, so of two threads racing with the
/// same new size exactly one is told "first".  Once all kSeenSizeSlots are
/// taken, every size not among them answers "first" each time and the dedupe
/// falls back to PluginLog::once() - slower, never wrong.
class SeenSizes {
public:
    /// True the first time (width, height) is offered - the caller then
    /// writes its line - and false for a size already remembered.  (0, 0),
    /// the empty-slot marker and the host's "any size", is never "first".
    [[nodiscard]] bool firstTime(std::int32_t width, std::int32_t height) noexcept;

private:
    std::array<std::atomic<std::uint64_t>, kSeenSizeSlots> m_sizes{};  ///< Packed (width << 32 | height); 0 = empty.
};

}  // namespace osv::premiere::rowcheck
