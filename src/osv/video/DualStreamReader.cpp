// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DualStreamReader: two HevcStreamDecoders driven in parallel (one on a
// helper thread, one on the calling thread) with a presentation-time check,
// or one side-by-side decoder split into two half-width views.

#include "osv/video/DualStreamReader.h"

#include "osv/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace osv::video {

// =============================================================================
//  Impl
// =============================================================================
struct DualStreamReader::Impl {
    HevcStreamDecoder decoders[2];   ///< [0] slave, [1] master ([1] unused when side by side).
    bool sideBySide = false;         ///< Single stream split into halves.
    std::uint32_t frameCount = 0;
    std::uint32_t lensWidth = 0;
    std::uint32_t lensHeight = 0;
    double fps = 0.0;
    std::int64_t ptsToleranceUs = 1;  ///< One time-base tick expressed in microseconds (rounded up).

    /// Make a PlanarFrame16 that views the left or right half of `whole`
    /// while sharing its backing memory.
    static PlanarFrame16 halfView(const PlanarFrame16& whole, int half) {
        PlanarFrame16 out = whole;
        const std::uint32_t halfW = whole.width / 2;
        out.width = halfW;
        out.chromaW = (halfW + 1) / 2;
        if (half == 1) {
            // Right half: advance the luma pointer by halfW samples and the
            // chroma pointers by the corresponding chroma columns (doubled
            // when Cb/Cr are interleaved because each column is two elements).
            const std::size_t lumaOffset = halfW;
            const std::size_t chromaOffset = whole.chromaInterleaved ? static_cast<std::size_t>(whole.chromaW / 2) * 2
                                                                     : static_cast<std::size_t>(whole.chromaW / 2);
            if (out.plane[0]) {
                out.plane[0] = whole.plane[0] + lumaOffset;
            }
            if (out.plane[1]) {
                out.plane[1] = whole.plane[1] + chromaOffset;
            }
            if (out.plane[2]) {
                out.plane[2] = whole.plane[2] + chromaOffset;
            }
        }
        return out;
    }

    /// Frame `index` from one decoder plus its DeviceFrameRef (if any).
    struct LensResult {
        Result<PlanarFrame16> frame = Error{ErrorCode::Internal, "not decoded"};
        std::optional<DeviceFrameRef> device;
    };

    LensResult decodeLens(int lens, std::uint32_t index) {
        LensResult r;
        r.frame = decoders[lens].decodeFrame(index);
        if (r.frame.ok()) {
            r.device = decoders[lens].lastDeviceFrame();
        }
        return r;
    }
};

// =============================================================================
//  Lifetime
// =============================================================================
DualStreamReader::DualStreamReader() = default;
DualStreamReader::~DualStreamReader() = default;
DualStreamReader::DualStreamReader(DualStreamReader&& other) noexcept = default;
DualStreamReader& DualStreamReader::operator=(DualStreamReader&& other) noexcept = default;

// =============================================================================
//  open
// =============================================================================
Result<DualStreamReader> DualStreamReader::open(const std::filesystem::path& path, const meta::FormatInfo& format,
                                                const DecoderOptions& options) {
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty path"};
    }
    DualStreamReader reader;
    reader.m_impl = std::make_unique<Impl>();
    Impl& impl = *reader.m_impl;
    impl.sideBySide = format.sideBySideProxy;

    if (impl.sideBySide) {
        // ---- one track, two halves ------------------------------------------
        const std::uint32_t trackId = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
        OSV_TRY_ASSIGN(impl.decoders[0], HevcStreamDecoder::open(path, trackId, options));
        const HevcStreamDecoder& d = impl.decoders[0];
        if (d.width() < 2 || (d.width() % 2) != 0) {
            return Error{ErrorCode::Malformed, "side-by-side stream width " + std::to_string(d.width()) +
                                                   " cannot be split into two lenses"};
        }
        impl.frameCount = d.frameCount();
        impl.lensWidth = d.width() / 2;
        impl.lensHeight = d.height();
        impl.fps = d.fps();
        const TimeBase tb = d.timeBase();
        impl.ptsToleranceUs = static_cast<std::int64_t>(std::ceil(1e6 * static_cast<double>(tb.num) / static_cast<double>(tb.den)));
        return reader;
    }

    // ---- two tracks ----------------------------------------------------------
    for (int lens = 0; lens < 2; ++lens) {
        const std::uint32_t trackId = format.videoTrackIds[static_cast<std::size_t>(lens)];
        if (trackId == 0) {
            return Error{ErrorCode::InvalidArgument, "FormatInfo has no video track id for lens " + std::to_string(lens)};
        }
        OSV_TRY_ASSIGN(impl.decoders[lens], HevcStreamDecoder::open(path, trackId, options));
    }
    const HevcStreamDecoder& a = impl.decoders[0];
    const HevcStreamDecoder& b = impl.decoders[1];
    if (a.width() != b.width() || a.height() != b.height()) {
        return Error{ErrorCode::Malformed, "lens streams differ in size: " + std::to_string(a.width()) + "x" +
                                               std::to_string(a.height()) + " vs " + std::to_string(b.width()) + "x" +
                                               std::to_string(b.height())};
    }
    if (a.frameCount() != b.frameCount()) {
        log::warn("video: lens streams have {} and {} frames; using the smaller count", a.frameCount(), b.frameCount());
    }
    if (std::fabs(a.fps() - b.fps()) > 1e-3) {
        return Error{ErrorCode::Malformed, "lens streams differ in frame rate"};
    }
    impl.frameCount = std::min(a.frameCount(), b.frameCount());
    impl.lensWidth = a.width();
    impl.lensHeight = a.height();
    impl.fps = a.fps();
    // Tolerance: one tick of the coarser time base, in microseconds.
    std::int64_t tol = 1;
    for (const HevcStreamDecoder* d : {&a, &b}) {
        const TimeBase tb = d->timeBase();
        if (tb.num > 0 && tb.den > 0) {
            tol = std::max(tol, static_cast<std::int64_t>(std::ceil(1e6 * static_cast<double>(tb.num) / static_cast<double>(tb.den))));
        }
    }
    impl.ptsToleranceUs = tol;
    return reader;
}

bool DualStreamReader::isOpen() const noexcept { return m_impl != nullptr && m_impl->decoders[0].isOpen(); }

// =============================================================================
//  read
// =============================================================================
Result<FramePair> DualStreamReader::read(std::uint32_t index) {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "reader not open"};
    }
    Impl& impl = *m_impl;
    if (index >= impl.frameCount) {
        return Error{ErrorCode::InvalidArgument, "frame " + std::to_string(index) + " out of range (" +
                                                     std::to_string(impl.frameCount) + " frames)"};
    }
    FramePair pair;
    pair.index = index;

    if (impl.sideBySide) {
        // ---- decode once, split ----------------------------------------------
        Impl::LensResult whole = impl.decodeLens(0, index);
        if (!whole.frame.ok()) {
            return Error(whole.frame.error());
        }
        const PlanarFrame16& frame = whole.frame.value();
        if (frame.width != impl.lensWidth * 2) {
            return Error{ErrorCode::Decoder, "side-by-side frame width changed mid-stream"};
        }
        pair.lens[0] = Impl::halfView(frame, 0);
        pair.lens[1] = Impl::halfView(frame, 1);
        pair.ptsUs = frame.ptsUs;
        // Device halves: same pitch, luma pointer shifted by half a row of
        // samples (2 bytes each for P010, 1 byte for NV12).
        if (whole.device && whole.device->valid()) {
            const std::size_t bytesPerSample = (whole.device->bitShift == 6 || whole.device->bitDepth > 8) ? 2u : 1u;
            const std::size_t byteOffset = static_cast<std::size_t>(impl.lensWidth) * bytesPerSample;
            for (int lens = 0; lens < 2; ++lens) {
                DeviceFrameRef ref = *whole.device;
                ref.width = impl.lensWidth;
                if (lens == 1) {
                    ref.yDevice = static_cast<std::uint8_t*>(ref.yDevice) + byteOffset;
                    ref.uvDevice = static_cast<std::uint8_t*>(ref.uvDevice) + byteOffset;
                }
                pair.device[static_cast<std::size_t>(lens)] = ref;
            }
        }
        return pair;
    }

    // ---- two decoders, two threads ----------------------------------------------
    // Lens 0 runs on a helper thread while lens 1 decodes on this one; the
    // helper's result is captured by value and any exception (there should
    // be none, decoders never throw) is converted into an error.
    Impl::LensResult slave;
    std::string threadFailure;
    std::thread worker([&impl, &slave, &threadFailure, index]() noexcept {
        try {
            slave = impl.decodeLens(0, index);
        } catch (const std::exception& e) {
            threadFailure = e.what();
        } catch (...) {
            threadFailure = "unknown exception";
        }
    });
    Impl::LensResult master = impl.decodeLens(1, index);
    worker.join();

    if (!threadFailure.empty()) {
        return Error{ErrorCode::Internal, "lens 0 decoder thread failed: " + threadFailure};
    }
    if (!slave.frame.ok()) {
        return Error{slave.frame.error().code, "lens 0: " + slave.frame.error().message};
    }
    if (!master.frame.ok()) {
        return Error{master.frame.error().code, "lens 1: " + master.frame.error().message};
    }
    const PlanarFrame16& s = slave.frame.value();
    const PlanarFrame16& m = master.frame.value();
    // Both tracks are written by the same encoder clock; anything beyond a
    // single tick means the tracks are not the pair we think they are.
    const std::int64_t delta = s.ptsUs > m.ptsUs ? s.ptsUs - m.ptsUs : m.ptsUs - s.ptsUs;
    if (delta > impl.ptsToleranceUs) {
        return Error{ErrorCode::Decoder, "lens presentation times differ at frame " + std::to_string(index) + ": " +
                                             std::to_string(s.ptsUs) + " us vs " + std::to_string(m.ptsUs) + " us"};
    }
    pair.lens[0] = s;
    pair.lens[1] = m;
    pair.ptsUs = s.ptsUs;
    if (slave.device) {
        pair.device[0] = *slave.device;
    }
    if (master.device) {
        pair.device[1] = *master.device;
    }
    return pair;
}

// =============================================================================
//  Accessors
// =============================================================================
std::uint32_t DualStreamReader::frameCount() const noexcept { return m_impl ? m_impl->frameCount : 0; }

double DualStreamReader::fps() const noexcept { return m_impl ? m_impl->fps : 0.0; }

std::uint32_t DualStreamReader::lensWidth() const noexcept { return m_impl ? m_impl->lensWidth : 0; }

std::uint32_t DualStreamReader::lensHeight() const noexcept { return m_impl ? m_impl->lensHeight : 0; }

bool DualStreamReader::isSideBySide() const noexcept { return m_impl != nullptr && m_impl->sideBySide; }

const HevcStreamDecoder* DualStreamReader::decoder(int lens) const noexcept {
    if (!m_impl || lens < 0 || lens > 1) {
        return nullptr;
    }
    return &m_impl->decoders[m_impl->sideBySide ? 0 : lens];
}

HevcStreamDecoder* DualStreamReader::decoder(int lens) noexcept {
    if (!m_impl || lens < 0 || lens > 1) {
        return nullptr;
    }
    return &m_impl->decoders[m_impl->sideBySide ? 0 : lens];
}

}  // namespace osv::video
