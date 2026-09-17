// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Adobe/Resolve style .cube 3D LUT writer and reader.
//
// writeCube bakes the full OsvColorParams pipeline (code -> linear -> output)
// into a size^3 table so an NLE can apply the D-Log M -> PQ/HLG/709 transform
// to footage without OpenOSV.  readCube parses such a file back (used by the
// tests to verify the writer and by `osvtool selfcheck`).
//
// File layout written:
//   TITLE "..."
//   LUT_3D_SIZE N
//   DOMAIN_MIN r g b
//   DOMAIN_MAX r g b
//   N^3 lines "r g b" (6 decimals), red index varying fastest.
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace osv::color {

/// Options for writeCube.
struct CubeOptions {
    std::uint32_t size = 65;                  ///< Grid points per axis (2..256).
    std::string title;                        ///< TITLE line (ASCII, quotes stripped); empty = auto.
    float domainMin[3] = {0.0f, 0.0f, 0.0f};  ///< Input value at grid index 0.
    float domainMax[3] = {1.0f, 1.0f, 1.0f};  ///< Input value at grid index size-1.
    /// When true the LUT input axis is the *narrow-range* video value
    /// (16..235 on the 8-bit scale) instead of the already expanded D-Log M
    /// code, i.e. a value v is decoded as code = (v * maxCode - black) / range
    /// before the curve.  Use it for hosts that hand a limited-range clip to
    /// the LUT without expanding it first.
    bool inputIsNarrowCode = false;
};

/// A 3D LUT held in memory (red index fastest, then green, then blue).
struct Lut3D {
    std::uint32_t size = 0;                   ///< Grid points per axis.
    std::string title;                        ///< TITLE line without quotes.
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};
    std::vector<float> data;                  ///< size^3 * 3 floats.

    /// True when data.size() == size^3 * 3 and size >= 2.
    [[nodiscard]] bool valid() const noexcept;

    /// Trilinear lookup.  Input is clamped to the domain; returns false (and
    /// writes zeros) when the LUT is not valid or a pointer is null.
    bool sample(const float in[3], float out[3]) const noexcept;

    /// Direct grid access; out-of-range indices are clamped.
    void at(std::uint32_t r, std::uint32_t g, std::uint32_t b, float out[3]) const noexcept;
};

/// Write the LUT for `params` to `path`.  Fails with InvalidArgument for a bad
/// size, Io when the file cannot be created or written.
[[nodiscard]] Status writeCube(const std::filesystem::path& path, const OsvColorParams& params,
                               const CubeOptions& options);

/// Parse a .cube file.  Fails with Io / Malformed / Unsupported (1D LUTs).
[[nodiscard]] Result<Lut3D> readCube(const std::filesystem::path& path);

/// Evaluate the pipeline for one LUT input the same way writeCube does
/// (handles inputIsNarrowCode), so tests can compare against the file.
void evaluateCubeEntry(const OsvColorParams& params, const CubeOptions& options, const float in[3],
                       float out[3]) noexcept;

}  // namespace osv::color
