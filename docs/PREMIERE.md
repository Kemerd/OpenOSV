# Premiere Pro plug-ins (milestone 2)

OpenOSV ships two native Premiere Pro plug-ins for Windows. Together they give
the same workflow as DJI's macOS-only "Reframe for Adobe Premiere": drop an
`.OSV` on the timeline, get a stitched 360 clip, add one effect to reframe it
with host keyframes and GPU acceleration. Both are built from this repository
against the Adobe SDKs (never committed) and share the milestone 1 library.

| Binary | Kind | Entry point | What it does |
|---|---|---|---|
| `OpenOSVImporter.prm` | standard file importer | `xImportEntry` | Registers `.osv` and `.lrf`; decodes both lenses with FFmpeg, stitches with the CUDA / OpenCL / CPU renderer and hands Premiere an equirectangular 360 x 180 frame in `BGRA_4444_32f`, colour-tagged Rec.2100 PQ, HLG or Rec.709. Decodes the AAC track. Declares the clip as monoscopic equirectangular VR. Per-clip options live in a Source Settings dialog (right-click > Source Settings). |
| `Open360Reframe.aex` | After Effects API effect + `PrGPUFilter` | `EffectMain`, `xGPUFilterEntry` | "Open 360 Reframe" in the Effects panel (bin "OpenOSV"). Host-keyframed Pan / Tilt / Roll / FOV / Distortion plus preset perspectives. Renders on the GPU through Premiere's own CUDA device (driver API, embedded fatbin) and falls back to a 32-bit float CPU path that runs the same kernel. |

Install both (plus their runtime DLLs) in
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\`; Premiere Pro,
Media Encoder and After Effects all scan that folder.

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
  CMakeLists.txt              options OSV_PREMIERE_IMPORTER / OSV_PREMIERE_REFRAME (default ON), adds the three below
  common/                     osv_premiere_common (static)
    HostSuites.h/.cpp         RAII acquire/release of SweetPea suites by name+version
    PluginLog.h/.cpp          file log (%LOCALAPPDATA%\OpenOSV\<plugin>.log) + OutputDebugString
    DelayLoad.h/.cpp          delay-load hook: resolve avcodec/avformat/... /OpenCL.dll from the plug-in's own folder
    PixelCopy.h/.cpp          RGBA float top-down  ->  BGRA 32f / 8u bottom-up with row bytes (SIMD friendly loops)
    HostContext.h/.cpp        process-wide lazily created renderer pool + thread pool, torn down explicitly
    PrefsBlob.h               the importer prefs struct (shared with the source settings dialog)
  importer/                   OpenOSVImporter.prm
    ImporterEntry.cpp         xImportEntry dispatch, DllMain, imInit/imShutdown, open/quiet/close, privateData handle
    ImporterInstance.h/.cpp   per-clip state (privateData): reader, rig, colour, stabilisation, frame + analysis caches, mutex
    ImporterVideo.cpp         imGetInfo8/9, pixel formats, frame sizes, clip frame descriptors, colour spaces, imGetSourceVideo, imAnalysis
    ImporterAudio.h/.cpp      AudioDecoder: its own AVFormatContext, AAC -> planar float, exact random + sequential positioning
    ImporterAudioSelectors.cpp  imImportAudio7 / imResetSequentialAudio / imGetSequentialAudio / imGetAudioChannelLayout
    PrefsMapping.cpp          the PURE PrefsBlob <-> dialog-control mapping and the colour-space tokens (compiled into the tests too)
    SourceSettingsDialog.cpp  Win32 modal dialog shown from imGetPrefs8 / imGetInstancePrefs
    ImporterPlugin.h          identity constants, privateData accessors, handler declarations
    OpenOSVImporter.rc        IMPT resource + DIALOGEX template + VERSIONINFO
    resource.h                resource and control IDs shared by the .rc and the dialog code
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
                              kernel on the library ThreadPool; also compiled into osv_reframe_tests
    DllMain.cpp               arms the delay-load hook; nothing else (loader lock)
tests/premiere/               mock-host harness (see Verification); added from plugins/CMakeLists.txt because every target needs the SDK targets
  mockhost/                   osv_premiere_mockhost (static, Catch2-free): fake piSuites + SPBasicSuite serving our own suite implementations
    MockHost.h/.cpp           public API, suite registry, legacy memory callbacks, inspection
    MockHostImpl.h            internal state shared by the suite files
    MockPPix.cpp              PPix v1, PPix2 v3, Creator v1, Creator2 v4, Cache v8/v7 (real LRU)
    MockSuites.cpp            Time, String, App Info, Error, Color Management, Memory Manager, Importer File Manager, Sequence Info v5..v9, Video Segment v6..v9
    MockGpu.cpp               GPU Device Suite v2 on a real CUDA driver-API context (stub without CUDA)
    MockAe.cpp                PF Pixel Format v1, PF Utility v4..v13, PF_InData/PF_OutData builders, effect worlds
  common/                     osv_premiere_common_tests (mock host + plugins/common)
  importer/                   osv_importer_tests: LoadLibraryW on the built .prm, driven through the mock host
    ImporterHarness.h/.cpp    loads the module, plays host (imInit/imShutdown, ClipHandle RAII, request builders)
    test_importer.cpp         registration, open/quiet/close, imGetInfo8/9, formats, sizes, colour, frames, audio, prefs, timing
    test_prefs_mapping.cpp    the pure prefs <-> controls mapping (links plugins/importer/PrefsMapping.cpp directly)
  reframe/                    osv_reframe_tests (loads Open360Reframe.aex)
scripts/install_plugins.ps1   copy stage dir -> MediaCore\OpenOSV (-StageDir, -Destination, -Uninstall, -NoElevate, -Force), self-elevating, prints the Plugin Loading.log path and the Shift-launch hint
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
* `imQuietFile`: close the handle, release decoders, renderer references and
  device memory, keep parsed metadata.
* `imCloseFile`: delete the instance, dispose privateData, release suites.
* `imShutdown`: destroy the process-wide `HostContext` (renderers, FFmpeg
  hardware contexts, thread pool). Never from `DllMain`.

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

* `imGetIndPixelFormat`: 0 -> `PrPixelFormat_BGRA_4444_32f`, 1 -> `PrPixelFormat_BGRA_4444_8u`, 2 -> `imBadFormatIndex`.
* `imGetPreferredFrameSize`: index 0 native, 1 half, 2 quarter (`imIterateFrameSizes`), then `imOtherErr`.
* `imSelectClipFrameDescriptor(2)`: return the desired descriptor unchanged
  except pixel format coerced to one of the two above and size to the nearest
  advertised size.
* `imGetIndColorSpace` index 0: `kPrSDKColorSpaceType_Predefined` with
  `ioProfileRec.outName` (String Suite) = `kPrOverranged2100PQ` ("BT.2100 PQ RGB Full"),
  `kPrOverranged2100HLG` or `kPrOverranged709` according to prefs; index 1 -> `imBadFormatIndex`.
  The tokens describe exactly what we emit: full-range 32f RGB signal codes.
  A compile-time switch (`OSV_IMPORTER_COLOR_SEI`) answers with
  `kPrSDKColorSpaceType_SEITags` `{9, 16|18|1, 0, 32, full, rgb, display}`
  instead, for the runtime comparison on real hosts.
* `imGetColorSpaceFromOpaqueData` (23.3, undocumented) and every unknown
  selector return `imUnsupported`, logged once.
* `imGetSourceVideo`: pick the first requested format we support (0 = any ->
  32f at native size), look the frame up in the PPix cache
  (`GetFrameFromCacheWithColorSpace`, key = importerID / stream / frame /
  quality / prefs blob), else decode + stitch, create with
  `PPixCreator2Suite::CreateColorManagedPPix(..., opaqueColorSpaceIdentifier)`
  (plain `CreatePPix` when the id is invalid), copy with `PixelCopy` honouring
  `GetRowBytes` and bottom-left origin, `AddFrameToCacheWithColorSpace`.
  If `selectedColorProfileName == kPrOverranged709` the host could not use the
  declared space: render with the Rec.709 transfer regardless of prefs.
  Draft: `inQuality <= kPrRenderQuality_Low` or a scrubbing/playing intent with
  `inPlaybackRatio < 1` disables seam search for that frame.
* Frame index = `inFrameTime / ticksPerFrame` with rounding, clamped.

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
| `colorOutput` | 0 PQ, 1 HLG, 2 Rec.709 | 0 |
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
initialise defaults when `magic` is wrong and show the modal Win32 dialog
(`DialogBoxParamW` on a DIALOGEX template in the `.rc`, owner = host main
window). Cancel returns `imCancel`. The blob is part of every PPix cache key so
changed settings never hit stale frames. `imGetInstancePrefs` mirrors it.

### Concurrency

One `std::mutex` per `ImporterInstance` serialises decode + render; frames
for different clips run in parallel. Renderers come from the process-wide
`HostContext` (one CUDA renderer per device, one OpenCL renderer, one CPU
renderer) and are internally serialised. No mutable globals besides that
context. FFmpeg runtime DLLs and `OpenCL.dll` are delay-loaded from the
plug-in folder so a machine without an OpenCL ICD still loads the importer.

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
  `PrPixelFormat_BGRA_4444_32f` then `PrPixelFormat_BGRA_4444_8u` through the
  PF Pixel Format Suite.

### Parameters (IDs are permanent; the INDEX is not the ID)

`PF_ADD_TOPIC` and `PF_END_TOPIC` each issue their own `PF_ADD_PARAM`
(`Param_Utils.h:298-320`), so a group terminator is a real parameter holding
a real index, and it sits in the MIDDLE of the list. There are therefore 15
parameters, not 13, and a control that follows a closed group has an index
one higher than its id per group already closed. `ReframeParams.h` spells the
index table out literally (`ParamIndex`, with `kParamIdByIndex` beside it)
rather than deriving it, and `EffectMain.cpp` static_asserts the
relationship. The GPU path's `GetParam(index - 1)` uses the same table, so
both halves read the same control.

| Index | ID | Name | Type | Range / items | Default |
|---|---|---|---|---|---|
| 1 | 1 | Output Aspect | popup | Match Sequence \| 16:9 \| 9:16 \| 1:1 \| 4:3 \| 3:4 \| 2.35:1 \| Full Frame | Match Sequence |
| 2 | 2 | Camera | topic (GROUP_START) | | |
| 3 | 3 | Preset | popup | Custom \| Crystal Ball \| Asteroid \| Wide \| Ultra Wide \| Dewarping | Wide |
| 4 | 4 | Pan | angle | unbounded | 0 |
| 5 | 5 | Tilt | angle | unbounded (clamped to +-90 in code) | 0 |
| 6 | 6 | Roll | angle | unbounded | 0 |
| 7 | 7 | FOV | float slider | valid 10..350, slider 30..180, tenths, degrees | 120 |
| 8 | 8 | Distortion | float slider | 0..100 %, slider 0..100 | 15 |
| 9 | 14 | (closes Camera) | GROUP_END | | |
| 10 | 9 | Source | topic (GROUP_START, starts collapsed) | | |
| 11 | 10 | Source Pan | angle | | 0 |
| 12 | 11 | Source Tilt | angle | | 0 |
| 13 | 12 | Source Roll | angle | | 0 |
| 14 | 15 | (closes Source) | GROUP_END | | |
| 15 | 13 | Smooth Keyframes | checkbox | | off |

"Output Aspect" and "Smooth Keyframes" are top-level siblings of the two
groups, which is only true because both groups close. A test walks the real
parameter list keeping a nesting depth and asserts exactly that.

Changing Preset (supervised, `PF_Cmd_USER_CHANGED_PARAM`) writes FOV,
Distortion and Tilt from the preset table and marks them
`PF_ChangeFlag_CHANGED_VALUE`; editing any of those flips Preset back to
Custom (and does nothing when it is already Custom, so a slider drag does not
fill the undo stack). `PF_Cmd_UPDATE_PARAMS_UI` greys nothing today; the
handler and the out-flag exist because adding either later would change the
PiPL and invalidate the plug-in cache on every installed machine.

`PF_ParamFlag_SUPERVISE` is set on Output Aspect, Preset, Tilt, FOV and
Distortion. Output Aspect does not act on the message yet - it carries the
flag for the same "do not change the PiPL later" reason.

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

### CPU path (`PF_Cmd_RENDER`)

`PF_Cmd_RENDER` receives `params[0]` (input layer) and `output` in the
registered Premiere format. Rows are addressed as `data + y * rowbytes` in
top-down order for both worlds (negative row bytes are legal). The same
`osvReframeEquirectPixel` runs per pixel on the library `ThreadPool`. An 8u
INPUT world is promoted once into a float scratch buffer
(`promoteBgra8uToFloat`, codes / 255) because the shared sampler reads float
or half only; an 8u OUTPUT world is quantised with rounding on write. The
effect advertises `PrPixelFormat_BGRA_4444_8u` in GLOBAL_SETUP, so it accepts
one rather than declining and hoping the host renegotiates.
Parameters come from `params[i]` at the render time, with the same
three-sample smoothing implemented through `PF_CHECKOUT_PARAM` at
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
     bit for bit, `PrPixelFormat_BGRA_4444_32f` then `_8u` are registered
     through the PF Pixel Format Suite when `appl_id == 'PrMr'` and nothing
     is registered otherwise, every acquired suite is released.
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
3. `scripts/install_plugins.ps1` then a Premiere launch: the two plug-ins
   appear in `%APPDATA%\Adobe\Premiere Pro\26.0\Plugin Loading.log` as
   successfully loaded, the registry cache has a `GPUVideoFilter.0` entry with
   our match name, importing the sample clip shows 6000 x 3000, 59.94 fps,
   VR Properties pre-filled, and the effect renders in the Program Monitor
   with the GPU badge.

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
  substitute.
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
