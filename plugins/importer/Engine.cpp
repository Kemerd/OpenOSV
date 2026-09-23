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
//   OsvEngine_QuerySettings  [WP-SETTINGS] the Source Settings the engine
//                            would render a file with, and their generation -
//                            no decode, no GPU - so the effect can decide
//                            before it decodes anything.
//
// SOURCE SETTINGS [WP-SETTINGS].  The engine renders with its own instance of
// each file; Premiere's instances publish the settings the user chose
// (enginePublishPrefs, from ImporterInstance::applyPrefsLocked) and every
// acquire applies the ones in force.  Files are keyed by their identity on
// disk (volume serial + file index), never by path spelling, and the newest
// Premiere instance of a file wins over older ones still holding an old
// blob.  Each change bumps a per-file generation that is reported with every
// frame, so a field log shows exactly which settings reached the screen.
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
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

// ===========================================================================
//  [WP-SETTINGS] File identity
//
//  Two parties name the same file: Premiere's importer instances (the path
//  imOpenFile8 was handed) and the effect (the media node's
//  "MediaNode::MediaInstanceString").  Nothing guarantees they spell it the
//  same way - case, '/' versus '\', a "\\?\" long-path prefix, an 8.3 short
//  name, a drive letter versus a UNC share, a hard link - and a mismatch
//  means the direct path renders with settings nobody published for it,
//  silently.  So a file is keyed by what the file system says it IS: the
//  volume serial number and the file index (GetFileInformationByHandle).
//  Only when that cannot be read (the file vanished, a file system without
//  stable ids) does the key fall back to the normalised spelling.
// ===========================================================================

/// What the file system says a file is.
struct FileIdentity {
    std::uint64_t volume = 0;  ///< dwVolumeSerialNumber.
    std::uint64_t index = 0;   ///< nFileIndexHigh:nFileIndexLow.
    bool valid = false;        ///< False: the identity could not be read.
    std::uint32_t error = 0;   ///< GetLastError() of the failed read, for the log.
};

/// Strip the Win32 namespace prefixes, turn '/' into '\' and make the path
/// absolute with "." and ".." resolved (GetFullPathNameW, no disk access).
/// The case is kept: this is the spelling the file is OPENED with, and a
/// directory can be case-sensitive (the per-directory flag WSL sets).
[[nodiscard]] std::wstring canonicalPath(const std::filesystem::path& path) {
    std::wstring s = path.wstring();
    // "\\?\UNC\server\share\x" -> "\\server\share\x"; "\\?\C:\x" -> "C:\x".
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        s = L"\\\\" + s.substr(8);
    } else if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\??\\", 0) == 0) {
        s = s.substr(4);
    }
    std::replace(s.begin(), s.end(), L'/', L'\\');

    // Absolute and canonical.  A failure keeps the spelling as it is: the
    // key is then merely less forgiving, never wrong.
    const DWORD need = GetFullPathNameW(s.c_str(), 0, nullptr, nullptr);
    if (need > 0 && need <= kMaxPathChars + 1) {
        std::wstring full(static_cast<std::size_t>(need), L'\0');
        const DWORD got = GetFullPathNameW(s.c_str(), need, full.data(), nullptr);
        if (got > 0 && got < need) {
            full.resize(got);
            s = std::move(full);
        }
    }
    return s;
}

/// The canonical path lower-cased: the form two spellings are compared in.
/// Windows compares names case-insensitively, and every character a real
/// path uses lower-cases the way NTFS's upcase table does.
[[nodiscard]] std::wstring loweredPath(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    return s;
}

/// Read a file's identity.  Opens for attributes only, sharing everything,
/// so it never conflicts with the importer's own GENERIC_READ handle or
/// with anything else that has the clip open.
[[nodiscard]] FileIdentity readFileIdentity(const std::wstring& path) noexcept {
    FileIdentity id;
    if (path.empty()) {
        return id;
    }
    // FILE_FLAG_BACKUP_SEMANTICS lets the open succeed on a directory too,
    // which is harmless (a directory is never a clip) and avoids a special
    // error path for it.
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        id.error = static_cast<std::uint32_t>(GetLastError());
        return id;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    const DWORD infoError = ok ? 0u : GetLastError();
    CloseHandle(h);
    if (!ok) {
        id.error = static_cast<std::uint32_t>(infoError);
        return id;
    }
    id.volume = static_cast<std::uint64_t>(info.dwVolumeSerialNumber);
    id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | static_cast<std::uint64_t>(info.nFileIndexLow);
    // A file system that reports neither (some network redirectors) gives no
    // usable identity; every file would share one key.
    id.valid = (id.volume != 0 || id.index != 0);
    return id;
}

/// A path as UTF-8 for a log line; never throws.
[[nodiscard]] std::string utf8Of(const std::wstring& wide) noexcept {
    try {
        if (wide.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
        if (n <= 0) {
            return "?";
        }
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    } catch (...) {
        return "?";
    }
}

/// A path's file name as UTF-8 for a log line; never throws.
[[nodiscard]] std::string nameOf(const std::filesystem::path& path) noexcept {
    try {
        return utf8Of(path.filename().wstring());
    } catch (...) {
        return "?";
    }
}

/// Identities already read, by canonical spelling (case kept: in a
/// case-sensitive directory "A.OSV" and "a.osv" are two files).  Every
/// acquire (one per rendered frame) and every publication asks, and an open
/// + query costs tens of microseconds - more with an on-access virus scanner
/// - so each spelling is resolved once per process.  Only successful reads
/// are kept: a file that does not exist yet is looked up again next time.
struct IdentityCache {
    std::mutex mutex;
    std::unordered_map<std::wstring, FileIdentity> byPath;
};

/// Created on first use, never destroyed (see the file header).
[[nodiscard]] IdentityCache& identityCache() {
    static IdentityCache* instance = new IdentityCache();
    return *instance;
}

/// Upper bound on cached spellings; the cache is simply emptied when a
/// session somehow names more files than this.
constexpr std::size_t kMaxIdentityCache = 4096;

/// The identity of `path` (cached), and its normalised (canonical,
/// lower-cased) spelling in `normalisedOut` when given.
[[nodiscard]] FileIdentity identityOf(const std::filesystem::path& path, std::wstring* normalisedOut = nullptr) {
    const std::wstring canonical = canonicalPath(path);
    const std::wstring normal = loweredPath(canonical);
    if (normalisedOut) {
        *normalisedOut = normal;
    }
    IdentityCache& cache = identityCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.byPath.find(canonical);
        if (it != cache.byPath.end()) {
            return it->second;
        }
    }
    // The disk is asked OUTSIDE the lock: another thread's lookup of a
    // cached file must not wait behind this one's open.
    const FileIdentity id = readFileIdentity(canonical);
    if (id.valid) {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.byPath.size() >= kMaxIdentityCache) {
            cache.byPath.clear();
        }
        const bool inserted = cache.byPath.emplace(canonical, id).second;
        // Once per spelling: the line that shows, in a field log, whether
        // Premiere's importer and the effect named the file the same way.
        if (inserted) {
            PluginLog::info("direct: '{}' is file {:08x}:{:016x}", utf8Of(path.wstring()), id.volume, id.index);
        }
    } else {
        PluginLog::oncef("direct/no-identity/" + utf8Of(normal), PluginLog::Level::Warn,
                         "direct: '{}' has no readable file identity (Win32 error {}); it is keyed by its "
                         "normalised path",
                         utf8Of(path.wstring()), id.error);
    }
    return id;
}

/// The registry key for an identity (or, when it could not be read, for the
/// normalised spelling).  The two forms can never collide ("id:" / "path:").
[[nodiscard]] std::wstring keyForIdentity(const FileIdentity& id, const std::wstring& normalised) {
    if (id.valid) {
        wchar_t buffer[64] = {};
        std::swprintf(buffer, std::size(buffer), L"id:%016llx:%016llx", static_cast<unsigned long long>(id.volume),
                      static_cast<unsigned long long>(id.index));
        return buffer;
    }
    return L"path:" + normalised;
}

/// The registry key for a file: its identity when readable, otherwise its
/// normalised spelling.
[[nodiscard]] std::wstring keyFor(const std::filesystem::path& path) {
    std::wstring normal;
    const FileIdentity id = identityOf(path, &normal);
    return keyForIdentity(id, normal);
}

// ===========================================================================
//  [WP-SETTINGS] Published Source Settings
// ===========================================================================

/// The Source Settings in force for one file, and who put them there.
struct PublishedSettings {
    PrefsBlob prefs = PrefsBlob::defaults();
    /// Token of the publishing instance (see SettingsPublisher).
    std::uint64_t publisher = 0;
    /// False while the only publication is an instance's defaults (the host
    /// handed it no blob); any host blob then replaces it.
    bool fromHost = false;
    /// Bumped on every change of `prefs`; the first publication is 1.
    std::uint32_t generation = 0;
    /// Refused publications logged since the last change, bounded so a
    /// lingering instance cannot flood the log.
    std::uint32_t refusalsLogged = 0;
    /// The file the settings belong to, for the effect's log lines.
    FileIdentity identity;
};

/// OSV_TRANSFER_* the clip's colour output encodes the importer's frames in.
/// Mirrors the importer's own mapping (toOutputTransfer in
/// ImporterInstance.cpp); the engine tests pin the two together by comparing
/// this with the transfer of the clip's own colour block.
[[nodiscard]] std::int32_t clipTransferFor(const PrefsBlob& prefs) noexcept {
    switch (prefs.color()) {
    case PrefsColorOutput::HLG:    return OSV_TRANSFER_HLG;
    case PrefsColorOutput::Rec709: return OSV_TRANSFER_REC709;
    case PrefsColorOutput::DLogM:  return OSV_TRANSFER_PASSTHROUGH;
    case PrefsColorOutput::PQ:
    case PrefsColorOutput::Count:
    default:                       return OSV_TRANSFER_PQ;
    }
}

/// Fill the ABI block from a blob.  structSize is set to this build's size.
void fillClipSettings(OsvEngineClipSettings& out, const PrefsBlob& prefs, std::uint32_t generation,
                      const FileIdentity& identity) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.structSize = static_cast<std::uint32_t>(sizeof(OsvEngineClipSettings));
    out.generation = generation;
    out.clipTransfer = clipTransferFor(prefs);
    out.exposureStops = prefs.exposureStops;
    out.colorOutput = prefs.colorOutput;
    out.calibration = prefs.calibration;
    out.dlogmFit = prefs.dlogmFit;
    out.stabilization = prefs.stabilization;
    out.directColour = prefs.directColour;
    out.fileVolume = identity.valid ? identity.volume : 0u;
    out.fileIndex = identity.valid ? identity.index : 0u;
}

/// Next publisher token (0 is reserved for "anonymous").
std::atomic<std::uint64_t> g_nextPublisherToken{1};

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
    /// The engine's own instance of each file it has been asked for, keyed
    /// by keyFor() (the file's identity).
    std::unordered_map<std::wstring, std::shared_ptr<ImporterInstance>> clips;
    /// [WP-SETTINGS] The Source Settings in force per file (keyFor()), as
    /// the newest of Premiere's instances of it published them.
    std::unordered_map<std::wstring, PublishedSettings> prefs;
};

/// Created on first use, never destroyed (see the file header).
[[nodiscard]] Registry& registry() {
    static Registry* instance = new Registry();
    return *instance;
}

/// The engine's instance of `path` (registry key `key`, from keyFor()),
/// opened on first use.  Returns null with `error` filled when the file
/// cannot be opened as a dual-fisheye OSV.
[[nodiscard]] std::shared_ptr<ImporterInstance> clipFor(const std::filesystem::path& path, const std::wstring& key,
                                                        std::string& error) {
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

/// [WP-SETTINGS] The Source Settings in force for the file with registry key
/// `key`, if any were published.
[[nodiscard]] bool publishedPrefs(const std::wstring& key, PublishedSettings& out) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.prefs.find(key);
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

// ---- [WP-SETTINGS] ------------------------------------------------------------

std::uint64_t engineNewPublisherToken() noexcept {
    return g_nextPublisherToken.fetch_add(1);
}

void enginePublishPrefs(const std::filesystem::path& path, const PrefsBlob& prefs,
                        const SettingsPublisher& publisher) noexcept {
    try {
        // The key (a cached identity lookup) is computed before the registry
        // lock is taken: a first lookup opens the file.
        std::wstring normal;
        const FileIdentity identity = identityOf(path, &normal);
        const std::wstring key = keyForIdentity(identity, normal);

        // What happened, decided under the lock and logged after it.
        enum class Outcome { Unchanged, Adopted, Refused };
        Outcome outcome = Outcome::Unchanged;
        PublishedSettings now;
        {
            Registry& r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.prefs.find(key);
            if (it == r.prefs.end()) {
                // Nothing known for the file yet: any publication fills the
                // blank, the defaults of a blob-less instance included.
                PublishedSettings fresh;
                fresh.prefs = prefs;
                fresh.publisher = publisher.token;
                fresh.fromHost = publisher.fromHost;
                fresh.generation = 1;
                fresh.identity = identity;
                it = r.prefs.emplace(key, fresh).first;
                outcome = Outcome::Adopted;
            } else {
                PublishedSettings& slot = it->second;
                // The precedence rule (see SettingsPublisher):
                //  * an instance's defaults never override a host blob, and
                //    replace other defaults only when they are its own age
                //    or newer;
                //  * a host blob replaces defaults always, and another host
                //    blob when its instance is at least as new.
                const bool newerOrSame = publisher.token >= slot.publisher;
                const bool accept = publisher.fromHost ? (!slot.fromHost || newerOrSame)
                                                       : (!slot.fromHost && newerOrSame);
                if (accept) {
                    const bool changed = !(slot.prefs == prefs);
                    slot.prefs = prefs;
                    slot.publisher = publisher.token;
                    slot.fromHost = publisher.fromHost;
                    slot.identity = identity;
                    if (changed) {
                        ++slot.generation;
                        // Each change may show its own few refusals.
                        slot.refusalsLogged = 0;
                        outcome = Outcome::Adopted;
                    }
                } else if (!(slot.prefs == prefs) && slot.refusalsLogged < 5) {
                    // Only a DIFFERENT blob from an older instance is worth a
                    // line; the same blob republished changes nothing.
                    ++slot.refusalsLogged;
                    outcome = Outcome::Refused;
                }
            }
            now = it->second;
        }

        // Only real changes are logged: every Premiere instance of the clip
        // publishes the same blob when it opens, and those repeats say
        // nothing.  This line and the effect's "Source Settings generation"
        // line are the two ends of one change.
        if (outcome == Outcome::Adopted) {
            PluginLog::info("direct: Source Settings generation {} for '{}' (file {:08x}:{:016x}) from importer "
                            "instance #{}{} (importer id {}): colour {}, fit {}, exposure {:+.2f}, calibration {}, "
                            "stabilisation {}, seam {}, gain {}, parallax {}, Program Monitor Colour {}",
                            now.generation, nameOf(path), identity.volume, identity.index, publisher.token,
                            publisher.fromHost ? "" : " (its defaults: the host gave it no settings)",
                            publisher.importerId, static_cast<int>(prefs.colorOutput),
                            static_cast<int>(prefs.dlogmFit), static_cast<double>(prefs.exposureStops),
                            static_cast<int>(prefs.calibration), static_cast<int>(prefs.stabilization),
                            static_cast<int>(prefs.seamSearch), static_cast<int>(prefs.gainMatch),
                            static_cast<int>(prefs.parallax),
                            prefs.directColourMode() == PrefsDirectColour::MatchSource ? "match Source monitor"
                                                                                       : "sequence space");
        } else if (outcome == Outcome::Refused) {
            // Two reasons to refuse, worded apart so a field log says which.
            PluginLog::info("direct: kept Source Settings generation {} for '{}' (colour {}, exposure {:+.2f}) from "
                            "importer instance #{}: instance #{} {} colour {}, exposure {:+.2f} - {}",
                            now.generation, nameOf(path), static_cast<int>(now.prefs.colorOutput),
                            static_cast<double>(now.prefs.exposureStops), now.publisher, publisher.token,
                            publisher.fromHost ? "is older and holds" : "was given no settings; its defaults are",
                            static_cast<int>(prefs.colorOutput), static_cast<double>(prefs.exposureStops),
                            publisher.fromHost ? "the direct path follows the newest instance"
                                               : "defaults never override settings the host handed over");
        }
    } catch (...) {
        // A failed publish only means the engine renders this clip with the
        // settings it had; never worth failing the importer call over.
    }
}

// ---- [/WP-SETTINGS] -----------------------------------------------------------

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
        // [WP-SETTINGS] One identity lookup for both the clip and its settings.
        const std::wstring key = keyFor(path);

        // ---- the clip -------------------------------------------------------------
        std::string openError;
        std::shared_ptr<ImporterInstance> clip = clipFor(path, key, openError);
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
        // [WP-SETTINGS] The settings this frame is rendered with, for the caller.
        OsvEngineClipSettings frameSettings{};
        {
            std::lock_guard<std::mutex> lock(clip->lock());

            // ---- [WP-SETTINGS] the user's Source Settings for this file -----------
            // As the newest of Premiere's instances published them (a no-op
            // when nothing changed).  Read once: the generation reported with
            // the frame is exactly the one applied to it, even if another
            // publication lands while the frame is being built.
            PublishedSettings published;
            const bool known = publishedPrefs(key, published);
            if (known) {
                const PrefsBlob before = clip->prefsLocked();
                clip->applyPrefsLocked(&published.prefs, PrefsBlob::kSize);
                if (!(before == clip->prefsLocked())) {
                    PluginLog::info("direct: engine now renders '{}' with Source Settings generation {}: colour {}, "
                                    "exposure {:+.2f}, fit {}, calibration {}, stabilisation {} (was colour {}, "
                                    "exposure {:+.2f}, fit {}, calibration {}, stabilisation {})",
                                    nameOf(path), published.generation, static_cast<int>(published.prefs.colorOutput),
                                    static_cast<double>(published.prefs.exposureStops),
                                    static_cast<int>(published.prefs.dlogmFit),
                                    static_cast<int>(published.prefs.calibration),
                                    static_cast<int>(published.prefs.stabilization),
                                    static_cast<int>(before.colorOutput), static_cast<double>(before.exposureStops),
                                    static_cast<int>(before.dlogmFit), static_cast<int>(before.calibration),
                                    static_cast<int>(before.stabilization));
                }
            }
            fillClipSettings(frameSettings, clip->prefsLocked(), known ? published.generation : 0u,
                             known ? published.identity : identityOf(path));

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
        out->settings = frameSettings;  // [WP-SETTINGS]
        out->lease = lease.release();
        g_liveLeases.fetch_add(1);

        PluginLog::oncef("direct/first-frame", PluginLog::Level::Info,
                         "direct: first frame served - '{}' frame {} (media {} ticks), transfer {}, seam {}, warp {}, "
                         "blend seam {}, Source Settings generation {}",
                         nameOf(path), index, static_cast<long long>(request->mediaTicks), out->stitch.color.transfer,
                         out->seamDevice ? "yes" : "no", out->warpDevice ? "yes" : "no",
                         out->blendSeamDevice ? "yes" : "no", out->settings.generation);
        return OSV_ENGINE_OK;
    } catch (const std::exception& e) {
        writeError(error, errorCapacity, std::string("internal error: ") + e.what());
        return OSV_ENGINE_ERR_INTERNAL;
    } catch (...) {
        writeError(error, errorCapacity, "internal error");
        return OSV_ENGINE_ERR_INTERNAL;
    }
}

// ---- [WP-SETTINGS] ------------------------------------------------------------

extern "C" __declspec(dllexport) std::int32_t OsvEngine_QuerySettings(const wchar_t* pathPtr,
                                                                       OsvEngineClipSettings* out, char* error,
                                                                       std::int32_t errorCapacity) {
    using namespace osv::premiere;
    try {
        // ---- the caller's structures ------------------------------------------
        if (!pathPtr || !out) {
            writeError(error, errorCapacity, "null path or settings block");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        if (out->structSize < sizeof(OsvEngineClipSettings)) {
            writeError(error, errorCapacity, "settings block size mismatch (built from a different ABI)");
            return OSV_ENGINE_ERR_VERSION;
        }
        const std::size_t pathLen = wcsnlen(pathPtr, kMaxPathChars + 1);
        if (pathLen == 0 || pathLen > kMaxPathChars) {
            writeError(error, errorCapacity, "empty or unterminated path");
            return OSV_ENGINE_ERR_ARGUMENT;
        }
        const std::filesystem::path path(std::wstring(pathPtr, pathLen));

        // ---- what is in force -----------------------------------------------------
        // No clip is opened and nothing touches the GPU: this is the cheap
        // question the effect asks before deciding whether to decode at all.
        const std::wstring key = keyFor(path);
        PublishedSettings published;
        if (publishedPrefs(key, published)) {
            fillClipSettings(*out, published.prefs, published.generation, published.identity);
        } else {
            // Generation 0: what the engine WOULD render (its defaults), and
            // the file's identity so the caller's log can still name it.
            fillClipSettings(*out, PrefsBlob::defaults(), 0u, identityOf(path));
        }
        return OSV_ENGINE_OK;
    } catch (const std::exception& e) {
        writeError(error, errorCapacity, std::string("internal error: ") + e.what());
        return OSV_ENGINE_ERR_INTERNAL;
    } catch (...) {
        writeError(error, errorCapacity, "internal error");
        return OSV_ENGINE_ERR_INTERNAL;
    }
}

// ---- [/WP-SETTINGS] -----------------------------------------------------------

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
