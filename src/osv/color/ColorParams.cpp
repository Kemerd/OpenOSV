// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// makeColorParams and the enum name / parse helpers.

#include "osv/color/ColorParams.h"

#include "osv/color/Matrices.h"

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

// -----------------------------------------------------------------------------
//  Parsing
// -----------------------------------------------------------------------------
bool parseDlogMFit(std::string_view text, DlogMFit& out) noexcept {
    const std::string t = lowerAscii(text);
    if (t == "dji" || t == "refit" || t == "dji-refit" || t == "djirefit") {
        out = DlogMFit::DjiRefit;
        return true;
    }
    if (t == "pocket3" || t == "pocket" || t == "pocket-3") {
        out = DlogMFit::Pocket3;
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

const OsvDlogMCurve& dlogmCurve(DlogMFit fit) noexcept {
    switch (fit) {
    case DlogMFit::Pocket3: return kDlogMPocket3;
    case DlogMFit::DjiRefit: break;
    }
    return kDlogMDjiRefit;
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
    p.curve = kDlogMDjiRefit;
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
                               float sceneScale) noexcept {
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
        setMatrix(p.nativeToWorking, kNativeToRec2020_Pocket3);
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
    return p;
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
    return params.sceneScale > 0.0f && params.exposureGain > 0.0f && params.peakNits > 0.0f;
}

}  // namespace osv::color
