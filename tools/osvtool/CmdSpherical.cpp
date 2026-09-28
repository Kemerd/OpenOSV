// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool spherical`: tag an existing equirectangular MP4 / MOV as 360
// video (Spherical Video V1 + V2), for example a Premiere Pro or DaVinci
// Resolve export, so YouTube, VR players and 360 editors play it as a
// sphere.  `osvtool render --mode equirect` tags its own output already.
//
//   osvtool spherical <file.mp4> [--out other.mp4]
//
// In place by default; either way the file is written beside the target
// first and moved over it only once complete.

#include "Commands.h"

#include "osv/core/Log.h"
#include "osv/io/SphericalMetadata.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace osvtool {

namespace {

/// Options collected by CLI11 for the spherical command.
struct SphericalOptions {
    std::string input;  ///< The video to tag.
    std::string out;    ///< Optional: write the tagged copy here instead.
};

/// Run the command; returns one of the kExit* codes.
int runSpherical(const SphericalOptions& opt) {
    namespace fs = std::filesystem;

    // ---- the input ---------------------------------------------------------------
    if (opt.input.empty()) {
        std::fprintf(stderr, "error: input file is required\n");
        return kExitUsage;
    }
    const fs::path input(opt.input);
    std::error_code ec;
    if (!fs::is_regular_file(input, ec) || ec) {
        std::fprintf(stderr, "error: file not found: %s\n", osv::log::safe(opt.input).c_str());
        return kExitInput;
    }
    const fs::path output = opt.out.empty() ? fs::path() : fs::path(opt.out);

    // ---- tag ------------------------------------------------------------------------
    const osv::Result<osv::io::SphericalInjectReport> tagged = osv::io::injectSphericalMetadata(input, output);
    if (!tagged.ok()) {
        std::fprintf(stderr, "error: %s\n", osv::log::safe(tagged.error().toString()).c_str());
        // The input is the problem unless writing or replacing failed.
        switch (tagged.error().code) {
        case osv::ErrorCode::InvalidArgument:
            return kExitUsage;
        case osv::ErrorCode::Io:
        case osv::ErrorCode::Internal:
            return kExitRuntime;
        default:
            return kExitInput;
        }
    }

    // ---- report (7-bit ASCII only) ---------------------------------------------------
    const osv::io::SphericalInjectReport& r = tagged.value();
    const std::string written = osv::log::safe(opt.out.empty() ? opt.input : opt.out);
    if (!r.changed && opt.out.empty()) {
        std::printf("already tagged: %s is 360 equirectangular video, nothing to do\n", written.c_str());
        return kExitOk;
    }
    std::printf("tagged %s as 360 equirectangular video\n", written.c_str());
    std::printf("  metadata  : Spherical Video V1 (uuid) + V2 (st3d, sv3d/equi), monoscopic\n");
    std::printf("  track     : %u ('%s', %u sample entr%s)\n", r.videoTrackId, r.sampleEntryType.c_str(),
                r.sampleEntriesTagged, r.sampleEntriesTagged == 1 ? "y" : "ies");
    std::printf("  moov      : %+lld bytes%s\n", static_cast<long long>(r.moovGrowth),
                r.replacedExisting ? " (an earlier spherical tag was replaced)" : "");
    if (r.moovBeforeMdat) {
        std::printf("  offsets   : %llu chunk offsets moved (moov before the media data)\n",
                    static_cast<unsigned long long>(r.chunkOffsetsShifted));
    } else {
        std::printf("  offsets   : unchanged (media data before moov)\n");
    }
    return kExitOk;
}

}  // namespace

void registerSphericalCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<SphericalOptions>();
    CLI::App* sub = app.add_subcommand(
        "spherical", "Tag an equirectangular MP4 / MOV as 360 video (Spherical Video V1 + V2, for YouTube and VR)");
    sub->add_option("file", opt->input, "Equirectangular .mp4 / .mov to tag (in place unless --out)")->required();
    sub->add_option("--out", opt->out, "Write the tagged copy here and leave the input untouched");
    // The callback runs during parse; the exit code lands in the context.
    sub->callback([opt, &ctx]() { ctx.exitCode = runSpherical(*opt); });
}

}  // namespace osvtool
