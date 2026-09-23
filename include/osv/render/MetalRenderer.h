// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MetalRenderer: the GPU backend on macOS.  The kernel library is built from
// the same shared shader as every other backend (preamble.metal + ColorMath.h
// + osv_kernel.h + kernel.metal, concatenated at build time and embedded), so
// Apple GPUs execute literally the same per-pixel code as the CPU reference.
//
// When the build machine has the Metal compiler the library is also compiled
// ahead of time into a metallib and embedded; the renderer then loads it
// without a run-time compile.  Otherwise (or if the metallib does not load on
// the device at hand) the embedded source is compiled when the renderer is
// created, with fast math off.
//
// Frames live in shared-storage buffers: on Apple Silicon the CPU and GPU
// share one memory, so an "upload" is a strided memcpy into the buffer the
// GPU reads and the readback is a memcpy out of the buffer it wrote.
//
// This header is plain C++ (no Objective-C types), so any translation unit
// can include it; the implementation is Objective-C++.
#pragma once

#include "osv/render/Renderer.h"

#include <memory>
#include <string>

namespace osv::render {

class MetalRenderer final : public IRenderer {
public:
    ~MetalRenderer() override;

    /// Create a renderer on the `deviceIndex`-th Metal device (0 = the
    /// system default GPU, then the remaining devices in the order Metal
    /// lists them).  Fails with Gpu when no device exists or the kernel
    /// library cannot be built (the compiler log is in the message).
    static Result<std::unique_ptr<MetalRenderer>> create(int deviceIndex = 0);

    /// True when at least one Metal device is present.
    static bool available(std::string* reason);

    /// Number of Metal devices.
    static int deviceCount();

    /// The device's name, e.g. "Apple M2 Pro" ("unknown" out of range).
    static std::string deviceName(int deviceIndex);

    Result<ImageRGBAf> render(const RenderJob& job) override;
    [[nodiscard]] const char* name() const noexcept override { return "metal"; }

    /// The compiler log of a run-time library build ("" when the embedded
    /// metallib was used or the build produced no messages).
    [[nodiscard]] const std::string& buildLog() const noexcept;

    /// True when the library came from the ahead-of-time metallib rather
    /// than a run-time compile of the embedded source.
    [[nodiscard]] bool usesPrecompiledLibrary() const noexcept;

private:
    MetalRenderer();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::render
