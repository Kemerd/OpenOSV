// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Hardware acceleration selection and the decoder option block.
//
// The video module always has a software (libavcodec hevc / h264) path; the
// hardware paths are opt-in because they change the frame memory layout
// (P010 instead of planar yuv420p10) and because a host application may
// already own the GPU.  `HwAccel::Auto` tries CUDA, then D3D11VA (on macOS:
// VideoToolbox), then falls back to software and never fails just because no
// GPU is present.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace osv::video {

/// Hardware decode back-end requested by the caller.
enum class HwAccel : std::uint8_t {
    None = 0,     ///< libavcodec software decoding (always available).
    D3D11VA = 1,  ///< Direct3D 11 video acceleration (any Windows GPU vendor).
    Cuda = 2,     ///< NVDEC through FFmpeg's CUDA hwaccel (device pointers exposable).
    Auto = 3,     ///< Try Cuda, then D3D11VA (macOS: VideoToolbox), then None.
    /// Apple's VideoToolbox through FFmpeg's hwaccel (macOS).  Frames come
    /// back to host memory as P010, exactly like the D3D11VA path.
    VideoToolbox = 4
};

/// Stable lower-case name of a back-end ("none", "d3d11va", "cuda", "auto",
/// "videotoolbox").
[[nodiscard]] const char* hwAccelName(HwAccel hw) noexcept;

/// Parse a back-end name as typed on a command line (case-insensitive).
/// Returns std::nullopt for anything that is not one of the five names (or
/// their aliases: sw / software, d3d11, nvdec, vt).
[[nodiscard]] std::optional<HwAccel> parseHwAccel(std::string_view text) noexcept;

/// Everything that influences how a stream decoder is opened.
struct DecoderOptions {
    /// Which hardware path to use (see HwAccel).
    HwAccel hw = HwAccel::None;

    /// libavcodec thread count (0 = let libavcodec pick from the CPU count,
    /// at most 16).  Applies to the hardware paths too: libavcodec pipelines
    /// hardware submissions over its frame threads, and every frame thread
    /// adds one decoder surface to the hardware pool and one frame of delay
    /// before the first picture comes out.
    int threads = 0;

    /// CUDA only: leave decoded frames in device memory and expose them via
    /// HevcStreamDecoder::lastDeviceFrame().  The PlanarFrame16 returned by
    /// decodeFrame()/next() then carries dimensions, timing and the owning
    /// AVFrame but NO host plane pointers (valid() is false).  Callers that
    /// need host pixels must leave this false.
    bool keepOnDevice = false;

    /// CUDA only: ordinal of the device to decode on.  With `cudaContext`
    /// set it must name that context's device; it is then only reported back
    /// in DeviceFrameRef::deviceIndex.
    int cudaDeviceIndex = 0;

    /// CUDA only: an existing CUcontext to decode into, passed as void* so
    /// this header never needs cuda.h.  nullptr keeps the historical
    /// behaviour, where FFmpeg creates a context of its own for
    /// `cudaDeviceIndex`.  With a context supplied the FFmpeg device context
    /// is built around it (av_hwdevice_ctx_alloc, AVCUDADeviceContext::cuda_ctx,
    /// av_hwdevice_ctx_init): FFmpeg then never creates, retains or destroys
    /// a context, and every NVDEC surface it allocates lives in the caller's
    /// context, where the caller's kernels can read it.  Allocations made in
    /// one CUDA context are not usable by kernels running in another, which
    /// is why a host application that renders in its own context needs this.
    /// The caller keeps the context alive for the decoder's whole lifetime.
    /// Requires a build with the CUDA toolkit (OSV_VIDEO_HAVE_CUDA);
    /// otherwise open() fails with Unsupported.
    void* cudaContext = nullptr;

    /// CUDA only, used together with `cudaContext`: the CUstream FFmpeg
    /// orders its surface copies and the NVDEC post-processing on.  nullptr
    /// is FFmpeg's default, the context's legacy default stream, which
    /// serialises against every blocking stream of that context; callers
    /// that share the context with a renderer should pass a non-blocking
    /// stream of their own.
    void* cudaStream = nullptr;

    /// Bypass libavformat: open the file with the OpenOSV container parser and
    /// hand every sample of the selected track straight to libavcodec.  The
    /// decoded frame index then equals the container sample index (and thus
    /// the djmd metadata sample index) by construction.  Requires a track
    /// whose sample entry carries an hvcC (native OSV streams) or avcC (LRF
    /// proxy) record; anything else returns Unsupported.
    bool useContainerSamples = false;

    /// Hardware paths (D3D11VA / CUDA without `cudaContext`): take the
    /// process-wide shared device of this type and adapter when one is alive
    /// instead of creating a device per decoder.  Creating a device costs
    /// ~120 ms (D3D11) / ~50 ms (CUDA) on the sample machine, more than
    /// everything else in open() together.  Devices are held weakly: one
    /// lives while a decoder or a frame uses it, so a process that stops
    /// decoding gets the GPU memory back.  false restores a private device
    /// per decoder.
    bool shareHwDevice = true;

    /// Which of the shared devices of one type and adapter to use (0 = the
    /// common one).  FFmpeg serialises every surface download of a D3D11
    /// device behind one lock, so two decoders that run in PARALLEL on one
    /// device download one after the other; giving them different slots keeps
    /// their downloads concurrent while still sharing each device with every
    /// other decoder of the same slot.  Ignored without `shareHwDevice`.
    int hwDeviceSlot = 0;

    /// Skip decoding frame 0 inside open() when the container's parameter
    /// sets already describe the stream (size and pixel format known after
    /// avcodec_open2 - always the case for the hvcC streams in container-
    /// sample mode).  open() then costs no decode at all; the first
    /// decodeFrame() pays for what it decodes anyway, and a stream the GPU
    /// cannot decode falls back to software on that first decode (activeHw()
    /// reports the fallback from then on) instead of inside open().  When
    /// the parameters are NOT known up front the probe runs as usual, so the
    /// reported geometry is never a guess.
    bool deferFirstFrame = false;
};

}  // namespace osv::video
