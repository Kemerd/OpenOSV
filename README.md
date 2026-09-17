# OpenOSV

**Clean-room, open-source toolkit for DJI Osmo 360 `.OSV` footage on Windows.**
Parse the container, read the embedded lens calibration and IMU data, stitch and
reframe the dual-fisheye streams on CPU, CUDA or OpenCL, and convert 10-bit
D-Log M to Rec.2100 PQ / HLG (or Rec.709) with public colour math — then edit
it natively in Adobe Premiere Pro.

> Status: the core library, the `osvtool` CLI and both Premiere Pro plug-ins
> are implemented and tested. See `docs/PREMIERE.md` for the plug-ins and
> `docs/ROADMAP.md` for what is still open.

## Premiere Pro plug-ins

| Plug-in | What it does |
|---|---|
| `OpenOSVImporter.prm` | Registers `.osv` and `.lrf`. Decodes both fisheye lenses, stitches on CUDA / OpenCL / CPU and hands Premiere an equirectangular 360 x 180 frame tagged Rec.2100 PQ, HLG or Rec.709, with the AAC track decoded. Stitch options (colour output, output size, stabilisation and horizon lock, seam search, exposure match, calibration slot, D-Log M curve, exposure, render device) live in **Source Settings**. |
| `Open360Reframe.aex` | "Open 360 Reframe" in the Effects panel. Pan, Tilt, Roll, FOV, Distortion, preset perspectives and source orientation, all keyframed by Premiere itself. Renders on the GPU through Premiere's own CUDA device, with a CPU fallback running the same kernel. |

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
scripts\install_plugins.ps1        # copies into the shared MediaCore plug-ins folder
```

## Why

DJI only ships its Premiere reframe plug-in for macOS, and its own apps export
SDR only. Osmo 360 owners on Windows with HDR pipelines were left out. OpenOSV
fixes that with a permissively licensed implementation anyone can build on.

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
