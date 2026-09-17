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

DJI publishes no formula for D-Log M. Two analytic 7-parameter curves of the
form

```
tmp = 2^(scale * code + yShift) + xShift
lin = (tmp < cut ? tmp * slope + intercept : tmp * slope2) * midGrayScaling
```

are shipped, both with the branch cut defined as the intersection of the two
branches so the curve is continuous:

| Curve | Origin | Grey (code 0.40) | Code 1.0 |
|---|---|---|---|
| `kDlogMDjiRefit` (default) | least-squares fit to 64 neutral-axis measurements of DJI's D-Log M to HLG rendering (`scripts/fit_dlogm.py`) | 0.180 | see DlogM.h |
| `kDlogMPocket3` | public Pocket 3 fit constants (Thatcher Freeman) | 0.180 | 2.47 |

DJI's own rendering places code 0.400 at HLG 0.380 and code 0.714 (diffuse
white) at HLG 0.750; the refit reproduces both anchors. The toe below code
~0.24 is crushed 8-bit data in DJI's LUT and is weighted low in the fit.

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
  rendering (0.400 -> 0.388, 0.714 -> 0.735) far better than a peak-to-peak
  tone map, which came out washed out.

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

## "Color Recovery"

DJI's "Color Recovery" option only affects the live-view preview; recorded
D-Log M files are unchanged and carry no flag for it.

## Known limitation: lens shading

The fisheye images carry strong vignetting towards the rim (DJI corrects it
with a per-unit lens shading calibration we do not have). Near the seam the sky
therefore renders darker than in DJI's own stitch. A radial gain model is on
the roadmap; `--gain` only matches the overall exposure of the two lenses.
