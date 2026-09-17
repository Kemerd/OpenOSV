# Architecture

OpenOSV is a set of static libraries with a strict one-way dependency chain,
a command line tool on top, and (in later milestones) Premiere Pro plug-ins
that reuse the same libraries.

```
osv_core -> osv_container -> osv_meta -> osv_color -> osv_video -> osv_geom
         -> osv_render_cpu -> { osv_render_cuda, osv_render_opencl } -> osv_io -> osvtool
```

| Library | Responsibility |
|---|---|
| `osv_core` | `Result`/`Status` error model (no exceptions cross the API), bounds-checked `ByteSpan`/`ByteReader`, `MappedFile`, double-precision `Vec3d`/`Mat3d`/`Quatd`, `ThreadPool`, logging facade |
| `osv_container` | ISO BMFF box walker, sample tables, HEVC configuration, DJI index table and the nested `camd` movie. Everything aliases the file mapping; nothing is copied |
| `osv_meta` | Hand-rolled protobuf wire decoder for the `djmd` tracks, typed `ClipMeta`/`StreamMeta`/`DewarpParams`/`FrameMeta`, format detection, calibration set selection, JSON export |
| `osv_color` | Device-agnostic colour math (`ColorMath.h`, plain C style so the GPU kernels can include it): D-Log M decoding, BT.2100 HLG/PQ, Rec.709 tone mapping, `.cube` export, colour-mode auto-detection |
| `osv_video` | FFmpeg (LGPL, dynamic) HEVC decoding with frame-accurate seeking, optional D3D11VA/CUDA hardware acceleration, dual-stream pairing, LRF proxy splitting |
| `osv_geom` | Kannala-Brandt fisheye model, extrinsic conventions, stream scaling, virtual cameras and equirect layouts, IMU attitude timeline, stabilisation, blend weights, seam search and gain compensation |
| `osv_render` | One per-pixel shader (`osv_kernel.h`) executed by the CPU reference renderer and by the CUDA and OpenCL backends; parity is enforced by tests |
| `osv_io` | PNG/TIFF/EXR writers, MP4 output through an external `ffmpeg.exe`, IMU CSV, audio extraction |

## Render data flow

1. `OsvFile` maps the clip and builds the track table.
2. `MetadataTrack` decodes the calibration and per-frame metadata;
   `FormatDetector` names the mode; `CalibrationSelector` picks the lens pair.
3. `StreamScaling` + `LensRig` turn the 3840-px calibration into stream-pixel
   lens models with body-to-lens rotations.
4. `AttitudeTrack` (optional) supplies a world-from-body rotation per frame;
   `Stabilization` turns it into a correction rotation.
5. `DualStreamReader` decodes the two fisheye frames (CPU planes or CUDA
   device memory).
6. `RenderParamsBuilder` packs everything into a plain-old-data
   `OsvRenderParams` block (a few KB) that the kernels take by value.
7. The renderer runs `osvShadePixel` for every output pixel: view ray ->
   body ray -> per-lens projection -> validity + feather + occlusion weight ->
   bilinear YCbCr sample -> D-Log M to linear -> blend in linear light ->
   working-space matrix -> output transfer (PQ / HLG / 709 / linear).
8. `osv_io` writes stills or streams frames to `ffmpeg.exe`.

Nothing before step 7 or after it depends on a GPU, so the CPU-only
configuration builds the entire tool.

## Conventions

Every convention that had to be inferred from footage (quaternion order,
rotation sense, crop scale, focal source, colour anchors) is a parameter with a
verified default and a test that fails if the default drifts. See
`docs/GEOMETRY.md` and `docs/COLOR.md`.

## Later milestones

* **Importer (.prm)**: decodes + stitches to an equirect tagged BT.2100 PQ/HLG
  and hands CPU frames to Premiere (the SDK does not accept GPU frames from
  importers).
* **Effect (.aex)**: AE-style effect with a `PrGPUFilter` CUDA/OpenCL path that
  runs `osv_kernel.h` in reframe mode on the equirect, with host-keyframed
  pan/tilt/roll/FOV/correction parameters.
