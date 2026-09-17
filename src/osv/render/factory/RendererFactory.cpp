// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// makeRenderer(): picks a backend by name.  This translation unit lives in the
// umbrella target osv_render, which links whichever backends were built, so
// the OSV_HAVE_* macros decide what "auto" can choose.

#include "osv/render/Renderer.h"
#include "osv/render/CpuRenderer.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include "osv/core/Log.h"

#include <algorithm>
#include <cctype>

namespace osv::render {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

Result<std::unique_ptr<IRenderer>> makeRenderer(const std::string& deviceIn, ThreadPool& pool, std::string* chosen) {
    const std::string device = lower(deviceIn.empty() ? "auto" : deviceIn);

    if (device == "cpu") {
        if (chosen) {
            *chosen = "cpu";
        }
        return std::unique_ptr<IRenderer>(std::make_unique<CpuRenderer>(pool));
    }

    if (device == "cuda" || device == "auto") {
#if defined(OSV_HAVE_CUDA)
        auto r = CudaRenderer::create(0);
        if (r.ok()) {
            if (chosen) {
                *chosen = "cuda";
            }
            return std::unique_ptr<IRenderer>(std::move(r).value());
        }
        if (device == "cuda") {
            return Error(r.error());
        }
        log::debug("auto renderer: CUDA unavailable ({})", r.error().message);
#else
        if (device == "cuda") {
            return Error{ErrorCode::Unsupported, "CUDA backend not compiled in (OSV_ENABLE_CUDA=OFF)"};
        }
#endif
    }

    if (device == "opencl" || device == "auto") {
#if defined(OSV_HAVE_OPENCL)
        auto r = OpenClRenderer::create(0);
        if (r.ok()) {
            if (chosen) {
                *chosen = "opencl";
            }
            return std::unique_ptr<IRenderer>(std::move(r).value());
        }
        if (device == "opencl") {
            return Error(r.error());
        }
        log::debug("auto renderer: OpenCL unavailable ({})", r.error().message);
#else
        if (device == "opencl") {
            return Error{ErrorCode::Unsupported, "OpenCL backend not compiled in (OSV_ENABLE_OPENCL=OFF)"};
        }
#endif
    }

    if (device == "auto") {
        if (chosen) {
            *chosen = "cpu";
        }
        return std::unique_ptr<IRenderer>(std::make_unique<CpuRenderer>(pool));
    }

    return Error{ErrorCode::InvalidArgument, "unknown renderer device '" + deviceIn + "' (cpu|cuda|opencl|auto)"};
}

}  // namespace osv::render
