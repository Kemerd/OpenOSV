// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DualStreamReader: delivers the two lens images of one time instant as a
// FramePair.
//
// For a native .OSV the two hvc1 tracks are decoded by two independent
// HevcStreamDecoder instances running on two threads; their presentation
// timestamps must agree to the tick or the pair is rejected.  For the .LRF
// proxy (FormatInfo::sideBySideProxy) there is a single side-by-side track
// that is decoded once and split into a left half (stream 0 / slave lens)
// and a right half (stream 1 / master lens) sharing the same backing memory.
//
// The proxy's hardware pictures can be checked against software
// (OPENOSV_VERIFY_HW_DECODE=1, ShadowDecodeVerifier below): a diagnostic for
// a host session that shows a damaged proxy frame no offline decode
// reproduces.  Off by default; never applied to the native streams, where a
// software decode costs about a second per landing.
#pragma once

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/Decoder.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace osv::video {

// =============================================================================
//  Comparing two decodes of one picture
// =============================================================================

/// Whole-frame luma PSNR (dB, 10-bit peak) below which two decodes of the
/// same picture count as different.  H.264 / HEVC decoding is bit-exact by
/// specification, so any honest pair is infinite; 60 dB leaves room only for
/// a stray rounding difference in a vendor's driver.
inline constexpr double kDecodeMismatchPsnrDb = 60.0;

/// A luma row is "bad" when its mean squared error exceeds this: the row-wise
/// form of the same 60 dB (1023^2 / 10^6, on the 10-bit scale).
inline constexpr double kDecodeBadRowMse = 1023.0 * 1023.0 / 1.0e6;

/// @brief The row-by-row luma comparison of two decodes of one picture.
struct LumaComparison {
    bool comparable = false;   ///< Both frames valid and the same size (nothing else below is set otherwise).
    double psnrDb = std::numeric_limits<double>::infinity();  ///< Whole-frame luma PSNR, 10-bit peak.
    std::uint32_t height = 0;          ///< Rows compared.
    std::uint32_t firstBadRow = 0;     ///< First row with MSE > kDecodeBadRowMse (== height when none).
    std::uint32_t lastBadRow = 0;      ///< Last such row (0 when none).
    std::uint32_t badRows = 0;         ///< How many rows are bad.
    std::uint32_t badRowsBottom = 0;   ///< Of those, rows in the bottom half (row >= height / 2).

    /// True when the two pictures differ beyond kDecodeMismatchPsnrDb.
    [[nodiscard]] bool mismatch() const noexcept { return comparable && psnrDb < kDecodeMismatchPsnrDb; }
};

/// @brief Compare the luma of two decodes of the same picture, row by row.
///
/// Samples are compared on the 10-bit scale (sample >> bitShift), so a
/// hardware P010 / NV12 picture and a software planar one compare by value.
/// The rows say WHERE a damaged picture went wrong - the first bad row, and
/// how much of the damage sits in the bottom half - which a single PSNR
/// cannot.
///
/// @param a  One decode (the hardware one, by convention).
/// @param b  The other (the software reference).
/// @return The comparison; `comparable` is false when either frame is not a
///         valid host picture or the sizes differ.
[[nodiscard]] LumaComparison compareDecodedLuma(const PlanarFrame16& a, const PlanarFrame16& b) noexcept;

/// %LOCALAPPDATA%\OpenOSV\decode-mismatch (Windows), ~/Library/Logs/OpenOSV/
/// decode-mismatch (macOS), or <temp>/OpenOSV/decode-mismatch elsewhere:
/// where ShadowDecodeVerifier writes the two luma planes of a mismatch.
/// Empty when none of the base folders can be determined.
[[nodiscard]] std::filesystem::path defaultDecodeMismatchDirectory();

// =============================================================================
//  ShadowDecodeVerifier
// =============================================================================

/// @brief Re-decodes hardware pictures of a track in software and compares.
///
/// The question it answers: when a host shows a damaged proxy frame, is the
/// picture the hardware decoder returned already wrong (then this logs it,
/// names the frame, its distance from the sync sample and the first bad row,
/// and hands back the software picture to deliver instead), or is it right
/// and the damage came later in the chain?
///
/// It owns a private software HevcStreamDecoder of the reference file (two
/// frame threads, the primary's sample feed) and decodes the same index the
/// primary did.  On a mismatch it writes both luma planes as 16-bit PGM
/// (maxval 1023) to the dump directory - the first mismatch after each sync
/// sample only, and at most kMaxDumps per verifier, because a damaged GOP
/// would otherwise write 8 MB per proxy frame.
///
/// Not thread-safe: the owning reader serialises calls, as it does decodes.
class ShadowDecodeVerifier {
public:
    /// Mismatch dumps (pairs of PGMs) one verifier writes at most.
    static constexpr std::uint32_t kMaxDumps = 16;

    /// What one check() found.
    struct Verdict {
        LumaComparison comparison;           ///< The row-wise comparison.
        /// The software picture, set exactly when the comparison is a
        /// mismatch: the caller delivers it instead of the primary picture.
        std::optional<PlanarFrame16> replacement;
        std::filesystem::path dumpedPrimary;   ///< PGM of the primary's luma (empty when not dumped).
        std::filesystem::path dumpedReference; ///< PGM of the reference's luma (empty when not dumped).
    };

    ShadowDecodeVerifier();
    ~ShadowDecodeVerifier();
    ShadowDecodeVerifier(ShadowDecodeVerifier&& other) noexcept;
    ShadowDecodeVerifier& operator=(ShadowDecodeVerifier&& other) noexcept;
    ShadowDecodeVerifier(const ShadowDecodeVerifier&) = delete;
    ShadowDecodeVerifier& operator=(const ShadowDecodeVerifier&) = delete;

    /// @brief Open the software reference decoder.
    /// @param reference       The file the reference decodes - the primary's own file in
    ///                        production; a test passes a clean copy of a file it damaged.
    /// @param trackId         The track (1-based), as for HevcStreamDecoder::open.
    /// @param primaryOptions  The primary decoder's options; the reference takes its
    ///                        sample feed and replaces the rest (software, two threads).
    /// @param dumpDirectory   Where mismatch PGMs go (created on the first dump); empty
    ///                        disables dumps.
    /// @return The verifier, or the reference decoder's open error.
    static Result<ShadowDecodeVerifier> open(const std::filesystem::path& reference, std::uint32_t trackId,
                                             const DecoderOptions& primaryOptions,
                                             const std::filesystem::path& dumpDirectory);

    /// @brief Decode `index` in software and compare it with `primary`.
    ///
    /// Logs a warning for a mismatch with everything the decoders know: the
    /// clip, the frame and its distance from `previousSync`, the PSNR, the
    /// first and last bad row and the bad-row counts, the primary's path,
    /// libavcodec's damage flags and the D3D11 surface (`primaryInfo`).
    ///
    /// @param index         The frame both decodes are of.
    /// @param primary       The picture under test (any layout).
    /// @param primaryInfo   What the primary decoder said about it (may be empty).
    /// @param previousSync  The sync sample the primary decoded from (may be empty).
    /// @param clipName      The clip, for the log line and the dump file names.
    /// @return The verdict; an error only when the reference could not decode
    ///         the frame (the caller then keeps the primary picture).
    Result<Verdict> check(std::uint32_t index, const PlanarFrame16& primary,
                          const std::optional<DecodedFrameInfo>& primaryInfo,
                          std::optional<std::uint32_t> previousSync, std::string_view clipName);

    /// True when open() succeeded and the object has not been moved from.
    [[nodiscard]] bool isOpen() const noexcept;

    /// Pictures compared so far, and how many of them mismatched.
    [[nodiscard]] std::uint64_t checked() const noexcept;
    [[nodiscard]] std::uint64_t mismatched() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// =============================================================================
//  DualStreamReader
// =============================================================================

class DualStreamReader {
public:
    /// An unopened reader (see HevcStreamDecoder for why this exists).
    DualStreamReader();
    ~DualStreamReader();

    DualStreamReader(DualStreamReader&& other) noexcept;
    DualStreamReader& operator=(DualStreamReader&& other) noexcept;
    DualStreamReader(const DualStreamReader&) = delete;
    DualStreamReader& operator=(const DualStreamReader&) = delete;

    /// Open the two lens tracks named by `format.videoTrackIds` (or the one
    /// side-by-side track when `format.sideBySideProxy` is set).  Both
    /// decoders receive the same `options`, except that each lens takes its
    /// own shared hardware device slot (options.hwDeviceSlot * 2 + lens) so
    /// the two lenses never queue behind one device lock.  The two lenses
    /// open in parallel.  Fails when the tracks disagree in frame count or
    /// dimensions.
    static Result<DualStreamReader> open(const std::filesystem::path& path, const meta::FormatInfo& format,
                                         const DecoderOptions& options = {});

    // ---- what the reader was opened on (ReaderPool matches these) ---------

    /// The path open() was given (empty when not open).
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /// The options open() was given, exactly as passed (default options when
    /// not open).
    [[nodiscard]] DecoderOptions options() const noexcept;

    /// The track ids open() used: both lens tracks, or the side-by-side
    /// track twice ({0, 0} when not open).
    [[nodiscard]] std::array<std::uint32_t, 2> trackIds() const noexcept;

    /// Identity of the file VERSION the reader decodes: absolute path, size
    /// and modification time as they were at open().  Empty when they could
    /// not be read, which keeps the reader out of any pool.
    [[nodiscard]] const std::wstring& fileIdentity() const noexcept;

    /// True when open() succeeded and the object has not been moved from.
    [[nodiscard]] bool isOpen() const noexcept;

    /// Decode frame `index` from both lenses.  The pair's ptsUs is the
    /// (identical) presentation time; a mismatch between the two tracks of
    /// more than one time-base tick is reported as a Decoder error.
    ///
    /// Side-by-side proxy with OPENOSV_VERIFY_HW_DECODE=1 in the environment
    /// when the reader opened: every HARDWARE picture is also decoded in
    /// software (ShadowDecodeVerifier) and, when the two differ, the software
    /// picture is the one returned and the mismatch is logged and dumped.
    Result<FramePair> read(std::uint32_t index);

    /// The shadow verifier's counters (pictures checked, mismatches); both 0
    /// when verification is off or nothing hardware-decoded was checked yet.
    [[nodiscard]] std::uint64_t verifiedFrames() const noexcept;
    [[nodiscard]] std::uint64_t verifyMismatches() const noexcept;

    /// Number of frames available from both lenses (the smaller count when
    /// the tracks differ, which never happens on camera-written files).
    [[nodiscard]] std::uint32_t frameCount() const noexcept;

    /// Frames per second of the underlying stream(s).
    [[nodiscard]] double fps() const noexcept;

    /// Per-lens output width / height (half the coded width on the proxy).
    [[nodiscard]] std::uint32_t lensWidth() const noexcept;
    [[nodiscard]] std::uint32_t lensHeight() const noexcept;

    /// True when the frames are split halves of one side-by-side stream.
    [[nodiscard]] bool isSideBySide() const noexcept;

    /// The decoder serving `lens` (0 = slave, 1 = master).  On a side-by-side
    /// proxy both lenses map to the single decoder.  nullptr when not open
    /// or `lens` is out of range.
    [[nodiscard]] const HevcStreamDecoder* decoder(int lens) const noexcept;
    [[nodiscard]] HevcStreamDecoder* decoder(int lens) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::video
