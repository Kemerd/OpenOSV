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
#include <utility>

namespace osv::render {

class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// Render one frame into a new host image.
    virtual Result<ImageRGBAf> render(const RenderJob& job) = 0;

    /// Render one frame into `out`, reusing its allocation when it is already
    /// the right size (see ImageRGBAf::reshape).
    ///
    /// For a caller that renders frame after frame at one size - the importer
    /// during playback - this removes a full-frame allocate and zero-fill per
    /// frame (~50 ms at native 6000 x 3000).
    ///
    /// On failure `out` may hold a partially written frame: callers must treat
    /// it as invalid until a later call succeeds.  The default implementation
    /// renders a new image and moves it in, so a backend that does not
    /// override this still works - it simply does not save the allocation.
    virtual Status renderInto(const RenderJob& job, ImageRGBAf& out) {
        OSV_TRY_ASSIGN(ImageRGBAf image, render(job));
        out = std::move(image);
        return okStatus();
    }

    /// Backend name for logs and --version ("cpu", "cuda", "opencl").
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// Create a renderer by name: "cpu", "cuda", "metal", "opencl" or "auto"
/// (best available: cuda > metal > opencl > cpu; Metal exists on macOS
/// only).  `chosen` receives the backend name.  Backends compiled out return
/// Unsupported.
Result<std::unique_ptr<IRenderer>> makeRenderer(const std::string& device, ThreadPool& pool, std::string* chosen);

}  // namespace osv::render
