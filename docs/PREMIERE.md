# Premiere Pro plug-ins (milestone 2)

OpenOSV ships three native Premiere Pro plug-ins for Windows. Together they
give the same workflow as DJI's macOS-only "Reframe for Adobe Premiere": drop
an `.OSV` on the timeline, get a stitched 360 clip, see the stitch options in
the Effect Controls panel, add one effect to reframe it with host keyframes and
GPU acceleration. All three are built from this repository against the Adobe
SDKs (never committed) and share the milestone 1 library.

| Binary | Kind | Entry point | What it does |
|---|---|---|---|
| `OpenOSVImporter.prm` | standard file importer | `xImportEntry` | Registers `.osv` and `.lrf`; decodes both lenses with FFmpeg, stitches with the CUDA / OpenCL / CPU renderer and hands Premiere an equirectangular 360 x 180 frame in `BGRA_4444_32f`, colour-tagged Rec.2100 PQ, HLG or Rec.709. Decodes the AAC track. Declares the clip as monoscopic equirectangular VR. Per-clip options reach it as a `PrefsBlob`, from either the Source Settings effect or the modal Source Settings dialog. |
| `OpenOSVSourceSettings.aex` | AE API **source settings** effect | `EffectMain` | "OpenOSV Source Settings", attached by Premiere to the **master clip** automatically. Twenty-two controls - colour output and its Rec.709 look, output size, stabilisation, seam search, exposure match, calibration, sun ghost removal, the sky seam fix (mode, strength, edge inset), the seam tools (Seam Blend, Parallax Blend, Seam Smoothing, Near / Far Offset), the lens shading correction (mode, strength), D-Log M curve, exposure, render device, Program Monitor colour - visible in the Effect Controls panel instead of behind a dialog. Renders nothing; its values reach the importer as a flat prefs blob, so they **cannot be keyframed**. See "Source Settings effect" below. |
| `Open360Reframe.aex` | After Effects API effect + `PrGPUFilter` | `EffectMain`, `xGPUFilterEntry` | "Open 360 Reframe" in the Effects panel (bin "OpenOSV"). Host-keyframed Pan / Tilt / Roll / FOV / Distortion plus preset perspectives. Renders on the GPU through Premiere's own CUDA device (driver API, embedded fatbin) and falls back to a 32-bit float CPU path that runs the same kernel. |

Install all three (plus their runtime DLLs) in
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\`; Premiere Pro,
Media Encoder and After Effects all scan that folder.

The repository also ships three **sequence presets** (`presets/`), installed
per user so `File > New > Sequence > OpenOSV` gives a correct 59.94 fps
timeline in one click. See "Sequence presets" below.

Plug-ins never see the timeline. Putting Open 360 Reframe on every `.OSV`
dropped into a sequence is done by the **OpenOSV companion panel**
(`panel/`, UXP and CEP builds), installed per user by the same script.
`docs/PANEL.md` covers it.

## Compatibility rule

Target Premiere Pro 2026 (26.x, SDK 26.0) but only use API surfaces that exist
since Premiere Pro 2022 (22.0):

* Importer: `IMPORTMOD_VERSION_23` behaviour (14.1) is the floor. Everything
  used below (colour-space enumeration 13.0, colour-managed PPix creation 14.0,
  `imGetSourceVideo`, VR fields CC 2017) predates 22.0.
  `imSelectClipFrameDescriptor2` (23.2) is only answered when
  `imStdParms::imInterfaceVer >= IMPORTMOD_VERSION_24`.
* Effect: AE effect spec 13.x, `PrSDKGPUFilterInterfaceVersion2`, GPU Device
  Suite v2, Video Segment Suite v4 (acquire v4 explicitly, it has `GetParam`
  and `GetNodeProperty`), Sequence Info Suite: v9 preferred with a
  9 -> 8 -> 7 -> 6 -> 5 fallback, any of which answers `GetFrameRect` for
  "Match Sequence" (VR configuration).
* Suites are acquired with explicit version numbers and a NULL result is a
  graceful downgrade, never a crash.
* No DirectX path (still gated by Adobe), no OpenCL path in the effect
  (Premiere dropped OpenCL rendering in 15.4). The importer keeps OpenCL as its
  vendor-neutral stitch fallback because it owns its own context.

## Build

```
cmake --preset windows-msvc-premiere-release
cmake --build --preset windows-msvc-premiere-release
ctest --preset premiere
scripts\install_plugins.ps1            # copies into MediaCore\OpenOSV (needs admin)
```

Cache variables:

| Variable | Meaning |
|---|---|
| `OSV_BUILD_PREMIERE=ON` | build `plugins/` |
| `OSV_PREMIERE_SDK_DIR` | Premiere Pro C++ SDK root (contains `Examples/Headers`) |
| `OSV_AE_SDK_DIR` | After Effects C++ SDK root (contains `Examples/Headers`, `Examples/Resources/PiPLtool.exe`, `Examples/Util`) |
| `OSV_PLUGIN_STAGE_DIR` | where the built plug-ins and their DLLs are staged (default `<build>/plugins/OpenOSV`) |

The presets point the two SDK variables at repo-relative, git-ignored folders
(`Premiere Pro 26.0 C++ SDK/` and `third_party/ae-sdk/`); override them with
`-D` when the SDKs live elsewhere. Adobe's SDK licence forbids redistribution,
so the repository contains no SDK file and CI only builds the plug-ins when a
secret provides the SDK archives.

Compiler settings that Adobe's samples rely on and that the plug-in targets
reproduce: `/MD` (never a static CRT, it exhausts fiber-local storage slots),
x64, `PRWIN_ENV` + `MSWindows` + `_WINDOWS` defines, `/SUBSYSTEM:WINDOWS`,
`SUFFIX ".prm"` / `".aex"`, no import-library prefix. Every SDK header is
`#pragma pack(push, 1)`; plug-in code never redefines those structs.

## Layout

```
plugins/
  CMakeLists.txt              options OSV_PREMIERE_IMPORTER / OSV_PREMIERE_REFRAME /
                              OSV_PREMIERE_SOURCE_SETTINGS (default ON), adds the four below
  common/                     osv_premiere_common (static)
    HostSuites.h/.cpp         RAII acquire/release of SweetPea suites by name+version
    PluginLog.h/.cpp          file log (%LOCALAPPDATA%\OpenOSV\<plugin>.log) + OutputDebugString
    DelayLoad.h/.cpp          delay-load hook: resolve avcodec/avformat/... /OpenCL.dll from the plug-in's own folder
    PixelCopy.h/.cpp          RGBA float top-down  ->  BGRA 32f / 16u / 8u bottom-up with row bytes (SIMD friendly
                              loops), whole frames or streamed bands (rgbaRowsToHost, packedRowsToHost)
    HostContext.h/.cpp        process-wide lazily created renderer pool + thread pool, torn down explicitly
    PrefsBlob.h               the importer prefs struct (shared with the dialog AND the source settings effect)
    UserDefaults.h/.cpp       the per-user defaults new clips start from (%APPDATA%\OpenOSV\defaults.json):
                              named-key JSON, atomic writes, per-module mtime cache; SDK-free, so osvtool
                              compiles it too (see "User defaults for new clips")
    SourceSettingsIdentity.h  the ONE spelling of the source settings effect's match name, included by
                              the effect's .r/.cpp and by the importer - the binding is a string compare
                              with no diagnostic, so it must not exist twice
  importer/                   OpenOSVImporter.prm
    ImporterEntry.cpp         xImportEntry dispatch, DllMain, imInit/imShutdown, open/quiet/close, privateData handle
    ImporterInstance.h/.cpp   per-clip state (privateData): reader, rig, colour, stabilisation, frame + analysis caches, mutex
    ImporterVideo.cpp         imGetInfo8/9, pixel formats, frame sizes, clip frame descriptors, colour spaces, imGetSourceVideo, imAnalysis
    ImporterGpuFrame.h/.cpp   the importer's own GPU frame path: pinned banded readback (GpuReadback), the lock
                              around the shared CUDA renderer's output, OPENOSV_IMPORTER_NO_GPU_DECODE
    ImporterPackKernel.h/.cu  16u / 8u packing on the GPU before the readback (own static library: nvcc never
                              sees the plug-in's MSVC options)
    ImporterAudio.h/.cpp      AudioDecoder: its own AVFormatContext, AAC -> planar float, exact random + sequential positioning
    ImporterAudioSelectors.cpp  imImportAudio7 / imResetSequentialAudio / imGetSequentialAudio / imGetAudioChannelLayout
    PrefsMapping.cpp          the PURE PrefsBlob <-> dialog-control mapping and the colour-space tokens (compiled into the tests too)
    SourceSettingsDialog.cpp  Win32 modal dialog shown from imGetPrefs8 / imGetInstancePrefs
    ImporterPlugin.h          identity constants, privateData accessors, handler declarations
    OpenOSVImporter.rc        IMPT resource + DIALOGEX template + VERSIONINFO
    resource.h                resource and control IDs shared by the .rc and the dialog code
  sourcesettings/             OpenOSVSourceSettings.aex
    OpenOSVSourceSettings.r   PiPL (AE kind); includes SourceSettingsParams.h, which includes
                              SourceSettingsIdentity.h (reached through osv_add_pipl's new INCLUDES argument)
    OpenOSVSourceSettings.rc  #include the generated .rcp + VERSIONINFO
    SourceSettingsParams.h    out-flags, parameter ids/indices, popup item strings, ranges, defaults
                              (single source of truth; the C++ half is hidden from the .r preprocessor pass)
    SourceSettingsMapping.h/.cpp  the PURE ControlValues <-> PrefsBlob mapping (1-based AE popups vs
                              0-based blob enums), compiled into the tests too
    SourceSettingsMain.cpp    AE entry: ABOUT, GLOBAL_SETUP (SetIsSourceSettingsEffect), PARAMS_SETUP,
                              SEQUENCE_SETUP (PerformSourceSettingsCommand), TRANSLATE_PARAMS_TO_PREFS
    DllMain.cpp               arms the delay-load hook; nothing else (loader lock)
  reframe/                    Open360Reframe.aex
    Open360Reframe.r          PiPL (AE kind) -> cl /EP -> PiPLtool -> cl /EP -> .rcp; includes ReframeParams.h
    Open360Reframe.rc         #include "Open360Reframe.rcp" + VERSIONINFO
    ReframeParams.h           identity, out-flags, parameter IDs, ranges, defaults, aspect and preset tables
                              (single source of truth; the C++ half is hidden from the .r preprocessor pass)
    EffectMain.cpp            AE entry: ABOUT, GLOBAL_SETUP, PARAMS_SETUP, USER_CHANGED_PARAM,
                              UPDATE_PARAMS_UI, SEQUENCE_* (sequence_data stays null), RENDER (CPU)
    GpuFilter.cpp             xGPUFilterEntry: own module glue (no PrGPUFilterModule.h - it needs Boost),
                              CUDA driver API only, host context pushed around every call, per-device
                              fatbin module cache released at shutdown
    ReframeKernel.cu          thin CUDA wrapper around osvReframeEquirectPixel; nvcc -fatbin, embedded
                              with cmake/EmbedKernel.cmake, loaded with cuModuleLoadFatBinary
    ReframeCpu.h/.cpp         the geometry builder (buildParams) and the CPU pixel loop over the same
                              kernel on the library ThreadPool; owns PixelLayout (Bgra32f / 16f / 8u /
                              16u), kBgra16uWhite = 32768 and promoteIntegerToFloat; also compiled
                              into osv_reframe_tests
    DllMain.cpp               arms the delay-load hook; nothing else (loader lock)
tests/premiere/               mock-host harness (see Verification); added from plugins/CMakeLists.txt because every target needs the SDK targets
  mockhost/                   osv_premiere_mockhost (static, Catch2-free): fake piSuites + SPBasicSuite serving our own suite implementations
    MockHost.h/.cpp           public API, suite registry, legacy memory callbacks, inspection
    MockHostImpl.h            internal state shared by the suite files
    MockPPix.cpp              PPix v1, PPix2 v3, Creator v1, Creator2 v4, Cache v8/v7 (real LRU)
    MockSuites.cpp            Time, String, App Info, Error, Color Management, Memory Manager, Importer File Manager, Sequence Info v5..v9, Video Segment v6..v9
    MockGpu.cpp               GPU Device Suite v2 on a real CUDA driver-API context (stub without CUDA)
    MockAe.cpp                PF Pixel Format v1, PF Utility v4..v13, PF Source Settings v1/v2,
                              PF_InData/PF_OutData builders, effect worlds in BGRA 32f /
                              32f_Linear / 16u / 8u and ARGB 8u (DEEP set from the sample
                              width, not a format list), plus setWorldFormat() to relabel a
                              world's format without touching its bytes so a test can prove
                              an unknown format is refused rather than misread
  common/                     osv_premiere_common_tests (mock host + plugins/common)
  importer/                   osv_importer_tests: LoadLibraryW on the built .prm, driven through the mock host
    ImporterHarness.h/.cpp    loads the module, plays host (imInit/imShutdown, ClipHandle RAII, request builders)
    test_importer.cpp         registration, open/quiet/close, imGetInfo8/9, formats, sizes, colour, frames, audio, prefs, timing
    test_importer_bitdepth.cpp  bit-depth negotiation per signal, 16u delivery, GPU frame path == host path, shared
                              renderer under concurrency, direct path changes nothing, Rec.709 override stays local
    test_prefs_mapping.cpp    the pure prefs <-> controls mapping (links plugins/importer/PrefsMapping.cpp directly)
  reframe/                    osv_reframe_tests (loads Open360Reframe.aex)
  sourcesettings/             osv_source_settings_tests (loads OpenOSVSourceSettings.aex)
    SourceSettingsTestSupport.h/.cpp  LoadedPlugin, an EffectFixture that runs GLOBAL_SETUP then
                              PARAMS_SETUP in the host's own order, and the PiPL reader
    test_params.cpp           module, GLOBAL_SETUP (incl. the source-settings declaration), PARAMS_SETUP
    test_prefs.cpp            TRANSLATE_PARAMS_TO_PREFS round trips, SEQUENCE_SETUP seeding, the mapping
    test_pipl.cpp             the PiPL, and its match name against the importer's constant
    test_defaults.cpp         the Defaults group's buttons, new-clip seeding from the user defaults
presets/                      three .sqpreset sequence presets + README.md recording the verified schema
panel/                        the OpenOSV companion panel: auto-applies Open 360 Reframe to OSV clips
                              (shared/ + uxp/ + cep/ + tests/; see docs/PANEL.md)
scripts/install_plugins.ps1   copy stage dir -> MediaCore\OpenOSV AND presets/ -> the user's
                              SequencePresets\OpenOSV (-StageDir, -Destination, -Uninstall, -NoElevate,
                              -Force, -NoPresets, -PresetDir, -PresetDestination), self-elevating,
                              prints the Plugin Loading.log path and the Shift-launch hint; the panel
                              per user (-NoPanel, -PanelOnly, -PanelFlavor, -PanelDestination)
```

Everything that touches Premiere lives under `plugins/`; the library under
`include/`/`src/` stays host-agnostic and keeps building without any SDK.

## Shared kernel additions (library)

`include/osv/render/osv_kernel.h` gains a second, independent entry point so
the effect and the importer's own tests use the exact same math on CPU and
GPU:

```c
/* Eye-offset ("generalised stereographic") projection. d = 0 is rectilinear,
   d = 1 is stereographic; r/f = (1 + d) sin(theta) / (d + cos(theta)). */
#define OSV_PROJ_EYE_OFFSET 3

typedef struct OsvRgbaSource {
    int w, h;            /* equirect size */
    int pitchBytes;      /* row pitch (positive) */
    int isHalf;          /* 1 = 16-bit float BGRA, 0 = 32-bit float */
    int isBgra;          /* 1 = channel order B,G,R,A (Premiere), 0 = R,G,B,A */
} OsvRgbaSource;

typedef struct OsvReframeParams {
    int outW, outH;              /* full output frame */
    int viewX, viewY, viewW, viewH; /* rectangle that receives the picture; outside = transparent black */
    int projection;              /* OSV_PROJ_* (EYE_OFFSET recommended) */
    float eyeOffset;             /* d in [0, 1] */
    float focalPx;               /* for the viewport width */
    float tanHalfH, tanHalfV;    /* rectilinear helpers */
    float Rout[9];               /* body <- view rotation (source orientation * camera) */
    int fillAlphaOne;            /* 1 = opaque output inside the viewport */
} OsvReframeParams;

OSV_FN void osvReframeEquirectPixel(const OsvReframeParams* p, const OsvRgbaSource* src,
                                    const void* pixels, int px, int py, float out[4]);
```

The equirect sampler is bilinear with longitude wrap-around and latitude
clamp, works in whatever colour space the pixels are in (the effect is colour
agnostic), and returns straight RGBA. The *source* side conversions (16f ->
float through a bit-manipulation decoder `osvHalfToFloat`, BGRA -> RGBA
channel swap) live inside the shared function, driven by
`OsvRgbaSource::isHalf` / `isBgra`, so a device buffer never needs a
converted copy; only the *output* side conversions (float -> 16f / 8u, RGBA ->
BGRA writes) are done by the GPU and CPU wrappers. Everything stays C99.
`osvViewRay` is the ray generator shared by both entry points and
`osvEyeOffsetTheta` the eye-offset inverse; the effect fills `focalPx` from
`geom::VirtualCamera::focalPx()` with `w = viewW`, `h = viewH`. The existing
fisheye pipeline (`OsvRenderParams`) gets `OSV_PROJ_EYE_OFFSET` and an
`eyeOffset` field so `osvtool render --proj eye-offset --distortion 0.5`
produces identical framing to the effect. `render::cudaReframeEquirect`
(CudaRenderer.h) runs the device build of the function on a host frame for
verification. `geom::Projection::EyeOffset` is appended after `Equirect` so
existing enumerator values are unchanged.

`geom::VirtualCamera` gains `Projection::EyeOffset` + `double eyeOffset`;
`geom::kPresets` gains an `eyeOffset` column (Crystal Ball 1.0, Asteroid 1.0,
Wide 0.15, Ultra Wide 0.4, Dewarping 0.0) and switches those presets to the
eye-offset projection (stereographic == eye offset 1.0, rectilinear == 0.0, so
existing looks are unchanged). Parity tests cover the new projection across
CPU / CUDA / OpenCL exactly like the fisheye path.

## Importer design

### Registration and lifetime

* `imInit`: `hasSetup = kPrTrue` (Source Settings dialog), `canProvidePeakAudio = kPrFalse`,
  `avoidAudioConform = kPrFalse` (AAC is compressed, let the host conform),
  `priority = 0`, `keepLoaded = kPrFalse`. Returns `imIsCacheable`; nothing
  hardware-specific happens in `imInit`, the GPU probe is lazy (first frame).
* `imGetIndFormat` index 0: filetype `'OSV_'`, `FormatName "DJI Osmo 360 (OpenOSV)"`,
  `FormatShortName "OSV"`, `PlatformExtension "osv\0lrf\0\0"`, `flags = xfCanOpen | xfCanImport | xfIsMovie`.
  Index 1 -> `imBadFormatIndex`.
* `imGetSupports7/8` -> `malSupports7/8`. `imGetSupportsPerInstancePrefs` -> `malSupportsPerInstancePrefs`.
* `imOpenFile8`: `CreateFileW(GENERIC_READ, FILE_SHARE_READ)`; sniff the ISO
  header with `osv::OsvFile` (fails -> close and `imBadFile`); allocate
  privateData (host `newHandle`) holding a pointer to an `ImporterInstance`.
* `imQuietFile`: close the handle and release renderer references and device
  memory, keep parsed metadata. The decoders are PARKED rather than destroyed,
  so the unquiet - or the new instance Premiere opens after a Source Settings
  change - takes them back warm:
  * the host path's dual reader in `osv::video::ReaderPool` (instead of
    ~200-400 ms of hardware device creation and a frame-0 decode);
  * the GPU frame path's NVDEC `GpuClipDecoder` in
    `osv::video::GpuDecoderPool`, trimmed to 4 cached frames while parked and
    released when free VRAM drops under 2 GiB (unquiet ~100 ms -> ~14 ms).
  Both pools hold at most 2 idle items for 60 s on the shared `IdlePool`
  core. The effect's direct-path decoders live in Premiere's own CUDA
  context and are never pooled.
* `imCloseFile`: delete the instance (parking its reader the same way),
  dispose privateData, release suites.
* `imShutdown`: release the direct-GPU engine, clear the reader pool, then
  destroy the process-wide `HostContext` (renderers, FFmpeg hardware
  contexts, thread pool). Never from `DllMain`.

### Information (`imGetInfo8`, mirrored by `imGetInfo9`)

* `hasVideo = 1`, `imageWidth/Height` = output equirect size from prefs
  (Native = 2 x decoded height, e.g. 6000 x 3000 for 6K; 4K = 3840 x 1920;
  2K = 1920 x 960), `pixelAspect 1:1`, `fieldType prFieldsNone`,
  `alphaType alphaStraight` (NOT opaque: the calibration's occlusion polygon
  leaves the directions hidden by the camera body fully transparent, and
  declaring opaque would let the host ignore the alpha channel and composite
  those pixels as black), `depth 32`, `subType 'hvc1'`, `bitDepth 10`,
  `codecDescription "DJI Osmo 360 dual fisheye HEVC, stitched by OpenOSV"`,
  `supportsGetSourceVideo = 1`, `supportsAsyncIO = 0` (synchronous path first;
  Premiere prefetches on its own threads), `colorSpaceSupport = imColorSpaceSupport_Fixed`,
  `frameRate` = ticks per frame from the container rational (`ticksPerSecond * num / den`,
  exact for 60000/1001), `vidDurationInFrames` = frame count,
  `vidScale/vidSampleSize` also filled from the rational for old hosts.
* VR: `ivProjectionType = kPrIVProjectionType_Equirectangular`,
  `ivFrameLayout = kPrIVFrameLayout_Monoscopic`, `ivHorizontalCapturedView = 360`,
  `ivVerticalCapturedView = 180`.
* Audio: `hasAudio` when the AAC track exists, `numChannels` (1/2/6 else
  `imGetAudioChannelLayout`), `sampleRate`, `sampleType kPrAudioSampleType_Compressed`,
  `audDuration` in sample frames, `accessModes = kSeparateSequentialAudio`
  (random access for scrubbing through `imImportAudio7`, conform through
  `imResetSequentialAudio` / `imGetSequentialAudio`).
* `streamsAsComp = 0`, `mayBeGrowing = 0`, `filePath` filled.
* `imGetTimeInfo8` returns `imNoTimecode`: DJI writes no `tmcd` track, and the
  container's creation time is a date rather than a start timecode, so
  synthesising one would put a wrong number in the Media Start column.
  `imGetFileAttributes` returns `imUnsupported` for the same reason - the
  record carries no privateData, so the only date available is the file
  system's, which is exactly what the host shows without an importer.
  `imAnalysis` is a short text block (camera model, mode, output size and
  colour, frame rate, lens accessory, calibration slots, stabilisation, seam /
  gain settings, audio, render backend) with CR/LF line endings.

### Pixel formats, sizes, colour

* The importer produces `BGRA_4444_32f`, `BGRA_4444_16u` (0..32768, the SDK's
  16-bit white) and `BGRA_4444_8u` for any clip, but OFFERS them per clip,
  by the signal its Source Settings produce (a 10-bit HDR picture must not be
  offered 8 bits - 8-bit PQ bands in every sky):

  | Colour output | `imGetIndPixelFormat` (preference order) | `imSelectClipFrameDescriptor(2)` |
  |---|---|---|
  | Rec.709 (SDR) | 32f, 8u | Maximum Bit Depth off -> 8u; else the host's wish if we make it, else 32f |
  | PQ / HLG (HDR, clamped to [0, 1]) | 32f, 16u | Maximum Bit Depth off -> **16u, never 8u**; else 32f / 16u as wished, else 32f |
  | D-Log M passthrough (log, can exceed [0, 1]) | 32f | always 32f |

  SDK basis: "Pixel formats should be returned in the preferred order" and
  "the host will attempt to always talk to the importer in the preferred
  pixel format if possible" (the record carries `prefs`, and the host
  enumerates again when Source Settings change); imSelectClipFrameDescriptor
  exists to "change pixel formats based on ... source settings, such as HDR";
  "For high-bit depth support, the 32f formats are the recommended route"
  (so 32f leads every list).  The 8u that Premiere 26.2.2 asked for on every
  frame came from this importer's own descriptor answer: it said 8u whenever
  Maximum Bit Depth was off, which is the sequence default.  Each distinct
  negotiation is now logged once (`imSelectClipFrameDescriptor2: host wants
  ..., Maximum Bit Depth ... -> answering ...`) so a real session shows what
  the host asked for.
* `imGetPreferredFrameSize`: index 0 native, 1 half, 2 quarter (`imIterateFrameSizes`), then `imOtherErr`.
* `imSelectClipFrameDescriptor(2)`: return the desired descriptor unchanged
  except the pixel format (table above) and the size, snapped to the nearest
  advertised size.
* `imGetIndColorSpace` index 0: `kPrSDKColorSpaceType_Predefined` with
  `ioProfileRec.outName` (String Suite) = `kPrOverranged2100PQ` ("BT.2100 PQ RGB Full"),
  `kPrOverranged2100HLG`, `kPrOverranged709` or `kPrOverranged2020Scene`
  according to prefs; index 1 -> `imBadFormatIndex`.
  The first three tokens describe exactly what we emit: full-range 32f RGB
  signal codes. The fourth is the D-Log M passthrough output, for which the
  SDK has no token at all, so it is declared approximately and logged as
  such - see "D-Log M passthrough" under the Source Settings effect for the
  full reasoning and for what is deliberately *not* returned.
  A compile-time switch (`OSV_IMPORTER_COLOR_SEI`) answers with
  `kPrSDKColorSpaceType_SEITags` `{9, 16|18|1|2, 0, 32, full, rgb, display}`
  instead, for the runtime comparison on real hosts.
* `imGetColorSpaceFromOpaqueData` (23.3, undocumented) and every unknown
  selector return `imUnsupported`, logged once.
* `imGetSourceVideo`: walk the requested formats in the host's order and take
  the first we produce (`PrPixelFormat_Any` -> the clip's preferred 32f; an
  explicit `BGRA_4444_8u` is ALWAYS honoured - "all importers must support
  BGRA pixel format as well" - and logged once when the clip is HDR or log;
  16u for the unbounded log output is served as the lossless 32f); nothing
  usable -> 32f at native size.  Look the frame up in the PPix cache
  (`GetFrameFromCacheWithColorSpace`, key = importerID / stream / frame /
  quality / prefs blob), else create the PPix with
  `PPixCreator2Suite::CreateColorManagedPPix(..., opaqueColorSpaceIdentifier)`
  (plain `CreatePPix` when the id is invalid), render straight into it
  (`ImporterInstance::renderFrameToHost`, see "The importer's own frame"
  below), `AddFrameToCacheWithColorSpace`.
  If `selectedColorProfileName == kPrOverranged709` the host could not use the
  declared space: render THIS request with the Rec.709 transfer (a
  request-local `outputTransfer`; the clip's prefs, and so what the direct-path
  engine renders, are untouched).  The cache key blob says Rec.709 for such a
  frame, so it is never served to an ordinary request.
  Draft: `inQuality <= kPrRenderQuality_Low` or a scrubbing/playing intent with
  `inPlaybackRatio < 1` disables seam search for that frame.
* Frame index = `inFrameTime / ticksPerFrame` with rounding, clamped.

### The importer's own frame

Premiere requests the importer's equirect for every clip on the timeline -
also when the reframe effect renders its view straight from the fisheyes
(docs/DIRECT_GPU.md) and ignores that input.  So the importer's frame has to
be cheap, and it has to stay full quality (next paragraph).  Two paths
produce it, both from one parameter block (`ImporterInstance::buildEquirectJob`,
the builder chain renderFrame always had):

* **GPU path** (`renderFrameToHost` -> `renderFrameOnGpu`): both lenses
  decoded by `video::GpuClipDecoder` on NVDEC into VRAM (GOP-aware cache,
  decode-ahead) in the PRIMARY context of the shared CUDA renderer's device -
  the context the CUDA runtime, and so `CudaRenderer`, runs in - so the
  stitch reads the decoded planes in place (`CudaRenderer::renderToDevice`,
  zero upload).  16u / 8u frames are packed on the GPU
  (`ImporterPackKernel.cu`, same codes as PixelCopy bit for bit), then
  `GpuReadback` streams the frame through two 32 MiB pinned bands and copies
  each band into the PPix (with the row flip) while the next one crosses the
  bus: one DMA and one host pass, no float image in host memory.  The
  decoder lives in `m_gpuDecoders` under the primary context, so imQuietFile
  frees it with the direct path's decoders.  Every use of the shared
  renderer's output (this path's readback and the host path's `renderInto`)
  holds `cudaRendererOutputMutex()`: the buffer is shared by all clips.
* **Host path** (`renderFrame` + PixelCopy): D3D11VA / NVDEC-copy / software
  decode to host memory, upload, stitch, readback, float image, convert -
  the path before the GPU one, kept as the fallback for everything the GPU
  path does not take: CPU / OpenCL render device, no NVIDIA GPU, a stream
  NVDEC refuses (the LRF proxy), `OPENOSV_IMPORTER_NO_GPU_DECODE=1` (escape
  hatch; the tests use it to compare the paths), three GPU failures in a row.

With the analyses off the two paths are byte-identical (32f, 16u and 8u,
tested at 6000 x 3000).  With one on they differ by that analysis' float
noise only: the analyses shade their bands on the GPU from device frames and
on the CPU from host frames (108-111 dB apart), so a gain is a hair apart
(8e-6), a seam table a hair apart moves every column by a hair (8e-5), and
the parallax grid can gate a flow cell differently (single pixels up to 0.09
inside the overlap, band mean 1.4e-4, exactly zero outside it).  The GPU
path's analyses are the ones the effect's direct path computes, so the
importer's equirect and the direct view now agree with each other.

**Why the frame is never degraded when the direct path is active.**  The
engine knows when the effect renders a clip directly (`engineDirectPathActive`,
`engineDirectFrameCount` in Engine.h; the Properties panel says so).  But
`imSourceVideoRec` names no consumer - no sequence, no track item, no effect -
and the PPix cache key is importer id / stream / frame / format / quality /
prefs / colour space.  The same master clip is the Source Monitor's picture,
every sequence's use of it without the effect, the effect's own fallback
whenever the direct path refuses a frame (`DirectLaunchReject`, device loss),
and a bypassed effect's output; all of those send the identical request and
hit the identical cache entry.  A cheaper frame served "because the effect
ignores it" would be cached and shown there.  So the signal is diagnostics
only, and the importer's frame is made cheap instead (tested: the frame is
byte-identical before and after the engine served the clip).

Measured (RTX 5090, sample clip, `osv_importer_bench --part F`, native
6000 x 3000, analyses off, importer time per frame from its `frame-cost`
debug lines - i.e. without the mock host's zero-filled PPix allocation -
while nine other agents were building, so ratios measured in one process are
the trustworthy part):

| Path | Format | Park (scattered, Stopped) | Play (sequential) | Stages while playing |
|---|---|---|---|---|
| GPU | 32f | 47 ms | 13.6 ms | render 1.4, readback 12.5 |
| GPU | 16u | 41 ms | 8.8 ms | render 1.4, readback 7.4 |
| GPU | 8u | 39 ms | 5.4 ms | render 1.4, readback 4.0 |
| host | 32f | 126 ms | 79 ms | decode+upload+stitch+readback 73, convert 9 |
| host | 16u | 143 ms | 84 ms | 78, 7.5 |
| host | 8u | 142 ms | 80 ms | 76, 6 |

Parking is NVDEC catching up inside the GOP (docs/DIRECT_GPU.md, WP-A:
40-48 ms); playing is the readback.  16u costs 35 % less than 32f on the GPU
path and carries the clamped PQ / HLG signal 32x finer than the 10-bit
source.

### Audio (`imImportAudio7`, `imResetSequentialAudio`, `imGetSequentialAudio`)

A second `AVFormatContext` on the same file (its own OS handle) decodes AAC
to planar float with `swresample`; sequential requests continue from the
conform cursor, random requests seek to the containing packet and discard
priming samples. Output buffers are zero-filled past the end of stream.
Sample positions are in the audio sample rate. Audio and video decoders never
share a demuxer, so conforming cannot disturb video seeks.

### Prefs and Source Settings dialog

`PrefsBlob` (flat, `#pragma pack(1)`, 128 bytes, versioned):

| Field | Values | Default |
|---|---|---|
| `magic` `'OOSV'`, `version` 1 | | |
| `colorOutput` | 0 PQ, 1 HLG, 2 Rec.709, 3 D-Log M passthrough (appended; never renumber) | 0 |
| `outputSize` | 0 native, 1 4K (3840x1920), 2 2K (1920x960) | 0 |
| `stabilization` | 0 off, 1 horizon lock, 2 full, 3 smooth | 1 |
| `seamSearch` | 0/1 | 1 |
| `gainMatch` | 0/1 | 1 |
| `calibration` | 0 native, 1 lens guards, 2 underwater | 0 |
| `dlogmFit` | 0 DJI refit, 1 Pocket 3 | 0 |
| `exposureStops` | float | 0 |
| `renderDevice` | 0 auto, 1 CPU, 2 CUDA, 3 OpenCL | 0 |
| `reserved[..]` | zero | |

`imGetPrefs8` first call sets `prefsLength = sizeof(PrefsBlob)`; later calls
start from the user defaults (see "User defaults for new clips") when the
buffer holds no blob of ours - a `firstTime` call, zeros, another importer's
bytes - and show the modal Win32 dialog (`DialogBoxParamW` on a DIALOGEX
template in the `.rc`, owner = host main window). Cancel returns `imCancel`.
The dialog's "Save as De&fault" button stores what it shows as the user
defaults without closing it. The blob is part of every PPix cache key so
changed settings never hit stale frames. `imGetInstancePrefs` mirrors it.

### Concurrency

One `std::mutex` per `ImporterInstance` serialises decode + render; frames
for different clips run in parallel. Renderers come from the process-wide
`HostContext` (one CUDA renderer per device, one OpenCL renderer, one CPU
renderer) and are internally serialised. No mutable globals besides that
context. FFmpeg runtime DLLs and `OpenCL.dll` are delay-loaded from the
plug-in folder so a machine without an OpenCL ICD still loads the importer.

## Source Settings effect design

`OpenOSVSourceSettings.aex` exists for one reason: the stitch options were
only reachable through a modal Win32 dialog, which users have to know to go
looking for. The SDK's intended mechanism for per-clip decode options is a
*source settings effect* (`prsdk26.txt` lines 365-400,
`PrSDKAESupport.h:1620-1637`): Premiere attaches it to the **master clip**
automatically and its parameters appear in the Effect Controls panel.

### What it deliberately cannot do, and why

**It is not, and cannot be, the reframe effect.** Two SDK facts make that
structural rather than a design preference:

1. **A source settings effect is never sent `PF_Cmd_RENDER`.** It sits on the
   master clip, describing how the media should be *decoded*; there is no
   frame passing through it to filter. A render handler here would be dead
   code, which is why there is not one (and why no colour-awareness out-flag
   is set: those describe `PF_Cmd_RENDER` behaviour, and claiming a pixel
   capability for code that does not exist would be a lie the host acts on).
2. **Its values reach the importer only as one flat byte blob.**
   `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS` hands the effect a
   `PF_TranslateParamsToPrefsExtra` whose `prefsPC` is a host-owned buffer
   (`AE_Effect.h:2177-2190`) that the effect fills - our 128-byte `PrefsBlob`.
   **A blob has no time axis.** The host asks for it once for the whole clip,
   so a keyframe has literally nowhere to be stored and nowhere to be read
   back from.

Therefore every parameter here is static per clip, and every one carries
`PF_ParamFlag_CANNOT_TIME_VARY` so the panel shows **no stopwatch**. That is
honest UI: a stopwatch on a control whose keyframes can never reach the
decoder is a control that silently does nothing, and "why did my keyframes do
nothing?" is a far worse bug than "why can I not keyframe this?".

**Pan / Tilt / Roll / FOV therefore stay in `Open360Reframe.aex`**, which is
an ordinary timeline effect: it does receive `PF_Cmd_RENDER` at a specific
time and reads its parameters per frame, which is exactly what keyframed
reframing needs. The division is:

| Belongs in Source Settings | Belongs in Open 360 Reframe |
|---|---|
| How the sphere is built (stitch, stabilisation, calibration, seam, gain) | Where the camera looks (Pan, Tilt, Roll) |
| How the sphere is coloured (PQ / HLG / 709, D-Log M curve, exposure) | How wide it looks (FOV, Distortion, Preset) |
| How big the sphere is (output size) | What shape the output is (Output Aspect) |
| Which device stitches it (render device) | Smooth Keyframes |
| **Static per clip** | **Keyframeable per frame** |

It is also a **separate module** rather than a second PiPL in the reframe
`.aex`, because the AE SDK lists "multiple PiPLs in a single plug-in" among
the features Premiere does not support. A test asserts resource 16001 is
absent, so that fact cannot be forgotten.

### Identity

* Display name "OpenOSV Source Settings", category "OpenOSV", match name
  `OpenOSV.SourceSettings` (never changes), version 1.0.0.
* `out_flags = PF_OutFlag_SEND_UPDATE_PARAMS_UI` (`0x04000000`) only.
  `out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_SUPPORTS_THREADED_RENDERING`
  (`0x08000008`). `SourceSettingsParams.h` carries both as decimal literals so
  the `.r` can paste them into the resource (PiPLtool's parser rejects
  anything more structured than a number), and `SourceSettingsMain.cpp`
  `static_assert`s each against the real `AE_Effect.h` macro.
* **The match name is the entire interface.** Premiere pairs importer and
  effect by comparing `imFileInfoRec8::sourceSettingsMatchName`
  (`PrSDKImport.h:425`) to the PiPL's `AE_Effect_Match_Name`, with no
  handshake and no diagnostic on a mismatch: one mistyped character and the
  panel simply never shows the options, with nothing in any log to say why.
  So the string lives once, in `plugins/common/SourceSettingsIdentity.h`, and
  is read by the effect's `.r`, the effect's `.cpp` and the importer. A test
  reads the resource back out of the built module and compares it to that
  header; another reads it out of a live `imGetInfo8`.

### Parameters (IDs are permanent; the INDEX is not the ID)

As in the reframe effect, `PF_ADD_TOPIC` and `PF_END_TOPIC` each issue their
own `PF_ADD_PARAM`, so a group occupies two real parameter slots and the
`GROUP_END` slot sits in the MIDDLE of the list. There are 33 parameters: 25
value controls, 2 buttons and 6 group markers. `SourceSettingsParams.h`
spells the index table out literally. Ids are permanent and only ever
appended; indices moved when a control joined a group (the ids did not).

Popup items are in `PrefsBlob` enum order - AE popup values are **1-based**
while the blob's enums are **0-based**, so each conversion is a deliberate -1
/ +1 in `SourceSettingsMapping.cpp` - with ONE exception: Calibration goes
through `kCalibrationChoiceByPopup`. Its first three items sit where the
original "Native | Lens Guards | Underwater" items did, and that first item
never forced anything (calibration 0 always followed the recorded accessory),
so it is labelled for what it does, Auto, and a saved project keeps its
meaning; forcing the bare-lens set is new and therefore last.

| Index | ID | Name | Type | Items / range | Default | Prefs field |
|---|---|---|---|---|---|---|
| 1 | 1 | Colour Output | popup | BT.2100 PQ \| BT.2100 HLG \| Rec. 709 \| D-Log M (no transform) | PQ | `colorOutput` |
| 2 | 15 | Look (Rec. 709 only) | popup | DJI (default) \| OpenOSV standard | DJI | `look` |
| 3 | 46 | HDR Peak (PQ only) | popup | 1000 nits (default) \| 600 nits \| 400 nits \| 203 nits (SDR-safe) | 1000 nits | `hdrPeak` |
| 4 | 2 | Output Size | popup | Native (2 x decoded height) \| 4K (3840 x 1920) \| 2560 x 1280 \| 2K (1920 x 960) | Native | `outputSize` |
| 5 | 3 | Stabilisation | popup | Off \| Horizon Lock \| Full \| Smooth | Horizon Lock | `stabilization` |
| 6 | 4 | Stitching | topic (GROUP_START) | | | |
| 7 | 5 | Seam Search | checkbox (also carves the seam) | | on | `seamSearch` |
| 8 | 6 | Exposure Match | checkbox | | on | `gainMatch` |
| 9 | 7 | Calibration | popup | Auto (as recorded) \| Lens Protectors / ND Filters \| Underwater \| Native (bare lenses) | Auto | `calibration` + `calibrationForceNative` |
| 10 | 16 | Sun Ghost Removal | checkbox | | on | `flareRemoval` |
| 11 | 17 | Sky Seam Fix | popup | Off \| Rim only \| Rim and colour | Rim and colour | `photoSeam` |
| 12 | 18 | Sky Seam Strength | float slider | 0..100 %, whole percent | 100 | `photoStrength` |
| 13 | 19 | Seam Edge Inset | float slider | 0..6 deg, tenths | 2.6 | `seamInset` |
| 14 | 20 | Seam Blend | float slider | 0.2..8 deg, hundredths shown, twentieths stored | 1.5 | `seamBlend` |
| 15 | 21 | Parallax Blend | float slider | 0..4 deg (0 = hard cut), as above | 0.35 | `parallaxBlend` |
| 16 | 22 | Seam Smoothing | float slider | 0..8 deg (0 = off), as above | 0 | `seamSmoothing` |
| 17 | 23 | Near Offset | float slider | -3..+3 deg, hundredths | 0 | `nearOffset` |
| 18 | 24 | Far Offset | float slider | -3..+3 deg, hundredths | 0 | `farOffset` |
| 19 | 34 | Lens Shading | popup | Off \| Auto | Auto | `lensShading` |
| 20 | 35 | Shading Strength | float slider | 0..100 %, whole percent | 100 | `shadingStrength` |
| 21 | 40 | Parallax Grid | popup | Auto (steady unless the scene moves) \| Steady (per clip) \| Follows scene (per moment) | Auto | `parallaxGrid` |
| 22 | 41 | Lens Alignment | popup | Auto (fit per clip) \| Off (calibration only) | Auto | `lensAlign` |
| 23 | 8 | (closes Stitching) | GROUP_END | | | |
| 24 | 9 | Advanced | topic (GROUP_START, starts collapsed) | | | |
| 25 | 10 | D-Log M Curve | popup | DJI Refit \| Pocket 3 \| Osmo 360 | Osmo 360 | `dlogmFit` |
| 26 | 11 | Exposure | float slider | valid -6..+6, slider -3..+3, tenths, stops | 0 | `exposureStops` |
| 27 | 12 | Render Device | popup | Auto \| CPU \| CUDA \| OpenCL | Auto | `renderDevice` |
| 28 | 14 | Program Monitor Colour | popup | Sequence space (fast) \| Match Source monitor | Sequence space | `directColour` |
| 29 | 13 | (closes Advanced) | GROUP_END | | | |
| 30 | 30 | Defaults | topic (GROUP_START, starts collapsed) | | | |
| 31 | 31 | Save | button, `PF_ParamFlag_SUPERVISE` | "Save as Default for New Clips" | | writes the user defaults file |
| 32 | 32 | Restore | button, `PF_ParamFlag_SUPERVISE` | "Restore Built-in Defaults" | | removes it |
| 33 | 33 | (closes Defaults) | GROUP_END | | | |

The Defaults group is always last and its indices are defined relative to
the Advanced terminator, so a control added to an earlier group moves them
without renumbering; ids 20-29 are left to the Stitching group. The lens
shading correction's ids (34-35) come after every id already shipped, the
steady seam's (40-41) after those, and the HDR peak's (46) after those.
See "User defaults for new clips" below.

An effect saved before ids 15-19 (or 34-35, or 40-41) existed has no stored
value for them, so it picks up the control defaults above (DJI look, ghost
removal, the sky seam fix, the lens shading correction, the steady seam and
lens alignment on) - a project opened in this build gets the improved
stitch.

#### Lens shading (ids 34-35)

Each lens's own brightness structure near its rim, measured from that lens's
own sky and added back before the lenses are blended
(`include/osv/render/LensShading.h`, docs/research/NEURAL_STITCHING.md
section 9). On the sample clip it is a soft dark ring in the lens facing the
sun, 83-89 deg from its axis: after the sky seam fix it was the soft darker
band left on every sky seam crossing, on the front lens's side.

| Control | What it does | Measured on the sample (default stitch, frames 0 / 32 / 64) |
|---|---|---|
| Lens Shading | Auto measures the ring per bucket of eight frames and adds the missing light back in the lens's native linear light, before every gain; Off leaves the lenses as decoded. A lens whose sky shows no structure, and every sector of a lens that shows no sky (the ground), is left untouched. | The band's dip below the sky's own trend at the seam 158 -> 21 millistops (open sky elsewhere scores 57-67); seam metrics line x0.40, band x0.32, broad x0.94, colour x0.86; the ground unchanged. |
| Shading Strength | How much of the measured correction is added. | Linear: 50 % leaves half the ring. |

Auto for new clips, and for a project whose Source Settings effect was
saved before the control existed (the control's default, like every control
added after the effect shipped).  A prefs blob written before it existed -
the modal dialog's - holds a zero at `PrefsBlob` offset 46, which reads as
Off: that clip renders as before until the control is set. The
correction travels inside the stitch block, so the importer's equirect, the
Source monitor and the Program monitor's direct path show the same picture.
Cost: 7-10 ms of analysis per bucket of eight frames; the render kernels do
not measurably slow down (+0.00 ms at 2560x1440 on the direct path).

#### Steady seam and lens alignment (ids 40-41)

Two per-clip analyses (`include/osv/render/LensAlign.h`,
`include/osv/render/ClipSteady.h`, `plugins/importer/SteadyStage.h`;
docs/research/AI_STITCHING.md section 5, "As built").

| Control | What it does | Measured on the sample |
|---|---|---|
| Lens Alignment | Auto fits the small rotation between the two lenses - three numbers per clip - from the flow of three fixed frames at 10 / 50 / 90 % of the clip, and turns each lens by half of it, in opposite senses, before anything else is measured: the stitched world, the horizon and stabilisation stay where they were. Refused (the calibration is kept, and the Properties panel says why) when the frames disagree by more than 0.05 deg, the fit leaves more than 0.1 deg RMS, fewer than 20 % of the cells agree, it exceeds 2 deg, or the cells do not pin down all three axes. Off: the recorded calibration, exactly. | 0.356-0.360 deg, the three frames within 0.004-0.008 deg, 0.058-0.068 deg RMS residual over 5551 of 5834 cells. The ground's lens-to-lens NCC without any flow grid 0.37 -> 0.88; with the grid 0.922 / 0.932 / 0.932 -> 0.932 / 0.942 / 0.946 (frames 0 / 32 / 64); the wing 0.310 / 0.288 / 0.332 -> 0.324 / 0.323 / 0.339; the whole band 0.916 / 0.918 / 0.921 -> 0.923 / 0.924 / 0.925; the sky 0.976 -> 0.974-0.975. |
| Parallax Grid | Steady measures the three per-moment seam corrections - the parallax flow grid, the seam-shift table and the carved seam - once, on nine fixed frames spread over the whole clip, and renders every frame with their median (per cell, per column). Follows scene re-measures them every eight frames and glides between measurements, as before. Auto is Steady unless a near object both lenses see moves past the seam: a textured 11.25 deg sector whose own correction aligns it (NCC >= 0.7, +0.05 over none) but which keeps less than 40 % of that gain AND loses more than 0.02 NCC under the clip correction makes the clip follow the scene. | Auto chooses Steady: the worst judged sector keeps 71-86 % of its own gain. The per-bucket analyses around the nacelle move each lens's picture by up to 1.96 px per frame at 6K (grid glide, p99 0.86 px, 24 of 64 frames above 1 px), the carved seam line by up to 0.60 px, and the seam-shift table steps by up to 13.5 px at a bucket edge where it is used; Steady: 0 for all three, by construction. The clip correction aligns as well as each frame's own: ground 0.925 / 0.932 / 0.930, wing 0.297 / 0.307 / 0.320 against 0.922 / 0.932 / 0.932 and 0.310 / 0.288 / 0.332. |

**What moved the picture.** `osv_importer_bench --part S` renders ONE
decoded frame of the sample at 6000 x 3000 with the corrections of every
frame of the per-moment schedule in turn, so the scene cannot move and every
pixel of motion on a 35 x 12 deg patch around the nacelle is the corrections'
own (px per frame at 6K):

| Correction | mean | p99 | worst frame pair's p99 | max |
|---|---|---|---|---|
| Parallax grid, re-measured every 8 frames and glided | 0.039 | 0.63 | 1.29 | 3.3 |
| Carved seam, the same | 0.002 | 0.05 | 0.14 | 0.63 |
| Seam-shift table, stepped at each bucket edge (only where the grid is refused) | 0.18 | 5.5 | 12.3 | 19.1 |
| All three, as Follows scene renders them | 0.040 | 0.64 | 1.29 | 3.3 |
| Steady (and Auto on the sample) | 0 | 0 | 0 | 0 |

The grid's glide is the "slight movement" (the same size with the rotation
folded: 0.053 mean, 1.19 worst p99). The carve's line and feather edges move
by up to 0.60 / 0.91 px per frame but hardly change the picture there. Of
what Seam Search off removes, the seam-shift table is what moves the
picture: on the sample at 6K every bucket's grid is accepted and the table
is unused, but wherever a bucket's grid is refused the table steps by up to
12-19 px at the bucket edge. The direct path serves the same clip grid and
seam to every frame (a test renders consecutive frames and compares the
tables). Rendered through the importer on the moving clip, the patch's own
motion - the nacelle reflects the moving ground - is 0.679 px per frame with
no seam corrections,
0.726 with Follows scene and 0.719 with the new defaults; what remains above
0.679 stays with every correction held still (Seam Search off: 0.677, a
1.5 deg Parallax Blend: 0.710), so it is how a sharper seam shows the
nacelle's two different reflections, not movement.

**Schedule.** Both run on a background worker of the clip's importer
instance from its first non-draft frame (thumbnails never start one), each
measured once per process and clip (a second instance of the same clip, the
direct path's included, reuses it) and keyed by the rig, the blend and the
analysis settings.

* **Exact frames** - export, the direct path, every frame the Program
  monitor is built from - wait for both, once per clip, up to 60 s (a
  failure or a timeout is logged once and the frame takes the per-moment
  corrections, measured on the spot). Nothing waits per bucket afterwards.
* **Interactive frames** (Source monitor scrubbing) never wait: until the
  clip correction lands they render with the nearest sample frame's own
  correction, marked non-exact so the host does not cache them, and from
  then on with the clip correction - the same pixels an Exact render of the
  frame gives.
* The rotation is also remembered on disk, next to the importer log
  (`lens-alignment.tsv`, keyed by the file's path, size, modification time and
  the rig), so reopening a clip folds it in before the first frame. The log
  says when each lands, for example "lens alignment: 'x.OSV': 0.360 deg about
  (+0.26, +0.04, -0.97) ... measured in 329 ms (decode 276)" and "steady:
  'x.OSV': clip correction ready 741 ms after the first request (... 9 sample
  frames ..., 9 grids accepted ...); Auto: steady: 0 of 32 judged sectors
  lose their alignment ...".

**Cost.** Lens alignment: 160-330 ms once per clip (three decodes and three
flow solves; the disk cache makes a reopen free). Clip correction: 0.74-0.95 s
once per clip (nine decodes, bands, flow and carves); after that no
per-bucket analysis of the three runs at all. The first Exact frame of a clip
therefore takes 0.9-1.2 s instead of 0.15-0.19 s.

**Rotation and the flow grid together.** With the rotation folded, the
residual the grid has to carry per cell is small, and the grid's default
benefit bar (a cell must improve its NCC by 20 %) switched off corrections
that were right: the ground fell to 0.892-0.904. A rig with a fitted rotation
therefore uses a 5 % bar (`render::kAlignedRequiredImprovement`, which lists
the gates measured); every other rig keeps 20 %.

**Old projects.** Auto / Auto for new clips, in the built-in user defaults
(`"parallaxGrid": "auto" | "steady" | "follows-scene"`,
`"lensAlignment": "auto" | "off"`), and for a project whose Source Settings
effect was saved before the controls existed (the controls' defaults, like
every control added after the effect shipped). A prefs blob written before
them - the modal dialog's - holds zeros at `PrefsBlob` offsets 50-51, which
read as Follows scene / Off: that clip renders exactly as before until they
are set. Auto was chosen as the default because on the sample it loses
nothing - no judged sector loses its alignment to the clip correction (the
worst keeps 71 % of its own gain), and the ground and whole band improve -
and a clip it does not suit falls back to the per-moment corrections by
itself. Also in the importer dialog ("Parallax
grid", "Lens alignment") and osvtool (`seam --lens-align`, `seam --steady`,
`seam --regions` for the ground / sky / wing scores).

#### HDR peak (id 46)

The display peak the BT.2100 PQ output's highlights roll off into, with the
BT.2408 Annex 5 EETF (docs/COLOR.md, "HDR peak brightness"). Everything below
the knee is left bit for bit; only the highlights above it compress.

| Choice | Knee (untouched below) | Diffuse white (203 nits) | The sample's sunlit aircraft, frame 30 (p50 / p90 / max nits) |
|---|---|---|---|
| 1000 nits (default) | - (no roll-off) | 203 | 525 / 753 / 1008 |
| 600 nits | 464 nits | 203 | 514 / 591 / 600 |
| 400 nits | 251 nits | 203 | 381 / 398 / 400 |
| 203 nits (SDR-safe) | 88 nits | 159 | 200 / 203 / 203 |

"PQ only" is in the name because a source settings effect cannot dependably
grey a control out: HLG is display-relative (the HLG display applies its own
peak), and Rec. 709, linear and the passthrough have no HDR highlights, so
all of them ignore it. It follows the clip onto the direct path: a clip
rendered into a PQ working space - whatever its own Colour Output - rolls off
into its own peak, and a change moves the Source Settings generation like
any colour setting, so the Program monitor re-renders. A Rec. 709 working
space is untouched by it. The Properties panel says "HDR peak: 600 nits
(highlights above 464 nits roll off, BT.2408 EETF)" when it is not 1000.

1000 for new clips, for every blob written before the setting existed (its
zero at `PrefsBlob` offset 54) and for an effect saved before id 46 existed
(the control's default) - so every existing project renders exactly as
before. Also in the importer dialog (greyed unless Colour output is PQ), the
user defaults file (`"hdrPeakNits": 1000 | 600 | 400 | 203`) and osvtool
(`render --hdr-peak`, `lut --hdr-peak`).

#### Seam tools (ids 20-24)

Five tweaks of the carved seam (`include/osv/render/SeamTools.h`). Every
default is the seam exactly as it rendered before they existed - an older
project's zero bytes (`PrefsBlob` offsets 38-45) read as those defaults - and
each changes only the band where both lenses see the scene. They act on the
carved seam, so they need Seam Search on.

| Control | What it does | Measured on the sample (frames 0 / 32 / 64, x1000 luma) |
|---|---|---|
| Seam Blend | Feather half width where the lenses agree. Wider hides colour differences, but shows more of both lenses. | 1.5 -> 3 / 6 deg: band ghost 4.4 -> 7.4 / 11.7, band edge unchanged (0.02); 0.5 deg: ghost 2.0, edge 0.08 (a visible cut). |
| Parallax Blend | Feather half width where they disagree (never wider than Seam Blend; 0 = a hard cut). | 0.35 -> 1 / 2 deg: the nacelle's seam edge 0.32 -> 0.16 / 0.15, but its sharp double image 1.6 -> 4.9 / 6.2. 0 deg: edge 0.76. |
| Seam Smoothing | A two-band blend (DJI's multiband): colour and shading blend across this half width, detail still switches at the seam, so a step on a near object becomes a gradient instead of a double image. 0 = off. | 1 / 2 deg: nacelle edge 0.32 -> 0.17 / 0.13, sharp double image 1.6 -> 2.1 / 2.5. From 4 deg on it adds soft halos (edge 0.31 / 0.54). Best at 1-2 deg. |
| Near Offset | Shifts content ALONG the seam where the lenses disagree (the carve's own disagreement per column). | See below: the mask does not isolate the nacelle on the sample. |
| Far Offset | The same where they agree. | +1.25 deg improves the ring's lens mismatch 57.8 -> 57.1 (frame 0). |

**Direction of the offsets.** Physical parallax on a back-to-back rig runs
along meridians, and the parallax grid corrects it where its flow is
trustworthy; what remains visible at the seam on the sample is ALONG the seam.
At the engine nacelle the seam runs vertically on a level view, and an NCC
search on the two lenses there finds them 1.41 deg apart along the seam and
0.35 deg across it - the flow's benefit gate switches itself off on that
shiny, texture-poor surface. So both offsets rotate the lenses' pictures
about their axes, in opposite directions: a positive offset turns the front
(master) lens's content by +offset / 2 in polar longitude and the rear
lens's by -offset / 2. On the sample's level view of the nacelle that moves
the rear lens's side of the seam (the nacelle body) DOWN and the spinner's
side up, 19 px per degree at 40 px per degree of view; the same rotation
reads the other way round on the camera's opposite side.

**The near mask on the sample.** The carve's disagreement weight is 0.16-0.19
on the nacelle's own columns (the dynamic programming routes the seam through
the few rows where the lenses agree there, so the residual along it is low)
and 0.49 on average elsewhere (textured ground misregisters by a fraction of
a degree; 401-402 of 840 other columns score above 0.5). Near Offset
therefore moves the ground more than the nacelle on this clip; the nacelle's
lens NCC is best at +0.75..+1.25 deg of Near Offset (0.27-0.30 against
0.23-0.28 at 0) and keeps improving with Far Offset. No per-column
photometric measure tried (band-wide gradient residual, 1 - NCC of luma or of
gradients, contrast-normalised difference) separates the nacelle from the
ground either: it sits at about the ground's 90th percentile. Telling near
from far reliably needs disparity, which the flow only measures where it is
consistent - exactly not on the nacelle.

**Cost of the smoothing.** Its low band is built per frame on the GPU from
the NVDEC frames (the importer's GPU path, and the engine for the direct
path, on the effect's stream) and on the host from host frames. CUDA, build
included: 2560 x 1440 view +0.08-0.14 ms on 0.28-0.40 ms; 6000 x 3000
equirect +0.12-0.28 ms on 1.5-1.9 ms (shared machine). The direct kernel
alone: +0.008 ms at 2560 x 1440. Host build 5.5 ms.

#### D-Log M passthrough

The fourth Colour Output entry is for the **grade-it-yourself** workflow: keep
the camera's log signal and convert it once, downstream, with a D-Log M LUT or
Lumetri's log handling, instead of having the importer convert and then
grading on top of that.

It maps to `color::OutputTransfer::Passthrough`, which the library already
had (`include/osv/color/ColorParams.h`, exposed by `osvtool` as
`--transfer dlogm`); the importer simply never wired it up. Passthrough
bypasses **both** the transfer curve **and** the primaries matrix -
`osvCodeToOutput` in `include/osv/color/ColorMath.h` returns the input
untouched - so what Premiere receives is the stitched sphere still in the
camera's own D-Log M encoding and its own gamut, as 32-bit float code values.
The frame therefore looks flat and washed out until it is graded, which is the
point, and the Properties panel (`imAnalysis`) says so in as many words.

**Applying a D-Log M LUT on top of a PQ, HLG or Rec.709 output would
double-convert.** That is the mistake this option exists to prevent, and it is
why the popup entry reads "D-Log M (no transform)" rather than "D-Log M": the
other three entries name what the output *is*, and this one has to say that
nothing was done to it, or a user reads it as "convert to D-Log M" and applies
a LUT anyway.

**What Premiere is told the colour space is.** There is no predefined token
for DJI D-Log M. `PrSDKColorSpaces.h` carries camera log spaces
(`kPrSonySGamutSLog2`, `kPrSony2020SLog3`, `kPrSonySGamut3CineSLog3`,
`kPrSonySGamut3SLog3`) but nothing for DJI, and grepping the whole SDK header
set for "dlog" / "dji" finds nothing. So the declaration is necessarily an
approximation, and the only question is which one misleads the host least.

`imGetIndColorSpace` returns **`kPrOverranged2020Scene`** - "BT.2020 RGB Full
(Scene)", full range, RGB, 32f, scene-referred - because three of its four
properties are exactly right and the fourth is the closest available:

* **full range**, **RGB** and **32f** are correct, and they are the properties
  that decide whether the host *rescales or reinterprets our bytes*. Getting
  these wrong corrupts pixels; getting the transfer wrong only mis-previews
  them.
* **scene-referred** is correct and matters: a log signal is scene light, not
  display light. The `(Display)` variant would invite the host to tone-map it
  for the monitor, which is precisely what a user who chose passthrough asked
  us not to do.
* **BT.2020 primaries** is the approximation. The real gamut is DJI's native
  camera gamut, which is wide and has no SDK token; BT.2020 is the widest
  standard gamut on offer, so it neither clips the data nor claims a small
  gamut for wide-gamut values.

Deliberately **not** returned:

* `kPrOverranged709` - claiming BT.709 for log data is the one genuinely
  harmful answer. The host would treat the flat log curve as a finished
  Rec.709 image, so the Program Monitor would show washed-out mush **and** a
  "Match Source" export would bake that interpretation in.
* `kPrOverranged2100PQ` / `...HLG` - both assert a specific HDR display
  transfer we did not apply, so the host's tone mapping would fight the log.
* `kPrWorkingColorSpace` - explicitly forbidden: `PrSDKColorSpaces.h:104`
  says "you can't use this token in the importer. the importer needs to
  explicitly identify media color space to the host."

Because the answer is approximate, `imGetIndColorSpace` **logs it once** with
the reasoning, so a user chasing an unexpected preview finds it in the support
log rather than nowhere. `colorSpaceIsApproximate()` is the single predicate
that decides, and a test asserts it is true for passthrough and false for the
other three. Under the `OSV_IMPORTER_COLOR_SEI` build the same output is
declared as primaries 9 (BT.2020) with transfer **2 (unspecified)** - the
honest H.273 answer, since there is no code point for D-Log M and naming a
specific curve would assert one we did not apply.

`PrefsColorOutput::DLogM` is **appended** as value 3, before `Count`, so every
previously saved project keeps its colour setting. `sanitise()` now accepts
0..3 and still rejects 4 and above, and a test pins that boundary from both
sides - a `sanitise()` that still stopped at 2 would silently reset every clip
a user had set to passthrough.

**The modal dialog's colour control is now a combo box**, not the three-way
radio group it was. A fourth option would not have fitted the old layout (the
three buttons sat at x = 16 / 92 / 168 inside a 254-unit group box), and every
other multi-choice setting in that dialog was already a combo. The retired IDs
`IDC_COLOR_PQ` / `_HLG` / `_709` (1001-1003) are recorded as retired in
`plugins/importer/resource.h` and deliberately not reused: a stale
`GetDlgItem(IDC_COLOR_PQ)` must return null rather than silently finding a
real but wrong control. Every `fillCombo` call in that dialog now passes
`std::size(...)` rather than a literal count, and each list's length is
`static_assert`ed against its enum's `Count` - a literal 3 against a
four-entry size list had already made the new default output size unreachable
in the one place a user goes to change it.

Two classes of drift are compiled out rather than tested for:

* every popup's item **count** is `static_assert`ed against its enum's
  `Count`, so an enum that gains a value breaks the build instead of shipping
  a popup that cannot express the new setting;
* the Exposure slider's **valid** range is `static_assert`ed against
  `PrefsBlob::kMinExposureStops` / `kMaxExposureStops`, so a slider can never
  offer a value `sanitise()` would silently clamp.

The 1-based popup **defaults** cannot be a `static_assert` because
`PrefsBlob::defaults()` is a runtime function (it memsets, then assigns), so
`test_params.cpp` compares each one against `defaults()` instead. That is the
assertion that matters most: two independently written defaults - a 1-based
literal in the PiPL and a 0-based enum in the blob - is exactly how a plug-in
ends up showing "2560 x 1280" while decoding at 6000 x 3000.

### Command flow

* **`PF_Cmd_GLOBAL_SETUP`** reports the version and the two PiPL flag words,
  then - inside Premiere only (`appl_id == 'PrMr'`) - calls
  `PF_SourceSettingsSuite::SetIsSourceSettingsEffect(effect_ref, TRUE)`. That
  single call is the difference between a master-clip settings panel and an
  ordinary video filter offered in the Effects panel; without it
  `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS` never arrives and the nine controls do
  nothing. The suite is acquired at v2 with a fallback to v1 (v2 is a typedef
  of v1, so one code path serves both). A missing suite is a logged
  degradation returning `PF_Err_NONE`, never an error: a plug-in that fails
  `GLOBAL_SETUP` is dropped entirely, and a panel that works minus the
  master-clip attachment is strictly better than no panel.
* **`PF_Cmd_SEQUENCE_SETUP`** calls
  `PF_SourceSettingsSuite::PerformSourceSettingsCommand`, which the host
  routes to the importer's `imPerformSourceSettingsCommand` (selector 66).
  The payload is a `PrefsBlob`; the SDK's only rule is that both halves agree
  on it ("the data can be anything as long as both the importer and the
  source settings effect both know what it is", `PrSDKImport.h:996`). The
  effect seeds the buffer with what its controls currently say, the importer
  replaces it with what the clip is actually being decoded with, and the
  effect writes the result back into the controls with
  `PF_ChangeFlag_CHANGED_VALUE` **on the changed ones only** - flagging an
  untouched parameter would make the host record a spurious change, and on a
  master clip effect that means an unnecessary media refresh and a re-stitch
  of the whole clip. This is what makes the panel show "as shot" after a
  project reopen instead of snapping every control back to the global default.
  Every failure path (no suite, a failed call, a blob that is not ours) keeps
  the stored control values and returns `PF_Err_NONE`. `sequence_data` stays
  null throughout: the effect keeps no per-instance state, so there is nothing
  to allocate, flatten or free.
* **`PF_Cmd_TRANSLATE_PARAMS_TO_PREFS`** is the only route by which anything
  set in the panel reaches the decoder. It reads the nine controls, builds a
  sanitised `PrefsBlob` and `memcpy`s exactly `sizeof(PrefsBlob)` bytes into
  `extra->prefsPC`. Three refusals, each a real failure mode: a null `extra`
  or `prefsPC` leaves the importer on its existing prefs (writing through the
  null would take the host down); a `prefs_sizeLu` **smaller** than the blob
  is refused outright and logged, because writing 128 bytes into a smaller
  buffer is a heap overflow in the *host's* allocator; a **larger** buffer is
  fine and its tail is left untouched, because it is not our memory to define.
* **`PF_Cmd_RENDER`** is answered with `PF_Err_NONE` and does nothing, because
  it never arrives. A test sends it with a null params array and a null output
  world anyway, so the one selector that should be impossible cannot be the
  one that crashes.

### Importer side

* `imInit`: `hasSourceSettingsEffect = kPrTrue`. Without this flag Premiere
  ignores `sourceSettingsMatchName` entirely, so the two must be set together.
  `hasSetup` stays `kPrTrue` as well - the modal dialog is deliberately kept
  working, because right-click > Source Settings is muscle memory for a lot of
  users and is the only route left on a machine where the `.aex` failed to
  install. Both paths write the same `PrefsBlob`.
* `imGetInfo8` fills `sourceSettingsMatchName` from
  `kSourceSettingsMatchNameW`.
* `imPerformSourceSettingsCommand` (selector 66, `param1` an
  `imFileAccessRec8*`, `param2` an `imSourceSettingsCommandRec*`) is in
  `SourceSettingsDialog.cpp` beside the other prefs selectors. With a live
  instance it answers with that instance's blob; with none (normal during
  project load, before `imOpenFile8`) it **echoes the effect's own blob back**
  rather than writing defaults - overwriting with defaults there would reset
  every control of every clip on every project open. A payload that fails
  `isValid()` becomes the defaults rather than being trusted, so a stale blob
  from an older build cannot reinterpret its bytes as stitch settings. A
  record with no buffer, or a buffer shorter than a blob, returns `imOtherErr`
  and writes nothing. The two-step `imGetPrefs8` protocol is untouched.

## User defaults for new clips

Users set the same Source Settings on every clip - Rec.709 with the DJI look,
2560 x 1280 on a laptop, the D-Log M passthrough for a colourist. They can now
set them once: every NEWLY imported clip starts from the user's saved
defaults instead of `PrefsBlob::defaults()`. Clips that already have settings
keep them, always. `PrefsBlob::defaults()` itself is unchanged: it is the
built-in reference the code and the tests are written against, and what a new
clip gets when nothing was saved.

### What the user clicks

* **Source Settings effect** (Effect Controls, master clip): the collapsed
  **Defaults** group at the end holds two buttons.
  * **Save as Default for New Clips** writes THIS clip's settings - exactly
    the blob `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS` produces, i.e. what the clip
    is decoded with - to the defaults file. The clip itself is unchanged.
  * **Restore Built-in Defaults** deletes the file; new clips start from the
    built-in settings again.
* **Source Settings dialog** (right-click > Source Settings): **Save as
  De&fault** on the OK / Cancel row stores the settings shown - on top of the
  blob the dialog was opened with, exactly as OK builds its blob, so the
  fields the dialog does not show (parallax, flow backend) are saved as the
  clip has them. The dialog stays open ("Saved." beside the button); only OK
  changes the clip.

The confirmation is a line in `OpenOSVSourceSettings.log` /
`OpenOSVImporter.log`. The effect also fills `out_data->return_msg`, but
raises `PF_OutFlag_DISPLAY_ERROR_MESSAGE` only outside Premiere - which is
exactly what Adobe's own Paramarama sample does for its button
(`if (in_data->appl_id != kAppID_Premiere) out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE`),
and a modal alert after every click of a settings button would be noise.

### Buttons in Premiere: the evidence

`PF_Param_BUTTON` is documented in `AE_Effect.h` as "supported by AE starting
with CS 5.5 (AE 10.5); may be supported in other hosts", and a click arrives
as `PF_Cmd_USER_CHANGED_PARAM` when the parameter carries
`PF_ParamFlag_SUPERVISE`. For Premiere specifically:

* Adobe's **Paramarama** sample (AE SDK `Examples/Effect/Paramarama`) adds its
  button (`PF_ADD_BUTTON`, flags `PF_ParamFlag_SUPERVISE` only - which is
  exactly how ours are declared) for every host reporting effect API 13.1 or
  later, Premiere included - its 3-D point next to it is the one parameter it
  withholds from Premiere - and its `UserChangedParam` has a Premiere-specific
  branch, so the button path is one Adobe wrote with Premiere in mind; that
  branch is the reason the alert flag above is gated.
* The Premiere SDK documents `PF_Cmd_USER_CHANGED_PARAM` delivery in Premiere
  for its own AE-API extensions (`PF_TransitionSuite`,
  `PrSDKAESupport.h:1673-1690`).
* Third-party Premiere effects ship buttons (an Adobe forum thread on
  handling `PF_Cmd_USER_CHANGED_PARAM` button clicks in Premiere, and open
  source Premiere effects with "Choose file" buttons), with one caveat worth
  keeping in mind: their authors describe Premiere's delivery of button
  clicks as historically less reliable than After Effects'.

What could not be verified without launching Premiere is the delivery to a
*source settings* effect in particular. No supported alternative is more
robust: a "Defaults" popup that resets itself after acting would need the
same `PF_Cmd_USER_CHANGED_PARAM` to reset, and acting on it from
`PF_Cmd_TRANSLATE_PARAMS_TO_PREFS` would re-save on every translate. The
modal dialog's button does not depend on the effect at all, so there is
always a working route.

### Where a new clip gets its first settings

The SDK guide names the moment: "When a clip is first imported, the effect is
called with `PF_Cmd_SEQUENCE_SETUP`. It should call
`PerformSourceSettingsCommand()` ... where it can read the file and set the
default prefs"; a saved project comes back through
`PF_Cmd_SEQUENCE_RESETUP` (the Opaque Effect Data section: "when reopening a
saved project"). User defaults are applied there and only there:

| Where | New clip (no blob of ours) | Clip with a stored blob |
|---|---|---|
| `imOpenFile8` (instance created) | seeded with the user defaults, before `open()` builds the rig | the seed is replaced by the stored blob at `imGetInfo8`, before any frame |
| `imGetInfo8` / every prefs-carrying selector | no blob, zeros or foreign bytes keep the seed (a zero-filled buffer used to be adopted as `PrefsBlob::defaults()`, as if the host had chosen them) | the blob is adopted as before |
| `imPerformSourceSettingsCommand`, live instance | answers the seed, so the effect's controls show it | answers the stored blob |
| Source Settings effect `SEQUENCE_SETUP` | untouched controls (translating to `PrefsBlob::defaults()`) are replaced by the user defaults before the importer is asked; with no live instance the importer echoes them | controls that say anything else are the clip's settings and are sent as they are; the importer's stored blob wins anyway |
| `imGetPrefs8` / `imGetInstancePrefs` | the dialog opens on the user defaults (`firstTime`, zeros, foreign bytes) | the stored blob, unchanged |

A new clip is logged once, with the file: `new clip 'DJI_....OSV'
(imGetInfo8): no stored Source Settings; starting from the user defaults in
C:\...\defaults.json - colourOutput rec709, ...`. Nothing is logged for the
built-in defaults. A clip Premiere holds no settings for at all is, by this
rule, a new clip; with the Source Settings effect installed every clip has a
stored blob after its first `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS`.

### The file

`%APPDATA%\OpenOSV\defaults.json` (roaming: a preference that follows the
user), or the path in `OPENOSV_DEFAULTS_FILE` (render farms, the tests).
One named key per setting - never a dump of the 128 bytes, so it survives
`PrefsBlob` layout additions - in the panel's order and words:

```json
{
  "format": "openosv-source-settings-defaults",
  "version": 1,
  "savedBy": "OpenOSV 0.1.0",
  "settings": {
    "colourOutput": "rec709",             // pq | hlg | rec709 | dlogm
    "rec709Look": "dji",                  // dji | standard
    "outputSize": "2560x1280",            // native | 3840x1920 | 2560x1280 | 1920x960
    "stabilisation": "horizon-lock",      // off | horizon-lock | full | smooth
    "seamSearch": true,
    "exposureMatch": true,
    "calibration": "auto",                // auto | native | lens-protectors | underwater
    "sunGhostRemoval": true,
    "skySeamFix": "rim-and-colour",       // off | rim-only | rim-and-colour
    "skySeamStrengthPercent": 100,        // 0..100
    "seamEdgeInsetDeg": 2.6,              // 0..6, tenths
    "lensShading": "auto",                // off | auto
    "shadingStrengthPercent": 100,        // 0..100
    "parallaxCorrection": true,
    "flowBackend": "auto",                // auto | classical | neural
    "dlogmCurve": "osmo360",              // dji-refit | pocket3 | osmo360
    "exposureStops": 0.0,                 // -6..6
    "renderDevice": "auto",               // auto | cpu | cuda | opencl
    "programMonitorColour": "sequence-space"  // sequence-space | match-source
  }
}
```

(The comments are this document's; the file as written has none, though a
hand-added `//` or `/* */` comment is accepted.)

Reading is forgiving, key by key: a missing key keeps the built-in value (an
older file predates it), an unknown key is ignored, a value this build does
not understand keeps the built-in value for that key, and an out-of-range
number is clamped - each noted once in the log. A document that is not JSON,
not an object, or whose `"format"` names something else is ignored as a
whole (logged once per state of the file): new clips get the built-in
defaults, which is always safe. Files over 64 KB are not read.

Writes go to a temporary sibling, are flushed (`FlushFileBuffers`) and
renamed over the target (`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`,
retried briefly while another process holds the old file), so no reader -
another Premiere, Media Encoder, osvtool - ever sees a torn file. Reads are
cached per module (the .prm and the .aex each have one) and re-validated per
call with one attribute-only open: modification time, size and the file's own
identity (volume serial + file index, which changes on every save because a
save is a new file). Everything is `noexcept` and thread-safe.

Adding a `PrefsBlob` field means adding one row to `kFields` in
`UserDefaults.cpp`: the test "the defaults file covers every byte of the blob
that holds a setting" fails until every non-padding byte before `reserved`
belongs to a key, so a new field cannot silently never become a default.

### osvtool

`osvtool render --use-user-defaults` starts every Source Settings option that
is NOT given on the command line from the defaults file (colour, look,
stabilisation, calibration, D-Log M curve, exposure, device, seam search,
gain, parallax, flow backend, sky seam fix and strength, seam edge inset, and
in the equirect modes the output size); anything on the command line wins,
and `--no-seam-search`, `--no-gain` and `--no-parallax` switch a saved default
off for one render. Sun ghost removal is an importer stage osvtool does not
have, and Program Monitor Colour belongs to the reframe effect; both are
ignored. Without the flag the file is never read, so a plain render is the
same on every machine.

## Sequence presets

Premiere copies a new sequence's frame size from the clip it is built from.
An equirectangular sphere is necessarily 2:1, no importer field asks for a
differently shaped sequence, and there is no SDK hook to suggest one - so a
new sequence from a 2560 x 1280 `.OSV` is 2560 x 1280, which is right for
working on the sphere and wrong for 16:9 delivery. The answer is to ship the
sequence presets, so the correct timeline is one click rather than a
hand-typed frame size and a frame rate that has to be exactly 59.94.

`presets/` holds three, and `presets/README.md` documents the schema field by
field and records where each field was verified from:

| Preset | Frame size | Shape | For |
|---|---|---|---|
| `OpenOSV 2560x1440 59.94.sqpreset` | 2560 x 1440 | 16:9 | Reframed delivery. Pairs with the importer's default 2560 x 1280 output. |
| `OpenOSV 3840x2160 59.94.sqpreset` | 3840 x 2160 | 16:9 | 4K delivery (set Output Size to Native or 4K first). |
| `OpenOSV 360 equirect 2560x1280 59.94.sqpreset` | 2560 x 1280 | 2:1 | The sphere itself / VR export. Declares monoscopic equirectangular VR. |

All three: square pixels (1:1), progressive, 59.94 fps, 48 kHz stereo over
four mono audio tracks, `VideoUseMaxBitDepth = true`.

### The verified format

The `.sqpreset` schema is not documented by Adobe. It was read off the presets
the installed application ships, under
`C:\Program Files\Adobe\Adobe Premiere Pro 2026\Settings\SequencePresets\`,
and nothing in ours is invented:

* `HD 1080p\HD 1080p 59.94 fps.sqpreset` and
  `UHD (4K)\UHD (4K) 2160p 59.94 fps.sqpreset` gave the Premiere 2026 record
  layout - `ClassID 5e73dd7e-4f86-4917-80eb-08ddb2f4a5f3`, `Version="9"`,
  which is the version that carries `WorkingColorSpace`,
  `SequenceWorkingColorSpace` and `AutoToneMapEnabled` - the `AudioTracks`
  JSON, the `EditingModeGUID` pair and the 59.94 tick value.
* `Legacy\VR\Monoscopic 29.97\3840x1920.sqpreset` gave the
  `ImmersiveVideoVRConfiguration` payload that declares equirectangular VR
  (`"projectionType":1`, `"capturedHorizontalView":360`,
  `"capturedVerticalView":180`). That file is `Version="8"`, so the VR field
  was lifted into the Version 9 body rather than the whole file being copied.

`VideoFrameRate` is a frame **duration in ticks**, not a rate. At Premiere's
254016000000 ticks per second one 59.94 fps frame is exactly
`254016000000 * 1001 / 60000 = 4237833600` - the same integer the importer
reports from `imGetInfo8` and the same one `tests/premiere/common` already
pins. A sequence built by hand at "60" is 4233600000 ticks and drifts against
the media by one frame in a thousand. `VideoUseMaxBitDepth` is `true`, unlike
Adobe's stock presets, because the importer hands Premiere 32-bit float frames
and an 8-bit sequence would quantise the sphere before the reframe resamples
it, banding every gradient in the sky. The files are UTF-8 with CRLF and no
trailing newline after `</PremiereData>`, byte-for-byte matching Adobe's own.

### Where they install

    %USERPROFILE%\Documents\Adobe\Premiere Pro\26.0\Profile-<user>\Settings\SequencePresets\OpenOSV\

They then appear in **File > New > Sequence** under a group called **OpenOSV**
- the subfolder name is the group heading, which is how Adobe's own presets
are grouped. Premiere caches the preset list, so it must be **restarted**.

That path is not documented either, so it was derived:

1. The per-user settings root is
   `Documents\Adobe\Premiere Pro\<ver>\Profile-<user>\Settings\` - not
   `%APPDATA%`. That directory exists on the test machine for 11.0, 24.0 and
   26.0 and already holds `EssentialSound`, `Export Destinations`,
   `Ingest Presets`, `Overlay Presets`, `Project View Presets`,
   `Source Patcher Presets`, `Timecode Presets` and `Track Height Presets`,
   all written by the application itself.
2. The sequence-preset subfolder is `SequencePresets`, spelled **without** a
   space. Premiere Pro 2026's own installed files name it that way, alongside
   every one of the folder names above, and never as `Sequence Presets`. It
   is also exactly the folder name the shipped presets live in under Program
   Files.
3. `.sqpreset` is the extension of the presets Premiere Pro ships, and the
   application's own installed files pair it with `SequencePresets`.

`scripts/install_plugins.ps1` enumerates the `<version>` folders and picks the
newest numerically (so "9.0" does not outrank "26.0"), enumerates `Profile-*`
rather than assembling it from `$env:USERNAME` (a domain account or a renamed
profile would break that), and when it finds no settings root it **says so and
skips** instead of inventing a path and reporting success. `-NoPresets` skips
them; `-PresetDestination <dir>` overrides the location.

One subtlety worth recording: the presets are installed by the **unelevated**
parent process, before it hands the modules to the elevated child, and the
child is always passed `-NoPresets`. A per-user path resolved inside an
elevated session belongs to whichever account answered the UAC prompt, which
on a machine with a separate admin account is not the person editing video -
the presets would land in a profile nobody ever opens.

## Effect design

### Identity

* Display name "Open 360 Reframe", category "OpenOSV", match name
  `OpenOSV.Open360Reframe` (never changes), version 1.0.0.
* `PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_CUSTOM_UI`
  (`0x06008000`). Not `PF_OutFlag_WIDE_TIME_INPUT`, and deliberately not
  `PF_OutFlag_FORCE_RERENDER` - the overlay commits values with
  `PF_ChangeFlag_CHANGED_VALUE`, which `AE_Effect.h:752` says already causes a
  re-render, and the same paragraph warns that forcing one adds cache
  invalidation that fights undo.
  `PF_OutFlag_CUSTOM_UI` is what makes `PF_Cmd_EVENT` arrive at all; see
  "Program Monitor overlay" below.
* `PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_REVEALS_ZERO_ALPHA`.
  Never `PF_OutFlag_PIX_INDEPENDENT`, never `PF_OutFlag_I_USE_AUDIO`.
  The PiPL `AE_Effect_Global_OutFlags(_2)` values are generated from the same
  constants (`ReframeParams.h` is included by the `.r` through the preprocessor
  step) so they cannot drift. Those constants are plain integer literals,
  because PiPLtool evaluates the resource text itself and rejects anything more
  structured than a number (`PIPL_GetExpression: Matching parantheses
  expected!` on `(1 << 19) | (3 << 9)`); `EffectMain.cpp` `static_assert`s each
  literal against the real `AE_Effect.h` macro, so an SDK that renumbers a bit
  breaks the build instead of shipping a wrong PiPL.
* In Premiere (`in_data->appl_id == 'PrMr'`) `PF_Cmd_GLOBAL_SETUP` registers
  the four formats below through the PF Pixel Format Suite, and
  `PF_Cmd_RENDER` re-registers them. See "Pixel formats" below.

### Parameters (IDs are permanent; the INDEX is not the ID)

`PF_ADD_TOPIC` and `PF_END_TOPIC` each issue their own `PF_ADD_PARAM`
(`Param_Utils.h:298-320`), so a group terminator is a real parameter holding
a real index, and it sits in the MIDDLE of the list. There are therefore 22
parameters, not 20, and a control that follows a closed group has an index
one higher than its id per group already closed. `ReframeParams.h` spells the
index table out literally (`ParamIndex`, with `kParamIdByIndex` beside it)
rather than deriving it, and `EffectMain.cpp` static_asserts the
relationship. The GPU path's `GetParam(index - 1)` uses the same table, so
both halves read the same control.

The "Shown" column is what the Effect Controls panel displays for each
choice of the Lens popup (see "One lens at a time" below).

| Index | ID | Name | Type | Range / items | Default | Shown |
|---|---|---|---|---|---|---|
| 1 | 1 | Output Resolution | popup | Match Sequence \| 3840 x 2160 \| 2560 x 1440 \| 1920 x 1080 \| 1280 x 720 | Match Sequence | always |
| 2 | 2 | Camera | topic (GROUP_START) | | | always |
| 3 | 3 | Preset | popup | Custom \| Crystal Ball \| Asteroid \| Wide \| Ultra Wide \| Dewarping | Wide | always |
| 4 | 4 | Pan | angle | unbounded | 0 | always |
| 5 | 5 | Tilt | angle | unbounded (clamped to +-90 in code) | 0 | always |
| 6 | 6 | Roll | angle | unbounded | 0 | always |
| 7 | 7 | FOV | float slider | valid 10..350, slider 30..180, tenths, degrees; the visible angle across the width | 120 | Classic |
| 8 | 8 | Distortion | float slider | 0..100 %, slider 0..100 | 15 | Classic |
| 9 | 14 | (closes Camera) | GROUP_END | | | |
| 10 | 9 | Source | topic (GROUP_START, starts collapsed) | | | always |
| 11 | 10 | Source Pan | angle | | 0 | always |
| 12 | 11 | Source Tilt | angle | | 0 | always |
| 13 | 12 | Source Roll | angle | | 0 | always |
| 14 | 15 | (closes Source) | GROUP_END | | | |
| 15 | 13 | Smooth Keyframes | checkbox | | off | always |
| 16 | 16 | Camera Model | checkbox "DJI", registered `PF_PUI_INVISIBLE` | retired: the Lens popup's hidden mirror (ticked = DJI) | on | never |
| 17 | 17 | Zoom | float slider, not animatable | valid 0..360, slider 30..330, degrees; DJI Studio's Zoom read-out, editing it walks DJI's zoom path | 142.4 | DJI |
| 18 | 18 | DJI FOV | float slider | valid 1..178, degrees; DJI's vertical pinhole angle. Registered as "DJI FOV", shown as "FOV" | 60 | DJI |
| 19 | 19 | Correction Angle | float slider | valid 0..1.8, sphere radii the eye sits behind the centre (0 rectilinear, 1 stereographic, >1 crystal ball) | 0.6 | DJI |
| 20 | 20 | Drag Sensitivity | float slider | valid 0.1..10, slider 0.25..5; Program Monitor grab speed, never rendered from | 2.0 | always |
| 21 | 21 | Lens | popup, not animatable | DJI \| Classic: which lens renders and which lens's controls are shown | DJI | always |
| 22 | 22 | Keyframe Easing | popup, not animatable | None \| Linear Smooth \| Fast In, Slow Out \| Slow In, Fast Out \| Fast In, Fast Out \| Slow In, Slow Out \| Linear: how the camera moves between keyframes (see "Keyframe Easing" below) | None | always |

"Output Resolution" and "Smooth Keyframes" are top-level siblings of the two
groups, which is only true because both groups close. Ids 16-22 were appended
after Smooth Keyframes, outside both groups, so every existing index - and
every saved project - stayed where it was; the DJI model's maths and evidence
are in `docs/research/DJI_CAMERA.md`. A test walks the real parameter list
keeping a nesting depth and asserts exactly that.

The Lens popup is appended rather than placed at the top of the Camera group
on purpose. After Effects matches saved values to parameters by these ids and
so allows an insertion anywhere (the AE SDK's "Changing Parameter Orders"
chapter), but nothing in the Premiere Pro SDK guide says Premiere does the
same, and an insertion that Premiere resolved by index would load every saved
project's Pan into the popup and shift every control after it. An append is
the one placement that is safe however the host binds.

A project saved before the Lens popup existed has no value for it and opens
at the popup's default, DJI - the user's choice. Its old Camera Model
checkbox, ticked or not, is never rendered from.

Changing Preset (supervised, `PF_Cmd_USER_CHANGED_PARAM`) writes FOV,
Distortion, DJI FOV, Correction Angle, Zoom and Tilt from the preset table,
selects DJI in the Lens popup and marks each changed value
`PF_ChangeFlag_CHANGED_VALUE`; editing a look control flips Preset back to
Custom (and does nothing when it is already Custom, so a slider drag does not
fill the undo stack). Picking a lens converts the current look into that
lens's controls so the framing does not jump (Classic -> DJI exactly, DJI ->
Classic exactly unless the Classic ramp or a Correction above 1 makes the DJI
look unrepresentable), updates the hidden mirror, sets Preset to Custom and
returns `PF_OutFlag_REFRESH_UI`. The mirror holds the lens that was on screen
before the edit, which is how a re-pick of the same lens is told from a
switch: it converts nothing. Classic is always left beside Preset "Custom",
even on a re-pick, because the GPU path decodes Classic's ambiguous popup
value on Premiere (1, where popups count from 0 on the GPU side) with the 0
that Custom reads there; DJI reads 0 itself.

`PF_ParamFlag_SUPERVISE` is set on Output Resolution, Preset, Tilt, FOV,
Distortion, Camera Model, Zoom, DJI FOV, Correction Angle and Lens. Output
Resolution does not act on the message - it carries the flag so adding
behaviour later does not change the PiPL.

#### One lens at a time (`PF_Cmd_UPDATE_PARAMS_UI`)

The panel shows only the selected lens's controls, so a user never sees two
"FOV" sliders that mean different angles (Classic's is the visible angle
across the width, DJI's the vertical pinhole angle):

* **Lens: DJI** (the default) - Output Resolution, Preset, Pan, Tilt, Roll,
  Source, Smooth Keyframes, **Zoom**, **FOV** (DJI's), **Correction Angle**,
  Drag Sensitivity, Lens.
* **Lens: Classic** - Output Resolution, Preset, Pan, Tilt, Roll, **FOV**,
  **Distortion**, Source, Smooth Keyframes, Drag Sensitivity, Lens.

`PF_Cmd_UPDATE_PARAMS_UI` calls `PF_UpdateParamUI` (PF Param Utils Suite v3)
for the five lens controls and the Camera Model mirror, setting or clearing
`PF_PUI_INVISIBLE`, and names DJI FOV "FOV" while it is shown. What says
Premiere supports this:

* `AE_Effect.h`, `PF_PUI_INVISIBLE`: "in Premiere since earlier than [CS6],
  this hides the parameter UI in the Effect Controls, which includes the
  keyframe track; for PPro only, the flag is dynamic and can be cleared to
  make the parameter visible again". The flag is also documented as the way
  to keep "hidden data parameters that affect rendering", so a hidden control
  still reaches the renderer (the GPU probe's one-time dump in the plug-in log
  shows the host list, should that ever need checking).
* `AE_EffectSuites.h`, `PF_UpdateParamUI`: the fields it may change are
  "ui_flags: PF_PUI_ECW_SEPARATOR, PF_PUI_DISABLED only (and
  PF_PUI_INVISIBLE in Premiere)", the name, `PF_ParamFlag_COLLAPSE_TWIRLY`
  and a slider's range, precision and display flags.
* Adobe's Supervisor sample (`Examples/UI/Supervisor/Supervisor.cpp`,
  `UpdateParameterUI`) hides and shows its advanced controls in Premiere
  exactly this way, from `PF_Cmd_UPDATE_PARAMS_UI`; its After Effects branch
  needs the AEGP Dynamic Stream Suite (`AEGP_DynStreamFlag_HIDDEN`) instead,
  which Premiere does not provide. Adobe's Zac Lam confirms
  `PF_UpdateParamUI` "can work in Premiere Pro" (Adobe community, "what can I
  use instead PF_UpdateParamUI for converting After Effects plugin to
  Premiere Pro plugin?").

Known Premiere behaviours the implementation works around:

* Premiere 25 to 25.2 beta ignored `PF_PUI_INVISIBLE` changes on the FIRST
  instance of an effect when they were made during
  `PF_Cmd_USER_CHANGED_PARAM` (Adobe tracking DVARC-3737; the reported
  workaround is to change visibility only in `PF_Cmd_UPDATE_PARAMS_UI`). The
  lens switch therefore only returns `PF_OutFlag_REFRESH_UI`, as the
  Supervisor sample does, and the visibility is set in the refresh.
* A developer reported a topic that came back nameless after hiding it
  (Adobe community, "Conditionally hiding topic params in the ECP"); another
  reported name changes that did not refresh when several parameters were
  updated at once, which Adobe traced to
  `PF_OutFlag2_PPRO_DO_NOT_CLONE_SEQUENCE_DATA_FOR_RENDER` (bug 3197343; this
  effect does not set it). So no topic is ever hidden, and every def passed
  to `PF_UpdateParamUI` gets its type, name and slider display from the
  effect's own constants, never from whatever the host's copy held.
* Undo is reported not to send `PF_Cmd_UPDATE_PARAMS_UI` (same DVARC-3737
  report): after undoing a lens switch the panel can show the previous lens's
  controls until the next refresh (selecting the clip, moving the playhead).
  The picture is right either way - the renderer reads the popup.
* The update is applied on every call, not only when the host's copy of a
  def looks different: nothing documents that the params array handed to
  `PF_Cmd_UPDATE_PARAMS_UI` reflects earlier `PF_UpdateParamUI` calls, and
  trusting it could leave a control hidden after switching back.

Every failure is non-fatal: without the suite, or when the host refuses an
update, the panel shows every control - the layout before this - and the
effect renders exactly the same.

#### Keyframe Easing (id 22)

DJI Studio's Keyframe Animation section offers seven presets for how the view
travels between keyframes, applied per clip, with an "Apply to all" button.
Premiere's scripting APIs can set a keyframe's interpolation TYPE (linear,
hold, Bezier) but never an arbitrary curve, so the effect draws the curve
itself: the Keyframe Easing popup picks one, and the OpenOSV panel's Keyframe
Animation card sets it on selected clips or on every OSV clip of a sequence
(`docs/PANEL.md`).

**What it eases.** Pan, Tilt, Roll and the selected lens's two controls (DJI
FOV and Correction Angle, or Classic FOV and Distortion). Between two
keyframes `k0 <= t < k1` of such a control the readers compute

    v(t) = v0 + (v1 - v0) * s((t - k0) / (k1 - k0))

from the control's values AT the two keyframes, instead of taking the host's
in-between value. The Source angles are left to Premiere (they orient the
panorama; DJI Studio's keyframe animation eases the camera), Zoom is not
animatable, and a control with fewer than two keyframes, or a time before the
first or after the last one, keeps the host's value. Smooth Keyframes then
averages eased samples.

**The presets.** Each is defined by its speed profile across one interval,
relative to a straight line's speed (1). DJI Studio's own preset icons draw
exactly these profiles - a flat line, a bell, a valley, an S-shaped fall and
rise, and a tilde for Linear Smooth - and the panel draws its tiles from the
same functions (`osvcore.js`, `easeSpeed`):

| Popup entry | Speed profile | s(u) | Established how |
|---|---|---|---|
| None | - | Premiere's own interpolation | the popup's default; nothing is computed |
| Linear Smooth | steady through each keyframe | cubic Hermite, tangent at each keyframe = mean of the straight-line slopes on either side (exactly linear on two keyframes) | DJI's name and icon; the curve is the standard one |
| Fast In, Slow Out | 2 -> 0, flat at both ends | 2u - 2u^3 + u^4 | DJI's name and profile shape; standard polynomial |
| Slow In, Fast Out | 0 -> 2, flat at both ends | 2u^3 - u^4 | as above |
| Fast In, Fast Out | 2 at both keys, 0.5 halfway | 2u - 3u^2 + 2u^3 | as above |
| Slow In, Slow Out | 0 at both keys, 1.5 halfway | 3u^2 - 2u^3 (smoothstep) | as above |
| Linear | 1 | u | DJI's name; exact by definition |

The seven entries, their names and their order are DJI Studio's. Linear is
exact by definition; for the other five the shape of each profile matches
DJI Studio's icon, but the exact numbers DJI Studio uses could not be
established from DJI's public material, so each is the standard polynomial
with that shape. All are monotone, start at 0, end at 1 and cover the same
distance as a straight line; Slow In / Slow Out and Fast In / Fast Out are
point-symmetric, and Slow In / Fast Out is Fast In / Slow Out reversed.

**Why the value is computed, not sampled at a remapped time.** For
Premiere's default Linear keyframes `v0 + (v1 - v0) s(u)` is exactly the value
Premiere gives at the remapped time `k0 + s(u) (k1 - k0)`. Computing it
instead of sampling there has two advantages: the CPU path's time grid can be
one unit per frame, where a remapped time between frames is not
representable, and both paths then do the same arithmetic in
`ReframeEasing.cpp` - CPU / GPU parity by construction. The consequence:
while a preset is chosen it alone decides how an eased control moves between
its keyframes, as the keyframe connection does in DJI Studio; Premiere's
Bezier handles or Hold on those keyframes are not consulted.

**How each path finds the keyframes.**

* CPU (`PF_Cmd_RENDER`): `PF_ParamUtilsSuite3::PF_FindKeyframeTime` (less
  than or equal / less than / greater than) for the neighbouring keyframes,
  and `checkout_param` at each keyframe's own time and time scale for its
  value. The AE SDK's list of features Premiere does not support does not
  name the Param Utils suite (`PF_UpdateParamUI` from the same suite already
  works in Premiere, see "One lens at a time"); a host without the suite
  logs one line and keeps its own interpolation.
* GPU and the direct path: the Video Segment Suite's `GetNextKeyframeTime`
  ("the next keyframe time after the specified time") through the probed
  host index, and `GetParam` at the keyframe for its value. The suite can
  only walk forwards, so the last keyframe at or before `t` is found by a
  doubling backwards probe - O(log) calls however far back it is; the test
  measured at most 14 calls per search over 300 random keyframes, against
  a 300-call scan. The direct path renders from the same `Settings`, so it
  eases identically.

**None is bit-identical.** With None the CPU path never acquires the Param
Utils suite and neither path makes a single keyframe query; the tests count
zero `PF_FindKeyframeTime` and zero `GetNextKeyframeTime` calls and compare
the render byte for byte. An old project loads the popup at None.

**The overlay** reads and writes the host's values (keyframes are recorded
the usual way); between keyframes of an eased control its read-out shows the
host's value, not the eased one the picture uses.

### Geometry

* Output frame = input frame size (Premiere renders clip effects at source
  resolution scaled by `inDownsampleFactor`). The picture goes into the
  largest rectangle of the chosen aspect centred in that frame; outside is
  transparent black so "Set to Frame Size" in the sequence crops cleanly.
  "Match Sequence" reads `SequenceInfoSuite::GetFrameRect` (GPU:
  `inTimelineID`; CPU: `PF_UtilitySuite::GetContainingTimelineID`) and falls
  back to 16:9. "Full Frame" uses the whole input frame.
* `Rout = R_source(sourcePan, sourceTilt, sourceRoll) * R_camera(pan, tilt, roll)`
  with the milestone 1 body frame (X right, Y forward, Z up) and
  `VirtualCamera::rotation()` order Rz(yaw) Rx(pitch) Ry(roll). Input equirect
  uses the Standard layout (centre column = +Y).
* Effective FOV is clamped below `2 * acos(-d) - 1 degree` so the eye-offset
  projection stays invertible.
* Half-float (16f) GPU frames are read and written as half; math is float.

### GPU path (`xGPUFilterEntry`)

* `Startup`: nothing device-specific. `CreateInstance`: `GetDeviceInfo`;
  accept only `PrGPUDeviceFramework_CUDA` with
  `outMeetsMinimumRequirementsForAcceleration`; otherwise return
  `suiteError_Fail` so Premiere renders through the CPU entry.
* CUDA is used exclusively through the driver API (`cuda.h`, link `cuda.lib`,
  no cudart): `cuCtxPushCurrent(outContextHandle)` around every call, kernels
  launched on `(CUstream)outCommandQueueHandle`, module loaded once per device
  from an embedded fatbin (`cuModuleLoadFatBinary`, cached in a static table
  keyed by device index, released at `Shutdown`).
* The fatbin is produced at build time by nvcc from `ReframeKernel.cu`
  (`-fatbin -gencode arch=compute_50,code=[sm_50,compute_50]` plus real
  cubins for 60, 61, 70, 75, 80, 86, 89, 90, 120 when the toolkit supports
  them) and embedded with `EmbedKernel.cmake`. `-fmad=false`, no fast math,
  same as the library, so GPU and CPU stay within the parity budget.
* Parameters are read with `VideoSegmentSuite::GetParam(nodeID, hostIndex, inClipTime)`
  where `hostIndex` comes from a RUNTIME PROBE, not from `index - 1`. The
  SDK's documented rule (`PrGPUFilterModule.h:153`, "GPU filters do not
  include the input frame") assumes the host's index space is the AE
  parameter list, group markers included. Premiere Pro 26.2 disproves it:
  this effect adds 15 parameters (11 controls + 4 group markers) and
  `GetParamCount` answers **8** - the markers are dropped AND the three
  controls inside the `START_COLLAPSED` "Source" group are not exposed. Under
  the static rule FOV was read from host index 6, which holds Distortion;
  `buildParams()` then failed its focal-length check and the render bailed,
  so every control appeared to do nothing. `probeParams()` (GpuFilter.cpp)
  now reads each host entry's PrParam TYPE once per instance and matches the
  sequence `i32 i32 f32 f32 f32 f64 f64 f32 f32 f32 bool` against the host
  list allowing one contiguous gap; the match must be unique and must consume
  every host entry, or the static mapping is kept and the choice is logged.
  A control the host does not expose maps to -1 and uses its default.
* `buildParams()` no longer returns an invalid setup for a bad field of view:
  it clamps, retries once at the default, and only refuses a camera that is
  degenerate at the default. A wrong-but-visible frame beats a dead effect.
* (superseded) Parameters were read with `GetParam(nodeID, index - 1, inClipTime)`
  (angles arrive as `mFloat32` degrees, float sliders as `mFloat64`, popups as
  `mInt32`, checkboxes as `mBool`); the suite is acquired at version 9 with
  fallback to 8, 7 and 6 (only `GetParam` / `GetNodeProperty` are used). When "Smooth Keyframes" is on,
  the angle values are sampled at t - 1, t, t + 1 frames and averaged
  (`GetParam` at neighbouring times), giving a cheap temporal smoothing that
  matches the CPU path bit for bit.
* `Render`: `GetGPUPPixData` + `GetRowBytes` + `GetPixelFormat` on
  `inFrames[0]`, output written in place into `*outFrame` (same size, same
  format, top-left origin). `outIsRealtime = true`.

### Pixel formats

The effect registers exactly these four, in this order, through
`PF_PixelFormatSuite1::AddSupportedPixelFormat`. The order is a **preference
ranking** to the host (`PrSDKAESupport.h:150-157`), not just a list.

| # | `PrPixelFormat` | `PixelLayout` | Why, and where it sits |
|---|---|---|---|
| 1 | `BGRA_4444_32f` | `Bgra32f` | The native working format. The panorama is HDR-capable and reframing is a **resample**, so any quantisation before it bands the sky. |
| 2 | `BGRA_4444_32f_Linear` | `Bgra32f` | Identical bytes to the above - four floats, B, G, R, A - differing **only** in transfer function. Accepted because this effect is colour agnostic (see below), and it renders **bit-identically** to `_32f`. |
| 3 | `BGRA_4444_16u` | `Bgra16u` | Premiere's 16-bit integer RGB, so a **10-bit sequence** has a high-bit-depth format in common with us. The SDK guide recommends 32f over 16u for high bit depth, which is exactly why it sits below both float entries rather than being omitted. |
| 4 | `BGRA_4444_8u` | `Bgra8u` | Last, so an 8-bit sequence still gets a native-format render instead of a host conversion. |

`VUYA` is deliberately **not** offered: the shared sampler works in RGBA and
converting per sample would cost more than letting the host convert the frame
once. A `static_assert` in `EffectMain.cpp` proves every advertised format is
one `layoutFor()` accepts, so advertising a format the render path would
refuse cannot compile.

**The 16-bit scale is 0..32768, not 0..65535.** Premiere SDK guide section
5.4.2 ("Byte Order"): *"8-bit and 16-bit BGRA formats do not contain super
whites or super blacks. The 16-bit formats use channels that go from black at
0 to white at 32768, like After Effects and Photoshop 16-bit formats."* So
white is `kBgra16uWhite = 32768` (`ReframeCpu.h`); codes above it are
out-of-gamut rather than illegal. Input codes are **not** clamped (an
over-range highlight is resampled faithfully); output is clamped to
`[0, 32768]` on write. Getting this constant wrong is a 100 %-scale error on
every pixel, so the tests assert it against the documented number spelled out
independently on the test side (`kBgra16uWhiteRef`) rather than importing the
plug-in's own constant.

**Why `_32f_Linear` is safe to accept.** The effect never interprets a code as
a luminance: it computes a direction per output pixel and bilinearly resamples
the input there, so every operation is a weighted average of neighbouring
samples in whatever space they already are. It introduces no error the host
has not already accepted by asking a resampler for the frame, and the output
carries exactly the tag the host gave the destination world. A
colour-*managing* effect could not do this; a resampler can.

**When the suite is missing.** `AcquireSuite(kPFPixelFormatSuite, 1)` can fail
at `PF_Cmd_GLOBAL_SETUP` on a real host - the live log shows it missing on a
minority of setups, on their own threads, interleaved with successful ones
milliseconds apart, because Premiere calls `GLOBAL_SETUP` many times over
(once per render session, plus short-lived probe instances) and not every
effect reference has the pixel-format machinery attached. `pica_basicP` is
valid on all of them, so there is no pointer to test and no documented way to
tell them apart in advance. The effect therefore:

* treats the failure as a **graceful downgrade**, never an error - it returns
  `PF_Err_NONE` and logs one `WARN` naming the `AcquireSuite` error code and
  which command it was in (the call-site is part of the log's once-key, so a
  successful setup cannot silence a failed render-time retry);
* **retries the registration from `PF_Cmd_RENDER`**, unconditionally. It
  cannot help the current frame - the worlds are already allocated - but it
  stops one missed negotiation from being permanent for the session. The
  retry is not memoised: the registration is keyed on `effect_ref`, which is
  a foreign pointer with no destruction hook this effect receives, so a table
  of them could be matched by a *new* reference at a recycled address and
  suppress the retry for the one instance that needs it. `AcquireSuite` is a
  name lookup and the render path already acquires this same suite twice per
  frame, so the unconditional retry costs nothing measurable;
* **renders anyway**, because `layoutFor()` accepts all four formats above.
  This is the half of the fix that matters when the retry also fails: a host
  choosing unaided is then very likely to pick a format we can render.

A format that is still unknown degrades with **one** `ERROR` line that names
the offending fourcc as **readable characters** as well as hex (`PrPixelFormat`
enumerators are fourccs; non-printable bytes render as `.`), says which side
was wrong, and lists what would have been accepted - then returns
`PF_Err_BAD_CALLBACK_PARAM` rather than misreading the bytes. The refusal
happens every time; only the log line is deduplicated.

> This is the fix for the reported bug: on a 10-bit HDR sequence, pressing
> PLAY showed correctly reframed footage but stepping or scrubbing did not
> update the picture. Playback goes through `xGPUFilterEntry`, which
> negotiates separately and was fine; stepping and scrubbing go through
> `PF_Cmd_RENDER`, which advertised only `32f` and `8u` and so had **no format
> in common** with the sequence and refused every frame.

### CPU path (`PF_Cmd_RENDER`)

`PF_Cmd_RENDER` receives `params[0]` (input layer) and `output` in the
registered Premiere format. Rows are addressed as `data + y * rowbytes` in
top-down order for both worlds (negative row bytes are legal). The same
`osvReframeEquirectPixel` runs per pixel on the library `ThreadPool`. An
integer-coded INPUT world (`8u` or `16u`) is promoted once into a float
scratch buffer (`promoteIntegerToFloat`, codes / 255 or codes / 32768) because
the shared sampler reads float or half only; an integer OUTPUT world is
quantised with rounding on write. Promotion rather than teaching the sampler
to read integers is deliberate: a bilinear fetch touches four pixels and
sixteen components, so decoding inside it would put a multiply and a format
branch in the hottest loop in the effect - on the GPU too, since the kernel is
shared source - whereas promotion pays the conversion exactly once per source
pixel in a parallel row-wise pass and leaves the kernel byte-for-byte the one
that renders a float source. The effect advertises every one of these formats
in GLOBAL_SETUP, so it accepts them rather than declining and hoping the host
renegotiates. Parameters come from `params[i]` at the render time, with the
same three-sample smoothing implemented through `PF_CHECKOUT_PARAM` at
neighbouring times when "Smooth Keyframes" is on.

### Program Monitor overlay (`PF_Cmd_EVENT`)

The headline feature: the user reframes by dragging the picture in the
Program Monitor, and Premiere records the keyframes. Implemented in
`plugins/reframe/ReframeUi.{h,cpp}` (the interaction maths, no Adobe header)
and `ReframeUiEvent.{h,cpp}` (the `PF_Cmd_EVENT` / DrawBot shim). The split
is why `tests/premiere/reframe/test_ui.cpp` can pin the drag-to-degrees
mapping as pure arithmetic against the very source the `.aex` contains.

#### Registration

`PF_OutFlag_CUSTOM_UI` in `GLOBAL_SETUP` **and** in the PiPL (both come from
`OSV_REFRAME_OUT_FLAGS` in `ReframeParams.h`, so they cannot drift; a
`static_assert` in `EffectMain.cpp` ties the literal to `AE_Effect.h`).
`PARAMS_SETUP` then calls `in_data->inter.register_ui` with a
`PF_CustomUIInfo` whose `events` is `PF_CustomEFlag_COMP`. There is no
`PF_OutData` member for this - `register_ui` is the only route. `COMP` is the
composition window, which in Premiere is the Program Monitor; the AE docs
also note Premiere requires that flag for a custom UI to receive keyboard
events at all. `comp_ui_width` / `comp_ui_height` stay **zero**, meaning "the
whole view" - the overlay tracks the picture as the monitor is zoomed, so
pinning it to a fixed pixel box would be wrong at every zoom level but one.
`PF_CustomEFlag_EFFECT` is *not* set: the Effect Controls panel keeps its
standard dials and sliders.

#### Interaction model

All coordinates are frame pixels, origin top-left, +y down. The overlay works
on the **viewport** - the centred rectangle of the chosen Output Aspect that
the effect letterboxes into - not on the whole frame, so the crosshair never
floats in the black bars.

| Gesture | Result |
| --- | --- |
| Drag anywhere in open picture | Pan + Tilt |
| `Shift` + drag | Constrained to the dominant axis (Pan only or Tilt only) |
| `Ctrl` + drag anywhere | FOV (zoom) |
| `Alt` + drag anywhere | Roll |
| Drag a roll-ring arc | Roll |
| Drag a corner grip vertically | FOV (zoom) |

`Ctrl` is `PF_Mod_CMD_CTRL_KEY` and `Alt` is `PF_Mod_OPT_ALT_KEY`
(`AE_EffectUI.h:252-259`); both are read from `PF_EventExtra` on every event,
so a modifier pressed **mid-drag** retargets the same gesture immediately
without letting go. A deliberate handle grab is never overridden by a
modifier. `Ctrl` wins over `Alt` when both are held. `PF_Mod_CAPS_LOCK_KEY`
and `PF_Mod_MAC_CONTROL_KEY` are ignored.

**Rate.** Pan/Tilt move at `fov / viewportWidth` degrees per pixel: dragging
across the full viewport width turns the view by exactly one field of view,
so the content under the cursor stays under the cursor and a drag feels
identical at 30 and at 150 degrees. The rate is taken from the FOV **at grab
time**, so a keyframe moving the FOV mid-drag cannot make the picture slide
out from under the hand. Every mode measures from the gesture's **anchor**,
never by accumulating per-event deltas, so returning the cursor to the grab
point returns the value exactly.

**Signs** (derived in `ReframeUi.h` from the kernel, and pinned by tests
tagged `[signs]` so they cannot silently invert). The chain is `osvViewRay`
(+x view = right on screen, +z = up) then `VirtualCamera::rotation()` =
`Rz(pan) * Rx(tilt) * Ry(roll)` then `lon = atan2(d.x, d.y)`,
`lat = asin(d.z)`. Feeding the frame centre through it gives `lon = -pan`
and `lat = +tilt`. So "the world follows the cursor" means:

* drag **right** - content from smaller longitude must reach the centre, so
  **Pan increases**;
* drag **down** - content from higher latitude must reach the centre, so
  **Tilt increases**;
* drag the ring **clockwise** - **Roll decreases** (Roll is right-handed
  about the view axis, which turns the *image* counter-clockwise);
* drag a grip **down** - **FOV increases** (pull the corner outward, see
  more).

Tilt clamps to +-90 (the geometry clamps there anyway, so letting the control
run past makes the picture stick while the number climbs). FOV clamps to the
parameter's **valid** range 10..350, not the slider's 30..180 - the presets
themselves live outside the slider range. Pan and Roll are deliberately
**unbounded** so a full turn stays keyframeable; a ring drag across the
+-180 boundary takes the shortest signed difference, so it cannot bake a
360-degree spin into the timeline.

**Shift axis lock.** The axis is chosen once, when travel from the anchor
first exceeds 3 px, and then held for the rest of the gesture. Re-deciding
every mouse move would swap axes under the hand; below the threshold nothing
moves at all rather than jittering between the two.

#### How keyframes get recorded

There are two APIs that look like they change a parameter and only one of
them does.

* `PF_ParamUtilsSuite3::PF_UpdateParamUI` (`AE_EffectSuites.h:203-239`) is
  documented as *"These changes are cosmetic only, and don't go into the undo
  buffer"*, and the exhaustive list of fields it may touch - `ui_flags`,
  `ui_width`, `ui_height`, `name`, `slider_min`/`max`, `precision`,
  `display_flags` - contains **no value field**. It cannot set Pan. Using it
  would give an overlay that appears to drag and records nothing.
* `PF_ChangeFlag_CHANGED_VALUE` (`AE_Effect.h:2371-2384`) is documented as
  *"Set this flag for each param whose value you change when handling a
  `PF_Cmd_USER_CHANGED_PARAM` or specific `PF_Cmd_EVENT` events
  (`PF_Event_DO_CLICK`, `PF_Event_DRAG`, & `PF_Event_KEYDOWN`) ... These
  changes are undoable and re-doable by the user"*, with the requirement *"If
  set during `PF_Cmd_EVENT`, be sure to also set `PF_EO_HANDLED_EVENT` before
  returning."*

So the overlay uses the second, exactly as documented. On each
`PF_Event_DRAG` it

1. writes the new value into the `PF_ParamDef` the host handed us
   (`u.ad.value`, PF_Fixed 16.16 degrees, for the three angles;
   `u.fs_d.value` for the FOV slider);
2. sets `params[i]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE` for **every
   parameter whose value moved and for no others** - marking an untouched
   parameter would make the host record a spurious keyframe on it, which is
   exactly the "why is there a Roll keyframe?" bug that makes an
   auto-keyframing UI infuriating;
3. sets `extra->evt_out_flags |= PF_EO_HANDLED_EVENT`.

The host then commits the value **at the current time**. With the stopwatch
running that is precisely what creates or updates a keyframe - the same code
path a typed-in number takes - so keyframing is automatic and needs no
keyframe API of our own. With the stopwatch off, the same write moves the
constant value. Both behaviours fall out without a special case.

`PF_Event_DO_CLICK` sets `send_drag = TRUE` (without it the host sends no
drags and nothing moves) and stores a slot index plus a generation counter in
`continue_refcon`, the four `A_intptr_t` words the host carries through the
gesture. A `DragState` does not fit in 32 bytes, so it lives in a small fixed
process-wide table; the generation counter means a stale or foreign refcon is
**rejected** rather than resuming somebody else's gesture. The click itself
changes no value - a click without movement must not record a keyframe.

#### Drawing

`PF_Event_DRAW` acquires `PF Effect Custom UI Suite` v2 for
`PF_GetDrawingReference`, then the DrawBot Draw / Supplier / Surface / Path /
Pen suites directly through `SPBasicSuite` (not through the SDK's
`AEFX_AcquireDrawbotSuites`, which never acquires the Pen suite and writes a
diagnostic into `out_data->return_msg` - a modal error dialog on a path that
should degrade silently).

The HUD is a thin centre crosshair **with a gap in the middle** so the exact
centre pixel is never covered, two roll-ring arcs straddling the horizontal
(drawn from the same constants the hit-test uses, so the drawn shape and the
grab target cannot drift apart), four corner grips drawn as "L" strokes
rather than boxes (a box reads as a crop rectangle, an L reads as a resize
grip), and a `Pan / Tilt / Roll / FOV` readout in degrees. Nothing is ever
filled: `PaintRect` and `FillPath` are never called on the picture, and a
test asserts that.

Legibility over both bright and dark footage comes from drawing every stroke
twice - a dark 1 px pass under a 1 px light pass, offset by translating the
surface rather than duplicating the path, so the two are provably the same
shape. Alpha stays well below 1 so the HUD never competes with the picture.
`PF_Event_ADJUST_CURSOR` sets `PF_Cursor_PAN`, `PF_Cursor_CROSS_ROTATE` or
`PF_Cursor_SCALE_DIAG_LR` from the same hit-test, and remembers what is under
the pointer so the next repaint highlights that handle (`PF_DrawEventInfo`
carries no pointer position of its own). `PF_EI_DONT_DRAW` is honoured: when
the host has hidden overlays, nothing is drawn.

#### What the SDK does not allow

**There is no mouse-wheel event, so there is no scroll-to-zoom.** The
complete event list is `AE_EffectUI.h:103-117` - `NEW_CONTEXT`, `ACTIVATE`,
`DO_CLICK`, `DRAG`, `DRAW`, `DEACTIVATE`, `CLOSE_CONTEXT`, `IDLE`,
`KEYDOWN_OBSOLETE`, `ADJUST_CURSOR`, `KEYDOWN`, `MOUSE_EXITED` - and none of
them carries a wheel delta. The only wheel-ish field anywhere in the event
structures is `PF_StylusEventInfo::stylus_wheelF`, which belongs to
`PF_PointerEventInfo`, a stylus struct that is **not reachable from
`PF_EventUnion` at all**. Faking it would mean installing a Windows mouse
hook behind the host's back, so it is not implemented. `Ctrl` + drag is the
zoom shortcut instead.

Also not available: there is no way to ask the host to *create* a keyframe
directly - `PF_ParamUtilsSuite3` can only read keyframes
(`PF_CheckoutKeyframe`, `PF_GetKeyframeCount`), never write one. Committing a
value at the current time, as above, is the only supported route, and it is
the one the host's own controls use.

#### Failure behaviour

Every degradation is "no overlay, log one line, return `PF_Err_NONE`", never a
crash and never a blocked render. A missing `SPBasicSuite`, a missing custom
UI suite, a missing DrawBot suite, a null drawing reference, a failed
`NewPen`/`NewPath`, a degenerate viewport and a full drag table are each
handled and each covered by a test. The whole `PF_Cmd_EVENT` body is inside a
`try`/`catch` because an exception unwinding into Premiere's C stack is
undefined behaviour. Every DrawBot object is released through an RAII scope,
and a test runs five repaints and asserts the live-object count is zero.

## Verification

0. `osv_premiere_common_tests` (Catch2 v3, `tests/premiere/common/`) tests the
   harness and the shared layer before any plug-in exists: suite refcounting
   through the mock `SPBasicSuite`, PPix row bytes and the bottom-left origin,
   the PPix cache as an LRU keyed on the prefs bytes, String round trips, Time
   ticks (254016000000/s; one 59.94 fps frame is exactly 4237833600 ticks),
   Video Segment keyframe interpolation (floats blend, popups and checkboxes
   hold), Sequence Info VR configuration, Error events, GPU alloc/free and
   `CreateGPUPPix` (`[cuda]`, skipped with no device), `PrefsBlob`
   defaults/sanitise/static asserts, `PixelCopy` round trips with positive and
   negative row bytes, `PluginLog` writing and rotation, and the `DelayLoad`
   hook resolving a DLL beside the test executable.
1. `osv_importer_tests` (`tests/premiere/importer/`) loads the built `.prm`
   with `LoadLibraryW` - never links its objects - and plays host:
   * module and registration: `xImportEntry` is the only exported entry
     point; `imInit` returns `imIsCacheable` and sets the documented
     capability flags; `imGetIndFormat` index 0 is `'OSV_'` with the
     `"osv\0lrf\0\0"` extension list and `xfIsMovie`, index 1 is
     `imBadFormatIndex`; the three `imGetSupports*` selectors answer with
     their `mal*` codes; an unknown selector (and `imGetColorSpaceFromOpaqueData`,
     23.3) returns `imUnsupported`; every selector survives a null record.
   * lifetime: the sample clip opens and closes with the host's handle count
     back where it started; a non-OSV file returns `imBadFile` with the OS
     handle closed (proved by deleting the file afterwards); a missing file
     allocates no privateData; `imQuietFile` keeps privateData and a frame
     request afterwards transparently re-opens the decoders.
   * `imGetInfo8` / `imGetInfo9`: 6000 x 3000 native, 2:1, `alphaStraight`,
     10-bit, `'hvc1'`; the frame period is exactly 4237833600 ticks (the
     assertion that catches a rate computed from a rounded double);
     65 frames; the four VR fields; stereo 48 kHz audio with a plausible
     duration; `kSeparateSequentialAudio`; the prefs change the advertised
     size (native / 4K / 2K); stream index 1 ends the enumeration.
   * formats and sizes: `imGetIndPixelFormat` lists 32f then 8u then
     `imBadFormatIndex`; `imGetPreferredFrameSize` enumerates native, half
     and quarter with `imIterateFrameSizes`; `imSelectClipFrameDescriptor`
     keeps a supported format, coerces an unsupported one, snaps an odd size
     to the nearest advertised one, answers 8u for
     `kMaxBitDepth_Off` through the v2 record, and refuses the v2 selector
     on a host reporting `IMPORTMOD_VERSION_23`.
   * colour: `imGetIndColorSpace` declares
     `kPrOverranged2100PQ` / `kPrOverranged2100HLG` / `kPrOverranged709` per
     prefs and ends at index 1; a `selectedColorProfileName` of
     `"BT.709 RGB Full"` makes the frame come back bit-identical to an
     explicit Rec.709 render and clearly different from the PQ one; a frame
     created with a non-zero `opaqueColorSpaceIdentifier` is tagged with it.
   * frames: frame 0 at native size is non-black, finite, in [0, 1], over
     90 % fully opaque (alpha is straight lens coverage, transparent only
     where the occlusion polygon hides a direction) and the right way up
     (sky in the top rows, ground in the bottom, with stabilisation off -
     the ground truth measured with `osvtool render --mode equirect --stab
     off`); a half-size request renders directly at that size; 8u and 32f
     agree within 1/255; PQ, HLG and 709 differ; the host cache hits on the
     second identical request, hands back the same handle, and misses on a
     single changed prefs byte; a frame time a few ticks short of a boundary
     still resolves to that frame, and times past the end or below zero
     clamp.
   * audio: `imImportAudio7` writes every channel with finite, non-silent
     samples; a random read and the sequential walk over the same range are
     bit-identical (the assertion that found both the swresample delay and
     the AAC decoder state that `avcodec_flush_buffers` does not clear);
     reads past the end are zero-filled; `imGetAudioChannelLayout` is
     `imUnsupported` for the 2-channel clip.
   * prefs: the two-step `imGetPrefs8` protocol (size, then buffer), a
     too-small buffer is refused rather than overrun, a valid blob round
     trips unchanged, and `imGetInstancePrefs` reaches the live instance
     (proved by `imGetInfo8` then reporting the new size).
     `test_prefs_mapping.cpp` links `plugins/importer/PrefsMapping.cpp`
     directly and walks every value of every prefs enum through
     `controlsFromPrefs` / `prefsFromControls`, plus hostile inputs
     (`CB_ERR`, out-of-range indices, infinities and NaN in the exposure
     field) - the dialog itself is modal and cannot be driven from a test,
     which is exactly why the mapping is a separate, pure translation unit.
   * timing (`[!benchmark]`): reports ms per 6000 x 3000 frame on the CPU and
     CUDA backends rather than asserting a threshold, which would only fail
     on a slower machine.
2. `osv_reframe_tests` (`tests/premiere/reframe/`) loads the built `.aex`
   with `LoadLibraryW` - never links its objects - and drives it through the
   mock host, so what is tested is what Premiere sees:
   * module: `EffectMain` and `xGPUFilterEntry` both exported; unknown
     selectors answered with `PF_Err_NONE`; `PF_Cmd_ABOUT` fills a message.
   * PiPL: the `'PiPL'` resource 16000 is read back out of the module with
     `FindResourceW` and parsed, and its match name, display name, category,
     entry-point name, version word and both out-flag words are compared to
     `ReframeParams.h` - the same header that generated them. The VERSIONINFO
     block is checked too.
   * `PF_Cmd_GLOBAL_SETUP`: `out_flags` / `out_flags2` equal the PiPL words
     bit for bit, all four of `PrPixelFormat_BGRA_4444_32f`, `_32f_Linear`,
     `_16u` and `_8u` are registered through the PF Pixel Format Suite **in
     that order** (asserted positionally, because the order is a preference
     ranking) when `appl_id == 'PrMr'` and nothing is registered otherwise,
     every acquired suite is released.
   * pixel formats (`[format16u]`, `[formatlinear]`, `[format]`) - the
     regression suite for the "plays but does not update when I step" bug,
     every case of which fails against the old two-format code:
     a `PF_Cmd_RENDER` with **both** worlds `BGRA_4444_16u` (which is what a
     10-bit sequence actually hands us) succeeds and aims where it is told,
     decoded from the labelled panorama exactly as the 32f cases are;
     the 16u render matches the 32f render within **one** step of the 0..32768
     scale - a bound ~128x tighter than the 8-bit one, so a store using 65535
     or 255 as white misses it by orders of magnitude and cannot pass by luck
     - with a guard that neither render was simply black;
     a 16u **input** reproduces the float panorama it was quantised from;
     `_32f_Linear` is accepted as input, as output and as both, and is
     **byte-identical** to the `_32f` render;
     an unknown format (`VUYA_4444_32f`, relabelled onto a good buffer via the
     mock's `setWorldFormat` so only the label differs, and the same 16-byte
     width so nothing but the format check stands between us and misreading
     luma as blue) is refused with `PF_Err_BAD_CALLBACK_PARAM` **every** time,
     not just the first;
     and - the regression proper - with the PF Pixel Format Suite **hidden**
     for the whole of `GLOBAL_SETUP` and `PARAMS_SETUP`, so nothing is
     registered exactly as on the failing host, a subsequent render into 16u
     still succeeds, is still correctly aimed, and the render-time retry has
     registered the full four-format list. No suite must not mean no picture.
   * `PF_Cmd_PARAMS_SETUP`: 15 parameters (13 controls + 2 group
     terminators) with the permanent ids, types,
     names, popup item strings, slider ranges and defaults of the table
     above, and `PF_ParamFlag_SUPERVISE` on exactly Output Aspect, Preset,
     Tilt, FOV and Distortion.
   * `PF_Cmd_USER_CHANGED_PARAM`: every preset writes FOV / Distortion /
     Tilt with `PF_ChangeFlag_CHANGED_VALUE` and touches nothing else,
     "Custom" writes nothing, editing FOV / Distortion / Tilt flips Preset to
     Custom (and does not re-flag it when it is already Custom), a garbage
     popup value and a null `extra` are survived.
   * geometry: `resolveAspectRatio` and `computeViewport` for every aspect,
     including the 16:9 fallback and degenerate ratios; `buildParams` rejects
     null / zero-sized / 8-bit / short-pitch sources and clamps NaN angles,
     out-of-range tilt and an un-invertible field of view.
   * `PF_Cmd_RENDER` on a synthetic labelled equirect (1024 x 512; longitude
     encoded as a cos/sin pair so the label is continuous across the seam)
     lands the expected direction at the expected pixel for pan 0 / 90 / 180
     / -90 and tilt +-45, the letterbox alpha is 0 outside and 1 inside for
     every aspect, "Match Sequence" follows the Sequence Info Suite and falls
     back to 16:9 without it, 8u and 32f agree within 1/255, 16f agrees to
     half precision, a negative destination pitch writes the same picture,
     and the CPU function agrees with `osvReframeEquirectPixel` to 1e-5.
   * GPU (`[cuda]`, skipped with no device): `xGPUFilterEntry` startup fills
     all five callbacks and reports the effect's match name, declines
     interface version 1 and a non-zero index, `CreateInstance` declines a
     host with no GPU Device Suite or no Video Segment Suite and falls back
     through the Video Segment versions, 32 create/dispose cycles leak no
     suite reference, and `Render` on a device buffer through the real
     driver-API context the mock GPU Device Suite provides is compared to the
     CPU render: PSNR >= 60 dB for 32f and >= 45 dB for 16f. Keyframed
     parameters are read at the render time and two concurrent instances
     render independently.
   * Keyframe Easing (`test_easing.cpp`, `[easing]`): every curve starts at
     0 and ends at 1, is monotone with its speed never negative, has the
     documented speed profile (checked against a central difference too),
     is point-symmetric or mirrored as documented, and survives NaN / inf
     input; None, Linear and Linear Smooth are the identity in time. On a
     synthetic keyframe set each preset lands on its value halfway, keeps the
     host's value outside the keyframes and with one keyframe, and Linear
     Smooth is exactly linear on straight runs and flat through a peak. The
     backwards keyframe search agrees with a brute-force scan on 2,000 times
     over 300 random keyframes (at most 14 host calls a search) and refuses a
     host that answers out of order, past its call budget or by throwing. The
     popup is registered last, id 22, None by default, not animatable, and
     the parameter probe maps it on the compact list (a list without it reads
     None). Through the loaded module, `PF_Cmd_RENDER` of every preset is
     byte-identical to a render of the expected constant pan (keyframes 0 ->
     16 deg, halfway: Fast In / Slow Out 13, Slow In / Fast Out 3, the other
     four 8), Classic FOV and Tilt ease the same way, and None renders the
     host's value with zero `PF_FindKeyframeTime`
     calls; `Render` on the GPU matches the CPU picture of the eased pan at
     >= 60 dB on hosts that number popups from 1 and from 0 (and does not
     match the linear one), walks Linear Smooth's neighbouring keyframes, and
     makes zero `GetNextKeyframeTime` calls for None. DJI Studio's four
     Manual Framing read-outs (Zoom 45.5 / 101.4 / 221.5 / 256.6) lie on
     the zoom path the effect walks (`test_dji_camera.cpp`).
3. `osv_source_settings_tests` (`tests/premiere/sourcesettings/`) loads the
   built `OpenOSVSourceSettings.aex` with `LoadLibraryW` - never links its
   objects - and drives it through the mock host:
   * module: `EffectMain` exported; unknown selectors (including
     `PF_Cmd_RENDER`, which must never arrive and must still not crash when it
     does) answered with `PF_Err_NONE`; every selector survives null
     `in_data` / `out_data` / params; `PF_Cmd_ABOUT` fills a message that
     actually mentions the keyframe constraint.
   * PiPL: resource 16000 is read back with `FindResourceW` and parsed, and
     its kind, display name, category, entry-point name, PiPL / spec / effect
     version words, info flags, reserved word and both out-flag words are
     compared to `SourceSettingsParams.h`. **The match name is compared to
     `kSourceSettingsMatchName`** from `plugins/common/SourceSettingsIdentity.h`
     - the same constant `ImporterVideo.cpp` copies into
     `sourceSettingsMatchName` - and the narrow and wide spellings are checked
     against each other. Resource 16001 is asserted absent (one PiPL per
     module). The VERSIONINFO block is checked too.
   * `PF_Cmd_GLOBAL_SETUP`: `out_flags` / `out_flags2` / `my_version` equal
     the PiPL words bit for bit; `SetIsSourceSettingsEffect(TRUE)` **was
     called** when `appl_id == 'PrMr'` and was **not** called otherwise
     (`'FXTC'`); a host with the suite hidden still succeeds and still reports
     the flags; every acquired suite is released.
   * `PF_Cmd_PARAMS_SETUP`: 13 parameters (9 controls + 4 group markers) with
     the permanent ids, types, names, popup item strings and slider ranges of
     the table above; `PF_ParamFlag_CANNOT_TIME_VARY` on **every** one of the
     nine value-carrying controls; `START_COLLAPSED` on Advanced and not on
     Stitching; the parameter list walked with a nesting depth so both groups
     are proved balanced, non-nested, and holding exactly the intended
     controls; every popup's item count equals its `PrefsBlob` enum's `Count`
     and its item string has that many entries; the Exposure slider's valid
     range equals the blob's clamp range and its slider range sits inside it;
     and **every default equals `PrefsBlob::defaults()`** field by field.
   * `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS`: the blob is valid, sanitises to
     itself unchanged (nothing to repair, not merely repairable), and is
     byte-identical to `PrefsBlob::defaults()` for untouched controls; every
     value of every popup, both states of both checkboxes and seven exposure
     values round-trip; all nine controls moved at once round-trip together
     (so a field read from the wrong index cannot hide behind a neighbour's
     default); a buffer one byte too small is refused and **not written at
     all**; a larger buffer is accepted with its tail untouched; a null
     `extra` and a null `prefsPC` are survived; hostile popup values
     (0, -1, -12345, 99, 1000000) still produce a valid blob and fall back to
     the **default** rather than to enum value 0; an out-of-range exposure is
     clamped and NaN / +-infinity reset to zero.
   * `PF_Cmd_SEQUENCE_SETUP`: `PerformSourceSettingsCommand` is actually
     called, exactly once, with `sizeof(PrefsBlob)` and with a valid blob
     seeded into the buffer; the importer's reply is written into all nine
     controls and translating them back reproduces that blob exactly; a host
     that replies with nothing leaves the controls alone; a **failed** call
     leaves them alone even with a poison reply staged (proving the error is
     checked before the buffer is read); a reply that fails `isValid()` is
     rejected; `sequence_data` is forced non-null by the test and every
     sequence selector clears it; and nothing is called at all under `'FXTC'`.
   * the pure mapping (`SourceSettingsMapping.cpp`, compiled in directly):
     every combination of colour x size x stabilisation x device and of
     calibration x fit x seam x gain round-trips; the default controls give
     `PrefsBlob::defaults()` and vice versa; a blob with a wrong magic or a
     wrong version word yields the defaults rather than being reinterpreted;
     and for seven hostile control values including `INT_MIN` / `INT_MAX` the
     result is always valid, always sanitises to itself, and always has zeroed
     reserved bytes (they are part of the PPix cache key).
4. `scripts/install_plugins.ps1` then a Premiere launch: the three plug-ins
   appear in `%APPDATA%\Adobe\Premiere Pro\26.0\Plugin Loading.log` as
   successfully loaded, the registry cache has a `GPUVideoFilter.0` entry with
   the reframe match name, importing the sample clip shows the stitched size,
   59.94 fps and VR Properties pre-filled, the Effect Controls panel shows
   "OpenOSV Source Settings" on the master clip with nine controls and no
   stopwatches, `File > New > Sequence > OpenOSV` lists the three presets, and
   the reframe effect renders in the Program Monitor with the GPU badge.

## Verification results (2026-09-16, integration build)

Both plug-ins built together from one configure of the canonical preset
`windows-msvc-premiere-release`, on Windows 11 with MSVC 14.44, CUDA 12.9.86
and an NVIDIA GeForce RTX 5090 (driver 616.56).  The Premiere SDK reports
IMPORTMOD_VERSION 24 (Premiere 23.2) and the AE SDK effect spec 13.29.

### Build

| Preset | Result | Warnings |
|---|---|---|
| `windows-msvc-premiere-release` (importer ON, reframe ON) | 288/288 targets, exit 0 | 0 |
| `windows-msvc-cuda-release` (library only, no SDK targets) | exit 0 | 0 |
| `windows-msvc-cpu-only` (no CUDA, no OpenCL) | exit 0 | 0 |

`/W4` with `OSV_WARNINGS_AS_ERRORS`; a `warning C####`, `warning LNK` and
`warning MSB` grep over each full build log returns zero.  The two plug-in
agents developed in separate build directories, and the first combined
configure and build of their work linked with no duplicate symbols, no option
conflicts and no stage-directory collision.

### Tests

| Suite | Result |
|---|---|
| `osv_tests` (library, premiere preset) | 5500334 assertions in 167 test cases |
| `osv_tests` (library-only preset) | 5500334 assertions in 167 test cases |
| `osv_tests` (CPU-only preset) | 5500276 assertions in 164 test cases |
| `osv_premiere_common_tests` | 2666 assertions in 30 test cases |
| `osv_importer_tests` | 1222009 assertions in 38 test cases |
| `osv_reframe_tests` | 13095 assertions in 72 test cases |
| `ctest --preset premiere` | 100% passed, 0 failed, out of 307 |

The CPU-only preset is three cases short of the other two because the
GPU-tagged cases are compiled out with no backend, which is the intended
behaviour, not a skip.  `ctest` reports no test as Not Run or Skipped: the
`[cuda]` cases execute on the 5090 for real (9 cases / 245 assertions in
`osv_reframe_tests`, 1 case / 74 assertions in `osv_premiere_common_tests`).

### Measured numbers

* GPU vs CPU parity in the effect: **151.944 dB** PSNR in 32f (budget 60 dB)
  and **114.537 dB** in 16f (budget 45 dB).
* Importer, 6000 x 3000 stitched frame, decode + stitch + copy:
  **198.8 ms** CUDA against **580.9 ms** CPU, a 2.9x speed-up.

### Stage directory

`build/windows-msvc-premiere-release/plugins/OpenOSV/` holds the two modules
and exactly the eight runtime DLLs they need between them:

| File | Bytes | Needed by |
|---|---|---|
| `OpenOSVImporter.prm` | 1688064 | - |
| `Open360Reframe.aex` | 1426944 | - |
| `avcodec-63.dll` | 13904896 | importer |
| `avformat-63.dll` | 2569216 | importer |
| `avutil-61.dll` | 892416 | importer (also via avcodec/avformat/swresample) |
| `swresample-7.dll` | 123904 | importer (also via avcodec) |
| `z.dll` | 91136 | importer (via avcodec/avformat) |
| `OpenCL.dll` | 56320 | importer |
| `spdlog.dll` | 286208 | importer |
| `fmt.dll` | 131072 | importer (via spdlog) |

`dumpbin /dependents` confirms neither module carries an unexpected direct
import.  The importer imports only `USER32`, `KERNEL32`, `MSVCP140`,
`VCRUNTIME140(_1)` and the `api-ms-win-crt-*` set, and delay-loads all seven
third-party DLLs (`avformat-63`, `avcodec-63`, `swresample-7`, `avutil-61`,
`OpenCL`, `spdlog`, `fmt`).  The effect imports the same OS/CRT set and
delay-loads `nvcuda.dll` and nothing else - its CUDA kernel is an embedded
fatbin (`.nv_fatb` / `.nvFatBi` sections) and cudart is linked statically, so
there is no cudart DLL to collide with the host's.

`dumpbin /exports` shows exactly the entry points each host looks for:
`xImportEntry` on the importer, `EffectMain` and `xGPUFilterEntry` on the
effect.  Both also export the data symbol `NvOptimusEnablementCuda`, a
harmless `cudart_static` artefact.

Two facts worth recording, because both look wrong at a glance and are not:

* **`fmt.dll` and `spdlog.dll` are staged twice**, once by each module's
  post-build step, because the two modules share one stage directory.  The
  copies are byte-identical to the vcpkg originals (MD5 verified), so the
  second copy overwrites the first with the same bytes; there is no version
  skew.
* **The effect does not need `fmt.dll` or `spdlog.dll`** even though they sit
  beside it.  They reach the directory only through the importer.  The build
  log notes this from the other direction as "declared but unused /DELAYLOAD
  entries" for the effect.  Proved by copying `Open360Reframe.aex` alone into
  an otherwise empty directory and loading it: `LoadLibraryExW` succeeds and
  both `EffectMain` and `xGPUFilterEntry` resolve.

### Install

`scripts/install_plugins.ps1 -StageDir build\windows-msvc-premiere-release\plugins\OpenOSV`
run from an already-elevated shell copied 2 modules and 8 DLLs into

    C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\

and exited 0.  No UAC prompt was needed and none had to be brokered through
`Start-Process -Verb RunAs`.  All ten installed files hash-match the staged
originals (SHA-256), and both installed modules load from that folder with
their entry points resolving, so the deployment is complete and self-contained.

Premiere Pro was **not** launched, so everything the host does before calling
a function remains unverified: whether it accepts the PiPL, whether the
`GPUVideoFilter.0` registry entry appears, and whether the clip imports with
VR Properties pre-filled.  Verification step 3 above is still outstanding and
is the one thing a human has to do.

## Known limits (milestone 2)

* The Program Monitor overlay is implemented and unit-tested against the mock
  host, but Premiere itself was never launched, so whether the live host
  delivers `PF_Cmd_EVENT` to a `PF_CustomEFlag_COMP` custom UI on this build
  is unverified. If a Premiere 25.6.x DrawBot regression suppresses the
  drawing, the effect still renders and the Effect Controls still keyframe -
  the overlay degrades to nothing and logs one line.
* There is no scroll-wheel zoom, because the SDK has no mouse-wheel event.
  See "Program Monitor overlay" for the evidence; `Ctrl` + drag is the
  substitute. (A Windows mouse hook was considered and deliberately not
  built: `Ctrl` + drag does the job and a global hook in a host process is
  the kind of fragility this plug-in avoids.)
* Keyframe Easing reads keyframes through `PF_FindKeyframeTime` on the CPU
  path and `GetNextKeyframeTime` on the GPU path; both are exercised against
  the mock host, not yet against a live Premiere. Two facts to confirm on the
  first live run: that Premiere answers `PF_FindKeyframeTime` for an effect's
  own parameters, and that `GetNextKeyframeTime` uses the same index space as
  `GetParam` (the probed map). Either failing leaves Premiere's own
  interpolation, logged once.
* No LRF-as-proxy attach automation; `.lrf` imports as its own clip.
* A `.LRF` proxy now opens AND renders. The fix is `meta::FormatInfo::lensW()`
  / `lensH()`: the proxy is one 2048 x 1024 side-by-side track holding two
  1024 x 1024 fisheye halves, `video::DualStreamReader` already split it, but
  every rig builder was feeding `geom::StreamScaling::derive` the WHOLE TRACK,
  so `render::RenderParamsBuilder` rejected each frame with "frame size does
  not match the rig (1024x1024 vs 2048x1024)". Both rig builders
  (`ImporterInstance::rebuildRig` and `tools/osvtool/Pipeline.cpp`) now use the
  per-lens accessor. `geom::LensRig::build` additionally refuses a
  `digital_focal_length` that disagrees with `calibration * scale` by more than
  a factor of 1.2 and falls back to the scaled calibration, because the proxy's
  ClipMeta repeats the 6K camera's 829.36 px verbatim for a 1024 px stream;
  taken at face value the fisheye circle collapsed to a disc. The 6K clip is
  unaffected (the two agree to 0.01 %).
* The `.LRF`'s colour is still wrong: it is an 8-bit stream tagged
  `DLogM (19)` in its own StreamMeta, and applying the D-Log M curve to it
  produces a magenta cast. The GEOMETRY is correct and matches the `.OSV`
  exactly; only the transfer/matrix handling for the 8-bit proxy is
  outstanding. Unverified and untouched by this work.
* Windows on ARM builds are not produced (no CUDA); the CPU path would work.
