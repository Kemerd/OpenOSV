# Colour pipeline

All per-pixel colour math lives in `include/osv/color/ColorMath.h`, written
in the C subset shared with the GPU kernels. The pipeline for D-Log M input:

```
10-bit YCbCr (narrow)  ->  R'G'B' D-Log M code   (BT.709 matrix, (Y-64)/876, (C-512)/896)
D-Log M code           ->  scene-linear, native primaries, 18 % grey = 0.18
blend / gain           ->  (in linear light)
native -> Rec.2020     ->  3x3 matrix
x sceneScale x 2^stops ->  BT.2408 anchor: 0.2674 puts 18 % grey at HLG 0.38
transfer               ->  HLG OETF | HLG-OOTF(1000 nit, gamma 1.2) + PQ inverse EOTF
                           | Rec.709: the DJI Studio look (default, see below) or the
                             standard 2020->709 in linear light + HLG OETF | linear | passthrough
```

## D-Log M curves

DJI publishes no formula for D-Log M. Three analytic 7-parameter curves of the
form

```
tmp = 2^(scale * code + yShift) + xShift
lin = (tmp < cut ? tmp * slope + intercept : tmp * slope2) * midGrayScaling
```

are shipped, all with the branch cut defined as the intersection of the two
branches so the curve is continuous:

| Curve | CLI name | Origin | Grey (code 0.40) | Code 1.0 |
|---|---|---|---|---|
| `kDlogMOsmo360` (**default**) | `osmo360` | least-squares fit to the 33 neutral-axis samples of DJI's own **Osmo 360** D-Log M to Rec.709 LUT (`scripts/fit_dlogm.py --from-cube`) | 0.180 | 3.765 |
| `kDlogMDjiRefit` | `dji` | least-squares fit to 64 neutral-axis measurements of a Pocket-3-era D-Log M to HLG rendering | 0.180 | 3.429 |
| `kDlogMPocket3` | `pocket3` | public Pocket 3 fit constants (Thatcher Freeman) | 0.180 | 2.47 |

All three pin 18 % grey: code 0.400 -> scene-linear 0.180 -> HLG 0.380
(BT.2408). The toe below code ~0.24 is crushed 8-bit data in DJI's LUTs and is
weighted low in every fit.

### Why the default changed to `kDlogMOsmo360`

`kDlogMDjiRefit` was fitted before any Osmo 360 reference existed, against
Pocket-3-era D-Log M -> HLG measurements. Measured against DJI's own Osmo 360
LUT it is up to 0.30 stops off through the upper mids and 0.66 stops too bright
in the toe. Neutral-axis error against that reference, in HLG code units:

| Curve | RMS (all 33) | worst | RMS (code >= 0.24) |
|---|---|---|---|
| `kDlogMDjiRefit` | 0.0318 | 0.0659 (code 0.094) | 0.0259 |
| `kDlogMOsmo360` | 0.0233 | 0.0462 (code 0.063) | 0.0160 |

Both figures are dominated by the bottom four samples, which are the crushed
part of the reference. `kDlogMDjiRefit` is kept verbatim and still selectable
as `--fit dji`, so a project already graded against it renders unchanged;
`--fit dji` deliberately does **not** follow the default for that reason.

0.0160 RMS is the ceiling of this seven-parameter family for this data, not a
solver failure: relaxing the slope-ratio bound from 3 to unbounded (ratio 40.8)
changes the RMS above code 0.24 by 0.00002. A closer match needs a different
functional form, which would cost the closed-form inverse.

### Comparing DJI's Rec.709 rendering with ours

DJI publishes a D-Log M -> Rec.709 LUT but not an Osmo 360 HLG one, so the fit
had to be done against a Rec.709 reference. That is sound here without any
inversion, because **the standard Rec.709 rendering (`Look::Standard`) is the
HLG signal in Rec.709 primaries** (see "Rec.709 output" below). Since the DJI
Studio look became the Rec.709 default, this section describes the standard
rendering only - the curve is fitted and tested through it. Reading the
standard `OSV_TRANSFER_REC709` branch of `osvLinearToOutput` for a neutral
input (R == G == B):

```
working = nativeToWorking * (lin, lin, lin)     -> (k*lin, k*lin, k*lin)
working = working * sceneScale                     (0.2674)
tmp     = workingToOutput * working                (2020 -> 709, in linear light)
out     = HLG_OETF(tmp)
```

The rows of a primaries-conversion matrix sum to 1 for an equal-energy triple,
so on the neutral axis both matrices are the identity and the chain collapses
to

```
out(code) = HLG_OETF(sceneScale * lin(code))
```

which is exactly the HLG branch. For neutrals the standard 709 and HLG outputs
are the same function, so a 709 reference LUT's diagonal is usable directly as
HLG-signal targets. `tests/unit/test_color.cpp` asserts this identity to 2e-6
rather than assuming it (with `Look::Standard` named explicitly).

This was also checked the other way round: inverting DJI's diagonal through
`HLG_OETF^-1 / 0.2674` recovers a smooth, strictly monotonic scene-linear curve
reaching 3.74 at code 1.0 with 18 % grey at code 0.406, so the curve refit was
the right first step. It was not the whole story, and the conclusion drawn
here at the time - that DJI's rendering "really is" HLG-in-709 and not a tone
map - was withdrawn once the rest of the file was measured: on the neutral
axis alone a different log curve and a tone map are indistinguishable, and
the other 35904 entries show a look (a crushed toe, S-shaped mid-tones, a
highlight shoulder with slope 0.54 at code 1.0 against the HLG rendering's
0.83, per-channel shadow saturation, gamut compression). See "The DJI Studio
look" below.

The apparent mismatch that prompted this (their 0.5 -> 0.487 against our
0.5 -> 0.526) was mostly the old curve: the new curve gives 0.5 -> 0.506 through
the standard rendering and is within 0.030 everywhere above the crushed toe.
The default Rec.709 output, the DJI Studio look, gives 0.5 -> 0.487.

### Anchors

| Curve | code 0.400 -> HLG | code 0.714 -> HLG |
|---|---|---|
| BT.2408 nominal | 0.380 | 0.750 |
| DJI's Osmo 360 LUT | 0.3882 (at code 0.40625) | 0.7404 (at code 0.71875) |
| `kDlogMOsmo360` | 0.3800 (pinned) | 0.7433 |
| `kDlogMDjiRefit` | 0.3800 (pinned) | 0.7548 |

Grey is pinned exactly in both fits. Diffuse white is a fit result: the new
curve's 0.7433 is 0.0067 below BT.2408's nominal 0.750, but DJI's own file
reads 0.7404, so it is **closer to the camera manufacturer's placement** than
the old curve's 0.7548. `kDlogMOsmo360` reaches HLG 1.0012 at code 1.000, which
the output clamp flattens to 1.0; only codes above 0.9986 are affected, and
DJI's own LUT likewise reaches exactly 1.0 at code 1.0.

## Shipped LUTs

`luts/` holds three 65^3 .cube files for NLEs that cannot load OpenOSV:

| File | Output | code 0.40625 | code 0.71875 |
|---|---|---|---|
| `OpenOSV_Osmo360_DLogM_to_Rec2100_PQ.cube` | BT.2100 PQ | 0.3849 | 0.5794 |
| `OpenOSV_Osmo360_DLogM_to_Rec2100_HLG.cube` | BT.2100 HLG | 0.3873 | 0.7479 |
| `OpenOSV_Osmo360_DLogM_to_Rec709.cube` | BT.709, DJI Studio look | 0.3847 | 0.7457 |

(Grid points 26 and 46 of 65, the nearest to the 18 % grey and diffuse-white
codes. The Rec.709 LUT carries the default DJI Studio look, whose grey scale is
DJI's own: DJI's file reads 0.3882 and 0.7404 at these two grid points, and the
standard rendering would give the HLG row's 0.3873 / 0.7479. PQ's 0.5794 is
against BT.2408's 0.5807 for 203-nit reference white.  `osvtool lut --look
standard` bakes the standard Rec.709 rendering instead.)

These are **build output, not source**. Regenerate them with

```
scripts\gen_luts.ps1              # rewrites all three from the built osvtool
scripts\gen_luts.ps1 -Check       # fails if the committed files are stale
```

Never edit one by hand. `tests/unit/test_cube.cpp` regenerates each in-process
and compares byte for byte against the committed copy, so a curve change that
is not followed by a re-run fails the build instead of shipping a stale table.

Every byte is generated by our own `osvtool lut` from our own fitted curve (and,
for Rec.709, our own fitted look); no DJI LUT data is redistributed (see NOTICE).

`scripts\install_plugins.ps1` copies them into

```
C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\LUTs\
```

Point Lumetri at that folder with **Basic Correction > Input LUT > Browse...**
or **Creative > Look > Browse...**. Apply one only to a clip the importer
delivered as **D-Log M passthrough** -- never on top of a PQ, HLG or Rec.709
output, which is already converted.

## Standards used

* BT.2100 HLG: a = 0.17883277, b = 0.28466892, c = 0.55991073; OOTF with
  Ys = 0.2627 R + 0.6780 G + 0.0593 B, gamma 1.2 at 1000 nit.
* BT.2100 PQ: m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375,
  c2 = 18.8515625, c3 = 18.6875. Check values: 26 nit -> 0.380, 100 -> 0.5081,
  203 -> 0.5807, 1000 -> 0.7518.
* BT.2408: 18 % grey = 38 % HLG/PQ = 26 nit; HDR reference white = 75 % HLG /
  58 % PQ = 203 nit.
* Rec.709 output: by default the DJI Studio look ("The DJI Studio look"
  below). The standard rendering, `Look::Standard`, is the HLG signal as the
  SDR rendering (BT.2390, "HLG on an SDR display"), computed in Rec.709
  primaries: grey lands at 38 %, diffuse white at 75 %, the camera clip near
  99 %. The standard rendering matches DJI's own D-Log M to Rec.709
  rendering far better than a peak-to-peak tone map, which came out washed out.
  Checked against DJI's Osmo 360 file, which reads 0.3882 at code 0.40625 and
  0.7404 at code 0.71875 -- see "Comparing DJI's Rec.709 rendering with ours"
  above for the algebra, and "The DJI Studio look" below for why the default
  moved on from it.

## The DJI Studio look (the Rec.709 default)

DJI Studio renders an Osmo 360 D-Log M clip through its bundled
`DJI Osmo 360 D-Log M to Rec.709 V1.cube`: the clip's automatic "D-LOG M"
filter (slug `LOG_Osmo360_DLogM`, service `mika.lut2`, strength 1.0, recorded
in DJI Studio's project files). The file is byte-identical to DJI's Pocket 3
D-Log M LUT; its header dates it to the Mavic 3 Pro. It is a **look**, not a
colour-space conversion, and the standard rendering was measurably not it:

| Against DJI's LUT, dE2000 (BT.1886 display) | mean | p95 | max |
|---|---|---|---|
| Standard rendering, whole 33^3 input cube | 2.81 | 6.57 | 15.8 |
| Standard rendering, the sample clip's pixels | 2.15 | 4.86 | 6.1 |
| **DJI Studio look**, whole cube | **1.23** | **2.80** | **6.1** |
| **DJI Studio look**, the sample clip's pixels | **0.50** | **1.18** | **3.1** |
| DJI Studio look, neutral axis | 0.13 | - | 0.42 |

What the look does, in plain terms: blacks crushed like DJI's (code 0.0625
renders 0.001; DJI 0.0115, standard 0.058); darker upper mid-tones (code
0.5625: 0.554, standard 0.584); a real highlight shoulder (top slope 0.63,
DJI 0.54, standard 0.83); a more saturated blue sky (sky chroma 33 against
28; DJI 33); dark colours far more saturated than their mid-tone versions,
bright oranges that stay orange, and colours outside Rec.709 compressed rather
than clipped.

### The model

`osvLookApply` (ColorMath.h, shared with the kernels), stage by stage:

```
x   = toLook * working          3x3, rows sum to 1 (Rec.2020 light in)
u_c = shaper(x_c)               kDlogMOsmo360 inverted: light -> D-Log M code,
                                continued linearly below code 0
y_c = T(u_c)                    monotone cubic Hermite, 17 uniform knots
y  += a * (yh - y)              highlight hue preservation (smoothstep on the
                                tone of max(x); yh = that tone * ratio^e)
y   = display * y               signal-space 3x3, rows sum to 1
y   = compress(y)               per-channel soft gamut compression below
                                max(y) (the ACES reference gamut compression
                                curve)
out = clamp(y, 0, 1)
```

Both matrices have unit row sums and the two colour stages leave neutrals
alone, so the neutral axis is exactly T - DJI's own grey scale, knot for knot.
Each stage is there because it bought a measured improvement; additive-model
tests showed DJI's table is not separable in any single domain (code, linear
light, display signal), which is why every curve-plus-matrix model plateaued
near 2 dE2000.

### Provenance

`include/osv/color/Look.h` ships 46 constants (38 free parameters) fitted by
`python scripts/fit_look.py --samples <equirect D-Log M frames>`: least
squares in CIELAB over all 35937 entries of DJI's file plus 20000 of the
sample clip's pixels, with the neutral axis weighted in. No LUT data is
shipped (see NOTICE); 105 measured entries are kept in
`tests/unit/DjiReference.h` as the tests' reference. The C++ reproduces the
script's model to 1.3e-6.

Checked properties (`tests/unit/test_look.cpp`): the neutral axis is strictly
increasing; no hue ever renders darker as its exposure rises (worst drop
8.8e-6 of display light over 400 random ramps from -9 to +5 stops); every join
is continuous; hostile input (NaN, infinities, 1e6, negatives) stays finite in
[0, 1]; CPU / CUDA / OpenCL agree at 123.7 / 120.3 dB.

### Where it applies, and selecting it

Only the Rec.709 output has a look. **HDR has none**: DJI Studio ships no
colour-managed HDR rendering for the Osmo 360 to match - its only D-Log M HDR
asset is an optional creative style (`FT_StyleGeneralDlogm2HLG`: an 8-bit 64^3
PNG LUT, 80 % default strength, blacks lifted to 3/255), the same source the
`kDlogMDjiRefit` measurements came from. PQ and HLG keep the standard
rendering, bit for bit.

| Where | Control |
|---|---|
| Source Settings effect | **Look (Rec. 709 only)**: DJI (default) / OpenOSV standard, under Colour Output |
| Importer Source Settings dialog | **Rec.709 look** (greyed unless Colour output is Rec.709) |
| Preference blob | `PrefsBlob::look`, offset 28: 0 = DJI (the default, and every older project), 1 = standard |
| `osvtool render` / `osvtool lut` | `--look dji` (default) / `--look standard` |
| Library | `makeColorParams(..., Look)`, `setLook`, `lookOf`, `parseLook` |

A look change behaves like any colour setting: it is part of the PPix cache
key (the whole blob), rebuilds the importer's colour block and bumps the
engine's Source Settings generation, so the direct path follows it. In the
everyday setup - a Rec.709 sequence holding PQ clips - the direct path renders
the clip straight into the working space, so the Program monitor shows this
look.

### The sun ghost

The look does not remove the faint rounded rectangle around the sun; DJI's
own LUT does not either. Measured on frame 20 of the sample clip, the ghost
stands out from the sky around it by 5.97 dE2000 in the standard rendering,
5.41 through DJI's LUT and 5.59 with the look, and a +10 % light step in the
sky is 1.71 / 1.76 / 1.68. The remaining difference to DJI Studio's picture
is photometric (lens gain, veiling glare, the seam field), which is
WP-PHOTO's and WP-FLARE's work, not the tone transform's.

## Native primaries

The Osmo 360's native -> Rec.2020 matrix (`kNativeToRec2020_Osmo360`) is
fitted, through the standard rendering, from DJI's own Osmo 360 D-Log M ->
Rec.709 reference LUT, measured over
all 35937 entries by `scripts/fit_primaries.py`. A colour chart is not needed:
a per-channel tone curve cannot move energy between channels, yet the
reference plainly does -- a red-only input of 0.500 renders with 0.053 of
blue -- and cross-channel terms of that shape are exactly what a primaries
matrix produces, so they are recoverable by inverting the output transform and
solving for the 3x3.

Against that reference, in HLG code units, replacing the Pocket 3 matrix with
this one cuts the full-cube error by 52.5 % (0.0947 -> 0.0450 RMS) and the
saturated-entry error by 53.6 % (0.1002 -> 0.0465 RMS). The neutral axis is
unchanged at 0.0233 RMS, which is structural rather than lucky: every matrix
here has rows summing to 1, so it acts as the identity on equal-energy greys
and cannot move 18 % grey off HLG 0.380 or disturb any BT.2408 anchor.

The fit is physically plausible -- determinant +0.873, positive diagonal, all
three implied primaries at positive luminance (R x=0.6914 y=0.3206,
G x=0.2616 y=0.8225, B x=0.1448 y=0.0372), a gamut between Rec.709 and
Rec.2020 -- unlike the Pocket 3 matrix, whose implied blue primary sits at
negative luminance and therefore cannot describe a real sensor.

What it does not reproduce, stated plainly: about 37 % of the reference's
entries sit on an output boundary and roughly 71 % of the cube falls outside
the Rec.709 output gamut, so on deeply saturated entries DJI's table holds a
*gamut-mapped* value rather than a matrixed one. No 3x3 can reproduce that,
because it is not a linear operation, and those entries dominate the residual
worst case (0.263). Restricting the fit to the ~24 % of entries that are clean
on both sides does not improve it, which is the evidence that the remaining
error is DJI's gamut compression and not a mis-fitted matrix.

The matrix remains a parameter: `--fit pocket3` selects the older one, since a
project already graded against it must keep rendering the same way, and other
DJI bodies may genuinely use those primaries.

## Signalling

| Output | Container `colr` nclx | ffmpeg flags |
|---|---|---|
| HLG | 9 / 18 / 9 | `-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc` |
| PQ | 9 / 16 / 9 | `-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc` |
| Rec.709 | 1 / 1 / 1 | `-color_primaries bt709 -color_trc bt709 -colorspace bt709` |

The camera tags its D-Log M files as 1/1/1; the container is wrong and is
ignored. `StreamMeta.color_mode` (19 = D-Log M, 9 = HLG, 0 = Normal) is the
truth, with a luma-histogram fallback when the metadata is missing.

## Input encoding vs output transfer (the double-conversion rule)

These are two independent decisions and must stay that way:

* the **input encoding** follows the CLIP's `color_mode`, always
  (`color::inputEncodingForColorMode`);
* the **output transfer** follows the USER's preference
  (`PrefsColorOutput`, default PQ).

Deriving one from the other is the one genuinely destructive mistake available
here. An HLG or Normal clip decoded with the D-Log M curve is double-converted
-- a de-log applied to a signal that was never logged -- and no downstream
grade recovers it. The damage is also easy to miss: at mid grey the two
decoders agree to within 0.5 stops, because both encodings anchor 18 % grey by
construction. It is the *contrast* that is wrong, by ~0.8 stops across the
range, so the symptom is "fine until the shadows and the sky".

One rule, one implementation: `osvtool --input-encoding auto` and the importer
both call `color::inputEncodingForColorMode`, so the same clip cannot render
differently through the two front ends. `Unknown` and the modes with no curve
of their own (D-Cinelike, Vivid, D-Log, D-Log2) resolve to D-Log M, the mode
this container overwhelmingly carries.

What the importer does for each source mode, at the default PQ output:

| `color_mode` | Input encoding | Output transfer | Declared to Premiere |
|---|---|---|---|
| 19 D-Log M | D-Log M (curve applied) | PQ | `kPrOverranged2100PQ` |
| 9 HLG | HLG (inverse OETF; **no** log curve) | PQ | `kPrOverranged2100PQ` |
| 0 Normal | Rec.709 (inverse OETF; **no** log curve) | PQ | `kPrOverranged2100PQ` |

So "auto PQ for D-Log M footage" is the default, and a non-log source is
converted from its own encoding rather than being treated as log. Changing the
output preference changes the last two columns only; the input encoding column
never moves.

Every clip logs one line at open recording exactly this, so an unexpected
preview is answerable from a support log without reproducing it:

```
colour: 'CAM_....OSV': source D-Log M (from metadata) -> input encoding dlogm,
        output pq (Rec. 2100 PQ Full)
```

`(inferred from luma statistics)` in place of `(from metadata)` means the
metadata was missing and the histogram fallback decided.

## "Color Recovery"

DJI's "Color Recovery" option only affects the live-view preview; recorded
D-Log M files are unchanged and carry no flag for it.

## Lens shading

The .OSV metadata carries no lens shading table (only a shading mode number),
so whatever shading correction the camera applies has already happened when
the frames are decoded.  What remains near the rim is measured from the
footage: the photometric seam field evens the two lenses where both see the
sky, and the lens shading correction (Source Settings "Lens Shading",
`include/osv/render/LensShading.h`) measures each lens's own rim structure
from its own sky and adds the missing light back in native linear light,
before every gain.  On the sample that structure is an additive ring in the
lens facing the sun - missing veiling glare rather than a vignette - so the
correction is additive too.  See docs/research/NEURAL_STITCHING.md, section
9.
