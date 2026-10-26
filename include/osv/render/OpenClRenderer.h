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
#include <string_view>

namespace osv::render {

/**
 * @brief Vendor-neutral OpenCL 1.2 GPU renderer backend.
 *
 * Compiles the shared shader sources (`preamble.cl`, `ColorMath.h`,
 * `osv_kernel.h`, and `kernel.cl`) at runtime for the selected OpenCL device
 * and executes `osvReframe`, `osvSeamLowDecimate`, and `osvSeamLowBlur`.
 */
class OpenClRenderer final : public IRenderer {
public:
    ~OpenClRenderer() override;

    /**
     * @brief Create a renderer on the `deviceIndex`-th enumerated OpenCL device.
     *
     * Devices are enumerated across all platforms and ranked so discrete GPUs
     * with larger dedicated memory come first, followed by integrated GPUs,
     * translation layers (OpenCLOn12), and CPU devices last.
     *
     * When `deviceIndex == 0` and a preferred device selector has been set via
     * `setPreferredDevice()` or the `OSV_CL_DEVICE` / `OSV_OPENCL_DEVICE`
     * environment variable, that selector is honoured.  Otherwise, if device 0
     * fails context creation or kernel compilation, `create(0)` automatically
     * falls back to the remaining enumerated devices in ranked order before
     * failing.
     *
     * @param deviceIndex  Zero-based index in the ranked device list.
     * @return             Initialized OpenCL renderer or `ErrorCode::Gpu` / `ErrorCode::InvalidArgument`.
     */
    static Result<std::unique_ptr<OpenClRenderer>> create(int deviceIndex = 0);

    /**
     * @brief Create a renderer matching an explicit index or name/vendor/platform substring.
     *
     * Accepts `"auto"` (or empty) for automatic ranked selection with fallback,
     * a non-negative integer index (`"0"`, `"1"`, ...), or a case-insensitive
     * substring matched against the device name, vendor, or platform name
     * (e.g. `"arc"`, `"9070"`, `"amd"`, `"intel"`).
     *
     * @param deviceSelector  Index or case-insensitive substring selector.
     * @return                Initialized OpenCL renderer or an error listing available devices.
     */
    static Result<std::unique_ptr<OpenClRenderer>> create(std::string_view deviceSelector);

    /**
     * @brief Set the process-wide preferred OpenCL device selector (from `--cl-device`).
     *
     * An empty string or `"auto"` clears the override so `OSV_CL_DEVICE` or
     * automatic ranked device selection is used.
     *
     * @param deviceSelector  Device index or name/vendor/platform substring.
     */
    static void setPreferredDevice(std::string deviceSelector);

    /**
     * @brief Return the currently configured preferred OpenCL device selector.
     */
    [[nodiscard]] static std::string preferredDevice();

    /// True when at least one OpenCL device is present.
    static bool available(std::string* reason);

    /// Number of enumerated devices.
    static int deviceCount();

    /// "Vendor Device (OpenCL 3.0)".
    static std::string deviceName(int deviceIndex);

    /// Full display name of the OpenCL device this renderer instance is bound to.
    [[nodiscard]] const std::string& activeDeviceName() const noexcept;

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
