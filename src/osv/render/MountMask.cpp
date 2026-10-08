// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MountMask implementation: Hide Mount Auto's per-clip measurement, its
// decision and the rebuild of the occlusion polygons.  The policy and the
// numbers are in the header.

#include "osv/render/MountMask.h"

#include "osv/geom/EquirectMap.h"
#include "osv/render/RenderJob.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

namespace osv::render {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t`.
[[nodiscard]] double msSince(Clock::time_point t) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// Median of the finite entries of `v` (NaN when there are none).
[[nodiscard]] double medianOf(std::vector<double> v) {
    std::erase_if(v, [](double x) { return !std::isfinite(x); });
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double upper = v[mid];
    if (v.size() % 2 == 1) {
        return upper;
    }
    // Even count: the mean of the two middle values.
    const double lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lower + upper);
}

/// `a` wrapped into [-pi, pi].
[[nodiscard]] double wrapPi(double a) noexcept { return std::remainder(a, kTwoPi); }

/// Ring column `c` (any integer) wrapped into [0, columns).
[[nodiscard]] std::uint32_t ringColumn(long long c, std::uint32_t columns) noexcept {
    const long long n = static_cast<long long>(columns);
    return static_cast<std::uint32_t>(((c % n) + n) % n);
}

/// Body direction on the seam (phi = 0) at continuous polar-axis column
/// position `x` of a ring of `columns` - exactly the mapping the band
/// renderer uses (EquirectMap, PolarAxis layout).
[[nodiscard]] bool seamDirection(std::uint32_t columns, double x, Vec3d& dir) noexcept {
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = static_cast<int>(columns);
    map.h = static_cast<int>(columns / 2);
    return map.pixelToDir(x, 0.5 * static_cast<double>(map.h), dir);
}

/// Polar angle (fisheye pixels, about the principal point) at which lens
/// `lens` images `dir`, and the pixel itself.  False when it does not project.
[[nodiscard]] bool lensPolarAngle(const geom::LensRig& rig, int lens, const Vec3d& dir, double& angle,
                                  Vec2d& px) noexcept {
    double theta = 0.0;
    if (!rig.projectBody(lens, dir, px, theta)) {
        return false;
    }
    const geom::KannalaBrandt5& L = rig.lens[static_cast<std::size_t>(lens)];
    angle = std::atan2(px.y - L.cy, px.x - L.cx);
    return std::isfinite(angle);
}

/// Pixel radius of lens `lens` at `thetaRad` from its axis, on its longer
/// focal axis (the conservative one for "beyond this radius").
[[nodiscard]] double radiusAt(const geom::LensRig& rig, int lens, double thetaRad) noexcept {
    const geom::KannalaBrandt5& L = rig.lens[static_cast<std::size_t>(lens)];
    return std::max(L.fx, L.fy) * L.thetaD(thetaRad);
}

/// The occlusion feather of a blend in pixels (0 for a nonsensical value).
[[nodiscard]] double featherPx(const geom::BlendParams& blend) noexcept {
    return std::isfinite(blend.occlusionFeatherPx) && blend.occlusionFeatherPx > 0.0 ? blend.occlusionFeatherPx
                                                                                      : 0.0;
}

/// One decoded sample: the frame actually measured and its pair.
struct MountSample {
    std::uint32_t frame = 0;
    video::FramePair pair;
};

/// Decode planned sample `frame`, or the first of `alternates(frame)` that
/// decodes - the same substitution rule as the clip correction's samples
/// (ClipSteady.h, "A sample frame the decoder refuses").  Every refusal and
/// the substitution or skip is appended to `notes`; `decodeMs` accumulates
/// the time in `source`.  nullopt when nothing decoded; a cancellation sets
/// `stop`.
[[nodiscard]] std::optional<MountSample> decodeMountSample(std::uint32_t frame, const ClipFrameSource& source,
                                                           const ClipFrameAlternates& alternates,
                                                           std::vector<std::string>& notes, double& decodeMs,
                                                           const ClipCancel& cancelled, bool& stop) {
    stop = false;
    // ---- the planned frame first ------------------------------------------------
    const auto t0 = Clock::now();
    auto pair = source(frame);
    decodeMs += msSince(t0);
    if (pair.ok()) {
        return MountSample{frame, std::move(pair).value()};
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
    for (const std::uint32_t alt : candidates) {
        if (alt == frame) {
            continue;
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
            return MountSample{alt, std::move(altPair).value()};
        }
    }
    notes.push_back(std::format("frame {} could not be decoded ({}); skipped", frame, why));
    return std::nullopt;
}

/// Running sums of one NCC.
struct NccSums {
    double n = 0.0;
    double sa = 0.0;
    double sb = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    double sab = 0.0;

    void add(const NccSums& o) noexcept {
        n += o.n;
        sa += o.sa;
        sb += o.sb;
        saa += o.saa;
        sbb += o.sbb;
        sab += o.sab;
    }

    /// The correlation, or NaN with fewer than `minN` pixels or no variance
    /// in either image (a flat patch correlates with nothing).
    [[nodiscard]] double ncc(double minN) const noexcept {
        if (n < minN || !(n > 0.0)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double va = saa - sa * sa / n;
        const double vb = sbb - sb * sb / n;
        const double cov = sab - sa * sb / n;
        if (!(va > 1e-12 * n) || !(vb > 1e-12 * n)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double r = cov / std::sqrt(va * vb);
        return std::isfinite(r) ? std::clamp(r, -1.0, 1.0) : std::numeric_limits<double>::quiet_NaN();
    }
};

/// The best correlation found so far and where.
struct BestShift {
    double value = -std::numeric_limits<double>::infinity();
    int dy = 0;
    int dx = 0;
    bool found = false;

    void offer(double v, int y, int x) noexcept {
        if (std::isfinite(v) && v > value) {
            value = v;
            dy = y;
            dx = x;
            found = true;
        }
    }
};

/// The coarse search positions of [-limit, limit]: every second one, and the
/// two ends always.
[[nodiscard]] std::vector<int> coarseShifts(int limit) {
    std::vector<int> s;
    for (int v = -limit; v <= limit; v += 2) {
        s.push_back(v);
    }
    if (s.empty() || s.back() != limit) {
        s.push_back(limit);
    }
    return s;
}

/// Population sigma of the covered pixels, NaN when fewer than a quarter of
/// them are covered.
[[nodiscard]] double coveredSigma(const std::vector<float>& v, const std::vector<std::uint8_t>& m) {
    double n = 0.0;
    double s = 0.0;
    double ss = 0.0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (m[i]) {
            n += 1.0;
            s += v[i];
            ss += static_cast<double>(v[i]) * v[i];
        }
    }
    if (n < 0.25 * static_cast<double>(v.size()) || !(n > 1.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double var = std::max(0.0, ss / n - (s / n) * (s / n));
    return std::sqrt(var);
}

}  // namespace

// =============================================================================
//  Parameters
// =============================================================================
Status validateMountMaskParams(const MountMaskParams& p) {
    if (p.equirectW < 64 || p.equirectW > 16384 || p.equirectW % 2 != 0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: equirectW must be an even 64..16384"};
    }
    if (p.windowCols < 2 || p.windowCols > p.equirectW / 4 || p.equirectW % p.windowCols != 0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: windowCols must divide the ring into at least 4"};
    }
    if (!std::isfinite(p.rowsHalfDeg) || p.rowsHalfDeg <= 0.0 || !std::isfinite(p.searchAlongDeg) ||
        p.searchAlongDeg < 0.0 || p.rowsHalfDeg + p.searchAlongDeg > 45.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: rowsHalfDeg + searchAlongDeg must be in (0, 45]"};
    }
    if (!std::isfinite(p.searchAcrossDeg) || p.searchAcrossDeg < 0.0 || p.searchAcrossDeg > 45.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: searchAcrossDeg must be in [0, 45]"};
    }
    if (!std::isfinite(p.releaseNcc) || p.releaseNcc <= 0.0 || p.releaseNcc > 1.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: releaseNcc must be in (0, 1]"};
    }
    if (!std::isfinite(p.flatSigma) || p.flatSigma < 0.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: flatSigma must be finite and >= 0"};
    }
    if (p.dilateCols > p.equirectW / 2) {
        return Error{ErrorCode::InvalidArgument, "mount mask: dilateCols is wider than half the ring"};
    }
    if (!std::isfinite(p.minCovalidFraction) || p.minCovalidFraction <= 0.0 || p.minCovalidFraction > 1.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: minCovalidFraction must be in (0, 1]"};
    }
    if (p.minFrames < 1) {
        return Error{ErrorCode::InvalidArgument, "mount mask: minFrames must be at least 1"};
    }
    if (!std::isfinite(p.clampThetaDeg) || p.clampThetaDeg < 45.0 || p.clampThetaDeg > 135.0) {
        return Error{ErrorCode::InvalidArgument, "mount mask: clampThetaDeg must be in [45, 135]"};
    }
    return okStatus();
}

// =============================================================================
//  The arc
// =============================================================================
Result<MountArc> mountArcColumns(const geom::LensRig& rig, std::uint32_t equirectW) {
    if (equirectW < 64 || equirectW > 16384) {
        return Error{ErrorCode::InvalidArgument, "mountArcColumns: bad ring width"};
    }
    for (int i = 0; i < geom::kLensCount; ++i) {
        if (!rig.lens[static_cast<std::size_t>(i)].isValid()) {
            return Error{ErrorCode::InvalidArgument, "mountArcColumns: the rig has an invalid lens"};
        }
    }
    MountArc arc;
    arc.columns = equirectW;
    arc.inArc.assign(equirectW, 0);
    arc.geometricClean.assign(equirectW, 0);
    for (std::uint32_t c = 0; c < equirectW; ++c) {
        Vec3d dir;
        if (!seamDirection(equirectW, static_cast<double>(c) + 0.5, dir)) {
            return Error{ErrorCode::Internal, "mountArcColumns: the ring direction is not finite"};
        }
        // ---- where each lens's polygon starts at this column, if it reaches the image ----
        bool reaches[2] = {false, false};
        double thetaStart[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        for (int i = 0; i < geom::kLensCount; ++i) {
            const std::vector<Vec2d>& poly = rig.occlusionPolyStream[static_cast<std::size_t>(i)];
            if (poly.size() < 3) {
                continue;  // no polygon: nothing hidden in this lens
            }
            double angle = 0.0;
            Vec2d px;
            if (!lensPolarAngle(rig, i, dir, angle, px)) {
                continue;
            }
            const geom::KannalaBrandt5& L = rig.lens[static_cast<std::size_t>(i)];
            const auto radii = geom::occlusionRayRadii(poly, Vec2d{L.cx, L.cy}, angle);
            if (!radii || !(L.rMaxPx > 0.0) || radii->first >= L.rMaxPx) {
                continue;  // the polygon misses this azimuth or starts beyond the usable circle
            }
            reaches[i] = true;
            // The angle from the axis the polygon starts at (mean focal, as
            // rMaxPx is measured), for the geometric fallback below.
            const double fMean = 0.5 * (L.fx + L.fy);
            const auto theta = L.thetaFromThetaD(radii->first / fMean);
            thetaStart[i] = theta.ok() ? theta.value() : radii->first / fMean;
        }
        if (!reaches[0] && !reaches[1]) {
            continue;
        }
        arc.inArc[c] = 1;
        ++arc.arcColumns;
        // ---- the lens the calibration says images the mount less here ------------------
        // The only lens without a polygon here; else the one whose polygon
        // starts further from its axis.
        if (reaches[0] && !reaches[1]) {
            arc.geometricClean[c] = 1;
        } else if (reaches[1] && !reaches[0]) {
            arc.geometricClean[c] = 0;
        } else {
            arc.geometricClean[c] = thetaStart[0] >= thetaStart[1] ? 0 : 1;
        }
    }
    return arc;
}

// =============================================================================
//  One frame's scores
// =============================================================================
Result<std::vector<MountWindowScore>> scoreMountBands(const LensBands& bands,
                                                      const std::vector<std::uint32_t>& windowStarts,
                                                      const MountMaskParams& params, ThreadPool* pool) {
    OSV_TRY(validateMountMaskParams(params));
    // ---- the bands must be what measureMountMask renders ----------------------------------
    const std::size_t plane = static_cast<std::size_t>(bands.w) * bands.h;
    if (bands.w != params.equirectW || bands.h < 3 || bands.mapH < 2 || plane == 0) {
        return Error{ErrorCode::InvalidArgument, "scoreMountBands: the bands are not the mount mask's ring"};
    }
    for (int i = 0; i < 2; ++i) {
        if (bands.luma[i].size() != plane || bands.alpha[i].size() != plane) {
            return Error{ErrorCode::InvalidArgument, "scoreMountBands: a band plane has the wrong size"};
        }
    }
    const std::uint32_t W = bands.w;
    const double rowsPerDeg = static_cast<double>(bands.mapH) / 180.0;
    const long long centreLL = static_cast<long long>(bands.mapH / 2) - static_cast<long long>(bands.rowOffset);
    if (centreLL <= 0 || centreLL >= static_cast<long long>(bands.h)) {
        return Error{ErrorCode::InvalidArgument, "scoreMountBands: the seam row is outside the band"};
    }
    const int centre = static_cast<int>(centreLL);
    const int halfRows = std::max(1, static_cast<int>(std::lround(params.rowsHalfDeg * rowsPerDeg)));
    // The search can only reach rows the band holds.
    int dyMax = static_cast<int>(std::lround(params.searchAlongDeg * rowsPerDeg));
    dyMax = std::min({dyMax, centre - halfRows, static_cast<int>(bands.h) - centre - halfRows});
    if (dyMax < 0) {
        return Error{ErrorCode::InvalidArgument, "scoreMountBands: the band is too short for the scored rows"};
    }
    const int dxMax = std::min(static_cast<int>(std::lround(params.searchAcrossDeg * static_cast<double>(W) / 360.0)),
                               static_cast<int>(W / 4));
    const int wc = static_cast<int>(params.windowCols);
    for (const std::uint32_t s : windowStarts) {
        if (s >= W) {
            return Error{ErrorCode::InvalidArgument, "scoreMountBands: a window starts beyond the ring"};
        }
    }

    const int rows = 2 * halfRows;
    const double minFull = params.minCovalidFraction * static_cast<double>(rows * wc);
    const double minHalf = params.minCovalidFraction * static_cast<double>(halfRows * wc);
    const std::vector<int> coarseDy = coarseShifts(dyMax);
    const std::vector<int> coarseDx = coarseShifts(dxMax);
    std::vector<MountWindowScore> out(windowStarts.size());

    // ---- one window: the reference, the search region, the search --------------------------
    const auto scoreWindow = [&](std::size_t wi) {
        const long long x0 = static_cast<long long>(windowStarts[wi]);
        const int r0 = centre - halfRows;
        // Lens 0's rows |phi| <= rowsHalfDeg, the reference.
        std::vector<float> a(static_cast<std::size_t>(rows * wc));
        std::vector<std::uint8_t> ma(a.size(), 0);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < wc; ++c) {
                const std::size_t idx = static_cast<std::size_t>(r0 + r) * W + ringColumn(x0 + c, W);
                const float v = bands.luma[0][idx];
                const std::size_t k = static_cast<std::size_t>(r * wc + c);
                a[k] = v;
                ma[k] = (std::isfinite(v) && bands.alpha[0][idx] > 0.5f) ? 1 : 0;
            }
        }
        // Lens 1 over every row and column the search can reach.
        const int regRows = rows + 2 * dyMax;
        const int regCols = wc + 2 * dxMax;
        std::vector<float> b(static_cast<std::size_t>(regRows * regCols));
        std::vector<std::uint8_t> mb(b.size(), 0);
        for (int r = 0; r < regRows; ++r) {
            for (int c = 0; c < regCols; ++c) {
                const std::size_t idx =
                    static_cast<std::size_t>(r0 - dyMax + r) * W + ringColumn(x0 - dxMax + c, W);
                const float v = bands.luma[1][idx];
                const std::size_t k = static_cast<std::size_t>(r * regCols + c);
                b[k] = v;
                mb[k] = (std::isfinite(v) && bands.alpha[1][idx] > 0.5f) ? 1 : 0;
            }
        }
        // ---- sigma of each lens over the unshifted window ------------------------------
        MountWindowScore& score = out[wi];
        score.sigma0 = static_cast<float>(coveredSigma(a, ma));
        {
            std::vector<float> b0(a.size());
            std::vector<std::uint8_t> mb0(a.size(), 0);
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < wc; ++c) {
                    const std::size_t k = static_cast<std::size_t>((r + dyMax) * regCols + c + dxMax);
                    b0[static_cast<std::size_t>(r * wc + c)] = b[k];
                    mb0[static_cast<std::size_t>(r * wc + c)] = mb[k];
                }
            }
            score.sigma1 = static_cast<float>(coveredSigma(b0, mb0));
        }
        // ---- the correlation of one shift: the two halves' sums ------------------------
        BestShift bestFull;
        BestShift bestUpper;
        BestShift bestLower;
        const auto evaluate = [&](int dy, int dx) {
            NccSums half[2];
            for (int r = 0; r < rows; ++r) {
                // Rows above the seam row are phi > 0: lens 0's far side.
                NccSums& s = half[r < halfRows ? 0 : 1];
                const float* av = a.data() + static_cast<std::size_t>(r * wc);
                const std::uint8_t* am = ma.data() + static_cast<std::size_t>(r * wc);
                const std::size_t brow = static_cast<std::size_t>((r + dyMax + dy) * regCols + dxMax + dx);
                const float* bv = b.data() + brow;
                const std::uint8_t* bm = mb.data() + brow;
                for (int c = 0; c < wc; ++c) {
                    if (am[c] && bm[c]) {
                        const double x = av[c];
                        const double y = bv[c];
                        s.n += 1.0;
                        s.sa += x;
                        s.sb += y;
                        s.saa += x * x;
                        s.sbb += y * y;
                        s.sab += x * y;
                    }
                }
            }
            NccSums full = half[0];
            full.add(half[1]);
            bestFull.offer(full.ncc(minFull), dy, dx);
            bestUpper.offer(half[0].ncc(minHalf), dy, dx);
            bestLower.offer(half[1].ncc(minHalf), dy, dx);
        };
        // ---- coarse: every second shift --------------------------------------------------
        for (const int dy : coarseDy) {
            for (const int dx : coarseDx) {
                evaluate(dy, dx);
            }
        }
        // ---- fine: the eight neighbours of each best coarse shift ------------------------
        const BestShift seeds[3] = {bestFull, bestUpper, bestLower};
        for (const BestShift& seed : seeds) {
            if (!seed.found) {
                continue;
            }
            for (int ddy = -1; ddy <= 1; ++ddy) {
                for (int ddx = -1; ddx <= 1; ++ddx) {
                    const int dy = seed.dy + ddy;
                    const int dx = seed.dx + ddx;
                    if ((ddy == 0 && ddx == 0) || dy < -dyMax || dy > dyMax || dx < -dxMax || dx > dxMax) {
                        continue;
                    }
                    evaluate(dy, dx);
                }
            }
        }
        if (bestFull.found) {
            score.ncc = static_cast<float>(bestFull.value);
        }
        if (bestUpper.found) {
            score.upper = static_cast<float>(bestUpper.value);
        }
        if (bestLower.found) {
            score.lower = static_cast<float>(bestLower.value);
        }
    };

    // ---- every window, in parallel when a pool is given --------------------------------
    if (pool != nullptr && windowStarts.size() > 1) {
        OSV_TRY(pool->parallelFor(0, windowStarts.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t wi = begin; wi < end; ++wi) {
                scoreWindow(wi);
            }
        }));
    } else {
        for (std::size_t wi = 0; wi < windowStarts.size(); ++wi) {
            scoreWindow(wi);
        }
    }
    return out;
}

// =============================================================================
//  The decision
// =============================================================================
Result<MountMask> decideMountMask(const MountArc& arc, const std::vector<std::uint32_t>& windowStarts,
                                  const std::vector<std::vector<MountWindowScore>>& perFrame,
                                  const MountMaskParams& params) {
    OSV_TRY(validateMountMaskParams(params));
    const std::uint32_t W = params.equirectW;
    if (arc.columns != W || arc.inArc.size() != W || arc.geometricClean.size() != W) {
        return Error{ErrorCode::InvalidArgument, "decideMountMask: the arc is not the mount mask's ring"};
    }
    for (const std::uint32_t s : windowStarts) {
        if (s >= W || s % params.windowCols != 0) {
            return Error{ErrorCode::InvalidArgument, "decideMountMask: a window does not start on the window grid"};
        }
    }
    for (const auto& frame : perFrame) {
        if (frame.size() != windowStarts.size()) {
            return Error{ErrorCode::InvalidArgument, "decideMountMask: a frame scored another set of windows"};
        }
    }
    const std::size_t needed =
        std::max<std::size_t>(1, std::min<std::size_t>(params.minFrames, std::max<std::size_t>(perFrame.size(), 1)));

    // ---- per window: the medians over the frames -------------------------------------------
    MountMask mask;
    mask.columns = W;
    mask.arcColumns = arc.arcColumns;
    mask.windows.resize(windowStarts.size());
    for (std::size_t w = 0; w < windowStarts.size(); ++w) {
        MountWindow& win = mask.windows[w];
        win.col0 = windowStarts[w];
        std::vector<double> ncc;
        std::vector<double> upper;
        std::vector<double> lower;
        std::vector<double> s0;
        std::vector<double> s1;
        for (const auto& frame : perFrame) {
            const MountWindowScore& s = frame[w];
            ncc.push_back(s.ncc);
            upper.push_back(s.upper);
            lower.push_back(s.lower);
            s0.push_back(s.sigma0);
            s1.push_back(s.sigma1);
        }
        win.frames = static_cast<std::uint32_t>(std::count_if(ncc.begin(), ncc.end(), [](double v) {
            return std::isfinite(v);
        }));
        // An agreement needs as many scored frames as the clip needs samples:
        // one lucky frame (a moment the background happened to be plain)
        // must not release a window.
        if (win.frames >= needed) {
            win.agreement = medianOf(ncc);
        }
        win.upperNcc = medianOf(upper);
        win.lowerNcc = medianOf(lower);
        win.sigma[0] = medianOf(s0);
        win.sigma[1] = medianOf(s1);
        win.flat = std::isfinite(win.sigma[0]) && std::isfinite(win.sigma[1]) && win.sigma[0] < params.flatSigma &&
                   win.sigma[1] < params.flatSigma;
    }

    // ---- the rule: textured windows on their agreement --------------------------------------
    std::vector<std::uint8_t> texturedRelease(windowStarts.size(), 0);
    for (std::size_t w = 0; w < windowStarts.size(); ++w) {
        const MountWindow& win = mask.windows[w];
        texturedRelease[w] = (!win.flat && std::isfinite(win.agreement) && win.agreement >= params.releaseNcc) ? 1 : 0;
        mask.windows[w].released = texturedRelease[w] != 0;
    }
    // ---- flat windows only between two released textured windows ---------------------------
    // A flat window agrees with anything, the open sky beside a selfie stick
    // included, so its own correlation says nothing.  Its neighbours do.
    const auto windowAt = [&](long long col0) -> std::optional<std::size_t> {
        const std::uint32_t c = ringColumn(col0, W);
        const auto it = std::find(windowStarts.begin(), windowStarts.end(), c);
        if (it == windowStarts.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(it - windowStarts.begin());
    };
    for (std::size_t w = 0; w < windowStarts.size(); ++w) {
        if (!mask.windows[w].flat) {
            continue;
        }
        const long long c0 = static_cast<long long>(windowStarts[w]);
        const auto left = windowAt(c0 - static_cast<long long>(params.windowCols));
        const auto right = windowAt(c0 + static_cast<long long>(params.windowCols));
        mask.windows[w].released = left && right && texturedRelease[*left] && texturedRelease[*right];
    }

    // ---- columns: released windows, then every kept window dilated over them -------------
    std::vector<std::uint8_t> released(W, 0);
    for (std::size_t w = 0; w < windowStarts.size(); ++w) {
        if (!mask.windows[w].released) {
            continue;
        }
        for (std::uint32_t c = 0; c < params.windowCols; ++c) {
            const std::uint32_t col = ringColumn(static_cast<long long>(windowStarts[w]) + c, W);
            if (arc.inArc[col]) {
                released[col] = 1;
            }
        }
    }
    for (std::size_t w = 0; w < windowStarts.size(); ++w) {
        if (mask.windows[w].released) {
            continue;
        }
        const long long from = static_cast<long long>(windowStarts[w]) - static_cast<long long>(params.dilateCols);
        const long long to = static_cast<long long>(windowStarts[w]) + params.windowCols + params.dilateCols;
        for (long long c = from; c < to; ++c) {
            released[ringColumn(c, W)] = 0;
        }
    }

    // ---- kept runs of the arc: which lens does not image the mount ---------------------------
    // Per run of kept arc columns: the lens whose FAR side (inside its own
    // polygon) agrees better with the other lens's near side sees scene
    // there, not the mount.  Without a scored half, the calibration's
    // geometry decides (MountArc::geometricClean).
    mask.state.assign(W, kMountKeepClean0);
    const auto keptArc = [&](std::uint32_t c) { return arc.inArc[c] && !released[c]; };
    std::uint32_t start = W;  // a column that is NOT a kept arc column, to start the ring walk at
    for (std::uint32_t c = 0; c < W; ++c) {
        if (!keptArc(c)) {
            start = c;
            break;
        }
    }
    const auto settleRun = [&](const std::vector<std::uint32_t>& run) {
        if (run.empty()) {
            return;
        }
        std::vector<double> diffs;
        for (std::size_t w = 0; w < windowStarts.size(); ++w) {
            const MountWindow& win = mask.windows[w];
            const bool overlaps = std::any_of(run.begin(), run.end(), [&](std::uint32_t c) {
                return ringColumn(static_cast<long long>(c) - win.col0, W) < params.windowCols;
            });
            if (overlaps && std::isfinite(win.upperNcc) && std::isfinite(win.lowerNcc)) {
                diffs.push_back(win.upperNcc - win.lowerNcc);
            }
        }
        std::uint8_t clean = kMountKeepClean0;
        const double diff = medianOf(diffs);
        if (std::isfinite(diff)) {
            clean = diff >= 0.0 ? kMountKeepClean0 : kMountKeepClean1;
        } else {
            const auto ones = std::count_if(run.begin(), run.end(), [&](std::uint32_t c) {
                return arc.geometricClean[c] != 0;
            });
            clean = 2 * static_cast<std::size_t>(ones) > run.size() ? kMountKeepClean1 : kMountKeepClean0;
        }
        for (const std::uint32_t c : run) {
            mask.state[c] = clean;
        }
    };
    if (start == W) {
        // The whole ring is one kept arc run.
        std::vector<std::uint32_t> run(W);
        for (std::uint32_t c = 0; c < W; ++c) {
            run[c] = c;
        }
        settleRun(run);
    } else {
        std::vector<std::uint32_t> run;
        for (std::uint32_t k = 1; k <= W; ++k) {
            const std::uint32_t c = ringColumn(static_cast<long long>(start) + k, W);
            if (keptArc(c)) {
                run.push_back(c);
            } else {
                settleRun(run);
                run.clear();
            }
        }
        settleRun(run);
    }
    for (std::uint32_t c = 0; c < W; ++c) {
        if (released[c]) {
            mask.state[c] = kMountRelease;
            ++mask.releasedColumns;
        }
    }
    return mask;
}

// =============================================================================
//  The measurement
// =============================================================================
Result<MountMask> measureMountMask(const geom::LensRig& rig, const geom::BlendParams& blend,
                                   const std::vector<std::uint32_t>& frames, const ClipFrameSource& source,
                                   const MountMaskParams& params, ThreadPool& pool, const ClipCancel& cancelled,
                                   const ClipFrameAlternates& alternates) {
    OSV_TRY(validateMountMaskParams(params));
    if (!source) {
        return Error{ErrorCode::InvalidArgument, "measureMountMask: no frame source"};
    }
    const auto tAll = Clock::now();
    // ---- the arc and its windows (geometry only) ----------------------------------------
    OSV_TRY_ASSIGN(MountArc arc, mountArcColumns(rig, params.equirectW));
    std::vector<std::uint32_t> windowStarts;
    for (std::uint32_t c0 = 0; c0 < params.equirectW; c0 += params.windowCols) {
        bool any = false;
        for (std::uint32_t c = 0; c < params.windowCols && !any; ++c) {
            any = arc.inArc[c0 + c] != 0;
        }
        if (any) {
            windowStarts.push_back(c0);
        }
    }
    if (windowStarts.empty()) {
        // No polygon reaches the image: nothing to keep or release.
        MountMask none;
        none.columns = params.equirectW;
        none.state.assign(params.equirectW, kMountKeepClean0);
        none.measureMs = msSince(tAll);
        return none;
    }
    if (frames.empty()) {
        return Error{ErrorCode::InvalidArgument, "measureMountMask: no sample frames"};
    }

    // ---- both lenses alone, without the mask, around the seam ---------------------------
    geom::BlendParams bandBlend = blend;
    bandBlend.useOcclusionMask = false;
    BandParams band;
    band.equirectW = params.equirectW;
    band.bandHalfDeg = params.rowsHalfDeg + params.searchAlongDeg;

    std::vector<std::vector<MountWindowScore>> perFrame;
    std::vector<std::uint32_t> measured;
    std::vector<std::string> notes;
    double decodeMs = 0.0;
    for (const std::uint32_t planned : frames) {
        if (cancelled && cancelled()) {
            return Error{ErrorCode::Unsupported, "mount mask measurement cancelled"};
        }
        bool stop = false;
        std::optional<MountSample> sample =
            decodeMountSample(planned, source, alternates, notes, decodeMs, cancelled, stop);
        if (stop) {
            return Error{ErrorCode::Unsupported, "mount mask measurement cancelled"};
        }
        if (!sample) {
            continue;  // skipped (noted); the minimum is checked after the pass
        }
        OSV_TRY_ASSIGN(LensBands bands, renderLensBands(rig, sample->pair, bandBlend, band, false, nullptr, pool));
        OSV_TRY_ASSIGN(std::vector<MountWindowScore> scores, scoreMountBands(bands, windowStarts, params, &pool));
        perFrame.push_back(std::move(scores));
        measured.push_back(sample->frame);
    }

    // ---- enough frames scored? -----------------------------------------------------------------
    const std::size_t needed = std::max<std::size_t>(1, std::min<std::size_t>(params.minFrames, frames.size()));
    if (perFrame.size() < needed) {
        std::string why;
        for (const std::string& n : notes) {
            why += (why.empty() ? "" : "; ") + n;
        }
        return Error{ErrorCode::Decoder, std::format("measureMountMask: only {} of {} sample frames could be "
                                                     "decoded ({} needed): {}",
                                                     perFrame.size(), frames.size(), needed, why)};
    }
    OSV_TRY_ASSIGN(MountMask mask, decideMountMask(arc, windowStarts, perFrame, params));
    mask.frames = std::move(measured);
    mask.sampleNotes = std::move(notes);
    mask.decodeMs = decodeMs;
    mask.measureMs = std::max(0.0, msSince(tAll) - decodeMs);
    return mask;
}

// =============================================================================
//  Applying the verdict to a rig
// =============================================================================
Result<MountMaskApplied> applyMountMask(geom::LensRig& rig, const MountMask& mask, const geom::BlendParams& blend,
                                        const MountMaskParams& params) {
    OSV_TRY(validateMountMaskParams(params));
    if (!mask.valid() || mask.columns != params.equirectW) {
        return Error{ErrorCode::InvalidArgument, "applyMountMask: the verdict is not the mount mask's ring"};
    }
    for (const std::uint8_t s : mask.state) {
        if (s > kMountRelease) {
            return Error{ErrorCode::InvalidArgument, "applyMountMask: a column has an unknown state"};
        }
    }
    const std::uint32_t W = mask.columns;
    OSV_TRY_ASSIGN(MountArc arc, mountArcColumns(rig, W));
    const double feather = featherPx(blend);

    // ---- per column: does EITHER lens have full weight on the seam plane? ------------------
    // Where neither does, the coverage dips below 1 on the seam (both
    // polygons start before it and their feathers overlap there).
    std::vector<std::uint8_t> strip(W, 0);
    for (std::uint32_t c = 0; c < W; ++c) {
        if (!arc.inArc[c]) {
            continue;
        }
        Vec3d dir;
        if (!seamDirection(W, static_cast<double>(c) + 0.5, dir)) {
            return Error{ErrorCode::Internal, "applyMountMask: the ring direction is not finite"};
        }
        double best = 0.0;
        for (int i = 0; i < geom::kLensCount; ++i) {
            double angle = 0.0;
            Vec2d px;
            if (!lensPolarAngle(rig, i, dir, angle, px)) {
                continue;  // not imaged on the seam at all: no weight
            }
            const double sd =
                geom::signedDistanceToPolygon(rig.occlusionPolyStream[static_cast<std::size_t>(i)], px);
            const double factor = feather > 0.0 ? std::clamp(sd / feather, 0.0, 1.0) : (sd > 0.0 ? 1.0 : 0.0);
            best = std::max(best, factor);
        }
        strip[c] = best < 1.0 - 1e-9 ? 1 : 0;
    }

    MountMaskApplied applied;
    for (std::uint32_t c = 0; c < W; ++c) {
        if (arc.inArc[c] && mask.state[c] == kMountRelease) {
            ++applied.releasedColumns;
        }
    }

    // ---- each lens: per-column treatment, stretches, the rebuilt polygon -------------------
    std::vector<Vec2d> rebuilt[2];
    for (int i = 0; i < geom::kLensCount; ++i) {
        const std::vector<Vec2d>& poly = rig.occlusionPolyStream[static_cast<std::size_t>(i)];
        rebuilt[i] = poly;
        if (poly.size() < 3) {
            continue;  // nothing to rebuild
        }
        const geom::KannalaBrandt5& L = rig.lens[static_cast<std::size_t>(i)];
        // ---- per column ------------------------------------------------------------------
        std::vector<geom::OcclusionSpanState> st(W, geom::OcclusionSpanState::Keep);
        for (std::uint32_t c = 0; c < W; ++c) {
            if (!arc.inArc[c]) {
                continue;
            }
            if (mask.state[c] == kMountRelease) {
                st[c] = geom::OcclusionSpanState::Release;
            } else if (mask.state[c] == static_cast<std::uint8_t>(i) && strip[c]) {
                st[c] = geom::OcclusionSpanState::Clamp;
            }
        }
        // A clamp only for whole stretches: close Keep gaps narrower than a
        // window between clamped columns, then drop clamped runs narrower
        // than a window (each run costs vertices and buys nothing visible).
        const auto runsOf = [&](geom::OcclusionSpanState want) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // first column, length
            std::uint32_t begin = W;
            for (std::uint32_t c = 0; c < W; ++c) {
                if (st[c] != want) {
                    begin = c;
                    break;
                }
            }
            if (begin == W) {
                runs.emplace_back(0u, W);  // the whole ring
                return runs;
            }
            std::uint32_t runStart = 0;
            std::uint32_t runLength = 0;
            for (std::uint32_t k = 1; k <= W; ++k) {
                const std::uint32_t c = ringColumn(static_cast<long long>(begin) + k, W);
                if (st[c] == want) {
                    if (runLength == 0) {
                        runStart = c;
                    }
                    ++runLength;
                } else if (runLength > 0) {
                    runs.emplace_back(runStart, runLength);
                    runLength = 0;
                }
            }
            if (runLength > 0) {
                runs.emplace_back(runStart, runLength);
            }
            return runs;
        };
        for (const auto& [first, length] : runsOf(geom::OcclusionSpanState::Keep)) {
            if (length >= params.windowCols || length == W) {
                continue;
            }
            const std::uint32_t before = ringColumn(static_cast<long long>(first) - 1, W);
            const std::uint32_t after = ringColumn(static_cast<long long>(first) + length, W);
            if (st[before] == geom::OcclusionSpanState::Clamp && st[after] == geom::OcclusionSpanState::Clamp) {
                for (std::uint32_t k = 0; k < length; ++k) {
                    const std::uint32_t c = ringColumn(static_cast<long long>(first) + k, W);
                    if (arc.inArc[c]) {
                        st[c] = geom::OcclusionSpanState::Clamp;
                    }
                }
            }
        }
        for (const auto& [first, length] : runsOf(geom::OcclusionSpanState::Clamp)) {
            if (length < params.windowCols) {
                for (std::uint32_t k = 0; k < length; ++k) {
                    st[ringColumn(static_cast<long long>(first) + k, W)] = geom::OcclusionSpanState::Keep;
                }
            }
        }

        // ---- column boundaries -> the lens's polar angles -------------------------------
        // Boundary b sits between columns b - 1 and b; the ring maps onto
        // the lens's polar angle monotonically, in one direction or the
        // other depending on which side of the camera the lens faces.
        std::vector<double> boundary(W);
        for (std::uint32_t b = 0; b < W; ++b) {
            Vec3d dir;
            Vec2d px;
            if (!seamDirection(W, static_cast<double>(b), dir) || !lensPolarAngle(rig, i, dir, boundary[b], px)) {
                return Error{ErrorCode::Unsupported, "applyMountMask: the seam does not project into a lens"};
            }
        }
        double turn = 0.0;
        for (std::uint32_t b = 0; b < W; ++b) {
            turn += wrapPi(boundary[ringColumn(static_cast<long long>(b) + 1, W)] - boundary[b]);
        }
        const bool counterClockwise = turn >= 0.0;

        // ---- the rebuilt polygon, merging stretches until it fits the kernels ---------------
        geom::OcclusionClipParams clip;
        clip.usableRadiusPx =
            radiusAt(rig, i, std::max(L.thetaMaxRad, geom::effectiveThetaMax(i, blend))) + feather;
        clip.clampRadiusPx = radiusAt(rig, i, deg2rad(params.clampThetaDeg)) + feather;
        clip.maxVertices = OSV_MAX_OCCLUSION_POINTS;
        for (;;) {
            std::vector<geom::OcclusionSpan> spans;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> changedRuns;
            for (const geom::OcclusionSpanState want :
                 {geom::OcclusionSpanState::Release, geom::OcclusionSpanState::Clamp}) {
                for (const auto& [first, length] : runsOf(want)) {
                    if (length >= W) {
                        return Error{ErrorCode::Unsupported, "applyMountMask: a stretch covers the whole ring"};
                    }
                    const double a0 = boundary[first];
                    const double a1 = boundary[ringColumn(static_cast<long long>(first) + length, W)];
                    geom::OcclusionSpan span;
                    span.fromRad = counterClockwise ? a0 : a1;
                    span.toRad = counterClockwise ? a1 : a0;
                    span.state = want;
                    spans.push_back(span);
                    changedRuns.emplace_back(first, length);
                }
            }
            auto result = geom::clipOcclusionPolygon(poly, Vec2d{L.cx, L.cy}, spans, clip);
            if (result.ok()) {
                rebuilt[i] = std::move(result).value();
                break;
            }
            if (result.error().code != ErrorCode::Unsupported || changedRuns.empty()) {
                return result.error();
            }
            // Too many vertices (or a shape the rebuild refuses): keep the
            // narrowest changed stretch as the calibration drew it and try
            // again - the safe direction, the mask stays.
            const auto narrowest = std::min_element(changedRuns.begin(), changedRuns.end(),
                                                    [](const auto& x, const auto& y) { return x.second < y.second; });
            for (std::uint32_t k = 0; k < narrowest->second; ++k) {
                st[ringColumn(static_cast<long long>(narrowest->first) + k, W)] = geom::OcclusionSpanState::Keep;
            }
            ++applied.mergedStretches;
        }
        for (std::uint32_t c = 0; c < W; ++c) {
            if (st[c] == geom::OcclusionSpanState::Clamp) {
                ++applied.clampedColumns[i];
            }
        }
        // Changed = any vertex differs (Vec2d has no ==; exact comparison on
        // purpose: an unchanged polygon is the very same vertices).
        applied.changed[i] =
            rebuilt[i].size() != poly.size() ||
            !std::equal(rebuilt[i].begin(), rebuilt[i].end(), poly.begin(),
                        [](const Vec2d& x, const Vec2d& y) { return x.x == y.x && x.y == y.y; });
    }
    // ---- commit both lenses together ----------------------------------------------------------
    for (int i = 0; i < geom::kLensCount; ++i) {
        rig.occlusionPolyStream[static_cast<std::size_t>(i)] = std::move(rebuilt[i]);
    }
    return applied;
}

// =============================================================================
//  Text form and log line
// =============================================================================
std::string encodeMountColumns(const std::vector<std::uint8_t>& state) {
    std::string out;
    std::size_t c = 0;
    while (c < state.size()) {
        std::size_t e = c;
        while (e + 1 < state.size() && state[e + 1] == state[c]) {
            ++e;
        }
        out += std::format("{}{}:{}-{}", out.empty() ? "" : ",", static_cast<unsigned>(state[c]), c, e);
        c = e + 1;
    }
    return out;
}

Result<std::vector<std::uint8_t>> decodeMountColumns(std::string_view text, std::uint32_t columns) {
    if (columns == 0 || columns > 16384) {
        return Error{ErrorCode::InvalidArgument, "decodeMountColumns: bad ring width"};
    }
    std::vector<std::uint8_t> state;
    state.reserve(columns);
    std::size_t pos = 0;
    // Parse one unsigned number at `pos`, advancing past it.
    const auto number = [&](unsigned& v) {
        const char* first = text.data() + pos;
        const char* last = text.data() + text.size();
        const auto r = std::from_chars(first, last, v);
        if (r.ec != std::errc{} || r.ptr == first) {
            return false;
        }
        pos += static_cast<std::size_t>(r.ptr - first);
        return true;
    };
    const auto expect = [&](char ch) {
        if (pos >= text.size() || text[pos] != ch) {
            return false;
        }
        ++pos;
        return true;
    };
    while (pos < text.size()) {
        if (!state.empty() && !expect(',')) {
            return Error{ErrorCode::InvalidArgument, "decodeMountColumns: runs must be separated by ','"};
        }
        unsigned s = 0;
        unsigned a = 0;
        unsigned b = 0;
        if (!number(s) || !expect(':') || !number(a) || !expect('-') || !number(b)) {
            return Error{ErrorCode::InvalidArgument, "decodeMountColumns: a run is not <state>:<first>-<last>"};
        }
        if (s > kMountRelease || a != state.size() || b < a || b >= columns) {
            return Error{ErrorCode::InvalidArgument, "decodeMountColumns: a run is out of order or out of range"};
        }
        state.insert(state.end(), static_cast<std::size_t>(b - a + 1), static_cast<std::uint8_t>(s));
    }
    if (state.size() != columns) {
        return Error{ErrorCode::InvalidArgument, "decodeMountColumns: the runs do not cover the ring"};
    }
    return state;
}

std::string describeMountMask(const MountMask& mask) {
    if (!mask.valid()) {
        return "no verdict";
    }
    // Released stretches of the ring.
    std::size_t stretches = 0;
    for (std::uint32_t c = 0; c < mask.columns; ++c) {
        const std::uint32_t prev = ringColumn(static_cast<long long>(c) - 1, mask.columns);
        if (mask.state[c] == kMountRelease && mask.state[prev] != kMountRelease) {
            ++stretches;
        }
    }
    std::size_t releasedWindows = 0;
    std::size_t flatWindows = 0;
    for (const MountWindow& w : mask.windows) {
        releasedWindows += w.released ? 1u : 0u;
        flatWindows += (w.released && w.flat) ? 1u : 0u;
    }
    std::string text = std::format("released {} of {} arc columns ({:.1f} deg) in {} stretch{}", mask.releasedColumns,
                                   mask.arcColumns, 360.0 * mask.releasedColumns / mask.columns, stretches,
                                   stretches == 1 ? "" : "es");
    if (!mask.windows.empty()) {
        text += std::format("; {} of {} windows agree ({} flat)", releasedWindows, mask.windows.size(), flatWindows);
    }
    return text;
}

}  // namespace osv::render
