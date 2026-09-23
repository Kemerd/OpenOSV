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

/// Which display look the Rec.709 output uses (details in osv/color/Look.h).
///
/// Persisted (as PrefsLook in the importer's preference blob, where zero means
/// "the default"), so values are never renumbered.  Only the Rec.709 output
/// has a look; HLG, PQ, linear and passthrough ignore it.
enum class Look : int {
    Standard = OSV_LOOK_STANDARD,  ///< The HLG signal in Rec.709 primaries (the pre-look rendering).
    DjiStudio = OSV_LOOK_DJI       ///< DJI Studio's D-Log M -> Rec.709 rendering (the default).
};

/// The look new code and the CLI select, and what a zeroed preference means.
inline constexpr Look kDefaultLook = Look::DjiStudio;

// -----------------------------------------------------------------------------
//  [WP-HDRPEAK] HDR peak brightness (the PQ output's highlight roll-off)
// -----------------------------------------------------------------------------

/// The PQ output's target display peak when none is chosen: the 1000-nit
/// display of the OOTF itself, which means no roll-off at all - the PQ
/// output of every build before the setting existed, bit for bit.
inline constexpr float kDefaultHdrPeakNits = kDefaultPeakNits;

/// The lowest target setHdrPeak accepts (the SDR reference peak); a lower
/// request is raised to it.  At 100 nits the BT.2408 knee sits at 27 nits,
/// just above 18 % grey, so nothing lower is a meaningful HDR target.
inline constexpr float kMinHdrPeakNits = 100.0f;

/// The targets the Source Settings offer, in PrefsHdrPeak order (the order
/// is persisted, so it is append-only): the untouched 1000-nit master, two
/// common consumer HDR peaks, and 203 nits - BT.2408's HDR reference white,
/// the "SDR-safe" choice (nothing brighter than diffuse white).
inline constexpr float kHdrPeakChoicesNits[] = {1000.0f, 600.0f, 400.0f, 203.0f};

/**
 * @brief Set the PQ output's target display peak on an already built block.
 *
 * Fills the block's hdrPeak* group for osvHdrPeakRolloff (ColorMath.h): the
 * target, PQ(peakNits), the target normalised to that source range and the
 * BT.2408 knee start KS = 1.5 * maxLum - 0.5.  The group is ZEROED (no
 * roll-off) when the block's transfer is not PQ, when the target is not
 * finite or not positive, or when it is not below the block's own peakNits
 * (1000) - so a 1000-nit request leaves a block byte-identical to one built
 * before the setting existed.  Positive targets below kMinHdrPeakNits are
 * raised to it.
 */
void setHdrPeak(OsvColorParams& params, float targetNits) noexcept;

/// The peak the block's PQ output can reach in nits: its roll-off target
/// when one is active, otherwise its OOTF peak (1000).  0 for a block that
/// is not PQ, because only PQ output is an absolute display light level.
[[nodiscard]] float hdrPeakNitsOf(const OsvColorParams& params) noexcept;

/// Where the roll-off starts for a target, in nits: the BT.2408 knee
/// PQ^-1(KS * PQ(sourcePeakNits)).  Everything at or below it is untouched.
/// Returns sourcePeakNits when the target is not below it (no roll-off) and
/// 0 for non-finite or non-positive arguments.
[[nodiscard]] float hdrPeakKneeNits(float targetNits, float sourcePeakNits = kDefaultPeakNits) noexcept;

/// Parse a target peak: "1000", "600", "400" or "203" (the Source Settings
/// choices), also spelled with a "nits" suffix, and "sdr" / "sdr-safe" for
/// 203.  Returns false and leaves `nits` untouched for anything else.
[[nodiscard]] bool parseHdrPeak(std::string_view text, float& nits) noexcept;

/// Stable lower-case names ("dji", "pocket3", "osmo360").
[[nodiscard]] const char* dlogMFitName(DlogMFit fit) noexcept;
/// Stable lower-case names ("hlg", "pq", "709", "linear", "dlogm").
[[nodiscard]] const char* outputTransferName(OutputTransfer transfer) noexcept;
/// Stable lower-case names ("dlogm", "hlg", "709").
[[nodiscard]] const char* inputEncodingName(InputEncoding encoding) noexcept;
/// Stable lower-case names ("dji", "standard").
[[nodiscard]] const char* lookName(Look look) noexcept;

/// Parse a name (case-insensitive; accepts the aliases documented in the CLI
/// help: "dji"/"refit", "pocket3"/"pocket", "osmo360"/"osmo").  Returns false
/// and leaves `out` untouched when the text is not recognised.
[[nodiscard]] bool parseDlogMFit(std::string_view text, DlogMFit& out) noexcept;
/// Parse "pq", "hlg", "709"/"rec709"/"sdr", "linear"/"exr", "dlogm"/"passthrough"/"none".
[[nodiscard]] bool parseOutputTransfer(std::string_view text, OutputTransfer& out) noexcept;
/// Parse "dlogm"/"dlog-m"/"log", "hlg", "709"/"rec709"/"normal".
[[nodiscard]] bool parseInputEncoding(std::string_view text, InputEncoding& out) noexcept;
/// Parse "dji"/"dji-studio"/"djistudio"/"studio" or "standard"/"std"/"hlg709".
[[nodiscard]] bool parseLook(std::string_view text, Look& out) noexcept;

/// The curve constants for a fit.
[[nodiscard]] const OsvDlogMCurve& dlogmCurve(DlogMFit fit) noexcept;

/// The camera native primaries -> Rec.2020 matrix that pairs with a fit.
///
/// Curve and matrix are two halves of one camera characterisation and must be
/// selected together: decoding with the Osmo 360 curve while converting with
/// the Pocket 3 primaries mixes two different cameras' measurements. This is
/// the single place that pairing is decided, so the two cannot drift apart.
///
///   * DlogMFit::Osmo360  -> kNativeToRec2020_Osmo360 (both fitted from DJI's
///                           own Osmo 360 reference LUT: the curve from its
///                           neutral axis, the matrix from its other 35904
///                           entries).
///   * DlogMFit::Pocket3  -> kNativeToRec2020_Pocket3 (the public Pocket 3
///                           curve with the Pocket 3 chart fit, still
///                           reachable as `--fit pocket3`).
///   * DlogMFit::DjiRefit -> kNativeToRec2020_Pocket3, because that is the
///                           matrix it shipped and rendered against; the whole
///                           point of keeping this fit is bit-stable output
///                           for projects already graded on it, which a matrix
///                           change would break.
///
/// An out-of-range fit (a corrupt persisted preference byte) returns the
/// default matrix rather than an arbitrary one, matching dlogmCurve.
[[nodiscard]] const OsvMat3f& nativeToWorkingForFit(DlogMFit fit) noexcept;

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
 * @param look           Display look for the Rec.709 output (DJI Studio by
 *                       default; Look::Standard keeps the pre-look rendering).
 *                       Ignored by every other transfer.
 * @param hdrPeakNits    [WP-HDRPEAK] Target display peak of the PQ output
 *                       (see setHdrPeak).  The default, 1000, is no roll-off.
 *                       Ignored by every other transfer.
 *
 * Inputs outside their valid range are clamped (bit depth to 8..16, non-finite
 * stops to 0, non-positive scene scale to the BT.2408 default, an unknown look
 * to the standard rendering, a non-finite HDR peak to no roll-off) rather than
 * rejected, so the function can never produce a block that crashes a kernel.
 */
[[nodiscard]] OsvColorParams makeColorParams(DlogMFit fit, OutputTransfer transfer, float exposureStops,
                                             InputEncoding input = InputEncoding::DLogM, bool narrowInput = true,
                                             std::uint32_t bitDepth = 10,
                                             const OsvDlogMCurve* curveOverride = nullptr,
                                             float sceneScale = kBt2408SceneScale,
                                             Look look = kDefaultLook,
                                             float hdrPeakNits = kDefaultHdrPeakNits) noexcept;

/// A disabled block (every stage copies input to output).
[[nodiscard]] OsvColorParams makeDisabledColorParams() noexcept;

/// True when every float in the block is finite and the enums are in range.
[[nodiscard]] bool colorParamsValid(const OsvColorParams& params) noexcept;

}  // namespace osv::color
