# OpenOSV

**Independent, open-source toolkit for DJI Osmo 360 `.OSV` footage on Windows.**
Parse the container, read the embedded lens calibration and IMU data, stitch and
reframe the dual-fisheye streams on CPU, CUDA or OpenCL, and convert 10-bit
D-Log M to Rec.2100 PQ / HLG (or Rec.709) with open, documented colour math —
then edit it natively in Adobe Premiere Pro.

> Status: the core library, the `osvtool` CLI and both Premiere Pro plug-ins
> are implemented and tested. See `docs/PREMIERE.md` for the plug-ins and
> `docs/ROADMAP.md` for what is still open.

## Premiere Pro plug-ins

| Plug-in | What it does |
|---|---|
| `OpenOSVImporter.prm` | Registers `.osv` and `.lrf`. Decodes both fisheye lenses, stitches on CUDA / OpenCL / CPU and hands Premiere an equirectangular 360 x 180 frame tagged Rec.2100 PQ, HLG, Rec.709 or D-Log M passthrough, with the AAC track decoded. |
| `OpenOSVSourceSettings.aex` | "OpenOSV Source Settings", which Premiere attaches to the **master clip** by itself. The stitch options (colour output, output size, stabilisation and horizon lock, seam search, exposure match, calibration slot, D-Log M curve, exposure, render device) are right there in the Effect Controls panel. They apply to the whole clip and deliberately have no stopwatches — see below. |
| `Open360Reframe.aex` | "Open 360 Reframe" in the Effects panel. Pan, Tilt, Roll, FOV, Distortion, preset perspectives and source orientation, all keyframed by Premiere itself. Renders on the GPU through Premiere's own CUDA device, with a CPU fallback running the same kernel. |

Two effects, because they are two different jobs. **Source Settings** decides
how the sphere is built and coloured; those values are handed to the decoder as
one setting per clip, so they cannot be keyframed and the panel honestly shows
no stopwatch. **Open 360 Reframe** decides where the camera looks, per frame,
and keyframes everything. The old modal dialog (right-click > Source Settings)
still works if you prefer it, and writes exactly the same settings.

Reframing is interactive: **drag the picture** in the Program Monitor to pan and
tilt, `Shift` to lock an axis, `Ctrl` to zoom, `Alt` or the roll ring to roll.
Dragging the full frame width turns exactly one field of view, so the image
stays under the cursor. With a stopwatch armed, dragging at two points in time
gives an interpolated camera move with Premiere's own easing and curve editor.

Building the plug-ins needs the Adobe Premiere Pro and After Effects SDKs, which
are not redistributable and are therefore never committed — `docs/BUILDING.md`
says where to put them.

```powershell
cmake --preset windows-msvc-premiere-release
cmake --build --preset windows-msvc-premiere-release
ctest --preset premiere
scripts\install_plugins.ps1        # plug-ins -> MediaCore, presets + OpenOSV panel -> your profile
```

## Companion panel: the reframe goes on by itself

Drop an `.OSV` on the timeline and **Open 360 Reframe is already on it.** The
**OpenOSV** panel watches your sequences and applies the effect to every OSV or
LRF clip you add, once. It uses the Lens (DJI or Classic) and the Drag
Sensitivity you pick in the panel.

* It never touches clips that were already there. **Apply to all OSV clips in
  this sequence** does that on purpose.
* If you remove the effect from a clip, it stays removed.
* One switch turns it off.

`scripts\install_plugins.ps1` installs it along with the plug-ins
(`-PanelOnly` for just the panel, no admin rights; `-NoPanel` to skip it). It
picks whichever build installs without a click:

* **UXP** (Premiere 25.6+) when Adobe's plug-in installer is present;
* **CEP** (Premiere 22 and later) otherwise.

Find it under **Window > UXP Plugins** or **Window > Extensions**, and dock it
once. How it decides what's new, which APIs it uses and why, and what's left to
check live: `docs/PANEL.md`.

## Sequence presets

Premiere builds a new sequence at the clip's own frame size, and an
equirectangular sphere is always 2:1 — so a new sequence from an `.OSV` is
2:1 too, which is right for working on the sphere and wrong for 16:9 delivery.
`scripts\install_plugins.ps1` installs three presets to save you typing one:

| Preset | Size | For |
|---|---|---|
| **OpenOSV 2560x1440 59.94** | 2560 x 1440 | Reframed 16:9 delivery. The one you usually want. |
| **OpenOSV 3840x2160 59.94** | 3840 x 2160 | 4K delivery — set Output Size to Native or 4K first. |
| **OpenOSV 360 equirect 2560x1280 59.94** | 2560 x 1280 | The sphere itself, or a 360 VR export. Declares equirectangular VR, so Premiere's VR view works. |

They show up under **File > New > Sequence** in a group called **OpenOSV**,
and land in
`Documents\Adobe\Premiere Pro\<version>\Profile-<you>\Settings\SequencePresets\OpenOSV\`.
Premiere caches the preset list, so restart it if it was already running. Pass
`-NoPresets` to skip them. All three are 59.94 fps exactly — not 60, which
drifts against the footage by a frame every thousand.

## Why

DJI only ships its Premiere reframe plug-in for macOS, and its own apps export
SDR only. Osmo 360 owners on Windows with HDR pipelines were left out. OpenOSV
fixes that with a permissively licensed implementation anyone can build on.

## Shooting notes

**Record in D-Log M, 10-bit.** That keeps the full latitude of the sensor, and
the importer reads the clip's own `color_mode` metadata and applies the right
curve automatically. Normal 10-bit works too; the importer follows the
metadata rather than assuming.

**Leave "Color Recovery" on if you like it.** It only affects the camera's
live-view preview. The recorded `.OSV` is ordinary D-Log M either way and
carries no flag for the setting, so it changes nothing about the file or how
this toolkit decodes it. Monitoring is simply easier with it on, because raw
D-Log M looks flat and grey on the camera screen.

**Grading.** The importer converts D-Log M to Rec.2100 PQ (the default), HLG
or Rec.709 for you, so no LUT is needed in Premiere. If you would rather grade
the log yourself, set the clip's colour output to D-Log M passthrough in
Source Settings and apply one of the LUTs in `luts/` (also installed beside
the plug-ins) in Lumetri. Do not do both: applying a D-Log M LUT on top of a
converted PQ/HLG/709 output double-converts the footage.

## Quick start

```powershell
# One-time: vcpkg, Visual Studio 2022, CUDA 12.9 (optional), CMake 3.28+
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset windows-msvc-cuda-release      # or windows-msvc-cpu-only
cmake --build --preset windows-msvc-cuda-release
ctest --preset all

# Inspect a clip
osvtool probe CAM_XXXX.OSV --json probe.json

# Render a reframed PQ HDR video with horizon lock on the GPU
osvtool render CAM_XXXX.OSV --all --preset wide --stab horizon --color pq --device cuda --out out.mp4

# Export a D-Log M -> Rec.2100 PQ LUT for any NLE
osvtool lut --fit dji --out-transfer pq --size 65 dlogm_to_pq.cube
```

Exit codes: `0` ok, `1` usage error, `2` input error, `3` runtime error.

## What is inside

| Module | Purpose |
|---|---|
| `osv::core` | `Result`/`Status` error model, bounds-checked byte access, file mapping, math, thread pool |
| `osv::container` | ISO BMFF walker, sample tables, `camd` nested movie, index table |
| `osv::meta` | hand-rolled protobuf decoder for the `djmd` metadata, format detection, calibration selection |
| `osv::geom` | Kannala-Brandt fisheye model, extrinsics, virtual cameras, IMU attitude, stabilisation, seam search |
| `osv::color` | D-Log M decoding, BT.2100 HLG/PQ, `.cube` export, colour-mode auto-detect |
| `osv::video` | FFmpeg (LGPL) HEVC decoding with frame-accurate seek and optional hardware acceleration |
| `osv::render` | one shared per-pixel kernel (`osv_kernel.h`) executed by CPU, CUDA and OpenCL renderers |
| `osv::io` | PNG/TIFF/EXR writers, MP4 output through an external `ffmpeg.exe`, IMU CSV |

See `docs/ARCHITECTURE.md`, `docs/FORMAT.md`, `docs/GEOMETRY.md` and
`docs/COLOR.md` for the details and the verified conventions.

## Licence

Apache-2.0. See `LICENSE` and `NOTICE`. FFmpeg is used under the LGPL and is
linked dynamically; no GPL components are enabled.

## Legal

OpenOSV is an independent project, not affiliated with, endorsed by or
sponsored by DJI or Adobe. DJI, Osmo and Osmo 360 are trademarks of SZ DJI
Technology Co., Ltd.; Adobe, Premiere Pro and After Effects are trademarks of
Adobe Inc. The names are used only to say what OpenOSV works with.

It contains no DJI code, binaries, neural-network models, LUT files or
artwork. DJI's publicly distributed software was analysed so OpenOSV can read
the camera's files and render them the way DJI's own tools do; the numbers
and formulas taken from that analysis are re-implemented in OpenOSV's own code
and listed, with how each was obtained, in `docs/LEGAL.md`. Third-party
licences are in `NOTICE`; the papers and standards the project builds on are in
`docs/CITATIONS.md`.
