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
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamAnalysis.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

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
    /// Optional path prefix: write the per-lens bands (uncorrected and
    /// corrected) as raw float32 planes for offline inspection.
    std::string dumpBands;
};

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
Result<geom::LensRig> variantRig(const Pipeline& P, std::optional<double> scaleOverride, geom::RotationSense sense,
                                 geom::QuatOrder order) {
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(P.format.streamW), static_cast<int>(P.format.streamH),
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
    auto pair = P.reader->read(static_cast<std::uint32_t>(o.frame));
    if (!pair.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(pair.error().toString()).c_str());
        return kExitRuntime;
    }

    render::BandParams band;
    band.bandHalfDeg = 4.0;
    nlohmann::json out;
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
    const Variant variants[] = {
        {"scale 0.78125 (no crop)", 3000.0 / 3840.0, geom::RotationSense::BodyToLens, geom::QuatOrder::WXYZ},
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

    if (o.search) {
        render::SeamSearchParams sp;
        auto profile = render::searchSeam(P.rig, pair.value(), P.blendParams, sp, *P.pool);
        if (profile.ok()) {
            out["search"] = {{"meanNcc", profile.value().meanNcc},
                             {"acceptedColumns", profile.value().acceptedColumns},
                             {"columns", profile.value().columns},
                             {"shiftDeg", profile.value().shiftDeg}};
            auto after = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool, &profile.value().shiftDeg);
            if (after.ok()) {
                out["nccAfterSearch"] = after.value();
            }
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
        if (o.flowBackend == "classical") {
            pw.backend = render::FlowBackendKind::Classical;
        } else if (o.flowBackend == "neural") {
            pw.backend = render::FlowBackendKind::Neural;
        } else if (o.flowBackend == "auto") {
            pw.backend = render::FlowBackendKind::Auto;
        } else {
            std::fprintf(stderr, "error: unknown --flow-backend '%s'\n", log::safe(o.flowBackend).c_str());
            return kExitUsage;
        }
        std::vector<float> seamHold;
        const std::vector<float>* seamIn = nullptr;
        if (o.search && out.contains("search")) {
            seamHold = out["search"]["shiftDeg"].get<std::vector<float>>();
            if (!seamHold.empty()) {
                seamIn = &seamHold;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto grid = render::buildParallaxWarp(P.rig, pair.value(), P.blendParams, pw, seamIn, *P.pool);
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        nlohmann::json pj;
        pj["buildMs"] = buildMs;
        // Raw band dump: lens luma and coverage, without and with the warp.
        // The uncorrected pair is written even when the grid was refused, since
        // a refused grid is exactly the case worth looking at.
        const auto dump = [&](const char* tag, const render::WarpGridView* w) {
            // The ANALYSIS band (pw.band), so --parallax-band-deg can widen
            // the dump to show content beyond the scored band.
            auto bands = render::renderLensBands(P.rig, pair.value(), P.blendParams, pw.band, false, seamIn, *P.pool, w);
            if (!bands.ok()) {
                return;
            }
            const render::LensBands& b = bands.value();
            for (int lens = 0; lens < 2; ++lens) {
                writeRawPlane(o.dumpBands + "_" + tag + "_luma" + std::to_string(lens) + ".f32", b.luma[lens]);
                writeRawPlane(o.dumpBands + "_" + tag + "_alpha" + std::to_string(lens) + ".f32", b.alpha[lens]);
            }
            pj["dump"] = {{"w", b.w}, {"h", b.h}, {"rowOffset", b.rowOffset}};
        };
        if (!o.dumpBands.empty()) {
            dump("none", nullptr);
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
                dump("parallax", &view);
                writeRawPlane(o.dumpBands + "_grid.f32", g.uv);
            }
            pj["backend"] = render::flowBackendName(g.usedBackend);
            pj["gridW"] = g.w;
            pj["gridH"] = g.h;
            pj["consistentFraction"] = g.consistentFraction();
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
            // Region score: the same column window uncorrected, with the seam
            // table (when searched), and with the parallax grid on top.
            int rc0 = 0, rc1 = 0;
            if (!o.region.empty() && std::sscanf(o.region.c_str(), "%d-%d", &rc0, &rc1) == 2 && rc0 >= 0 &&
                rc1 >= 0 && rc0 < static_cast<int>(band.equirectW) && rc1 < static_cast<int>(band.equirectW)) {
                nlohmann::json rj;
                const auto score = [&](const char* name, const std::vector<float>* seamT,
                                       const render::WarpGridView* w) {
                    auto bands = render::renderLensBands(P.rig, pair.value(), P.blendParams, band, false, seamT,
                                                         *P.pool, w);
                    if (bands.ok()) {
                        const RegionScore s = scoreRegion(bands.value(), rc0, rc1);
                        rj[name] = {{"ncc", s.ncc}, {"meanAbsDiff", s.meanAbsDiff}, {"samples", s.samples}};
                    }
                };
                score("none", nullptr, nullptr);
                if (seamIn) {
                    score("seam", seamIn, nullptr);
                }
                score("parallax", seamIn, &view);
                pj["region"] = rj;
            }
        } else {
            pj["error"] = grid.error().message;
        }
        out["parallax"] = pj;
    }

    if (o.json) {
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("frame %d overlap NCC: %.4f (verified conventions)\n", o.frame, current.value());
        for (const auto& a : alt) {
            std::printf("  %-28s NCC %.4f\n", a["variant"].get<std::string>().c_str(), a["ncc"].get<double>());
        }
        if (out.contains("search")) {
            std::printf("  seam search: meanNcc %.4f, accepted %u/%u columns, NCC after %.4f\n",
                        out["search"]["meanNcc"].get<double>(), out["search"]["acceptedColumns"].get<unsigned>(),
                        out["search"]["columns"].get<unsigned>(), out.value("nccAfterSearch", -1.0));
        }
        if (out.contains("parallax")) {
            const nlohmann::json& pj = out["parallax"];
            if (pj.contains("error")) {
                std::printf("  parallax: unavailable (%s), %.0f ms\n", pj["error"].get<std::string>().c_str(),
                            pj["buildMs"].get<double>());
            } else {
                std::printf("  parallax (%s, grid %ux%u): consistent %.1f%%, disparity mean %.3f / max %.3f deg, "
                            "%.0f ms, NCC after %.4f\n",
                            pj["backend"].get<std::string>().c_str(), pj["gridW"].get<unsigned>(),
                            pj["gridH"].get<unsigned>(), 100.0 * pj["consistentFraction"].get<double>(),
                            pj["meanDisparityDeg"].get<double>(), pj["maxDisparityDeg"].get<double>(),
                            pj["buildMs"].get<double>(), pj.value("nccAfter", -1.0));
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
    sub->add_option("--dump-bands", opt->dumpBands, "Write raw float32 lens bands to this path prefix (diagnostic)");
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
