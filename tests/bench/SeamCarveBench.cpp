// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SeamCarveBench.cpp - does the carved seam actually remove the doubled
// fin, what does it cost, and does it hold still over time?
//
// A measurement tool, not a test: it asserts nothing and is deliberately NOT
// registered with ctest (numbers depend on the machine and on what else runs
// on it).  Run it by hand:
//
//     build\<dir>\bin\osv_seam_carve_bench.exe [clip.OSV] [--out DIR] [--frames 0,32,64]
//
// The clip defaults to OSV_SAMPLE_FILE (environment first, then the path the
// build was configured with).  Output is plain ASCII.
//
// WHAT IT MEASURES
// ----------------
// For each frame, with the frame's own 2-D parallax grid in force (the
// importer's default correction), three blends of the same frame:
//
//   feather   the FOV feather alone (what shipped before the carved seam);
//   fixed     a narrow feather on the GEOMETRIC seam (latitude 0) - a seam
//             that is narrow but not carved, to separate the two effects;
//   carved    the DP seam with its adaptive feather (SeamCarve.h).
//
// and two single-lens renders (S0, S1) through the same correction, which
// the metrics compare the blends against, over the co-visible overlap band
// (both lenses coverage > 0.5, |latitude| <= 8 degrees):
//
//   ghost     mean of 2 min(a, 1 - a) |S0 - S1| over the band, where a is the
//             per-pixel mix B = a S0 + (1 - a) S1 solved from the renders:
//             how much DISAGREEING content is shown at partial strength -
//             the doubled fin is exactly this.  x1000, display luma codes.
//   seam-edge mean excess gradient max(0, |grad B| - max(|grad S0|, |grad S1|))
//             - edges in the blend that neither lens has: what a badly
//             placed cut draws.  x1000.
//
// both over the whole band and over the wing region (the polar-axis
// longitudes where the wing tip, its fin and the light cover sit on the
// sample clip).  It saves 16-bit TIFF crops of the wing region and of a
// rectilinear view onto it, per blend, for eyeballing.
//
// Then, over every frame of the clip, the importer's own schedule - one carve
// per kParallaxBucketFrames bucket, steered by the previous bucket's seam,
// glided between buckets - against an independent carve per frame: mean and
// largest per-frame seam movement (degrees of latitude, over all columns).
//
// And the cost: the carve itself (median of 9), and the direct-path render
// of a 2560 x 1440 view across the seam from NVDEC frames in VRAM with the
// feather vs the carved seam (median of 30 each).

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/io/ImageWriter.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamCarve.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaAnalysis.h"
#include "osv/render/CudaRenderer.h"
#endif
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace osv;

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
//  Clip
// ---------------------------------------------------------------------------

/// Clip, rig and format - everything but the frames.
struct Clip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
};

Result<Clip> openClip(const std::filesystem::path& path) {
    Clip c;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(path));
    c.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(c.track, meta::MetadataTrack::load(*c.file));
    OSV_TRY_ASSIGN(c.format, meta::FormatDetector::detect(*c.file, &c.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(c.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(c.format.streamW), static_cast<int>(c.format.streamH),
                                               static_cast<int>(c.format.sensorW), static_cast<int>(c.format.sensorH),
                                               c.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(c.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               c.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    return c;
}

// ---------------------------------------------------------------------------
//  Rendering one configuration
// ---------------------------------------------------------------------------

/// What a render is blended with.
struct Blend {
    const render::ParallaxWarpGrid* grid = nullptr;  ///< Parallax grid in force (or none).
    const render::BlendSeam* seam = nullptr;         ///< Carved / fixed seam (or none = FOV feather).
    int onlyLens = -1;                               ///< 0 / 1 = that lens alone.
};

/// Builder for one configuration; the caller adds the output geometry.
render::RenderParamsBuilder makeBuilder(const Clip& clip, const OsvColorParams& cp, const Blend& b) {
    render::RenderParamsBuilder builder;
    builder.rig(clip.rig).color(cp).blend(geom::BlendParams{}, true).alphaCoverage(true);
    if (b.grid && b.grid->valid()) {
        builder.warp(b.grid->uv, b.grid->w, b.grid->h, b.grid->latMinRad, b.grid->latMaxRad);
    }
    if (b.seam) {
        render::applyBlendSeam(builder, *b.seam);
    }
    if (b.onlyLens == 0 || b.onlyLens == 1) {
        builder.lensEnabled(1 - b.onlyLens, false);
    }
    return builder;
}

/// Polar-axis equirect (lens axes at the poles, seam on the equator).
Result<render::ImageRGBAf> renderPolar(render::IRenderer& r, const Clip& clip, const video::FramePair& pair,
                                       const OsvColorParams& cp, const Blend& b, int w, int h) {
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = w;
    map.h = h;
    render::RenderParamsBuilder builder = makeBuilder(clip, cp, b);
    builder.equirect(map);
    OSV_TRY_ASSIGN(render::RenderJob job, builder.build(pair));
    return r.render(job);
}

/// A rectilinear view.
Result<render::ImageRGBAf> renderView(render::IRenderer& r, const Clip& clip, const video::FramePair& pair,
                                      const OsvColorParams& cp, const Blend& b, const geom::VirtualCamera& cam) {
    render::RenderParamsBuilder builder = makeBuilder(clip, cp, b);
    builder.camera(cam);
    OSV_TRY_ASSIGN(render::RenderJob job, builder.build(pair));
    return r.render(job);
}

// ---------------------------------------------------------------------------
//  Metrics
// ---------------------------------------------------------------------------

/// Rec.709 luma of a display-referred pixel.
float luma(const float* px) noexcept {
    return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
}

/// Ghost and seam-edge energy over the co-visible band rows, optionally
/// restricted to a longitude window of the polar map ([x0, x1) wrapping).
struct Metrics {
    double ghost = 0.0;     ///< x1000
    double seamEdge = 0.0;  ///< x1000
    std::size_t pixels = 0;
};

Metrics measure(const render::ImageRGBAf& B, const render::ImageRGBAf& S0, const render::ImageRGBAf& S1, double bandDeg,
                int x0, int x1) {
    Metrics m;
    const int W = static_cast<int>(B.w);
    const int H = static_cast<int>(B.h);
    const int half = static_cast<int>(std::lround(bandDeg / 180.0 * H));
    const int r0 = std::max(1, H / 2 - half);
    const int r1 = std::min(H - 2, H / 2 + half);
    const auto inWindow = [&](int x) {
        if (x0 < 0) {
            return true;
        }
        return x0 <= x1 ? (x >= x0 && x < x1) : (x >= x0 || x < x1);
    };
    const auto lumaAt = [&](const render::ImageRGBAf& im, int x, int y) {
        const int xx = ((x % W) + W) % W;
        return luma(im.pixel(static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(y)));
    };
    const auto gradAt = [&](const render::ImageRGBAf& im, int x, int y) {
        const double gx = 0.5 * (lumaAt(im, x + 1, y) - lumaAt(im, x - 1, y));
        const double gy = 0.5 * (lumaAt(im, x, y + 1) - lumaAt(im, x, y - 1));
        return std::sqrt(gx * gx + gy * gy);
    };
    double ghost = 0.0;
    double edge = 0.0;
    for (int y = r0; y <= r1; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!inWindow(x)) {
                continue;
            }
            const float* p0 = S0.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const float* p1 = S1.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            if (!(p0[3] > 0.5f) || !(p1[3] > 0.5f)) {
                continue;
            }
            const double l0 = luma(p0);
            const double l1 = luma(p1);
            const double lb = lumaAt(B, x, y);
            const double d = l0 - l1;
            if (std::fabs(d) > 0.02) {
                const double a = std::clamp((lb - l1) / d, 0.0, 1.0);
                ghost += 2.0 * std::min(a, 1.0 - a) * std::fabs(d);
            }
            const double gb = gradAt(B, x, y);
            const double gs = std::max(gradAt(S0, x, y), gradAt(S1, x, y));
            edge += std::max(0.0, gb - gs - 0.004);
            ++m.pixels;
        }
    }
    if (m.pixels > 0) {
        m.ghost = 1000.0 * ghost / static_cast<double>(m.pixels);
        m.seamEdge = 1000.0 * edge / static_cast<double>(m.pixels);
    }
    return m;
}

// ---------------------------------------------------------------------------
//  Output crops
// ---------------------------------------------------------------------------

/// The wing region of a polar map: columns [x0, W) + [0, x1), rows centre +/- half.
render::ImageRGBAf cropWrap(const render::ImageRGBAf& im, int x0, int x1, int half) {
    const int W = static_cast<int>(im.w);
    const int H = static_cast<int>(im.h);
    const int cw = (x0 <= x1) ? (x1 - x0) : (W - x0 + x1);
    const int r0 = std::max(0, H / 2 - half);
    const int r1 = std::min(H, H / 2 + half);
    auto out = render::ImageRGBAf::create(static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(r1 - r0));
    if (!out.ok()) {
        return {};
    }
    render::ImageRGBAf c = std::move(out).value();
    for (int y = r0; y < r1; ++y) {
        for (int k = 0; k < cw; ++k) {
            const int x = (x0 + k) % W;
            const float* s = im.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            float* d = c.data.data() + (static_cast<std::size_t>(y - r0) * c.w + static_cast<std::size_t>(k)) * 4u;
            std::memcpy(d, s, 4 * sizeof(float));
            d[3] = 1.0f;
        }
    }
    return c;
}

void save(const std::filesystem::path& dir, const std::string& name, const render::ImageRGBAf& im) {
    if (!im.valid()) {
        return;
    }
    io::ImageTag tag;
    tag.transfer = io::ImageTransfer::Rec709;
    tag.rec2020 = false;
    tag.writeSidecar = false;
    const Status st = io::writeImage(dir / (name + ".tif"), im, io::ImageFormat::Tiff16, tag);
    if (!st.ok()) {
        std::printf("  (could not write %s: %s)\n", name.c_str(), st.error().message.c_str());
    }
}

/// Median of `reps` runs of `body` in ms, after one warm-up.
double medianMs(int reps, const std::function<bool()>& body) {
    if (!body()) {
        return -1.0;
    }
    std::vector<double> ms;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = Clock::now();
        if (!body()) {
            return -1.0;
        }
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

/// Apply one "key=value" override to the carve parameters (tuning runs).
/// Returns false for an unknown key.
bool setParam(render::SeamCarveParams& p, const std::string& kv) {
    const std::size_t eq = kv.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    const std::string key = kv.substr(0, eq);
    const double v = std::atof(kv.substr(eq + 1).c_str());
    struct Field {
        const char* name;
        double* ptr;
    };
    const Field fields[] = {
        {"narrow", &p.narrowHalfWidthDeg}, {"wide", &p.wideHalfWidthDeg},   {"agree", &p.agreeResidual},
        {"disagree", &p.disagreeResidual}, {"edge", &p.edgeRampDeg},        {"diff", &p.diffWeight},
        {"grad", &p.gradWeight},           {"centre", &p.centreWeight},     {"temporal", &p.temporalWeight},
        {"tnorm", &p.temporalNormDeg},     {"tclamp", &p.temporalClampDeg}, {"covmin", &p.coverageMin},
        {"covw", &p.coverageWeight},       {"step", &p.stepPenalty},        {"smooth", &p.smoothSigmaCols},
        {"wsmooth", &p.widthSigmaCols}};
    for (const Field& f : fields) {
        if (key == f.name) {
            *f.ptr = v;
            return true;
        }
    }
    if (key == "maxstep") {
        p.maxStepRows = static_cast<int>(v);
        return true;
    }
    if (key == "columns") {
        p.columns = static_cast<std::uint32_t>(v);
        return true;
    }
    return false;
}

/// A seam on the geometric seam (latitude 0) with the narrow feather: the
/// "narrow but not carved" control.
render::BlendSeam fixedSeam(const render::SeamCarveParams& p) {
    render::BlendSeam s;
    s.columns = p.columns;
    s.table.assign(static_cast<std::size_t>(p.columns) * 2u, 0.0f);
    for (std::uint32_t c = 0; c < p.columns; ++c) {
        s.table[static_cast<std::size_t>(c) * 2u + 1u] = static_cast<float>(deg2rad(p.narrowHalfWidthDeg));
    }
    s.edgeRad = static_cast<float>(deg2rad(p.edgeRampDeg));
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    // ---- arguments -------------------------------------------------------------
    std::filesystem::path clipPath;
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        clipPath = env;
    } else {
        clipPath = OSV_SAMPLE_FILE;
    }
    std::filesystem::path outDir = "research/seam";
    std::vector<std::uint32_t> frames{0, 32, 64};
    double viewYaw = -90.0;  // looks at the wing tip on the sample clip
    double viewPitch = -72.0;
    bool quick = false;             // quality section only, no crops
    std::vector<std::string> sets;  // --set key=value overrides of the carve
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames.clear();
            std::string list = argv[++i];
            std::size_t pos = 0;
            while (pos < list.size()) {
                const std::size_t comma = list.find(',', pos);
                frames.push_back(static_cast<std::uint32_t>(std::atoi(list.substr(pos, comma - pos).c_str())));
                pos = comma == std::string::npos ? list.size() : comma + 1;
            }
        } else if (std::strcmp(argv[i], "--yaw") == 0 && i + 1 < argc) {
            viewYaw = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
            viewPitch = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (std::strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
            sets.push_back(argv[++i]);
        } else {
            clipPath = argv[i];
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    std::printf("osv_seam_carve_bench - carved seam vs feather blend\nclip: %s\nout:  %s\n", clipPath.string().c_str(),
                outDir.string().c_str());

    auto clip = openClip(clipPath);
    if (!clip.ok()) {
        std::printf("cannot open the clip: %s\n", clip.error().message.c_str());
        return 1;
    }
    ThreadPool pool;

    // ---- renderer: CUDA when present (fast), the CPU reference otherwise ----
    std::unique_ptr<render::IRenderer> renderer;
#if defined(OSV_HAVE_CUDA)
    if (render::CudaRenderer::available(nullptr)) {
        auto cuda = render::CudaRenderer::create(0);
        if (cuda.ok()) {
            renderer = std::move(cuda).value();
        }
    }
#endif
    if (!renderer) {
        renderer = std::make_unique<render::CpuRenderer>(pool);
    }

    video::DecoderOptions hostOpt;
    hostOpt.hw = video::HwAccel::Auto;
    auto reader = video::DualStreamReader::open(clipPath, clip.value().format, hostOpt);
    if (!reader.ok()) {
        std::printf("cannot open the decoder: %s\n", reader.error().message.c_str());
        return 1;
    }
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);
    render::SeamCarveParams carveParams;
    for (const std::string& kv : sets) {
        if (!setParam(carveParams, kv)) {
            std::printf("unknown --set %s\n", kv.c_str());
            return 1;
        }
        std::printf("set %s\n", kv.c_str());
    }
    const render::BlendSeam fixed = fixedSeam(carveParams);

    // Polar map for the metrics and the wing crops.
    const int PW = 4096;
    const int PH = 2048;
    // The wing tip on the sample clip: polar-axis longitudes ~136..-159 deg.
    const int wingX0 = PW * 1800 / 2048;
    const int wingX1 = PW * 120 / 2048;

    // ======================================================================
    //  1. Quality per frame
    // ======================================================================
    std::printf("\nquality (x1000 display luma; band = |lat| <= 8 deg co-visible, wing = the wing-tip longitudes)\n");
    std::printf("  frame  blend       ghost(band)  edge(band)   ghost(wing)  edge(wing)\n");
    for (std::uint32_t f : frames) {
        auto pair = reader.value().read(f);
        if (!pair.ok()) {
            std::printf("  frame %u: decode failed: %s\n", f, pair.error().message.c_str());
            continue;
        }
        render::ParallaxWarpParams pw;
        pw.backend = render::FlowBackendKind::Classical;
        auto grid = render::buildParallaxWarp(clip.value().rig, pair.value(), geom::BlendParams{}, pw, nullptr, pool);
        const render::ParallaxWarpGrid* gridPtr = grid.ok() ? &grid.value() : nullptr;
        render::WarpGridView view;
        render::SeamCorrection corr;
        if (gridPtr) {
            view.uv = gridPtr->uv.data();
            view.w = gridPtr->w;
            view.h = gridPtr->h;
            view.latMinRad = gridPtr->latMinRad;
            view.latMaxRad = gridPtr->latMaxRad;
            corr.warp = &view;
        }
        auto carved = render::carveSeam(clip.value().rig, pair.value(), geom::BlendParams{},
                                        render::ParallaxWarpParams{}.band, corr, carveParams, nullptr, pool);
        if (!carved.ok()) {
            std::printf("  frame %u: carve failed\n", f);
            continue;
        }
        std::printf("  frame %2u: grid %s, seam latitude mean %+.2f / max %.2f deg, feather mean %.2f deg, %u narrow, "
                    "%u forced columns\n",
                    f, gridPtr ? "applied" : "refused", carved.value().meanLatDeg, carved.value().maxAbsLatDeg,
                    carved.value().meanHalfWidthDeg, carved.value().narrowColumns, carved.value().forcedColumns);

        auto s0 = renderPolar(*renderer, clip.value(), pair.value(), cp, Blend{gridPtr, nullptr, 0}, PW, PH);
        auto s1 = renderPolar(*renderer, clip.value(), pair.value(), cp, Blend{gridPtr, nullptr, 1}, PW, PH);
        if (!s0.ok() || !s1.ok()) {
            std::printf("  frame %u: single-lens render failed\n", f);
            continue;
        }
        struct Config {
            const char* name;
            const render::BlendSeam* seam;
        };
        const Config configs[] = {{"feather", nullptr}, {"fixed", &fixed}, {"carved", &carved.value()}};
        for (const Config& c : configs) {
            auto b = renderPolar(*renderer, clip.value(), pair.value(), cp, Blend{gridPtr, c.seam, -1}, PW, PH);
            if (!b.ok()) {
                std::printf("  frame %u %s: render failed: %s\n", f, c.name, b.error().message.c_str());
                continue;
            }
            const Metrics band = measure(b.value(), s0.value(), s1.value(), 8.0, -1, -1);
            const Metrics wing = measure(b.value(), s0.value(), s1.value(), 8.0, wingX0, wingX1);
            std::printf("  %5u  %-10s  %11.3f  %10.3f   %11.3f  %10.3f\n", f, c.name, band.ghost, band.seamEdge,
                        wing.ghost, wing.seamEdge);
            if (quick) {
                continue;
            }
            char name[96];
            std::snprintf(name, sizeof(name), "wing_f%02u_%s", f, c.name);
            save(outDir, name, cropWrap(b.value(), wingX0, wingX1, 240));
            geom::VirtualCamera cam;
            cam.w = 1600;
            cam.h = 900;
            cam.hfovDeg = 50;
            cam.yawDeg = viewYaw;
            cam.pitchDeg = viewPitch;
            auto v = renderView(*renderer, clip.value(), pair.value(), cp, Blend{gridPtr, c.seam, -1}, cam);
            if (v.ok()) {
                std::snprintf(name, sizeof(name), "view_f%02u_%s", f, c.name);
                save(outDir, name, v.value());
            }
        }
        if (quick) {
            continue;
        }
        char name[96];
        std::snprintf(name, sizeof(name), "wing_f%02u_lens0", f);
        save(outDir, name, cropWrap(s0.value(), wingX0, wingX1, 240));
        std::snprintf(name, sizeof(name), "wing_f%02u_lens1", f);
        save(outDir, name, cropWrap(s1.value(), wingX0, wingX1, 240));

        // Carve cost on this frame (bands + DP, the importer's call).
        const double carveMs = medianMs(9, [&]() {
            auto s = render::carveSeam(clip.value().rig, pair.value(), geom::BlendParams{},
                                       render::ParallaxWarpParams{}.band, corr, carveParams, nullptr, pool);
            return s.ok();
        });
        std::printf("         carve (bands + seam, host frames): %.2f ms median; seam part %.2f ms (correct %.2f, "
                    "cost %.2f, DP %.2f)\n",
                    carveMs, carved.value().carveMs, carved.value().correctMs, carved.value().costMs,
                    carved.value().dpMs);
    }

    // ======================================================================
    //  2. Temporal: the importer's bucket schedule vs a fresh carve per frame
    // ======================================================================
    if (!quick) {
        std::printf("\ntemporal (every frame of the clip, degrees of latitude per frame, over all columns)\n");
        const std::uint32_t count = reader.value().frameCount();
        std::vector<std::shared_ptr<render::BlendSeam>> buckets;
        render::BlendSeam lastScheduled;
        render::BlendSeam lastIndependent;
        bool haveScheduled = false;
        bool haveIndependent = false;
        double sumSched = 0.0, maxSched = 0.0, sumIndep = 0.0, maxIndep = 0.0;
        std::uint32_t steps = 0;
        std::shared_ptr<render::BlendSeam> prevBucket;
        std::shared_ptr<render::BlendSeam> curBucket;
        for (std::uint32_t f = 0; f < count; ++f) {
            auto pair = reader.value().read(f);
            if (!pair.ok()) {
                std::printf("  frame %u: decode failed\n", f);
                break;
            }
            // The correction per bucket (as the importer measures it), reused
            // by both schedules so only the seam logic differs.
            static render::ParallaxWarpGrid bucketGrid;
            static bool bucketGridOk = false;
            if (f % render::kParallaxBucketFrames == 0) {
                render::ParallaxWarpParams pw;
                pw.backend = render::FlowBackendKind::Classical;
                auto g =
                    render::buildParallaxWarp(clip.value().rig, pair.value(), geom::BlendParams{}, pw, nullptr, pool);
                bucketGridOk = g.ok();
                if (g.ok()) {
                    bucketGrid = std::move(g).value();
                }
            }
            render::WarpGridView view;
            render::SeamCorrection corr;
            if (bucketGridOk) {
                view.uv = bucketGrid.uv.data();
                view.w = bucketGrid.w;
                view.h = bucketGrid.h;
                view.latMinRad = bucketGrid.latMinRad;
                view.latMaxRad = bucketGrid.latMaxRad;
                corr.warp = &view;
            }
            // Scheduled: one carve per bucket, steered by the previous one,
            // glided per frame.
            if (f % render::kParallaxBucketFrames == 0) {
                auto s = render::carveSeam(clip.value().rig, pair.value(), geom::BlendParams{},
                                           render::ParallaxWarpParams{}.band, corr, carveParams, curBucket.get(), pool);
                if (s.ok()) {
                    prevBucket = curBucket;
                    curBucket = std::make_shared<render::BlendSeam>(std::move(s).value());
                }
            }
            render::BlendSeam scheduled;
            if (curBucket) {
                scheduled = *curBucket;
                if (prevBucket) {
                    auto g = render::blendSeams(*prevBucket, *curBucket, render::parallaxCrossfadeWeight(f));
                    if (g.ok()) {
                        scheduled = std::move(g).value();
                    }
                }
            }
            // Independent: a fresh carve every frame, no memory.
            auto ind = render::carveSeam(clip.value().rig, pair.value(), geom::BlendParams{},
                                         render::ParallaxWarpParams{}.band, corr, carveParams, nullptr, pool);
            if (!ind.ok() || !scheduled.valid()) {
                continue;
            }
            const auto moved = [](const render::BlendSeam& a, const render::BlendSeam& b, double& mx) {
                double s = 0.0;
                for (std::uint32_t c = 0; c < a.columns; ++c) {
                    const double d =
                        rad2deg(std::fabs(static_cast<double>(a.table[c * 2u]) - static_cast<double>(b.table[c * 2u])));
                    s += d;
                    mx = std::max(mx, d);
                }
                return s / static_cast<double>(a.columns);
            };
            if (haveScheduled && haveIndependent) {
                sumSched += moved(scheduled, lastScheduled, maxSched);
                sumIndep += moved(ind.value(), lastIndependent, maxIndep);
                ++steps;
            }
            lastScheduled = scheduled;
            lastIndependent = std::move(ind).value();
            haveScheduled = haveIndependent = true;
        }
        if (steps > 0) {
            std::printf("  importer schedule (bucket %u, prior + glide): mean %.4f deg/frame, max %.3f deg\n",
                        render::kParallaxBucketFrames, sumSched / steps, maxSched);
            std::printf("  independent carve per frame:                  mean %.4f deg/frame, max %.3f deg\n",
                        sumIndep / steps, maxIndep);
        }
    }

    // ======================================================================
    //  3. Direct-path cost: a 2560 x 1440 view across the seam, VRAM frames
    // ======================================================================
#if defined(OSV_HAVE_CUDA)
    if (!quick) {
        std::printf(
            "\nrender cost, 2560x1440 rectilinear 90 deg across the seam, NVDEC frames in VRAM (median of 30)\n");
        video::DecoderOptions devOpt;
        devOpt.hw = video::HwAccel::Cuda;
        devOpt.keepOnDevice = true;
        auto devReader = video::DualStreamReader::open(clipPath, clip.value().format, devOpt);
        auto cuda = render::CudaRenderer::create(0);
        if (devReader.ok() && cuda.ok() && render::installCudaAnalyses().ok()) {
            auto devPair = devReader.value().read(frames.empty() ? 0u : frames.front());
            auto hostPair = reader.value().read(frames.empty() ? 0u : frames.front());
            if (devPair.ok() && hostPair.ok()) {
                auto seam = render::carveSeam(clip.value().rig, hostPair.value(), geom::BlendParams{},
                                              render::ParallaxWarpParams{}.band, {}, carveParams, nullptr, pool);
                geom::VirtualCamera cam;
                cam.w = 2560;
                cam.h = 1440;
                cam.hfovDeg = 90;
                cam.yawDeg = 90;  // the seam runs vertically through the middle
                const auto timeWith = [&](const render::BlendSeam* s) {
                    render::RenderParamsBuilder b = makeBuilder(clip.value(), cp, Blend{nullptr, s, -1});
                    b.camera(cam);
                    auto job = b.build(devPair.value());
                    if (!job.ok()) {
                        return -1.0;
                    }
                    return medianMs(30, [&]() {
                        std::size_t pitch = 0;
                        return cuda.value()->renderToDevice(job.value(), &pitch).ok();
                    });
                };
                const double tFeather = timeWith(nullptr);
                const double tCarved = seam.ok() ? timeWith(&seam.value()) : -1.0;
                std::printf("  feather %.3f ms   carved %.3f ms (launch + sync, no plane upload)\n", tFeather, tCarved);
                // The carve from VRAM frames: GPU band shading + the host DP.
                const double tGpuCarve = medianMs(9, [&]() {
                    auto s = render::carveSeam(clip.value().rig, devPair.value(), geom::BlendParams{},
                                               render::ParallaxWarpParams{}.band, {}, carveParams, nullptr, pool);
                    return s.ok();
                });
                std::printf("  carve from VRAM frames (GPU band shading + DP): %.2f ms median\n", tGpuCarve);
            }
        } else {
            std::printf("  (NVDEC / CUDA unavailable)\n");
        }
    }
#endif
    return 0;
}
