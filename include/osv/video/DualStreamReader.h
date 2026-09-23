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
#pragma once

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/Decoder.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace osv::video {

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
    Result<FramePair> read(std::uint32_t index);

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
