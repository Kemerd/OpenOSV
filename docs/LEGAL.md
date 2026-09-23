# Legal notes

> **This is not legal advice.** It records, as accurately as the project can,
> what OpenOSV contains, what it does not, and where its DJI-specific
> knowledge came from, so that anyone assessing the project has the facts.
> Have it reviewed by counsel before any commercial distribution (section 12).

## Summary

* OpenOSV is an independent, Apache-2.0 project. It is **not affiliated with,
  endorsed by or sponsored by DJI or Adobe**.
* It contains **no DJI code, binaries, models, LUT files, masks, remap tables,
  SDK headers or artwork**, and no code transcribed from DJI software. It
  does keep a small set of values measured from DJI's LUTs, as the reference
  its colour fits and tests are checked against (section 5.4).
* DJI's publicly distributed software - DJI Studio, DJI's Premiere plug-ins
  and the stitching library inside DJI's Premiere importer - **was** studied
  for interoperability: to read the camera's files and to understand how
  DJI's tools render them. A limited set of numeric facts, formulas and
  parameters came out of that study and out of measurements of DJI's LUTs.
  Each one is re-implemented in OpenOSV's own code and listed in section 5.
* The project's rule: studying DJI's software to understand it is allowed;
  copying DJI code or DJI data files into OpenOSV is not.

## 1. Trademarks

DJI, Osmo and Osmo 360 are trademarks of SZ DJI Technology Co., Ltd. Adobe,
Premiere Pro and After Effects are trademarks of Adobe Inc. NVIDIA and CUDA
are trademarks of NVIDIA Corporation. All other names belong to their
owners.

OpenOSV uses these names only to say what it works with and which behaviour
a setting matches. User-facing labels such as "Lens: DJI", "DJI FOV" and the
"DJI Studio" Rec.709 look name the DJI behaviour the setting reproduces; they
do not mean DJI made, supplied or approved it.

## 2. No affiliation or endorsement

OpenOSV is not affiliated with, endorsed by, sponsored by or supported by
SZ DJI Technology Co., Ltd., Adobe Inc. or NVIDIA Corporation. Report
problems with OpenOSV to OpenOSV, not to them.

## 3. Purpose: interoperability

OpenOSV exists so that owners of an Osmo 360 can read, stitch, colour and
edit their own `.OSV` recordings on Windows, in Premiere Pro or from the
command line, including in HDR. Reading the file
format correctly and rendering it the way users expect from DJI's own tools
is the point of the DJI-specific work below.

## 4. What is in the repository, and what is not

### 4.1 Not included

Neither this repository nor the binaries built from it contain:

* DJI source code, or DJI binaries, libraries or other files taken from DJI
  software;
* code transcribed from DJI software;
* DJI neural-network models or weights;
* DJI LUT files or LUT images, DJI masks or DJI remap tables;
* DJI SDK headers, or DJI images or artwork;
* Adobe SDK headers or other Adobe SDK files.

How this was checked (2026-09-23): `git ls-files` lists no `.onnx`, `.bin`,
`.mlpackage`, `.dll`, `.dylib`, `.exe`, `.pb` or `.osv` file, and no such
file, nothing under `ref/`, and no LUT other than the three below appears
anywhere in the git history. The only tracked binary or media files are:

| Files | What they are |
|---|---|
| `luts/*.cube` (3) | Generated in full by `osvtool lut` from OpenOSV's own fitted models (`scripts/gen_luts.ps1`). `tests/unit/test_cube.cpp` regenerates them and requires byte equality with the committed copies. |
| `tests/fixtures/*.mp4` (4, 0.9-1.6 KB each) | Synthetic container fixtures written by `scripts/make_fixtures.py`. |
| `docs/research/neural_sky_seam_f32.png`, `research/flare/*.jpg`, `research/photo/*.tif` | Crops of OpenOSV's own renders of the project author's own recording (section 10). |

### 4.2 Vendor files that developer tools read locally

Some scripts and optional tests read a file from a copy of DJI software the
developer already has installed, in order to fit or check OpenOSV's own
numbers. The file is read in place; nothing from it is written into the
repository, and every such test skips when the file is absent.

| Tool | Reads | Writes / checks |
|---|---|---|
| `scripts/fit_dlogm.py --from-cube`, `scripts/fit_primaries.py --from-cube`, `scripts/fit_look.py` | DJI's Osmo 360 D-Log M to Rec.709 LUT, as installed by DJI Studio | Prints fitted constants only |
| `tests/unit/test_look.cpp` (optional case, `OSV_DJI_OSMO360_LUT`) | The same LUT | Compares OpenOSV's look against it |
| `scripts/calib_fit_lens_protector.py`, `tests/unit/test_lens_protector.cpp` (optional case) | DJI's 121-point lens-protector curve, from a local copy of DJI's macOS Premiere importer | Prints the five fitted coefficients and the residual; checks the shipped fit |

### 4.3 The `ref/` folder

During development, the copies of DJI's macOS plug-ins that were studied,
and material produced while studying them, were kept in a local `ref/`
folder (DJI Studio was studied where it is installed). `ref/` is listed in
`.gitignore`, has never been committed and is never distributed.

### 4.4 Adobe material

* The plug-ins are built against the Adobe Premiere Pro and After Effects
  SDKs. Adobe's SDK licence does not allow redistribution, so the SDKs are
  not in this repository; every developer downloads them from Adobe
  (`docs/BUILDING.md`).
* The three sequence presets in `presets/` follow the XML format of the
  presets that ship with Premiere Pro. The format is undocumented, so
  presets shipped with Premiere Pro 2026 were used as templates: some fields
  (for example the audio track layout and the VR projection block) are taken
  from them unchanged, and the frame size, frame rate and bit-depth fields
  are OpenOSV's (`presets/README.md` records exactly which).
* The undocumented install folder for those presets was confirmed from
  Premiere Pro's own installed files (`presets/README.md`). No Adobe file is
  included.

## 5. What OpenOSV took from DJI software, item by item

This is the complete list of DJI-derived facts, formulas and parameters that
OpenOSV's code uses. Each is implemented in OpenOSV's own C++, CUDA or OpenCL
code; none is copied DJI code or a DJI data file.

**Sources studied** for interoperability (all DJI's publicly distributed
software, installed or downloaded by the project author):

* **DJI Studio for Windows 1.0.0.24724**, including its bundled LUTs and the
  project files it wrote for the author's own clips. DJI Studio was also run,
  and the numbers it displayed for the author's clip were recorded (the
  screenshots themselves are not in the repository).
* **DJI's Premiere reframe plug-in for macOS.**
* **DJI's Premiere importer for macOS**, including its stitching library.

The findings are written up, as behaviour and formulas, in
`docs/research/DJI_CAMERA.md`, `docs/research/NEURAL_STITCHING.md`
(section 2) and `docs/FORMAT.md`.

### 5.1 The reframe camera ("Lens: DJI")

| Item | Where in OpenOSV | Source |
|---|---|---|
| The camera model: a unit sphere seen by a perspective camera with a **vertical** field of view (DJI's "FOV") placed behind the centre at a distance in sphere radii (DJI's "Correction Angle"), seeing the far intersection | `include/osv/geom/VirtualCamera.h` (`DjiSphereCamera`), `include/osv/render/osv_kernel.h` (`OSV_PROJ_DJI_SPHERE`, `osvDjiSphereRay`) | Determined from DJI's Premiere reframe plug-in and DJI Studio for interoperability. OpenOSV derived its own trig-free closed form and checked it against the equivalent perspective-matrix formulation (2 488 random cases, worst error 2.3e-15). |
| The **Zoom** read-out: `zoom = 360 - 2 atan(1/a) - 2 acos(d a / sqrt(1 + a^2))`, `a = tan(min(180, fov) / 2) * aspect`, with DJI's input guards | `include/osv/geom/VirtualCamera.h` / `src/osv/geom/VirtualCamera.cpp` (`djiZoomDeg`), `plugins/reframe/ReframeParams.h` | Determined from DJI Studio for interoperability. Written in OpenOSV's own C++ so the displayed number matches DJI Studio's; confirmed against two DJI Studio read-outs of the author's clip. |
| The **zoom path**: FOV + 130·δ and Correction + δ together, clamped to FOV [20, 150] and Correction [0, 1] | `plugins/reframe/ReframeParams.h` (`OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION`, `OSV_REFRAME_DJI_STUDIO_*`, `djiZoomTo`), `plugins/reframe/ReframeUi.cpp`, `plugins/reframe/ReframeCpu.cpp` | Determined from DJI Studio (its zoom gesture and its FOV and Correction limits) for interoperability. |
| Control ranges: FOV valid 1-178 and Correction 0-1.8 (the plug-in), FOV slider 20-150 (DJI Studio) | `plugins/reframe/ReframeParams.h` (`OSV_REFRAME_DJI_FOV_*`, `OSV_REFRAME_CORRECTION_*`) | DJI's Premiere reframe plug-in; DJI Studio. |
| The five presets (Crystal Ball, Asteroid, Wide, Ultra Wide, Dewarping): FOV per output shape, Correction, tilt | `include/osv/geom/Presets.h` (`kDjiPresets`) | DJI's Premiere reframe plug-in; the landscape column matches DJI Studio's own preset list. |
| Premiere Pro numbers popup values from 0 on the GPU path | `PopupBase` and `decodeHostPopup` in `plugins/reframe/ReframeParams.h` | Established from OpenOSV's own parameter dump in Premiere Pro 26.2.2; consistent with DJI's Premiere reframe plug-in, which adds 1 to every popup value it reads there. |

### 5.2 Stitching

| Item | Where in OpenOSV | Source |
|---|---|---|
| Usable lens field of view, 195.18 degrees | `include/osv/geom/Blend.h`, `include/osv/geom/LensRig.h`, `include/osv/geom/KannalaBrandt5.h`, `plugins/importer/ImporterInstance.cpp`, `osvtool --lens-fov` default | The value DJI's stitching library uses (`docs/GEOMETRY.md`). |
| The optical-flow algorithm is Dense Inverse Search | `include/osv/render/DisFlow.h`, `src/osv/render/DisFlow.cpp`, `src/osv/render/cuda/CudaDisKernel.cu` | DJI's stitching library uses DIS. OpenOSV implements the algorithm from the published paper (Kroeger et al., ECCV 2016); where DJI's stage boundaries differ from the paper's, OpenOSV's mirror them so DJI's values map onto named parameters. |
| DIS parameters: patch stride 5 px (patch size 8, also the paper's), step damping 0.4, structure-tensor determinant floor 0.001, patch SSD rejection 1e7, the 8-bit intensity scale (255) those thresholds assume, and the densification weight 1 / max(1, \|residual\|) | `DisFlowParams` in `include/osv/render/DisFlow.h`; `src/osv/render/DisFlow.cpp`; `src/osv/render/cuda/CudaDisKernel.cu`, `CudaDisFlow.cpp` | DJI's stitching library, determined for interoperability. The densification weight is also the one in OpenCV's DIS implementation. The ±24 px displacement limit equals DJI's search clamp (`docs/research/NEURAL_STITCHING.md` section 2). |
| Seam-finder cost weights: pull toward the geometric seam 0.15, coverage weight 6.0, temporal hold relaxing at 4 px of disparity to a floor of 0.1, forbidden-transition cost 1e6 | `SeamCarveParams` in `include/osv/render/SeamCarve.h` (marked "reference implementation"), `src/osv/render/SeamCarve.cpp` | DJI's stitching library, determined for interoperability. The seam method itself is published dynamic-programming seam carving (Avidan and Shamir 2007); using the selfie-stick coverage as an input to seam placement, rather than masking it, follows DJI's design. |
| Photometric rules: a pixel is trusted for gain estimation only when both lenses' weights are at least 0.99; chroma corrections decay twice as fast as luma | `kTrustedAlpha` in `src/osv/render/SeamAnalysis.cpp`; `trustAlpha` and `chromaDecayScale` in `include/osv/render/PhotoSeam.h`; `include/osv/render/osv_kernel.h` | DJI's stitching library (`docs/research/NEURAL_STITCHING.md` section 2). The gain field is OpenOSV's own estimator, designed from measurements of the author's footage; DJI's learned colour stage outputs the same kind of low-resolution field. |

### 5.3 Lens protectors and calibration

| Item | Where in OpenOSV | Source |
|---|---|---|
| The lens-protector field-angle correction: five coefficients of an odd polynomial `g(θ)` | `kLensProtectorCoefficients` in `include/osv/geom/LensProtector.h`, `src/osv/geom/LensProtector.cpp` | OpenOSV's own least-squares fit (worst residual 0.0129°) to the 121-point protector curve DJI's macOS Premiere importer applies. **The curve itself is not in the repository**; `scripts/calib_fit_lens_protector.py` re-derives the fit from a local copy of DJI's importer (section 4.2). |
| The order of application: the protector curve bends each ray's angle before the Kannala-Brandt projection with the native calibration | `ProtectorDirection::Forward` in `include/osv/geom/LensProtector.h`; `include/osv/render/LensProtectorCheck.h` | DJI's Premiere importer, determined for interoperability. OpenOSV still checks the direction on each clip's first frame. |
| Calibration behaviour: DJI's tools never read the lens-guard slots and instead correct protector footage with that curve on the native calibration; DJI Studio defaults its protector option from `extri_lens_mode == 1`, and OpenOSV's Auto choice does the same | `src/osv/meta/CalibrationSelector.cpp`, `include/osv/meta/CalibrationSelector.h`, `plugins/importer/CalibrationUi.h`, `plugins/importer/ProtectorGuard.h` | DJI's Premiere importer and DJI Studio (`docs/FORMAT.md`). DJI's rule for choosing a calibration slot is documented there but not followed: OpenOSV stitches with `native_refine` by default. |

### 5.4 Colour (fits to measurements of DJI's LUTs)

These were obtained by measuring DJI's publicly distributed LUTs, not from
DJI's programs. The LUTs are:

* **"DJI Osmo 360 D-Log M to Rec.709 V1.cube"**, bundled with DJI Studio
  1.0.0.24724 (byte-identical to DJI's Pocket 3 D-Log M LUT, `docs/COLOR.md`);
* **`FT_StyleGeneralDlogm2HLG`**, an 8-bit 64³ PNG style LUT bundled with DJI
  Studio.

| Item | Where in OpenOSV | How it was obtained |
|---|---|---|
| Osmo 360 D-Log M curve (7 constants, the default) | `kDlogMOsmo360` in `include/osv/color/DlogM.h` | Fitted by `scripts/fit_dlogm.py` to the 33 neutral-axis entries of the Osmo 360 Rec.709 LUT. |
| "DJI-matched" D-Log M curve (7 constants, `--fit dji`) | `kDlogMDjiRefit` in `include/osv/color/DlogM.h` | Fitted by `scripts/fit_dlogm.py` to 64 neutral-axis measurements of `FT_StyleGeneralDlogm2HLG`. |
| Osmo 360 native primaries matrix (9 constants, the default) | `kNativeToRec2020_Osmo360` in `include/osv/color/Matrices.h` | Fitted by `scripts/fit_primaries.py` to all 35 937 entries of the Osmo 360 Rec.709 LUT. |
| The "DJI Studio" Rec.709 look (46 stored constants, 38 free) | `kLookDjiRec709` in `include/osv/color/Look.h`; `osvLookApply` in `include/osv/color/ColorMath.h` | OpenOSV's own parametric model, fitted by `scripts/fit_look.py` to all 35 937 entries of the Osmo 360 Rec.709 LUT plus pixels of the author's footage. Its gamut compression uses the published ACES Reference Gamut Compression curve. |
| The fact that DJI Studio applies that LUT to Osmo 360 D-Log M clips as its automatic "D-LOG M" filter | `include/osv/color/Look.h`, `docs/COLOR.md` | DJI Studio project files written for the author's clips. |

**Measured values recorded in the repository.** Besides the fitted constants,
the repository keeps a small set of measured values as the fits' provenance
and the tests' reference:

* 64 neutral-axis values of `FT_StyleGeneralDlogm2HLG`:
  `HLG_TABLE` in `scripts/fit_dlogm.py` and `kDjiHlgTable` in
  `tests/unit/test_color.cpp`;
* 105 of the Osmo 360 Rec.709 LUT's 35 937 entries: 33 neutral-axis values
  (`kOsmo360Rec709Table`) and 72 colour entries (`kDjiLookSamples`), both in
  `tests/unit/DjiReference.h`;
* individual measured values quoted in comments and docs to explain a fit
  (for example in `docs/COLOR.md`, `include/osv/color/Look.h`,
  `include/osv/color/DlogM.h` and `include/osv/color/Matrices.h`).

No LUT file, LUT image or other DJI LUT data is included beyond these values.

### 5.5 The file format

| Item | Where in OpenOSV | Source |
|---|---|---|
| The `djmd` metadata's protobuf message layout, field numbers, scalar types and field and enum names | `proto/dvtm_osmo360.proto` (OpenOSV's own transcription), `src/osv/meta/DjmdDecoder.cpp`, `include/osv/meta/Types.h` | Recorded clips, and the schema's names as used by DJI Studio for Windows and DJI's macOS Premiere importer. Cross-checked against the `dvtm_oq101.proto` published in the open-source telemetry-parser project (MIT OR Apache-2.0). |
| The meaning of the calibration slots and fields (`native_refine`, far presets, lens-guard and water slots, `extri_lens_mode`, `digital_focal_length`, the occlusion polygon, the extrinsic quaternion) and the 5-term Kannala-Brandt lens model | `include/osv/meta/*`, `include/osv/geom/*`, `docs/FORMAT.md`, `docs/GEOMETRY.md` | The schema's names, recorded clips (every convention in `docs/GEOMETRY.md` was verified on real footage and has a test), and DJI's Premiere importer (which projects with the native calibration through a Kannala-Brandt model). |
| The container: an ISO base media file with DJI's `djmd`, `dbgi` and `camd` additions and an index table in a `free` box | `include/osv/container/*`, `src/osv/container/*` | Recorded clips. The base format is the public ISO/IEC 14496-12 standard. |

### 5.6 Recorded in documentation only, not used by the code

The research documents also describe DJI behaviour that OpenOSV does **not**
implement, so that the difference is understood. Examples: DJI Studio's
extra de-distortion stage and the two 13-sample curves that weight it
(`docs/research/DJI_CAMERA.md` section 4); the four photometric layers of
DJI's stitcher, the sizes its learned colour stage works at and its seam
strip height (`docs/research/NEURAL_STITCHING.md` section 2); DJI's
calibration-slot rule and the size of its underwater correction
(`docs/FORMAT.md`). These are descriptions and a few numbers in prose; none of
them is in OpenOSV's code.

The study also found DJI's Kalman flow-filter parameters, its Gaussian
blur taps and the layout of its remap tables. OpenOSV uses none of them, and
their values are not recorded in this repository.

### 5.7 Neural networks

DJI's Premiere importer uses neural-network models on its AI stitching path.
Their role in DJI's pipeline is described, as behaviour, in
`docs/research/NEURAL_STITCHING.md` section 2. **No DJI model, weight or
model file is used by OpenOSV or included in this repository or its
binaries.** The only neural network OpenOSV's code loads is SEA-RAFT
(BSD-3-Clause), rebuilt from its authors' release by a script and never
committed (`NOTICE`).

## 6. The .OSV format

OpenOSV reads `.OSV` and `.LRF` files, which are ISO base media files with
DJI-specific boxes and protobuf metadata. The container layout was
determined from recorded files; the protobuf schema and the lens calibration
model from recorded files and from DJI's software (section 5.5).
`proto/dvtm_osmo360.proto`
is OpenOSV's own transcription of the schema: its comments, layout and
placeholder names for fields whose meaning is not established are OpenOSV's;
its message, field and enum names follow the schema's own names where those
are known.

## 7. Third-party licences

OpenOSV is licensed under Apache-2.0 (`LICENSE`). Third-party components,
their licences and how they are linked are listed in `NOTICE`. The research
papers, standards and datasets the project used or evaluated are listed in
`docs/CITATIONS.md`.

## 8. Research-only material

* `docs/research/` holds analysis and design documents. They describe what
  was measured and, for DJI's software, how it behaves.
* `research/` holds measurement scripts and figures. They are not part of
  the library or the plug-ins, and the project's CMake build does not build
  them.
* Third-party model weights that research scripts download for evaluation
  land in git-ignored folders and are never committed. Whether any of them
  may ship is decided per model, by its licence.
* The local `ref/` folder holding the studied DJI plug-ins is git-ignored and
  never distributed (section 4.3).

## 9. Patents

No freedom-to-operate review has been done. The patents the flare research
turned up, and why some flare code is implemented but switched off until
cleared, are recorded in `docs/research/FLARE.md` section 3.4 and listed in
`docs/CITATIONS.md`.

## 10. Footage and images

The sample clip used for measurements and every image in `docs/research/`
and `research/` are the project author's own recordings, rendered by
OpenOSV. Test golden data in `tests/golden/` is produced by the project's
own scripts, from that recording and from OpenOSV's own constants.

## 11. No warranty

OpenOSV is provided "AS IS", without warranties or conditions of any kind,
and without liability of its contributors, as set out in sections 7
("Disclaimer of Warranty") and 8 ("Limitation of Liability") of the Apache
License 2.0 (`LICENSE`).

## 12. Review before distribution

Laws on studying software for interoperability vary by jurisdiction, and the
licence terms of the software studied may add their own conditions. The
project has not had either assessed. Before any commercial distribution, have
counsel review this document, the research documents it points to and the
relevant licence terms. Nothing in this document is legal advice.
