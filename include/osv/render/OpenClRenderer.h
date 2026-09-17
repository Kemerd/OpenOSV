// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OpenClRenderer: vendor-neutral GPU backend.  The kernel is built at run time
// from the embedded sources (preamble.cl + ColorMath.h + osv_kernel.h +
// kernel.cl) so the very same shader code as the CPU and CUDA paths executes
// on AMD, Intel and NVIDIA devices.
//
// Numerical policy: built with -cl-fp32-correctly-rounded-divide-sqrt and no
// fast-relaxed-math; OpenCL's built-in transcendental functions are allowed a
// few ulp, which is invisible after 16-bit quantisation.
#pragma once

#include "osv/render/Renderer.h"

#include <memory>
#include <string>

namespace osv::render {

class OpenClRenderer final : public IRenderer {
public:
    ~OpenClRenderer() override;

    /// Create a renderer on the `deviceIndex`-th GPU device (enumerated over
    /// all platforms; CPU devices come last).  Fails with Gpu when no device
    /// exists or the kernel does not compile (the build log is in the message).
    static Result<std::unique_ptr<OpenClRenderer>> create(int deviceIndex = 0);

    /// True when at least one OpenCL device is present.
    static bool available(std::string* reason);

    /// Number of enumerated devices.
    static int deviceCount();

    /// "Vendor Device (OpenCL 3.0)".
    static std::string deviceName(int deviceIndex);

    Result<ImageRGBAf> render(const RenderJob& job) override;
    [[nodiscard]] const char* name() const noexcept override { return "opencl"; }

    /// The program build log of the last compilation (for diagnostics).
    [[nodiscard]] const std::string& buildLog() const noexcept;

private:
    OpenClRenderer();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::render
