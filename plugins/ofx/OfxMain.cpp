// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxMain.cpp - the module's OpenFX exports: the plug-in table, the host
// hand-off, load / unload, and DllMain.
//
// ===========================================================================
//  One module, two effects
// ===========================================================================
// OpenOSV.ofx carries both OpenFX effects:
//
//   0  "Open 360 Reframe"  (org.openosv.Open360Reframe) - a filter
//   1  "OpenOSV Source"    (org.openosv.OSVSource)      - a generator
//
// The host sees two plug-ins and loads each separately (kOfxActionLoad per
// plug-in), so the module-wide state - the log, the suites, the importer
// engine's renderers and decoders - is reference counted: set up on the
// first load, torn down on the last unload.
//
// Every entry point is noexcept and catches everything: an exception
// unwinding into the host's C call stack takes the host down with it, and a
// host that crashes while loading a plug-in blacklists it.
#include "OfxCuda.h"
#include "OfxHost.h"
#include "OfxReframe.h"
#include "OfxSource.h"

#include "DelayLoad.h"
#include "HostContext.h"
#include "PluginLog.h"

#include "osv/core/Version.h"
#include "osv/video/GpuDecoderPool.h"
#include "osv/video/ReaderPool.h"

#include <cstring>
#include <mutex>

namespace {

using osv::premiere::PluginLog;

// ===========================================================================
//  Module load / unload (reference counted over the two plug-ins)
// ===========================================================================

std::mutex g_loadMutex;
int g_loadCount = 0;

/// kOfxActionLoad for either plug-in.
OfxStatus moduleLoad() noexcept {
    std::lock_guard<std::mutex> lock(g_loadMutex);
    if (g_loadCount > 0) {
        ++g_loadCount;
        return kOfxStatOK;
    }
    // %LOCALAPPDATA%\OpenOSV\OpenOSVOfx.log - beside the Premiere plug-ins'
    // logs, so one folder holds every OpenOSV diagnosis.
    PluginLog::init(L"OpenOSVOfx");
    if (!osv::ofx::fetchSuites()) {
        return kOfxStatErrMissingHostFeature;
    }
    ++g_loadCount;
    PluginLog::info("OpenOSV {} OpenFX module loaded by '{}'", osv::Version::string(), osv::ofx::hostName());
    return kOfxStatOK;
}

/// kOfxActionUnload for either plug-in.
OfxStatus moduleUnload() noexcept {
    std::lock_guard<std::mutex> lock(g_loadMutex);
    if (g_loadCount <= 0) {
        return kOfxStatOK;
    }
    if (--g_loadCount > 0) {
        return kOfxStatOK;
    }
    // The last user is gone.  Tear down in the importer's own order
    // (ImporterEntry.cpp, imShutdown): the clips, then the parked decoders,
    // and finally the renderers and the thread pool.
    PluginLog::info("OpenOSV OpenFX module unloading: releasing clips, decoders and renderers");
    osv::ofx::source::shutdown();
    osv::video::ReaderPool::instance().clear();
    osv::video::GpuDecoderPool::instance().clear();
    osv::premiere::HostContext::shutdown();
    osv::ofx::cuda::releaseModules();
    osv::ofx::clearSuites();
    return kOfxStatOK;
}

/// True when two action names are the same string.
bool isAction(const char* action, const char* name) noexcept {
    return action && name && std::strcmp(action, name) == 0;
}

// ===========================================================================
//  The two plug-ins
// ===========================================================================

/// OfxPlugin::setHost - both plug-ins share the one host.
void setHost(OfxHost* host) { osv::ofx::setHost(host); }

/// The filter's mainEntry: module actions here, the rest in OfxReframe.cpp.
OfxStatus reframeEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                       OfxPropertySetHandle outArgs) {
    try {
        if (isAction(action, kOfxActionLoad)) {
            return moduleLoad();
        }
        if (isAction(action, kOfxActionUnload)) {
            return moduleUnload();
        }
        return osv::ofx::reframe_filter::mainEntry(action, handle, inArgs, outArgs);
    } catch (...) {
        return kOfxStatFailed;
    }
}

/// The generator's mainEntry.
OfxStatus sourceEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                      OfxPropertySetHandle outArgs) {
    try {
        if (isAction(action, kOfxActionLoad)) {
            return moduleLoad();
        }
        if (isAction(action, kOfxActionUnload)) {
            return moduleUnload();
        }
        return osv::ofx::source::mainEntry(action, handle, inArgs, outArgs);
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxPlugin g_reframePlugin = {
    kOfxImageEffectPluginApi,
    1,  // image effect API version
    osv::ofx::reframe_filter::kPluginId,
    osv::ofx::reframe_filter::kVersionMajor,
    osv::ofx::reframe_filter::kVersionMinor,
    &setHost,
    &reframeEntry,
};

OfxPlugin g_sourcePlugin = {
    kOfxImageEffectPluginApi,
    1,
    osv::ofx::source::kPluginId,
    osv::ofx::source::kVersionMajor,
    osv::ofx::source::kVersionMinor,
    &setHost,
    &sourceEntry,
};

}  // namespace

// ===========================================================================
//  The OpenFX exports
// ===========================================================================

/// Number of plug-ins in this module.
OfxExport int OfxGetNumberOfPlugins(void) { return 2; }

/// The nth plug-in; null for an index out of range.
OfxExport OfxPlugin* OfxGetPlugin(int nth) {
    switch (nth) {
    case 0:
        return &g_reframePlugin;
    case 1:
        return &g_sourcePlugin;
    default:
        return nullptr;
    }
}

/// OpenFX 1.5's optional module-level host hand-off, called (by hosts that
/// know it) before OfxGetNumberOfPlugins.  Equivalent to the per-plug-in
/// setHost above; a host that calls both simply sets the same pointer twice.
OfxExport OfxStatus OfxSetHost(const OfxHost* host) {
    osv::ofx::setHost(const_cast<OfxHost*>(host));
    return kOfxStatOK;
}

// ===========================================================================
//  DllMain
//
//  Only what is safe under the loader lock, exactly like the Premiere
//  plug-ins: point the delay-load hook at this module's own folder (the
//  bundle's Contents\Win64, where its FFmpeg / OpenCL / fmt / spdlog DLLs
//  sit), and turn off thread notifications.  No logging, no CUDA, no
//  allocation.
// ===========================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        osv::premiere::delayload::installHook();
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
