# DJI's reframe camera: Zoom, FOV and Correction Angle

What DJI's two reframe tools actually render, determined from DJI's publicly
distributed software for interoperability, and how Open 360 Reframe
reproduces it (the "Lens: DJI" choice - package WP-CAMERA, with the Lens
popup and one-lens-at-a-time panel of WP-LENSUI).

Sources, neither redistributed:

* **DJI Studio for Windows** 1.0.0.24724.
* **DJI's Premiere reframe plug-in for macOS.**
* A DJI Studio project written for the author's own clip, and two DJI Studio
  screenshots of the same clip (values in section 5).

This was for understanding only; no DJI code or data file is copied into the
product.  The numbers the product uses (slider limits, the five presets, the
zoom-path rate) are facts about DJI's behaviour, restated in OpenOSV's own
code.

---

## 1. The model

Both tools texture the equirect onto a **unit sphere** and look at it with an
ordinary **perspective camera** placed on the view axis **behind the centre**:

| DJI control | What it is | Where it goes |
|---|---|---|
| **FOV** | the camera's *vertical* pinhole field of view, degrees | `fovy` of the perspective matrix |
| **Correction Angle** | the eye's distance behind the sphere's centre, in sphere radii (`eyez`) | the view matrix's eye position |
| **Zoom** | *not an input*: the visible horizontal angle across the centre row, derived from the other two and the canvas shape | a read-out (DJI Studio only) |

A pixel sees the point where its pinhole ray **leaves** the sphere (the far
root of the ray/sphere intersection).  With the eye at `E = (0, -e, 0)`
(view axis +Y) and unit ray `u`:

    t  = e·u_y + sqrt(1 - e²·sin²α)          (α = the ray's angle off axis)
    P  = E + t·u                              (on the unit sphere: P is the direction)

Equivalently, a ray at pinhole angle `α` lands on the sphere at

    θ = α + asin(e · sin α)                   (seen from the sphere's centre)

* `e = 0`: rectilinear.  `e = 1`: stereographic (DJI's "Asteroid").
* `e > 1`: the eye is **outside** the sphere ("Crystal Ball", e = 1.8); rays
  that miss the sphere are black, the picture is a disc of angular radius
  `asin(1/e)` with black around it.

For `e <= 1` this is the same family as Open 360 Reframe's original
eye-offset projection (`r/f = (1+d) sin θ / (d + cos θ)`, i.e. a pinhole of
focal `f(1+d)` behind the centre); what differs is the *parameterisation*:
DJI fixes the pinhole's **vertical** field of view, the Classic lens fixed the
**visible horizontal** angle.  That is why typing DJI's numbers into the
Classic controls gave a different picture.

### How DJI's Premiere plug-in draws it

* **Projection:** an ordinary perspective projection with vertical field of
  view `fovy` = FOV (degrees), the canvas aspect, near plane 0.0005 and far
  plane 10.  The FOV is used as entered - no clamp and no remapping - so the
  slider range is the only limit.
* **View:** the eye sits on the view axis, Correction Angle sphere radii
  behind the centre, looking through the centre.  The Correction Angle is used
  as entered: **no mapping**.
* **Sphere:** a unit sphere tessellated as 200 slices × 100 parallels, drawn
  with back-face culling so that only faces seen from inside survive, with no
  depth test and a black background.  That is exactly "the far intersection"
  for every eye distance, and it is why rays that miss the sphere are black.
* **Orientation:** the model rotation is `Rz(roll)·Rx(tilt)·Ry(pan)`; the
  camera and source angles are summed first, and **roll is negated**.
* **Shading:** one texture sample per pixel.  There is no distortion stage in
  this plug-in (DJI Studio has one, section 4).
* A closed form of the above (far root, the formulas in section 1) was
  checked against the equivalent perspective-matrix formulation over 2 488
  random cases: worst error 2.3e-15.

### How DJI Studio parameterises it

* The reframe filter's own parameters are **`fov`** and **`eyez`**: the
  project file stores `"fov": 60, "eyez": 1` at filter level and keyframes
  `{"fov": 101.247, "distortion": 0.625, "hDegree": ..., "vDegree": ...,
  "rotationZ": ...}`.  A keyframe's `fov` drives the renderer's `fov`, its
  `distortion` drives `eyez`, and the three angles become the renderer's
  rotation.  So the UI's **Correction angle** *is* `eyez`, exactly as in the
  plug-in, which also labels it "Correction Angle".
* DJI Studio builds the same camera (eye on the axis at `eyez`, vertical
  focal `f = 1/tan(fov/2)`, horizontal scale `f/aspect`) and takes the
  visible half angles as `α + asin(eyez·sin α)` on both axes - the formula of
  section 1.

---

## 2. Zoom (DJI Studio)

DJI Studio's **Zoom** is a derived read-out.  From the canvas aspect `w/h`,
the FOV `fov` and the Correction Angle `d`:

    if aspect <= 0 or fov <= 0 or d < 0:           return 0
    a     = tan(min(180, fov)·π/360) · aspect             # tan of the horizontal half angle
    if |a| < DBL_EPSILON:                          return 0
    zoom  = 360 - 2·atan(1/a)·180/π - 2·acos(clamp(d·a/sqrt(1+a²), -1, 1))·180/π

which is `2·(α_h + asin(d·sin α_h))`, `α_h = atan(a)`: the visible angle of
the centre row.  **This is the "Zoom" DJI Studio shows** - checked against
the two screenshots in section 5.  Open 360 Reframe implements the same
formula, guards included (`osv::geom::djiZoomDeg`).

Zoom is never stored: DJI's keyframes hold `fov` and `distortion` only.
DJI Studio's editable reframe values are exactly five: FOV, clamped to
**[20, 150]**; Correction, clamped to **[0, 1]**; and pan, tilt and roll,
each clamped to ±35640.

### The zoom gesture

DJI Studio's zoom gesture, by an amount `delta`, moves **both** lens
controls:

    fov        += 130 · delta
    correction += delta

then clamps FOV to [20, 150] and Correction to [0, 1].  Zooming in stops
when FOV ≈ 20 and Correction ≈ 0; zooming out stops when FOV ≈ 150 and
Correction ≈ 1.

Open 360 Reframe's **Zoom** control is the inverse of this path: editing it
moves DJI FOV and Correction Angle along `(fov + 130δ, correction + δ)` until
the lens shows the requested Zoom (bisection; Zoom is monotonic along the
path), and the overlay's zoom drag moves along the same path.

---

## 3. Presets

DJI's Premiere plug-in keeps one set of `{pan, tilt, roll, fov, eyez}` per
preset (5) and output resolution (12); a preset applies its tilt, FOV and
Correction Angle.

| Preset | FOV landscape (16:9, 1:1, 2.35:1) | FOV 9:16 | FOV 3:4 | Correction | Tilt |
|---|---|---|---|---|---|
| Crystal Ball | 75 | 110 | 87 | 1.8 | - |
| Asteroid | 138 | 147 | 147 | 1.0 | -90 |
| Wide | 60 | 90 | 72 | 0.6 | - |
| Ultra Wide | 78 | 110 | 95 | 0.5 | - |
| Dewarping | 80 | 112 | 97 | 0.2 | - |

DJI Studio's list has four entries - Asteroid (pan 0, tilt −90, roll 0,
FOV **138**, Correction **1.0**), Ultra Wide (78, 0.5), Wide (60, 0.6),
Dewarping (80, 0.2) - the plug-in's landscape column.  DJI Studio re-derives
the FOV per canvas shape and preview-window size
(`fov' = 2·atan(tan(fov/2)·k·h_c/w0)`, over the canvas aspects 1:1, 3:4, 4:3,
9:16 and 16:9); for the current canvas in steady state that is the base
value.

Plug-in slider ranges: **FOV 1-178**, default 60.01; **Correction Angle
0-1.8**, default 0.601.  The .01 / .001 offsets are sentinels: a value equal
to the default (what Reset produces) is replaced by the preset's value for
the current preset and resolution, so a fresh instance renders Wide.

Open 360 Reframe: `osv::geom::kDjiPresets` holds the plug-in's values; a
preset writes the column for the output's shape (landscape / 9:16 / 3:4,
split at their geometric mean), the Correction Angle and the tilt (DJI
Studio's presets write tilt too - 0 or −90 - so ours do).

---

## 4. DJI Studio's extra stage: de-distortion (not reproduced)

DJI Studio - not the Premiere plug-in - bends the projected sphere once more
as it draws it: a vertex at pinhole NDC `(x, y)` is pushed outward along its
radius to `x' = k`, where `k` solves the cubic
`k = |x| + y²·(c0 + c1 k + c2 k² + c3 k³)`, and the result is blended back
towards the pinhole position.  On the centre row (`y = 0`) it is the
identity, so the Zoom read-out (the centre row's angle) is unaffected by it.

Its strength is the project's `de_distortion` value (the project stores 1)
times a weight that is **1 below a lower FOV curve A(eyez), 0 above an upper
curve B(eyez)** and a smootherstep `1 − t³(6t² − 15t + 10)` between them.
The curves are cubic fits of 13 samples at eyez = 0, 0.1 … 1.2:

    A: 123.15 113.45 103.75 96.45 89.45 82.75 76.2 70.85 65.2 59.7 54.35 50.5 45.45
    B: 142.15 135.15 128.15 120.55 113.55 107.15 101.2 94.95 89.75 84.1 78.75 73.55 68.5

and eyez outside [0, 1.2] gives weight 0.  The cubic's coefficients are
fitted per frame by projecting meridians through the camera.

**Not reproduced, deliberately:** the product is a Premiere plug-in, and
DJI's own Premiere plug-in has no such stage; the two screenshots in
section 5 are both above B(eyez) (weight 0), so they are the pure sphere
camera in DJI Studio too.  Framings narrower than B(eyez) - e.g. Wide at
FOV 60 / 0.6 - are DJI Studio pictures with this bend applied and DJI
plug-in pictures without it; ours match the plug-in.  Reproducing Studio
exactly there would need the per-frame cubic fit and a per-pixel inverse of a
forward vertex warp.

---

## 5. Field check: the two DJI Studio screenshots

Both at 16:9 (the project's canvas, 1920 × 1080).

| DJI Studio shows | Zoom from our formula | Correction that gives DJI's Zoom exactly |
|---|---|---|
| Zoom 207.1, FOV 103.3, Corr 0.67 | **207.5** | 0.6669 (displays as 0.67) |
| Zoom 201.7, FOV 150.0, Corr 0.34 | **202.1** | 0.3363 (displays as 0.34) |

DJI prints the correction to two decimals; ±0.005 of it moves Zoom by
±0.66° (first) and ±0.60° (second), so both screenshots are consistent with
the formula - the residual is DJI's rounding of the Correction Angle, not a
model difference.

What the two lenses look like (1920 × 1080, radial profile `θ(r)` = angle
from the view axis at `r` pixels from the centre):

| r (px) | 0 | 135 | 270 | 405 | 540 (top edge) | 810 | 960 (side edge) |
|---|---|---|---|---|---|---|---|
| FOV 103.3 / 0.67 | 0° | 29.2° | 53.3° | 70.9° | 83.3° | 98.5° | 103.8° |
| FOV 150 / 0.34 | 0° | 56.4° | 79.3° | 89.0° | 94.2° | 99.4° | 101.1° |

* **103.3 / 0.67 - "gentle wide fisheye".** The centre spends 0.224°/px, the
  top edge 0.077°/px: a 2.9× radial compression from centre to edge, the
  horizon a soft arc, the frame's sides looking 104° off axis (just behind
  the camera's plane).
* **150 / 0.34 - "extreme tunnel".** The pinhole is so wide that the centre
  spends 0.53°/px - 2.4× more than the first lens, so the plane in the middle
  is correspondingly small - while the top edge spends 0.029°/px: an 18×
  radial stretch (57× at the sides), which smears the ground radially towards
  the frame's edges.  The top and bottom edges look 94° off axis, past the
  zenith and nadir.

`tests/unit/test_dji_camera.cpp` and `tests/premiere/reframe/test_dji_camera.cpp`
pin these numbers through the library camera, the kernel and the rendered
effect; the direct fisheye path frames both views like the equirect path to
0.003 px and 0.10 px (`test_direct.cpp` framing cases).

---

## 6. How Open 360 Reframe exposes it

* **Lens** popup ("DJI | Classic", id 21, appended last), **DJI by
  default**.  It picks the lens that renders AND the controls the Effect
  Controls panel shows: DJI shows Zoom, FOV (DJI's vertical pinhole angle,
  registered as "DJI FOV" and shown as "FOV") and Correction Angle; Classic
  shows FOV (the visible angle across the width) and Distortion.  Pan, Tilt,
  Roll, Preset, Output Resolution, the Source group, Smooth Keyframes and
  Drag Sensitivity are always shown.  The two FOVs are the same family of
  camera measured differently (section 1), which is why they never appear
  side by side any more.  Hiding uses `PF_PUI_INVISIBLE` through
  `PF_UpdateParamUI` in `PF_Cmd_UPDATE_PARAMS_UI`, which Premiere honours
  dynamically (`docs/PREMIERE.md`, "One lens at a time", has the evidence).
* The popup replaced WP-CAMERA's **Camera Model** checkbox (id 16).  That was
  a checkbox because Premiere's GPU parameter reads number popups from 0
  where After Effects numbers them from 1 (the 26.2.2 dump; DJI's plug-in
  likewise adds 1 to the popup values it reads there), so a raw "1" is
  either lens.  A popup is safe now: the GPU path learns each instance's
  numbering from any popup that reads unambiguously; DJI reads 0 on
  Premiere, which settles it; and the effect only ever leaves Classic beside
  Preset "Custom", which also reads 0 there.  The checkbox stays in the list
  (a parameter that disappears breaks saved projects), registered invisible
  and kept in step as the popup's mirror, so a project saved now still opens
  on the right lens in the previous build.  A project saved before the popup
  opens on DJI, whatever its checkbox held - the user's choice.
* **Zoom** (id 17, not animatable), **DJI FOV** (id 18, slider 20-150, valid
  1-178), **Correction Angle** (id 19, 0-1.8, two decimals), **Drag
  Sensitivity** (id 20, default 2.0 - the old constant).
* Picking a preset writes DJI's numbers (and the Classic ones) and selects
  DJI; picking a lens in the popup carries the current look across to that
  lens's controls (Classic -> DJI exactly; DJI -> Classic exactly unless the
  Classic ramp or a Correction above 1 makes the look unrepresentable), so
  the framing does not jump.  Editing a control of the other lens - possible
  only through a host that shows every control - still selects that lens.
* Kernel: `OSV_PROJ_DJI_SPHERE` (`osvDjiSphereRay`, `osv_kernel.h`
  [WP-CAMERA] region) - the far-root intersection above, trig-free, any
  `e >= 0`; `focalPx` carries the pinhole focal `(H/2)/tan(FOV/2)` (cover-fit
  applied) and `eyeOffset` carries `e`.

## 7. Established vs inferred

**Established** (DJI's behaviour, determined from its software): everything
in sections 1-4, including the Zoom formula, the zoom-path rate and clamps,
the preset values, the slider ranges, the de-distortion weight curves and
which UI control drives which camera parameter.  Section 5 checks the result
against DJI Studio's own read-outs.

**Inferred:**

* The plug-in's triangle winding convention (y-up normalised device
  coordinates) - forced by the behaviour: any other reading makes every
  Correction Angle below 1 render black.
* That the formula in section 2 is what DJI Studio's "Zoom" label shows: it
  is the only derived angle DJI Studio computes for the reframe view, and it
  reproduces both screenshots within the display rounding.
* That the aspect DJI Studio uses for Zoom is the project canvas (16:9
  here); where DJI Studio takes that aspect from was not established.
* Pan / tilt / roll equivalence with **DJI Studio** is NOT established, and
  the DJI lens deliberately leaves Open 360 Reframe's angle conventions
  unchanged (every existing keyframe means what it meant):
  * against DJI's **Premiere plug-in**, pan and tilt agree in sign and zero
    (pan 0 looks at the equirect's centre column, positive pan moves the
    view to smaller longitude, positive tilt looks up); roll is opposite -
    the plug-in negates it before building `Rz(roll)`, so its positive roll
    turns the picture clockwise, ours counter-clockwise;
  * DJI Studio hands its renderer the angles as (tilt, −pan, roll) in
    radians - pan negated, roll not - and its equirect sampling shifts u by
    half a turn unless the input is flagged as reversed.  How its renderer
    turns those angles into a rotation was not established, so Studio's pan
    origin (possibly 180° away) and its pan and roll signs relative to ours
    are open.  One side-by-side check settles it: the same clip at pan 0 /
    tilt 0 / roll 0 in both, then pan +30 and roll +30.
