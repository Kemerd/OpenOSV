# Roadmap

## Milestone 1 - core library and osvtool (DONE)

* Container parser, protobuf metadata decoder, format detection.
* Kannala-Brandt lens model, verified extrinsic and scaling conventions.
* CPU reference renderer plus CUDA and OpenCL backends sharing one kernel.
* D-Log M to Rec.2100 PQ / HLG / Rec.709 colour pipeline, `.cube` export.
* IMU attitude timeline, horizon lock, full and smoothed stabilisation.
* Seam disparity search and exposure matching on the overlap band.
* Stills (PNG/TIFF/EXR) and HDR MP4 output through an external ffmpeg.

## Milestone 2 - Premiere Pro importer (.prm) (DONE)

Built as `OpenOSVImporter.prm` and verified by `osv_importer_tests`
(1222009 assertions in 38 cases) against the shipping binary; see the
"Verification results" section of `docs/PREMIERE.md` for the numbers.
Source settings, audio, colour tagging and the CUDA stitch are all in
place. Still open: the `.LRF` proxy opens and describes itself but does
not render (a milestone 1 library gap - `osvtool render` fails on it the
same way), the importer is synchronous, and Premiere itself has not been
launched to confirm the host accepts the plug-in.

* Registers `.OSV` (and the `.LRF` proxy) with Premiere.
* Declares the clip as equirectangular VR (360 x 180) so Premiere's own VR
  tools work, and tags frames BT.2100 PQ or HLG through the importer colour
  space API (SEI codes 9/16/9 or 9/18/9).
* Source settings (stitch mode, colour output, output size) through a
  source-settings effect.
* Stitches with the CUDA/OpenCL/CPU renderers inside the importer and returns
  10-bit biplanar or 32-bit float frames; uses the proxy for draft quality.
* Requires the Premiere Pro SDK (Adobe ID download) via `OSV_PREMIERE_SDK_DIR`.
* Compatibility target: Premiere Pro 2026 (26.x SDK) first, and the plug-ins
  are written against API surfaces that have existed since Premiere Pro 2022
  (v22: importer colour-space selectors, PPix Creator 2, GPU Device Suite v2,
  Sequence Info Suite) so the same binaries load in 2022-2025 as well. Only
  the colour-space tagging depends on newer hosts and degrades to Rec.709
  passthrough on old ones.

## Milestone 3 - Premiere Pro reframe effect (.aex) (DONE, one item deferred)

Built as `Open360Reframe.aex` with both entry points (`EffectMain` and
`xGPUFilterEntry`) in one module, a CUDA fatbin path and a threaded CPU
path over the shared kernel, verified by `osv_reframe_tests` (14266
assertions in 125 cases) at 151.9 dB GPU/CPU PSNR in 32f. Deferred: the
OpenCL branch of the GPU filter - CUDA and the CPU fallback cover every
machine we can currently test.

* AE-style effect with a `PrGPUFilter` CUDA + OpenCL path running the shared
  kernel in reframe mode on the equirect.
* Host-keyframed Pan / Tilt / Roll / FOV / Correction Angle, preset
  perspectives (Crystal Ball, Asteroid, Wide, Ultra Wide, Dewarping), source
  orientation offsets and resolution presets.
* Interactive Program Monitor overlay: drag the picture to pan and tilt, a
  roll ring and corner FOV grips, `Shift` to constrain an axis, `Ctrl` to
  zoom and `Alt` to roll from anywhere. Values are committed with
  `PF_ChangeFlag_CHANGED_VALUE` at the current time, so the host records the
  keyframes itself. Drawn with DrawBot as a thin crosshair, the ring arcs,
  the grips and a degrees readout, every stroke shadowed so it stays legible
  over bright and dark footage. There is no scroll-wheel zoom because the SDK
  has no mouse-wheel event; see docs/PREMIERE.md, "Program Monitor overlay".
* Same compatibility rule as the importer: PrGPUFilter interface v2 with a
  32-bit float CPU fallback so Premiere 2022 and newer all render it.
* `scripts/install_plugins.ps1` places both plug-ins in
  `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\`,
  which Premiere Pro, Media Encoder and After Effects all scan.

## Later

* DaVinci Resolve OpenFX plug-in reusing the same kernel.
* Lens shading (vignetting) correction: a radial gain model fitted from flat
  or sky frames, so the seam band matches DJI's brightness.
* Optical-flow seam refinement for close-range subjects.
* 4K-mode crop verification and a rotation clip to settle the attitude
  convention once and for all.
