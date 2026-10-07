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
// the importer runs it on a clip's first frames and on every frame at Debug
// level).  A row is
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
// SDK-free on purpose: the three host layouts are pixelcopy::HostPixelFormat,
// so the unit tests can hand it a buffer they built themselves.
#pragma once

#include "PixelCopy.h"

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

}  // namespace osv::premiere::rowcheck
