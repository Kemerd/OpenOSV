# OpenOSV

**Clean-room, open-source toolkit for DJI Osmo 360 `.OSV` footage on Windows.**
Parse the container, read the embedded lens calibration and IMU data, stitch and
reframe the dual-fisheye streams on CPU, CUDA or OpenCL, and convert 10-bit
D-Log M to Rec.2100 PQ / HLG (or Rec.709) with public colour math. The library
is the foundation for the *Open 360 Reframe* Adobe Premiere Pro plug-ins.

> Status: milestone 1 (core library + `osvtool` CLI). Premiere importer and
> effect are milestones 2 and 3 — see `docs/ROADMAP.md`.

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
