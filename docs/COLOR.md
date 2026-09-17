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
                           | 2020->709 in linear light + HLG OETF (SDR) | linear | passthrough
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
inversion, because **our Rec.709 output is the HLG signal in Rec.709
primaries** (see "Rec.709 output" below). Reading the `OSV_TRANSFER_REC709`
branch of `osvLinearToOutput` for a neutral input (R == G == B):

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

which is exactly the HLG branch. For neutrals our 709 and HLG outputs are the
same function, so a 709 reference LUT's diagonal is usable directly as
HLG-signal targets. `tests/unit/test_color.cpp` asserts this identity to 2e-6
rather than assuming it.

This was also checked the other way round: inverting DJI's diagonal through
`HLG_OETF^-1 / 0.2674` recovers a smooth, strictly monotonic scene-linear curve
reaching 3.74 at code 1.0 with 18 % grey at code 0.406. DJI's 709 rendering
therefore really is an HLG-in-709 rendering and not a separate tone map -- had
it been one, the recovered curve would show the characteristic roll-off kink
near diffuse white. **So the curve was what differed from DJI, not our output
rendering, and only the curve was refitted.**

The apparent mismatch that prompted this (their 0.5 -> 0.487 against our
0.5 -> 0.526) was entirely the old curve: the new default gives 0.5 -> 0.506
and is within 0.030 everywhere above the crushed toe.

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
| `OpenOSV_Osmo360_DLogM_to_Rec709.cube` | BT.709 | 0.3873 | 0.7479 |

(Grid points 26 and 46 of 65, the nearest to the 18 % grey and diffuse-white
codes. HLG and Rec.709 are identical on the neutral axis by the algebra above;
PQ's 0.5794 against BT.2408's 0.5807 for 203-nit reference white.)

These are **build output, not source**. Regenerate them with

```
scripts\gen_luts.ps1              # rewrites all three from the built osvtool
scripts\gen_luts.ps1 -Check       # fails if the committed files are stale
```

Never edit one by hand. `tests/unit/test_cube.cpp` regenerates each in-process
and compares byte for byte against the committed copy, so a curve change that
is not followed by a re-run fails the build instead of shipping a stale table.

Every byte is generated by our own `osvtool lut` from our own fitted curve; no
DJI LUT data is redistributed (see NOTICE).

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
* Rec.709 output: the HLG signal is the SDR rendering (BT.2390, "HLG on an SDR
  display"), computed in Rec.709 primaries: grey lands at 38 %, diffuse white
  at 75 %, the camera clip near 99 %. This matches DJI's own D-Log M to Rec.709
  rendering far better than a peak-to-peak tone map, which came out washed out.
  Verified against DJI's Osmo 360 file, which reads 0.3882 at code 0.40625 and
  0.7404 at code 0.71875 -- see "Comparing DJI's Rec.709 rendering with ours"
  above for the algebra and the check that confirmed it is not a tone map.

## Native primaries

The Osmo 360 sensor primaries are approximated by the Pocket 3 fit matrix
(native -> Rec.2020). Saturated hues are therefore approximate until a chart
based fit exists; the matrix is a parameter.

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

## Known limitation: lens shading

The fisheye images carry strong vignetting towards the rim (DJI corrects it
with a per-unit lens shading calibration we do not have). Near the seam the sky
therefore renders darker than in DJI's own stitch. A radial gain model is on
the roadmap; `--gain` only matches the overall exposure of the two lenses.
