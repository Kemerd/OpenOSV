// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// makeColorParams, the enum name / parse helpers and [WP-HDRPEAK] the PQ
// output's HDR peak roll-off set-up (setHdrPeak and its queries).

#include "osv/color/ColorParams.h"

#include "osv/color/Look.h"
#include "osv/color/Matrices.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace osv::color {

namespace {

/// Lower-case ASCII copy (the parse helpers are case-insensitive).
std::string lowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        // Only 7-bit letters are folded; anything else is copied verbatim.
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/// Copy a matrix into the parameter block.
void setMatrix(OsvMat3f& dst, const OsvMat3f& src) noexcept {
    for (int i = 0; i < 9; ++i) {
        dst.m[i] = src.m[i];
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  Names
// -----------------------------------------------------------------------------
const char* dlogMFitName(DlogMFit fit) noexcept {
    switch (fit) {
    case DlogMFit::DjiRefit: return "dji";
    case DlogMFit::Pocket3: return "pocket3";
    case DlogMFit::Osmo360: return "osmo360";
    }
    return "unknown";
}

const char* outputTransferName(OutputTransfer transfer) noexcept {
    switch (transfer) {
    case OutputTransfer::HLG: return "hlg";
    case OutputTransfer::PQ: return "pq";
    case OutputTransfer::Rec709: return "709";
    case OutputTransfer::Linear: return "linear";
    case OutputTransfer::Passthrough: return "dlogm";
    }
    return "unknown";
}

const char* inputEncodingName(InputEncoding encoding) noexcept {
    switch (encoding) {
    case InputEncoding::DLogM: return "dlogm";
    case InputEncoding::HLG: return "hlg";
    case InputEncoding::Rec709Normal: return "709";
    }
    return "unknown";
}

const char* lookName(Look look) noexcept {
    switch (look) {
    case Look::DjiStudio: return "dji";
    case Look::Standard: return "standard";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
//  Parsing
// -----------------------------------------------------------------------------
bool parseDlogMFit(std::string_view text, DlogMFit& out) noexcept {
    const std::string t = lowerAscii(text);
    // "dji" keeps resolving to the original refit rather than following the
    // default: a stored preference, a CI script or a user's documented command
    // line that says "dji" must keep decoding with the curve it was written
    // against.  The new curve has its own explicit names.
    if (t == "dji" || t == "refit" || t == "dji-refit" || t == "djirefit") {
        out = DlogMFit::DjiRefit;
        return true;
    }
    if (t == "pocket3" || t == "pocket" || t == "pocket-3") {
        out = DlogMFit::Pocket3;
        return true;
    }
    if (t == "osmo360" || t == "osmo" || t == "osmo-360" || t == "360") {
        out = DlogMFit::Osmo360;
        return true;
    }
    return false;
}

bool parseOutputTransfer(std::string_view text, OutputTransfer& out) noexcept {
    const std::string t = lowerAscii(text);
    if (t == "pq" || t == "st2084" || t == "smpte2084") {
        out = OutputTransfer::PQ;
        return true;
    }
    if (t == "hlg" || t == "arib-std-b67") {
        out = OutputTransfer::HLG;
        return true;
    }
    if (t == "709" || t == "rec709" || t == "bt709" || t == "sdr") {
        out = OutputTransfer::Rec709;
        return true;
    }
    if (t == "linear" || t == "exr" || t == "scene-linear") {
        out = OutputTransfer::Linear;
        return true;
    }
    if (t == "dlogm" || t == "dlog-m" || t == "passthrough" || t == "none" || t == "log") {
        out = OutputTransfer::Passthrough;
        return true;
    }
    return false;
}

bool parseInputEncoding(std::string_view text, InputEncoding& out) noexcept {
    const std::string t = lowerAscii(text);
    if (t == "dlogm" || t == "dlog-m" || t == "log" || t == "d-log-m") {
        out = InputEncoding::DLogM;
        return true;
    }
    if (t == "hlg") {
        out = InputEncoding::HLG;
        return true;
    }
    if (t == "709" || t == "rec709" || t == "bt709" || t == "normal" || t == "sdr") {
        out = InputEncoding::Rec709Normal;
        return true;
    }
    return false;
}

bool parseLook(std::string_view text, Look& out) noexcept {
    const std::string t = lowerAscii(text);
    // The DJI Studio look under the names a user is likely to type.
    if (t == "dji" || t == "dji-studio" || t == "djistudio" || t == "studio") {
        out = Look::DjiStudio;
        return true;
    }
    // The pre-look rendering (the HLG signal in Rec.709 primaries).
    if (t == "standard" || t == "std" || t == "hlg709" || t == "none") {
        out = Look::Standard;
        return true;
    }
    return false;
}

const OsvDlogMCurve& dlogmCurve(DlogMFit fit) noexcept {
    switch (fit) {
    case DlogMFit::Pocket3: return kDlogMPocket3;
    case DlogMFit::DjiRefit: return kDlogMDjiRefit;
    case DlogMFit::Osmo360: break;
    }
    // Anything out of range (a corrupt persisted preference byte) falls back
    // to the default curve rather than an arbitrary one.
    return kDlogMOsmo360;
}

const OsvMat3f& nativeToWorkingForFit(DlogMFit fit) noexcept {
    switch (fit) {
    case DlogMFit::Pocket3:
    case DlogMFit::DjiRefit:
        // Both of these shipped against the Pocket 3 chart fit.  DjiRefit in
        // particular exists only so a project graded on it keeps rendering
        // identically, so it keeps the matrix it was graded with.
        return kNativeToRec2020_Pocket3;
    case DlogMFit::Osmo360:
        break;
    }
    // The default, and the fallback for a corrupt persisted preference byte:
    // the curve and the matrix fitted from the same Osmo 360 reference.
    return kNativeToRec2020_Osmo360;
}

// -----------------------------------------------------------------------------
//  Parameter block construction
// -----------------------------------------------------------------------------
OsvColorParams makeDisabledColorParams() noexcept {
    OsvColorParams p{};
    std::memset(&p, 0, sizeof(p));
    // A disabled block still carries sane values so a caller that flips
    // `enabled` on gets an identity-ish pipeline rather than zeros.
    p.enabled = 0;
    p.inputEncoding = OSV_INPUT_DLOGM;
    p.curve = kDlogMOsmo360;
    setMatrix(p.nativeToWorking, kIdentity3);
    setMatrix(p.workingToOutput, kIdentity3);
    p.sceneScale = kBt2408SceneScale;
    p.exposureGain = 1.0f;
    p.transfer = OSV_TRANSFER_PASSTHROUGH;
    p.peakNits = kDefaultPeakNits;
    p.ootfGamma = kDefaultOotfGamma;
    p.sdrPeakNits = kDefaultSdrPeakNits;
    p.yuvBlack = 64.0f;
    p.yuvScaleY = 1.0f / 876.0f;
    p.yuvScaleC = 1.0f / 896.0f;
    for (int i = 0; i < 9; ++i) {
        p.yuvToRgb[i] = kYuvToRgb709Narrow.m[i];
    }
    p.bitDepth = 10;
    return p;
}

OsvColorParams makeColorParams(DlogMFit fit, OutputTransfer transfer, float exposureStops, InputEncoding input,
                               bool narrowInput, std::uint32_t bitDepth, const OsvDlogMCurve* curveOverride,
                               float sceneScale, Look look, float hdrPeakNits) noexcept {
    OsvColorParams p = makeDisabledColorParams();
    p.enabled = 1;

    // --- input encoding and curve -------------------------------------------
    p.inputEncoding = static_cast<int>(input);
    if (p.inputEncoding < OSV_INPUT_DLOGM || p.inputEncoding > OSV_INPUT_REC709_NORMAL) {
        p.inputEncoding = OSV_INPUT_DLOGM;
    }
    // A caller-supplied curve wins over the fit, but only when it is usable.
    if (curveOverride != nullptr && dlogmCurveValid(*curveOverride)) {
        p.curve = *curveOverride;
    } else {
        p.curve = dlogmCurve(fit);
    }

    // --- primaries ----------------------------------------------------------
    switch (input) {
    case InputEncoding::HLG:
        // Camera HLG is already BT.2020: nothing to convert.
        setMatrix(p.nativeToWorking, kIdentity3);
        break;
    case InputEncoding::Rec709Normal:
        setMatrix(p.nativeToWorking, kRec709ToRec2020);
        break;
    case InputEncoding::DLogM:
    default:
        // The primaries matrix follows the selected curve, because the two are
        // halves of one camera characterisation (see nativeToWorkingForFit).
        // Note this keys off `fit`, not off a curveOverride: a caller passing a
        // custom curve is tweaking the tone response of the camera the fit
        // names, and there is no matrix override to pair with it.
        setMatrix(p.nativeToWorking, nativeToWorkingForFit(fit));
        break;
    }
    // Only the Rec.709 output leaves the Rec.2020 working space.
    p.transfer = static_cast<int>(transfer);
    if (p.transfer < OSV_TRANSFER_HLG || p.transfer > OSV_TRANSFER_PASSTHROUGH) {
        p.transfer = OSV_TRANSFER_PQ;
    }
    setMatrix(p.workingToOutput, p.transfer == OSV_TRANSFER_REC709 ? kRec2020ToRec709 : kIdentity3);

    // --- scale and exposure -------------------------------------------------
    p.sceneScale = (std::isfinite(sceneScale) && sceneScale > 0.0f) ? sceneScale : kBt2408SceneScale;
    const float stops = std::isfinite(exposureStops) ? exposureStops : 0.0f;
    p.exposureGain = std::exp2(stops);

    // --- YCbCr expansion for the requested bit depth ------------------------
    std::uint32_t depth = bitDepth;
    if (depth < 8) {
        depth = 8;
    }
    if (depth > 16) {
        depth = 16;
    }
    p.bitDepth = static_cast<int>(depth);
    const float shift = static_cast<float>(1u << (depth - 8));
    if (narrowInput) {
        // Limited range: black 16, luma range 219, chroma range 224 (8-bit
        // scale) shifted up to the sample depth.
        p.yuvBlack = 16.0f * shift;
        p.yuvScaleY = 1.0f / (219.0f * shift);
        p.yuvScaleC = 1.0f / (224.0f * shift);
    } else {
        // Full range: black 0, both ranges span every code.
        const float maxCode = static_cast<float>((1u << depth) - 1u);
        p.yuvBlack = 0.0f;
        p.yuvScaleY = 1.0f / maxCode;
        p.yuvScaleC = 1.0f / maxCode;
    }
    // HLG clips are tagged BT.2020 non-constant luminance; D-Log M and Normal
    // clips carry BT.709 YCbCr.
    const OsvMat3f& yuv = (input == InputEncoding::HLG) ? kYuvToRgb2020Narrow : kYuvToRgb709Narrow;
    for (int i = 0; i < 9; ++i) {
        p.yuvToRgb[i] = yuv.m[i];
    }

    // --- display look ---------------------------------------------------------
    // Filled for the (sanitised) transfer actually stored above, so a look can
    // never land on an output it was not fitted for; makeLookParams returns a
    // zeroed "no look" block for every other combination and for an
    // out-of-range Look value.
    p.look = makeLookParams(look, static_cast<OutputTransfer>(p.transfer));

    // --- [WP-HDRPEAK] PQ highlight roll-off ----------------------------------
    // Also keyed off the sanitised transfer: only PQ gets a target, and the
    // default 1000-nit target leaves the group zeroed, so the block is byte
    // for byte what it was before the setting existed.
    setHdrPeak(p, hdrPeakNits);
    return p;
}

// -----------------------------------------------------------------------------
//  [WP-HDRPEAK] HDR peak brightness
// -----------------------------------------------------------------------------
void setHdrPeak(OsvColorParams& params, float targetNits) noexcept {
    // Start from "off": every early return below leaves the zeroed group,
    // which the kernel reads as no roll-off.
    params.hdrPeakNits = 0.0f;
    params.hdrPeakSrcCode = 0.0f;
    params.hdrPeakMaxLum = 0.0f;
    params.hdrPeakKnee = 0.0f;

    // Only the PQ output is an absolute display light level; HLG is relative
    // to whatever display shows it (that display applies its own peak), and
    // Rec.709, linear and the passthrough have no HDR highlights to limit.
    if (params.transfer != OSV_TRANSFER_PQ) {
        return;
    }
    // A garbage request (NaN, infinities, zero or negative light) is the
    // default: no roll-off.
    if (!std::isfinite(targetNits) || targetNits <= 0.0f) {
        return;
    }
    // The source (mastering) peak is the OOTF display the PQ signal was
    // rendered for.  A block without a usable one cannot be normalised.
    const double source = static_cast<double>(params.peakNits);
    if (!std::isfinite(source) || source <= 0.0) {
        return;
    }
    // Nothing to roll off when the target shows the whole source range.
    if (static_cast<double>(targetNits) >= source) {
        return;
    }
    // Raise absurdly low targets to the SDR reference peak (see
    // kMinHdrPeakNits); a target at or above the source was handled above,
    // so the clamp cannot lift one past it unless the source itself is lower.
    const double target = std::max(static_cast<double>(targetNits), static_cast<double>(kMinHdrPeakNits));
    if (target >= source) {
        return;
    }

    // BT.2408-7 Annex 5, steps 1 and 2, in double precision: the source range
    // in PQ, the target peak normalised to it, and the knee start.
    const double srcCode = ref::pqInverseEotf(source);
    if (!(srcCode > 1e-6)) {
        return;
    }
    const double maxLum = ref::pqInverseEotf(target) / srcCode;
    const double knee = 1.5 * maxLum - 0.5;
    // Defensive: the kernel refuses anything outside 0 < KS < maxLum < 1, so
    // a block that would be refused there is not built here either.
    if (!(maxLum > 0.0 && maxLum < 1.0) || !(knee < maxLum)) {
        return;
    }
    params.hdrPeakNits = static_cast<float>(target);
    params.hdrPeakSrcCode = static_cast<float>(srcCode);
    params.hdrPeakMaxLum = static_cast<float>(maxLum);
    params.hdrPeakKnee = static_cast<float>(knee);
}

float hdrPeakNitsOf(const OsvColorParams& params) noexcept {
    // Relative outputs have no absolute peak to report.
    if (params.transfer != OSV_TRANSFER_PQ) {
        return 0.0f;
    }
    // An active roll-off caps the output at its target; otherwise the OOTF's
    // own display peak is the brightest the output gets for in-range light.
    return params.hdrPeakNits > 0.0f ? params.hdrPeakNits : params.peakNits;
}

float hdrPeakKneeNits(float targetNits, float sourcePeakNits) noexcept {
    // Garbage in: nothing meaningful to report.
    if (!std::isfinite(targetNits) || !std::isfinite(sourcePeakNits) || targetNits <= 0.0f ||
        sourcePeakNits <= 0.0f) {
        return 0.0f;
    }
    // No roll-off: the whole source range is untouched.
    if (targetNits >= sourcePeakNits) {
        return sourcePeakNits;
    }
    // The same clamp and the same knee as setHdrPeak, back through the EOTF.
    const double target = std::max(static_cast<double>(targetNits), static_cast<double>(kMinHdrPeakNits));
    if (target >= static_cast<double>(sourcePeakNits)) {
        return sourcePeakNits;
    }
    const double srcCode = ref::pqInverseEotf(static_cast<double>(sourcePeakNits));
    const double maxLum = ref::pqInverseEotf(target) / srcCode;
    const double knee = std::max(1.5 * maxLum - 0.5, 0.0);
    return static_cast<float>(ref::pqEotf(knee * srcCode));
}

bool parseHdrPeak(std::string_view text, float& nits) noexcept {
    std::string t = lowerAscii(text);
    // Accept the unit spelled out ("600nits", "600 nits") as well as bare.
    for (const char* suffix : {" nits", "nits", " nit", "nit"}) {
        const std::size_t n = std::strlen(suffix);
        if (t.size() > n && t.compare(t.size() - n, n, suffix) == 0) {
            t.resize(t.size() - n);
            break;
        }
    }
    // Exactly the Source Settings choices, so the CLI renders only what
    // Premiere can: the same four targets, the same knees.
    for (const float choice : kHdrPeakChoicesNits) {
        if (t == std::to_string(static_cast<int>(choice))) {
            nits = choice;
            return true;
        }
    }
    // The 203-nit choice under the name the UI gives it.
    if (t == "sdr" || t == "sdr-safe" || t == "sdrsafe") {
        nits = 203.0f;
        return true;
    }
    return false;
}

bool colorParamsValid(const OsvColorParams& params) noexcept {
    // Enum ranges.
    if (params.inputEncoding < OSV_INPUT_DLOGM || params.inputEncoding > OSV_INPUT_REC709_NORMAL) {
        return false;
    }
    if (params.transfer < OSV_TRANSFER_HLG || params.transfer > OSV_TRANSFER_PASSTHROUGH) {
        return false;
    }
    if (params.bitDepth < 1 || params.bitDepth > 16) {
        return false;
    }
    // Curve.
    if (!dlogmCurveValid(params.curve)) {
        return false;
    }
    // Every float member must be finite.
    const float scalars[] = {params.sceneScale, params.exposureGain, params.peakNits, params.ootfGamma,
                             params.sdrPeakNits, params.yuvBlack, params.yuvScaleY, params.yuvScaleC};
    for (const float v : scalars) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(params.nativeToWorking.m[i]) || !std::isfinite(params.workingToOutput.m[i]) ||
            !std::isfinite(params.yuvToRgb[i])) {
            return false;
        }
    }
    // The look block: either "no look" or a complete, finite one.
    if (!lookParamsValid(params.look)) {
        return false;
    }
    // [WP-HDRPEAK] The roll-off group: zeroed (off), or a PQ block with a
    // finite target below its own peak and constants the kernel accepts.
    const float peakGroup[] = {params.hdrPeakNits, params.hdrPeakSrcCode, params.hdrPeakMaxLum, params.hdrPeakKnee};
    for (const float v : peakGroup) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    if (params.hdrPeakNits != 0.0f) {
        if (params.transfer != OSV_TRANSFER_PQ || !(params.hdrPeakNits > 0.0f) ||
            !(params.hdrPeakNits < params.peakNits) || !(params.hdrPeakSrcCode > 0.0f) ||
            !(params.hdrPeakMaxLum > 0.0f && params.hdrPeakMaxLum < 1.0f) ||
            !(params.hdrPeakKnee < params.hdrPeakMaxLum)) {
            return false;
        }
    }
    return params.sceneScale > 0.0f && params.exposureGain > 0.0f && params.peakNits > 0.0f;
}

}  // namespace osv::color
