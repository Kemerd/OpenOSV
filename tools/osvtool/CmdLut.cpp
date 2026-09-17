// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool lut`: bake the D-Log M -> HLG / PQ / Rec.709 pipeline into a .cube
// 3D LUT for NLEs that cannot load OpenOSV directly.
//
//   osvtool lut [--fit dji|pocket3] [--out-transfer pq|hlg|709] [--size 65]
//               [--exposure 0] [--title "..."] [--input dlogm|hlg|709]
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
    std::string fit = "dji";
    std::string outTransfer = "pq";
    std::string input = "dlogm";
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
    DlogMFit fit = DlogMFit::DjiRefit;
    if (!parseDlogMFit(opt.fit, fit)) {
        std::fprintf(stderr, "error: unknown --fit '%s' (expected dji or pocket3)\n", osv::log::safe(opt.fit).c_str());
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
    const OsvColorParams params = makeColorParams(fit, transfer, static_cast<float>(opt.exposure), input);
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
        cube.title = std::string("OpenOSV ") + inputEncodingName(input) + " to " + outputTransferName(transfer) +
                     " (" + dlogMFitName(fit) + " fit)";
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
    sub->add_option("--fit", opt->fit, "D-Log M curve: dji (default) or pocket3")->capture_default_str();
    sub->add_option("--out-transfer", opt->outTransfer, "Output encoding: pq (default), hlg, 709, linear, dlogm")
        ->capture_default_str();
    sub->add_option("--input", opt->input, "Source encoding: dlogm (default), hlg, 709")->capture_default_str();
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
