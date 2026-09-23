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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
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
    // [WP-SETTINGS] Optional: without it the settings reported with each
    // frame decide, one decode later.
    found.querySettings =
        reinterpret_cast<OsvEngineQuerySettingsFn>(GetProcAddress(module, OSV_ENGINE_SYM_QUERY_SETTINGS));
    ColourMode forced = ColourMode::SequenceSpace;
    const bool overridden = colourModeOverride(forced);
    PluginLog::info("reframe/direct: the importer's engine is available (ABI {}, Source Settings query {}, "
                    "Program Monitor Colour {}{})",
                    version, found.querySettings ? "yes" : "no",
                    overridden ? colourModeLabel(forced) : "per clip (Source Settings)",
                    overridden ? " for every clip (OSV_DIRECT_COLOR)" : "");
    return found;
}

// ===========================================================================
//  Segment-graph property reading
// ===========================================================================

/// IterateNodeProperties hands every property to a C callback whose
/// plug-in object is a 32-bit integer - too narrow for a pointer - so the
/// lookup state is thread-local; the iteration is synchronous.  Several keys
/// are collected in ONE pass: a pass walks every property of the node.
struct PropertyLookup {
    static constexpr std::size_t kMaxKeys = 4;
    const char* keys[kMaxKeys] = {};
    std::string values[kMaxKeys];
    bool found[kMaxKeys] = {};
    std::size_t count = 0;
};
thread_local PropertyLookup* t_lookup = nullptr;

prSuiteError lookupProperty(csSDK_int32, const char* key, const prUTF8Char* value) {
    if (!t_lookup || !key) {
        return suiteError_NoError;
    }
    // A C callback from the host: an allocation failure copying a value must
    // not unwind into Premiere's stack, it just leaves that key unfound.
    try {
        for (std::size_t i = 0; i < t_lookup->count && i < PropertyLookup::kMaxKeys; ++i) {
            if (t_lookup->keys[i] && std::strcmp(key, t_lookup->keys[i]) == 0) {
                t_lookup->values[i] = value ? reinterpret_cast<const char*>(value) : "";
                t_lookup->found[i] = true;
            }
        }
    } catch (...) {
    }
    return suiteError_NoError;
}

/// Read the properties named in `lookup` from a node, as UTF-8, in one pass.
/// Returns false when the host could not iterate at all.
[[nodiscard]] bool nodeProperties(const PrSDKVideoSegmentSuite& s, csSDK_int32 node, PropertyLookup& lookup) {
    if (!s.IterateNodeProperties) {
        return false;
    }
    t_lookup = &lookup;
    const prSuiteError err = s.IterateNodeProperties(node, &lookupProperty, 0);
    t_lookup = nullptr;
    return err == suiteError_NoError;
}

// ===========================================================================
//  [WP-SETTINGS] Media identity evidence
//
//  Premiere decides whether to render the effect again from its own identity
//  of the clip's nodes.  When a Source Settings change reaches the Program
//  monitor, that identity must have changed; when it does not, this log
//  line is how a field session proves which of the two happened.  One line
//  per file per change - Premiere creates a GPU instance (and so resolves
//  the source) for every parameter step of a drag, ~100 a second.
// ===========================================================================

/// What the media node looked like the last time a source was resolved.
struct MediaEvidence {
    std::string mediaHash;
    std::string modState;
    std::string clipId;
};

/// Last evidence per clip (file + ClipID: two track items of one file are
/// two clips and must not be reported as one clip flapping between them),
/// never destroyed (static-destructor order).
struct EvidenceMemory {
    std::mutex mutex;
    std::map<std::wstring, MediaEvidence> byClip;
};
[[nodiscard]] EvidenceMemory& evidenceMemory() {
    static EvidenceMemory* instance = new EvidenceMemory();
    return *instance;
}

/// Log the binding's media identity when it is the first for its clip or
/// differs from the last one seen for it.
void noteMediaEvidence(const SourceBinding& b) noexcept {
    try {
        EvidenceMemory& m = evidenceMemory();
        const std::wstring key = b.path + L"|" + std::wstring(b.clipId.begin(), b.clipId.end());
        MediaEvidence previous;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(m.mutex);
            const auto it = m.byClip.find(key);
            if (it != m.byClip.end()) {
                known = true;
                previous = it->second;
                if (previous.mediaHash == b.mediaHash && previous.modState == b.modState &&
                    previous.clipId == b.clipId) {
                    return;  // Nothing new: the common case.
                }
            }
            if (!known && m.byClip.size() >= 1024) {
                m.byClip.clear();
            }
            MediaEvidence& slot = m.byClip[key];
            slot.mediaHash = b.mediaHash;
            slot.modState = b.modState;
            slot.clipId = b.clipId;
        }
        const std::size_t slash = b.path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? b.path : b.path.substr(slash + 1);
        std::string nameUtf8;
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0,
                                              nullptr, nullptr);
            if (n > 0) {
                nameUtf8.assign(static_cast<std::size_t>(n), '\0');
                WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nameUtf8.data(), n,
                                    nullptr, nullptr);
            }
        }
        if (!known) {
            PluginLog::info("reframe/direct: '{}' media node hash {}, mod state {}, clip id {}", nameUtf8,
                            b.mediaHash, b.modState, b.clipId);
        } else {
            PluginLog::info("reframe/direct: '{}' media node CHANGED - hash {} (was {}), mod state {} (was {}), clip "
                            "id {} (was {}): Premiere rebuilt the clip's media",
                            nameUtf8, b.mediaHash, previous.mediaHash, b.modState, previous.modState, b.clipId,
                            previous.clipId);
        }
    } catch (...) {
        // Evidence only; never worth a failed CreateInstance.
    }
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

// ===========================================================================
//  The clip time -> media time log
//
//  Premiere maps the time an effect renders at to a time in the MEDIA
//  through the clip node's TransformNodeTime: in point, speed, reverse and
//  time remapping all live there, and the engine then rounds that media time
//  to a frame.  Nothing but a real host can confirm the whole chain on a
//  trimmed, sped-up or reversed clip, so the first frames of every instance
//  say exactly what happened.  CreateInstance runs again on every parameter
//  change (a Program Monitor drag makes dozens a second), so the lines are
//  also capped process-wide per time window: a drag costs at most
//  kMappingLinesPerWindow lines per window, and a field check - trim, play,
//  read the log - finds its lines unless such a burst spent the budget in
//  the seconds just before.
// ===========================================================================

/// Frames logged per instance.
constexpr int kMappingFramesPerInstance = 3;
/// Lines allowed per window across every instance of the process.
constexpr int kMappingLinesPerWindow = 24;
/// The window.
constexpr std::chrono::seconds kMappingWindow{10};
/// Premiere's tick rate (PrSDKTypes.h), for the seconds in the line.
constexpr double kTicksPerSecond = 254016000000.0;

/// The file name of a UTF-16 path as UTF-8, for a log line.
[[nodiscard]] std::string fileNameUtf8(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    if (name.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), out.data(), n, nullptr, nullptr);
    return out;
}

/// Log one frame's clip time -> media time -> frame, if this instance and
/// the process-wide budget both still allow it.
void logMapping(const DirectRequest& request, PrTime media, std::uint32_t frameIndex) noexcept {
    try {
        if (!request.source) {
            return;
        }
        static std::mutex mutex;
        static std::chrono::steady_clock::time_point windowStart{};
        static int linesInWindow = 0;
        std::lock_guard<std::mutex> lock(mutex);
        const SourceBinding& source = *request.source;
        if (source.mappingFramesLogged >= kMappingFramesPerInstance) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (linesInWindow == 0 || now - windowStart >= kMappingWindow) {
            windowStart = now;
            linesInWindow = 0;
        }
        if (linesInWindow >= kMappingLinesPerWindow) {
            return;  // over budget: this frame is not counted, a later one may be logged
        }
        ++linesInWindow;
        const int ordinal = ++source.mappingFramesLogged;
        PluginLog::info("reframe/direct: mapping {}/{} of this instance (clip node {}) - clip time {} ticks ({:.4f} s) "
                        "-> media time {} ticks ({:.4f} s) -> frame {} of '{}'",
                        ordinal, kMappingFramesPerInstance, source.ownerNode,
                        static_cast<long long>(request.clipTime), static_cast<double>(request.clipTime) / kTicksPerSecond,
                        static_cast<long long>(media), static_cast<double>(media) / kTicksPerSecond, frameIndex,
                        fileNameUtf8(source.path));
    } catch (...) {
        // A diagnostic must never fail a render.
    }
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
        // The path, plus [WP-SETTINGS] the media's identity evidence, in one
        // pass over the node's properties.
        PropertyLookup props;
        props.keys[0] = kVideoSegmentProperty_Media_InstanceString;
        props.keys[1] = kVideoSegmentProperty_Media_ModState;
        props.keys[2] = kVideoSegmentProperty_Media_ClipID;
        props.count = 3;
        const bool gotPath = isMedia && nodeProperties(*segment, media, props) && props.found[0];
        const std::string pathUtf8 = gotPath ? props.values[0] : std::string();
        if (isMedia) {
            // The GUID is NUL-terminated in a 37-byte field; never read past it.
            b.mediaHash.assign(hash.mGUID, strnlen(hash.mGUID, sizeof(hash.mGUID)));
            b.modState = props.found[1] ? props.values[1] : std::string("<none>");
            b.clipId = props.found[2] ? props.values[2] : std::string("<none>");
        }
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
        noteMediaEvidence(b);  // [WP-SETTINGS]
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
        // real session: primaries 9, transfer 16 - Rec.2100 PQ.)  The table
        // is [WP-SETTINGS] transferForSeiCodes(), shared with the Source
        // Settings rules so both agree on which spaces exist.
        const int primaries = sei.colorPrimariesCode;
        const int transfer = sei.transferCharacteristicCode;
        reason = "working space: primaries " + std::to_string(primaries) + ", transfer " + std::to_string(transfer);
        const int produced = transferForSeiCodes(primaries, transfer);
        if (produced >= 0) {
            return produced;
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

        // ---- [WP-SETTINGS] may the direct path render this clip at all? -----------
        // Asked BEFORE anything is decoded: the engine's answer is a map
        // lookup, a decode is milliseconds.  A clip the rule hands to the
        // equirect route costs nothing here on every later frame either.
        const std::wstring& path = request.source->path;
        OsvEngineClipSettings queried{};
        bool haveQuery = false;
        if (api.querySettings) {
            queried.structSize = static_cast<std::uint32_t>(sizeof(OsvEngineClipSettings));
            char queryError[256] = {};
            const std::int32_t qrc =
                api.querySettings(path.c_str(), &queried, queryError, static_cast<std::int32_t>(sizeof(queryError)));
            if (qrc == OSV_ENGINE_OK) {
                haveQuery = true;
                const SettingsDecision decision =
                    decideSettings(queried, request.transfer, colourModeFor(queried));
                if (noteDecision(path, queried, request.transfer, decision)) {
                    PluginLog::info("{}", describeDecision(path, queried, request.transfer, decision));
                }
                if (!decision.direct) {
                    reason = std::string(kPolicyReasonPrefix) + decision.why;
                    return false;
                }
            } else {
                // A failed QUESTION is not a verdict: the frame's own
                // settings block decides below, after the acquire.
                PluginLog::oncef("reframe/direct/query-failed", PluginLog::Level::Warn,
                                 "reframe/direct: the engine could not report the Source Settings ({}: {}); deciding "
                                 "from each frame's settings instead",
                                 qrc, queryError);
            }
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

        // The first frames of each instance record the whole time chain.
        logMapping(request, media, frame.frameIndex);

        if (frame.paramsSize != sizeof(OsvRenderParams)) {
            reason = "the engine's parameter block has a different layout";
            return false;
        }

        // ---- [WP-SETTINGS] the settings the frame was ACTUALLY rendered with ----------
        // A publication can land between the question above and the acquire;
        // the frame's own block is authoritative, so a changed generation is
        // decided again (and without the query, it is the only decision).
        if (!haveQuery || frame.settings.generation != queried.generation) {
            const SettingsDecision decision =
                decideSettings(frame.settings, request.transfer, colourModeFor(frame.settings));
            if (noteDecision(path, frame.settings, request.transfer, decision)) {
                PluginLog::info("{}", describeDecision(path, frame.settings, request.transfer, decision));
            }
            if (!decision.direct) {
                reason = std::string(kPolicyReasonPrefix) + decision.why;
                return false;  // the guard releases the lease
            }
        }

        // ---- the view ----------------------------------------------------------------
        StitchState stitch;
        stitch.equirect = frame.stitch;
        stitch.seamTable = frame.seamDevice;
        stitch.warpGrid = frame.warpDevice;
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
