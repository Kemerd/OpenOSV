// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// xImportEntry: the one exported symbol of OpenOSVImporter.prm, and the
// lifetime selectors that do not deserve a file of their own (imInit,
// imShutdown, imGetIndFormat, imOpenFile8, imQuietFile, imCloseFile, the
// imGetSupports* trio).
//
// Three rules the whole dispatcher obeys:
//
//   1. Every extern "C" entry point catches (...) and returns an error code.
//      An exception escaping into Premiere's C call stack is undefined
//      behaviour and in practice takes the host down; a C++ library that
//      allocates (which this one does, on every frame) can throw.
//   2. Unknown selectors return imUnsupported - a NON-ERROR code per
//      PrImporterReturnValueIsError() - and are logged exactly once each, so
//      a support log shows what the host asked for without a million lines.
//   3. Nothing hardware-specific happens in imInit or DllMain.  The GPU is
//      probed the first time a frame is rendered, and torn down from
//      imShutdown, never from DllMain (by then CUDA may already be unloaded
//      and freeing device memory would crash the host).

#include "ImporterPlugin.h"

#include "ImporterInstance.h"

#if defined(_WIN32)
#include "DelayLoad.h"
#endif
#include "Engine.h"
#include "HostContext.h"
#include "HostUtf16.h"
#include "PluginLog.h"
#include "UserDefaults.h"
#include "osv/video/GpuDecoderPool.h"
#include "osv/video/ReaderPool.h"

#include "PrSDKEntry.h"
#include "PrSDKMALErrors.h"

#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace osv::premiere {

namespace {

#if defined(_WIN32)
/// Captured in DllMain; used by the Source Settings dialog to find its own
/// resources.  A plain HINSTANCE store from DllMain is one of the very few
/// things that is safe under the loader lock.
HINSTANCE g_module = nullptr;
#else
/// macOS has no DllMain and the importer no dialog resources to find there
/// (the Source Settings effect is the UI), so the handle stays null.
void* const g_module = nullptr;
#endif

/// Copy a C string into a fixed char field, truncating, always terminated.
/// strncpy_s(..., _TRUNCATE) on Windows, where it is what the importer always
/// used; a bounded copy elsewhere, where there is no strncpy_s.
void copyTruncated(char* dst, std::size_t capacity, const char* src) noexcept {
    if (!dst || capacity == 0) {
        return;
    }
#if defined(_WIN32)
    ::strncpy_s(dst, capacity, src ? src : "", _TRUNCATE);
#else
    std::size_t n = 0;
    if (src) {
        while (n + 1 < capacity && src[n] != '\0') {
            ++n;
        }
        std::memcpy(dst, src, n);
    }
    dst[n] = '\0';
#endif
}

/// The process-wide plug-in state.  A function-local static rather than a
/// namespace-scope object so its construction order is defined and so it is
/// never constructed before DllMain has run.
ImporterGlobals& globalsImpl() noexcept {
    static ImporterGlobals instance;
    return instance;
}

/// Name of a selector, for the log.  Only the ones this importer cares about
/// are named; everything else is reported by number.
[[nodiscard]] const char* selectorName(csSDK_int32 selector) noexcept {
    switch (selector) {
    case imInit:                    return "imInit";
    case imShutdown:                return "imShutdown";
    case imGetIndFormat:            return "imGetIndFormat";
    case imGetSupports7:            return "imGetSupports7";
    case imGetSupports8:            return "imGetSupports8";
    case imGetSupportsPerInstancePrefs: return "imGetSupportsPerInstancePrefs";
    case imOpenFile8:               return "imOpenFile8";
    case imQuietFile:               return "imQuietFile";
    case imCloseFile:               return "imCloseFile";
    case imGetInfo8:                return "imGetInfo8";
    case imGetInfo9:                return "imGetInfo9";
    case imGetIndPixelFormat:       return "imGetIndPixelFormat";
    case imGetPreferredFrameSize:   return "imGetPreferredFrameSize";
    case imSelectClipFrameDescriptor:  return "imSelectClipFrameDescriptor";
    case imSelectClipFrameDescriptor2: return "imSelectClipFrameDescriptor2";
    case imGetIndColorSpace:        return "imGetIndColorSpace";
    case imGetSourceVideo:          return "imGetSourceVideo";
    case imImportAudio7:            return "imImportAudio7";
    case imResetSequentialAudio:    return "imResetSequentialAudio";
    case imGetSequentialAudio:      return "imGetSequentialAudio";
    case imGetAudioChannelLayout:   return "imGetAudioChannelLayout";
    case imGetPrefs8:               return "imGetPrefs8";
    case imGetInstancePrefs:        return "imGetInstancePrefs";
    case imPerformSourceSettingsCommand: return "imPerformSourceSettingsCommand";
    case imAnalysis:                return "imAnalysis";
    case imGetTimeInfo8:            return "imGetTimeInfo8";
    case imGetFileAttributes:       return "imGetFileAttributes";
    case imCreateAsyncImporter:     return "imCreateAsyncImporter";
    case imGetColorSpaceFromOpaqueData: return "imGetColorSpaceFromOpaqueData";
    default:                        return nullptr;
    }
}

// ---------------------------------------------------------------------------
//  imInit
// ---------------------------------------------------------------------------

/// [WP-DEFAULTS] The UserDefaults log sink of this module: every message of
/// plugins/common/UserDefaults.cpp lands in OpenOSVImporter.log.
void logUserDefaultsMessage(UserDefaultsLogLevel level, std::string_view message) noexcept {
    PluginLog::Level mapped = PluginLog::Level::Debug;
    switch (level) {
    case UserDefaultsLogLevel::Info:  mapped = PluginLog::Level::Info; break;
    case UserDefaultsLogLevel::Warn:  mapped = PluginLog::Level::Warn; break;
    case UserDefaultsLogLevel::Error: mapped = PluginLog::Level::Error; break;
    case UserDefaultsLogLevel::Debug:
    default:                          mapped = PluginLog::Level::Debug; break;
    }
    if (PluginLog::enabled(mapped)) {
        PluginLog::write(mapped, message);
    }
}

csSDK_int32 doInit(imStdParms* stdParms, imImportInfoRec* info) {
    if (!info) {
        return imOtherErr;
    }

    PluginLog::init(kLogName);
    // [WP-DEFAULTS] The defaults file's own messages (read, saved, ignored as
    // corrupt) belong in this module's log, not on a stderr nobody sees.
    setUserDefaultsLogSink(&logUserDefaultsMessage);
    // Copy the version struct under the lock that publishes it: ensureSuites()
    // has already run for this selector, but on another thread it may be
    // running for a different one right now.
    HostVersion host;
    {
        ImporterGlobals& g = globals();
        std::lock_guard<std::mutex> guard(g.mutex);
        host = g.host;
    }
    PluginLog::info("OpenOSV importer imInit: host {} {}, importer interface version {}", host.appName(),
                    host.toString(), stdParms ? stdParms->imInterfaceVer : 0);

    // The whole capability declaration in one place (decision D12 and the
    // design doc's "Registration and lifetime").
    info->canSave = kPrFalse;
    info->canDelete = kPrFalse;
    info->canResize = kPrFalse;
    info->canDoSubsize = kPrFalse;
    info->canDoContinuousTime = kPrFalse;
    info->noFile = kPrFalse;
    info->addToMenu = imMenuNone;
    // The modal Source Settings dialog stays available (right-click > Source
    // Settings) alongside the effect below.  Two routes to the same PrefsBlob
    // on purpose: the dialog is muscle memory for existing users, and a
    // machine where OpenOSVSourceSettings.aex failed to install still needs a
    // way to reach the options.
    info->hasSetup = kPrTrue;
    info->setupOnDblClk = kPrFalse;
    info->dontCache = kPrFalse;
    // The module is unloaded when no clip needs it; nothing here is
    // expensive to set up again.
    info->keepLoaded = kPrFalse;
    // Nobody else claims .osv, so there is no need to outrank another
    // importer.
    info->priority = 0;
    info->canAsync = kPrFalse;
    info->canCreate = kPrFalse;
    info->canCalcSizes = kPrFalse;
    info->canTrim = kPrFalse;
    // AAC is compressed; let the host conform it once rather than claiming
    // random access we would have to fake (decision D11).
    info->avoidAudioConform = kPrFalse;
    info->canCopy = kPrFalse;
    info->canSupplyMetadataClipName = kPrFalse;
    info->canValidatePrefs = kPrFalse;
    info->canProvidePeakAudio = kPrFalse;
    info->canProvideFileList = kPrFalse;
    info->canProvideClosedCaptions = kPrFalse;
    // The master clip Source Settings effect (OpenOSVSourceSettings.aex).
    // This flag is what makes Premiere look at
    // imFileInfoRec8::sourceSettingsMatchName at all; without it the field is
    // ignored and the effect is never attached, so the two must be set
    // together.  imGetInfo8 fills the name from
    // plugins/common/SourceSettingsIdentity.h.
    info->hasSourceSettingsEffect = kPrTrue;
    info->hasPersistentData = kPrFalse;

    // imIsCacheable tells the host it may skip loading us on later launches.
    // That is only honest because imInit probes no hardware: the GPU is
    // found lazily on the first frame, so a machine that gains or loses a
    // CUDA device between sessions still behaves correctly.
    return imIsCacheable;
}

// ---------------------------------------------------------------------------
//  imGetIndFormat
// ---------------------------------------------------------------------------

csSDK_int32 doGetIndFormat(csSDK_int32 index, imIndFormatRec* rec) {
    if (!rec) {
        return imOtherErr;
    }
    if (index != 0) {
        return imBadFormatIndex;
    }

    rec->filetype = kOsvFileType;
    rec->canWriteTimecode = kPrFalse;
    rec->canWriteMetaData = kPrFalse;
    rec->hasAlternateTypes = kPrFalse;

    // xfIsMovie is documented as obsolete but is still required for the
    // filetype to appear in Proxy > Attach Proxies (SDK guide, 7.3.19).
    rec->flags = xfCanOpen | xfCanImport | xfIsMovie;

    std::memset(rec->FormatName, 0, sizeof(rec->FormatName));
    std::memset(rec->FormatShortName, 0, sizeof(rec->FormatShortName));
    std::memset(rec->PlatformExtension, 0, sizeof(rec->PlatformExtension));
    copyTruncated(rec->FormatName, sizeof(rec->FormatName), kFormatName);
    copyTruncated(rec->FormatShortName, sizeof(rec->FormatShortName), kFormatShortName);

    // Extensions are NUL separated and the LIST is NUL terminated, so the
    // bytes on the wire must be: o s v \0 l r f \0 \0.
    //
    // The literal below spells all nine out, including the final terminator,
    // rather than relying on sizeof() silently contributing it: the value has
    // embedded NULs, so `sizeof` is the only correct length and strncpy_s
    // (which the two fields above legitimately use) would truncate it to
    // "osv" and quietly lose .lrf proxy recognition.  memcpy with an explicit
    // sizeof of an explicitly terminated literal makes both facts local.
    static const char kExtensions[] = "osv\0lrf\0\0";
    static_assert(sizeof(kExtensions) == 10,
                  "the extension list must be o s v NUL l r f NUL NUL plus the literal's own terminator");
    // Copy nine bytes: the list and its terminator.  The tenth (the literal's
    // own implicit NUL) is redundant here because the field was memset above,
    // but copying it costs nothing and removes the dependency on that memset.
    std::memcpy(rec->PlatformExtension, kExtensions, sizeof(kExtensions) - 1u);
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  imOpenFile8
// ---------------------------------------------------------------------------

csSDK_int32 doOpenFile8(imStdParms* stdParms, imFileRef* fileRef, imFileOpenRec8* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    PlugMemoryFuncsPtr memFuncs = stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr;

    if (!rec->fileinfo.filepath) {
        return imBadFile;
    }
    // prUTF16Char is a 16-bit code unit: on Windows exactly wchar_t, on
    // macOS transcoded to the native UTF-8 path (HostUtf16.h).
    const std::filesystem::path path = pathFromHostUtf16(rec->fileinfo.filepath);
    if (path.empty()) {
        return imBadFile;
    }

    // An existing privateData means this is an unquiet of a clip we already
    // parsed; reuse the instance so the metadata is not read again.
    ImporterInstance* instance = instanceFromHandle(rec->privatedata, memFuncs);
    bool created = false;
    if (!instance) {
        instance = new (std::nothrow) ImporterInstance(std::filesystem::path(path));
        if (!instance) {
            return imMemErr;
        }
        created = true;

        // [WP-DEFAULTS] A new instance starts from the user's saved Source
        // Settings defaults instead of the built-in ones.  For a clip with
        // stored settings that is only the starting point - the host hands
        // the stored blob to imGetInfo8, next, and it replaces the seed
        // before anything is rendered - but for a NEW clip, which has no
        // blob, it is what the clip is decoded with.  Seeded before open(),
        // because open() builds the lens rig from the calibration in force.
        // noteNewClipDefaults() logs the new-clip case once it is certain.
        const UserDefaults startFrom = currentUserDefaults();
        instance->seedStartingPrefs(startFrom.prefs,
                                    startFrom.fromFile ? userDefaultsPathForLog(startFrom.path) : std::string());
    }

    const Status st = instance->open();
    if (!st.ok()) {
        // The design doc and the SDK are both explicit: close the handle
        // before returning imBadFile or no lower-priority importer can open
        // the file.  ImporterInstance::open() already did that on failure.
        PluginLog::debug("imOpenFile8: '{}' is not an OpenOSV clip: {}",
                         std::filesystem::path(path).filename().string(), st.error().message);
        if (created) {
            delete instance;
        }
        return st.error().code == ErrorCode::Io ? imFileOpenFailed : imBadFile;
    }

    // Publish the OS handle: the host stores it and hands it back as param1
    // of the selectors that take an imFileRef.
    if (fileRef) {
        *fileRef = instance->fileHandle();
    }
    rec->fileinfo.fileref = instance->fileHandle();
    rec->fileinfo.filetype = kOsvFileType;

    // The importer id keys the PPix cache for this clip.
    instance->setImporterId(static_cast<std::uint32_t>(rec->inImporterID));

    // Tell the host roughly what we cost while open so its media-cache
    // accounting is not blind to our decoders.
    rec->outExtraMemoryUsage = static_cast<csSDK_size_t>(instance->extraMemoryUsage());

    if (created) {
        void* handle = allocateHandleFor(memFuncs, instance);
        if (!handle) {
            delete instance;
            return imMemErr;
        }
        rec->privatedata = handle;

        // Everything the open discovered, once per clip.
        for (const std::string& note : instance->notes()) {
            PluginLog::debug("open '{}': {}", instance->path().filename().string(), note);
        }
        PluginLog::info("imOpenFile8: '{}' opened - {} x {} per lens, {} frames, {:.3f} fps, audio {} ch",
                        instance->path().filename().string(), instance->format().lensW(),
                        instance->format().lensH(), instance->frameCount(), instance->fps(),
                        instance->audioChannels());
    }
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  imQuietFile / imCloseFile
// ---------------------------------------------------------------------------

csSDK_int32 doQuietFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData) {
    PlugMemoryFuncsPtr memFuncs = stdParms && stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr;
    ImporterInstance* instance = instanceFromHandle(privateData, memFuncs);
    if (instance) {
        // Decoders, renderer lease, audio decoder and the OS handle go; the
        // parsed metadata stays so the next unquiet is nearly free.
        instance->releaseHeavy();
    }
    // The handle is closed, so the host's copy must be invalidated too.
    if (fileRef) {
        *fileRef = imInvalidHandleValue;
    }
    return imNoErr;
}

csSDK_int32 doCloseFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData) {
    PlugMemoryFuncsPtr memFuncs = stdParms && stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr;
    ImporterInstance* instance = instanceFromHandle(privateData, memFuncs);
    if (instance) {
        PluginLog::debug("imCloseFile: '{}'", instance->path().filename().string());
        delete instance;
        // Clear the pointer in the handle so a second imCloseFile (which the
        // host does send in some teardown paths) is a no-op instead of a
        // double free.
        storeInstanceInHandle(privateData, memFuncs, nullptr);
    }
    if (privateData && memFuncs && memFuncs->disposeHandle) {
        memFuncs->disposeHandle(static_cast<PrMemoryHandle>(privateData));
    }
    if (fileRef) {
        *fileRef = imInvalidHandleValue;
    }
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  imShutdown
// ---------------------------------------------------------------------------

csSDK_int32 doShutdown() {
    PluginLog::info("imShutdown: releasing the renderer pool and the host suites");
    // The direct-GPU engine first: its NVDEC decoders live in Premiere's CUDA
    // context and must be released while that context and the driver are
    // still alive.  Then the renderers (and with them any CUDA / OpenCL
    // context of our own), also here and never from DllMain.
    engineShutdown();

    // Readers parked by quiet / close (osv::video::ReaderPool) hold decoders,
    // hardware devices and a file mapping for up to a minute so that a reopen
    // is instant.  At shutdown nobody will reopen anything: release them now,
    // while the D3D11 / CUDA runtimes are certainly alive, instead of letting
    // the pool's idle timer do it after the host has moved on.  The pool's
    // background thread also pins this module while it runs, so clearing here
    // lets the module unload promptly.
    osv::video::ReaderPool::instance().clear();
    // The same for the importer frame's NVDEC decoders parked by quiet /
    // close (osv::video::GpuDecoderPool).  They live in the primary context
    // of the renderer's device and hold VRAM there, so they go BEFORE the
    // renderer pool below and while the CUDA driver is certainly alive.
    osv::video::GpuDecoderPool::instance().clear();
    HostContext::shutdown();

    // Same lock the acquisition uses.  imShutdown arrives on one thread while
    // nothing else should still be in flight, but "should" is not a
    // guarantee, and releasing the table under the lock at least makes the
    // release atomic with respect to a concurrent ensureSuites().
    ImporterGlobals& g = globals();
    {
        std::lock_guard<std::mutex> guard(g.mutex);
        // Clear the flag FIRST so no thread can start using a table that is
        // about to be released; the release store pairs with the acquire
        // load in ensureSuites().
        g.suitesAcquired.store(false, std::memory_order_release);
        g.suites.release();
        g.basic = nullptr;
    }

    PluginLog::shutdown();
    return imNoErr;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Shared helpers declared in ImporterPlugin.h
// ---------------------------------------------------------------------------

ImporterGlobals& globals() noexcept { return globalsImpl(); }

void* importerModuleHandle() noexcept { return g_module; }

void ensureSuites(imStdParms* stdParms) noexcept {
    ImporterGlobals& g = globalsImpl();
    if (!stdParms) {
        return;
    }
    // Plain atomic store: it tracks the last call and takes part in no
    // invariant, so it needs no ordering with respect to the suite table.
    g.interfaceVersion.store(stdParms->imInterfaceVer, std::memory_order_relaxed);

    // Fast path.  An ACQUIRE load, so a thread that sees true also sees every
    // write the acquiring thread made to g.suites before its release store.
    if (g.suitesAcquired.load(std::memory_order_acquire)) {
        return;
    }
    if (!stdParms->piSuites || !stdParms->piSuites->utilFuncs || !stdParms->piSuites->utilFuncs->getSPBasicSuite) {
        return;
    }
    SPBasicSuite* basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
    if (!basic) {
        return;
    }

    // Slow path under the lock, then RE-TEST: between the fast-path load and
    // taking the mutex another thread may have finished the acquisition.
    // Without this second test both threads would run acquire(), whose first
    // act is release() - dropping the host's refcounts on suite pointers the
    // first thread has already published to an in-flight render.
    std::lock_guard<std::mutex> guard(g.mutex);
    if (g.suitesAcquired.load(std::memory_order_relaxed)) {
        return;
    }
    g.basic = basic;
    const int acquired = g.suites.acquire(basic);
    g.host = g.suites.hostVersion();
    // RELEASE store, and last: it publishes everything written above to any
    // thread that later reads the flag with acquire semantics.
    g.suitesAcquired.store(true, std::memory_order_release);
    PluginLog::info("suites: {} acquired - {}", acquired, g.suites.describe());
}

ImporterInstance* instanceFromHandle(void* privateData, PlugMemoryFuncsPtr memFuncs) noexcept {
    if (!privateData) {
        return nullptr;
    }
    // A PrMemoryHandle is a "pointer to a master pointer" (PrSDKTypes.h:196),
    // so the payload block is *handle and our single pointer lives at its
    // start.  lockHandle/unlockHandle pin the block around the access; they
    // return void, they only mark it non-relocatable.
    PrMemoryHandle handle = static_cast<PrMemoryHandle>(privateData);
    if (memFuncs && memFuncs->lockHandle) {
        memFuncs->lockHandle(handle);
    }
    ImporterInstance* instance = nullptr;
    if (*handle) {
        std::memcpy(&instance, *handle, sizeof(instance));
    }
    if (memFuncs && memFuncs->unlockHandle) {
        memFuncs->unlockHandle(handle);
    }
    return instance;
}

bool storeInstanceInHandle(void* privateData, PlugMemoryFuncsPtr memFuncs, ImporterInstance* instance) noexcept {
    if (!privateData) {
        return false;
    }
    PrMemoryHandle handle = static_cast<PrMemoryHandle>(privateData);
    if (memFuncs && memFuncs->lockHandle) {
        memFuncs->lockHandle(handle);
    }
    const bool ok = (*handle != nullptr);
    if (ok) {
        std::memcpy(*handle, &instance, sizeof(instance));
    }
    if (memFuncs && memFuncs->unlockHandle) {
        memFuncs->unlockHandle(handle);
    }
    return ok;
}

void* allocateHandleFor(PlugMemoryFuncsPtr memFuncs, ImporterInstance* instance) noexcept {
    if (!memFuncs || !memFuncs->newHandleClear) {
        return nullptr;
    }
    PrMemoryHandle handle = memFuncs->newHandleClear(kPrivateDataSize);
    if (!handle) {
        return nullptr;
    }
    if (!storeInstanceInHandle(handle, memFuncs, instance)) {
        if (memFuncs->disposeHandle) {
            memFuncs->disposeHandle(handle);
        }
        return nullptr;
    }
    return handle;
}

}  // namespace osv::premiere

// ---------------------------------------------------------------------------
//  DllMain
// ---------------------------------------------------------------------------
//
// Only two things happen here, both of which are safe under the loader lock:
// the module handle is stored, and the delay-load hook pointer is set (which
// is a single atomic store - see plugins/common/DelayLoad.h).  No CUDA, no
// FFmpeg, no logging, no allocation.
//
// Windows only.  A macOS bundle needs neither: its FFmpeg sits inside the
// bundle under install names no other image uses (cmake/OsvPremiereSdk.cmake),
// so the dynamic loader cannot bind it to a host's copy in the first place.

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        osv::premiere::g_module = static_cast<HINSTANCE>(module);
        // Resolve our own FFmpeg / OpenCL DLLs from the plug-in's folder
        // rather than from Premiere's application directory, which ships its
        // own (older) FFmpeg.
        osv::premiere::delayload::installHook();
        // Thread notifications cost a callback per thread and this module
        // needs none; Premiere creates a lot of threads.
        ::DisableThreadLibraryCalls(module);
        break;
    case DLL_PROCESS_DETACH:
        // Deliberately nothing.  imShutdown has already torn the renderers
        // down; if it did not, the process is exiting and touching CUDA here
        // would be worse than leaking.
        break;
    default:
        break;
    }
    return TRUE;
}
#endif

// ---------------------------------------------------------------------------
//  xImportEntry
// ---------------------------------------------------------------------------

extern "C" PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms, void* param1,
                                                void* param2) {
    using namespace osv::premiere;

    // Every path out of here is a plain integer: no exception may cross back
    // into the host's C stack.
    try {
        ensureSuites(stdParms);

        switch (selector) {
        // ---- lifetime -----------------------------------------------------
        case imInit:
            return doInit(stdParms, static_cast<imImportInfoRec*>(param1));
        case imShutdown:
            return doShutdown();
        case imGetIndFormat:
            // param1 is the index, passed by value in the pointer.
            return doGetIndFormat(static_cast<csSDK_int32>(reinterpret_cast<std::intptr_t>(param1)),
                                  static_cast<imIndFormatRec*>(param2));

        // ---- capability declarations --------------------------------------
        case imGetSupports7:
            return malSupports7;
        case imGetSupports8:
            return malSupports8;
        case imGetSupportsPerInstancePrefs:
            return malSupportsPerInstancePrefs;

        // ---- file lifetime -------------------------------------------------
        case imOpenFile8:
            return doOpenFile8(stdParms, static_cast<imFileRef*>(param1), static_cast<imFileOpenRec8*>(param2));
        case imQuietFile:
            return doQuietFile(stdParms, static_cast<imFileRef*>(param1), param2);
        case imCloseFile:
            return doCloseFile(stdParms, static_cast<imFileRef*>(param1), param2);

        // ---- information ---------------------------------------------------
        case imGetInfo8:
            return handleGetInfo8(stdParms, static_cast<imFileAccessRec8*>(param1),
                                  static_cast<imFileInfoRec8*>(param2));
        case imGetInfo9:
            return handleGetInfo9(stdParms, static_cast<imFileAccessRec8*>(param1),
                                  static_cast<imFileInfoRec9*>(param2));
        case imAnalysis:
            return handleAnalysis(stdParms, static_cast<imAnalysisRec*>(param2));
        case imGetTimeInfo8:
            return handleGetTimeInfo8(stdParms, static_cast<imTimeInfoRec8*>(param2));
        case imGetFileAttributes:
            return handleGetFileAttributes(stdParms, static_cast<imFileAttributesRec*>(param1));

        // ---- format negotiation ---------------------------------------------
        case imGetIndPixelFormat:
            return handleGetIndPixelFormat(stdParms, static_cast<csSDK_int32>(reinterpret_cast<std::intptr_t>(param1)),
                                           static_cast<imIndPixelFormatRec*>(param2));
        case imGetPreferredFrameSize:
            return handleGetPreferredFrameSize(stdParms, static_cast<imPreferredFrameSizeRec*>(param1));
        case imSelectClipFrameDescriptor:
            return handleSelectClipFrameDescriptor(stdParms, static_cast<imClipFrameDescriptorRec*>(param2), false);
        case imSelectClipFrameDescriptor2:
            // 23.2 and later only; on an older host the selector never
            // arrives, but the version gate makes that explicit.
            if (!stdParms || stdParms->imInterfaceVer < IMPORTMOD_VERSION_24) {
                PluginLog::oncef("sel-descriptor2-old-host", PluginLog::Level::Debug,
                                 "imSelectClipFrameDescriptor2 from a host reporting interface version {}",
                                 stdParms ? stdParms->imInterfaceVer : 0);
                return imUnsupported;
            }
            return handleSelectClipFrameDescriptor(stdParms, static_cast<imClipFrameDescriptorRec*>(param2), true);
        case imGetIndColorSpace:
            return handleGetIndColorSpace(stdParms, static_cast<csSDK_int32>(reinterpret_cast<std::intptr_t>(param1)),
                                          static_cast<imIndColorSpaceRec*>(param2));

        // ---- frames ----------------------------------------------------------
        case imGetSourceVideo:
            return handleGetSourceVideo(stdParms, static_cast<imSourceVideoRec*>(param2));

        // ---- audio -----------------------------------------------------------
        case imImportAudio7:
            return handleImportAudio7(stdParms, static_cast<imImportAudioRec7*>(param2));
        case imResetSequentialAudio:
            return handleResetSequentialAudio(stdParms, static_cast<imImportAudioRec7*>(param2));
        case imGetSequentialAudio:
            return handleGetSequentialAudio(stdParms, static_cast<imImportAudioRec7*>(param2));
        case imGetAudioChannelLayout:
            return handleGetAudioChannelLayout(stdParms, static_cast<imGetAudioChannelLayoutRec*>(param2));

        // ---- prefs -----------------------------------------------------------
        case imGetPrefs8:
            return handleGetPrefs8(stdParms, static_cast<imFileAccessRec8*>(param1),
                                   static_cast<imGetPrefsRec*>(param2));
        case imGetInstancePrefs:
            return handleGetInstancePrefs(stdParms, static_cast<imFileAccessRec8*>(param1),
                                          static_cast<imGetInstancePrefsRec*>(param2));
        case imPerformSourceSettingsCommand:
            // The private channel to OpenOSVSourceSettings.aex.  param1 is an
            // imFileAccessRec8*, param2 an imSourceSettingsCommandRec*
            // (PrSDKImport.h:1729-1734).
            return handlePerformSourceSettingsCommand(stdParms, static_cast<imFileAccessRec8*>(param1),
                                                      static_cast<imSourceSettingsCommandRec*>(param2));

        default:
            break;
        }

        // Everything else, including imGetColorSpaceFromOpaqueData (23.3,
        // whose parameter struct is not in the public headers) and the
        // AE-only selectors.  imUnsupported is a non-error return.
        const char* name = selectorName(selector);
        PluginLog::oncef("unsupported-" + std::to_string(selector), PluginLog::Level::Debug,
                         "selector {} ({}) is not supported", selector, name ? name : "unnamed");
        return imUnsupported;
    } catch (const std::bad_alloc&) {
        PluginLog::error("selector {}: out of memory", selector);
        return imMemErr;
    } catch (const std::exception& e) {
        PluginLog::error("selector {}: unhandled exception: {}", selector, e.what());
        return imOtherErr;
    } catch (...) {
        PluginLog::error("selector {}: unhandled non-standard exception", selector);
        return imOtherErr;
    }
}
