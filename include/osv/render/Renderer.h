// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// IRenderer: the backend interface.  Every backend executes osvShadePixel from
// osv_kernel.h for each output pixel; only the execution strategy differs.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderJob.h"

#include <memory>
#include <string>

namespace osv::render {

class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// Render one frame into a host image.
    virtual Result<ImageRGBAf> render(const RenderJob& job) = 0;

    /// Backend name for logs and --version ("cpu", "cuda", "opencl").
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// Create a renderer by name: "cpu", "cuda", "opencl" or "auto" (best
/// available: cuda > opencl > cpu).  `chosen` receives the backend name.
/// Backends compiled out return Unsupported.
Result<std::unique_ptr<IRenderer>> makeRenderer(const std::string& device, ThreadPool& pool, std::string* chosen);

}  // namespace osv::render
