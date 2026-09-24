// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "HostContext.h"

#include "PluginLog.h"

#include <algorithm>
#include <cctype>

namespace osv::premiere {

namespace {

/// Guards creation and destruction of the singleton.
std::mutex g_instanceMutex;

/// The singleton; a raw pointer on purpose so no static destructor ever
/// runs for it (see the header).
HostContext* g_instance = nullptr;

/// Backend names in slot order, which is also Auto's preference order.
/// macOS has no CUDA: its native GPU backend, Metal, takes the first slot
/// (and the prefs value that means "CUDA" on Windows, see preferenceName).
#if defined(__APPLE__)
constexpr const char* kBackendNames[3] = {"metal", "opencl", "cpu"};
#else
constexpr const char* kBackendNames[3] = {"cuda", "opencl", "cpu"};
#endif

std::string lowerCopy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Singleton management
// -----------------------------------------------------------------------------
HostContext& HostContext::instance() {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    if (!g_instance) {
        g_instance = new HostContext();
        PluginLog::info("HostContext created ({} worker threads)", g_instance->m_pool->size());
    }
    return *g_instance;
}

bool HostContext::exists() noexcept {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    return g_instance != nullptr;
}

void HostContext::shutdown() noexcept {
    HostContext* doomed = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_instanceMutex);
        doomed = g_instance;
        g_instance = nullptr;
    }
    if (!doomed) {
        return;
    }
    try {
        PluginLog::info("HostContext shutdown");
        delete doomed;
    } catch (...) {
        // A renderer destructor must not throw; if one does, swallowing is
        // the only option inside a host process.
    }
}

HostContext::HostContext() : m_pool(std::make_shared<ThreadPool>(0)) {}

HostContext::~HostContext() {
    // Release renderers before the pool: the CPU renderer holds a reference
    // to the pool.  Leases elsewhere keep both alive through shared_ptr.
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Backend& b : m_backends) {
        b.renderer.reset();
    }
}

// -----------------------------------------------------------------------------
//  Renderers
// -----------------------------------------------------------------------------
const char* HostContext::preferenceName(RenderDevicePreference pref) noexcept {
    switch (pref) {
    case RenderDevicePreference::Auto: return "auto";
    case RenderDevicePreference::Cpu: return "cpu";
#if defined(__APPLE__)
    // The stored value 2 is "the platform's GPU API": CUDA on Windows,
    // Metal on macOS, so a project moved between the two keeps meaning
    // "render on the GPU" (the Source Settings popup names it accordingly).
    case RenderDevicePreference::Cuda: return "metal";
#else
    case RenderDevicePreference::Cuda: return "cuda";
#endif
    case RenderDevicePreference::OpenCl: return "opencl";
    }
    return "auto";
}

int HostContext::slotFor(std::string_view name) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (name == kBackendNames[i]) {
            return i;
        }
    }
    return -1;
}

Result<std::shared_ptr<render::IRenderer>> HostContext::obtainLocked(const std::string& name) {
    const int slot = slotFor(name);
    if (slot < 0) {
        return Error{ErrorCode::InvalidArgument, "HostContext: unknown backend '" + name + "'"};
    }
    Backend& b = m_backends[slot];

    // Cached success.
    if (b.renderer) {
        return b.renderer;
    }
    // Cached failure: do not probe the device again for every frame.
    if (b.probed) {
        return Error{ErrorCode::Unsupported, "HostContext: backend '" + name + "' unavailable: " + b.failure};
    }

    b.probed = true;
    std::string chosen;
    auto created = render::makeRenderer(name, *m_pool, &chosen);
    if (!created.ok()) {
        b.failure = created.error().toString();
        PluginLog::warn("HostContext: renderer '{}' unavailable: {}", name, b.failure);
        return Error(created.error());
    }
    if (!created.value()) {
        b.failure = "makeRenderer returned null";
        PluginLog::error("HostContext: makeRenderer('{}') returned a null renderer", name);
        return Error{ErrorCode::Internal, b.failure};
    }
    b.renderer = std::shared_ptr<render::IRenderer>(std::move(created).value());
    PluginLog::info("HostContext: renderer '{}' ready", b.renderer->name());
    return b.renderer;
}

Result<HostContext::RendererLease> HostContext::acquireRenderer(std::string_view backendIn) {
    const std::string backend = lowerCopy(backendIn.empty() ? std::string_view("auto") : backendIn);
    std::lock_guard<std::mutex> lock(m_mutex);

    // Explicit backend.
    if (backend != "auto") {
        auto r = obtainLocked(backend);
        if (!r.ok()) {
            return Error(r.error());
        }
        return RendererLease{r.value(), m_pool, backend};
    }

    // Auto: best available in fixed preference order.  Failures are logged
    // by obtainLocked (once per backend) and remembered.
    std::string lastFailure;
    for (const char* name : kBackendNames) {
        auto r = obtainLocked(name);
        if (r.ok()) {
            return RendererLease{r.value(), m_pool, name};
        }
        lastFailure = r.error().message;
    }
    return Error{ErrorCode::Internal, "HostContext: no renderer backend available: " + lastFailure};
}

Result<HostContext::RendererLease> HostContext::acquireRenderer(RenderDevicePreference pref) {
    return acquireRenderer(std::string_view(preferenceName(pref)));
}

bool HostContext::backendAvailable(std::string_view backend, bool* probed) const {
    const int slot = slotFor(lowerCopy(backend));
    if (slot < 0) {
        if (probed) {
            *probed = false;
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const Backend& b = m_backends[slot];
    if (probed) {
        *probed = b.probed;
    }
    return b.renderer != nullptr;
}

std::string HostContext::backendFailure(std::string_view backend) const {
    const int slot = slotFor(lowerCopy(backend));
    if (slot < 0) {
        return "unknown backend";
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_backends[slot].failure;
}

}  // namespace osv::premiere
