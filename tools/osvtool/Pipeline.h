// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Pipeline: the object graph shared by `render`, `seam` and `selfcheck`.
// Opens the clip, decodes the metadata, builds the lens rig, the attitude
// timeline, the colour parameters, the dual-stream decoder and the renderer
// from one options struct that every command fills from CLI11 flags.
#pragma once

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/Stabilization.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/Renderer.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osvtool {

/// Everything a command can influence about how a clip is interpreted.
struct PipelineOptions {
    std::filesystem::path input;

    // Calibration / geometry conventions (defaults = verified values)
    std::string calib = "native";                 ///< native | lens-guards | underwater
    std::optional<double> stitchDistanceM;        ///< far_XX preset selection
    std::optional<double> cropScale;              ///< override StreamScaling
    std::string focalSource = "dfl";              ///< dfl | scaled
    std::string extrinsicOrder = "wxyz";          ///< wxyz | xyzw
    std::string extrinsicSense = "body2lens";     ///< body2lens | lens2body
    double lensFovDeg = 195.18;
    double featherDeg = 4.0;
    bool occlusionMask = true;
    bool blend = true;

    // Stabilisation
    std::string stab = "off";                     ///< off | horizon | full | smooth
    std::string attitudeConvention = "auto";      ///< auto | xyzw-b2w-ny | wxyz-w2b-z | ...
    double smoothSigmaFrames = 15.0;

    // Colour
    std::string color = "pq";                     ///< pq | hlg | 709 | linear | dlogm
    std::string fit = "osmo360";                  ///< osmo360 | dji | pocket3
    std::string inputEncoding = "auto";           ///< auto | dlogm | hlg | normal
    double exposureStops = 0.0;

    // Decode / render backends
    std::string hw = "none";                      ///< none | d3d11va | cuda | auto
    std::string device = "auto";                  ///< cpu | cuda | opencl | auto
    int threads = 0;

    /// True when a per-frame analysis (seam search, gain match, parallax)
    /// reads the decoded pixels on the CPU.  Those analyses shade bands from
    /// HOST planes, so a CUDA decode must then copy its frames back instead
    /// of leaving them on the GPU for the zero-copy render.  Not a flag -
    /// the command sets it from the analyses it was asked for.
    bool hostFramesRequired = false;
};

/// Register the option flags shared by the commands on `sub`.
void addPipelineOptions(CLI::App* sub, PipelineOptions& opt);

/// The opened pipeline.
struct Pipeline {
    PipelineOptions options;
    std::unique_ptr<osv::OsvFile> file;
    osv::meta::MetadataTrack track;
    osv::meta::FormatInfo format;
    osv::meta::CalibrationSet calibration;
    osv::geom::LensRig rig;
    osv::geom::BlendParams blendParams;
    osv::color::InputEncoding inputEncoding = osv::color::InputEncoding::DLogM;
    osv::color::OutputTransfer outputTransfer = osv::color::OutputTransfer::PQ;
    OsvColorParams color{};
    std::optional<osv::geom::AttitudeTrack> attitude;
    osv::geom::StabilizationParams stabParams;
    std::vector<osv::Quatd> smoothedAttitude;    ///< Per-frame smoothed quaternions (Smooth mode).
    osv::Quatd referenceAttitude;
    std::unique_ptr<osv::ThreadPool> pool;
    std::unique_ptr<osv::video::DualStreamReader> reader;
    std::unique_ptr<osv::render::IRenderer> renderer;
    std::string rendererName;
    std::vector<std::string> notes;

    /// Open everything.  Fails with Io for a missing file and Malformed for a
    /// clip without usable calibration.
    static osv::Result<std::unique_ptr<Pipeline>> open(const PipelineOptions& options, bool needRenderer = true);

    /// Body-from-world correction for `frameIndex` (identity when off).
    [[nodiscard]] osv::Mat3d stabilizationFor(std::uint32_t frameIndex) const;

    /// Number of video frames.
    [[nodiscard]] std::uint32_t frameCount() const noexcept;

    /// Frame rate.
    [[nodiscard]] double fps() const noexcept;
};

/// Parse "WxH" into two integers (false on malformed input).
bool parseSize(const std::string& text, int& w, int& h);

}  // namespace osvtool
