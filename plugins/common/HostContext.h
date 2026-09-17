// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HostContext: the one process-wide object shared by every plug-in instance
// living in the host process.
//
// It owns
//   * a ThreadPool sized to the machine (the CPU renderer, the pixel copy
//     and the decoders run on it);
//   * at most one renderer per backend ("cuda", "opencl", "cpu"), created
//     lazily through osv::render::makeRenderer the first time a request
//     asks for it and cached afterwards.  A backend whose creation failed
//     (no device, driver too old, DLL missing) is remembered as
//     unavailable so the probe is not repeated on every frame.
//
// Lifetime is explicit: the singleton is created on first use (mutex
// guarded, from any thread) and destroyed only by shutdown(), which the
// importer calls from imShutdown and the effect from its global setdown.
// It is never torn down from a static destructor or DllMain, because by then
// the CUDA / OpenCL runtimes may already be unloaded and freeing device
// memory would crash the host.  If shutdown() is never called the objects
// are intentionally leaked to the process exit.
//
// Renderers are handed out as shared_ptr leases: a lease keeps the renderer
// (and the pool it may reference) alive even if shutdown() runs while a
// frame is still being rendered on another thread.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/Renderer.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace osv::premiere {

/// Which backend a caller wants.  The numeric values are the ones stored in
/// PrefsBlob::renderDevice.
enum class RenderDevicePreference : std::uint8_t {
    Auto = 0,    ///< cuda, then opencl, then cpu.
    Cpu = 1,     ///< Reference renderer only.
    Cuda = 2,    ///< CUDA only (fails when unavailable).
    OpenCl = 3,  ///< OpenCL only (fails when unavailable).
};

class HostContext {
public:
    /// A renderer plus what keeps it alive.
    struct RendererLease {
        std::shared_ptr<render::IRenderer> renderer;  ///< Never null on success.
        std::shared_ptr<ThreadPool> pool;             ///< The pool the renderer may reference.
        std::string backend;                          ///< "cuda", "opencl" or "cpu".
    };

    /// The singleton, created on first call.
    static HostContext& instance();

    /// True when instance() has been called and shutdown() has not.
    [[nodiscard]] static bool exists() noexcept;

    /// Destroy the singleton: releases every cached renderer and the pool.
    /// Safe to call when nothing exists.  Leases still held by callers stay
    /// valid until they are dropped.
    static void shutdown() noexcept;

    HostContext(const HostContext&) = delete;
    HostContext& operator=(const HostContext&) = delete;

    /// The shared thread pool.
    [[nodiscard]] ThreadPool& threadPool() noexcept { return *m_pool; }
    [[nodiscard]] std::shared_ptr<ThreadPool> threadPoolShared() noexcept { return m_pool; }

    /// Obtain a renderer for `pref`.  "Auto" walks cuda -> opencl -> cpu and
    /// returns the first that works; an explicit preference fails with Gpu /
    /// Unsupported when that backend is unavailable.  The CPU backend can
    /// only fail on allocation failure.
    Result<RendererLease> acquireRenderer(RenderDevicePreference pref);

    /// Same, by backend name ("auto", "cuda", "opencl", "cpu").
    Result<RendererLease> acquireRenderer(std::string_view backend);

    /// State of a backend as far as this context knows: true after a
    /// successful creation, false after a failed one; `probed` tells the
    /// two apart from "never asked".
    [[nodiscard]] bool backendAvailable(std::string_view backend, bool* probed = nullptr) const;

    /// Human readable reason of the last failure for a backend (empty when
    /// it never failed).
    [[nodiscard]] std::string backendFailure(std::string_view backend) const;

    /// Canonical name for a preference ("auto", "cpu", "cuda", "opencl").
    [[nodiscard]] static const char* preferenceName(RenderDevicePreference pref) noexcept;

private:
    HostContext();
    ~HostContext();

    /// One cached backend.
    struct Backend {
        std::shared_ptr<render::IRenderer> renderer;
        bool probed = false;
        std::string failure;
    };

    /// Create or fetch one specific backend (caller holds m_mutex).
    Result<std::shared_ptr<render::IRenderer>> obtainLocked(const std::string& name);

    /// Index of the backend slot for a name, or -1.
    static int slotFor(std::string_view name) noexcept;

    std::shared_ptr<ThreadPool> m_pool;
    mutable std::mutex m_mutex;
    Backend m_backends[3];  ///< cuda, opencl, cpu.
};

}  // namespace osv::premiere
