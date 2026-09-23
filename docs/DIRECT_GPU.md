# Direct GPU pipeline

Status: in progress (approved 2026-09-22). This document is the contract the
work packages below are built against; change it before changing an interface.

## Why

Measured on the reference machine (RTX 5090, 32 cores, sample clip
`example_footage_dlogm.OSV`, dual 3000x3000 10-bit HEVC at 59.94 fps):

| Stage, one 2560x1440 view | Old Premiere pipeline | Hardware floor |
|---|---|---|
| Decode both lenses | 5-50 ms + a host copy (software: 949 ms median to park on a new frame) | **2.1 ms** - NVDEC sustains 467 frame pairs/s, D3D11VA 496, frames stay in VRAM |
| Build the view | stitch a 6000x3000 equirect on the GPU, read 18 MP back, convert it on the CPU, Premiere uploads it again, then reframe it (CPU path: 12 ms) | **~0.3 ms** - one kernel from the fisheyes into Premiere's GPU frame |
| Parallax measurement | 16 ms CPU DIS per 8-frame bucket (was 205 ms) | ~1-2 ms on the GPU |

The old design routes a full stitched sphere through host memory so that a
2560x1440 window can be cut out of it. That round trip, not any arithmetic, is
the cost. It is also a quality loss: two resamplings (fisheye -> equirect ->
view) instead of one, and a 90 degree view at 2560 wide needs a 10240-wide
equirect for 1:1 sampling - the fisheyes have that density, the 6000-wide
equirect does not.

## Target architecture

```
                         Premiere render thread (GPU filter Render)
                                         |
  Open360Reframe.aex  --- C ABI --->  OpenOSVImporter.prm  (the ENGINE lives here)
   thin client:                        ClipEngine registry (per source file):
   - finds its source clip             - metadata, rig, colour, stabilisation
     (Video Segment node walk)         - GpuClipDecoder: NVDEC into Premiere's
   - maps clip time -> media frame       CUcontext, VRAM frame cache, GOP-aware,
   - passes view params, CUcontext,      decode-ahead                     (WP-A)
     stream, output device pointer     - GPU analyses: band shading + CUDA DIS
                                         -> seam table / gain / parallax  (WP-B)
                                       - fused kernel: fisheyes -> view,
                                         straight into Premiere's frame   (WP-C)
```

* One process-wide engine, inside the importer module, exported through a
  versioned C ABI (`OsvEngine_*`). The effect never links the engine; it finds
  the loaded importer module and resolves the entry points, so the two plug-ins
  keep independent builds and a missing/old importer degrades cleanly to the
  existing equirect path.
* Everything per frame stays on the GPU in Premiere's own CUDA context. The
  only host traffic is parameters and a few KB of analysis results.
* The CPU path (software renderer, Media Encoder without a GPU) keeps working
  through the existing equirect route until the same kernel is wired to CPU
  decode; output must match the GPU path.
* The importer keeps producing the equirect (for users who want the 360 clip
  itself) and becomes cheap when the effect is present.

## Work packages

### WP-A  GPU clip decoder (`osv::video::GpuClipDecoder`)

New files: `include/osv/video/GpuClipDecoder.h`, `src/osv/video/GpuClipDecoder.cpp`,
tests `tests/unit/test_gpu_decoder.cpp`.

* NVDEC through FFmpeg's CUDA hwaccel, decoding into an **externally supplied
  `CUcontext`** (`av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA)`, set
  `AVCUDADeviceContext::cuda_ctx` (and `stream` when given), `av_hwdevice_ctx_init`).
  With no context supplied, use the device's primary context
  (`AV_CUDA_USE_PRIMARY_CONTEXT`). Never create a private context per decoder.
* CUDA **driver API only** in new code (`cuda.h`): the Premiere side must not
  depend on a cudart DLL.
* Own VRAM frame store: each decoded NVDEC surface pair is copied
  (`cuMemcpy2DAsync`, device to device) into pooled buffers, so FFmpeg's small
  surface pool is never pinned by the cache. Layout per lens: P010, luma plane
  plus interleaved CbCr, one pitch - exactly what `DeviceFrameRef` and
  `fillDevicePlane()` describe.
* Cache policy: keyed by frame index; capacity from a VRAM budget (default:
  min(1.5 GB, 20 % of free VRAM), overridable). A random access to frame k
  decodes from the previous sync sample and **keeps every frame it decoded on
  the way**, so stepping backwards/forwards inside that GOP is a cache hit.
  Eviction: least recently used, never a leased slot.
* Decode-ahead: a worker thread that, after sequential access is detected,
  decodes up to N frames ahead (default 8) into the cache; cancellable;
  pushes/pops the context itself.
* API sketch (final names are WP-A's call, recorded here when settled):

  ```cpp
  struct GpuDecoderOptions {
      void* cuContext = nullptr;       // CUcontext; nullptr = primary context of cudaDevice
      void* cuStream = nullptr;        // CUstream the copies are ordered on; nullptr = own stream
      int cudaDevice = 0;
      std::size_t vramBudgetBytes = 0; // 0 = automatic
      std::uint32_t decodeAhead = 8;
  };
  class GpuFrameLease;                 // pins one cached pair; RAII release
  class GpuClipDecoder {
  public:
      static Result<std::unique_ptr<GpuClipDecoder>> open(const std::filesystem::path&,
                                                          const meta::FormatInfo&,
                                                          const GpuDecoderOptions&);
      Result<GpuFrameLease> acquire(std::uint32_t frameIndex);  // thread-safe
      // lease.pair() -> video::FramePair with device[] filled, lens[] sized, no host planes
  };
  ```
* Thread safety: several Premiere render threads call `acquire` concurrently.
* Acceptance: frames bit-exact with software decode (after the P010 shift) at
  scattered indices; park (12 scattered frames) median < 60 ms cold-GOP and
  < 1 ms on a cache hit; sequential with decode-ahead >= 300 frame pairs/s on
  the reference machine; VRAM stays under budget; destruction while the worker
  is decoding neither hangs nor leaks.

### WP-B  GPU analyses (band shading + CUDA DIS)

New files under `src/osv/render/cuda/` plus tests `tests/unit/test_disflow_cuda.cpp`.

* A CUDA DIS solver with the same maths and DJI constants as
  `src/osv/render/DisFlow.cpp` (Kroeger et al., ECCV 2016: pyramid, inverse
  structure tensors, inverse-search patch solve, photometric-weight densify,
  Gaussian smoothing, forward/backward consistency). Exposed as a new
  `FlowBackendKind` so `computeFlow()` / `parallaxFromBands()` use it
  unchanged. Parity with the CPU solver is tolerance-based (float order
  differs): mean endpoint difference < 0.05 px on the sample bands, and the
  resulting parallax grid's gated cells within 2 %.
* Band shading on the GPU: `renderLensBands()` must work when the frames are
  device-resident (`RenderJob::planesOnDevice`), shading only the band rows on
  the GPU and downloading the small luma/alpha bands. This makes seam search,
  gain estimation and the parallax measurement all work from `GpuClipDecoder`
  frames. The library dependency runs cpu -> cuda, so the CPU library needs an
  injectable band shader (or an equivalent seam) rather than a direct call.
* Acceptance: whole parallax measurement for one bucket (bands + bidirectional
  flow + grid) < 3 ms on the reference machine from device frames.

**Settled (WP-B delivered).**  Interface, as built:

  ```cpp
  // include/osv/render/CudaAnalysis.h  (osv_render_cuda)
  bool   cudaAnalysesAvailable(std::string* reason = nullptr);
  Status installCudaAnalyses();          // installs both hooks below; Unsupported without a GPU
  void   uninstallCudaAnalyses() noexcept;
  bool   cudaAnalysesInstalled() noexcept;
  Result<BidirFlow> cudaDisFlowBidirectional(const GrayImage&, const GrayImage&,
                                             const DisFlowParams&, void* stream = nullptr);
  Result<BidirFlow> cudaDisFlowBidirectionalDevice(const float* a, const float* b, uint32_t w,
                                                   uint32_t h, size_t pitchBytes,
                                                   const DisFlowParams&, void* stream = nullptr);
  Result<std::vector<float>> cudaShadeBandRgba(const RenderJob&, uint32_t row0, uint32_t row1,
                                               void* stream = nullptr);
  Status cudaShadeBandLumaAlpha(const RenderJob&, uint32_t row0, uint32_t row1,
                                std::vector<float>& luma, std::vector<float>& alpha,
                                void* stream = nullptr);

  // osv_render_cpu seams the CUDA library installs into
  enum class FlowBackendKind { Auto = 0, Classical = 1, Neural = 2, ClassicalCuda = 3, Count };
  void setCudaFlowBackendFactory(FlowBackendFactory);          // FlowBackend.h
  class DeviceBandShader;                                      // DeviceBandShader.h
  void setDeviceBandShader(std::shared_ptr<DeviceBandShader>);
  Result<ParallaxWarpGrid> gridFromFlow(bands, flow, params, ThreadPool* pool = nullptr);
  ```

  A caller with device-only frames calls `installCudaAnalyses()` once; after
  that `renderLensBands` / `searchSeam` / `estimateGain` / `overlapNcc` /
  `measureParallaxBands` shade the band rows on the GPU automatically whenever
  the job's planes are on the device, and `ParallaxWarpParams::backend =
  FlowBackendKind::ClassicalCuda` puts the flow there too.  Host frames keep
  the CPU band path unchanged.  Everything runs in the CUDA context current on
  the calling thread (never `cudaSetDevice` / `cudaDeviceReset` /
  `cudaDeviceSynchronize`), ordered on the caller's stream or a pooled
  per-context one, synchronising only that stream.

  Parity: the CUDA DIS field is **bit-identical** to `DisFlow.cpp` (synthetic
  pairs and the sample's real bands, frames 0/32/64, compared with `==`), so
  the parallax grids are identical too; GPU band shading is 108-111 dB PSNR
  against the CPU band path.  Measured (`osv_gpu_analysis_bench`, medians,
  frames 0/32/64): bands 0.25 ms, bidirectional flow 0.79 ms, grid 0.95-1.2 ms,
  end to end 2.1-2.7 ms; the CPU path on 32 threads is 21-23 ms.

  Deviations: the grid (`gridFromFlow`) stays on the CPU - it was ~7-9 ms
  single-threaded, which nobody had measured, and is now split over the pool
  and restructured to ~1 ms with a byte-identical result.  `Auto` never picks
  `ClassicalCuda` (existing selections do not change underneath callers); the
  plug-in preference enum is not extended yet (WP-D).  The analyses use the
  CUDA runtime (`cudart_static`, as `osv_render_cuda` already does), bound to
  whatever driver context is current - not the driver-only API the plug-in
  rule asks of new Premiere-side code.

### WP-C  Fused fisheye -> view kernel for the effect

Files under `plugins/reframe/` (kernel added to `ReframeKernel.cu`'s fatbin or a
sibling), tests under `tests/premiere/reframe/`.

* A driver-API kernel that runs `osvShadePixelW()` in `OSV_MODE_REFRAME` from
  device P010 planes (+ seam table, + warp grid) and writes Premiere GPU frame
  pixels (BGRA 32f or 16f, top-left origin, pitch in bytes).
* A host-side parameter builder that turns the effect's `Settings` (pan, tilt,
  roll, FOV, distortion, preset, source pan/tilt/roll, output resolution
  cover-fit, sequence size) plus the clip's stitch state (rig, colour, blend,
  seam/warp, per-frame stabilisation rotation) into `OsvRenderParams`.
  **The framing must be identical to the existing two-step path** (importer
  equirect -> `osvReframeEquirectPixel`): same rotation conventions, same
  eye-offset/distortion mapping, same cover-fit. If `OsvRenderParams` lacks a
  field the equirect path relies on (e.g. a viewport rectangle), add it to
  `osv_kernel.h` in all dialects and keep the parameter block <= 4 KB.
* Acceptance: against the two-step path on the sample clip at several views
  (rectilinear, wide eye-offset, tiny planet, across the seam, rolled), the
  direct render has the same framing (geometric alignment within 0.5 px,
  measured) and is at least as sharp (higher high-frequency energy); a
  CPU-reference twin of the kernel agrees with the GPU to PSNR >= 60 dB.

### WP-D  Engine ABI, registry and effect integration (lead)

* `OsvEngine_*` C ABI exported from the importer; ClipEngine registry keyed by
  file identity; prefs published by live importer instances.
* Effect: node walk to the media node (evidence from the
  `reframe/gpu/source:` probe log), clip time -> media frame via
  `TransformNodeTime`, output colour in the sequence working space, fallback to
  the equirect path on any failure.
* Prerequisite, open: Premiere must call our GPU filter at all. Every recorded
  session loaded `xGPUFilterEntry` and never called `CreateInstance`; the
  out-flags now match the two known-good GPU effects (db6eb14) pending a test.

## Rules for every package

* Heavy Doxygen, defensive checks, no TODOs or stubs, no fake data.
* CUDA driver API in anything the plug-ins load; `-fmad=false`, no fast-math.
* Tests never touch the user's real plug-in logs (`isolatePluginLogs()` in any
  new Premiere test main); `[sample]` tests read `OSV_SAMPLE_FILE`.
* Never launch Premiere; never install plug-ins.
