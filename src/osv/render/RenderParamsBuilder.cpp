// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/RenderParamsBuilder.h"
#include "osv/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::render {

RenderParamsBuilder::RenderParamsBuilder() = default;

RenderParamsBuilder& RenderParamsBuilder::rig(const geom::LensRig& rig) {
    m_rig = rig;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::camera(const geom::VirtualCamera& camera) {
    m_camera = camera;
    m_equirect.reset();
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::equirect(const geom::EquirectMap& map) {
    m_equirect = map;
    m_camera.reset();
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::stabilization(const Mat3d& bodyFromWorld) {
    m_bodyFromWorld = bodyFromWorld;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::blend(const geom::BlendParams& params, bool enabled) {
    m_blend = params;
    m_blendEnabled = enabled;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::gain(const Vec3d& slaveGain, const Vec3d& masterGain) {
    m_gain[0] = slaveGain;
    m_gain[1] = masterGain;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::seam(const std::vector<float>& shiftDeg) {
    m_seam = shiftDeg;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::warp(const std::vector<float>& uv, std::uint32_t w, std::uint32_t h,
                                               float latMinRad, float latMaxRad) {
    // Defensive: a grid whose payload does not match its declared size would
    // be read past its end by the kernel, so a mismatch disables the warp
    // rather than configuring it half way.  The latitudes must also describe
    // a real span - a degenerate one makes the kernel's row mapping divide by
    // approximately zero.
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2u;
    const bool sane = w > 0 && h > 1 && uv.size() == need && std::isfinite(latMinRad) && std::isfinite(latMaxRad) &&
                      std::fabs(latMaxRad - latMinRad) > 1e-6f;
    if (!sane) {
        return clearWarp();
    }
    m_warp = uv;
    m_warpW = w;
    m_warpH = h;
    m_warpLatMin = latMinRad;
    m_warpLatMax = latMaxRad;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::clearWarp() {
    m_warp.clear();
    m_warpW = 0;
    m_warpH = 0;
    m_warpLatMin = 0.0f;
    m_warpLatMax = 0.0f;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::color(const OsvColorParams& params) {
    m_color = params;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::alphaCoverage(bool enabled) {
    m_alphaCoverage = enabled;
    return *this;
}

RenderParamsBuilder& RenderParamsBuilder::lensEnabled(int lens, bool enabled) {
    if (lens == 0 || lens == 1) {
        m_lensEnabled[static_cast<std::size_t>(lens)] = enabled;
    }
    return *this;
}

Result<OsvRenderParams> RenderParamsBuilder::buildParams() const {
    if (!m_rig) {
        return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: rig() is required"};
    }
    if (!m_color) {
        return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: color() is required"};
    }
    if (!m_camera && !m_equirect) {
        return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: camera() or equirect() is required"};
    }

    OsvRenderParams p;
    std::memset(&p, 0, sizeof(p));

    // -------------------------------------------------------------------------
    //  Output geometry
    // -------------------------------------------------------------------------
    Mat3d viewToBody = Mat3d::identity();
    if (m_equirect || (m_camera && m_camera->projection == geom::Projection::Equirect)) {
        geom::EquirectMap map;
        if (m_equirect) {
            map = *m_equirect;
        } else {
            map.layout = geom::EquirectLayout::Standard;
            map.w = m_camera->w;
            map.h = m_camera->h;
        }
        if (!map.isValid()) {
            return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: invalid equirect size"};
        }
        p.mode = OSV_MODE_EQUIRECT;
        p.layout = map.layout == geom::EquirectLayout::PolarAxis ? OSV_LAYOUT_POLAR_AXIS : OSV_LAYOUT_STANDARD;
        p.outW = map.w;
        p.outH = map.h;
        p.focalPx = 1.0f;
        p.tanHalfH = 1.0f;
        p.tanHalfV = 1.0f;
        // A camera with equirect projection still contributes its rotation
        // (re-orienting the panorama); a bare EquirectMap does not.
        if (m_camera) {
            viewToBody = m_camera->rotation();
        }
    } else {
        const geom::VirtualCamera& cam = *m_camera;
        if (!cam.isValid()) {
            return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: invalid virtual camera"};
        }
        p.mode = OSV_MODE_REFRAME;
        p.outW = cam.w;
        p.outH = cam.h;
        switch (cam.projection) {
        case geom::Projection::Rectilinear: p.projection = OSV_PROJ_RECTILINEAR; break;
        case geom::Projection::Fisheye: p.projection = OSV_PROJ_FISHEYE; break;
        case geom::Projection::Stereographic: p.projection = OSV_PROJ_STEREOGRAPHIC; break;
        case geom::Projection::EyeOffset: p.projection = OSV_PROJ_EYE_OFFSET; break;
        case geom::Projection::Equirect: p.projection = OSV_PROJ_RECTILINEAR; break;  // handled above
        }
        p.focalPx = static_cast<float>(cam.focalPx());
        // The eye offset only matters for OSV_PROJ_EYE_OFFSET; the other
        // projections ignore it and the value is clamped to the model's range.
        p.eyeOffset = static_cast<float>(clampd(cam.eyeOffset, 0.0, 1.0));
        // effectiveHfovDeg() equals hfovDeg except for the eye-offset model,
        // which clamps itself below its asymptote.
        const double halfH = deg2rad(cam.effectiveHfovDeg()) * 0.5;
        p.tanHalfH = static_cast<float>(std::tan(halfH));
        p.tanHalfV = static_cast<float>(std::tan(halfH) * static_cast<double>(cam.h) / static_cast<double>(cam.w));
        viewToBody = cam.rotation();
    }

    // body <- view = (body <- world) * (world <- view).  With stabilisation
    // off the "world" is simply the body of the current frame.
    const Mat3d rout = m_bodyFromWorld * viewToBody;
    rout.toFloat9(p.Rout);

    // -------------------------------------------------------------------------
    //  Lenses
    // -------------------------------------------------------------------------
    const geom::LensRig& rig = *m_rig;
    for (int i = 0; i < 2; ++i) {
        OsvLens& L = p.lens[i];
        const geom::KannalaBrandt5& kb = rig.lens[static_cast<std::size_t>(i)];
        if (!kb.isValid()) {
            return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: lens " + std::to_string(i) + " is invalid"};
        }
        L.fx = static_cast<float>(kb.fx);
        L.fy = static_cast<float>(kb.fy);
        L.cx = static_cast<float>(kb.cx);
        L.cy = static_cast<float>(kb.cy);
        for (int k = 0; k < 5; ++k) {
            L.k[k] = static_cast<float>(kb.k[static_cast<std::size_t>(k)]);
        }
        rig.bodyToLens[static_cast<std::size_t>(i)].toFloat9(L.R);
        L.thetaMax = static_cast<float>(geom::effectiveThetaMax(i, m_blend));
        L.featherRad = static_cast<float>(deg2rad(m_blend.featherDeg));
        L.gain[0] = static_cast<float>(m_gain[static_cast<std::size_t>(i)].x);
        L.gain[1] = static_cast<float>(m_gain[static_cast<std::size_t>(i)].y);
        L.gain[2] = static_cast<float>(m_gain[static_cast<std::size_t>(i)].z);
        L.width = rig.streamW;
        L.height = rig.streamH;
        L.enabled = m_lensEnabled[static_cast<std::size_t>(i)] ? 1 : 0;

        // Occlusion polygon (optional, capped at the kernel's vertex budget).
        L.occlN = 0;
        L.occlFeatherPx = static_cast<float>(m_blend.occlusionFeatherPx);
        if (m_blend.useOcclusionMask) {
            const auto& poly = rig.occlusionPolyStream[static_cast<std::size_t>(i)];
            if (poly.size() >= 3) {
                const std::size_t n = poly.size() < OSV_MAX_OCCLUSION_POINTS ? poly.size() : OSV_MAX_OCCLUSION_POINTS;
                if (poly.size() > OSV_MAX_OCCLUSION_POINTS) {
                    log::warn("occlusion polygon has {} points; only {} are used", poly.size(),
                              OSV_MAX_OCCLUSION_POINTS);
                }
                for (std::size_t v = 0; v < n; ++v) {
                    L.occlX[v] = static_cast<float>(poly[v].x);
                    L.occlY[v] = static_cast<float>(poly[v].y);
                }
                L.occlN = static_cast<int>(n);
            }
        }
    }

    // -------------------------------------------------------------------------
    //  Blend / seam / colour
    // -------------------------------------------------------------------------
    p.blendEnabled = m_blendEnabled ? 1 : 0;
    p.seamShiftEnabled = m_seam.empty() ? 0 : 1;
    p.seamColumns = static_cast<int>(m_seam.size());
    p.warpEnabled = m_warp.empty() ? 0 : 1;
    p.warpW = static_cast<int>(m_warpW);
    p.warpH = static_cast<int>(m_warpH);
    p.warpLatMinRad = m_warpLatMin;
    p.warpLatMaxRad = m_warpLatMax;
    // Early-out limits for the kernel (see OsvRenderParams).  Only set when a
    // grid is present; a zero pair means "no early-out", never "no warp".
    p.warpSinLatLo = 0.0f;
    p.warpSinLatHi = 0.0f;
    if (!m_warp.empty()) {
        p.warpSinLatLo = std::sin(std::min(m_warpLatMin, m_warpLatMax));
        p.warpSinLatHi = std::sin(std::max(m_warpLatMin, m_warpLatMax));
    }
    p.outputAlphaCoverage = m_alphaCoverage ? 1 : 0;
    p.color = *m_color;
    return p;
}

Result<RenderJob> RenderParamsBuilder::build(const video::FramePair& frames) const {
    OSV_TRY_ASSIGN(OsvRenderParams params, buildParams());
    RenderJob job;
    job.params = params;
    for (std::size_t i = 0; i < 2; ++i) {
        const video::PlanarFrame16& f = frames.lens[i];
        const video::DeviceFrameRef& dev = frames.device[i];
        // A frame the decoder left on the GPU (keepOnDevice) carries its size
        // and timing but NO host planes, so f.valid() is false for it by
        // design.  Rejecting that as "invalid" is what broke
        // `osvtool --hw cuda --device cuda` ("lens frame 0 is invalid"):
        // exactly the zero-copy path the CUDA renderer was written for.
        const bool onHost = f.valid();
        const bool onDevice = !onHost && dev.valid();
        if (!onHost && !onDevice) {
            return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: lens frame " + std::to_string(i) +
                                                         " has neither host planes nor a device frame"};
        }
        const std::uint32_t fw = onHost ? f.width : dev.width;
        const std::uint32_t fh = onHost ? f.height : dev.height;
        if (static_cast<int>(fw) != params.lens[i].width || static_cast<int>(fh) != params.lens[i].height) {
            return Error{ErrorCode::InvalidArgument,
                         "RenderParamsBuilder: frame size does not match the rig (" + std::to_string(fw) + "x" +
                             std::to_string(fh) + " vs " + std::to_string(params.lens[i].width) + "x" +
                             std::to_string(params.lens[i].height) + ")"};
        }
        const bool described = onHost ? fillPlane(f, job.planes[i]) : fillDevicePlane(dev, job.planes[i]);
        if (!described) {
            return Error{ErrorCode::InvalidArgument, "RenderParamsBuilder: cannot describe lens frame"};
        }
        job.planesOnDevice[i] = onDevice;
        job.frames[i] = f;
        job.deviceFrames[i] = dev;
    }
    job.seamShiftDeg = m_seam;
    job.warpGrid = m_warp;
    return job;
}

}  // namespace osv::render
