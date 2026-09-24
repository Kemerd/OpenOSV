// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SteadyStage implementation: the request, the background worker, the
// process-wide caches of lens rotations (memory + disk) and clip corrections
// (memory), and the stage's own short-lived decoder.  The policy is in the
// header.

#include "SteadyStage.h"

#include "NumberParse.h"
#include "PluginLog.h"

#include "osv/core/ThreadPool.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace osv::premiere {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t`.
[[nodiscard]] double msSince(Clock::time_point t) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// False while OPENOSV_STEADY_NO_SHARED_CACHE is set to anything but "0": every
/// instance then measures its own rotation and clip correction and nothing is
/// read from or written to the process-wide caches (the disk cache included).
/// A diagnostics switch - it proves a clip's result does not depend on what
/// another instance happened to measure - read on every use, so a test can
/// toggle it.  The same CRT getenv the importer's other switches use.
[[nodiscard]] bool sharedCacheEnabled() noexcept {
#if defined(_WIN32)
    char value[8] = {};
    std::size_t length = 0;
    if (::getenv_s(&length, value, sizeof(value), "OPENOSV_STEADY_NO_SHARED_CACHE") != 0 || length == 0) {
        return true;
    }
    return value[0] == '0';
#else
    // POSIX getenv: no getenv_s, and nothing here outlives the call.
    const char* value = std::getenv("OPENOSV_STEADY_NO_SHARED_CACHE");
    if (!value || value[0] == '\0') {
        return true;
    }
    return value[0] == '0';
#endif
}

// =============================================================================
//  Identities
// =============================================================================

/// Identity of a clip FILE: an edited or replaced file (another size or write
/// time) is another clip as far as any cached measurement is concerned.
struct FileIdentity {
    std::string path;         ///< Absolute path, UTF-8, ASCII lower-cased (Windows paths ignore case).
    std::uintmax_t size = 0;  ///< Bytes.
    long long mtime = 0;      ///< last_write_time ticks.
};

/// The identity of `path`; nullopt when the file cannot be inspected (then
/// nothing about it is cached - a measurement still runs).
[[nodiscard]] std::optional<FileIdentity> identityOf(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(path, ec);
        if (ec) {
            return std::nullopt;
        }
        FileIdentity id;
        const std::u8string u8 = abs.u8string();
        id.path.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        for (char& c : id.path) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        id.size = std::filesystem::file_size(abs, ec);
        if (ec) {
            return std::nullopt;
        }
        const auto t = std::filesystem::last_write_time(abs, ec);
        if (ec) {
            return std::nullopt;
        }
        id.mtime = static_cast<long long>(t.time_since_epoch().count());
        return id;
    } catch (...) {
        return std::nullopt;
    }
}

/// 64-bit FNV-1a over the exact bytes of everything a measurement depends
/// on.  Doubles are hashed by their bit pattern: two rigs that differ in the
/// last bit are two rigs (a measurement through one is not the other's).
class Hasher {
public:
    void bytes(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            m_h ^= p[i];
            m_h *= 1099511628211ull;
        }
    }
    void u64(std::uint64_t v) noexcept { bytes(&v, sizeof(v)); }
    void f64(double v) noexcept {
        // -0.0 and +0.0 are the same setting; one bit pattern for both.
        if (v == 0.0) {
            v = 0.0;
        }
        bytes(&v, sizeof(v));
    }
    void text(const std::string& s) noexcept {
        u64(s.size());
        bytes(s.data(), s.size());
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return m_h; }

private:
    std::uint64_t m_h = 1469598103934665603ull;
};

/// Everything of the rig a band pixel depends on.
void hashRig(Hasher& h, const geom::LensRig& rig) noexcept {
    for (std::size_t i = 0; i < 2; ++i) {
        const geom::KannalaBrandt5& L = rig.lens[i];
        for (const double v : {L.fx, L.fy, L.cx, L.cy, L.thetaMaxRad, L.rMaxPx}) {
            h.f64(v);
        }
        for (const double k : L.k) {
            h.f64(k);
        }
        for (const double m : rig.bodyToLens[i].m) {
            h.f64(m);
        }
        h.u64(rig.occlusionPolyStream[i].size());
        for (const Vec2d& p : rig.occlusionPolyStream[i]) {
            h.f64(p.x);
            h.f64(p.y);
        }
    }
    h.u64(static_cast<std::uint64_t>(rig.streamW));
    h.u64(static_cast<std::uint64_t>(rig.streamH));
    h.f64(rig.lensFovDeg);
}

/// The analysis blend.
void hashBlend(Hasher& h, const geom::BlendParams& b) noexcept {
    h.f64(b.lensFovDeg);
    h.f64(b.featherDeg);
    h.f64(b.occlusionFeatherPx);
    h.f64(b.seamShiftDeg);
    h.u64(b.useOcclusionMask ? 1u : 0u);
}

/// The clip correction's parameters (everything the importer can vary).
void hashClipParams(Hasher& h, const render::ClipSteadyParams& p) noexcept {
    const render::ParallaxWarpParams& w = p.parallax;
    h.u64(w.band.equirectW);
    h.f64(w.band.bandHalfDeg);
    h.u64(static_cast<std::uint64_t>(w.backend));
    h.u64(w.gridW);
    h.u64(w.gridRows);
    h.u64(w.decayRows);
    for (const double v : {w.crossMeridianScale, w.crossMeridianSmooth, w.maxCorrectionDeg, w.minConsistentFraction,
                           w.requiredImprovement, w.minResidual}) {
        h.f64(v);
    }
    h.u64(p.parallaxOn ? 1u : 0u);
    h.u64(p.seamOn ? 1u : 0u);
    h.u64(p.seamSearch.band.equirectW);
    h.f64(p.seamSearch.band.bandHalfDeg);
    h.u64(static_cast<std::uint64_t>(p.seamSearch.maxShiftPx));
    h.u64(static_cast<std::uint64_t>(p.seamSearch.windowHalfCols));
    h.f64(p.seamSearch.smoothSigmaCols);
    h.f64(p.seamSearch.minNcc);
    h.u64(p.carve.columns);
    for (const double v : {p.carve.narrowHalfWidthDeg, p.carve.wideHalfWidthDeg, p.carve.costWindowDeg,
                           p.carve.agreeResidual, p.carve.disagreeResidual, p.carve.edgeRampDeg}) {
        h.f64(v);
    }
    h.u64(p.rimCost ? 1u : 0u);
    h.u64(static_cast<std::uint64_t>(p.photo.mode));
    h.f64(p.photo.strength);
    h.u64(p.shadingOn ? 1u : 0u);
    h.u64(static_cast<std::uint64_t>(p.shading.mode));
    h.f64(p.shading.strength);
    const render::SteadyDecisionParams& d = p.decision;
    h.u64(d.sectors);
    h.u64(d.minPixels);
    for (const double v : {d.latHalfDeg, d.minStd, d.minOwnNcc, d.minGain, d.maxLoss, d.minKeep}) {
        h.f64(v);
    }
    h.u64(p.minGrids);
}

/// Lower-case hex of a hash.
[[nodiscard]] std::string hex64(std::uint64_t v) { return std::format("{:016x}", v); }

/// Cache key of a rotation: the file and the calibration rig.  Bump the
/// leading version when the fit changes, so an old verdict is not reused.
[[nodiscard]] std::string rotationKey(const FileIdentity& id, const geom::LensRig& baseRig) {
    Hasher h;
    hashRig(h, baseRig);
    return std::format("r1|{}|{}|{}|{}", id.size, id.mtime, hex64(h.value()), id.path);
}

/// Cache key of a clip correction: the file, the rig it is measured through
/// (rotation included), the blend, the sample frames and the parameters.
[[nodiscard]] std::string clipKey(const FileIdentity& id, const geom::LensRig& rig, const geom::BlendParams& blend,
                                  const std::vector<std::uint32_t>& frames, const render::ClipSteadyParams& params) {
    Hasher h;
    hashRig(h, rig);
    hashBlend(h, blend);
    h.u64(frames.size());
    for (const std::uint32_t f : frames) {
        h.u64(f);
    }
    hashClipParams(h, params);
    return std::format("c1|{}|{}|{}|{}", id.size, id.mtime, hex64(h.value()), id.path);
}

// =============================================================================
//  Process-wide caches
// =============================================================================

/// Clip corrections kept in memory: each is a grid (~100 KB), a seam (~8 KB)
/// and a table, so 16 clips are a couple of MB.
constexpr std::size_t kMaxCachedClips = 16;

struct CachedClip {
    std::shared_ptr<const render::ClipSteady> clip;
    std::uint64_t lastUse = 0;  ///< For the least-recently-used trim.
};

/// Everything shared between instances, behind one mutex.  `cv` wakes the
/// instances waiting for another instance's measurement of the same key.
struct Global {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, LensAlignVerdict> rotations;
    std::set<std::string> rotationsInFlight;
    bool diskLoaded = false;
    std::map<std::string, CachedClip> clips;
    std::set<std::string> clipsInFlight;
    std::uint64_t useCounter = 0;
};

Global& global() {
    static Global g;
    return g;
}

/// The disk cache path (next to the plug-in log); empty when the log has no
/// file, and then only the memory cache is used.
[[nodiscard]] std::filesystem::path diskCachePath() noexcept {
    try {
        const std::wstring log = PluginLog::filePath();
        if (log.empty()) {
            return {};
        }
        return std::filesystem::path(log).parent_path() / kLensAlignCacheFile;
    } catch (...) {
        return {};
    }
}

/// Parse a whole string as a number; false on anything but a complete parse.
template <class T>
[[nodiscard]] bool parseNumber(const std::string& text, T& out) noexcept {
    if (text.empty()) {
        return false;
    }
    // std::from_chars, or its locale-free twin where the standard library
    // has no floating-point from_chars (NumberParse.h).
    return parseWholeNumber(text.data(), text.data() + text.size(), out);
}

/// A number for the cache file: fixed, locale independent.
[[nodiscard]] std::string numberText(double v, int digits) {
    char buf[64] = {};
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, digits);
    return r.ec == std::errc{} ? std::string(buf, r.ptr) : std::string("0");
}

/// Load every well-formed line of the disk cache into `g.rotations`.  Lines:
///   1 TAB key-without-path TAB accepted TAB wx TAB wy TAB wz TAB angle TAB residual TAB path
/// where the key part is "r1|size|mtime|righash".  Anything malformed (a
/// truncated last line, another version) is skipped.  Caller holds g.mutex.
void loadDisk(Global& g) noexcept {
    g.diskLoaded = true;
    try {
        const std::filesystem::path file = diskCachePath();
        if (file.empty()) {
            return;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in.good()) {
            return;  // no cache yet
        }
        std::string line;
        std::size_t loaded = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            // Eight tab-separated fields, then the path (which may hold tabs
            // in theory, so it is simply the rest of the line).
            std::vector<std::string> f;
            std::size_t start = 0;
            for (int i = 0; i < 8; ++i) {
                const std::size_t tab = line.find('\t', start);
                if (tab == std::string::npos) {
                    break;
                }
                f.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            if (f.size() != 8 || f[0] != "1" || start >= line.size() || !f[1].starts_with("r1|")) {
                continue;
            }
            LensAlignVerdict v;
            int accepted = 0;
            bool ok = parseNumber(f[2], accepted) && (accepted == 0 || accepted == 1);
            ok = ok && parseNumber(f[3], v.wRad.x) && parseNumber(f[4], v.wRad.y) && parseNumber(f[5], v.wRad.z);
            ok = ok && parseNumber(f[6], v.angleDeg) && parseNumber(f[7], v.residualDeg) && v.wRad.isFinite();
            if (!ok) {
                continue;
            }
            v.accepted = accepted == 1;
            v.fromCache = true;
            v.summary = v.accepted ? std::format("{:.3f} deg, residual {:.3f} deg RMS (cached)", v.angleDeg,
                                                 v.residualDeg)
                                   : std::string("no rotation the flow agrees on (cached)");
            g.rotations[f[1] + "|" + line.substr(start)] = v;  // later lines win
            ++loaded;
        }
        PluginLog::debug("lens alignment: {} cached verdict(s) loaded", loaded);
    } catch (...) {
        // A damaged cache only costs a re-measurement.
        PluginLog::warn("lens alignment: the cache file could not be read; clips will be re-measured");
    }
}

/// Append one verdict to the disk cache.  Failures are logged, not fatal.
void appendDisk(const std::string& key, const LensAlignVerdict& v) noexcept {
    try {
        const std::filesystem::path file = diskCachePath();
        if (file.empty()) {
            return;
        }
        // The key is "r1|size|mtime|hash|path": the path goes last on the line.
        const std::size_t cut = [&] {
            std::size_t pos = 0;
            for (int i = 0; i < 4 && pos != std::string::npos; ++i) {
                pos = key.find('|', pos == 0 && i == 0 ? 0 : pos + 1);
            }
            return pos;
        }();
        if (cut == std::string::npos) {
            return;
        }
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out.good()) {
            PluginLog::warn("lens alignment: cannot write the cache file");
            return;
        }
        std::string text = "1\t" + key.substr(0, cut) + '\t' + (v.accepted ? "1" : "0");
        for (const double x : {v.wRad.x, v.wRad.y, v.wRad.z}) {
            text += '\t' + numberText(x, 12);
        }
        text += '\t' + numberText(v.angleDeg, 6) + '\t' + numberText(v.residualDeg, 6);
        text += '\t' + key.substr(cut + 1) + '\n';
        out << text;
    } catch (...) {
        PluginLog::warn("lens alignment: cannot write the cache file");
    }
}

// =============================================================================
//  The stage's own decoder
// =============================================================================

/// A short-lived host-frame reader for the sample frames: D3D11VA copied back
/// (two decoder threads - it only walks forward through a few frames), else
/// software; a hardware failure mid-way is retried once in software.  It never
/// touches the instance's own decoders, so it can run beside them.
class JobReader {
public:
    JobReader(std::filesystem::path path, meta::FormatInfo format, bool containerSamples)
        : m_path(std::move(path)), m_format(std::move(format)), m_containerSamples(containerSamples) {}

    [[nodiscard]] Result<video::FramePair> read(std::uint32_t frame) {
        if (!m_reader) {
            OSV_TRY(open(false));
        }
        auto pair = m_reader->read(frame);
        if (pair.ok() || m_software) {
            return pair;
        }
        // A hardware failure: software can still decode the frame.
        PluginLog::debug("steady: hardware decode of frame {} failed ({}); retrying in software", frame,
                         pair.error().message);
        m_reader.reset();
        OSV_TRY(open(true));
        return m_reader->read(frame);
    }

private:
    [[nodiscard]] Status open(bool softwareOnly) {
        Error last{ErrorCode::Decoder, "no decoder could be opened"};
        for (const video::HwAccel hw : {video::kHostFrameHwAccel, video::HwAccel::None}) {
            if (softwareOnly && hw != video::HwAccel::None) {
                continue;
            }
            // Container samples first (frame index == sample index, exactly
            // the importer's own reader), libavformat second.
            for (const bool samples : {m_containerSamples, false}) {
                video::DecoderOptions opt;
                opt.hw = hw;
                opt.threads = hw == video::HwAccel::None ? 0 : 2;
                opt.keepOnDevice = false;  // the band analyses read host planes
                opt.useContainerSamples = samples;
                opt.shareHwDevice = true;
                opt.deferFirstFrame = true;
                auto reader = video::DualStreamReader::open(m_path, m_format, opt);
                if (reader.ok()) {
                    m_reader = std::make_unique<video::DualStreamReader>(std::move(reader).value());
                    m_software = hw == video::HwAccel::None;
                    return okStatus();
                }
                last = reader.error();
                if (!samples) {
                    break;
                }
            }
        }
        return last;
    }

    std::filesystem::path m_path;
    meta::FormatInfo m_format;
    bool m_containerSamples = false;
    bool m_software = false;
    std::unique_ptr<video::DualStreamReader> m_reader;
};

/// Threads of the job's private pool: a background task should not take the
/// whole machine from the frame renders it exists to stay out of the way of.
[[nodiscard]] unsigned jobThreads() noexcept {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw / 4u, 2u, 8u);
}

/// "0, 8, 16" for the log.
[[nodiscard]] std::string frameList(const std::vector<std::uint32_t>& frames) {
    std::string s;
    for (const std::uint32_t f : frames) {
        s += (s.empty() ? "" : ", ") + std::to_string(f);
    }
    return s;
}

/// The identity key of a request (no filesystem access: cheap per frame).
[[nodiscard]] std::string requestKey(const SteadyRequest& r) {
    Hasher h;
    const std::u8string u8 = r.path.u8string();
    h.bytes(u8.data(), u8.size());
    h.u64(r.frameCount);
    h.u64(r.syncFrames.size());
    for (const std::uint32_t s : r.syncFrames) {
        h.u64(s);
    }
    hashRig(h, r.baseRig);
    hashBlend(h, r.blend);
    h.u64(r.wantRotation ? 1u : 0u);
    h.u64(r.wantClip ? 1u : 0u);
    h.u64(r.containerSamples ? 1u : 0u);
    hashClipParams(h, r.clip);
    return hex64(h.value());
}

/// The rig and the parameters the clip correction is measured with, given
/// the rotation verdict.
void clipRigAndParams(const SteadyRequest& r, const std::optional<LensAlignVerdict>& rotation, geom::LensRig& rig,
                      render::ClipSteadyParams& params) {
    rig = r.baseRig;
    params = r.clip;
    if (r.wantRotation && rotation && rotation->accepted && render::applyLensRotation(rig, rotation->wRad).ok()) {
        params.parallax.requiredImprovement = render::kAlignedRequiredImprovement;
    }
}

}  // namespace

// =============================================================================
//  Rotation cache lookup (rebuildRig)
// =============================================================================
std::optional<LensAlignVerdict> cachedLensAlign(const std::filesystem::path& path,
                                                const geom::LensRig& baseRig) noexcept {
    try {
        if (!sharedCacheEnabled()) {
            return std::nullopt;  // every instance measures its own (diagnostics)
        }
        const std::optional<FileIdentity> id = identityOf(path);
        if (!id) {
            return std::nullopt;
        }
        const std::string key = rotationKey(*id, baseRig);
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.diskLoaded) {
            loadDisk(g);
        }
        const auto it = g.rotations.find(key);
        if (it == g.rotations.end()) {
            return std::nullopt;
        }
        LensAlignVerdict v = it->second;
        v.fromCache = true;
        v.millis = 0.0;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

// =============================================================================
//  The stage
// =============================================================================
SteadyStage::~SteadyStage() { stop(); }

template <class Fn>
void SteadyStage::publish(std::uint64_t gen, Fn&& fn) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (gen != m_generation.load(std::memory_order_acquire) || !m_request) {
            return;  // measured for settings that are no longer in force
        }
        fn(m_state);
        m_state.serial = m_serial.load(std::memory_order_relaxed) + 1u;
        m_serial.store(m_state.serial, std::memory_order_release);
    }
    m_settledCv.notify_all();
}

bool SteadyStage::stale(std::uint64_t gen) const noexcept {
    return m_stopFlag.load(std::memory_order_acquire) || gen != m_generation.load(std::memory_order_acquire);
}

void SteadyStage::request(const SteadyRequest& req, const std::string& clipName) {
    if (!req.wantRotation && !req.wantClip) {
        return;  // nothing per clip was asked for
    }
    const std::string key = requestKey(req);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_request && key == m_key) {
            return;  // the common case: every frame of a clip asks the same
        }
    }

    // ---- what the process-wide caches already hold ---------------------------------
    // Looked up WITHOUT the stage lock (never both locks at once).
    std::optional<LensAlignVerdict> rotation;
    bool rotationKnown = !req.wantRotation;
    if (req.wantRotation) {
        rotation = cachedLensAlign(req.path, req.baseRig);
        rotationKnown = rotation.has_value();
    }
    std::shared_ptr<const render::ClipSteady> clip;
    if (req.wantClip && rotationKnown && sharedCacheEnabled()) {
        if (const std::optional<FileIdentity> id = identityOf(req.path)) {
            geom::LensRig rig;
            render::ClipSteadyParams params;
            clipRigAndParams(req, rotation, rig, params);
            const std::vector<std::uint32_t> frames =
                render::clipSampleFrames(req.frameCount, req.syncFrames, render::kClipSteadySamples);
            const std::string ck = clipKey(*id, rig, req.blend, frames, params);
            Global& g = global();
            std::lock_guard<std::mutex> lock(g.mutex);
            if (const auto it = g.clips.find(ck); it != g.clips.end()) {
                it->second.lastUse = ++g.useCounter;
                clip = it->second.clip;
            }
        }
    }

    // ---- install it -------------------------------------------------------------------
    bool startWorker = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request = req;
        m_key = key;
        m_clipName = clipName;
        m_generation.fetch_add(1u, std::memory_order_acq_rel);
        m_requestedAt = Clock::now();
        m_state = Snapshot{};
        m_state.active = true;
        if (req.wantRotation && rotation) {
            m_state.rotationSettled = true;
            m_state.rotation = rotation;
        }
        if (req.wantClip && clip) {
            m_state.clipSettled = true;
            m_state.clip = clip;
        }
        m_state.serial = m_serial.load(std::memory_order_relaxed) + 1u;
        m_serial.store(m_state.serial, std::memory_order_release);
        // Work only for what is missing.
        m_pending = (req.wantRotation && !m_state.rotationSettled) || (req.wantClip && !m_state.clipSettled);
        startWorker = m_pending && !m_worker.joinable();
    }
    if (clip) {
        PluginLog::info("steady: '{}': clip correction ready from the cache ({} sample frames, grid {}, seam {}); {}",
                        clipName, clip->frames.size(), clip->grid ? "yes" : "no", clip->seam ? "yes" : "no",
                        render::describeSteadyDecision(clip->decision));
    }
    // Started under no lock but always from the instance thread, which also
    // is the only caller of stop(): start and join never race.
    if (startWorker) {
        try {
            m_worker = std::thread(&SteadyStage::workerLoop, this);
        } catch (const std::exception& e) {
            PluginLog::warn("steady: '{}': could not start the analysis worker ({}); frames render with the "
                            "per-moment corrections",
                            clipName, e.what());
            publish(m_generation.load(), [&](Snapshot& s) {
                s.failure = "no worker thread";
                s.rotationSettled = true;
                s.clipSettled = true;
            });
            return;
        }
    }
    m_workCv.notify_one();
    m_settledCv.notify_all();
}

SteadyStage::Snapshot SteadyStage::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool SteadyStage::waitSettled(bool clip, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto settled = [&] {
        if (m_stopFlag.load(std::memory_order_acquire) || !m_request || !m_state.active) {
            return true;  // nothing to wait for
        }
        const bool rotationDone = !m_request->wantRotation || m_state.rotationSettled;
        const bool clipDone = !clip || !m_request->wantClip || m_state.clipSettled;
        return rotationDone && clipDone;
    };
    return m_settledCv.wait_for(lock, timeout, settled);
}

void SteadyStage::reset() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_request.reset();
        m_key.clear();
        m_generation.fetch_add(1u, std::memory_order_acq_rel);
        m_pending = false;
        m_state = Snapshot{};
        m_state.serial = m_serial.load(std::memory_order_relaxed) + 1u;
        m_serial.store(m_state.serial, std::memory_order_release);
    }
    m_settledCv.notify_all();
    // The process-wide waits poll their cancellation; wake them now.
    global().cv.notify_all();
}

void SteadyStage::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopFlag.store(true, std::memory_order_release);
        m_pending = false;
    }
    m_workCv.notify_all();
    m_settledCv.notify_all();
    global().cv.notify_all();
    if (m_worker.joinable()) {
        try {
            m_worker.join();
        } catch (...) {
            // join() throws only for a thread that is not joinable or is the
            // calling thread; neither can happen, and a noexcept function
            // must not let it escape if the library disagrees.
        }
    }
    // Ready for a fresh request after a quiet: the published state is
    // dropped too, so a new request starts from the caches.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopFlag.store(false, std::memory_order_release);
    m_request.reset();
    m_key.clear();
    m_generation.fetch_add(1u, std::memory_order_acq_rel);
    m_state = Snapshot{};
    m_state.serial = m_serial.load(std::memory_order_relaxed) + 1u;
    m_serial.store(m_state.serial, std::memory_order_release);
}

void SteadyStage::workerLoop() noexcept {
    for (;;) {
        SteadyRequest job;
        std::string key;
        std::string name;
        std::uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workCv.wait(lock, [this] { return m_stopFlag.load(std::memory_order_acquire) || m_pending; });
            if (m_stopFlag.load(std::memory_order_acquire)) {
                return;
            }
            m_pending = false;
            if (!m_request) {
                continue;
            }
            job = *m_request;
            key = m_key;
            name = m_clipName;
            gen = m_generation.load(std::memory_order_acquire);
        }
        try {
            runJob(job, key, gen, name);
        } catch (const std::exception& e) {
            // Allocation failure is the realistic case: settle the request as
            // failed so no Exact frame waits for it.
            PluginLog::warn("steady: '{}': the per-clip analysis failed ({}); frames render with the per-moment "
                            "corrections",
                            name, e.what());
            publish(gen, [&](Snapshot& s) {
                s.failure = e.what();
                s.rotationSettled = true;
                s.clipSettled = true;
            });
        } catch (...) {
            publish(gen, [&](Snapshot& s) {
                s.failure = "unknown exception";
                s.rotationSettled = true;
                s.clipSettled = true;
            });
        }
    }
}

void SteadyStage::runJob(const SteadyRequest& job, const std::string& key, std::uint64_t gen,
                         const std::string& clipName) {
    (void)key;
    const auto tJob = Clock::now();
    const auto cancelled = [this, gen] { return stale(gen); };
    // No identity (the file cannot be inspected) or the shared caches
    // switched off: measure, and share nothing (the empty keys below).
    const std::optional<FileIdentity> id = sharedCacheEnabled() ? identityOf(job.path) : std::nullopt;
    Global& g = global();

    // Opened on the first frame a measurement actually needs, never for a
    // job the caches answer.
    JobReader reader(job.path, job.format, job.containerSamples);
    std::optional<ThreadPool> pool;
    const auto ensurePool = [&]() -> ThreadPool& {
        if (!pool) {
            pool.emplace(jobThreads());
        }
        return *pool;
    };
    const render::ClipFrameSource source = [&reader](std::uint32_t f) { return reader.read(f); };

    // ---- 1. the lens rotation -------------------------------------------------------
    std::optional<LensAlignVerdict> rotation;
    if (job.wantRotation) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (gen == m_generation.load() && m_state.rotationSettled) {
                rotation = m_state.rotation;  // served from the cache at request time
            }
        }
        const std::string rk = id ? rotationKey(*id, job.baseRig) : std::string();
        bool produce = !rotation.has_value();
        // ---- the process-wide cache, or another instance's measurement -----------
        while (produce && !rk.empty()) {
            std::unique_lock<std::mutex> lock(g.mutex);
            if (!g.diskLoaded) {
                loadDisk(g);
            }
            if (const auto it = g.rotations.find(rk); it != g.rotations.end()) {
                rotation = it->second;
                rotation->fromCache = true;
                produce = false;
                break;
            }
            if (g.rotationsInFlight.count(rk) == 0) {
                g.rotationsInFlight.insert(rk);  // this worker measures it
                break;
            }
            // Another instance is measuring this very rotation: wait for it,
            // polling this job's own cancellation.
            g.cv.wait_for(lock, std::chrono::milliseconds(50));
            if (cancelled()) {
                return;
            }
        }
        if (produce) {
            // ---- measure: 3 fixed frames through the calibration rig -----------------
            const std::vector<std::uint32_t> frames =
                render::clipSampleFrames(job.frameCount, job.syncFrames, render::kLensRotationSamples, 0.1, 0.9);
            // The classical solver, on the GPU when the analyses are installed
            // (bit-identical either way): the verdict is cached on disk and
            // must not depend on whether a neural model happens to be present.
            render::ParallaxWarpParams rp;
            rp.backend = render::FlowBackendKind::ClassicalCuda;
            const auto t0 = Clock::now();
            auto measured = render::measureLensRotation(job.baseRig, job.blend, frames, source, rp,
                                                        render::LensRotationParams{}, ensurePool(), cancelled);
            const double ms = msSince(t0);
            std::optional<LensAlignVerdict> verdict;
            if (measured.ok()) {
                const render::LensRotationMeasurement& m = measured.value();
                LensAlignVerdict v;
                v.accepted = m.accepted;
                v.millis = ms;
                if (m.accepted) {
                    v.wRad = m.fit.wRad;
                    v.angleDeg = m.fit.angleDeg;
                    v.residualDeg = m.fit.residualRmsDeg;
                    v.summary = render::describeLensRotation(m.fit);
                } else {
                    v.summary = "no rotation the flow agrees on: " + m.reason;
                }
                verdict = v;
                PluginLog::info("lens alignment: '{}': {} - measured in {:.0f} ms (decode {:.0f}) on frames {}",
                                clipName, v.accepted ? v.summary : "keeping the calibration; " + v.summary, ms,
                                m.decodeMs, frameList(frames));
            } else if (!cancelled()) {
                PluginLog::warn("lens alignment: '{}': could not be measured ({}); keeping the calibration",
                                clipName, measured.error().message);
            }
            // ---- remember it (a failure or a cancellation is not a verdict) ---------
            if (!rk.empty()) {
                {
                    std::lock_guard<std::mutex> lock(g.mutex);
                    g.rotationsInFlight.erase(rk);
                    if (verdict) {
                        g.rotations[rk] = *verdict;
                        appendDisk(rk, *verdict);
                    }
                }
                g.cv.notify_all();
            }
            if (cancelled()) {
                return;
            }
            rotation = verdict;
            const std::string failure = measured.ok() ? std::string() : measured.error().message;
            publish(gen, [&](Snapshot& s) {
                s.rotationSettled = true;
                s.rotation = rotation;
                if (!failure.empty()) {
                    s.failure = "lens alignment: " + failure;
                }
            });
        } else {
            publish(gen, [&](Snapshot& s) {
                s.rotationSettled = true;
                s.rotation = rotation;
            });
        }
    }
    if (cancelled()) {
        return;
    }

    // ---- 2. the clip correction -----------------------------------------------------
    if (!job.wantClip) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (gen == m_generation.load() && m_state.clipSettled) {
            return;  // served from the cache at request time
        }
    }
    geom::LensRig rig;
    render::ClipSteadyParams params;
    clipRigAndParams(job, rotation, rig, params);
    const std::vector<std::uint32_t> frames =
        render::clipSampleFrames(job.frameCount, job.syncFrames, render::kClipSteadySamples);
    const std::string ck = id ? clipKey(*id, rig, job.blend, frames, params) : std::string();
    std::shared_ptr<const render::ClipSteady> clip;
    bool produce = true;
    while (!ck.empty()) {
        std::unique_lock<std::mutex> lock(g.mutex);
        if (const auto it = g.clips.find(ck); it != g.clips.end()) {
            it->second.lastUse = ++g.useCounter;
            clip = it->second.clip;
            produce = false;
            break;
        }
        if (g.clipsInFlight.count(ck) == 0) {
            g.clipsInFlight.insert(ck);
            break;
        }
        g.cv.wait_for(lock, std::chrono::milliseconds(50));
        if (cancelled()) {
            return;
        }
    }
    std::string failure;
    if (produce) {
        // ---- measure: the fixed sample frames through the (aligned) rig -------------
        // Each sample grid is published the moment it exists, so Interactive
        // frames have a stand-in long before the whole correction is ready.
        const auto onSample = [&](std::uint32_t frame, std::shared_ptr<const render::ParallaxWarpGrid> grid) {
            publish(gen, [&](Snapshot& s) { s.samples.emplace_back(frame, std::move(grid)); });
        };
        auto measured = render::measureClipSteady(rig, job.blend, frames, source, params, ensurePool(), onSample,
                                                  cancelled);
        if (measured.ok()) {
            clip = std::make_shared<const render::ClipSteady>(std::move(measured).value());
        } else if (!cancelled()) {
            failure = measured.error().message;
            PluginLog::warn("steady: '{}': the clip correction could not be measured ({}); frames render with the "
                            "per-moment corrections",
                            clipName, failure);
        }
        // ---- remember it ------------------------------------------------------------
        if (!ck.empty()) {
            {
                std::lock_guard<std::mutex> lock(g.mutex);
                g.clipsInFlight.erase(ck);
                if (clip) {
                    g.clips[ck] = CachedClip{clip, ++g.useCounter};
                    // Least recently used first out.
                    while (g.clips.size() > kMaxCachedClips) {
                        auto victim = g.clips.begin();
                        for (auto it = g.clips.begin(); it != g.clips.end(); ++it) {
                            if (it->second.lastUse < victim->second.lastUse) {
                                victim = it;
                            }
                        }
                        g.clips.erase(victim);
                    }
                }
            }
            g.cv.notify_all();
        }
        if (cancelled()) {
            return;
        }
    }
    if (clip) {
        const double sinceRequest = [&] {
            std::lock_guard<std::mutex> lock(m_mutex);
            return msSince(m_requestedAt);
        }();
        PluginLog::info("steady: '{}': clip correction ready {:.0f} ms after the first request ({}; {} sample frames "
                        "{}, {} grids accepted; decode {:.0f} / measure {:.0f} / finish {:.0f} ms): grid {}, seam "
                        "table {}, seam {}; Auto: {}",
                        clipName, sinceRequest, produce ? std::format("measured in {:.0f} ms", msSince(tJob))
                                                        : std::string("another instance measured it"),
                        clip->frames.size(), frameList(clip->frames), clip->acceptedGrids, clip->decodeMs,
                        clip->measureMs, clip->finishMs, clip->grid ? "yes" : "no", clip->seamTable ? "yes" : "no",
                        clip->seam ? "yes" : "no", render::describeSteadyDecision(clip->decision));
    }
    publish(gen, [&](Snapshot& s) {
        s.clipSettled = true;
        s.clip = clip;
        if (!failure.empty()) {
            s.failure = "steady: " + failure;
        }
    });
}

}  // namespace osv::premiere
