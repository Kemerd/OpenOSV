// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectPath.cpp - the effect's side of the direct GPU pipeline.
// See DirectPath.h for what and why.

#include "DirectPath.h"

#include "PluginLog.h"

// Adobe headers are #pragma pack(push, 1); nothing of ours is declared while
// they are open.
#include "PrSDKColorManagementSuite.h"
#include "PrSDKVideoSegmentProperties.h"
#include "SPBasic.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::reframe::direct {

using osv::premiere::PluginLog;

namespace {

// ===========================================================================
//  The engine
// ===========================================================================

/// True when the kill switch OSV_DISABLE_DIRECT=1 is set in the environment.
[[nodiscard]] bool directDisabledByEnvironment() noexcept {
    wchar_t value[8] = {};
    const DWORD n = GetEnvironmentVariableW(L"OSV_DISABLE_DIRECT", value, static_cast<DWORD>(std::size(value)));
    return n > 0 && n < std::size(value) && value[0] == L'1';
}

/// Resolve the importer's exports.  The module is pinned for the life of the
/// process (GET_MODULE_HANDLE_EX_FLAG_PIN): the effect keeps function
/// pointers into it, and Premiere never unloads an importer mid-session, so
/// pinning costs nothing and rules out a dangling call for good.
[[nodiscard]] EngineApi resolveEngine() noexcept {
    EngineApi api;
    if (directDisabledByEnvironment()) {
        PluginLog::info("reframe/direct: disabled by OSV_DISABLE_DIRECT=1; using the equirect path");
        return api;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, OSV_ENGINE_MODULE_NAME, &module) || !module) {
        PluginLog::info("reframe/direct: the OpenOSV importer is not loaded; using the equirect path");
        return api;
    }
    EngineApi found;
    found.version = reinterpret_cast<OsvEngineAbiVersionFn>(GetProcAddress(module, OSV_ENGINE_SYM_ABI_VERSION));
    found.acquire = reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    found.release = reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    if (!found.ok()) {
        PluginLog::info("reframe/direct: the importer has no engine exports (an older build); using the equirect "
                        "path");
        return api;
    }
    const std::uint32_t version = found.version();
    if (version != OSV_ENGINE_ABI_VERSION) {
        PluginLog::warn("reframe/direct: the importer's engine speaks ABI {} and this effect {}; using the "
                        "equirect path until both are from the same build",
                        version, static_cast<std::uint32_t>(OSV_ENGINE_ABI_VERSION));
        return api;
    }
    PluginLog::info("reframe/direct: the importer's engine is available (ABI {})", version);
    return found;
}

// ===========================================================================
//  Segment-graph property reading
// ===========================================================================

/// IterateNodeProperties hands every property to a C callback whose
/// plug-in object is a 32-bit integer - too narrow for a pointer - so the
/// lookup state is thread-local; the iteration is synchronous.
struct PropertyLookup {
    const char* key = nullptr;
    std::string value;
    bool found = false;
};
thread_local PropertyLookup* t_lookup = nullptr;

prSuiteError lookupProperty(csSDK_int32, const char* key, const prUTF8Char* value) {
    if (t_lookup && key && t_lookup->key && std::strcmp(key, t_lookup->key) == 0) {
        t_lookup->value = value ? reinterpret_cast<const char*>(value) : "";
        t_lookup->found = true;
    }
    return suiteError_NoError;
}

/// Read one property of a node as UTF-8.
[[nodiscard]] bool nodeProperty(const PrSDKVideoSegmentSuite& s, csSDK_int32 node, const char* key,
                                std::string& out) {
    if (!s.IterateNodeProperties) {
        return false;
    }
    PropertyLookup lookup;
    lookup.key = key;
    t_lookup = &lookup;
    const prSuiteError err = s.IterateNodeProperties(node, &lookupProperty, 0);
    t_lookup = nullptr;
    if (err != suiteError_NoError || !lookup.found) {
        return false;
    }
    out = std::move(lookup.value);
    return true;
}

/// UTF-8 to UTF-16 for a path.
[[nodiscard]] std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

/// Case-insensitive "ends with" for an ASCII extension.
[[nodiscard]] bool hasExtension(const std::wstring& path, const wchar_t* ext) {
    const std::size_t n = std::wcslen(ext);
    if (path.size() < n) {
        return false;
    }
    return _wcsicmp(path.c_str() + (path.size() - n), ext) == 0;
}

/// One-line reason for a CUresult.
[[nodiscard]] std::string cudaText(const char* what, CUresult r) {
    const char* name = nullptr;
    (void)cuGetErrorName(r, &name);
    return std::string(what) + ": " + (name ? name : "CUDA_ERROR_UNKNOWN");
}

}  // namespace

// ===========================================================================
//  Public entry points
// ===========================================================================

const EngineApi& engine() noexcept {
    static EngineApi api;
    static std::once_flag once;
    try {
        std::call_once(once, [] { api = resolveEngine(); });
    } catch (...) {
    }
    return api;
}

SourceBinding resolveSource(const PrSDKVideoSegmentSuite* segment, int segmentVersion, SPBasicSuite* basic,
                            csSDK_int32 effectNode) noexcept {
    SourceBinding b;
    try {
        (void)basic;
        if (!segment || segmentVersion < kPrSDKVideoSegmentSuiteVersion6 || !segment->AcquireOperatorOwnerNodeID ||
            !segment->AcquireInputNodeID || !segment->ReleaseVideoNodeID || !segment->GetNodeInfo ||
            !segment->TransformNodeTime) {
            b.reason = "the Video Segment Suite cannot walk to the source";
            return b;
        }
        csSDK_int32 owner = 0;
        if (segment->AcquireOperatorOwnerNodeID(effectNode, &owner) != suiteError_NoError || owner == 0) {
            b.reason = "no owning clip node";
            return b;
        }
        // The clip node's first input is its media (the probe of the first
        // real session: owner 'RenderableNodeClipImpl' -> input
        // 'RenderableNodeMediaImpl').  Anything else - a nested sequence, a
        // multicam, an adjustment layer - is not a file we can open.
        PrTime offset = 0;
        csSDK_int32 media = 0;
        if (segment->AcquireInputNodeID(owner, 0, &offset, &media) != suiteError_NoError || media == 0) {
            segment->ReleaseVideoNodeID(owner);
            b.reason = "the clip node has no input";
            return b;
        }
        char type[kMaxNodeTypeStringSize] = {};
        prPluginID hash{};
        csSDK_int32 flags = 0;
        const bool isMedia = segment->GetNodeInfo(media, type, &hash, &flags) == suiteError_NoError &&
                             std::strcmp(type, kVideoSegment_NodeType_Media) == 0;
        std::string pathUtf8;
        const bool gotPath = isMedia && nodeProperty(*segment, media, kVideoSegmentProperty_Media_InstanceString,
                                                     pathUtf8);
        segment->ReleaseVideoNodeID(media);
        if (!isMedia) {
            segment->ReleaseVideoNodeID(owner);
            b.reason = std::string("the source is a '") + type + "' node, not media";
            return b;
        }
        if (!gotPath) {
            segment->ReleaseVideoNodeID(owner);
            b.reason = "the media node has no instance string";
            return b;
        }
        b.path = widen(pathUtf8);
        // The engine decodes dual-fisheye .OSV clips (the .LRF proxy is a
        // single side-by-side 8-bit stream the NVDEC decoder does not take).
        if (b.path.empty() || !hasExtension(b.path, L".osv")) {
            segment->ReleaseVideoNodeID(owner);
            b.path.clear();
            b.reason = "the source is not an .OSV file";
            return b;
        }
        b.ownerNode = owner;
        b.ok = true;
        return b;
    } catch (...) {
        b = SourceBinding{};
        b.reason = "internal error";
        return b;
    }
}

void releaseSource(const PrSDKVideoSegmentSuite* segment, SourceBinding& binding) noexcept {
    if (binding.ownerNode != 0 && segment && segment->ReleaseVideoNodeID) {
        segment->ReleaseVideoNodeID(binding.ownerNode);
    }
    binding.ownerNode = 0;
    binding.ok = false;
}

int workingTransfer(const PrSDKSequenceInfoSuite* sequence, int sequenceVersion, SPBasicSuite* basic,
                    PrTimelineID timeline, std::string& reason) noexcept {
    try {
        // GetWorkingColorSpace sits at the end of the v9 table; an older
        // host's shorter table must not be read past its end.
        if (!sequence || sequenceVersion < 9 || !sequence->GetWorkingColorSpace || !basic || !basic->AcquireSuite) {
            reason = "the host cannot report its working colour space";
            return -1;
        }
        PrSDKColorSpaceID id{};
        if (sequence->GetWorkingColorSpace(timeline, &id) != suiteError_NoError) {
            reason = "GetWorkingColorSpace failed";
            return -1;
        }
        const void* raw = nullptr;
        if (basic->AcquireSuite(kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion, &raw) != kSPNoError ||
            !raw) {
            reason = "no Color Management Suite";
            return -1;
        }
        const auto* cm = static_cast<const PrSDKColorManagementSuite*>(raw);
        prSEIColorCodesRec sei;
        const bool gotCodes =
            cm->GetSEIColorCodesForColorSpace && cm->GetSEIColorCodesForColorSpace(&id, &sei) == suiteError_NoError;
        basic->ReleaseSuite(kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion);
        if (!gotCodes) {
            reason = "the working colour space has no SEI description";
            return -1;
        }
        // The spaces the colour pipeline can produce exactly, by the H.273
        // codes: PQ and HLG on BT.2020 primaries, BT.709 on BT.709.  (First
        // real session: primaries 9, transfer 16 - Rec.2100 PQ.)
        const int primaries = sei.colorPrimariesCode;
        const int transfer = sei.transferCharacteristicCode;
        reason = "working space: primaries " + std::to_string(primaries) + ", transfer " + std::to_string(transfer);
        if (primaries == 9 && transfer == 16) {
            return OSV_TRANSFER_PQ;
        }
        if (primaries == 9 && transfer == 18) {
            return OSV_TRANSFER_HLG;
        }
        if (primaries == 1 && (transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15)) {
            return OSV_TRANSFER_REC709;
        }
        reason += " - not one the direct path produces";
        return -1;
    } catch (...) {
        reason = "internal error";
        return -1;
    }
}

bool renderDirect(const DirectRequest& request, std::string& reason) noexcept {
    try {
        const EngineApi& api = engine();
        if (!api.ok()) {
            reason = "no engine";
            return false;
        }
        if (!request.source || !request.source->ok || !request.segment || !request.segment->TransformNodeTime ||
            !request.context || !request.kernel || request.transfer < 0) {
            reason = "incomplete request";
            return false;
        }

        // ---- clip time -> media time --------------------------------------------
        // The owning clip node's transform accounts for the in point, speed,
        // reverse and time remapping (PrSDKVideoSegmentSuite.h).
        PrTime media = 0;
        if (request.segment->TransformNodeTime(request.source->ownerNode, request.clipTime, &media) !=
                suiteError_NoError ||
            media < 0) {
            reason = "TransformNodeTime failed";
            return false;
        }

        // ---- the frame from the engine --------------------------------------------
        // Always EXACT: with the analyses on the GPU a fresh bucket measures in
        // ~2-3 ms, so there is no reason to show a stand-in, and an exact
        // frame is identical whatever was rendered before it.
        OsvEngineFrameRequest req{};
        req.structSize = sizeof(OsvEngineFrameRequest);
        req.path = request.source->path.c_str();
        req.mediaTicks = static_cast<std::int64_t>(media);
        req.purpose = OSV_ENGINE_PURPOSE_EXACT;
        req.outputTransfer = request.transfer;
        req.cuContext = request.context;
        req.cuStream = CU_STREAM_LEGACY;
        OsvEngineFrame frame{};
        frame.structSize = sizeof(OsvEngineFrame);
        char error[512] = {};
        const std::int32_t rc = api.acquire(&req, &frame, error, static_cast<std::int32_t>(sizeof(error)));
        if (rc != OSV_ENGINE_OK) {
            reason = std::string("engine: ") + error;
            return false;
        }
        // From here the lease must be released on every path.
        struct LeaseGuard {
            const EngineApi& api;
            void* lease;
            ~LeaseGuard() {
                if (lease) {
                    api.release(lease, CU_STREAM_LEGACY);
                }
            }
        } guard{api, frame.lease};

        if (frame.paramsSize != sizeof(OsvRenderParams)) {
            reason = "the engine's parameter block has a different layout";
            return false;
        }

        // ---- the view ----------------------------------------------------------------
        StitchState stitch;
        stitch.equirect = frame.stitch;
        stitch.seamTable = frame.seamDevice;
        stitch.warpGrid = frame.warpDevice;
        stitch.blendSeam = frame.blendSeamDevice;  // [WP-SEAM] pass-through of the carved seam table
        const DirectSetup setup = buildDirectParams(request.settings, stitch, request.output.width,
                                                    request.output.height, request.sequenceSize);
        if (!setup.valid) {
            reason = std::string("setup: ") + directRejectName(setup.reject);
            return false;
        }

        // ---- launch and wait (same ordering contract as the equirect kernel) ----------
        const DirectLaunchResult launched =
            launchDirect(request.kernel, CU_STREAM_LEGACY, setup, frame.planes, request.output);
        if (!launched.ok()) {
            reason = std::string("launch: ") + directLaunchRejectName(launched.reject) + " (" +
                     cudaText("cuLaunchKernel", launched.result) + ")";
            return false;
        }
        const CUresult synced = cuStreamSynchronize(CU_STREAM_LEGACY);
        if (synced != CUDA_SUCCESS) {
            reason = cudaText("cuStreamSynchronize", synced);
            return false;
        }
        return true;
    } catch (...) {
        reason = "internal error";
        return false;
    }
}

}  // namespace osv::reframe::direct
