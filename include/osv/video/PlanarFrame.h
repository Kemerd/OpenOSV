// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Decoded frame descriptors shared between the video, geom and render modules.
//
// A PlanarFrame16 describes 10-bit (or 8-bit widened) YCbCr 4:2:0 data as
// three planes of uint16 samples.  Two memory layouts are supported without
// copying:
//   * planar   (FFmpeg yuv420p10le): three separate planes, samples 0..1023,
//               bitShift = 0, chromaInterleaved = false
//   * biplanar (P010 from hardware decoders): luma plane plus one interleaved
//               CbCr plane, samples in the top 10 bits, bitShift = 6,
//               chromaInterleaved = true (plane[2] == plane[1] + 1, strideC
//               counts uint16 elements per row of the interleaved plane)
// The render kernels normalise with `value >> bitShift` so both look the same.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace osv::video {

struct PlanarFrame16 {
    std::uint32_t width = 0;                  ///< Luma width in pixels.
    std::uint32_t height = 0;                 ///< Luma height in pixels.
    std::uint32_t chromaW = 0;                ///< Chroma plane width ((width+1)/2 for 4:2:0).
    std::uint32_t chromaH = 0;                ///< Chroma plane height.
    std::array<const std::uint16_t*, 3> plane{};   ///< Y, Cb, Cr sample pointers.
    std::array<std::size_t, 3> strideElems{};     ///< Row pitch of each plane in uint16 elements.
    std::uint8_t bitDepth = 10;               ///< Significant bits (10 for Osmo 360).
    std::uint8_t bitShift = 0;                ///< Right shift to bring samples to `bitDepth` scale.
    bool chromaInterleaved = false;           ///< True for P010-style CbCr interleaving.
    bool narrowRange = true;                  ///< Limited (TV) range coding.
    std::int64_t ptsUs = 0;                   ///< Presentation time in microseconds.
    std::uint32_t frameIndex = 0;             ///< Zero-based frame number in the stream.
    std::shared_ptr<void> owner;              ///< Keeps the backing memory alive (AVFrame or buffer).

    /// True when every plane pointer and dimension is usable.
    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && chromaW > 0 && chromaH > 0 && plane[0] != nullptr &&
               plane[1] != nullptr && plane[2] != nullptr && strideElems[0] >= width &&
               strideElems[1] >= (chromaInterleaved ? 2 * chromaW : chromaW) &&
               strideElems[2] >= (chromaInterleaved ? 2 * chromaW : chromaW);
    }

    /// Luma sample at (x, y) already shifted to `bitDepth` scale; clamps to
    /// the frame so it can never read out of bounds.
    [[nodiscard]] std::uint16_t luma(std::uint32_t x, std::uint32_t y) const noexcept {
        if (!plane[0] || width == 0 || height == 0) {
            return 0;
        }
        x = x < width ? x : width - 1;
        y = y < height ? y : height - 1;
        return static_cast<std::uint16_t>(plane[0][static_cast<std::size_t>(y) * strideElems[0] + x] >> bitShift);
    }

    /// Chroma sample (c = 1 for Cb, 2 for Cr) at chroma coordinates (x, y).
    [[nodiscard]] std::uint16_t chroma(int c, std::uint32_t x, std::uint32_t y) const noexcept {
        if (c < 1 || c > 2 || !plane[static_cast<std::size_t>(c)] || chromaW == 0 || chromaH == 0) {
            return 0;
        }
        x = x < chromaW ? x : chromaW - 1;
        y = y < chromaH ? y : chromaH - 1;
        const std::size_t step = chromaInterleaved ? 2 : 1;
        const std::uint16_t* p = plane[static_cast<std::size_t>(c)];
        return static_cast<std::uint16_t>(p[static_cast<std::size_t>(y) * strideElems[static_cast<std::size_t>(c)] + x * step] >> bitShift);
    }
};

/// Reference to a frame that lives in GPU memory (CUDA device pointers) when
/// the decoder was asked to keep frames on the device.  Layout is always
/// biplanar P010/NV12 style pitch-linear memory.
struct DeviceFrameRef {
    void* yDevice = nullptr;        ///< Device pointer of the luma plane.
    void* uvDevice = nullptr;       ///< Device pointer of the interleaved CbCr plane.
    std::size_t pitchBytes = 0;     ///< Row pitch in bytes (same for both planes).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitShift = 6;      ///< 6 for P010 (10 bits in the top of 16), 8 for NV12 widened.
    std::uint8_t bitDepth = 10;
    int deviceIndex = 0;            ///< CUDA device ordinal the memory belongs to.
    std::shared_ptr<void> owner;    ///< Keeps the device memory alive.

    [[nodiscard]] bool valid() const noexcept {
        return yDevice != nullptr && uvDevice != nullptr && pitchBytes > 0 && width > 0 && height > 0;
    }
};

/// The two lens frames of one time instant plus optional device copies.
struct FramePair {
    std::array<PlanarFrame16, 2> lens{};      ///< [0] = slave (track 1), [1] = master (track 2).
    std::array<DeviceFrameRef, 2> device{};   ///< Optional GPU-resident copies.
    std::uint32_t index = 0;                  ///< Frame number.
    std::int64_t ptsUs = 0;                   ///< Presentation time (both lenses match).

    [[nodiscard]] bool valid() const noexcept { return lens[0].valid() && lens[1].valid(); }
    [[nodiscard]] bool onDevice() const noexcept { return device[0].valid() && device[1].valid(); }
};

}  // namespace osv::video
