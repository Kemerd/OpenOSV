// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ClipSteady.cpp - the per-clip corrections (steady grid, seam table and
// carved seam), the Auto rule, and the per-clip lens rotation measurement.
// The reasoning behind each piece is in ClipSteady.h.

#include "osv/render/ClipSteady.h"

#include "osv/core/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

namespace osv::render {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t`.
[[nodiscard]] double msSince(Clock::time_point t) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// Median of `v` in place (the mean of the two middle values for an even
/// count).  `v` must not be empty; it is reordered.
[[nodiscard]] float medianInPlace(std::vector<float>& v) noexcept {
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const float hi = v[mid];
    if (v.size() % 2 == 1) {
        return hi;
    }
    const float lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    // Averaged in double so two large equal floats cannot overflow.
    return static_cast<float>(0.5 * (static_cast<double>(lo) + static_cast<double>(hi)));
}

/// Median of a copy of `v`; 0 for an empty input.
[[nodiscard]] double medianOf(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) {
        return hi;
    }
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}

/// A grid as the band code needs to see it (null view for a null grid).
[[nodiscard]] WarpGridView viewOf(const ParallaxWarpGrid* grid) noexcept {
    WarpGridView v;
    if (grid != nullptr && grid->valid()) {
        v.uv = grid->uv.data();
        v.w = grid->w;
        v.h = grid->h;
        v.latMinRad = grid->latMinRad;
        v.latMaxRad = grid->latMaxRad;
    }
    return v;
}

/// Per-sector NCC of the two lenses' luma over co-visible pixels of the
/// judged rows, with the texture test (see SteadyDecisionParams).
struct SectorScore {
    double ncc = 0.0;
    bool judged = false;  ///< Enough pixels and texture in both lenses.
};

[[nodiscard]] std::vector<SectorScore> sectorScores(const LensBands& b, const SteadyDecisionParams& p) {
    std::vector<SectorScore> out(p.sectors);
    const std::size_t n = static_cast<std::size_t>(b.w) * b.h;
    if (b.w == 0 || b.h == 0 || b.mapH == 0 || b.luma[0].size() != n || b.luma[1].size() != n ||
        b.alpha[0].size() != n || b.alpha[1].size() != n) {
        return out;  // nothing judged
    }
    // Per-sector running sums: n, sa, sb, saa, sbb, sab (double: a sector
    // holds a few thousand pixels, so no cancellation issue arises).
    struct Acc {
        double n = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    };
    std::vector<Acc> acc(p.sectors);
    const double radPerRow = kPi / static_cast<double>(b.mapH);
    const double latLimit = deg2rad(p.latHalfDeg);
    for (std::uint32_t r = 0; r < b.h; ++r) {
        // Latitude of the row centre, the band's own convention.
        const double lat = kHalfPi - (static_cast<double>(b.rowOffset) + r + 0.5) * radPerRow;
        if (std::fabs(lat) > latLimit) {
            continue;
        }
        for (std::uint32_t c = 0; c < b.w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * b.w + c;
            if (!(b.alpha[0][i] > 0.5f) || !(b.alpha[1][i] > 0.5f)) {
                continue;
            }
            const double va = b.luma[0][i];
            const double vb = b.luma[1][i];
            if (!std::isfinite(va) || !std::isfinite(vb)) {
                continue;
            }
            const std::size_t s = static_cast<std::size_t>(c) * p.sectors / b.w;
            Acc& a = acc[std::min<std::size_t>(s, p.sectors - 1u)];
            a.n += 1.0;
            a.sa += va;
            a.sb += vb;
            a.saa += va * va;
            a.sbb += vb * vb;
            a.sab += va * vb;
        }
    }
    for (std::uint32_t s = 0; s < p.sectors; ++s) {
        const Acc& a = acc[s];
        if (a.n < static_cast<double>(p.minPixels)) {
            continue;
        }
        const double ma = a.sa / a.n;
        const double mb = a.sb / a.n;
        const double va = std::max(a.saa / a.n - ma * ma, 0.0);
        const double vb = std::max(a.sbb / a.n - mb * mb, 0.0);
        const double cov = a.sab / a.n - ma * mb;
        // Texture in BOTH lenses: a sector one lens sees as flat has no
        // correspondence for any correction to improve or spoil.
        if (std::sqrt(va) < p.minStd || std::sqrt(vb) < p.minStd) {
            continue;
        }
        out[s].ncc = cov / std::sqrt(va * vb);
        out[s].judged = std::isfinite(out[s].ncc);
    }
    return out;
}

/// Whether a correction has anything in it.
[[nodiscard]] bool hasCorrection(const SeamCorrection& c) noexcept {
    return (c.warp != nullptr && c.warp->valid()) || (c.seamShiftDeg != nullptr && !c.seamShiftDeg->empty());
}

/// One decoded sample: the frame actually measured and its pair.
struct DecodedSample {
    std::uint32_t frame = 0;
    video::FramePair pair;
};

/// Decode planned sample `frame`, or - when the decoder refuses it - the first
/// of `alternates(frame)` that decodes.  Every refusal and the substitution
/// (or the skip) is appended to `notes` as one line; `decodeMs` accumulates
/// the time spent in `source`.  nullopt when nothing decoded (the caller
/// skips the sample).  `cancelled` is polled before every substitute; a
/// cancellation sets `stop` and returns nullopt at once, so it still ends the
/// measurement promptly.
[[nodiscard]] std::optional<DecodedSample> decodeSample(std::uint32_t frame, const ClipFrameSource& source,
                                                        const ClipFrameAlternates& alternates,
                                                        std::vector<std::string>& notes, double& decodeMs,
                                                        const ClipCancel& cancelled, bool& stop) {
    stop = false;
    // ---- the planned frame first ------------------------------------------------
    const auto t0 = Clock::now();
    auto pair = source(frame);
    decodeMs += msSince(t0);
    if (pair.ok()) {
        return DecodedSample{frame, std::move(pair).value()};
    }
    const std::string why = pair.error().message;

    // ---- then its substitutes, nearest first ---------------------------------------
    std::vector<std::uint32_t> candidates;
    if (alternates) {
        try {
            candidates = alternates(frame);
        } catch (...) {
            candidates.clear();  // a throwing callback only costs the substitutes
        }
    }
    std::string tried;
    for (const std::uint32_t alt : candidates) {
        if (alt == frame) {
            continue;  // never the refused frame again
        }
        if (cancelled && cancelled()) {
            stop = true;
            return std::nullopt;
        }
        const auto t1 = Clock::now();
        auto altPair = source(alt);
        decodeMs += msSince(t1);
        if (altPair.ok()) {
            notes.push_back(std::format("frame {} could not be decoded ({}); measured frame {} in its place", frame,
                                        why, alt));
            return DecodedSample{alt, std::move(altPair).value()};
        }
        tried += std::format("{}{} ({})", tried.empty() ? "" : ", ", alt, altPair.error().message);
    }
    notes.push_back(tried.empty()
                        ? std::format("frame {} could not be decoded ({}); skipped", frame, why)
                        : std::format("frame {} could not be decoded ({}); skipped - its substitutes failed too: {}",
                                      frame, why, tried));
    return std::nullopt;
}

/// "frame 7080 ...; frame 8082 ..." - the notes as one log phrase.
[[nodiscard]] std::string joinNotes(const std::vector<std::string>& notes) {
    std::string s;
    for (const std::string& n : notes) {
        s += (s.empty() ? "" : "; ") + n;
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Sample frames
// ---------------------------------------------------------------------------
std::vector<std::uint32_t> clipSampleFrames(std::uint32_t frameCount, const std::vector<std::uint32_t>& syncFrames,
                                            std::uint32_t samples, double firstFraction, double lastFraction,
                                            std::uint32_t sequentialFrames) {
    std::vector<std::uint32_t> out;
    if (frameCount == 0 || samples == 0) {
        return out;
    }
    // Garbage fractions land on the whole clip rather than on nonsense.
    if (!std::isfinite(firstFraction) || !std::isfinite(lastFraction)) {
        firstFraction = 0.0;
        lastFraction = 1.0;
    }
    firstFraction = std::clamp(firstFraction, 0.0, 1.0);
    lastFraction = std::clamp(lastFraction, firstFraction, 1.0);
    const double last = static_cast<double>(frameCount - 1u);

    // ---- the evenly spread targets ------------------------------------------
    std::vector<std::uint32_t> targets;
    targets.reserve(samples);
    for (std::uint32_t i = 0; i < samples; ++i) {
        const double f = samples == 1
                             ? 0.5 * (firstFraction + lastFraction)
                             : firstFraction + (lastFraction - firstFraction) * static_cast<double>(i) /
                                                   static_cast<double>(samples - 1u);
        targets.push_back(static_cast<std::uint32_t>(std::clamp(std::lround(f * last), 0L, static_cast<long>(last))));
    }

    // ---- a long clip: snap to its sync frames ----------------------------------
    // Only sync frames inside the clip count; the list must be ascending for
    // lower_bound, which a sync table is - but a caller's list is sorted
    // defensively all the same.
    std::vector<std::uint32_t> sync;
    for (const std::uint32_t s : syncFrames) {
        if (s < frameCount) {
            sync.push_back(s);
        }
    }
    std::sort(sync.begin(), sync.end());
    sync.erase(std::unique(sync.begin(), sync.end()), sync.end());
    if (frameCount > sequentialFrames && !sync.empty()) {
        for (std::uint32_t& t : targets) {
            const auto it = std::lower_bound(sync.begin(), sync.end(), t);
            if (it == sync.end()) {
                t = sync.back();
            } else if (it == sync.begin() || *it == t) {
                t = *it;
            } else {
                // Nearest of the two neighbours; the earlier one on a tie.
                const std::uint32_t after = *it;
                const std::uint32_t before = *(it - 1);
                t = (t - before) <= (after - t) ? before : after;
            }
        }
    }

    // ---- ascending, unique ------------------------------------------------------
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

std::vector<std::uint32_t> clipSampleAlternates(std::uint32_t frame, std::uint32_t frameCount,
                                                const std::vector<std::uint32_t>& syncFrames,
                                                const std::vector<std::uint32_t>& samples,
                                                std::uint32_t maxAlternates, std::uint32_t sequentialFrames) {
    std::vector<std::uint32_t> out;
    if (frame >= frameCount || maxAlternates == 0) {
        return out;
    }
    // ---- the open interval between the neighbouring samples ---------------------------
    // A substitute must stay strictly between them, so the measured set stays
    // ascending and distinct (each sample keeps its own stretch of the clip).
    std::vector<std::uint32_t> sorted(samples);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const auto self = std::lower_bound(sorted.begin(), sorted.end(), frame);
    if (self == sorted.end() || *self != frame) {
        return out;  // not one of the samples: nothing to substitute
    }
    // lo / hi are exclusive bounds; -1 and frameCount stand for "the clip's ends".
    const std::int64_t lo = self == sorted.begin() ? -1 : static_cast<std::int64_t>(*(self - 1));
    const std::int64_t hi = (self + 1) == sorted.end() ? static_cast<std::int64_t>(frameCount)
                                                       : static_cast<std::int64_t>(*(self + 1));

    // ---- the candidates: sync frames of a long clip, else plain neighbours --------------
    std::vector<std::uint32_t> after, before;  // nearest first on each side
    std::vector<std::uint32_t> sync;
    for (const std::uint32_t s : syncFrames) {
        if (s < frameCount) {
            sync.push_back(s);
        }
    }
    std::sort(sync.begin(), sync.end());
    sync.erase(std::unique(sync.begin(), sync.end()), sync.end());
    if (frameCount > sequentialFrames && !sync.empty()) {
        // The sync frames on each side, nearest first.
        const auto at = std::upper_bound(sync.begin(), sync.end(), frame);
        for (auto it = at; it != sync.end() && static_cast<std::int64_t>(*it) < hi && after.size() < maxAlternates;
             ++it) {
            after.push_back(*it);
        }
        auto it = std::lower_bound(sync.begin(), sync.end(), frame);
        while (it != sync.begin() && before.size() < maxAlternates) {
            --it;
            if (static_cast<std::int64_t>(*it) <= lo) {
                break;
            }
            before.push_back(*it);
        }
    } else {
        // Every frame is a candidate: the plain neighbours.
        for (std::int64_t f = static_cast<std::int64_t>(frame) + 1; f < hi && after.size() < maxAlternates; ++f) {
            after.push_back(static_cast<std::uint32_t>(f));
        }
        for (std::int64_t f = static_cast<std::int64_t>(frame) - 1; f > lo && before.size() < maxAlternates; --f) {
            before.push_back(static_cast<std::uint32_t>(f));
        }
    }

    // ---- interleaved by distance, the later one first on a tie ---------------------------
    std::size_t a = 0, b = 0;
    while (out.size() < maxAlternates && (a < after.size() || b < before.size())) {
        const bool takeAfter =
            b >= before.size() || (a < after.size() && after[a] - frame <= frame - before[b]);
        out.push_back(takeAfter ? after[a++] : before[b++]);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Lens rotation over several frames
// ---------------------------------------------------------------------------
Result<LensRotationMeasurement> measureLensRotation(const geom::LensRig& rig, const geom::BlendParams& blend,
                                                    const std::vector<std::uint32_t>& frames,
                                                    const ClipFrameSource& source, const ParallaxWarpParams& parallax,
                                                    const LensRotationParams& params, ThreadPool& pool,
                                                    const ClipCancel& cancelled, const ClipFrameAlternates& alternates,
                                                    std::uint32_t minSamples) {
    if (!source) {
        return Error{ErrorCode::InvalidArgument, "measureLensRotation: no frame source"};
    }
    LensRotationMeasurement m;
    if (frames.empty()) {
        m.reason = "the clip has no frame to measure";
        return m;
    }
    m.frames.reserve(frames.size());
    for (const std::uint32_t planned : frames) {
        if (cancelled && cancelled()) {
            return Error{ErrorCode::Unsupported, "lens rotation measurement cancelled"};
        }
        // ---- decode: the planned frame, a substitute, or nothing ------------------
        bool stop = false;
        std::optional<DecodedSample> sample =
            decodeSample(planned, source, alternates, m.sampleNotes, m.decodeMs, cancelled, stop);
        if (stop) {
            return Error{ErrorCode::Unsupported, "lens rotation measurement cancelled"};
        }
        if (!sample) {
            continue;  // skipped (noted); the minimum is checked after the pass
        }
        const std::uint32_t frame = sample->frame;
        m.frames.push_back(frame);
        // ---- bands, flow, raw cells, fit ------------------------------------------
        const auto tWork = Clock::now();
        const auto tBand = Clock::now();
        OSV_TRY_ASSIGN(LensBands bands, measureParallaxBands(rig, sample->pair, blend, parallax, nullptr, pool));
        const double bandMs = msSince(tBand);
        ParallaxCellStats cells;
        auto grid = parallaxFromBands(bands, parallax, &pool, bandMs, &cells);
        // Only the raw cells matter here: they are the measurement before the
        // structured gate, so neither its refusal nor its strength changes
        // the fit (a refused grid still filled them).
        (void)grid;
        if (!cells.valid()) {
            m.refusals.push_back(std::format("frame {}: no flow ({})", frame,
                                             grid.ok() ? std::string("no cells") : grid.error().message));
        } else {
            auto fit = fitLensRotation(cells, params);
            if (fit.ok()) {
                m.perFrame.push_back(fit.value());
            } else {
                m.refusals.push_back(std::format("frame {}: {}", frame, fit.error().message));
            }
        }
        m.analysisMs += msSince(tWork);
    }

    // ---- enough samples decoded? ---------------------------------------------------
    // Capped at the planned count, so a one-frame request still means "that
    // frame must decode".
    const std::size_t needed = std::min<std::size_t>(std::max<std::uint32_t>(minSamples, 1u), frames.size());
    if (m.frames.size() < needed) {
        return Error{ErrorCode::Decoder, std::format("measureLensRotation: only {} of {} sample frames could be "
                                                     "decoded ({} needed): {}",
                                                     m.frames.size(), frames.size(), needed, joinNotes(m.sampleNotes))};
    }

    // ---- one rigid rotation for the clip, or none ------------------------------
    auto combined = combineLensRotations(m.perFrame, params);
    if (combined.ok()) {
        m.accepted = true;
        m.fit = combined.value();
    } else {
        m.accepted = false;
        m.reason = combined.error().message;
        // The per-frame refusals are the more useful half of a refusal.
        for (const std::string& r : m.refusals) {
            m.reason += "; " + r;
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
//  Medians
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> clipParallaxGrid(const std::vector<const ParallaxWarpGrid*>& grids) {
    std::vector<const ParallaxWarpGrid*> in;
    for (const ParallaxWarpGrid* g : grids) {
        if (g != nullptr) {
            in.push_back(g);
        }
    }
    if (in.empty()) {
        return Error{ErrorCode::InvalidArgument, "clipParallaxGrid: no grid"};
    }
    // ---- one layout or nothing ------------------------------------------------
    const ParallaxWarpGrid& first = *in.front();
    for (const ParallaxWarpGrid* g : in) {
        if (!g->valid()) {
            return Error{ErrorCode::InvalidArgument, "clipParallaxGrid: a grid is empty or malformed"};
        }
        if (g->w != first.w || g->h != first.h || g->latMinRad != first.latMinRad ||
            g->latMaxRad != first.latMaxRad) {
            return Error{ErrorCode::InvalidArgument, "clipParallaxGrid: the grids have different layouts"};
        }
    }

    // ---- per cell, per component ------------------------------------------------
    ParallaxWarpGrid out = first;
    std::vector<float> column(in.size());
    for (std::size_t i = 0; i < out.uv.size(); ++i) {
        for (std::size_t k = 0; k < in.size(); ++k) {
            column[k] = in[k]->uv[i];
        }
        out.uv[i] = medianInPlace(column);
    }

    // ---- diagnostics: the median's disparities, the samples' summed cost ---------
    std::vector<double> measured, gated, strengths;
    out.consistentPixels = 0;
    out.totalPixels = 0;
    out.structuredPixels = 0;
    out.consistentStructuredPixels = 0;
    out.bandMs = out.flowMs = out.gridMs = 0.0;
    for (const ParallaxWarpGrid* g : in) {
        out.consistentPixels += g->consistentPixels;
        out.totalPixels += g->totalPixels;
        out.structuredPixels += g->structuredPixels;
        out.consistentStructuredPixels += g->consistentStructuredPixels;
        out.bandMs += g->bandMs;
        out.flowMs += g->flowMs;
        out.gridMs += g->gridMs;
        measured.push_back(static_cast<double>(g->measuredCells));
        gated.push_back(static_cast<double>(g->gatedCells));
        // A non-finite strength counts as no trust (0), as the gate defines it.
        strengths.push_back(std::isfinite(g->strength) ? std::clamp(g->strength, 0.0, 1.0) : 0.0);
    }
    out.measuredCells = static_cast<std::uint32_t>(std::lround(medianOf(measured)));
    out.gatedCells = static_cast<std::uint32_t>(std::lround(medianOf(gated)));
    // ---- the untrusted share per column: the median of the samples' ----------------
    // A column most samples could not measure is one the clip grid did not
    // measure either (its cells are medians of filled or gated values), which
    // is what the per-column guard asks.  Only when every sample says it.
    out.untrustedShare.clear();
    bool allShares = true;
    for (const ParallaxWarpGrid* g : in) {
        allShares = allShares && g->untrustedShare.size() == out.w;
    }
    if (allShares) {
        out.untrustedShare.assign(out.w, 0.0f);
        for (std::uint32_t c = 0; c < out.w; ++c) {
            for (std::size_t k = 0; k < in.size(); ++k) {
                const float v = in[k]->untrustedShare[c];
                column[k] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;  // not a number: trusted
            }
            out.untrustedShare[c] = medianInPlace(column);
        }
    }
    // Each sample's grid already carries its own structured-gate strength, so
    // the per-cell median is a median of scaled corrections; its strength is
    // the median of the samples' - all 1 gives exactly 1, and the clip seam
    // table fills the rest under a partly trusted clip grid (measureClipSteady).
    out.strength = medianOf(strengths);
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    const std::size_t cells = static_cast<std::size_t>(out.w) * out.h;
    for (std::size_t k = 0; k < cells; ++k) {
        const double mag = std::hypot(static_cast<double>(out.uv[k * 2u]), static_cast<double>(out.uv[k * 2u + 1u]));
        sumAbs += mag;
        maxAbs = std::max(maxAbs, mag);
    }
    // FULL disparity, like gridFromFlow reports it (twice the stored half).
    out.meanAbsCorrectionDeg = cells ? 2.0 * rad2deg(sumAbs / static_cast<double>(cells)) : 0.0;
    out.maxAbsCorrectionDeg = 2.0 * rad2deg(maxAbs);
    return out;
}

Result<std::vector<float>> clipSeamTable(const std::vector<const std::vector<float>*>& tables) {
    std::vector<const std::vector<float>*> in;
    for (const std::vector<float>* t : tables) {
        if (t != nullptr && !t->empty()) {
            in.push_back(t);
        }
    }
    if (in.empty()) {
        return Error{ErrorCode::InvalidArgument, "clipSeamTable: no table"};
    }
    const std::size_t len = in.front()->size();
    for (const std::vector<float>* t : in) {
        if (t->size() != len) {
            return Error{ErrorCode::InvalidArgument, "clipSeamTable: the tables have different lengths"};
        }
    }
    std::vector<float> out(len);
    std::vector<float> column(in.size());
    for (std::size_t i = 0; i < len; ++i) {
        for (std::size_t k = 0; k < in.size(); ++k) {
            column[k] = (*in[k])[i];
        }
        out[i] = medianInPlace(column);
        if (!std::isfinite(out[i])) {
            return Error{ErrorCode::InvalidArgument, "clipSeamTable: a table holds a non-finite shift"};
        }
    }
    return out;
}

Result<BlendSeam> clipBlendSeam(const std::vector<const BlendSeam*>& seams) {
    std::vector<const BlendSeam*> in;
    for (const BlendSeam* s : seams) {
        if (s != nullptr) {
            in.push_back(s);
        }
    }
    if (in.empty()) {
        return Error{ErrorCode::InvalidArgument, "clipBlendSeam: no seam"};
    }
    const std::uint32_t columns = in.front()->columns;
    bool allNear = true;
    for (const BlendSeam* s : in) {
        if (!s->valid() || s->columns != columns) {
            return Error{ErrorCode::InvalidArgument, "clipBlendSeam: a seam is malformed or has another width"};
        }
        allNear = allNear && s->nearWeight.size() == columns;
    }

    BlendSeam out;
    out.columns = columns;
    out.table.assign(static_cast<std::size_t>(columns) * 2u, 0.0f);
    std::vector<float> column(in.size());
    // ---- latitude and feather half width, per column ------------------------------
    for (std::size_t i = 0; i < out.table.size(); ++i) {
        for (std::size_t k = 0; k < in.size(); ++k) {
            column[k] = in[k]->table[i];
        }
        out.table[i] = medianInPlace(column);
    }
    // ---- the near weight, only when every seam carries one ---------------------------
    if (allNear) {
        out.nearWeight.assign(columns, 0.0f);
        for (std::uint32_t c = 0; c < columns; ++c) {
            for (std::size_t k = 0; k < in.size(); ++k) {
                column[k] = in[k]->nearWeight[c];
            }
            out.nearWeight[c] = std::clamp(medianInPlace(column), 0.0f, 1.0f);
        }
    }
    // ---- the validity ramp and the diagnostics ------------------------------------------
    std::vector<double> edge, carve, narrow, forced;
    for (const BlendSeam* s : in) {
        edge.push_back(static_cast<double>(s->edgeRad));
        carve.push_back(s->carveMs);
        narrow.push_back(static_cast<double>(s->narrowColumns));
        forced.push_back(static_cast<double>(s->forcedColumns));
        out.carveMs += s->carveMs;
        out.correctMs += s->correctMs;
        out.costMs += s->costMs;
        out.dpMs += s->dpMs;
        out.meanResidual += s->meanResidual / static_cast<double>(in.size());
    }
    out.edgeRad = static_cast<float>(medianOf(edge));
    out.narrowColumns = static_cast<std::uint32_t>(std::lround(medianOf(narrow)));
    out.forcedColumns = static_cast<std::uint32_t>(std::lround(medianOf(forced)));
    double sumLat = 0.0;
    double sumHalf = 0.0;
    double maxLat = 0.0;
    for (std::uint32_t c = 0; c < columns; ++c) {
        const double lat = rad2deg(static_cast<double>(out.table[c * 2u]));
        sumLat += lat;
        sumHalf += rad2deg(static_cast<double>(out.table[c * 2u + 1u]));
        maxLat = std::max(maxLat, std::fabs(lat));
    }
    out.meanLatDeg = columns ? sumLat / columns : 0.0;
    out.meanHalfWidthDeg = columns ? sumHalf / columns : 0.0;
    out.maxAbsLatDeg = maxLat;
    out.usedPrior = false;
    if (!out.valid()) {
        return Error{ErrorCode::Internal, "clipBlendSeam: the median seam is malformed"};
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Auto
// ---------------------------------------------------------------------------
Result<SteadyDecision> decideSteady(const std::vector<SteadySample>& samples, const SeamCorrection& clip,
                                    const SteadyDecisionParams& params, ThreadPool* pool) {
    // ---- the tuning, once ---------------------------------------------------------
    const auto finite01 = [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; };
    // NCC losses live in [-2, 2]; the tolerated loss is a non-negative part of that.
    const bool lossOk = std::isfinite(params.maxFailedLoss) && params.maxFailedLoss >= 0.0 &&
                        params.maxFailedLoss <= 2.0;
    if (params.sectors == 0 || params.sectors > 4096 || !(params.latHalfDeg > 0.0) ||
        !std::isfinite(params.latHalfDeg) || !finite01(params.maxLoss) || !finite01(params.minStd) ||
        !(params.minOwnNcc >= -1.0 && params.minOwnNcc <= 1.0) || !finite01(params.minGain) ||
        !finite01(params.minKeep) || !finite01(params.maxFailedFraction) || !lossOk) {
        return Error{ErrorCode::InvalidArgument, "decideSteady: parameters out of range"};
    }
    SteadyDecision d;
    double sumLoss = 0.0;
    for (const SteadySample& s : samples) {
        if (s.bands == nullptr) {
            return Error{ErrorCode::InvalidArgument, "decideSteady: a sample has no bands"};
        }
        // ---- the sample through its own correction and through the clip's -------
        const LensBands* ownBands = s.bands;
        std::optional<LensBands> ownHold;
        if (hasCorrection(s.own)) {
            OSV_TRY_ASSIGN(LensBands b, correctBandsForSeam(*s.bands, s.own, pool));
            ownHold = std::move(b);
            ownBands = &*ownHold;
        }
        const LensBands* clipBands = s.bands;
        std::optional<LensBands> clipHold;
        if (hasCorrection(clip)) {
            OSV_TRY_ASSIGN(LensBands b, correctBandsForSeam(*s.bands, clip, pool));
            clipHold = std::move(b);
            clipBands = &*clipHold;
        }
        const std::vector<SectorScore> none = sectorScores(*s.bands, params);
        const std::vector<SectorScore> own = sectorScores(*ownBands, params);
        const std::vector<SectorScore> held = sectorScores(*clipBands, params);

        // ---- per sector ---------------------------------------------------------------
        for (std::uint32_t k = 0; k < params.sectors; ++k) {
            // Scored only where all three views have the texture to say anything.
            if (!none[k].judged || !own[k].judged || !held[k].judged) {
                continue;
            }
            SteadySectorScore rec;
            rec.frame = s.frame;
            rec.sector = k;
            rec.none = none[k].ncc;
            rec.own = own[k].ncc;
            rec.clip = held[k].ncc;
            d.scores.push_back(rec);
            ++d.textured;
            const double loss = rec.own - rec.clip;
            sumLoss += loss;

            // Judged only where the own correction really aligned something
            // (see the header: one surface both lenses see, with parallax).
            const double gain = rec.own - rec.none;
            if (rec.own < params.minOwnNcc || gain < params.minGain) {
                continue;
            }
            ++d.judged;
            // The share of that alignment the clip correction keeps.
            const double keep = (rec.clip - rec.none) / gain;
            if (loss > params.maxLoss && keep < params.minKeep) {
                ++d.failed;
                d.worstFailedLoss = std::max(d.worstFailedLoss, loss);
            }
            if (keep < d.worstKeep) {
                d.worstKeep = keep;
                d.worstLoss = loss;
                d.worstFrame = s.frame;
                // Sector centre in the polar-axis layout's longitude.
                d.worstLonDeg = (static_cast<double>(k) + 0.5) * 360.0 / static_cast<double>(params.sectors) - 180.0;
                d.worstNoneNcc = rec.none;
                d.worstOwnNcc = rec.own;
                d.worstClipNcc = rec.clip;
            }
        }
    }
    d.meanLoss = d.textured ? sumLoss / static_cast<double>(d.textured) : 0.0;
    // ---- the verdict, with its tolerance (see the header) ------------------------------
    // Nothing judged means no own correction aligned anything the clip
    // correction could lose: holding still costs nothing anyone could see.
    d.steady = steadyWithinTolerance(d.judged, d.failed, d.worstFailedLoss, params);
    return d;
}

bool steadyWithinTolerance(std::uint32_t judged, std::uint32_t failed, double worstFailedLoss,
                           const SteadyDecisionParams& params) noexcept {
    if (failed == 0) {
        return true;  // the rule before the tolerance existed, unchanged
    }
    // Garbage in the tuning or the loss tolerates nothing.
    if (!std::isfinite(params.maxFailedFraction) || !std::isfinite(params.maxFailedLoss) ||
        !std::isfinite(worstFailedLoss) || failed > judged) {
        return false;
    }
    // A few small failures are transients (a car passing one sample), not a
    // scene that moves: rare (at most maxFailedFraction of the judged pairs,
    // rounded down, so 19 judged tolerate none and 20 tolerate one) AND small
    // (none losing more than maxFailedLoss).  The epsilon keeps 0.05 x 20
    // from rounding down to 0.
    const double fraction = std::clamp(params.maxFailedFraction, 0.0, 1.0);
    const auto tolerated = static_cast<std::uint32_t>(std::floor(fraction * static_cast<double>(judged) + 1e-9));
    return failed <= tolerated && worstFailedLoss <= params.maxFailedLoss;
}

std::string describeSteadyDecision(const SteadyDecision& d) {
    if (d.judged == 0) {
        return std::format("{}: no sector where one frame's own correction aligns anything ({} textured scored)",
                           d.steady ? "steady" : "follows scene", d.textured);
    }
    // A steady verdict over failures says they were within the tolerance.
    const std::string tolerance =
        d.failed > 0 ? std::format(" (largest failed loss {:.3f}{})", d.worstFailedLoss,
                                   d.steady ? ", within the tolerance" : "")
                     : std::string();
    return std::format("{}: {} of {} judged sectors lose their alignment to the clip correction{}; the worst keeps "
                       "{:.0f}% of its own gain (frame {}, lon {:+.0f} deg, NCC none {:.3f} / own {:.3f} / clip "
                       "{:.3f}); mean loss {:+.4f} over {} textured",
                       d.steady ? "steady" : "follows scene", d.failed, d.judged, tolerance, 100.0 * d.worstKeep,
                       d.worstFrame, d.worstLonDeg, d.worstNoneNcc, d.worstOwnNcc, d.worstClipNcc, d.meanLoss,
                       d.textured);
}

// ---------------------------------------------------------------------------
//  The clip correction
// ---------------------------------------------------------------------------
Result<ClipSteady> measureClipSteady(const geom::LensRig& rig, const geom::BlendParams& blend,
                                     const std::vector<std::uint32_t>& frames, const ClipFrameSource& source,
                                     const ClipSteadyParams& params, ThreadPool& pool,
                                     const ClipSampleGridFn& onSampleGrid, const ClipCancel& cancelled,
                                     const ClipFrameAlternates& alternates) {
    if (!source) {
        return Error{ErrorCode::InvalidArgument, "measureClipSteady: no frame source"};
    }
    ClipSteady out;
    if (frames.empty() || (!params.parallaxOn && !params.seamOn)) {
        out.frames = frames;
        return out;  // nothing to measure: every piece stays null
    }
    out.frames.reserve(frames.size());

    // ---- one pass over the samples ------------------------------------------------
    // Per sample: its uncorrected bands (kept for the carve and the verdict),
    // its own grid, its own seam table and the table's per-column confidence
    // (the correction where it has no grid, the per-column guard's under its
    // grid), and the usable rim the carve is steered by.
    struct Sample {
        std::uint32_t frame = 0;
        LensBands bands;
        std::shared_ptr<const ParallaxWarpGrid> grid;
        std::shared_ptr<const std::vector<float>> table;
        std::shared_ptr<const std::vector<float>> confidence;  ///< Same length as `table`, or null.
        std::shared_ptr<const PhotoSeamField> rim;
    };
    std::vector<Sample> samples;
    samples.reserve(frames.size());
    for (const std::uint32_t planned : frames) {
        if (cancelled && cancelled()) {
            return Error{ErrorCode::Unsupported, "clip analysis cancelled"};
        }
        // ---- decode: the planned frame, a substitute, or nothing ------------------
        bool stop = false;
        std::optional<DecodedSample> decoded =
            decodeSample(planned, source, alternates, out.sampleNotes, out.decodeMs, cancelled, stop);
        if (stop) {
            return Error{ErrorCode::Unsupported, "clip analysis cancelled"};
        }
        if (!decoded) {
            continue;  // skipped (noted); the minimum is checked after the pass
        }
        const std::uint32_t frame = decoded->frame;
        const video::FramePair& pair = decoded->pair;
        out.frames.push_back(frame);
        const auto tWork = Clock::now();
        Sample s;
        s.frame = frame;
        // The parallax band (2048 x +-6 deg) is also the carve's band, so one
        // render serves both (SeamCarve.cpp renders exactly this for a carve).
        const auto tBand = Clock::now();
        OSV_TRY_ASSIGN(s.bands, measureParallaxBands(rig, pair, blend, params.parallax, nullptr, pool));
        const double bandMs = msSince(tBand);
        if (params.parallaxOn) {
            // The importer's rule for one measurement, through the same call:
            // refused by the structured gate, or accepted and already scaled
            // by its strength (ParallaxWarpGrid::strength).
            auto grid = parallaxFromBands(s.bands, params.parallax, &pool, bandMs);
            if (grid.ok()) {
                s.grid = std::make_shared<const ParallaxWarpGrid>(std::move(grid).value());
            }
            if (onSampleGrid) {
                onSampleGrid(frame, s.grid);
            }
        }
        if (params.seamOn) {
            // The seam table is the correction of a sample without a grid,
            // the remaining share under a partly trusted one, and the
            // per-column guard's under an accepted one - exactly as the
            // importer builds a bucket's correction, which measures the table
            // for every bucket with Seam Search on.
            auto profile = searchSeam(rig, pair, blend, params.seamSearch, pool);
            if (profile.ok() && !profile.value().shiftDeg.empty()) {
                SeamProfile& p = profile.value();
                // The confidence only when it is the table's, column for column.
                if (p.confidence.size() == p.shiftDeg.size()) {
                    s.confidence = std::make_shared<const std::vector<float>>(std::move(p.confidence));
                }
                s.table = std::make_shared<const std::vector<float>>(std::move(p.shiftDeg));
            }
        }
        if (params.seamOn && params.rimCost) {
            // The rim on the lenses the render will blend: shading-corrected
            // first when the lens shading correction is on, exactly as the
            // importer's per-bucket field is measured.
            std::optional<LensShadingModel> shading;
            if (params.shadingOn && params.shading.mode == LensShadingMode::Auto) {
                auto model = measureLensShading(rig, pair, blend, params.shading, pool);
                if (model.ok() && model.value().active()) {
                    shading = params.shading.strength >= 1.0
                                  ? std::move(model).value()
                                  : scaledLensShadingModel(model.value(), params.shading.strength);
                }
            }
            auto field = measurePhotoSeam(rig, pair, blend, params.photo, pool,
                                          shading ? &*shading : nullptr);
            if (field.ok()) {
                s.rim = std::make_shared<const PhotoSeamField>(std::move(field).value());
            }
        }
        out.measureMs += msSince(tWork);
        samples.push_back(std::move(s));
    }

    // ---- enough samples decoded? ---------------------------------------------------
    // Capped at the planned count, so a short request still means "every one".
    const std::size_t needed = std::min<std::size_t>(std::max<std::uint32_t>(params.minSamples, 1u), frames.size());
    if (samples.size() < needed) {
        return Error{ErrorCode::Decoder, std::format("measureClipSteady: only {} of {} sample frames could be "
                                                     "decoded ({} needed): {}",
                                                     samples.size(), frames.size(), needed,
                                                     joinNotes(out.sampleNotes))};
    }

    const auto tFinish = Clock::now();
    // ---- the clip grid ---------------------------------------------------------------
    std::vector<const ParallaxWarpGrid*> grids;
    for (const Sample& s : samples) {
        if (s.grid) {
            grids.push_back(s.grid.get());
        }
    }
    out.acceptedGrids = static_cast<std::uint32_t>(grids.size());
    if (params.parallaxOn && !grids.empty() && grids.size() >= std::max<std::uint32_t>(params.minGrids, 1u)) {
        OSV_TRY_ASSIGN(ParallaxWarpGrid g, clipParallaxGrid(grids));
        out.grid = std::make_shared<const ParallaxWarpGrid>(std::move(g));
    }

    // ---- the clip seam table (the fallback where there is no clip grid) -------------
    // The median of the tables of the samples without a grid or with a partly
    // trusted one, as before every sample measured a table: the samples a
    // refused clip grid would have rendered with their tables.
    std::vector<const std::vector<float>*> tables;
    for (const Sample& s : samples) {
        if (s.table && (!s.grid || s.grid->strength < 1.0)) {
            tables.push_back(s.table.get());
        }
    }
    if (params.seamOn && !tables.empty()) {
        auto t = clipSeamTable(tables);
        if (t.ok()) {
            out.seamTable = std::make_shared<const std::vector<float>>(std::move(t).value());
        }
    }

    // ---- the per-column guard on the clip grid --------------------------------------------
    // Every sample's table and confidence, as the per-column median, against
    // the clip grid's per-column untrusted share (the median of the
    // samples'): the columns most samples could not measure, where the
    // tables are sure and found a disparity the grid missed, go to the table.
    // What renders is the guarded grid and the table share under it (1 -
    // strength everywhere plus those columns); without tables (seam off, or
    // none measured) the grid renders alone.
    if (out.grid && params.seamOn) {
        std::vector<const std::vector<float>*> allTables;
        std::vector<const std::vector<float>*> allConfidences;
        for (const Sample& s : samples) {
            // A table counts only with its confidence, so the two medians
            // are over the same samples.
            if (s.table && s.confidence) {
                allTables.push_back(s.table.get());
                allConfidences.push_back(s.confidence.get());
            }
        }
        if (!allTables.empty()) {
            auto table = clipSeamTable(allTables);
            auto confidence = clipSeamTable(allConfidences);  // the same per-column median
            if (table.ok() && confidence.ok()) {
                OSV_TRY_ASSIGN(GuardedCorrection guarded,
                               guardGridWithTable(*out.grid, table.value(), confidence.value()));
                out.guardedColumns = guarded.guardedColumns;
                if (guarded.changed) {
                    out.grid = std::make_shared<const ParallaxWarpGrid>(std::move(guarded.grid));
                }
                if (!guarded.table.empty()) {
                    out.gridTable = std::make_shared<const std::vector<float>>(std::move(guarded.table));
                }
            }
        }
    }

    // ---- the correction the clip renders with -------------------------------------------
    // The importer's rule: the grid when there is one, with the table share
    // the guard leaves under it (gridTable), else the table - exactly as the
    // importer applies it.
    const WarpGridView clipView = viewOf(out.grid.get());
    SeamCorrection clipCorrection;
    if (clipView.valid()) {
        clipCorrection.warp = &clipView;
        if (out.gridTable && !out.gridTable->empty()) {
            clipCorrection.seamShiftDeg = out.gridTable.get();
        }
    } else if (out.seamTable) {
        clipCorrection.seamShiftDeg = out.seamTable.get();
    }

    // ---- the Auto verdict -----------------------------------------------------------------
    {
        std::vector<WarpGridView> ownViews(samples.size());
        // Each sample's own correction exactly as a bucket measured on it
        // renders in the importer: its grid after the per-column guard with
        // its own table, the table share under it (1 - strength plus the
        // columns handed over), and its table where it has no grid.
        std::vector<GuardedCorrection> ownGuarded(samples.size());
        std::vector<SteadySample> judged;
        judged.reserve(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            SteadySample ss;
            ss.frame = samples[i].frame;
            ss.bands = &samples[i].bands;
            ownViews[i] = viewOf(samples[i].grid.get());
            if (ownViews[i].valid()) {
                ss.own.warp = &ownViews[i];
                if (samples[i].table) {
                    // Without a confidence the guard keeps the rule before it
                    // (the 1 - strength share of the table).
                    static const std::vector<float> kNoConfidence;
                    OSV_TRY_ASSIGN(ownGuarded[i],
                                   guardGridWithTable(*samples[i].grid, *samples[i].table,
                                                      samples[i].confidence ? *samples[i].confidence : kNoConfidence));
                    if (ownGuarded[i].changed) {
                        ownViews[i] = viewOf(&ownGuarded[i].grid);  // the vector never grows: stable
                    }
                    if (!ownGuarded[i].table.empty()) {
                        ss.own.seamShiftDeg = &ownGuarded[i].table;
                    }
                }
            } else if (samples[i].table) {
                ss.own.seamShiftDeg = samples[i].table.get();
            }
            judged.push_back(ss);
        }
        OSV_TRY_ASSIGN(out.decision, decideSteady(judged, clipCorrection, params.decision, &pool));
    }

    // ---- the clip seam: every sample carved through the clip correction ---------------------
    if (params.seamOn) {
        if (params.rimCost) {
            installPhotoRimPenaltyHook();
        }
        std::vector<BlendSeam> carves;
        carves.reserve(samples.size());
        for (const Sample& s : samples) {
            if (cancelled && cancelled()) {
                return Error{ErrorCode::Unsupported, "clip analysis cancelled"};
            }
            // The usable rim steers this carve on this thread, as it does the
            // importer's per-bucket carve (a null field installs nothing).
            PhotoRimPenaltyScope rimScope(params.rimCost ? &rig : nullptr, s.rim);
            // No temporal prior: each carve depends on its own sample alone.
            auto carved = carveSeamFromBands(s.bands, clipCorrection, params.carve, nullptr, &pool);
            if (carved.ok()) {
                carves.push_back(std::move(carved).value());
            }
        }
        std::vector<const BlendSeam*> seams;
        for (const BlendSeam& c : carves) {
            seams.push_back(&c);
        }
        if (!seams.empty()) {
            auto median = clipBlendSeam(seams);
            if (median.ok()) {
                out.seam = std::make_shared<const BlendSeam>(std::move(median).value());
            }
        }
    }
    out.finishMs = msSince(tFinish);
    return out;
}

}  // namespace osv::render
