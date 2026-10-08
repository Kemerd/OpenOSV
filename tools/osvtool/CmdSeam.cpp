// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvtool seam: overlap correlation with the current conventions plus a table
// of the alternatives, so a wrong sign anywhere in the geometry chain shows
// up as a number rather than as a mysterious double image.

#include "Commands.h"
#include "Pipeline.h"

#include "osv/core/Log.h"
#include "osv/geom/StreamScaling.h"
#include "osv/render/ClipSteady.h"  // [WP-STEADY]
#include "osv/render/LensAlign.h"   // [WP-STEADY]
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace osvtool {

using namespace osv;

namespace {

struct SeamOptions {
    PipelineOptions pipeline;
    int frame = 0;
    bool json = false;
    bool search = false;
    bool parallax = false;
    std::string flowBackend = "auto";
    // Tuning overrides for the parallax grid, so its resolution and the
    // cross-meridian anisotropy can be swept against the real clip instead of
    // being guessed.  Defaults are ParallaxWarpParams' own.
    render::ParallaxWarpParams parallaxTuning;
    /// Optional "c0-c1" column window (band columns, 2048 per revolution,
    /// wraps) scored separately.  The whole-band NCC is dominated by open sky
    /// and distant ground, where there is no parallax to fix; a window over
    /// the near object crossing the seam is the number that actually says
    /// whether the correction helps where it matters.
    std::string region;
    /// Optional path prefix: write the per-lens bands as raw float32 planes
    /// for offline inspection - "raw" (uncorrected), "table" (with the
    /// --search table) and "parallax" (with the --parallax grid).
    std::string dumpBands;
    /// [WP-PHOTO] Measure the photometric seam field and score the sky seam
    /// (NEURAL_STITCHING.md table 1.4) for off / inset / rim / full, over the
    /// --region columns (default 410-900 of 2048: the sample's open sky).
    bool photo = false;
    render::PhotoSeamParams photoTuning;
    /// Width of the polar map the metrics are scored on (the research used
    /// 4096; the per-pixel colour noise floor of the dE term depends on it).
    std::uint32_t photoMetricW = 4096;
    // ---- [WP-STEADY] --------------------------------------------------------
    /// Fit the per-clip lens rotation on the clip's fixed rotation frames
    /// (render/LensAlign.h) and fold it into the rig BEFORE every other
    /// measurement, as the importer's "Lens Alignment: Auto" does.
    bool lensAlign = false;
    /// Measure the per-clip steady correction (render/ClipSteady.h) on the
    /// clip's fixed sample frames and score it next to this frame's own grid.
    bool steady = false;
    /// Several named column windows scored with --parallax, e.g.
    /// "sky:410-900,ground:1110-1700,wing:1880-2040" (of 2048, wraps).
    std::string regions;
};

/// [WP-STEADY] One named --regions window.
struct NamedRegion {
    std::string name;
    int c0 = 0;
    int c1 = 0;
};

/// [WP-STEADY] Parse "name:c0-c1,name:c0-c1,..." into windows of a band
/// `width` columns wide; false (with `why`) on anything malformed.
bool parseRegions(const std::string& text, std::uint32_t width, std::vector<NamedRegion>& out, std::string& why) {
    out.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? text.size() : comma + 1;
        const std::size_t colon = item.find(':');
        NamedRegion r;
        if (colon == std::string::npos || colon == 0 ||
            std::sscanf(item.c_str() + colon + 1, "%d-%d", &r.c0, &r.c1) != 2 || r.c0 < 0 || r.c1 < 0 ||
            r.c0 >= static_cast<int>(width) || r.c1 >= static_cast<int>(width)) {
            why = "--regions expects name:c0-c1[,name:c0-c1...] with columns inside 0-" + std::to_string(width - 1);
            return false;
        }
        r.name = item.substr(0, colon);
        out.push_back(r);
    }
    return true;
}

/// [WP-PHOTO] Measure the field on `pair` and score the four variants.
/// Returns the JSON block for out["photo"].
nlohmann::json photoSeamReport(const Pipeline& P, const video::FramePair& pair, const SeamOptions& o) {
    nlohmann::json ph;
    render::PhotoSeamParams pp = o.photoTuning;
    pp.mode = render::PhotoSeamMode::RimAndGain;
    auto measured = render::measurePhotoSeam(P.rig, pair, P.blendParams, pp, *P.pool);
    if (!measured.ok()) {
        ph["error"] = measured.error().message;
        return ph;
    }
    const render::PhotoSeamField& f = measured.value();
    ph["gridW"] = f.w;
    ph["gridH"] = f.h;
    ph["latSpanDeg"] = {rad2deg(static_cast<double>(f.latMinRad)), rad2deg(static_cast<double>(f.latMaxRad))};
    ph["trustedPixels"] = f.trustedPixels;
    ph["bandPixels"] = f.bandPixels;
    ph["rimMedianDeg"] = {f.rimMedianDeg[0], f.rimMedianDeg[1]};
    ph["medianLog2Gain"] = {f.medianLog2Gain[0], f.medianLog2Gain[1], f.medianLog2Gain[2]};
    ph["bandMs"] = f.bandMs;
    ph["statsMs"] = f.statsMs;
    // The usable rim per grid column (degrees; the raw measurement is null
    // where a column was not measured) - WP-SEAM's seam cost, and the thing
    // to look at when a clip's rim is in doubt.
    for (int lens = 0; lens < 2; ++lens) {
        nlohmann::json rim = nlohmann::json::array();
        nlohmann::json raw = nlohmann::json::array();
        for (std::uint32_t g = 0; g < f.w; ++g) {
            const std::size_t k = static_cast<std::size_t>(g) * 2u + static_cast<std::size_t>(lens);
            rim.push_back(rad2deg(static_cast<double>(f.rim[k])));
            const float m = k < f.rimMeasured.size() ? f.rimMeasured[k] : std::nanf("");
            raw.push_back(std::isfinite(m) ? nlohmann::json(rad2deg(static_cast<double>(m))) : nlohmann::json());
        }
        ph["rimDeg" + std::to_string(lens)] = rim;
        ph["rimMeasuredDeg" + std::to_string(lens)] = raw;
    }

    // Column window of the metrics, as fractions of the band width.
    double c0 = 0.20;
    double c1 = 0.44;
    int r0 = 0, r1 = 0;
    if (!o.region.empty() && std::sscanf(o.region.c_str(), "%d-%d", &r0, &r1) == 2 && r0 >= 0 && r1 > r0 &&
        r1 < 2048) {
        c0 = static_cast<double>(r0) / 2048.0;
        c1 = static_cast<double>(r1 + 1) / 2048.0;
    }
    ph["regionFrac"] = {c0, c1};

    // The four variants, every one rendered through the shared kernel.
    const geom::BlendParams inset = render::insetRenderBlend(P.blendParams, render::kDefaultSeamInsetDeg);
    auto g = render::estimateGain(P.rig, pair, P.blendParams, render::BandParams{}, *P.pool);
    const Vec3d g0 = g.ok() ? g.value().gain[0] : Vec3d{1, 1, 1};
    const Vec3d g1 = g.ok() ? g.value().gain[1] : Vec3d{1, 1, 1};
    render::PhotoSeamParams rimOnly = pp;
    rimOnly.mode = render::PhotoSeamMode::RimOnly;
    struct Variant {
        const char* name;
        render::RenderParamsBuilder builder;
    };
    Variant variants[4] = {{"off", {}}, {"inset", {}}, {"rim", {}}, {"full", {}}};
    variants[0].builder.rig(P.rig).blend(P.blendParams, true);
    variants[1].builder.rig(P.rig).blend(inset, true).gain(g0, g1);
    variants[2].builder.rig(P.rig).blend(P.blendParams, true).gain(g0, g1).photo(f, rimOnly);
    variants[3].builder.rig(P.rig).blend(P.blendParams, true).photo(f, pp);
    render::MetricBandRequest req;
    req.mapW = o.photoMetricW;
    std::vector<std::uint8_t> trust;
    nlohmann::json vj;
    double base[4] = {0, 0, 0, 0};
    for (int v = 0; v < 4; ++v) {
        auto bands = render::renderMetricBands(variants[v].builder, pair, req, *P.pool);
        if (!bands.ok()) {
            vj[variants[v].name] = {{"error", bands.error().message}};
            continue;
        }
        if (v == 0) {
            // One trust mask for every variant: co-valid in the uncorrected
            // render and inside both usable rims.
            trust = render::rimTrustMask(bands.value(), P.rig, f);
        }
        auto m = render::skySeamMetrics(bands.value(), trust, c0, c1);
        if (!m.ok()) {
            vj[variants[v].name] = {{"error", m.error().message}};
            continue;
        }
        const double vals[4] = {m.value().line, m.value().band, m.value().broad, m.value().dE};
        if (v == 0) {
            for (int k = 0; k < 4; ++k) {
                base[k] = vals[k];
            }
        }
        const auto ratio = [&](int k) { return base[k] > 0.0 ? vals[k] / base[k] : 0.0; };
        vj[variants[v].name] = {{"line", vals[0]},       {"band", vals[1]},       {"broad", vals[2]},
                                {"dE", vals[3]},         {"lineX", ratio(0)},     {"bandX", ratio(1)},
                                {"broadX", ratio(2)},    {"dEX", ratio(3)},       {"trusted", m.value().trustedPixels}};
    }
    ph["metrics"] = vj;
    return ph;
}

/// Write one float plane as raw little-endian float32.  Diagnostic only.
bool writeRawPlane(const std::string& path, const std::vector<float>& plane) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const std::size_t n = std::fwrite(plane.data(), sizeof(float), plane.size(), f);
    std::fclose(f);
    return n == plane.size();
}

/// How well the two lenses agree over a column window of the band.
struct RegionScore {
    double ncc = 0.0;          ///< Normalised cross-correlation of the two lens lumas.
    double meanAbsDiff = 0.0;  ///< Mean |luma0 - luma1| (code values): the ghost amplitude.
    std::uint64_t samples = 0; ///< Co-visible pixels scored.
};

/// Score columns [c0, c1] of `b`, wrapping when c1 < c0.  Only co-visible
/// pixels count, exactly as in render::overlapNcc.
RegionScore scoreRegion(const render::LensBands& b, int c0, int c1) {
    RegionScore s;
    if (b.w == 0 || b.h == 0 || b.luma[0].size() != static_cast<std::size_t>(b.w) * b.h ||
        b.luma[1].size() != b.luma[0].size() || b.alpha[0].size() != b.luma[0].size() ||
        b.alpha[1].size() != b.luma[0].size()) {
        return s;
    }
    const int W = static_cast<int>(b.w);
    const int span = c1 >= c0 ? c1 - c0 + 1 : (W - c0) + c1 + 1;
    std::vector<double> xa, xb;
    for (std::uint32_t r = 0; r < b.h; ++r) {
        for (int k = 0; k < span; ++k) {
            const int c = ((c0 + k) % W + W) % W;
            const std::size_t i = static_cast<std::size_t>(r) * b.w + static_cast<std::size_t>(c);
            if (b.alpha[0][i] > 0.5f && b.alpha[1][i] > 0.5f) {
                xa.push_back(b.luma[0][i]);
                xb.push_back(b.luma[1][i]);
            }
        }
    }
    s.samples = xa.size();
    if (xa.size() < 16) {
        return s;
    }
    double ma = 0, mb = 0, mad = 0;
    for (std::size_t i = 0; i < xa.size(); ++i) {
        ma += xa[i];
        mb += xb[i];
        mad += std::fabs(xa[i] - xb[i]);
    }
    const double n = static_cast<double>(xa.size());
    ma /= n;
    mb /= n;
    double num = 0, da = 0, db = 0;
    for (std::size_t i = 0; i < xa.size(); ++i) {
        num += (xa[i] - ma) * (xb[i] - mb);
        da += (xa[i] - ma) * (xa[i] - ma);
        db += (xb[i] - mb) * (xb[i] - mb);
    }
    s.ncc = (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
    s.meanAbsDiff = mad / n;
    return s;
}

/// Rebuild the rig with a different scale / extrinsic sense for comparison.
///
/// The rig describes ONE lens image, so it is derived from lensW()/lensH(),
/// exactly as Pipeline.cpp builds the main rig.  The track size is wrong for
/// an LRF: its single 2048 x 1024 side-by-side track matches no rule, fell
/// through to digital_focal_length / fx (the 6K or 8K parent's scale on a
/// 1024 px half), and built a rig 2048 px wide that the 1024 px halves the
/// reader delivers never matched - every alternative scored -2.
///
/// @param P              The opened pipeline (format, calibration, main rig).
/// @param scaleOverride  Sensor -> lens-image scale to force, or nullopt for
///                       the clip's own rule.
/// @param sense          Extrinsic rotation sense to build with.
/// @param order          Quaternion component order to build with.
/// @return The rebuilt rig, or the error StreamScaling / LensRig reported.
Result<geom::LensRig> variantRig(const Pipeline& P, std::optional<double> scaleOverride, geom::RotationSense sense,
                                 geom::QuatOrder order) {
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(P.format.lensW()), static_cast<int>(P.format.lensH()),
                                               static_cast<int>(P.format.sensorW), static_cast<int>(P.format.sensorH),
                                               P.format.digitalFocalLength,
                                               0.5 * (P.calibration.slave.fx + P.calibration.master.fx), scaleOverride));
    geom::ExtrinsicConvention conv;
    conv.sense = sense;
    conv.order = order;
    return geom::LensRig::build(P.calibration, scaling, P.rig.focalSource, P.format.digitalFocalLength, conv,
                                P.rig.lensFovDeg);
}

int runSeam(const SeamOptions& o) {
    // Every seam measurement shades bands from host planes.
    PipelineOptions pipelineOptions = o.pipeline;
    pipelineOptions.hostFramesRequired = true;
    auto pipe = Pipeline::open(pipelineOptions, false);
    if (!pipe.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(pipe.error().toString()).c_str());
        return pipe.error().code == ErrorCode::Io ? kExitInput : kExitRuntime;
    }
    Pipeline& P = *pipe.value();
    if (o.frame < 0 || static_cast<std::uint32_t>(o.frame) >= P.frameCount()) {
        std::fprintf(stderr, "error: frame %d out of range\n", o.frame);
        return kExitUsage;
    }
    nlohmann::json out;

    // ---- [WP-STEADY] the flow backend, needed by the per-clip analyses too ----
    render::FlowBackendKind backend = render::FlowBackendKind::Auto;
    if (o.flowBackend == "classical") {
        backend = render::FlowBackendKind::Classical;
    } else if (o.flowBackend == "neural") {
        backend = render::FlowBackendKind::Neural;
    } else if (o.flowBackend != "auto") {
        std::fprintf(stderr, "error: unknown --flow-backend '%s'\n", log::safe(o.flowBackend).c_str());
        return kExitUsage;
    }
    std::vector<NamedRegion> regions;
    if (!o.regions.empty()) {
        std::string why;
        if (!parseRegions(o.regions, o.parallaxTuning.band.equirectW, regions, why)) {
            std::fprintf(stderr, "error: %s\n", why.c_str());
            return kExitUsage;
        }
    }
    // The clip's own frames for the per-clip analyses, in ascending order.
    const render::ClipFrameSource clipFrames = [&P](std::uint32_t f) { return P.reader->read(f); };

    // ---- [WP-STEADY] --lens-align: fit the clip's lens rotation, fold it ------
    // First, so every number below - the overlap NCC, the grids, the steady
    // correction - is measured on the aligned rig, as the importer renders.
    if (o.lensAlign) {
        render::ParallaxWarpParams rp = o.parallaxTuning;
        rp.backend = backend;
        const std::vector<std::uint32_t> frames = render::clipSampleFrames(
            P.frameCount(), P.syncFrames(), render::kLensRotationSamples, 0.1, 0.9);
        const auto t0 = std::chrono::steady_clock::now();
        auto measured = render::measureLensRotation(P.rig, P.blendParams, frames, clipFrames, rp,
                                                    render::LensRotationParams{}, *P.pool);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        nlohmann::json lj;
        lj["frames"] = frames;
        lj["ms"] = ms;
        if (!measured.ok()) {
            lj["error"] = measured.error().message;
        } else {
            const render::LensRotationMeasurement& m = measured.value();
            lj["accepted"] = m.accepted;
            lj["decodeMs"] = m.decodeMs;
            lj["analysisMs"] = m.analysisMs;
            nlohmann::json per = nlohmann::json::array();
            for (const render::LensRotationFit& f : m.perFrame) {
                per.push_back({{"w", {f.wRad.x, f.wRad.y, f.wRad.z}},
                               {"angleDeg", f.angleDeg},
                               {"residualDeg", f.residualRmsDeg},
                               {"inliers", f.inliers},
                               {"cells", f.cells},
                               {"conditioning", f.conditioning}});
            }
            lj["perFrame"] = per;
            lj["refusals"] = m.refusals;
            if (m.accepted) {
                lj["w"] = {m.fit.wRad.x, m.fit.wRad.y, m.fit.wRad.z};
                lj["angleDeg"] = m.fit.angleDeg;
                lj["spreadDeg"] = m.fit.spreadDeg;
                lj["residualDeg"] = m.fit.residualRmsDeg;
                lj["summary"] = render::describeLensRotation(m.fit);
                const Status folded = render::applyLensRotation(P.rig, m.fit.wRad);
                lj["applied"] = folded.ok();
            } else {
                lj["reason"] = m.reason;
                lj["applied"] = false;
            }
        }
        out["lensAlign"] = lj;
    }

    // ---- [WP-STEADY] --steady: the per-clip correction on the fixed samples ----
    std::optional<render::ClipSteady> steady;
    if (o.steady) {
        render::ClipSteadyParams cp;
        cp.parallax = o.parallaxTuning;
        cp.parallax.backend = backend;
        cp.parallaxOn = true;
        cp.seamOn = true;
        const std::vector<std::uint32_t> frames =
            render::clipSampleFrames(P.frameCount(), P.syncFrames(), render::kClipSteadySamples);
        const auto t0 = std::chrono::steady_clock::now();
        auto measured = render::measureClipSteady(P.rig, P.blendParams, frames, clipFrames, cp, *P.pool);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        nlohmann::json sj;
        sj["frames"] = frames;
        sj["ms"] = ms;
        if (!measured.ok()) {
            sj["error"] = measured.error().message;
        } else {
            const render::ClipSteady& c = measured.value();
            sj["acceptedGrids"] = c.acceptedGrids;
            sj["decodeMs"] = c.decodeMs;
            sj["measureMs"] = c.measureMs;
            sj["finishMs"] = c.finishMs;
            sj["grid"] = c.grid != nullptr;
            if (c.grid) {
                sj["meanDisparityDeg"] = c.grid->meanAbsCorrectionDeg;
                sj["maxDisparityDeg"] = c.grid->maxAbsCorrectionDeg;
                // The median of the accepted samples' structured-gate
                // strengths; below 1 the clip seam table fills the rest.
                sj["gridStrength"] = c.grid->strength;
                sj["structuredFraction"] = c.grid->structuredFraction();
            }
            sj["seamTable"] = c.seamTable != nullptr;
            // The table share under the clip grid and the columns the
            // per-column guard handed to it (render::guardGridWithTable).
            sj["gridTable"] = c.gridTable != nullptr && !c.gridTable->empty();
            sj["guardedColumns"] = c.guardedColumns;
            sj["seam"] = c.seam != nullptr;
            sj["decision"] = {{"steady", c.decision.steady},
                              {"textured", c.decision.textured},
                              {"judged", c.decision.judged},
                              {"failed", c.decision.failed},
                              {"worstKeep", c.decision.worstKeep},
                              {"worstLoss", c.decision.worstLoss},
                              {"meanLoss", c.decision.meanLoss},
                              {"worstFrame", c.decision.worstFrame},
                              {"worstLonDeg", c.decision.worstLonDeg},
                              {"summary", render::describeSteadyDecision(c.decision)}};
            nlohmann::json scores = nlohmann::json::array();
            for (const render::SteadySectorScore& r : c.decision.scores) {
                scores.push_back({r.frame, r.sector, std::isfinite(r.none) ? r.none : -2.0, r.own, r.clip});
            }
            sj["decision"]["scores"] = scores;  // [frame, sector, none, own, clip]
            steady = measured.value();
        }
        out["steady"] = sj;
    }

    auto pair = P.reader->read(static_cast<std::uint32_t>(o.frame));
    if (!pair.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(pair.error().toString()).c_str());
        return kExitRuntime;
    }

    render::BandParams band;
    band.bandHalfDeg = 4.0;
    out["frame"] = o.frame;

    auto current = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool);
    if (!current.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(current.error().toString()).c_str());
        return kExitRuntime;
    }
    out["ncc"] = current.value();

    // Alternatives table.
    struct Variant {
        const char* name;
        std::optional<double> scale;
        geom::RotationSense sense;
        geom::QuatOrder order;
    };
    // "No crop": the lens image shows the whole sensor width.  Relative to
    // the lens image, like variantRig itself: 3000 / 3840 = 0.78125 on the
    // 6K clip (the value this row always had), 1024 / 3840 on an LRF half,
    // 1.0 on an 8K stream.  A fixed 0.78125 on a 1024 px half put the whole
    // overlap off the image.  A clip without a sensor size gets 0, which
    // StreamScaling refuses, so its row reads -2 instead of a made-up scale.
    const double noCropScale = P.format.sensorW > 0 ? static_cast<double>(P.format.lensW()) /
                                                          static_cast<double>(P.format.sensorW)
                                                    : 0.0;
    char noCropName[64] = {};
    std::snprintf(noCropName, sizeof(noCropName), "scale %.5f (no crop)", noCropScale);
    const Variant variants[] = {
        {noCropName, noCropScale, geom::RotationSense::BodyToLens, geom::QuatOrder::WXYZ},
        {"extrinsic lens2body", std::nullopt, geom::RotationSense::LensToBody, geom::QuatOrder::WXYZ},
        {"extrinsic xyzw", std::nullopt, geom::RotationSense::BodyToLens, geom::QuatOrder::XYZW},
    };
    nlohmann::json alt = nlohmann::json::array();
    for (const Variant& v : variants) {
        auto rig = variantRig(P, v.scale, v.sense, v.order);
        double ncc = -2.0;
        if (rig.ok()) {
            auto r = render::overlapNcc(rig.value(), pair.value(), P.blendParams, band, *P.pool);
            if (r.ok()) {
                ncc = r.value();
            }
        }
        alt.push_back({{"variant", v.name}, {"ncc", ncc}});
    }
    out["alternatives"] = alt;

    // The seam table, kept for the band dump and the parallax measurement,
    // with its per-column confidence for the per-column guard.
    std::vector<float> seamHold;
    std::vector<float> seamConfHold;
    if (o.search) {
        render::SeamSearchParams sp;
        auto profile = render::searchSeam(P.rig, pair.value(), P.blendParams, sp, *P.pool);
        if (profile.ok()) {
            const render::SeamProfile& p = profile.value();
            // The raw measurement is NaN where no shift could be scored:
            // null in the JSON, which has no NaN.
            nlohmann::json measured = nlohmann::json::array();
            for (const float v : p.measuredDeg) {
                measured.push_back(std::isfinite(v) ? nlohmann::json(v) : nlohmann::json());
            }
            out["search"] = {{"meanNcc", p.meanNcc},
                             {"acceptedColumns", p.acceptedColumns},
                             {"columns", p.columns},
                             {"unmeasuredColumns", p.unmeasuredColumns},
                             {"confidentColumns", p.confidentColumns},
                             {"meanConfidence", p.meanConfidence},
                             {"shiftDeg", p.shiftDeg},
                             {"confidence", p.confidence},
                             {"measuredDeg", measured}};
            auto after = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, &p.shiftDeg);
            if (after.ok()) {
                out["nccAfterSearch"] = after.value();
            }
            seamHold = p.shiftDeg;
            seamConfHold = p.confidence;
        } else {
            out["searchError"] = profile.error().message;  // no "search" block: nothing was measured
        }
    }
    const std::vector<float>* seamIn = seamHold.empty() ? nullptr : &seamHold;

    // ---- --dump-bands: the per-lens bands as raw float32 planes ------------------
    // "raw" is the uncorrected pair, "table" the same band with the seam
    // table applied (when --search ran), "parallax" (below) with the grid on
    // top of the table, as the composed corrections render.  Written on the
    // ANALYSIS band (--parallax-band-deg), so a dump can be widened to show
    // content beyond the scored band.
    const auto writeBands = [&](const char* tag, const std::vector<float>* seamT, const render::WarpGridView* w) {
        auto bands = render::renderLensBands(P.rig, pair.value(), P.blendParams, o.parallaxTuning.band, false, seamT,
                                             *P.pool, w);
        if (!bands.ok()) {
            out["dumpError"] = bands.error().message;
            return;
        }
        const render::LensBands& b = bands.value();
        bool written = true;
        for (int lens = 0; lens < 2; ++lens) {
            const std::string stem = o.dumpBands + "_" + tag + "_";
            const std::string suffix = std::to_string(lens) + ".f32";
            written = writeRawPlane(stem + "luma" + suffix, b.luma[lens]) && written;
            written = writeRawPlane(stem + "alpha" + suffix, b.alpha[lens]) && written;
        }
        if (!written) {
            out["dumpError"] = std::string("could not write the ") + tag + " planes";
        }
        out["dump"] = {{"w", b.w}, {"h", b.h}, {"rowOffset", b.rowOffset}};
    };
    if (!o.dumpBands.empty()) {
        writeBands("raw", nullptr, nullptr);
        if (seamIn != nullptr) {
            writeBands("table", seamIn, nullptr);
        }
    }

    // ---- 2-D parallax correction -------------------------------------------
    // Measured on the residual the seam table leaves behind when --search was
    // also asked for, so the "after" number describes the two corrections
    // composed, which is how they are actually rendered.  The NCC is taken on
    // the SAME band as the "before" number, with the warp applied by the
    // kernel itself, so it measures the real render path.
    if (o.parallax) {
        render::ParallaxWarpParams pw = o.parallaxTuning;
        pw.backend = backend;  // parsed (and validated) above
        const auto t0 = std::chrono::steady_clock::now();
        // buildParallaxWarp's two halves, so the gate's counts (filled with
        // the cells) are there to report when the gate refuses the grid.
        render::ParallaxCellStats cells;
        auto grid = [&]() -> Result<render::ParallaxWarpGrid> {
            const auto tBand = std::chrono::steady_clock::now();
            OSV_TRY_ASSIGN(render::LensBands bands, render::measureParallaxBands(P.rig, pair.value(), P.blendParams,
                                                                                pw, seamIn, *P.pool));
            const double bandMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBand).count();
            return render::parallaxFromBands(bands, pw, P.pool.get(), bandMs, &cells);
        }();
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        nlohmann::json pj;
        pj["buildMs"] = buildMs;
        // The uncorrected pair ("raw", and "table" with --search) was dumped
        // above, whether or not the grid is refused - a refused grid is
        // exactly the case worth looking at.
        if (out.contains("dump")) {
            pj["dump"] = out["dump"];
        }
        // ---- the structured gate's numbers, accepted or refused ----------------
        // Both shares (all co-visible pixels - the retired gate's number - and
        // the structured ones the gate judges), the structured pixel count and
        // the strength: 0 on a refusal, so a JSON consumer never has to parse
        // the error text.  Absent only when the flow never ran (no cells).
        const auto putGate = [&](std::uint64_t covisible, std::uint64_t consistent, std::uint64_t structured,
                                 std::uint64_t consistentStructured, double strength) {
            const double all = covisible ? static_cast<double>(consistent) / static_cast<double>(covisible) : 0.0;
            pj["consistentFraction"] = all;
            pj["structuredFraction"] =
                structured ? static_cast<double>(consistentStructured) / static_cast<double>(structured) : 0.0;
            pj["structuredPixels"] = structured;
            pj["covisiblePixels"] = covisible;
            pj["strength"] = strength;
            // Whether the all-pixel rule this gate replaced would have kept it.
            pj["allPixelGate"] = all >= pw.minConsistentFraction;
        };
        if (grid.ok()) {
            const render::ParallaxWarpGrid& g = grid.value();
            putGate(g.totalPixels, g.consistentPixels, g.structuredPixels, g.consistentStructuredPixels, g.strength);
        } else if (cells.valid()) {
            putGate(cells.covisiblePixels, cells.consistentPixels, cells.structuredPixels,
                    cells.consistentStructuredPixels, 0.0);
        }
        // Region score: the same column window uncorrected and with the seam
        // table (when searched) whatever the gate decided - a refused bucket
        // renders exactly that table - and with the parallax grid on top when
        // there is one (below).
        int rc0 = 0, rc1 = 0;
        const bool regionAsked = !o.region.empty() && std::sscanf(o.region.c_str(), "%d-%d", &rc0, &rc1) == 2 &&
                                 rc0 >= 0 && rc1 >= 0 && rc0 < static_cast<int>(band.equirectW) &&
                                 rc1 < static_cast<int>(band.equirectW);
        nlohmann::json rj;
        const auto score = [&](const char* name, const std::vector<float>* seamT, const render::WarpGridView* w) {
            auto bands = render::renderLensBands(P.rig, pair.value(), P.blendParams, band, false, seamT, *P.pool, w);
            if (bands.ok()) {
                const RegionScore s = scoreRegion(bands.value(), rc0, rc1);
                rj[name] = {{"ncc", s.ncc}, {"meanAbsDiff", s.meanAbsDiff}, {"samples", s.samples}};
            }
        };
        if (regionAsked) {
            score("none", nullptr, nullptr);
            if (seamIn) {
                score("seam", seamIn, nullptr);
            }
        }
        if (grid.ok()) {
            const render::ParallaxWarpGrid& g = grid.value();
            render::WarpGridView view;
            view.uv = g.uv.data();
            view.w = g.w;
            view.h = g.h;
            view.latMinRad = g.latMinRad;
            view.latMaxRad = g.latMaxRad;
            if (!o.dumpBands.empty()) {
                writeBands("parallax", seamIn, &view);
                writeRawPlane(o.dumpBands + "_grid.f32", g.uv);
            }
            pj["backend"] = render::flowBackendName(g.usedBackend);
            pj["gridW"] = g.w;
            pj["gridH"] = g.h;
            pj["meanDisparityDeg"] = g.meanAbsCorrectionDeg;
            pj["maxDisparityDeg"] = g.maxAbsCorrectionDeg;
            pj["bandMs"] = g.bandMs;
            pj["flowMs"] = g.flowMs;
            pj["gridMs"] = g.gridMs;
            pj["measuredCells"] = g.measuredCells;
            pj["gatedCells"] = g.gatedCells;
            auto after = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, seamIn, &view);
            if (after.ok()) {
                pj["nccAfter"] = after.value();
            } else {
                pj["nccError"] = after.error().message;
            }
            // [WP-STEADY] The same with the clip's steady grid instead of this
            // frame's own, rendered by the kernel on the same band.
            render::WarpGridView steadyView;
            if (steady && steady->grid && steady->grid->valid()) {
                steadyView.uv = steady->grid->uv.data();
                steadyView.w = steady->grid->w;
                steadyView.h = steady->grid->h;
                steadyView.latMinRad = steady->grid->latMinRad;
                steadyView.latMaxRad = steady->grid->latMaxRad;
                auto held = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, seamIn, &steadyView);
                if (held.ok()) {
                    pj["nccAfterSteady"] = held.value();
                }
                // The clip correction exactly as the importer renders it: the
                // clip grid (after the per-column guard) and the clip table
                // share under it (ClipSteady::gridTable), not this frame's table.
                const std::vector<float>* clipShare =
                    (steady->gridTable && !steady->gridTable->empty()) ? steady->gridTable.get() : nullptr;
                auto asRendered =
                    render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, clipShare, &steadyView);
                if (asRendered.ok()) {
                    pj["nccAfterClip"] = asRendered.value();
                }
            }
            // [WP-STEADY] --regions: every named window uncorrected, with this
            // frame's grid and (with --steady) with the clip grid, each
            // rendered through the kernel on the scored band.
            if (!regions.empty()) {
                const auto renderScored = [&](const render::WarpGridView* w) {
                    return render::renderLensBands(P.rig, pair.value(), P.blendParams, band, false, seamIn, *P.pool,
                                                   w);
                };
                auto none = render::renderLensBands(P.rig, pair.value(), P.blendParams, band, false, nullptr,
                                                    *P.pool);
                auto own = renderScored(&view);
                std::optional<Result<render::LensBands>> held;
                if (steadyView.valid()) {
                    held = renderScored(&steadyView);
                }
                nlohmann::json rs;
                for (const NamedRegion& r : regions) {
                    nlohmann::json one;
                    const auto put = [&](const char* key, const Result<render::LensBands>& b) {
                        if (b.ok()) {
                            const RegionScore s = scoreRegion(b.value(), r.c0, r.c1);
                            one[key] = {{"ncc", s.ncc}, {"meanAbsDiff", s.meanAbsDiff}, {"samples", s.samples}};
                        }
                    };
                    put("none", none);
                    put("parallax", own);
                    if (held) {
                        put("steady", *held);
                    }
                    rs[r.name] = one;
                }
                pj["regions"] = rs;
            }
            // The region with the parallax grid on top of the table.
            if (regionAsked) {
                score("parallax", seamIn, &view);
            }
        } else {
            pj["error"] = grid.error().message;
        }
        if (regionAsked) {
            pj["region"] = rj;
        }

        // ---- the importer's policy: the grid on RAW bands, guarded by the table -------
        // With --search the grid above is measured on table-corrected bands
        // (the two composed), which is NOT what the plug-ins render: they
        // measure the grid on the uncorrected bands, let it replace the table,
        // and hand back to the table only the columns the grid could not
        // measure, where the table is sure and found a disparity the grid
        // missed (render::guardGridWithTable).  So the shipped picture is
        // measured here as well, through the same calls, and scored on the
        // same band and window as the rest.
        if (seamIn != nullptr) {
            nlohmann::json gj;
            auto raw = [&]() -> Result<render::ParallaxWarpGrid> {
                OSV_TRY_ASSIGN(render::LensBands bands, render::measureParallaxBands(P.rig, pair.value(),
                                                                                    P.blendParams, pw, nullptr,
                                                                                    *P.pool));
                return render::parallaxFromBands(bands, pw, P.pool.get());
            }();
            if (raw.ok()) {
                const render::ParallaxWarpGrid& rg = raw.value();
                auto guarded = render::guardGridWithTable(rg, seamHold, seamConfHold);
                if (guarded.ok()) {
                    const render::GuardedCorrection& gc = guarded.value();
                    // The guarded grid when the guard took columns, else the
                    // measured one, exactly as the importer renders.
                    const render::ParallaxWarpGrid& shown = gc.changed ? gc.grid : rg;
                    render::WarpGridView gv;
                    gv.uv = shown.uv.data();
                    gv.w = shown.w;
                    gv.h = shown.h;
                    gv.latMinRad = shown.latMinRad;
                    gv.latMaxRad = shown.latMaxRad;
                    // An empty share renders no table at all, as the importer does.
                    const std::vector<float>* share = gc.table.empty() ? nullptr : &gc.table;
                    gj["strength"] = rg.strength;
                    gj["guardedColumns"] = gc.guardedColumns;
                    gj["meanGuard"] = gc.meanGuard;
                    gj["changed"] = gc.changed;
                    // Per grid column: what the guard weighed, what it decided
                    // and the grid's own along-meridian correction (as a table
                    // value, degrees).
                    gj["untrustedShare"] = rg.untrustedShare;
                    gj["guard"] = gc.guard;
                    gj["gridAlongDeg"] = gc.gridAlongDeg;
                    auto after = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, share, &gv);
                    if (after.ok()) {
                        gj["nccAfter"] = after.value();
                    }
                    if (regionAsked) {
                        auto bands =
                            render::renderLensBands(P.rig, pair.value(), P.blendParams, band, false, share, *P.pool,
                                                    &gv);
                        if (bands.ok()) {
                            const RegionScore s = scoreRegion(bands.value(), rc0, rc1);
                            gj["region"] = {{"ncc", s.ncc}, {"meanAbsDiff", s.meanAbsDiff}, {"samples", s.samples}};
                        }
                    }
                } else {
                    gj["error"] = guarded.error().message;
                }
            } else {
                // Refused on the raw bands: the importer renders the table alone.
                gj["refused"] = raw.error().message;
                gj["nccAfter"] = out.value("nccAfterSearch", -1.0);
                if (regionAsked && rj.contains("seam")) {
                    gj["region"] = rj["seam"];
                }
            }
            pj["guarded"] = gj;
        }
        out["parallax"] = pj;
    }

    if (o.photo) {
        out["photo"] = photoSeamReport(P, pair.value(), o);  // [WP-PHOTO]
    }

    if (o.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("frame %d overlap NCC: %.4f (verified conventions)\n", o.frame, current.value());
        for (const auto& a : alt) {
            std::printf("  %-28s NCC %.4f\n", a["variant"].get<std::string>().c_str(), a["ncc"].get<double>());
        }
        if (out.contains("search")) {
            std::printf("  seam search: meanNcc %.4f, accepted %u/%u columns (%u confident, %u unmeasured, mean "
                        "confidence %.3f), NCC after %.4f\n",
                        out["search"]["meanNcc"].get<double>(), out["search"]["acceptedColumns"].get<unsigned>(),
                        out["search"]["columns"].get<unsigned>(), out["search"]["confidentColumns"].get<unsigned>(),
                        out["search"]["unmeasuredColumns"].get<unsigned>(),
                        out["search"]["meanConfidence"].get<double>(), out.value("nccAfterSearch", -1.0));
        }
        if (out.contains("photo")) {
            const nlohmann::json& ph = out["photo"];
            if (ph.contains("error")) {
                std::printf("  photometric seam field: unavailable (%s)\n", ph["error"].get<std::string>().c_str());
            } else {
                std::printf("  photometric seam field: usable rim %.2f / %.2f deg, median gain %+.3f / %+.3f / %+.3f "
                            "stops, %.1f + %.1f ms\n",
                            ph["rimMedianDeg"][0].get<double>(), ph["rimMedianDeg"][1].get<double>(),
                            ph["medianLog2Gain"][0].get<double>(), ph["medianLog2Gain"][1].get<double>(),
                            ph["medianLog2Gain"][2].get<double>(), ph["bandMs"].get<double>(),
                            ph["statsMs"].get<double>());
                for (const char* name : {"off", "inset", "rim", "full"}) {
                    if (!ph["metrics"].contains(name) || ph["metrics"][name].contains("error")) {
                        continue;
                    }
                    const nlohmann::json& m = ph["metrics"][name];
                    std::printf("    %-6s line %6.1f  band %6.1f  broad %6.1f  dE %6.1f  (x%.2f x%.2f x%.2f x%.2f)\n",
                                name, m["line"].get<double>(), m["band"].get<double>(), m["broad"].get<double>(),
                                m["dE"].get<double>(), m["lineX"].get<double>(), m["bandX"].get<double>(),
                                m["broadX"].get<double>(), m["dEX"].get<double>());
                }
            }
        }
        if (out.contains("parallax")) {
            const nlohmann::json& pj = out["parallax"];
            if (pj.contains("error")) {
                std::printf("  parallax: unavailable (%s), %.0f ms\n", pj["error"].get<std::string>().c_str(),
                            pj["buildMs"].get<double>());
            } else {
                std::printf("  parallax (%s, grid %ux%u): consistent %.1f%%, structured %.1f%% of %llu px, strength "
                            "%.2f, disparity mean %.3f / max %.3f deg, %.0f ms, NCC after %.4f\n",
                            pj["backend"].get<std::string>().c_str(), pj["gridW"].get<unsigned>(),
                            pj["gridH"].get<unsigned>(), 100.0 * pj["consistentFraction"].get<double>(),
                            100.0 * pj["structuredFraction"].get<double>(),
                            static_cast<unsigned long long>(pj["structuredPixels"].get<std::uint64_t>()),
                            pj["strength"].get<double>(), pj["meanDisparityDeg"].get<double>(),
                            pj["maxDisparityDeg"].get<double>(), pj["buildMs"].get<double>(),
                            pj.value("nccAfter", -1.0));
            }
        }
    }
    return current.value() >= 0.8 ? kExitOk : kExitRuntime;
}

}  // namespace

void registerSeamCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<SeamOptions>();
    CLI::App* sub = app.add_subcommand("seam", "Measure lens overlap alignment (NCC) and alternatives");
    addPipelineOptions(sub, opt->pipeline);
    sub->add_option("--frame", opt->frame, "Frame index")->default_val(0);
    sub->add_flag("--json", opt->json, "JSON output");
    sub->add_flag("--search", opt->search, "Also run the per-column seam search");
    sub->add_flag("--parallax", opt->parallax, "Also measure the 2-D optical-flow parallax correction");
    sub->add_option("--flow-backend", opt->flowBackend, "auto|classical|neural")->default_str("auto");
    sub->add_option("--region", opt->region, "Also score band columns c0-c1 (of 2048, wraps) with --parallax");
    sub->add_option("--dump-bands", opt->dumpBands,
                    "Write raw float32 lens bands to this path prefix: _raw_ (uncorrected), _table_ (with the "
                    "--search table), _parallax_ (with the --parallax grid) (diagnostic)");
    // [WP-STEADY]
    sub->add_flag("--lens-align", opt->lensAlign,
                  "Fit the clip's lens rotation on its fixed rotation frames and fold it into the rig first");
    sub->add_flag("--steady", opt->steady,
                  "Measure the per-clip steady correction and score it next to this frame's own grid (--parallax)");
    sub->add_option("--regions", opt->regions,
                    "Named column windows scored with --parallax: name:c0-c1[,name:c0-c1...] (of 2048, wraps)");
    sub->add_flag("--photo", opt->photo,
                  "Measure the photometric seam field and score the sky seam (off / inset / rim / full) over --region");
    sub->add_option("--photo-band-w", opt->photoTuning.band.equirectW, "Photo analysis band width (tuning)")
        ->default_val(opt->photoTuning.band.equirectW);
    sub->add_option("--photo-flat", opt->photoTuning.flatLog2PerDeg, "Photo texture gate, stops per degree (tuning)")
        ->default_val(opt->photoTuning.flatLog2PerDeg);
    sub->add_option("--photo-drop", opt->photoTuning.rimDropStops, "Photo rim departure, stops (tuning)")
        ->default_val(opt->photoTuning.rimDropStops);
    sub->add_option("--photo-decay", opt->photoTuning.decayDeg, "Photo gain decay beyond the overlap, degrees (tuning)")
        ->default_val(opt->photoTuning.decayDeg);
    sub->add_option("--photo-grid-w", opt->photoTuning.gridW, "Photo field columns (tuning)")
        ->default_val(opt->photoTuning.gridW);
    sub->add_option("--photo-grid-h", opt->photoTuning.gridH, "Photo field rows across the overlap (tuning)")
        ->default_val(opt->photoTuning.gridH);
    sub->add_option("--photo-sigma-lon", opt->photoTuning.sigmaLonDeg, "Photo field longitude sigma, deg (tuning)")
        ->default_val(opt->photoTuning.sigmaLonDeg);
    sub->add_option("--photo-sigma-lat", opt->photoTuning.sigmaLatDeg, "Photo field latitude sigma, deg (tuning)")
        ->default_val(opt->photoTuning.sigmaLatDeg);
    sub->add_option("--photo-margin", opt->photoTuning.trustMarginDeg, "Photo trust margin inside the rims (tuning)")
        ->default_val(opt->photoTuning.trustMarginDeg);
    sub->add_option("--photo-metric-w", opt->photoMetricW, "Polar map width the sky metrics are scored on")
        ->default_val(opt->photoMetricW);
    sub->add_option("--parallax-grid-w", opt->parallaxTuning.gridW, "Parallax grid columns (tuning)")
        ->default_val(opt->parallaxTuning.gridW);
    sub->add_option("--parallax-grid-rows", opt->parallaxTuning.gridRows, "Parallax grid rows across the band (tuning)")
        ->default_val(opt->parallaxTuning.gridRows);
    sub->add_option("--parallax-decay-rows", opt->parallaxTuning.decayRows, "Parallax decay-ring rows (tuning)")
        ->default_val(opt->parallaxTuning.decayRows);
    sub->add_option("--parallax-cross-scale", opt->parallaxTuning.crossMeridianScale,
                    "Trust in the cross-meridian component, 0..1 (tuning)")
        ->default_val(opt->parallaxTuning.crossMeridianScale);
    sub->add_option("--parallax-gate", opt->parallaxTuning.requiredImprovement,
                    "Required residual reduction per cell, 0 = gate off (tuning)")
        ->default_val(opt->parallaxTuning.requiredImprovement);
    sub->add_option("--parallax-band-w", opt->parallaxTuning.band.equirectW,
                    "Width of the analysis map, i.e. band resolution (tuning)")
        ->default_val(opt->parallaxTuning.band.equirectW);
    sub->add_option("--parallax-band-deg", opt->parallaxTuning.band.bandHalfDeg,
                    "Half height of the analysed band in degrees (tuning)")
        ->default_val(opt->parallaxTuning.band.bandHalfDeg);
    sub->callback([opt, &ctx]() { ctx.exitCode = runSeam(*opt); });
}

}  // namespace osvtool
