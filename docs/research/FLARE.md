# Sun ghosts and veiling glare: what they are, what we remove, what we cannot

WP-FLARE, 2026-09-23 (phase 1: research and the removal; phase 2: wired
into the importer and the direct path, on by default, section 6). Sample
clip: `example_footage_dlogm.OSV` (65 frames, dual 3000 x 3000 D-Log M, shot
through an ND filter from an aircraft).
Measurement tool: `osv_flare_bench` (`tests/bench/FlareBench.cpp`).
Crops: `research/flare/` (regenerate with `research/flare/make_crops.py`).
Every time quoted here was taken while nine other agents were building on the
same machine; treat absolute times as noisy, ratios as better.

## 1. Summary

**Two things were reported as "a large faint rounded-rectangle lens look near
the sun". Only one of them is a lens artifact.**

* **The ghost.** A pale, rounded-rectangle ("pill") reflection of the sun sits
  12 degrees from the sun in the master lens. On the sky behind it, it adds
  +23..29 % light across its plateau and +78 % at its brightest pixel. Three
  fainter ghosts (+7..11 %) sit further out along the same line. All of them
  are internal reflections that only the sun's own lens records. The other
  lens cannot see that part of the sky, so it cannot help here.
* **Not a ghost.** In the equirect there is a much larger darker rounded
  rectangle. That is the sky itself. The aircraft is banked about 90 degrees,
  which puts the zenith on the lens seam. The equirect then maps the circles
  of equal sky brightness around the zenith into barrel shapes. A veil in the
  sun's lens (below) makes its half of that shape paler, and the seam band
  cuts through the middle of it.

**What we remove, and how well** (frame 0, scene-linear):

| | before | after |
|---|---|---|
| Pill ghost: plateau excess over the sky | +23.4 % | +0.2 % |
| Pill ghost: RMS deviation over its whole footprint (rim included) | 13.5 % | 5.1 % |
| Faint ghost at (833, 1289): plateau excess | +7.2 % | -0.4 % |
| Pixels changed outside the fitted ghosts (1920 x 1080 view) | - | 0 of 2,011,781 |
| Pixels changed on the sun disc | - | 0 of 4,376 |
| Overlap step, master minus slave, equator rows (veil, research only) | +0.0497 | +0.0124 |

* **Tracking:** the pill is found in 65 of 65 frames.
* **Cost:** the analysis takes 36-67 ms per frame pair. The importer runs it
  once per sun position, not per frame: an in-order pass over the 65 frames
  measures 6 times. Every other frame pays a 3-6 ms sun check. The kernel
  cost is below the timing noise.
* **Where it runs:** every importer frame and every direct-path frame, on by
  default for new clips, switchable per clip ("Sun Ghost Removal" in the
  Source Settings effect, "Sun ghost removal" in the importer dialog).
  Playback and scrubbing never wait for it (section 6.1).
* **Crops:** see `research/flare/`.

**What we cannot remove without a model we cannot ship.** Some residue needs
a learned flare remover:

* the pill's irregular caustic interior (the 5 % that remains);
* the inverted rainbow ghost, which sits on clouds and ground texture;
* the diffraction star;
* the glow around the sun.

No commercially usable model exists. Flare7K / Flare7K++ / BracketFlare
weights are S-Lab non-commercial. The Wu et al. (ICCV 2021) model was never
released. Training our own is possible in principle, but it faces a Google
patent on the training method (section 3.4).

**How it is wired (importer, direct path, seam, switch):** section 6.

## 2. The sample clip, characterised

The sun is in the **master lens (lens 1) only**. Its centroid in stream pixels
runs from (1346.5, 1494.1) at frame 0 to (1348.5, 1510.3) at frame 64. That
is 9.8-9.9 degrees off the optical axis, at azimuth 178.8 degrees (image
left).

* The clipped core has an equivalent radius of 38.6-39 px (2.7 degrees).
  That is about five times the solar disc (0.53 degrees, 7.7 px), so most of
  the clipped core is lens glare.
* The slave lens looks 170 degrees away from the sun. Its largest clipped
  region is the sunlit white fuselage. That region is ragged and elongated,
  and the sun detector must not accept it (see the test note in section 4.2).
* The calibrated optical centre of the master is (1491.1, 1499.0) in stream
  px: c = 1500 + (c_cal - 1920) * 3000 / 3776.

### 2.1 The ghosts

Measured on the raw master frame (average of frames 0/16/32/48/64, linear
light, local quadratic background removed):

| | centre (px) | off axis | azimuth | r / r_sun | size (px) | shape | peak excess | colour of the excess |
|---|---|---|---|---|---|---|---|---|
| G1 "pill" | (1182, 1547) | 21.5 deg | 171.1 deg | 2.18 | 84 x 40-50 | stadium, rotated -12 deg, bright caustic rim on one side | **+78 %** | warm white on blue sky |
| G4 | (990, 1575) | 34.3 deg | 171.4 deg | 3.53 | ~85 | disc cut by a straight edge (bright line on top) | +10 % | orange |
| G3 | (876, 1582) | 41.7 deg | 172.3 deg | 4.34 | ~85 | rounded square, one side flattened | +13 % | neutral |
| G2 | (834, 1288) | 46.7 deg | 197.1 deg | 4.89 | ~97 | disc | +11 % | neutral, slightly blue |
| G0 (inverted) | (1704, 1506) | 14.6 deg | opposite side | -1.47 | ~50 | round, iridescent | +129 % over cloud texture | rainbow |

Visible in the crops: `sun_view_before_after.jpg` and the stretched
`sun_view_stretched.jpg`.

**Why rounded rectangles.** Each ghost is a defocused image of the aperture:
a disc, whose edge is brightened by a caustic. Some are cut by a straight
edge. That is what a rectangular stop (the sensor's cover glass, or the IR
filter's black mask) does to a reflection path that passes it twice. Discs
cut by straight edges read as rounded rectangles, and at +78 % the pill is
the one a viewer notices.

**Point symmetry does not hold here.** A reflection off a flat filter in
front of the lens is predicted to sit at the point-symmetric position,
r / r_sun = -1, in the angle domain. Dai et al. (CVPR 2023) prove that for
smartphone lenses. For the sample the exact mirror of the sun is
(1635.7, 1503.9), and nothing is there. The only inverted ghost (G0) sits at
-1.47, 68 px further out, so it comes from reflections between curved lens
surfaces, not from a flat plate.

The erect ghosts G1, G4 and G3 lie on one radial line, but that line is at
azimuth 171.5 degrees, 7 degrees off the sun's. The G1-G3 line misses the
sun by 26 px. The surfaces that form these ghosts are decentred or tilted
relative to the calibrated axis. A slightly tilted ND filter would do exactly
this: a tilted flat surface shifts its reflections by twice the tilt.

Consequences:

* **Pure prediction from the sun position** (symmetry, or the Hullin et al.
  lens-prescription model) would miss by tens of pixels, and would need the
  DJI lens prescription and the filter's pose. We have neither.
* **What works is detection plus a physical prior:** ghosts lie near the line
  through the optical centre and the sun, on either side. The tolerance used
  is ±20 degrees of azimuth, which covers G2's 18.3.
* The sun moves only 16 px across this one-second clip, so it cannot be used
  to calibrate a per-lens ghost map (magnification per reflection path). A
  clip with a sweeping sun could; see section 7.

### 2.2 The large "rounded rectangle" in the equirect is the sky

In the Standard equirect the horizon runs down the frame near longitude
-4 deg, because the aircraft is banked about 90 degrees. The zenith is
therefore near longitude -94 deg, latitude 0: on the equator, right at the
lens seam (longitude -90 deg).

Clear sky darkens towards the zenith and away from the sun. The equirect
maps circles of equal angle around an equator point into barrels whose sides
are nearly straight. The result is a large dark rounded rectangle spanning
longitude -170..-10 deg, bright where it meets the horizon on both sides and
at both poles, which also lie on the horizon. `equirect_sky_before_after.jpg`
shows it before and after. The removal does not change it, correctly.

Two lens effects sit on top of it:

* **The veil** (section 2.3) lifts the master's half, longitude -90..0,
  relative to the slave's.
* **Rim darkening** at the seam draws the two curved lines at about -97 and
  -83 deg, which are the feather edges. That rim darkening is multiplicative
  and belongs to WP-PHOTO (lens 0 darkens from about 92.8 deg); it is not
  glare.

### 2.3 Veiling glare: the dual-lens measurement

The overlap band is the one place where both lenses see the same scene.
There the master (sun) lens reads brighter:

* sky at the rim: master 0.11 linear against the slave's 0.065 (**+70 %**);
* ground: 0.17 against 0.13.

The offset is about the same in both. It is additive, not a gain:

| Fit over smooth column blocks | Additive-only offset (RMS) | Gain-only (RMS) |
|---|---|---|
| R | +0.037 (0.0107) | x1.53 (0.0249) |
| G | +0.048 (0.0160) | x1.44 (0.0197) |

A joint fit gives g = 0.94 / 0.97 with v = +0.045 / +0.049 for R / G. The
veil is roughly neutral, as scattered sunlight should be.

`estimateVeil()` measures +0.0384 on frames 0, 32 and 64. It uses only rows
within ±2.5 deg of the equator, so neither lens's darkened rim enters the
fit. Subtracting it cuts the overlap step (master minus slave, equator rows)
from +0.0497 to +0.0124 linear. What remains is multiplicative (vignetting,
gain).

This agrees with WP-NEURAL's independent measurement (NEURAL_STITCHING.md
section 1.3: "about 0.04-0.05 linear, roughly equal across R, G and B, the
signature of an additive veil"). It also agrees with DJI's own stitcher, which
carries an additive sun-glare pass gated by SSIM so that structure is left
alone (NEURAL_STITCHING.md section 2).

**Patent concern:** estimating flare by comparing the two images along the
stitch line is exactly what several active GoPro patents claim (section 3.4).
The estimator is therefore implemented and measured, but not wired, and must
stay off until it has been cleared.

**Why DJI's ghost looks subtler.** WP-NEURAL's study of DJI's stitcher
found no flare or ghost stage: its only glare stage is that seam-band
offset. So DJI does not remove the pill either. Its subtler look
must come from tone: highlight roll-off near the sun (WP-LOOK) and a darker
sky after its glare offset. This is an inference, not a measurement of DJI
Studio's output.

## 3. Research

Licences below were read from the repositories' LICENSE files or README
licence statements on 2026-09-23. Patent claim 1 of US11330208B2 and the
expiry of US9860446B2 were read on Google Patents; the other patent claims
are summarised from the same research pass and must be re-read by counsel.

### 3.1 Classical: prediction, templates, deconvolution

* **Physically based ghost prediction.** Hullin, Eisemann, Seidel, Lee,
  *Physically-Based Real-Time Lens Flare Rendering*, ACM TOG 30(4) (SIGGRAPH
  2011).
  - It traces every two-reflection path through a known lens prescription,
    including aberrations and coatings.
  - It would predict our ghosts exactly if we had DJI's lens design and the
    ND filter's pose. We have neither.
  - Follow-up: Lee and Eisemann, *Practical Real-Time Lens-Flare Rendering*,
    CGF 32(4) 2013.
* **Symmetry prior.** Dai, Luo, Zhou, Li, Loy, *Nighttime Smartphone
  Reflective Flare Removal Using Optical Center Symmetry Prior*, CVPR 2023
  (BracketFlare, arXiv 2303.15046).
  - Paraxial transfer matrices show that the ghost lies on the line through
    the source and the optical centre at a fixed ratio k (-1 for
    smartphones).
  - The authors add that the prior "does not always hold" for professional
    cameras. Our k = -1.47 and the 7-degree offset of the erect ghost train
    are an example.
  - Code and weights: S-Lab License 1.0, non-commercial. Not shippable.
* **Protection-glass ghosts.** Jin et al., *Toward Real Flare Removal: A
  Comprehensive Pipeline and A New Benchmark*, arXiv 2306.15884.
  - Separates protection-glass reflections ("symmetrical to the light
    source") from sensor / IR-cut glass reflections of diffracted light.
  - No code found.
* **Spot detection plus inpainting.**
  - Vitoria and Ballester, *Automatic flare spot artifact detection and
    removal in photographs*, JMIV 61(4) 2019.
  - Chabert, *Automated lens flare removal*, Stanford EE368 2015.
  - Both find bright compact spots and inpaint them. Inpainting replaces
    content instead of subtracting light; the right choice for a saturated
    spot, the wrong one for a +10..30 % ghost over sky we can still see
    through.
* **Veiling glare.**
  - Talvala, Adams, Horowitz, Levoy, *Veiling Glare in High Dynamic Range
    Imaging*, ACM TOG 26(3) (SIGGRAPH 2007). The glare spread function can be
    measured and deconvolved, but deconvolution is noise-limited; their
    working method needs an occlusion mask and several captures.
  - McCann and Rizzi, *Veiling glare: the dynamic range limit of HDR images*,
    Proc. SPIE 6492 (2007). Glare is scene-dependent and cannot be undone by
    multiple exposures.
  - Seibert, Nalcioglu, Roeck, *Removal of image intensifier veiling glare by
    mathematical deconvolution techniques*, Med. Phys. 12(3) 1985. The classic
    measured-PSF deconvolution.
  - Raskar, Agrawal, Wilson, Veeraraghavan, *Glare Aware Photography*, ACM
    TOG 27(3) (SIGGRAPH 2008). Needs a mask near the sensor.
  - Standards: ISO 9358:1994 (veiling glare index, glare spread function) and
    ISO 18844:2017 (digital camera flare measurement). A uniform veil is the
    VGI model; that is the kernel's `veil` term.

### 3.2 Learned

* **Wu, He, Xue, Garg, Chen, Veeraraghavan, Barron, *How to Train Neural
  Networks for Flare Removal*, ICCV 2021.**
  - Code: google-research/flare_removal, Apache-2.0.
  - Data: 5,001 flare images under CC BY 4.0.
  - **No pretrained model was released** ("due to licensing constraints").
  - Their reflective flares are captured from a smartphone lens, and they
    state the model "performs less well on images taken with an extremely
    different lens, such as a fisheye".
  - Idea we reuse: mask the saturated source and add it back. Our sun
    exclusion and the kernel leaving the sun disc untouched are the same
    principle.
* **Flare7K (NeurIPS 2022 D&B) and Flare7K++ (TPAMI 2024)**, Dai et al.
  - S-Lab License 1.0: "non-commercial purpose ... In the event that ...
    commercial purpose ... please contact". **Not shippable**, and neither is
    anything trained on them.
  - They model reflective flare separately: 2,000 synthetic ghosts.
* **Zhou et al., *Improving Lens Flare Removal with General-Purpose Pipeline
  and Multiple Light Sources Recovery*, ICCV 2023** (one author is from DJI's
  imaging group).
  - No LICENSE file; weights carry no licence. **Not shippable.**
  - Its light-source recovery weight W = ((I - min) / (max - min))^15 is a
    formula, not a model, and could be reimplemented.
* **Other checked datasets and code:**
  - Not shippable: FlareReal600 (CC BY-NC-SA 4.0), Flare-Free Vision
    (S-Lab), FlareX (GPL-3.0), PBFG (CC BY-NC-SA 4.0).
  - DeVeiler (CVPR 2026): code Apache-2.0, weights not released.

### 3.3 Multi-camera

* No published dual-fisheye or 360 flare-removal paper was found.
* GN-FR (Matta et al., BMVC 2024) removes flare with a multi-view NeRF; it is
  offline and no code was found.
* The practical dual-lens idea is patent territory. It comes in two forms.

### 3.4 Patents (not legal advice; for counsel)

* **GoPro US11330208B2** (active, expires 2038). Claim 1, verified: a flare
  model from the intensity-profile difference along the stitch line and the
  difference of "dark corner" profiles outside the two image circles, then a
  flare mask subtracted. Our `estimateVeil` uses the first element (overlap
  differences) and not the dark corners.
* **GoPro US10630921B2, US11503232B2, US12439177B2** (active; the last
  expires 2044). Claims as summarised by the research pass, not
  independently verified here:
  - a flare model from a second, overlapping camera;
  - the same on luma;
  - a per-channel flare value from the offsets of overlap bands reduced to
    1-D lines.
  The last is close to what `estimateVeil` does.
* **Apple US9860446B2**, *Flare detection and mitigation in panoramic
  images*. **Expired**, fee-related, January 2026 (verified). Steering the
  stitch seam away from flare is free to use; that is `flareCost()`.
* **Google US12033309B2** (active; inventors Wu et al.). It claims the
  synthetic-pair training method. Training our own model with it needs
  clearance.
* **Our ghost removal** is single-lens (detection plus parametric fit in one
  lens's own frame). It does not compare the two images, so the GoPro claims
  above do not describe it.

## 4. What was built

### 4.1 Pipeline

`include/osv/render/Flare.h`, `src/osv/render/Flare.cpp`:

1. **`flareDownsample`**: each lens is reduced to a factor-4 native-linear
   RGB working image (750 x 750 for 6K).
   - Each output pixel averages four decoded samples in linear light, a 2 x 2
     grid inside its 4 x 4 block (offsets 1 and 2 of 0..3), each decoded on
     its own (luma plus the co-sited 4:2:0 chroma) by the shared kernel
     function `osvFlareDownsamplePixel`.
   - Four samples instead of sixteen: the detector smooths at 1 working px
     and more, and the ghosts are tens of pixels wide, so the sparse grid
     loses nothing measurable. The same ghosts are kept on frames 0, 32 and
     64, and the pill's plateau after removal is +0.0018 against +0.0062
     with the full block mean.
   - The CPU path runs rows over the pool.
   - The GPU path (`FlareCuda.h`, `cuda/FlareKernel.cu`) runs the same
     function on frames that never left VRAM, and downloads 6.75 MB instead
     of the frame.
   - Time: 1.2-1.4 ms per lens on the GPU against 4.7-4.9 ms on the CPU pool
     (14-17 ms before the sparse grid).
2. **Sun**: the largest region within 92 % of the frame maximum must be
   compact (aspect <= 1.8, fill >= 0.5), and the maximum must be at least 6x
   the median. Otherwise there is no sun and nothing is removed.
3. **Seeds**:
   - Find relative band-pass bumps, (G_1 - G_12) / G_12 > 3 %, at least 3 sun
     radii from the sun, inside 95 % of the image circle, and within ±20 deg
     of the sun line on either side of the axis.
   - Keep a bump only where the surrounding texture is low (median relative
     detail <= 2 %) and the bump is at least 4x that texture. **A bump on
     scene texture is never a candidate**, which is what protects clouds, the
     ground and the aircraft.
   - Keep compact seeds of at most 2,000 analysis px².
4. **Fit** (per seed, in parallel), in two stages:
   - **Geometry.** Levenberg-Marquardt over a rotated rounded rectangle:
     centre, half extents, corner fraction, angle, edge softness. At each
     step the per-channel quadratic background and plateau amplitude are
     solved linearly (variable projection).
   - **Appearance.** At that geometry, the ghost gains a caustic rim (a
     compact bump on the edge) and a linear brightness tilt, refined within
     ±25 % of the flat fit.
   - Fitted jointly from the seed, the rim let the rectangle stretch past the
     aspect gate on two frames of three, hence the two stages.
   - The richer model raises the explained local variance from 0.78 to 0.86
     on the pill and from 0.61 to 0.83 on G3.
5. **Gates:** a ghost is accepted only if all of these hold. Every refusal
   is logged with its reason (`osv_flare_bench --verbose`).
   - aspect <= 3;
   - the fit did not run into its softness or size bounds;
   - mean added light over the plateau >= 4 % of the background;
   - the ghost terms explain >= 50 % of the variance over the footprint.
6. **Kernel** (`osv_kernel.h` `[WP-FLARE]` regions: types before `OsvRenderParams`,
   fields at its end, functions just before the shader; params block +708 B,
   2,068 B of the 4,096 B budget on today's main):
   - Per lens, up to 4 ghosts plus a uniform veil, subtracted in the lens's
     native linear light before gain and blend.
   - Subtraction is soft: f(x) = x - g x³ / (x³ + g³). It is the full x - g
     well above the estimate, never below 0.47 x, and has slope >= 0.16, so
     an over-estimate can dim but never clip or invert.
   - Each ghost ends exactly at its reach radius, so one compare rejects
     almost every pixel.
   - `flareEnabled == 0` renders bit-identically to before (the parity tests
     pass unchanged). The importer sets it per frame (section 6.1).
   - D-Log M passthrough output is not treated: it blends in code space and
     there is no device-side log inverse. The importer knows this and does
     not measure for such a clip; its analysis text says "on, not applied".
7. **`smoothFlare`**: temporal blending of matched ghosts. Over the whole
   clip at weight 0.5 it halves the pill's frame-to-frame jitter. The
   importer does not use it (section 6.1 says why):

   | | raw | smoothed |
   |---|---|---|
   | centre | 0.70 px | 0.38 px |
   | size | 0.88 px | 0.45 px |
   | plateau luma | 0.0073 | 0.0035 |
8. **Sun check** (`locateSun`, `locateSuns`, `flareSunsMatch`): the same sun
   detector on a coarse working image (factor `lensW / 375`, 8 at 6K). It
   tells the importer whether a measured model still describes a frame: a
   ghost is an image of the sun and moves with it, so a model is used only
   while the frame's sun is within `flareSunTolerancePx` (3 px at 6K) of the
   sun the model was measured with.
   - On the sample, the brightest ghost moves 0.45 px per px of sun motion,
     so within the tolerance a model is at most ~1.4 px off, under a quarter
     of its 6.6 px edge ramp.
   - Cost: 5.4-5.8 ms for both lenses on the CPU pool, 2.8-3.4 ms from
     device frames.

### 4.2 Tests

`tests/unit/test_flare.cpp`, 21 cases:

* **Kernel shape.** Plateau 1 at the centre, 0.5 on the edge, exactly 0 past
  the reach at every angle; rotation invariant.
* **Soft subtraction.** Never negative, never above the input, strictly
  increasing, >= 0.47 x, over a dense grid of six estimates.
* **Nothing outside is touched.** `osvFlareRemove` leaves every pixel outside
  a ghost bit-identical, and in the render kernel only the covered pixels
  change: none brighten and none go negative.
* **Synthetic recovery.** A planted ghost is recovered within:
  - centre ±0.3 px, extents ±0.5 px, angle ±0.03 rad, amplitude ±8 %;
  - rim ±0.004 and tilt ±0.003 when those are planted, and they come out
    ~0 when they are not.
* **Refusals.** Nothing is removed with no sun, on 25 % scene texture, or
  90 degrees off the sun line. The mirrored side of the axis is searched.
* **Synthetic subtraction.** More than 85 % of the ghost's light is gone and
  nothing outside its reach changes.
* **Downsample** block means, odd sizes and refusals. **CUDA downsample** =
  CPU within 1e-5 relative. **CUDA render with removal on** = CPU at
  PSNR >= 60 dB.
* **`applyFlare`** validation, clamping and 4-ghost cap. **`smoothFlare`**
  matching and fades.
* **`flareCost`** bounds, zero on clean pixels, signal dependence, sun glare
  and non-finite inputs. **`flareCostBand`** through the real rig.
* **`FlareSeamPenalty::hook`** puts a ghost placed in the overlap into the
  master's penalty map at exactly the `flareCost` value. It leaves every
  other pixel, the veil and the slave at 0, and refuses a missing model, a
  missing sun or wrongly sized maps. A `static_assert` pins its type to
  WP-SEAM's `SeamLensPenaltyFn`, and it installs in, and uninstalls from,
  the Flare slot.
* **`PrefsBlob`** (in `tests/premiere/common`): `flareRemoval` sits at
  offset 30; a corrupt byte and dirty padding are repaired; 1 survives.
* **`estimateVeil`** recovers an additive 0.04 against a 1.05 gain; returns
  nothing with no sun or with the sun in both lenses, and nothing when the
  sun lens is the darker one.
* **Sample clip.** The sun is found in the master only (a slave-lens glint
  used to pass until the "largest region must be compact" rule), and the
  pill is found at (1182, 1547).
* **Sun check.** `locateSun` returns the analysis' own sun to 1e-9 px;
  garbage in is "not found", never an error. `flareSunsMatch` needs the same
  lenses lit and each sun within the tolerance, and refuses a negative or
  NaN tolerance or position. The factor and tolerance scale with the lens.
  On the sample, the coarse check lands within 3 px of the full analysis'
  sun, frame 1 matches frame 0, and frame 64 (sun 15 px on) does not.

Through the built importer and engine (phase 2):

* `tests/premiere/importer/test_flare_importer.cpp`:
  - an export with the removal on changes pixels **only inside the fitted
    ghosts' footprints** (every changed equirect pixel maps, through the
    clip's rig, into a ghost of the library's own model of that frame; 3 px
    margin). None gets brighter than float rounding; the largest channel
    rise is below 1e-4;
  - off, a blob saved before the option existed, a draft request and the
    D-Log M passthrough output all render **bit-identically** to off;
  - the first interactive request comes back uncorrected, bit for bit (it
    did not wait), later ones pick up the background result, and an export
    of the stand-in's frame is never served the stand-in.
* `test_engine.cpp`: a device frame with the removal on carries the ghosts
  in its stitch block (`flareEnabled == 1`, the pill at (1182, 1547)), and
  after the switch is published off the next frame carries none.
* `test_importer_bitdepth.cpp`: the GPU frame path and the host path differ
  by float noise only with the removal on (264 pixels, worst 7.4e-6, none in
  the overlap band), and the removal really acted on that frame.
* `test_prefs_mapping.cpp`, `sourcesettings/test_params.cpp`,
  `sourcesettings/test_prefs.cpp`, `common/test_common.cpp`: the switch
  round-trips through the dialog mapping and the effect's parameters, the
  new clip default is on, and a blob from before the option is off.

## 5. Measurements (frame 0 unless stated)

`osv_flare_bench --reps 9 --sweep --out <dir>`. The visibility metric is
computed per ghost against a quadratic surface fitted to a ring of clean sky
(1.15-1.8 reaches out), which follows the sun's glow gradient:

* **contrast** is the plateau's mean excess over that surface;
* **footprint RMS** is the deviation over the whole footprint, rim included.

| View | Ghost | Contrast before -> after | Footprint RMS before -> after |
|---|---|---|---|
| sun, 100 deg, 1920 x 1080 | pill | +0.2344 -> +0.0018 | 0.1345 -> 0.0506 |
| sun, 100 deg | G2 | +0.0716 -> -0.0044 | 0.0415 -> 0.0243 |
| pill close-up, 30 deg, 1280 x 720 | pill | +0.2320 -> +0.0001 | 0.1329 -> 0.0497 |
| equirect 4096 x 2048 | pill | +0.2312 -> -0.0006 | 0.1326 -> 0.0495 |
| equirect | G2 | +0.0900 -> +0.0129 | 0.0472 -> 0.0213 |

(Phase 1, with the full 4 x 4 block mean, measured the pill at +0.0062 /
+0.0045 / +0.0038 after removal; the sparse grid is no worse.)

**No damage elsewhere.** Pixels changed outside the fitted footprints: 0 of
2,011,781 (sun view), 0 of 804,371 (close-up) and 0 of 8,363,476
(equirect). Sun disc: 0 changed in every view. Clouds and the aircraft are
never inside a footprint, because seeds on texture are refused.

**Ghosts per frame:**

| Frame | Kept | Master's candidates | Missing |
|---|---|---|---|
| 0 | 2 | 7 | G3 (local R² 0.45, gate 0.5) |
| 32 | 3 | 3 | - |
| 64 | 3 | 5 | - |

* G4 is never kept: its fitted plateau is not brighter than its background.
* G0 is never a candidate: it sits on texture.
* Over all 65 frames the pill is found every time.
* The CPU and GPU paths find the same ghosts on every frame measured.

**Runtime** (shared machine; median of 9):

* `analyseFlare` per frame pair (frames 0 / 32 / 64):
  - CPU path: 46 / 67 / 63 ms;
  - GPU path (NVDEC frames in VRAM): 36 / 58 / 54 ms.
  - Phase 1 measured 89-141 and 63-174 ms. The sparse working image and a
    size cap on seeds (a 204 px candidate on one frame cost 70 ms and was
    never a ghost; candidates wider than 160 stream px are now skipped)
    account for the difference.
* Downsample per lens: CPU 4.7-4.9 ms (pool), GPU 1.2-1.4 ms.
* Detection plus fits on the master: 31-52 ms pooled, 91-151 ms serial.
  Fits dominate.
* Sun check, both lenses: 5.4-5.8 ms CPU, 2.8-3.4 ms GPU.
* Importer schedule, in order over the 65 frames (the sun moves 15.1 px):
  6 measurements, at frames 0, 9, 20, 34, 43 and 60.
* CUDA render with the removal off / on: 6.10 / 6.08 ms (1920 x 1080),
  4.86 / 4.86 ms (1280 x 720), 12.30 / 12.32 ms (4096 x 2048). The
  difference is within the timing noise.
* Through the importer, the first interactive frame with the removal on
  (decode, sun check, working images handed to the worker, stitch, copy)
  took ~75 ms in the test harness, and came back uncorrected.

**Crops** in `research/flare/`:

* `sun_view_before_after.jpg`: the 100-degree sun view at native Rec.709,
  before and after.
  - The pill goes from an obvious white lozenge to a faint outline.
  - G2 (upper left) and G3 (lower left) disappear.
  - The sun, its star, the cloud bank and the sky gradient are unchanged.
* `sun_view_stretched.jpg`: the same with contrast stretched, so the faint
  ghosts and the pill's residual rim show.
* `sun_view_removed_x8.jpg`: exactly what was subtracted, x8. Two compact
  shapes and nothing else.
* `pill_closeup_before_after.jpg`: the pill at 30 degrees.
  - The plateau and most of the rim are gone.
  - What remains is its irregular caustic structure (the last 5 % RMS), which
    no smooth parametric shape can model.
* `equirect_sky_before_after.jpg`: the sky hemisphere in three versions:
  before, ghosts removed, and ghosts plus the overlap veil (research only).
  - The veil version brings the master's half of the sky (right of the seam)
    down to the slave's tone.
  - The large rounded "rectangle" of sky brightness stays, because it is the
    sky.

## 6. How it is wired (phase 2)

### 6.1 Removal: the importer and the direct path

One `FlareStage` per clip (`plugins/importer/FlareStage.h`) decides, for
every frame the importer stitches, which fitted model to subtract, and hands
it to the frame's `RenderParamsBuilder` (`RenderParamsBuilder::flare()`).
The model travels inside `OsvRenderParams`, so the CPU, CUDA and OpenCL
renderers and the direct kernel subtract it with no further plumbing. The
single call site is `ImporterInstance::applyAnalyses`, which the equirect
path (host and GPU frame paths) and the direct path share:

```cpp
// ---- [WP-FLARE] sun ghost removal (FlareStage.h) --------------------------
// Before the carve, which reads this frame's model through the penalty.
const FlareStage::Outcome flare = m_flare.apply(index, pair, m_rig, m_color,
        m_prefs.flareRemoval != 0, draft, exactWanted, pool, builder,
        m_path.filename().string());
frameExact = frameExact && flare.exact;
```

**The schedule** is the parallax grid's, plus one rule of its own:

* Models are cached per bucket (`render::parallaxBucket`, 8 frames) and
  measured on the frame of the bucket that is asked for first.
* **A model is usable only while the frame's sun is where the model saw
  it.** Every wanted frame runs the sun check (section 4.1, step 8) and
  takes a model whose check matches within 3 px (at 6K). A stale model
  would cut a dark copy of the ghost into clean sky. A matching model is
  usable in any bucket: a frame whose own bucket has none adopts the nearest
  match within 4 buckets. A steady shot therefore measures once, and a pan
  measures wherever the sun has moved on (6 times over the sample).
* **Exact requests** (export, a paused frame, every direct-path frame) with
  no usable model measure it now, on the render pool, and wait: 36-67 ms.
* **Interactive requests** (playback, scrubbing) never wait. Without a
  usable model the frame renders without removal and is marked non-exact,
  so an export never reuses it. The working images go to a background
  worker (one slot, latest wins) that never takes the instance lock. The
  render thread pays the sun check and the working images; the fits never
  land on a park or a scrub.
* **A frame keeps its first answer.** The model given to a frame is
  remembered per frame, so the frame renders the same every time, whatever
  is measured later, and a repeat render skips the sun check.
* **No glide between buckets.** `smoothFlare` halves the jitter (section
  4.1, step 7), but a blend depends on which neighbours happen to be cached.
  The direct path asks for every frame as Exact and relies on an Exact frame
  being the same whatever was rendered before it. A history-dependent glide
  broke that in `test_direct_e2e` (0.005-0.011 differences), so the importer
  subtracts each frame's own model. Within the 3 px tolerance the pill's
  jitter is 0.7 px, a tenth of its 6.6 px edge ramp.
* Draft requests (Low quality, playback below real time) skip it entirely.
* A prefs change resets the stage and discards a measurement in flight;
  releasing the clip's heavy state stops the worker.

**Device frames.** `ensureGpuAnalyses` calls `installCudaFlareSampler()`
next to the GPU analyses, so the direct path's frames, which never leave
VRAM, are analysed on the GPU (1.2-1.4 ms per lens). Without it, those
frames render without removal and the log says so. The two frame paths'
fits agree to float noise (section 4.2).

**The switch.** `PrefsBlob::flareRemoval` (offset 30 of WP-FLARE's range
30-31; `padAfterFlare` at 31 is kept for the veil):

* `PrefsBlob::defaults()` sets it to 1: **on for new clips**. A blob saved
  before the option existed holds 0 there and stays **off**, so an existing
  project renders as before.
* The Source Settings effect has a "Sun Ghost Removal" checkbox in the
  Stitching group after Calibration: persistent id 16
  (`OSV_SS_ID_FLARE_REMOVAL`), default on. The Rec.709 look (WP-LOOK) is
  id 15; indices after Calibration moved up by one, ids did not.
* The importer's Source Settings dialog has one appended row, "Sun ghost
  removal (subtracts the sun's lens reflections)", `IDC_FLARE_REMOVAL`
  1018, next to WP-LOOK's 1030 / 1130.
* The D-Log M passthrough output is not treated (section 4.1, step 6), so
  for such a clip the stage does not measure, and the analysis text says
  "Sun ghost removal: on, not applied (...)".

**The log**, once per clip (and again after a settings change), names what
was removed or why nothing was. The first line is from a test run on the
sample; the others show the formats of the other outcomes:

```
flare: 'example_footage_dlogm.OSV' frame 3: master lens: sun 10.0 deg off axis, 2 ghosts removed (+23% at (1183, 1548), +7% at (833, 1290)); slave lens: no sun
flare: 'CAM_0001.OSV': no sun in either lens at frame 12; nothing to remove
flare: 'CAM_0001.OSV': sun ghost removal is off in Source Settings
flare: 'CAM_0001.OSV': the D-Log M passthrough output is not treated (it blends in log code); rendering without ghost removal
```

A lens with a sun but no accepted ghost reads "sun X deg off axis, no
reflection passed the checks (N candidates)"; a failed sun check or analysis
names the error, and the frame renders without removal.

The library calls underneath, for any other caller:

```cpp
#include "osv/render/Flare.h"

auto model = analyseFlare(rig, framePair, color, FlareParams{}, pool);
auto suns = locateSuns(rig, framePair, color, FlareParams{}, pool);  // cheap
bool same = flareSunsMatch(sunsOfModel, suns, flareSunTolerancePx(lensW));
builder.flare(model.value());        // or applyFlare(model, params)
```

`color` only needs the right input encoding and curve: the model lives in
native linear light, before the colour matrix and exposure, so an exposure
or output change does not invalidate it.

### 6.2 The seam cost hook (WP-SEAM)

WP-SEAM's carve takes per-lens penalty maps from `SeamLensPenaltyFn` hooks
(`SeamCarve.h`). `FlareSeamPenalty::hook` has exactly that signature. Its
`user` pointer is a `FlareSeamPenalty` that holds the latest model, swapped
atomically so a background carve never sees a half-written one.

**Registered per clip**, not in the process-wide slot, so two open clips
never steer each other's seams:

```cpp
render::SeamCarveParams params;
params.penalty = m_flare.seamPenalty();  // {&FlareSeamPenalty::hook, &stage's source, 1.0}
```

`FlareStage::apply` updates that source with the model it gave the frame,
and clears it when the frame has none. It runs before the carve in
`applyAnalyses`, so the carve always sees the same frame's model.

* **What it fills:**
  - `flareCost()` per band pixel: fitted ghost light, plus a glare term that
    is 1 on the sun disc and falls to 0 at 4 sun radii, divided by
    (flare + 0.18).
  - About 0.2 on the pill, about 1 on the sun, 0 elsewhere. Against the
    seam's scale ("~0.05 per row is decisive"), weight 1 steers firmly.
  - 0 where a lens does not see the pixel, because coverage is the seam's own
    cost.
  - The veil is off for the seam (`seamDefaults()`): it is uniform over a
    lens, so moving the seam cannot remove it.
  - Where both lenses see a ghosted direction, the seam puts the clean lens
    there, which is Apple's expired US9860446B2.
* **On this clip it will not fire.** Every ghost is 21-47 degrees from the
  master's axis, far from the overlap at 88-97 degrees. It matters when the
  sun is near the seam.
* **Do not treat rim darkening as flare:** that is WP-PHOTO's rim map.

The pure pieces underneath, for other callers:

```cpp
// Fraction of the light at a lens pixel that is predicted flare, [0, 1).
float flareCost(const LensFlare& lens, double px, double py,
                double signal = -1.0, const FlareCostParams& = {}) noexcept;

// The same over a polar-axis band (LensBands geometry), both lenses; a lens
// that does not see a pixel gets 1 there.
Status flareCostBand(const FlareModel&, const geom::LensRig&, uint32_t mapW,
                     uint32_t mapH, uint32_t row0, uint32_t rows,
                     const std::array<const std::vector<float>*, 2>* signal,
                     std::array<std::vector<float>, 2>& cost,
                     const FlareCostParams& = {});
```

### 6.3 The veil (lead + counsel)

* `estimateVeil(renderLensBands(..., linear = true, ...), model)` returns a
  neutral per-lens veil. Put it into `model.lens[i].veil` and `applyFlare`
  carries it.
* Apply it **before** estimating gains. Otherwise the gain estimators
  (`estimateGain`, WP-PHOTO's field) absorb part of the additive veil as a
  ratio, and the two corrections overlap.
* **Do not wire it until the GoPro patents (section 3.4) are cleared.**

## 7. Limits and what would move them

* **Structured residue** is the last 5 % RMS of the pill: the caustic
  texture, the inverted rainbow ghost G0 on clouds, and the diffraction star.
  A smooth parametric ghost cannot model it. The shippable routes:
  1. **Per-lens ghost calibration.** Film a bright point source swept across
     each lens, fit magnification and offset per reflection path, and store
     a small template per path. That turns detection into prediction plus
     local fitting and would catch ghosts on texture. It needs clips we do
     not have. The sun moves 16 px in the sample.
  2. **A learned remover trained on shippable data.** Wu et al.'s Apache-2.0
     pipeline, their CC BY 4.0 flare images, plus our own captured Osmo 360
     reflective flares. It needs a legal check against Google US12033309B2
     and a training effort. No existing model is usable (section 3.2).
* **The glow around the sun** (sky aureole plus lens glare) is left alone.
  Deconvolving it is noise-limited (Talvala 2007), and some of it is real
  sky.
* **G3 at frame 0 and G4** fall to the gates. That is the conservative
  choice: a missed faint ghost costs a little visibility, a wrong fit
  subtracts scene.
* **Passthrough (D-Log M) output** is not treated.
* **Interactive frames can go without removal** for as long as the worker
  takes (tens of milliseconds): the first played frame at a new sun
  position, and a scrub to one. They are marked non-exact, and the paused
  or exported frame is always corrected.
* **Exact frames at a new sun position wait for the analysis** (36-67 ms).
  The direct path asks for every frame as Exact, so a playing direct view
  pays that at every new sun position (6 times over the sample's 65
  frames). Reusing a model across a larger sun motion would need the ghost
  to be moved with the sun (a per-path magnification, see the calibration
  route above) instead of re-measured.
* **No temporal smoothing in the importer** (section 6.1); the pill's
  frame-to-frame jitter stays at the raw 0.7 px.

## 8. Files

| File | What |
|---|---|
| `include/osv/render/Flare.h`, `src/osv/render/Flare.cpp` | analysis, veil, kernel parameters, seam cost hook |
| `include/osv/render/FlareCuda.h`, `src/osv/render/cuda/FlareCuda.cpp`, `FlareKernel.cu`, `FlareLaunch.h` | the GPU sampler |
| `include/osv/render/osv_kernel.h` `[WP-FLARE]` regions | `OsvFlareGhost` / `OsvFlareLens`, the shader hook, `osvFlareRemove`, `osvFlareDownsamplePixel` |
| `plugins/importer/FlareStage.h`, `FlareStage.cpp` | the importer's schedule, log and seam source |
| `plugins/importer/ImporterInstance.cpp` `[WP-FLARE]` lines | `applyAnalyses`, the carve's penalty, the cache key, the sampler install |
| `plugins/common/PrefsBlob.h`, `plugins/importer/{resource.h, OpenOSVImporter.rc, SourceSettingsDialog.cpp, PrefsMapping.cpp}`, `plugins/sourcesettings/*` | the switch |
| `tests/unit/test_flare.cpp` | 21 test cases |
| `tests/premiere/importer/test_flare_importer.cpp` | the importer end to end |
| `tests/bench/FlareBench.cpp` | `osv_flare_bench` (not in ctest); `--sweep` replays the importer's schedule |
| `research/flare/` | crops and `make_crops.py` |
