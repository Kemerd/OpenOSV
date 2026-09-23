// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SynthFisheye.h - synthetic dual-fisheye frames for the unit tests.
//
// A known scene rendered into two fisheye frames through the sample clip's
// own calibration (no clip needed): every luma pixel and chroma site is
// unprojected through the lens model to a body ray, lit by a caller-supplied
// radiance, D-Log M encoded and stored as 10-bit narrow Y'CbCr - the exact
// inverse of what the kernel does, so the kernel reads back what the scene
// says.  Written for the photometric seam tests (test_photoseam.cpp) and
// shared with the seam tools tests (test_seam_tools.cpp).
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/video/PlanarFrame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace osv::testsynth {

/// The sample clip's calibration as a stream-space rig at `streamW` pixels
/// (the verified constants, so this does not depend on the metadata decoder;
/// the same numbers tests/unit/test_render.cpp uses).
inline Result<geom::LensRig> makeSyntheticRig(int streamW) {
    meta::CalibrationSet cal;
    auto fill = [](meta::DewarpParams& d, float fx, float fy, float cx, float cy, const float k[5], float qw, float qx,
                   float qy, float qz) {
        d.fx = fx;
        d.fy = fy;
        d.cx = cx;
        d.cy = cy;
        for (int i = 0; i < 5; ++i) {
            d.k[static_cast<std::size_t>(i)] = k[i];
        }
        d.width = 3840;
        d.height = 3840;
        d.camExtriQ.w = qw;
        d.camExtriQ.x = qx;
        d.camExtriQ.y = qy;
        d.camExtriQ.z = qz;
        d.camExtriQ.present = true;
        for (int b = 1; b <= 11; ++b) {
            d.present.set(static_cast<std::size_t>(b));
        }
        d.present.set(28);
    };
    const float ks[5] = {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f};
    const float km[5] = {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f};
    fill(cal.slave, 1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, ks, 0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f);
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f, -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

/// Owns the 10-bit planar storage of one synthetic lens frame.
struct SynthFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

/// Scene-linear RGB seen by one lens along a body direction.  `thetaDeg` is
/// the ray's angle from that lens's own axis, so a lens-specific fall-off
/// can be modelled.
using Radiance = std::function<void(int lens, const Vec3d& dBody, double thetaDeg, double rgb[3])>;

/// Inverse of the D-Log M decode by table: code -> linear is monotone, so a
/// dense table plus linear interpolation is exact to far below one 10-bit
/// code step - and 1000x faster than a bisection per pixel.
class CodeEncoder {
public:
    explicit CodeEncoder(const OsvColorParams& cp) : m_cp(cp) {
        m_lin.resize(kN);
        for (int i = 0; i < kN; ++i) {
            const float c = static_cast<float>(i) / static_cast<float>(kN - 1);
            m_lin[static_cast<std::size_t>(i)] = static_cast<double>(osvDlogmToLinear(&m_cp.curve, c));
        }
        // The Y'CbCr -> R'G'B' matrix inverted once (the kernel applies the
        // forward one in osvYuvToCode).
        const double* k = nullptr;
        double m[9];
        for (int i = 0; i < 9; ++i) {
            m[i] = static_cast<double>(m_cp.yuvToRgb[i]);
        }
        k = m;
        const double det = k[0] * (k[4] * k[8] - k[5] * k[7]) - k[1] * (k[3] * k[8] - k[5] * k[6]) +
                           k[2] * (k[3] * k[7] - k[4] * k[6]);
        m_inv[0] = (k[4] * k[8] - k[5] * k[7]) / det;
        m_inv[1] = (k[2] * k[7] - k[1] * k[8]) / det;
        m_inv[2] = (k[1] * k[5] - k[2] * k[4]) / det;
        m_inv[3] = (k[5] * k[6] - k[3] * k[8]) / det;
        m_inv[4] = (k[0] * k[8] - k[2] * k[6]) / det;
        m_inv[5] = (k[2] * k[3] - k[0] * k[5]) / det;
        m_inv[6] = (k[3] * k[7] - k[4] * k[6]) / det;
        m_inv[7] = (k[1] * k[6] - k[0] * k[7]) / det;
        m_inv[8] = (k[0] * k[4] - k[1] * k[3]) / det;
    }

    /// Linear value -> log code in [0, 1].
    [[nodiscard]] double code(double linear) const {
        if (!(linear > m_lin.front())) {
            return 0.0;
        }
        if (linear >= m_lin.back()) {
            return 1.0;
        }
        const auto it = std::upper_bound(m_lin.begin(), m_lin.end(), linear);
        const std::size_t hi = static_cast<std::size_t>(it - m_lin.begin());
        const std::size_t lo = hi - 1;
        const double t = (linear - m_lin[lo]) / (m_lin[hi] - m_lin[lo]);
        return (static_cast<double>(lo) + t) / static_cast<double>(kN - 1);
    }

    /// Linear RGB -> 10-bit narrow Y, Cb, Cr codes (unrounded).
    void yuv(const double rgb[3], double out[3]) const {
        const double c[3] = {code(rgb[0]), code(rgb[1]), code(rgb[2])};
        const double yn = m_inv[0] * c[0] + m_inv[1] * c[1] + m_inv[2] * c[2];
        const double un = m_inv[3] * c[0] + m_inv[4] * c[1] + m_inv[5] * c[2];
        const double vn = m_inv[6] * c[0] + m_inv[7] * c[1] + m_inv[8] * c[2];
        out[0] = yn / static_cast<double>(m_cp.yuvScaleY) + static_cast<double>(m_cp.yuvBlack);
        out[1] = un / static_cast<double>(m_cp.yuvScaleC) + 512.0;
        out[2] = vn / static_cast<double>(m_cp.yuvScaleC) + 512.0;
    }

private:
    static constexpr int kN = 8192;
    OsvColorParams m_cp;
    std::vector<double> m_lin;
    double m_inv[9] = {};
};

/// Quantise to a 10-bit code.
inline std::uint16_t q10(double v) { return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 1023L)); }

/// Render lens `lens` of `rig` from `scene`: every luma pixel and every
/// chroma site is unprojected through the lens model to a body ray, lit by
/// `scene`, D-Log M encoded and stored as 10-bit narrow Y'CbCr - the inverse
/// of what the kernel does, so the kernel reads back what the scene says.
inline SynthFrame synthLens(const geom::LensRig& rig, int lens, const Radiance& scene, const CodeEncoder& enc,
                            ThreadPool& pool) {
    const std::uint32_t w = static_cast<std::uint32_t>(rig.streamW);
    const std::uint32_t h = static_cast<std::uint32_t>(rig.streamH);
    const std::uint32_t cw = (w + 1) / 2;
    const std::uint32_t ch = (h + 1) / 2;
    SynthFrame s;
    s.storage = std::make_shared<std::vector<std::uint16_t>>(static_cast<std::size_t>(w) * h +
                                                             2u * static_cast<std::size_t>(cw) * ch);
    std::uint16_t* Y = s.storage->data();
    std::uint16_t* U = Y + static_cast<std::size_t>(w) * h;
    std::uint16_t* V = U + static_cast<std::size_t>(cw) * ch;
    const geom::KannalaBrandt5& kb = rig.lens[static_cast<std::size_t>(lens)];
    const Mat3d lensToBody = rig.bodyToLens[static_cast<std::size_t>(lens)].transposed();

    // YCbCr of the scene at continuous lens pixel (px, py); black outside the circle.
    const auto sample = [&](double px, double py, double out[3]) {
        auto d = kb.unproject(Vec2d{px, py});
        if (!d.ok()) {
            out[0] = 64.0;
            out[1] = out[2] = 512.0;
            return;
        }
        const Vec3d dl = d.value();
        const double theta = rad2deg(std::atan2(std::hypot(dl.x, dl.y), dl.z));
        double rgb[3] = {0, 0, 0};
        scene(lens, lensToBody * dl, theta, rgb);
        enc.yuv(rgb, out);
    };
    // Luma at pixel centres; chroma at the kernel's left-sited positions
    // (chroma sample (j, i) sits at luma (2j + 0.5, 2i + 1), osvSampleYuv).
    (void)pool.parallelFor(0, h, 8, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                double v[3];
                sample(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, v);
                Y[y * w + x] = q10(v[0]);
            }
        }
    });
    (void)pool.parallelFor(0, ch, 8, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            for (std::uint32_t j = 0; j < cw; ++j) {
                double v[3];
                sample(2.0 * j + 0.5, 2.0 * static_cast<double>(i) + 1.0, v);
                U[i * cw + j] = q10(v[1]);
                V[i * cw + j] = q10(v[2]);
            }
        }
    });
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane = {Y, U, V};
    s.frame.strideElems = {w, cw, cw};
    s.frame.bitDepth = 10;
    s.frame.bitShift = 0;
    s.frame.chromaInterleaved = false;
    s.frame.narrowRange = true;
    s.frame.owner = s.storage;
    return s;
}

/// Both lenses of a synthetic scene, kept alive together with their pair.
struct SynthPair {
    SynthFrame lens[2];
    video::FramePair pair;
};

inline SynthPair synthPair(const geom::LensRig& rig, const Radiance& scene, ThreadPool& pool) {
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    const CodeEncoder enc(cp);
    SynthPair p;
    for (int i = 0; i < 2; ++i) {
        p.lens[i] = synthLens(rig, i, scene, enc, pool);
    }
    p.pair.lens = {p.lens[0].frame, p.lens[1].frame};
    return p;
}

/// Polar-axis longitude / latitude (radians) of a body direction.
inline double polarLon(const Vec3d& d) { return std::atan2(d.x, d.z); }
inline double polarLat(const Vec3d& d) { return std::asin(std::clamp(d.y, -1.0, 1.0)); }

}  // namespace osv::testsynth
