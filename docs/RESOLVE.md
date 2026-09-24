# DaVinci Resolve (OpenFX)

**Status: built and tested, but not yet run inside DaVinci Resolve**, on
Windows or on macOS. The plug-ins pass their tests against a strict mock
OpenFX host that loads the real `OpenOSV.ofx` and drives it the way a host
does (`tests/ofx`). Nobody on the project has run them in Resolve yet. Treat
this as a preview. If you try it, especially on a Mac, please report what you
see, or send a fix: see [Reporting a problem](#reporting-a-problem).

`OpenOSV.ofx.bundle` holds two OpenFX effects. Both show up in Resolve's
Effects Library under **OpenFX**, in the **OpenOSV** group:

| Effect | Kind | What it does |
|---|---|---|
| **OpenOSV Source** | Generator | Opens a DJI Osmo 360 `.OSV` (or its `.LRF` proxy) and stitches it with the Premiere importer's engine. It outputs either a reframed view or the whole 360 sphere. |
| **Open 360 Reframe** | Filter | Points the Premiere effect's virtual camera into any equirectangular clip. On Windows it renders on the GPU (CUDA) when Resolve hands over CUDA images; otherwise, and on a Mac, on all CPU cores. |

Both effects compile the Premiere plug-ins' own source files, not a port of
them:

- the importer's clip engine: stitch, seam, parallax, sky seam fix, lens
  shading, sun ghosts, stabilisation, colour;
- the reframe effect's camera: DJI and Classic lenses, presets, Keyframe
  Easing;
- the Source Settings mapping.

So a clip stitched and framed in Resolve is the clip Premiere shows. The
tests check this frame for frame.

## Install

Resolve only looks for plug-ins when it starts, so close it first.

**From the Windows release zip** (0.2.0 and later): unzip it and
double-click **`Install-Resolve.cmd`**. It asks for admin rights once.
**`Uninstall-Resolve.cmd`** takes the plug-ins out again. The zip's
`Install.cmd` is the Premiere installer; the two are independent.

**From source.** The bundle needs **no Adobe SDK**: every build makes it
(`OSV_BUILD_OFX`, on by default), with or without the Premiere plug-ins.

Windows (Visual Studio 2022, CMake, vcpkg; CUDA for the GPU path):

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset windows-msvc-cuda-release
cmake --build --preset windows-msvc-cuda-release
scripts\install_ofx.ps1            # into C:\Program Files\Common Files\OFX\Plugins
```

macOS on Apple Silicon (Xcode command line tools, CMake, Ninja, vcpkg; see
[`BUILDING_MAC.md`](BUILDING_MAC.md)):

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-release
cmake --build --preset macos-release
scripts/install_ofx.sh             # into /Library/OFX/Plugins
```

`-Uninstall` / `--uninstall` removes it again. The bundle is self-contained.
On Windows, its FFmpeg, OpenCL, fmt and spdlog DLLs sit next to
`OpenOSV.ofx`, and the module loads them from there, never from Resolve's own
folder. On a Mac, FFmpeg is embedded in `Contents/Frameworks` under
OpenOSV-prefixed names, and the installer clears the download quarantine and
signs the bundle ad hoc. To install by hand, copy the whole
`OpenOSV.ofx.bundle` folder from `<build>/plugins/ofx/` into the OpenFX
folder.

Third-party OpenFX plug-ins run in the free version of Resolve as well as in
Studio.

## Editing an .OSV clip in Resolve

Resolve can't open `.OSV` files, so the clip comes in through the generator.

1. Drag **OpenOSV Source** from the Effects Library onto the timeline.
2. In the Inspector, click **Choose .OSV File...** and pick the clip.
   Resolve's own file field has no Browse button. Pasting a path works too,
   quotes and all.
3. **Clip** now shows the clip's length (for example
   `K6 - 1234 frames at 59.940 fps (20.59 s)`). Trim the generator to that
   length: OpenFX gives a generator no way to tell Resolve how long it is.
   Past the end of the clip the generator renders transparent black.
4. Frame the shot with the **Camera** controls. They are Open 360 Reframe's
   controls: Preset, Lens, Pan / Tilt / Roll, FOV, Correction Angle and Zoom
   (DJI lens), Classic FOV / Distortion (Classic lens), Keyframe Easing and
   Smooth Keyframes. They keyframe in the Inspector. Choosing a preset,
   switching lens or typing a Zoom behaves exactly as in Premiere.

**Output**

- **Reframed view** (the default) renders the camera's view straight from the
  clip's native sphere, at the timeline's size.
- **360 equirect** renders the whole sphere at the timeline's size. Use it
  for a 2:1 timeline, a 360 export, or to feed **Open 360 Reframe** yourself.

**Start Frame** slides the clip under the generator: the clip frame shown on
the generator's first frame. It counts the clip's own frames.

### Colour

**Colour Output** defaults to **Rec. 709** (with DJI's look). A generator
can't tag its pixels with a colour space the way Premiere's importer does,
and a new Resolve project's timeline is Rec. 709 Gamma 2.4, so Rec. 709 looks
right straight away. For the other outputs:

- **BT.2100 PQ / HLG**: use these in HDR or colour-managed projects whose
  timeline colour space matches.
- **D-Log M (no transform)**: gives you the camera's log to grade yourself,
  for example with `luts\OpenOSV_Osmo360_DLogM_to_*.cube` or DJI's own LUT.

The stitching controls (Seam Search, Sky Seam Fix, Lens Shading, Parallax
Grid, Lens Alignment and the rest) are Premiere's Source Settings, with the
same defaults and meanings. The one exception is **Sphere Size** (Premiere's
"Output Size"): the size of the sphere the camera looks into in Reframed view.

### Audio

A generator has no audio. Take the clip's AAC track out with the command-line
tool and put it under the generator:

```powershell
osvtool extract CAM_0001.OSV --audio CAM_0001.aac
# If your Resolve refuses a raw .aac, remux it (no re-encode):
ffmpeg -i CAM_0001.aac -c copy CAM_0001.m4a
```

Line the audio up with the generator's first frame, or with **Start Frame**
if you moved it.

### Speed

The generator stitches the whole native sphere (6000 x 3000 for 6K) for every
frame, on the GPU through the importer's own CUDA or OpenCL renderer, then
frames the view from it on the CPU. That is Premiere's default path too. For
smoother playback, try one of these:

- set **Sphere Size** to 4K or 2K while cutting;
- use Resolve's proxy / render cache;
- turn off **Seam Search** in Stitching.

## Reframing other 360 footage

**Open 360 Reframe** treats the whole image it is given as the sphere. On the
Edit page Resolve scales every clip to the timeline before any effect sees
it. So a 2:1 equirect on a 16:9 timeline must be **stretched** to fill the
frame, not letterboxed:

- per project: **Project Settings > Image Scaling > Mismatched resolution
  files: Stretch frame to all corners**;
- or per clip: **Inspector > Retime and Scaling > Scaling: Stretch**.

Stretching distorts nothing that matters: the effect maps the sphere by
position across the frame, not by pixel aspect. It does cost resolution,
because the sphere is squeezed to the timeline's size first. For `.OSV`
clips, use **OpenOSV Source** in Reframed view instead: it frames the view
from the clip's native sphere with no squeeze.

On an NVIDIA machine with Resolve's GPU processing mode set to CUDA, the
filter renders on the GPU, on Resolve's own CUDA stream. Otherwise it renders
on all CPU cores.

## Reporting a problem

Please open an issue, or better, a pull request, at
<https://github.com/Kemerd/OpenOSV/issues>. That goes double for a Mac:
nobody on the project has one to test on.

Everything the plug-ins do is logged to
`%LOCALAPPDATA%\OpenOSV\OpenOSVOfx.log`. Set `OSV_PLUGIN_LOG_LEVEL=debug`
before starting Resolve for more detail. The first frame of every OpenOSV
Source logs the time Resolve asked for, the generator's frame range, both
frame rates and the clip frame it chose. If the generator shows the wrong
part of the clip, that line is what we need.

## What is tested, and what isn't

The following is checked by `tests/ofx` on every build:

- Plug-in list, identifiers, descriptors, contexts, clips and every
  parameter. That covers ranges and display ranges (Resolve clamps a double
  that lacks either), defaults and popup items.
- Every reframe, compared pixel for pixel with the Premiere effect's own CPU
  render of the same picture and controls: default view, moved DJI camera,
  Classic lens, Keyframe Easing, Smooth Keyframes, render windows. Also
  checked with top-down, bottom-up and padded image layouts, and that up is
  up.
- The supervised controls: presets, lens switch, Zoom, keyed controls, and
  that the plug-in's own writes are ignored.
- The CUDA path on a CUDA device, against the CPU path: on the host's
  stream; with no stream; with no context current; after the host recreates
  its context.
- On the sample clip:
  - the generator's sphere is the importer's stitch, with Start Frame
    working;
  - the generator's reframed view is bit for bit Premiere's two-step view of
    the same frame;
  - a render leaves the host's CUDA context current.
- A missing file or a non-OSV file: transparent output and one message, not
  one per frame.

The macOS build of the bundle compiles and runs these tests on GitHub's
Apple Silicon runners (`.github/workflows/macos.yml`), except the [cuda] and
[sample] ones.

The following can't be checked without Resolve:

- **What time Resolve gives a generator.** The OpenFX standard doesn't say,
  and Blackmagic doesn't document it. The generator counts from the start of
  its output clip's frame range. It falls back to the raw time when that
  range starts later than the time itself, and logs what it saw on its first
  frame.
- **How the Inspector draws the controls.** One lens's controls are shown at
  a time, through `kOfxParamPropSecret`, with `kOfxParamPropEnabled` as a
  fallback. Resolve is reported to honour both.
- **Colour management.** How a colour-managed project interprets a
  generator's output. See [Colour](#colour).
- **Playback speed inside Resolve.**
- **macOS inside Resolve at all.** The bundle builds and passes its tests on
  a Mac, but nobody has loaded it into Resolve on one. On a Mac the stitch
  runs on the GPU (Metal, through the clip engine). Open 360 Reframe renders
  on the CPU: Resolve hands a Mac plug-in Metal buffers, and a Metal path for
  the filter isn't written yet.

## Where the code is

| Path | What |
|---|---|
| `plugins/ofx/OfxMain.cpp` | Module exports, load / unload, DllMain |
| `plugins/ofx/OfxHost.*` | Defensive wrappers around the OpenFX suites |
| `plugins/ofx/OfxCamera.*` | The camera controls: describe, read (easing, smoothing), supervise |
| `plugins/ofx/OfxReframe.*`, `OfxRender.*`, `OfxCuda.*`, `OfxReframeKernel.cu` | The filter, its CPU loop and its CUDA launch |
| `plugins/ofx/OfxSource.*`, `OfxSourceParams.*`, `OfxFileDialog.*` | The generator, its stitch controls, the Choose File dialog |
| `plugins/ofx/OfxEngineHooks.cpp` | The two settings hooks the clip engine links against (Premiere's direct-path engine has no counterpart here) |
| `plugins/ofx/openfx/` | OpenFX 1.5.1 headers, vendored (BSD-3-Clause) |
| `tests/ofx/` | The mock OpenFX host and the tests |
| `plugins/ofx/OfxFileDialogMac.mm` | The macOS Choose File panel |
| `scripts/install_ofx.ps1`, `scripts/install_ofx.sh` | Install / uninstall (Windows / macOS) |

The module compiles the Premiere plug-ins' host-independent sources, but no
Adobe header: the clip engine is built with
`OSV_CLIP_ENGINE_WITHOUT_PREMIERE`, which leaves out its one Premiere-only
log detail. Premiere's direct GPU path (`Engine.cpp`) is replaced by
`EngineNoDirect.cpp`, which answers that there is none.
