// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DualStreamReader: two HevcStreamDecoders driven in parallel (one on a
// helper thread, one on the calling thread) with a presentation-time check,
// or one side-by-side decoder split into two half-width views.

#include "osv/video/DualStreamReader.h"

#include "FileIdentity.h"
#include "osv/core/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace osv::video {

// =============================================================================
//  Helpers
// =============================================================================
namespace {

/// @brief True when the environment variable `name` is an explicit yes.
///
/// "1", "true", "yes" or "on" in any case, as the importer's own switches
/// read them (importerGpuDecodeDisabledByEnvironment): anything else - unset,
/// "0", garbage - is no, so a stray value never turns a diagnostic on.
/// std::getenv reads the C runtime's copy, which the plug-ins share with the
/// host process (/MD) and which _putenv_s in a test updates.
[[nodiscard]] bool environmentSwitchOn(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const char c = value[0];
    const char d = value[1];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' ||
           ((c == 'o' || c == 'O') && (d == 'n' || d == 'N'));
}

/// @brief A path as UTF-8 text for a log line or an error message.
///
/// path::string() converts through the ANSI code page on Windows and throws
/// on a character it cannot map - a non-ASCII user profile under
/// %LOCALAPPDATA%, a renamed clip.  Inside the verifier that exception would
/// be swallowed by verifyAgainstSoftware()'s catch, silently skipping the
/// check or, after a mismatch, losing the software replacement.  The UTF-8
/// form exists for every path; "?" only when even that cannot be built.
[[nodiscard]] std::string pathText(const std::filesystem::path& p) noexcept {
    try {
        const std::u8string text = p.u8string();
        return std::string(text.begin(), text.end());
    } catch (...) {
        return "?";
    }
}

/// Mismatch dump pairs written by every verifier of this module so far.  The
/// cap (ShadowDecodeVerifier::kMaxDumps) bounds the dump folder for the whole
/// session: a reader that is reopened (a pool miss, a new instance of the
/// clip) gets a new verifier, and a per-verifier count would start again.
std::atomic<std::uint32_t> g_dumpPairsWritten{0};

/// @brief Make a clip name usable in a file name: [A-Za-z0-9._-] kept, the
/// rest replaced by '_', at most 80 characters.
[[nodiscard]] std::string fileSafeName(std::string_view name) {
    std::string out;
    out.reserve(std::min<std::size_t>(name.size(), 80u));
    for (const char ch : name) {
        if (out.size() >= 80u) {
            break;
        }
        const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                          ch == '.' || ch == '_' || ch == '-';
        out.push_back(keep ? ch : '_');
    }
    return out.empty() ? std::string("clip") : out;
}

/// @brief Write the luma plane of `frame` as a 16-bit binary PGM.
///
/// P5 with maxval 1023: the samples on their 10-bit scale (>> bitShift),
/// two bytes each, most significant first as the format requires above 255.
/// Any image viewer or `python -c "import imageio"` opens it, so the two
/// dumps of a mismatch can be compared outside the plug-in.
///
/// @param file   Destination (its folder must exist).
/// @param frame  A valid host picture.
/// @return okStatus, or Io when the file cannot be written.
Status writeLumaPgm(const std::filesystem::path& file, const PlanarFrame16& frame) {
    if (!frame.valid() || frame.bitShift > 15) {
        return failStatus(ErrorCode::InvalidArgument, "writeLumaPgm: not a host picture");
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failStatus(ErrorCode::Io, "cannot create " + pathText(file));
    }
    out << "P5\n" << frame.width << ' ' << frame.height << "\n1023\n";
    // One row of big-endian samples at a time.
    std::vector<unsigned char> row(static_cast<std::size_t>(frame.width) * 2u);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint16_t* src = frame.plane[0] + static_cast<std::size_t>(y) * frame.strideElems[0];
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::uint16_t v = static_cast<std::uint16_t>(src[x] >> frame.bitShift);
            row[static_cast<std::size_t>(x) * 2u] = static_cast<unsigned char>(v >> 8);
            row[static_cast<std::size_t>(x) * 2u + 1u] = static_cast<unsigned char>(v & 0xFFu);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    if (!out) {
        return failStatus(ErrorCode::Io, "cannot write " + pathText(file));
    }
    return okStatus();
}

}  // namespace

// =============================================================================
//  compareDecodedLuma / defaultDecodeMismatchDirectory
// =============================================================================
LumaComparison compareDecodedLuma(const PlanarFrame16& a, const PlanarFrame16& b) noexcept {
    LumaComparison out;
    // ---- comparable at all? ---------------------------------------------------
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height || a.bitShift > 15 ||
        b.bitShift > 15) {
        return out;
    }
    out.comparable = true;
    out.height = a.height;
    out.firstBadRow = a.height;

    // ---- row by row, on the 10-bit scale ---------------------------------------
    // Integer sums per row (at most 1023^2 * 32768 < 2^35, so 64 bits are
    // plenty), so the verdict never depends on floating-point order.
    std::uint64_t sse = 0;
    const std::uint32_t half = a.height / 2u;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        const std::uint16_t* pa = a.plane[0] + static_cast<std::size_t>(y) * a.strideElems[0];
        const std::uint16_t* pb = b.plane[0] + static_cast<std::size_t>(y) * b.strideElems[0];
        std::uint64_t rowSse = 0;
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const int d = static_cast<int>(pa[x] >> a.bitShift) - static_cast<int>(pb[x] >> b.bitShift);
            rowSse += static_cast<std::uint64_t>(d * d);
        }
        sse += rowSse;
        // A row is bad when ITS mean error alone fails the 60 dB bar.
        const double rowMse = static_cast<double>(rowSse) / static_cast<double>(a.width);
        if (rowMse > kDecodeBadRowMse) {
            if (out.badRows == 0) {
                out.firstBadRow = y;
            }
            out.lastBadRow = y;
            ++out.badRows;
            if (y >= half) {
                ++out.badRowsBottom;
            }
        }
    }

    // ---- the whole frame --------------------------------------------------------
    const double mse = static_cast<double>(sse) / (static_cast<double>(a.width) * static_cast<double>(a.height));
    out.psnrDb = mse > 0.0 ? 10.0 * std::log10(1023.0 * 1023.0 / mse) : std::numeric_limits<double>::infinity();
    return out;
}

std::filesystem::path defaultDecodeMismatchDirectory() {
    try {
#if defined(_WIN32)
        // _wdupenv_s: a user name with non-ASCII characters must survive, and
        // the copy is ours (the pointer _wgetenv returns is the CRT's).
        wchar_t* value = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") == 0 && value != nullptr) {
            std::filesystem::path base(value);
            std::free(value);
            if (!base.empty()) {
                return base / L"OpenOSV" / L"decode-mismatch";
            }
        }
#elif defined(__APPLE__)
        // Beside the plug-ins' logs (PluginLogPosix.cpp: ~/Library/Logs/OpenOSV).
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::filesystem::path(home) / "Library" / "Logs" / "OpenOSV" / "decode-mismatch";
        }
#endif
        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        if (!ec && !temp.empty()) {
            return temp / "OpenOSV" / "decode-mismatch";
        }
    } catch (...) {
        // Allocation failure building a path: no folder.
    }
    return {};
}

// =============================================================================
//  ShadowDecodeVerifier
// =============================================================================
struct ShadowDecodeVerifier::Impl {
    HevcStreamDecoder reference;            ///< Software decoder of the reference file.
    std::filesystem::path dumpDirectory;    ///< Empty = never dump.
    std::uint64_t checked = 0;              ///< Pictures compared.
    std::uint64_t mismatched = 0;           ///< Of those, how many differed.
    std::optional<std::uint32_t> lastDumpedSync;  ///< The GOP (its sync sample) of the last dump.

    /// @brief Dump both luma planes of a mismatch, if this one should be.
    ///
    /// Only the first mismatch after each sync sample - the onset of a
    /// damaged run, which is what says where it started - and at most
    /// kMaxDumps pairs per process (g_dumpPairsWritten), however many readers
    /// and verifiers the session opens.  Failures are logged, never returned:
    /// a dump is a convenience on top of the warning.
    void maybeDump(Verdict& verdict, std::uint32_t index, const PlanarFrame16& primary, const PlanarFrame16& ref,
                   const std::optional<DecodedFrameInfo>& info, std::uint32_t sync, std::string_view clipName) {
        if (dumpDirectory.empty() || (lastDumpedSync && *lastDumpedSync == sync)) {
            return;
        }
        // ---- reserve one of the session's pairs --------------------------------------
        // A compare-exchange, so two verifiers on two readers can never both
        // take the last slot.  Given back below when nothing was written.
        std::uint32_t used = g_dumpPairsWritten.load(std::memory_order_relaxed);
        do {
            if (used >= kMaxDumps) {
                return;
            }
        } while (!g_dumpPairsWritten.compare_exchange_weak(used, used + 1u, std::memory_order_relaxed));
        std::error_code ec;
        std::filesystem::create_directories(dumpDirectory, ec);
        if (ec) {
            g_dumpPairsWritten.fetch_sub(1u, std::memory_order_relaxed);
            log::warn("video: decode check: cannot create {} ({})", log::safe(pathText(dumpDirectory)), ec.message());
            return;
        }
        // <clip>_f<index>_<path>.pgm and <clip>_f<index>_software.pgm.
        const std::string stem = fileSafeName(clipName) + "_f" + std::to_string(index);
        const std::string primaryPath = (info && info->hw != HwAccel::None) ? hwAccelName(info->hw) : "primary";
        const std::filesystem::path a = dumpDirectory / (stem + "_" + primaryPath + ".pgm");
        const std::filesystem::path b = dumpDirectory / (stem + "_software.pgm");
        const Status wa = writeLumaPgm(a, primary);
        const Status wb = writeLumaPgm(b, ref);
        if (!wa.ok() || !wb.ok()) {
            g_dumpPairsWritten.fetch_sub(1u, std::memory_order_relaxed);  // nothing usable written
            log::warn("video: decode check: could not dump frame {} ({})", index,
                      !wa.ok() ? wa.error().message : wb.error().message);
            return;
        }
        lastDumpedSync = sync;
        verdict.dumpedPrimary = a;
        verdict.dumpedReference = b;
    }
};

ShadowDecodeVerifier::ShadowDecodeVerifier() = default;
ShadowDecodeVerifier::~ShadowDecodeVerifier() = default;
ShadowDecodeVerifier::ShadowDecodeVerifier(ShadowDecodeVerifier&& other) noexcept = default;
ShadowDecodeVerifier& ShadowDecodeVerifier::operator=(ShadowDecodeVerifier&& other) noexcept = default;

Result<ShadowDecodeVerifier> ShadowDecodeVerifier::open(const std::filesystem::path& reference, std::uint32_t trackId,
                                                        const DecoderOptions& primaryOptions,
                                                        const std::filesystem::path& dumpDirectory) {
    if (reference.empty()) {
        return Error{ErrorCode::InvalidArgument, "ShadowDecodeVerifier: empty reference path"};
    }
    if (trackId == 0) {
        return Error{ErrorCode::InvalidArgument, "ShadowDecodeVerifier: track ids are 1-based"};
    }
    // ---- the reference decoder ---------------------------------------------------
    // Software, whatever the primary runs on: the point is an independent
    // decode.  The same sample feed as the primary (so frame i means the
    // same sample in both), two frame threads (a third of the CPU a default
    // software decoder would take; the proxy decodes in 1.3-3.5 ms a frame),
    // no device of any kind, and no frame-0 probe at open.
    DecoderOptions options;
    options.hw = HwAccel::None;
    options.threads = 2;
    options.keepOnDevice = false;
    options.useContainerSamples = primaryOptions.useContainerSamples;
    options.shareHwDevice = false;
    options.deferFirstFrame = true;
    ShadowDecodeVerifier verifier;
    verifier.m_impl = std::make_unique<Impl>();
    OSV_TRY_ASSIGN(verifier.m_impl->reference, HevcStreamDecoder::open(reference, trackId, options));
    verifier.m_impl->dumpDirectory = dumpDirectory;
    return verifier;
}

Result<ShadowDecodeVerifier::Verdict> ShadowDecodeVerifier::check(std::uint32_t index, const PlanarFrame16& primary,
                                                                  const std::optional<DecodedFrameInfo>& primaryInfo,
                                                                  std::optional<std::uint32_t> previousSync,
                                                                  std::string_view clipName) {
    // ---- inputs ------------------------------------------------------------------
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "ShadowDecodeVerifier: not open"};
    }
    if (!primary.valid()) {
        return Error{ErrorCode::InvalidArgument, "ShadowDecodeVerifier: the primary picture is not a host picture"};
    }
    Impl& s = *m_impl;

    // ---- the reference picture ------------------------------------------------------
    auto ref = s.reference.decodeFrame(index);
    if (!ref.ok()) {
        return Error{ref.error().code, "the software reference could not decode frame " + std::to_string(index) +
                                           ": " + ref.error().message};
    }

    // ---- compare --------------------------------------------------------------------
    Verdict verdict;
    verdict.comparison = compareDecodedLuma(primary, ref.value());
    ++s.checked;
    const LumaComparison& c = verdict.comparison;
    const std::string clip = log::safe(clipName);
    if (!c.comparable) {
        // Different sizes: the reader's own width check rejects such a
        // picture anyway; say so and leave the decision to it.
        log::warn("video: decode check: '{}' frame {}: the primary picture ({}x{}) and the software one ({}x{}) "
                  "cannot be compared",
                  clip, index, primary.width, primary.height, ref.value().width, ref.value().height);
        return verdict;
    }
    if (!c.mismatch()) {
        // A heartbeat every 100 pictures, so a log shows the check is running.
        if (s.checked % 100u == 0u) {
            log::debug("video: decode check: '{}' {} pictures checked against software, {} mismatched", clip,
                       s.checked, s.mismatched);
        }
        return verdict;
    }

    // ---- a mismatch: dump, say everything, hand back the software picture -----------
    ++s.mismatched;
    const std::uint32_t sync = previousSync.value_or(index);
    s.maybeDump(verdict, index, primary, ref.value(), primaryInfo, sync, clipName);
    const DecodedFrameInfo info = primaryInfo.value_or(DecodedFrameInfo{});
    // What the SOFTWARE decoder said about this GOP decides what the mismatch
    // means: a reference that decoded every picture up to here cleanly makes
    // it a fault of the hardware decode; one that saw damage - on this
    // picture, or on one it is predicted from (gopDamagedAt) - means the
    // recording is damaged, and two decoders simply conceal it differently
    // (libavcodec's own concealment even varies with its thread count).
    const std::optional<DecodedFrameInfo> refInfo = s.reference.lastFrameInfo();
    const bool refDamaged = refInfo && refInfo->gopDamagedAt >= 0;
    const std::string refVerdict =
        refDamaged ? std::format("the software decode saw damage too, at frame {} (this picture: decode_error_flags "
                                 "0x{:x}, corrupt {}): the recording is damaged here and the two decoders conceal it "
                                 "differently",
                                 refInfo->gopDamagedAt, static_cast<unsigned>(refInfo->decodeErrorFlags),
                                 refInfo->corrupt ? "yes" : "no")
                   : std::string("the software decode is clean: the hardware decode is at fault");
    log::warn("video: decode check: '{}' frame {} (sync {} + {}) differs from software: luma PSNR {:.1f} dB, "
              "{} bad rows of {} ({} in the bottom half), first bad row {}, last {}; decoded on {}, "
              "decode_error_flags 0x{:x}, corrupt {}, key {}, surface {}; {}; serving the software picture{}",
              clip, index, sync, index >= sync ? index - sync : 0u, c.psnrDb, c.badRows, c.height, c.badRowsBottom,
              c.firstBadRow, c.lastBadRow, hwAccelName(info.hw), static_cast<unsigned>(info.decodeErrorFlags),
              info.corrupt ? "yes" : "no", info.keyFrame ? 1 : 0, info.surface, refVerdict,
              verdict.dumpedPrimary.empty() ? std::string()
                                            : "; luma planes dumped to " + log::safe(pathText(verdict.dumpedPrimary)) +
                                                  " and " + log::safe(pathText(verdict.dumpedReference)));
    verdict.replacement = std::move(ref).value();
    return verdict;
}

bool ShadowDecodeVerifier::isOpen() const noexcept { return m_impl != nullptr && m_impl->reference.isOpen(); }

std::uint64_t ShadowDecodeVerifier::checked() const noexcept { return m_impl ? m_impl->checked : 0u; }

std::uint64_t ShadowDecodeVerifier::mismatched() const noexcept { return m_impl ? m_impl->mismatched : 0u; }

// =============================================================================
//  Impl
// =============================================================================
struct DualStreamReader::Impl {
    HevcStreamDecoder decoders[2];   ///< [0] slave, [1] master ([1] unused when side by side).
    bool sideBySide = false;         ///< Single stream split into halves.
    // ---- the proxy's shadow check (OPENOSV_VERIFY_HW_DECODE) -------------------
    bool verifyHw = false;               ///< Asked for when the reader opened (proxy only).
    bool verifierTried = false;          ///< The verifier's open was attempted (never retried).
    ShadowDecodeVerifier verifier;       ///< Opened on the first hardware picture.
    // ---- what open() was given (ReaderPool keys on these) ----------------------
    std::filesystem::path path;                ///< File the reader decodes.
    DecoderOptions options;                    ///< Options exactly as passed to open().
    std::array<std::uint32_t, 2> trackIds{};   ///< Tracks decoded (the proxy track twice).
    std::wstring fileIdentity;                 ///< Path|size|mtime at open (empty = unknown).
    std::uint32_t frameCount = 0;
    std::uint32_t lensWidth = 0;
    std::uint32_t lensHeight = 0;
    double fps = 0.0;
    std::int64_t ptsToleranceUs = 1;  ///< One time-base tick expressed in microseconds (rounded up).
    // ---- frames inside an undecodable run (UndecodableRun) ---------------------
    /// The pair shown for every request inside a run one lens cannot decode,
    /// kept so those requests cost nothing after the first (host pictures
    /// only; a pair on the device would pin a decoder surface).  Dropped as
    /// soon as a request lands outside every run.
    std::optional<FramePair> heldPair;
    std::vector<UndecodableRun> announcedRuns;  ///< Runs already logged by this reader (once each).

    /// Make a PlanarFrame16 that views the left or right half of `whole`
    /// while sharing its backing memory.
    static PlanarFrame16 halfView(const PlanarFrame16& whole, int half) {
        PlanarFrame16 out = whole;
        const std::uint32_t halfW = whole.width / 2;
        out.width = halfW;
        out.chromaW = (halfW + 1) / 2;
        if (half == 1) {
            // Right half: advance the luma pointer by halfW samples and the
            // chroma pointers by the corresponding chroma columns (doubled
            // when Cb/Cr are interleaved because each column is two elements).
            const std::size_t lumaOffset = halfW;
            const std::size_t chromaOffset = whole.chromaInterleaved ? static_cast<std::size_t>(whole.chromaW / 2) * 2
                                                                     : static_cast<std::size_t>(whole.chromaW / 2);
            if (out.plane[0]) {
                out.plane[0] = whole.plane[0] + lumaOffset;
            }
            if (out.plane[1]) {
                out.plane[1] = whole.plane[1] + chromaOffset;
            }
            if (out.plane[2]) {
                out.plane[2] = whole.plane[2] + chromaOffset;
            }
        }
        return out;
    }

    /// Frame `index` from one decoder plus its DeviceFrameRef (if any).
    struct LensResult {
        Result<PlanarFrame16> frame = Error{ErrorCode::Internal, "not decoded"};
        std::optional<DeviceFrameRef> device;
    };

    LensResult decodeLens(int lens, std::uint32_t index) {
        LensResult r;
        r.frame = decoders[lens].decodeFrame(index);
        if (r.frame.ok()) {
            r.device = decoders[lens].lastDeviceFrame();
        }
        return r;
    }

    /// @brief The shadow check of one side-by-side picture (verifyHw only).
    ///
    /// Only a picture the HARDWARE decoded is checked - a software primary
    /// has nothing independent to be compared with - and only once the
    /// decoder's own report names this very index.  The verifier opens on
    /// the first such picture (a reader that never decodes on hardware never
    /// pays for it) and is not retried after a failed open.  When the two
    /// decodes differ, `whole` is replaced by the software picture, so the
    /// frame delivered is the right one, and any device view of the
    /// hardware picture is dropped with it.
    ///
    /// @param index  The frame just decoded.
    /// @param whole  The decode result; replaced in place on a mismatch.
    void verifyAgainstSoftware(std::uint32_t index, LensResult& whole) noexcept {
        try {
            if (!whole.frame.ok()) {
                return;
            }
            const HevcStreamDecoder& d = decoders[0];
            const std::optional<DecodedFrameInfo> info = d.lastFrameInfo();
            if (!info || info->hw == HwAccel::None || info->index != index) {
                return;
            }
            const std::string clip = log::safe(pathText(path.filename()));
            // ---- the reference decoder, on the first hardware picture -----------------
            if (!verifier.isOpen()) {
                if (verifierTried) {
                    return;
                }
                verifierTried = true;
                const std::filesystem::path dumps = defaultDecodeMismatchDirectory();
                auto opened = ShadowDecodeVerifier::open(path, trackIds[0], options, dumps);
                if (!opened.ok()) {
                    log::warn("video: decode check: '{}' cannot open the software reference ({}); hardware "
                              "pictures stay unchecked",
                              clip, opened.error().message);
                    return;
                }
                verifier = std::move(opened).value();
                log::info("video: decode check: '{}' every {} picture is compared with a software decode "
                          "(OPENOSV_VERIFY_HW_DECODE); mismatches are logged, replaced and dumped to {}",
                          clip, hwAccelName(info->hw),
                          dumps.empty() ? std::string("nowhere") : log::safe(pathText(dumps)));
            }
            // ---- compare, and deliver the software picture on a mismatch ------------
            auto verdict = verifier.check(index, whole.frame.value(), info, d.previousSyncIndex(index), clip);
            if (!verdict.ok()) {
                log::warn("video: decode check: '{}' frame {} not checked ({})", clip, index, verdict.error().message);
                return;
            }
            if (verdict.value().replacement) {
                whole.frame = std::move(*verdict.value().replacement);
                whole.device.reset();
            }
        } catch (...) {
            // A diagnostic never costs the frame: the primary picture stands.
        }
    }

    /// @brief The frame shown instead of `index` when a lens cannot decode it.
    ///
    /// Asks each lens decoder for an undecodable run it has met around
    /// `index` (HevcStreamDecoder::undecodableRun) and holds the last frame
    /// before the earliest such run (heldFrameFor).  The held frame is
    /// resolved again in case another run covers it, a bounded number of
    /// times, so the answer is never itself a frame no lens can decode.
    /// @param index  The frame asked for.
    /// @param run    Receives the run that decided it (when there is one).
    /// @return The frame to read instead, or std::nullopt when both lenses
    ///         can decode `index` as far as they know.
    [[nodiscard]] std::optional<std::uint32_t> holdFor(std::uint32_t index, UndecodableRun& run) const noexcept {
        std::uint32_t target = index;
        bool held = false;
        // Two lenses, each with runs that could in theory chain: four rounds
        // cover every arrangement the decoders can report.
        for (int round = 0; round < 4; ++round) {
            std::optional<std::uint32_t> next;
            const int lenses = sideBySide ? 1 : 2;
            for (int lens = 0; lens < lenses; ++lens) {
                const std::optional<UndecodableRun> r = decoders[lens].undecodableRun(target);
                if (!r) {
                    continue;
                }
                const std::optional<std::uint32_t> h = heldFrameFor(*r, frameCount);
                if (!h) {
                    return std::nullopt;  // no neighbour exists: the request fails as it is
                }
                if (!next || *h < *next) {
                    next = *h;
                    run = *r;
                }
            }
            if (!next) {
                break;  // `target` decodes on both lenses
            }
            target = *next;
            held = true;
        }
        if (!held) {
            return std::nullopt;
        }
        return target;
    }

    /// @brief Say once per run that its frames are shown as `heldIndex`.
    void announce(const UndecodableRun& run, std::uint32_t heldIndex) noexcept {
        try {
            for (const UndecodableRun& known : announcedRuns) {
                if (known.first == run.first && known.end == run.end) {
                    return;
                }
            }
            announcedRuns.push_back(run);
            log::warn("video: '{}': frames {}-{} cannot be decoded on one lens (they predict from a picture the file "
                      "does not contain); frame {} of both lenses is shown in their place",
                      log::safe(pathText(path.filename())), run.first, run.end - 1u, heldIndex);
        } catch (...) {
            // A log line is never worth a frame.
        }
    }

    /// @brief Decode the pair of `index` from the lens decoders.
    ///
    /// The proxy decodes once and is split; the native lenses decode on two
    /// threads and must agree on the presentation time.  This is the whole
    /// read() without the undecodable-run handling around it.
    /// @param index  Frame to decode (in range; the caller checked).
    /// @return The pair, or the first lens error.
    Result<FramePair> decodePair(std::uint32_t index) {
        FramePair pair;
        pair.index = index;

        if (sideBySide) {
            // ---- decode once, split ----------------------------------------------
            LensResult whole = decodeLens(0, index);
            if (!whole.frame.ok()) {
                return Error(whole.frame.error());
            }
            // ---- the shadow check (OPENOSV_VERIFY_HW_DECODE=1 only) ---------------
            if (verifyHw) {
                verifyAgainstSoftware(index, whole);
            }
            const PlanarFrame16& frame = whole.frame.value();
            if (frame.width != lensWidth * 2) {
                return Error{ErrorCode::Decoder, "side-by-side frame width changed mid-stream"};
            }
            pair.lens[0] = halfView(frame, 0);
            pair.lens[1] = halfView(frame, 1);
            pair.ptsUs = frame.ptsUs;
            // Device halves: same pitch, luma pointer shifted by half a row of
            // samples (2 bytes each for P010, 1 byte for NV12).
            if (whole.device && whole.device->valid()) {
                const std::size_t bytesPerSample =
                    (whole.device->bitShift == 6 || whole.device->bitDepth > 8) ? 2u : 1u;
                const std::size_t byteOffset = static_cast<std::size_t>(lensWidth) * bytesPerSample;
                for (int lens = 0; lens < 2; ++lens) {
                    DeviceFrameRef ref = *whole.device;
                    ref.width = lensWidth;
                    if (lens == 1) {
                        ref.yDevice = static_cast<std::uint8_t*>(ref.yDevice) + byteOffset;
                        ref.uvDevice = static_cast<std::uint8_t*>(ref.uvDevice) + byteOffset;
                    }
                    pair.device[static_cast<std::size_t>(lens)] = ref;
                }
            }
            return pair;
        }

        // ---- two decoders, two threads ----------------------------------------------
        // Lens 0 runs on a helper thread while lens 1 decodes on this one; the
        // helper's result is captured by value and any exception (there should
        // be none, decoders never throw) is converted into an error.
        LensResult slave;
        std::string threadFailure;
        std::thread worker([this, &slave, &threadFailure, index]() noexcept {
            try {
                slave = decodeLens(0, index);
            } catch (const std::exception& e) {
                threadFailure = e.what();
            } catch (...) {
                threadFailure = "unknown exception";
            }
        });
        LensResult master = decodeLens(1, index);
        worker.join();

        if (!threadFailure.empty()) {
            return Error{ErrorCode::Internal, "lens 0 decoder thread failed: " + threadFailure};
        }
        if (!slave.frame.ok()) {
            return Error{slave.frame.error().code, "lens 0: " + slave.frame.error().message};
        }
        if (!master.frame.ok()) {
            return Error{master.frame.error().code, "lens 1: " + master.frame.error().message};
        }
        const PlanarFrame16& s = slave.frame.value();
        const PlanarFrame16& m = master.frame.value();
        // Both tracks are written by the same encoder clock; anything beyond a
        // single tick means the tracks are not the pair we think they are.  The
        // tracks' own timing says so, whichever decoder read them: Timing.
        const std::int64_t delta = s.ptsUs > m.ptsUs ? s.ptsUs - m.ptsUs : m.ptsUs - s.ptsUs;
        if (delta > ptsToleranceUs) {
            return Error{ErrorCode::Timing, "lens presentation times differ at frame " + std::to_string(index) + ": " +
                                                std::to_string(s.ptsUs) + " us vs " + std::to_string(m.ptsUs) + " us"};
        }
        pair.lens[0] = s;
        pair.lens[1] = m;
        pair.ptsUs = s.ptsUs;
        if (slave.device) {
            pair.device[0] = *slave.device;
        }
        if (master.device) {
            pair.device[1] = *master.device;
        }
        return pair;
    }

    /// @brief The pair shown for `index`, which lies inside an undecodable run.
    ///
    /// The held frame's pair (both lenses at one instant), from the cache when
    /// it holds that frame, else decoded once and cached.  The pair carries
    /// its own index, so a caller can see the frame was held.
    /// @param heldIndex  holdFor()'s answer.
    /// @param run        The run that decided it (for the one log line).
    Result<FramePair> heldRead(std::uint32_t heldIndex, const UndecodableRun& run) {
        announce(run, heldIndex);
        if (heldPair && heldPair->index == heldIndex) {
            return *heldPair;
        }
        OSV_TRY_ASSIGN(FramePair pair, decodePair(heldIndex));
        // Host pictures only: a device pair would keep a decoder surface out
        // of its pool for as long as the run is being looked at.
        if (!pair.onDevice()) {
            heldPair = pair;
        } else {
            heldPair.reset();
        }
        return pair;
    }
};

// =============================================================================
//  Lifetime
// =============================================================================
DualStreamReader::DualStreamReader() = default;
DualStreamReader::~DualStreamReader() = default;
DualStreamReader::DualStreamReader(DualStreamReader&& other) noexcept = default;
DualStreamReader& DualStreamReader::operator=(DualStreamReader&& other) noexcept = default;

// =============================================================================
//  open
// =============================================================================
Result<DualStreamReader> DualStreamReader::open(const std::filesystem::path& path, const meta::FormatInfo& format,
                                                const DecoderOptions& options) {
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty path"};
    }
    DualStreamReader reader;
    reader.m_impl = std::make_unique<Impl>();
    Impl& impl = *reader.m_impl;
    impl.sideBySide = format.sideBySideProxy;
    impl.path = path;
    impl.options = options;
    // The file version, taken BEFORE the decoders map it: a reader must be
    // matched to the bytes it was opened on, never to a later rewrite of the
    // same path.  A failure only keeps the reader out of pools.
    {
        auto identity = detail::fileIdentity(path);
        if (identity.ok()) {
            impl.fileIdentity = std::move(identity).value();
        }
    }

    if (impl.sideBySide) {
        // ---- one track, two halves ------------------------------------------
        const std::uint32_t trackId = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
        impl.trackIds = {trackId, trackId};
        OSV_TRY_ASSIGN(impl.decoders[0], HevcStreamDecoder::open(path, trackId, options));
        const HevcStreamDecoder& d = impl.decoders[0];
        if (d.width() < 2 || (d.width() % 2) != 0) {
            return Error{ErrorCode::Malformed, "side-by-side stream width " + std::to_string(d.width()) +
                                                   " cannot be split into two lenses"};
        }
        impl.frameCount = d.frameCount();
        impl.lensWidth = d.width() / 2;
        impl.lensHeight = d.height();
        impl.fps = d.fps();
        const TimeBase tb = d.timeBase();
        impl.ptsToleranceUs = static_cast<std::int64_t>(std::ceil(1e6 * static_cast<double>(tb.num) / static_cast<double>(tb.den)));
        // The shadow check is a diagnostic switch read once per reader: it
        // doubles the proxy's decode work, so it is never on by default.
        impl.verifyHw = environmentSwitchOn("OPENOSV_VERIFY_HW_DECODE");
        return reader;
    }

    // ---- two tracks ----------------------------------------------------------
    for (int lens = 0; lens < 2; ++lens) {
        if (format.videoTrackIds[static_cast<std::size_t>(lens)] == 0) {
            return Error{ErrorCode::InvalidArgument, "FormatInfo has no video track id for lens " + std::to_string(lens)};
        }
    }
    impl.trackIds = {format.videoTrackIds[0], format.videoTrackIds[1]};
    // Each lens gets its own shared-device slot: the two decoders run in
    // parallel on every read(), and FFmpeg serialises the surface downloads
    // of one D3D11 device, so one device per lens keeps them concurrent.
    // Every reader's lens 0 still shares one device (and lens 1 another), so
    // a second reader of any clip creates no device at all.
    std::array<DecoderOptions, 2> lensOptions{options, options};
    for (std::size_t lens = 0; lens < 2; ++lens) {
        lensOptions[lens].hwDeviceSlot = std::max(0, options.hwDeviceSlot) * 2 + static_cast<int>(lens);
    }
    // Both lenses open at the same time (lens 0 on a helper thread), exactly
    // as read() decodes them: whatever open() still costs - a device on the
    // first open of the process, the first-frame probe for callers that keep
    // it - is paid once in wall time instead of twice.
    Result<HevcStreamDecoder> opened0 = Error{ErrorCode::Internal, "lens 0 was not opened"};
    std::string threadFailure;
    try {
        std::thread worker([&opened0, &threadFailure, &path, &format, &lensOptions]() noexcept {
            try {
                opened0 = HevcStreamDecoder::open(path, format.videoTrackIds[0], lensOptions[0]);
            } catch (const std::exception& e) {
                threadFailure = e.what();
            } catch (...) {
                threadFailure = "unknown exception";
            }
        });
        // Nothing may escape between the thread's start and its join, or the
        // std::thread destructor would terminate the process.
        Result<HevcStreamDecoder> opened1 = Error{ErrorCode::Internal, "lens 1 was not opened"};
        try {
            opened1 = HevcStreamDecoder::open(path, format.videoTrackIds[1], lensOptions[1]);
        } catch (const std::exception& e) {
            opened1 = Error{ErrorCode::Internal, std::string("lens 1 open failed: ") + e.what()};
        } catch (...) {
            opened1 = Error{ErrorCode::Internal, "lens 1 open failed: unknown exception"};
        }
        worker.join();
        if (!threadFailure.empty()) {
            return Error{ErrorCode::Internal, "lens 0 open thread failed: " + threadFailure};
        }
        if (!opened0.ok()) {
            return Error(opened0.error());
        }
        if (!opened1.ok()) {
            return Error(opened1.error());
        }
        impl.decoders[0] = std::move(opened0).value();
        impl.decoders[1] = std::move(opened1).value();
    } catch (const std::system_error& e) {
        // std::thread could not start (resource exhaustion): report it rather
        // than let it escape a function that returns Result.
        return Error{ErrorCode::Internal, std::string("cannot start the lens 0 open thread: ") + e.what()};
    }
    const HevcStreamDecoder& a = impl.decoders[0];
    const HevcStreamDecoder& b = impl.decoders[1];
    if (a.width() != b.width() || a.height() != b.height()) {
        return Error{ErrorCode::Malformed, "lens streams differ in size: " + std::to_string(a.width()) + "x" +
                                               std::to_string(a.height()) + " vs " + std::to_string(b.width()) + "x" +
                                               std::to_string(b.height())};
    }
    if (a.frameCount() != b.frameCount()) {
        log::warn("video: lens streams have {} and {} frames; using the smaller count", a.frameCount(), b.frameCount());
    }
    if (std::fabs(a.fps() - b.fps()) > 1e-3) {
        return Error{ErrorCode::Malformed, "lens streams differ in frame rate"};
    }
    impl.frameCount = std::min(a.frameCount(), b.frameCount());
    impl.lensWidth = a.width();
    impl.lensHeight = a.height();
    impl.fps = a.fps();
    // Tolerance: one tick of the coarser time base, in microseconds.
    std::int64_t tol = 1;
    for (const HevcStreamDecoder* d : {&a, &b}) {
        const TimeBase tb = d->timeBase();
        if (tb.num > 0 && tb.den > 0) {
            tol = std::max(tol, static_cast<std::int64_t>(std::ceil(1e6 * static_cast<double>(tb.num) / static_cast<double>(tb.den))));
        }
    }
    impl.ptsToleranceUs = tol;
    return reader;
}

bool DualStreamReader::isOpen() const noexcept { return m_impl != nullptr && m_impl->decoders[0].isOpen(); }

// =============================================================================
//  read
// =============================================================================
Result<FramePair> DualStreamReader::read(std::uint32_t index) {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "reader not open"};
    }
    Impl& impl = *m_impl;
    if (index >= impl.frameCount) {
        return Error{ErrorCode::InvalidArgument, "frame " + std::to_string(index) + " out of range (" +
                                                     std::to_string(impl.frameCount) + " frames)"};
    }

    // ---- a frame inside a run a lens already met ------------------------------
    // Shown as the frame before the run, on both lenses, without decoding
    // anything after the first request (heldRead caches the held pair).
    UndecodableRun run;
    if (const std::optional<std::uint32_t> held = impl.holdFor(index, run)) {
        return impl.heldRead(*held, run);
    }
    // The held frame itself (a host stepping back out of the run asks for
    // it next): the cached pair IS that frame, decoded once already.
    if (impl.heldPair && impl.heldPair->index == index) {
        return *impl.heldPair;
    }
    // Outside every run: the held pair is no longer wanted.
    impl.heldPair.reset();

    // ---- the pair itself ---------------------------------------------------------
    Result<FramePair> pair = impl.decodePair(index);
    if (pair.ok()) {
        return pair;
    }
    // ---- a run met by THIS request ------------------------------------------------
    // The lens decoder that crossed it remembered it while failing, so the
    // same question now has an answer: the request is served the held frame
    // instead of failing.  Any other failure is returned as it is.
    if (const std::optional<std::uint32_t> held = impl.holdFor(index, run)) {
        return impl.heldRead(*held, run);
    }
    return pair;
}

// =============================================================================
//  Accessors
// =============================================================================
const std::filesystem::path& DualStreamReader::path() const noexcept {
    static const std::filesystem::path kEmpty;
    return m_impl ? m_impl->path : kEmpty;
}

DecoderOptions DualStreamReader::options() const noexcept { return m_impl ? m_impl->options : DecoderOptions{}; }

std::array<std::uint32_t, 2> DualStreamReader::trackIds() const noexcept {
    return m_impl ? m_impl->trackIds : std::array<std::uint32_t, 2>{0u, 0u};
}

const std::wstring& DualStreamReader::fileIdentity() const noexcept {
    static const std::wstring kEmpty;
    return m_impl ? m_impl->fileIdentity : kEmpty;
}

std::uint32_t DualStreamReader::frameCount() const noexcept { return m_impl ? m_impl->frameCount : 0; }

double DualStreamReader::fps() const noexcept { return m_impl ? m_impl->fps : 0.0; }

std::uint32_t DualStreamReader::lensWidth() const noexcept { return m_impl ? m_impl->lensWidth : 0; }

std::uint32_t DualStreamReader::lensHeight() const noexcept { return m_impl ? m_impl->lensHeight : 0; }

bool DualStreamReader::isSideBySide() const noexcept { return m_impl != nullptr && m_impl->sideBySide; }

std::uint64_t DualStreamReader::verifiedFrames() const noexcept { return m_impl ? m_impl->verifier.checked() : 0u; }

std::uint64_t DualStreamReader::verifyMismatches() const noexcept {
    return m_impl ? m_impl->verifier.mismatched() : 0u;
}

const HevcStreamDecoder* DualStreamReader::decoder(int lens) const noexcept {
    if (!m_impl || lens < 0 || lens > 1) {
        return nullptr;
    }
    return &m_impl->decoders[m_impl->sideBySide ? 0 : lens];
}

HevcStreamDecoder* DualStreamReader::decoder(int lens) noexcept {
    if (!m_impl || lens < 0 || lens > 1) {
        return nullptr;
    }
    return &m_impl->decoders[m_impl->sideBySide ? 0 : lens];
}

}  // namespace osv::video
