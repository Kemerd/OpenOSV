// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Host-side construction of the OsvColorParams block consumed by the render
// kernels, plus the enums and name/parse helpers used by the CLI and the
// plug-ins.
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/color/DlogM.h"
#include "osv/color/Transfer.h"

#include <cstdint>
#include <string_view>

namespace osv::color {

/// Which D-Log M -> linear curve to decode with.
///
/// The numeric values are persisted (plugins/common/PrefsBlob.h stores the
/// enum as a byte in the effect/importer preference blob), so existing values
/// must never be renumbered: Osmo360 was appended as 2 rather than taking 0,
/// even though it is now the default curve.  A blob written by an older build
/// still deserialises to exactly the curve that build used.
enum class DlogMFit : int {
    DjiRefit = 0,  ///< kDlogMDjiRefit (Pocket-3-era HLG measurements).
    Pocket3 = 1,   ///< kDlogMPocket3 (public Pocket 3 constants).
    Osmo360 = 2    ///< kDlogMOsmo360 (default; fitted to the Osmo 360 reference).
};

/// The curve new code and the CLI defaults select.  Named so the default can
/// move again without hunting for literals.
inline constexpr DlogMFit kDefaultDlogMFit = DlogMFit::Osmo360;

/// Output encoding produced by osvLinearToOutput.
enum class OutputTransfer : int {
    HLG = OSV_TRANSFER_HLG,                 ///< Rec.2100 HLG (nclx 9/18/9).
    PQ = OSV_TRANSFER_PQ,                   ///< Rec.2100 PQ (nclx 9/16/9).
    Rec709 = OSV_TRANSFER_REC709,           ///< Rec.709 SDR via BT.2390 tone mapping.
    Linear = OSV_TRANSFER_LINEAR,           ///< Scene-linear Rec.2020 (EXR).
    Passthrough = OSV_TRANSFER_PASSTHROUGH  ///< The source code unchanged (D-Log M out).
};

/// How the source clip is encoded.
enum class InputEncoding : int {
    DLogM = OSV_INPUT_DLOGM,                ///< color_mode 19 (the project default).
    HLG = OSV_INPUT_HLG,                    ///< color_mode 9, BT.2020 HLG straight from the camera.
    Rec709Normal = OSV_INPUT_REC709_NORMAL  ///< color_mode 0 "Normal" Rec.709 clips.
};

/// Stable lower-case names ("dji", "pocket3", "osmo360").
[[nodiscard]] const char* dlogMFitName(DlogMFit fit) noexcept;
/// Stable lower-case names ("hlg", "pq", "709", "linear", "dlogm").
[[nodiscard]] const char* outputTransferName(OutputTransfer transfer) noexcept;
/// Stable lower-case names ("dlogm", "hlg", "709").
[[nodiscard]] const char* inputEncodingName(InputEncoding encoding) noexcept;

/// Parse a name (case-insensitive; accepts the aliases documented in the CLI
/// help: "dji"/"refit", "pocket3"/"pocket", "osmo360"/"osmo").  Returns false
/// and leaves `out` untouched when the text is not recognised.
[[nodiscard]] bool parseDlogMFit(std::string_view text, DlogMFit& out) noexcept;
/// Parse "pq", "hlg", "709"/"rec709"/"sdr", "linear"/"exr", "dlogm"/"passthrough"/"none".
[[nodiscard]] bool parseOutputTransfer(std::string_view text, OutputTransfer& out) noexcept;
/// Parse "dlogm"/"dlog-m"/"log", "hlg", "709"/"rec709"/"normal".
[[nodiscard]] bool parseInputEncoding(std::string_view text, InputEncoding& out) noexcept;

/// The curve constants for a fit.
[[nodiscard]] const OsvDlogMCurve& dlogmCurve(DlogMFit fit) noexcept;

/**
 * @brief Build the kernel parameter block.
 *
 * @param fit            D-Log M curve to decode with (ignored for HLG / 709 input).
 * @param transfer       Output encoding.
 * @param exposureStops  Exposure offset in stops applied in linear light (gain = 2^stops).
 * @param input          Source encoding (D-Log M by default).
 * @param narrowInput    True for limited (TV) range YCbCr (the camera always
 *                       writes narrow range), false for full range.
 * @param bitDepth       YCbCr sample bit depth (8..16; 10 for the Osmo 360).
 * @param curveOverride  Optional custom curve replacing the fit's constants.
 * @param sceneScale     Scene-linear -> HLG/PQ scale (kBt2408SceneScale).
 *
 * Inputs outside their valid range are clamped (bit depth to 8..16, non-finite
 * stops to 0, non-positive scene scale to the BT.2408 default) rather than
 * rejected, so the function can never produce a block that crashes a kernel.
 */
[[nodiscard]] OsvColorParams makeColorParams(DlogMFit fit, OutputTransfer transfer, float exposureStops,
                                             InputEncoding input = InputEncoding::DLogM, bool narrowInput = true,
                                             std::uint32_t bitDepth = 10,
                                             const OsvDlogMCurve* curveOverride = nullptr,
                                             float sceneScale = kBt2408SceneScale) noexcept;

/// A disabled block (every stage copies input to output).
[[nodiscard]] OsvColorParams makeDisabledColorParams() noexcept;

/// True when every float in the block is finite and the enums are in range.
[[nodiscard]] bool colorParamsValid(const OsvColorParams& params) noexcept;

}  // namespace osv::color
