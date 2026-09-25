// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool lut`: bake the D-Log M -> HLG / PQ / Rec.709 pipeline into a .cube
// 3D LUT for NLEs that cannot load OpenOSV directly.
//
//   osvtool lut [--fit osmo360|dji|pocket3] [--out-transfer pq|hlg|709] [--size 65]
//               [--exposure 0] [--title "..."] [--input dlogm|hlg|709]
//               [--look dji|standard] [--hdr-peak 1000|600|400|203]
//               [--tone aces-bright|aces-detailed|bt2408-natural|bt2408-punchy|bt2408-neutral]
//               [--narrow-input] out.cube

#include "Commands.h"

#include "osv/color/ColorParams.h"
#include "osv/color/Cube.h"
#include "osv/color/DlogM.h"
#include "osv/core/Log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace osvtool {

namespace {

/// Options collected by CLI11 for the lut command.
struct LutOptions {
    std::string fit = "osmo360";
    std::string outTransfer = "pq";
    std::string input = "dlogm";
    std::string look = "dji";  ///< Rec.709 display look: dji (default) | standard.
    std::string hdrPeak = "1000";  ///< [WP-HDRPEAK] PQ output's peak: 1000 (default) | 600 | 400 | 203.
    /// [WP-HDRTONE] HDR transfer function of D-Log M to PQ / HLG: aces-bright
    /// (default) | aces-detailed | bt2408-natural | bt2408-punchy | bt2408-neutral.
    std::string tone = "aces-bright";
    std::string title;
    std::string outPath;
    unsigned size = 65;
    double exposure = 0.0;
    bool narrowInput = false;
};

/// Run the command; returns one of the kExit* codes.
int runLut(const LutOptions& opt) {
    using namespace osv::color;

    // --- validate the enum-like strings ------------------------------------
    DlogMFit fit = kDefaultDlogMFit;
    if (!parseDlogMFit(opt.fit, fit)) {
        std::fprintf(stderr, "error: unknown --fit '%s' (expected osmo360, dji or pocket3)\n",
                     osv::log::safe(opt.fit).c_str());
        return kExitUsage;
    }
    OutputTransfer transfer = OutputTransfer::PQ;
    if (!parseOutputTransfer(opt.outTransfer, transfer)) {
        std::fprintf(stderr, "error: unknown --out-transfer '%s' (expected pq, hlg, 709, linear or dlogm)\n",
                     osv::log::safe(opt.outTransfer).c_str());
        return kExitUsage;
    }
    InputEncoding input = InputEncoding::DLogM;
    if (!parseInputEncoding(opt.input, input)) {
        std::fprintf(stderr, "error: unknown --input '%s' (expected dlogm, hlg or 709)\n",
                     osv::log::safe(opt.input).c_str());
        return kExitUsage;
    }
    Look look = kDefaultLook;
    if (!parseLook(opt.look, look)) {
        std::fprintf(stderr, "error: unknown --look '%s' (expected dji or standard)\n",
                     osv::log::safe(opt.look).c_str());
        return kExitUsage;
    }
    // [WP-HDRPEAK] The PQ output's peak: the Source Settings choices only.
    float hdrPeakNits = kDefaultHdrPeakNits;
    if (!parseHdrPeak(opt.hdrPeak, hdrPeakNits)) {
        std::fprintf(stderr, "error: unknown --hdr-peak '%s' (expected 1000, 600, 400 or 203)\n",
                     osv::log::safe(opt.hdrPeak).c_str());
        return kExitUsage;
    }
    // [WP-HDRTONE] The HDR transfer function: the Source Settings styles.
    HdrTone tone = kDefaultHdrTone;
    if (!parseHdrTone(opt.tone, tone)) {
        std::fprintf(stderr,
                     "error: unknown --tone '%s' (expected aces-bright, aces-detailed, bt2408-natural, "
                     "bt2408-punchy or bt2408-neutral)\n",
                     osv::log::safe(opt.tone).c_str());
        return kExitUsage;
    }
    // --- numeric sanity ------------------------------------------------------
    if (opt.size < 2 || opt.size > 256) {
        std::fprintf(stderr, "error: --size must be in [2, 256]\n");
        return kExitUsage;
    }
    if (!std::isfinite(opt.exposure) || opt.exposure < -10.0 || opt.exposure > 10.0) {
        std::fprintf(stderr, "error: --exposure must be a finite number of stops in [-10, 10]\n");
        return kExitUsage;
    }
    if (opt.outPath.empty()) {
        std::fprintf(stderr, "error: output path is required\n");
        return kExitUsage;
    }

    // --- build the pipeline block -------------------------------------------
    // The LUT input is R'G'B' (already expanded to 0..1), so the YCbCr part
    // of the block is unused; narrow-input handling is done by the cube
    // writer on the LUT axis instead.
    const OsvColorParams params = makeColorParams(fit, transfer, static_cast<float>(opt.exposure), input, true, 10,
                                                  nullptr, kBt2408SceneScale, look, hdrPeakNits, tone);
    // [WP-HDRPEAK] True when the table actually carries a roll-off (a PQ LUT
    // with a peak below 1000); every other combination is the default table.
    const bool rolledOff = params.hdrPeakNits > 0.0f;
    // [WP-HDRTONE] True when the style applies to this table at all: D-Log M
    // input to a BT.2100 output.  Neutral applies too (it IS the BT.2408
    // rendering), it just carries no tone-scale block.
    const bool toneApplies = input == InputEncoding::DLogM &&
                             (transfer == OutputTransfer::PQ || transfer == OutputTransfer::HLG);
    if (!colorParamsValid(params)) {
        std::fprintf(stderr, "error: internal colour parameter block is invalid\n");
        return kExitRuntime;
    }

    CubeOptions cube;
    cube.size = opt.size;
    cube.inputIsNarrowCode = opt.narrowInput;
    if (!opt.title.empty()) {
        cube.title = opt.title;
    } else {
        // Auto title documents what the LUT does.
        // [WP-HDRPEAK] A rolled-off PQ table names its peak inside the
        // brackets; the default title (and so the committed LUTs) is
        // unchanged.
        const std::string peak =
            rolledOff ? ", " + std::to_string(static_cast<int>(params.hdrPeakNits)) + "-nit peak" : std::string();
        // [WP-HDRTONE] An HDR table of D-Log M names its transfer function
        // too: the default is no longer the only rendering it can be.
        const std::string toneText = toneApplies ? ", " + std::string(hdrToneName(tone)) + " tone" : std::string();
        cube.title = std::string("OpenOSV ") + inputEncodingName(input) + " to " + outputTransferName(transfer) +
                     " (" + dlogMFitName(fit) + " fit" + toneText + peak + ")";
    }

    // --- write ---------------------------------------------------------------
    const osv::Status st = writeCube(std::filesystem::path(opt.outPath), params, cube);
    if (!st.ok()) {
        std::fprintf(stderr, "error: %s\n", osv::log::safe(st.error().toString()).c_str());
        return st.error().code == osv::ErrorCode::InvalidArgument ? kExitUsage : kExitRuntime;
    }

    // --- report (7-bit ASCII only) ------------------------------------------
    const unsigned long long entries = static_cast<unsigned long long>(opt.size) * opt.size * opt.size;
    std::printf("wrote %s\n", osv::log::safe(opt.outPath).c_str());
    std::printf("  input     : %s%s\n", inputEncodingName(input), opt.narrowInput ? " (narrow-range axis)" : "");
    std::printf("  fit       : %s\n", dlogMFitName(fit));
    std::printf("  output    : %s\n", outputTransferName(transfer));
    if (transfer == OutputTransfer::Rec709) {
        std::printf("  look      : %s\n", lookName(look));
    }
    // [WP-HDRTONE] The transfer function, with its display ceiling when it
    // has one; for every other table, that the option was ignored.
    if (toneApplies) {
        if (osvHdrToneActive(&params)) {
            std::printf("  tone      : %s (%s, ceiling %.0f nits)\n", hdrToneLabel(tone), hdrToneName(tone),
                        static_cast<double>(params.hdrToneCapNits));
        } else {
            std::printf("  tone      : %s (%s, BT.2408 scene-referred)\n", hdrToneLabel(tone), hdrToneName(tone));
        }
    } else if (tone != kDefaultHdrTone) {
        std::printf("  tone      : ignored (only D-Log M to PQ or HLG has a transfer function style)\n");
    }
    // [WP-HDRPEAK] What the peak did to this table.  HLG is display-relative:
    // the display showing it applies its own peak, so the signal is the same
    // whatever was asked for, and the report says so rather than staying
    // silent about an option it ignored.
    if (transfer == OutputTransfer::PQ) {
        if (rolledOff) {
            std::printf("  hdr peak  : %.0f nits (roll-off above %.0f nits, BT.2408 EETF)\n",
                        static_cast<double>(params.hdrPeakNits),
                        static_cast<double>(hdrPeakKneeNits(params.hdrPeakNits, params.peakNits)));
        } else {
            std::printf("  hdr peak  : %.0f nits (no roll-off)\n", static_cast<double>(params.peakNits));
        }
    } else if (hdrPeakNits < kDefaultHdrPeakNits) {
        std::printf("  hdr peak  : ignored (only the PQ output has an absolute peak)\n");
    }
    std::printf("  exposure  : %+.2f stops\n", opt.exposure);
    std::printf("  size      : %u^3 = %llu entries\n", opt.size, entries);
    // A grey anchor so the user can sanity check the LUT in their NLE.
    {
        const float grey[3] = {0.40f, 0.40f, 0.40f};
        float out[3] = {0.0f, 0.0f, 0.0f};
        osvCodeToOutput(&params, grey, out);
        std::printf("  grey 0.40 -> %.4f %.4f %.4f\n", static_cast<double>(out[0]), static_cast<double>(out[1]),
                    static_cast<double>(out[2]));
    }
    return kExitOk;
}

}  // namespace

void registerLutCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<LutOptions>();
    CLI::App* sub = app.add_subcommand("lut", "Write a .cube 3D LUT for the D-Log M colour pipeline");
    // The fit names are historical.  osmo360 is fitted to DJI's own D-Log M
    // LUT, which is the same file for the Pocket 3, so it is the right choice
    // for Pocket 3 footage too; the help says so because the name "pocket3"
    // (a community colour-chart fit) otherwise looks like the obvious pick.
    sub->add_option("--fit", opt->fit,
                    "D-Log M curve: osmo360 (default; matches DJI's own D-Log M LUT, Pocket 3 included), "
                    "dji or pocket3 (legacy community fit, shifts hues ~10 deg)")
        ->capture_default_str();
    sub->add_option("--out-transfer", opt->outTransfer, "Output encoding: pq (default), hlg, 709, linear, dlogm")
        ->capture_default_str();
    sub->add_option("--input", opt->input, "Source encoding: dlogm (default), hlg, 709")->capture_default_str();
    sub->add_option("--look", opt->look, "Rec.709 look: dji (DJI Studio, default) or standard")
        ->capture_default_str();
    sub->add_option("--hdr-peak", opt->hdrPeak,
                    "PQ output's peak in nits: 1000 (default, no roll-off), 600, 400 or 203 (SDR-safe)")
        ->capture_default_str();
    // [WP-HDRTONE] Source Settings' "Transfer Function (HDR)".
    sub->add_option("--tone", opt->tone,
                    "HDR transfer function (D-Log M to pq / hlg): aces-bright (default, outdoor), aces-detailed "
                    "(indoor), bt2408-natural, bt2408-punchy or bt2408-neutral (the BT.2408 scene-referred rendering "
                    "of 0.2.0 and earlier)")
        ->capture_default_str();
    sub->add_option("--size", opt->size, "Grid points per axis (2..256)")->capture_default_str();
    sub->add_option("--exposure", opt->exposure, "Exposure offset in stops")->capture_default_str();
    sub->add_option("--title", opt->title, "TITLE line written to the file");
    sub->add_flag("--narrow-input", opt->narrowInput,
                  "LUT input axis is limited-range video (16..235) instead of the expanded code");
    sub->add_option("out", opt->outPath, "Output .cube path")->required();
    // The callback runs during parse; the exit code lands in the context.
    sub->callback([opt, &ctx]() { ctx.exitCode = runLut(*opt); });
}

}  // namespace osvtool
