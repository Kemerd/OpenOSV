// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Engine.cpp - the exported direct-GPU entry points (OsvEngineAbi.h).
//
// One call from the effect, per frame it renders:
//
//   OsvEngine_AcquireFrame   find (or open) the engine's instance of the
//                            file, push the effect's CUDA context, decode the
//                            frame on NVDEC into it (ImporterInstance::
//                            directFrame - VRAM cache, GOP-aware), run or
//                            fetch the stitch analyses on the GPU, upload the
//                            seam table / warp grid, hand back device planes
//                            plus the stitch block and a lease.
//   OsvEngine_ReleaseFrame   give the frame slot back once the effect's
//                            stream has finished reading it.
//
// Every export is noexcept and catches everything: these are called from
// Premiere's render threads through a C function pointer, and an exception
// crossing that boundary would take the host down.
//
// LIFETIME.  The registry is created on first use and deliberately never
// destroyed by a static destructor: at process exit the CUDA driver and
// Premiere's context may already be gone, and tearing NVDEC decoders down
// then is exactly the kind of shutdown-order crash that is impossible to
// debug in the field.  imShutdown calls engineShutdown() instead, while
// everything is still alive.

#include "Engine.h"

#include "ImporterInstance.h"
#include "OsvEngineAbi.h"
#include "PluginLog.h"

#include <cuda.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace osv::premiere {

namespace {

// ===========================================================================
//  Small helpers
// ===========================================================================

/// Premiere's tick rate (PrSDKTypes.h), the unit of OsvEngineFrameRequest::
/// mediaTicks.
constexpr std::int64_t kTicksPerSecond = 254016000000LL;

/// Longest path the engine accepts, in UTF-16 units.  Far beyond MAX_PATH
/// (the long-path limit is 32767); anything longer is a garbage pointer
/// walking memory, not a file name.
constexpr std::size_t kMaxPathChars = 32767;

/// Tag that identifies a live lease, so a stale or foreign pointer handed to
/// OsvEngine_ReleaseFrame is refused instead of freed.
constexpr std::uint32_t kLeaseMagic = 0x4F53454Cu;  // 'OSEL'

/// Copy a message into the caller's UTF-8 buffer, truncated and terminated.
void writeError(char* buffer, std::int32_t capacity, const std::string& message) noexcept {
    if (!buffer || capacity <= 0) {
        return;
    }
    const std::size_t n = std::min(message.size(), static_cast<std::size_t>(capacity) - 1u);
    std::memcpy(buffer, message.data(), n);
    buffer[n] = '\0';
}

/// The registry key of a file: its lexically normalised path, lower-cased.
/// Windows paths are case-insensitive, and Premiere and the effect may spell
/// the same file differently (the media node's instance string versus the
/// path imOpenFile8 was given); normalising both sides makes them meet.
[[nodiscard]] std::wstring keyFor(const std::filesystem::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    for (wchar_t& c : key) {
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    return key;
}

/// Frame index for a media time, rounded to nearest and clamped - the same
/// rule the importer's own imGetSourceVideo applies, so the direct path and
/// the equirect path always pick the same frame for the same time.
[[nodiscard]] std::uint32_t frameIndexForTicks(const ImporterInstance& clip, std::int64_t ticks) noexcept {
    const std::uint32_t num = clip.rateNumerator();
    const std::uint32_t den = clip.rateDenominator();
    const std::uint32_t count = clip.frameCount();
    if (num == 0 || den == 0 || count == 0 || ticks <= 0) {
        return 0;
    }
    const std::int64_t perFrame = (kTicksPerSecond * static_cast<std::int64_t>(den)) / static_cast<std::int64_t>(num);
    if (perFrame <= 0) {
        return 0;
    }
    const std::int64_t index = (ticks + perFrame / 2) / perFrame;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(index, 0, static_cast<std::int64_t>(count) - 1));
}

/// RAII push / pop of a CUDA context on the calling thread.
class ContextScope {
public:
    explicit ContextScope(CUcontext context) noexcept {
        if (context) {
            m_pushed = (cuCtxPushCurrent(context) == CUDA_SUCCESS);
        }
    }
    ~ContextScope() {
        if (m_pushed) {
            CUcontext popped = nullptr;
            (void)cuCtxPopCurrent(&popped);
        }
    }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return m_pushed; }

private:
    bool m_pushed = false;
};

/// A CUresult as text.
[[nodiscard]] std::string cudaText(const char* what, CUresult r) {
    const char* name = nullptr;
    (void)cuGetErrorName(r, &name);
    return std::string(what) + ": " + (name ? name : "CUDA_ERROR_UNKNOWN");
}

// ===========================================================================
//  The registry
// ===========================================================================

struct Registry {
    std::mutex mutex;
    /// The engine's own instance of each file it has been asked for.
    std::unordered_map<std::wstring, std::shared_ptr<ImporterInstance>> clips;
    /// The latest Source Settings Premiere's instances published per file.
    std::unordered_map<std::wstring, PrefsBlob> prefs;
};

/// Created on first use, never destroyed (see the file header).
[[nodiscard]] Registry& registry() {
    static Registry* instance = new Registry();
    return *instance;
}

/// The engine's instance of `path`, opened on first use.  Returns null with
/// `error` filled when the file cannot be opened as a dual-fisheye OSV.
[[nodiscard]] std::shared_ptr<ImporterInstance> clipFor(const std::filesystem::path& path, std::string& error) {
    const std::wstring key = keyFor(path);
    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        const auto it = r.clips.find(key);
        if (it != r.clips.end() && it->second) {
            return it->second;
        }
    }
    // Opened OUTSIDE the registry lock: parsing a file takes milliseconds and
    // other clips' renders must not wait behind it.  Two threads racing to
    // open the same file both succeed and the second insert loses; the loser
    // is simply dropped.
    auto clip = std::make_shared<ImporterInstance>(path);
    clip->setEngineOwned(true);
    const Status opened = clip->open();
    if (!opened.ok()) {
        error = "cannot open '" + path.filename().string() + "': " + opened.error().message;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(r.mutex);
    auto [it, inserted] = r.clips.emplace(key, clip);
    if (inserted) {
        PluginLog::info("direct: engine opened '{}'", path.filename().string());
    }
    return it->second;
}

/// The Source Settings published for `path`, if any.
[[nodiscard]] bool publishedPrefs(const std::filesystem::path& path, PrefsBlob& out) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.prefs.find(keyFor(path));
    if (it == r.prefs.end()) {
        return false;
    }
    out = it->second;
    return true;
}

// ===========================================================================
//  The lease
// ===========================================================================

/// Everything one acquired frame holds, freed by OsvEngine_ReleaseFrame.
struct EngineLease {
    std::uint32_t magic = kLeaseMagic;
    std::shared_ptr<ImporterInstance> clip;  ///< Keeps the decoder's owner alive.
    video::GpuFrameLease frame;              ///< Pins the decoded pair in VRAM.
    CUcontext context = nullptr;
    CUdeviceptr seam = 0;                    ///< Device copy of the seam table.
    CUdeviceptr warp = 0;                    ///< Device copy of the warp grid.
    CUdeviceptr blendSeam = 0;               ///< [WP-SEAM] Device copy of the carved blend-seam table.

    /// Free the device tables.  The caller has pushed `context`.
    void freeTables() noexcept {
        if (seam) {
            (void)cuMemFree(seam);
            seam = 0;
        }
        if (warp) {
            (void)cuMemFree(warp);
            warp = 0;
        }
        // [WP-SEAM]
        if (blendSeam) {
            (void)cuMemFree(blendSeam);
            blendSeam = 0;
        }
    }
};

/// Upload a host float table to a fresh device allocation (the context is
/// current).  An empty table is "none" and yields 0.
[[nodiscard]] Status uploadTable(const std::vector<float>& host, CUdeviceptr& out) {
    out = 0;
    if (host.empty()) {
        return okStatus();
    }
    const std::size_t bytes = host.size() * sizeof(float);
    CUresult r = cuMemAlloc(&out, bytes);
    if (r != CUDA_SUCCESS) {
        out = 0;
        return failStatus(ErrorCode::Gpu, cudaText("cuMemAlloc", r));
    }
    // Synchronous on purpose: a few KB, and it guarantees the table is in
    // place before the effect's kernel - on whatever stream - reads it.
    r = cuMemcpyHtoD(out, host.data(), bytes);
    if (r != CUDA_SUCCESS) {
        (void)cuMemFree(out);
        out = 0;
        return failStatus(ErrorCode::Gpu, cudaText("cuMemcpyHtoD", r));
    }
    return okStatus();
}

/// The live-lease count, for the log and the tests' leak checks.
std::atomic<long> g_liveLeases{0};

}  // namespace

// ===========================================================================
//  Importer-internal hooks
// ===========================================================================

void enginePublishPrefs(const std::filesystem::path& path, const PrefsBlob& prefs) noexcept {
    try {
        Registry& r = registry();
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            PrefsBlob& slot = r.prefs[keyFor(path)];
            changed = !(slot == prefs);
            slot = prefs;
        }
        // Only real changes are logged: every Premiere instance of the clip
        // publishes the same blob when it opens, and those repeats say nothing.
        if (changed) {
            PluginLog::info("direct: Source Settings published for '{}' - colour {}, fit {}, exposure {:+.2f}",
                            path.filename().string(), static_cast<int>(prefs.colorOutput),
                            static_cast<int>(prefs.dlogmFit), static_cast<double>(prefs.exposureStops));
        }
    } catch (...) {
        // A failed publish only means the engine renders this clip with the
        // settings it had; never worth failing the importer call over.
    }
}

void engineShutdown() noexcept {
    try {
        Registry& r = registry();
        std::unordered_map<std::wstring, std::shared_ptr<ImporterInstance>> clips;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            clips.swap(r.clips);
        }
        if (!clips.empty()) {
            PluginLog::info("direct: engine releasing {} clip(s), {} frame lease(s) still out", clips.size(),
                            g_liveLeases.load());
        }
        // Destroyed here, outside the lock, while the driver is alive.  An
        // instance a live lease still references is freed with that lease.
        clips.clear();
    } catch (...) {
    }
}

}  // namespace osv::premiere

// ===========================================================================
//  The exports
// ===========================================================================

using osv::premiere::EngineLease;
using osv::premiere::PluginLog;

extern "C" __declspec(dllexport) std::uint32_t OsvEngine_AbiVersion(void) {
    return OSV_ENGINE_ABI_VERSION;
}

extern "C" __declspec(dllexport) std::int32_t OsvEngine_AcquireFrame(const OsvEngineFrameRequest* request,
                                                                      OsvEngineFrame* out, char* error,
                                                                      std::int32_t errorCapacity) {
    using namespace osv;
    using namespace osv::premiere;
    try {
        // ---- the caller's structures ------------------------------------------
        if (!request || !out) {
            writeError(error, errorCapacity, "null request or frame");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        if (request->structSize < sizeof(OsvEngineFrameRequest) || out->structSize < sizeof(OsvEngineFrame)) {
            writeError(error, errorCapacity, "structure size mismatch (built from a different ABI)");
            return OSV_ENGINE_ERR_VERSION;
        }
        // Everything past structSize is ours to fill; start from zero so a
        // failure never leaves a stale pointer for the caller to trust.
        const std::uint32_t outSize = out->structSize;
        std::memset(out, 0, sizeof(OsvEngineFrame));
        out->structSize = outSize;
        out->paramsSize = static_cast<std::uint32_t>(sizeof(OsvRenderParams));

        if (!request->path || !request->cuContext) {
            writeError(error, errorCapacity, "null path or CUDA context");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        const std::size_t pathLen = wcsnlen(request->path, kMaxPathChars + 1);
        if (pathLen == 0 || pathLen > kMaxPathChars) {
            writeError(error, errorCapacity, "empty or unterminated path");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        if (request->mediaTicks < 0) {
            writeError(error, errorCapacity, "negative media time");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        if (request->purpose != OSV_ENGINE_PURPOSE_EXACT && request->purpose != OSV_ENGINE_PURPOSE_INTERACTIVE) {
            writeError(error, errorCapacity, "unknown purpose");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        if (request->outputTransfer != OSV_ENGINE_TRANSFER_FROM_CLIP &&
            (request->outputTransfer < 0 || request->outputTransfer > OSV_TRANSFER_PASSTHROUGH)) {
            writeError(error, errorCapacity, "unknown output transfer");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        const std::filesystem::path path(std::wstring(request->path, pathLen));

        // ---- the clip -------------------------------------------------------------
        std::string openError;
        std::shared_ptr<ImporterInstance> clip = clipFor(path, openError);
        if (!clip) {
            writeError(error, errorCapacity, openError);
            return OSV_ENGINE_ERR_SOURCE;
        }

        // ---- the caller's context, for everything below ----------------------------
        const CUcontext context = static_cast<CUcontext>(request->cuContext);
        ContextScope scope(context);
        if (!scope.ok()) {
            writeError(error, errorCapacity, "cannot make the CUDA context current");
            return OSV_ENGINE_ERR_GPU;
        }

        auto lease = std::make_unique<EngineLease>();
        lease->clip = clip;
        lease->context = context;
        std::uint32_t index = 0;
        bool exact = true;
        render::RenderJob job;
        {
            std::lock_guard<std::mutex> lock(clip->lock());

            // The user's Source Settings for this file, as Premiere's own
            // instance last saw them (a no-op when nothing changed).
            PrefsBlob prefs;
            if (publishedPrefs(path, prefs)) {
                const PrefsBlob before = clip->prefsLocked();
                clip->applyPrefsLocked(&prefs, PrefsBlob::kSize);
                if (!(before == clip->prefsLocked())) {
                    PluginLog::info("direct: engine now renders '{}' with exposure {:+.2f}, fit {} (was {:+.2f}, "
                                    "fit {})",
                                    path.filename().string(), static_cast<double>(prefs.exposureStops),
                                    static_cast<int>(prefs.dlogmFit), static_cast<double>(before.exposureStops),
                                    static_cast<int>(before.dlogmFit));
                }
            }

            index = frameIndexForTicks(*clip, request->mediaTicks);
            const RenderPurpose purpose =
                request->purpose == OSV_ENGINE_PURPOSE_EXACT ? RenderPurpose::Exact : RenderPurpose::Interactive;
            auto frame = clip->directFrame(index, context, purpose, request->outputTransfer);
            if (!frame.ok()) {
                writeError(error, errorCapacity, frame.error().message);
                return frame.error().code == ErrorCode::Decoder ? OSV_ENGINE_ERR_DECODE : OSV_ENGINE_ERR_INTERNAL;
            }
            lease->frame = std::move(frame.value().lease);
            job = std::move(frame.value().job);
            exact = frame.value().exact;
        }

        // ---- the two tables, in the caller's context ----------------------------------
        Status uploaded = uploadTable(job.params.seamShiftEnabled ? job.seamShiftDeg : std::vector<float>{},
                                      lease->seam);
        if (uploaded.ok()) {
            uploaded = uploadTable(job.params.warpEnabled ? job.warpGrid : std::vector<float>{}, lease->warp);
        }
        // [WP-SEAM] the carved blend-seam table, a few KB like the others.
        if (uploaded.ok()) {
            uploaded = uploadTable(job.params.blendSeamEnabled ? job.blendSeam : std::vector<float>{},
                                   lease->blendSeam);
        }
        if (!uploaded.ok()) {
            lease->freeTables();
            writeError(error, errorCapacity, uploaded.error().message);
            return OSV_ENGINE_ERR_GPU;
        }

        // ---- hand it over -----------------------------------------------------------------
        out->stitch = job.params;
        out->planes[0] = job.planes[0];
        out->planes[1] = job.planes[1];
        out->seamDevice = reinterpret_cast<const float*>(static_cast<std::uintptr_t>(lease->seam));
        out->warpDevice = reinterpret_cast<const float*>(static_cast<std::uintptr_t>(lease->warp));
        // [WP-SEAM]
        out->blendSeamDevice = reinterpret_cast<const float*>(static_cast<std::uintptr_t>(lease->blendSeam));
        // The stitch block is an equirect block, whose Rout is exactly the
        // frame's body-from-world stabilisation.
        std::memcpy(out->bodyFromWorld, job.params.Rout, sizeof(out->bodyFromWorld));
        out->frameIndex = index;
        out->exact = exact ? 1 : 0;
        out->lease = lease.release();
        g_liveLeases.fetch_add(1);

        PluginLog::oncef("direct/first-frame", PluginLog::Level::Info,
                         "direct: first frame served - '{}' frame {} (media {} ticks), transfer {}, seam {}, warp {}, "
                         "blend seam {}",
                         path.filename().string(), index, static_cast<long long>(request->mediaTicks),
                         out->stitch.color.transfer, out->seamDevice ? "yes" : "no", out->warpDevice ? "yes" : "no",
                         out->blendSeamDevice ? "yes" : "no");
        return OSV_ENGINE_OK;
    } catch (const std::exception& e) {
        writeError(error, errorCapacity, std::string("internal error: ") + e.what());
        return OSV_ENGINE_ERR_INTERNAL;
    } catch (...) {
        writeError(error, errorCapacity, "internal error");
        return OSV_ENGINE_ERR_INTERNAL;
    }
}

extern "C" __declspec(dllexport) void OsvEngine_ReleaseFrame(void* leasePtr, void* cuStream) {
    try {
        if (!leasePtr) {
            return;
        }
        auto* lease = static_cast<EngineLease*>(leasePtr);
        if (lease->magic != osv::premiere::kLeaseMagic) {
            PluginLog::oncef("direct/bad-lease", PluginLog::Level::Error,
                             "direct: OsvEngine_ReleaseFrame was handed something that is not a live lease");
            return;
        }
        {
            osv::premiere::ContextScope scope(lease->context);
            // The frame slot: recycled only once the caller's stream has
            // passed this point, so the caller never synchronises for us.
            if (cuStream) {
                (void)lease->frame.releaseAfter(cuStream);
                // The tables are freed immediately below; wait for the reads
                // queued on that stream first (instant when the caller has
                // already synchronised, as the effect does).
                (void)cuStreamSynchronize(static_cast<CUstream>(cuStream));
            } else {
                lease->frame.release();
            }
            lease->freeTables();
        }
        lease->magic = 0;
        delete lease;
        osv::premiere::g_liveLeases.fetch_sub(1);
    } catch (...) {
        // Never across the C boundary.
    }
}
