// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Command registry and the --version report.  Commands whose module is not
// built (the OSV_HAVE_* macros come from tools/osvtool/CMakeLists.txt) are
// skipped so the tool always links, even in a partial build.

#include "Commands.h"

#include "osv/core/Version.h"
#if defined(OSV_HAVE_VIDEO)
#include "osv/video/Decoder.h"
#endif
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif
#if defined(OSV_HAVE_METAL)
#include "osv/render/MetalRenderer.h"
#endif

#include <cstdio>
#include <string>

namespace osvtool {

void registerAllCommands(CLI::App& app, CommandContext& ctx) {
#if defined(OSV_HAVE_META)
    registerProbeCommand(app, ctx);
#endif
#if defined(OSV_HAVE_VIDEO)
    registerExtractCommand(app, ctx);
#endif
#if defined(OSV_HAVE_IO)
    registerRenderCommand(app, ctx);
#endif
#if defined(OSV_HAVE_COLOR)
    registerLutCommand(app, ctx);
#endif
#if defined(OSV_HAVE_GEOM)
    registerSeamCommand(app, ctx);
#endif
#if defined(OSV_HAVE_IO)
    registerSelfcheckCommand(app, ctx);
#endif
    (void)app;
    (void)ctx;
}

int printVersion(CommandContext& ctx) {
    std::printf("%s %s\n", osv::Version::productName(), osv::Version::string());
    std::printf("  modules : core");
#if defined(OSV_HAVE_CONTAINER)
    std::printf(" container");
#endif
#if defined(OSV_HAVE_META)
    std::printf(" meta");
#endif
#if defined(OSV_HAVE_COLOR)
    std::printf(" color");
#endif
#if defined(OSV_HAVE_VIDEO)
    std::printf(" video");
#endif
#if defined(OSV_HAVE_GEOM)
    std::printf(" geom");
#endif
#if defined(OSV_HAVE_RENDER)
    std::printf(" render_cpu");
#endif
#if defined(OSV_HAVE_CUDA)
    std::printf(" render_cuda");
#endif
#if defined(OSV_HAVE_OPENCL)
    std::printf(" render_opencl");
#endif
#if defined(OSV_HAVE_METAL)
    std::printf(" render_metal");
#endif
#if defined(OSV_HAVE_IO)
    std::printf(" io");
#endif
    std::printf("\n");
    printBackendDiagnostics();
    ctx.exitCode = kExitOk;
    return kExitOk;
}

void printBackendDiagnostics() {
#if defined(OSV_HAVE_VIDEO)
    std::printf("  ffmpeg  : %s%s\n", osv::video::HevcStreamDecoder::ffmpegVersion().c_str(),
                osv::video::HevcStreamDecoder::ffmpegIsGpl() ? " (GPL build - not redistributable with OpenOSV)"
                                                               : " (LGPL)");
#endif
#if defined(OSV_HAVE_CUDA)
    {
        std::string reason;
        if (osv::render::CudaRenderer::available(&reason)) {
            std::printf("  cuda    : %s\n", osv::render::CudaRenderer::deviceName(0).c_str());
        } else {
            std::printf("  cuda    : unavailable (%s)\n", reason.c_str());
        }
    }
#endif
#if defined(OSV_HAVE_OPENCL)
    {
        std::string reason;
        if (osv::render::OpenClRenderer::available(&reason)) {
            std::printf("  opencl  : %s\n", osv::render::OpenClRenderer::deviceName(0).c_str());
        } else {
            std::printf("  opencl  : unavailable (%s)\n", reason.c_str());
        }
    }
#endif
#if defined(OSV_HAVE_METAL)
    {
        std::string reason;
        if (osv::render::MetalRenderer::available(&reason)) {
            std::printf("  metal   : %s\n", osv::render::MetalRenderer::deviceName(0).c_str());
        } else {
            std::printf("  metal   : unavailable (%s)\n", reason.c_str());
        }
    }
#endif
}

}  // namespace osvtool
