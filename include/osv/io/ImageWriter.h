// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Still-image output for rendered frames.
//
//   * PNG (8 or 16 bit) and TIFF (16 bit) are produced with libavcodec's
//     LGPL encoders, so no extra image library is linked.
//   * EXR (float, scene-linear) uses tinyexr and carries BT.2020
//     chromaticities when the data is wide gamut.
//   * A small JSON sidecar records the transfer function and primaries next
//     to PNG/TIFF files because neither format can signal HDR reliably.
#pragma once

#include "osv/core/Result.h"
#include "osv/render/ImageRGBAf.h"

#include <filesystem>
#include <string>

namespace osv::io {

enum class ImageFormat { Png16, Tiff16, Exr, Png8 };

/// Transfer function names understood by the sidecar / EXR header.
enum class ImageTransfer { HLG, PQ, Rec709, Linear, DLogM };

struct ImageTag {
    ImageTransfer transfer = ImageTransfer::PQ;
    bool rec2020 = true;         ///< BT.2020 primaries (false = BT.709)
    bool includeAlpha = false;   ///< Write the coverage alpha as a 4th channel
    bool writeSidecar = true;    ///< Emit <file>.json for PNG/TIFF
    float peakNits = 1000.0f;    ///< Informational, recorded in the sidecar
};

/// Pick a format from the file extension (.png -> Png16, .tif/.tiff -> Tiff16,
/// .exr -> Exr).  Unknown extensions map to Png16.
[[nodiscard]] ImageFormat formatFromExtension(const std::filesystem::path& path);

/// Human readable name of a transfer ("pq", "hlg", ...).
[[nodiscard]] const char* imageTransferName(ImageTransfer t) noexcept;

/// Write `image` to `path`.  Values are clamped to [0, 1] for the integer
/// formats; EXR keeps floats as they are.
Status writeImage(const std::filesystem::path& path, const render::ImageRGBAf& image, ImageFormat format,
                  const ImageTag& tag);

/// Read a PNG or TIFF back into float RGBA (values scaled to [0, 1]).  Used by
/// the tests and by `osvtool selfcheck`; 8- and 16-bit RGB/RGBA are accepted.
Result<render::ImageRGBAf> readImage(const std::filesystem::path& path);

/// Read an EXR written by writeImage (float RGB/RGBA).
Result<render::ImageRGBAf> readExr(const std::filesystem::path& path);

}  // namespace osv::io
