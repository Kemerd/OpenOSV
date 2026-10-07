# Geometry conventions

Every convention below was inferred from real footage and is encoded as a
parameter with a verified default plus a test that fails if the default
drifts. The evidence column names the test that guards it.

## Frames

| Frame | Axes | Notes |
|---|---|---|
| Lens | +z optical axis, +x image right, +y image down | Kannala-Brandt model lives here |
| Body | X right, Y forward, Z up (right-handed) | +Y = master lens axis, -Y = slave lens axis |
| View | X right, Y forward, Z up | Virtual camera; `VirtualCamera::rotation()` = Rz(yaw) Rx(pitch) Ry(roll) |
| World | Z-up (the IMU quaternion's own world, in rig axes; see IMU attitude) | Stabilisation, Scene Light's sky |

The kernel composes `Rout = bodyFromWorld * viewToBody`; with stabilisation
off, `bodyFromWorld` is the identity and the camera is defined in the body of
the current frame.

## Lens model (`KannalaBrandt5`)

```
theta   = atan2(hypot(x, y), z)              angle from the optical axis
r/f     = theta (1 + k1 th^2 + k2 th^4 + k3 th^6 + k4 th^8 + k5 th^10)
u       = cx + fx (r/f) cos(phi),   v = cy + fy (r/f) sin(phi)
```

The 5th term is required: the 4-term OpenCV form with DJI's coefficients turns
non-monotonic beyond ~88 degrees. Verified by reproducing the vendor remap
radii (r(90 deg) = 1773 px at 3840) and by `test_geom` (round trip < 1e-4 px,
monotonic to 100 degrees).

## Extrinsics

`cam_extri_q` is stored (w, x, y, z) and `d_lens = R(q) * d_body`. On the
sample clip this gives the master axis within 1.5 degrees of +Y, the slave
axis of -Y, and both image-down directions of -Z. The transposed reading drops
the seam correlation from 0.85 to below 0.6 (`test_seam`).

## Sensor to stream scaling

The 6K stream (3000 px) is the central 3776 px of the 3840 px calibration
frame scaled by 0.794492. `f_stream = digital_focal_length` (829.3612) and
`c_stream = 1500 + (c_cal - 1920) * 0.794492`. A pure 3000/3840 scale misaligns
the seam by 4.25 degrees; `test_seam` asserts the NCC drop. 8K uses scale 1.0;
4K derives the scale from `digital_focal_length / fx` and is flagged
unverified in `FormatInfo::notes`.

## Field of view and blending

Usable lens FOV is 195.18 degrees (the value DJI's own stitcher uses); the kernel
feathers the last 4 degrees before `thetaMax` with a smoothstep and multiplies
by an occlusion factor derived from the 14-point polygon in the calibration
(the selfie-stick side of the image). Blending happens in scene-linear light.

## Eye-offset projection

The reframing camera's default look is the eye-offset ("generalised
stereographic") model: the sphere is projected onto the image plane from a
point `d` radii behind its centre. With `f` the focal length in pixels and
`theta` the angle from the view axis:

```
r / f   = (1 + d) sin(theta) / (d + cos(theta))          forward
k       = r / (f (1 + d))
theta   = atan(k) + asin(k d / sqrt(1 + k^2))             inverse (pixel -> ray)
f       = (W/2) (d + cos(hfov/2)) / ((1 + d) sin(hfov/2))  viewport edge = hfov/2
```

`d = 0` reduces to the pinhole (`r = f tan(theta)`) and `d = 1` to the
stereographic projection (`r = 2 f tan(theta/2)`); `test_reframe` proves both
equalities to double precision on the host and to float precision inside the
kernel, plus monotonicity of `theta(r)` and the round trip for intermediate
offsets. The forward model has an asymptote at `theta = acos(-d)`; `theta(r)`
is strictly increasing below it (its derivative is proportional to
`1 + d cos(theta) > 0`), and the kernel reports anything at or beyond it as
uncovered. `VirtualCamera::effectiveHfovDeg()` clamps the requested field of
view below `2 acos(-d) - 1 degree` (179 degrees at `d = 0`, 359 at `d = 1`)
so the inverse stays well conditioned; `osvtool render --proj eye-offset
--distortion d` and the Premiere effect's Distortion slider (`d = percent /
100`) drive the same parameter, `OsvRenderParams::eyeOffset` /
`OsvReframeParams::eyeOffset` with `OSV_PROJ_EYE_OFFSET`.

The presets (`geom::kPresets`, user-editable starting points, not format
conventions) all use this projection so one control moves between looks:

| Preset | hfov (deg) | pitch (deg) | eye offset `d` | Equivalent |
|---|---|---|---|---|
| Crystal Ball | 240 | 0 | 1.0 | stereographic |
| Asteroid | 300 | -90 | 1.0 | stereographic, looking down |
| Wide | 120 | 0 | 0.15 | near-pinhole with mild barrel |
| Ultra Wide | 150 | 0 | 0.4 | wider, corners kept compact |
| Dewarping | 95 | 0 | 0.0 | rectilinear |

### Equirect reframe entry point

`osvReframeEquirectPixel` (osv_kernel.h) is the second kernel entry point:
it reframes an already stitched Standard-layout equirect (32-bit or 16-bit
float, RGBA or BGRA) with the same `osvViewRay` ray generator as the fisheye
shader and the same `Rout = body <- view` rotation from
`VirtualCamera::rotation()`, so `osvtool render --proj eye-offset` and the
Premiere effect produce identical framing. The lookup is bilinear with
longitude wrap-around at the +/-180 degree seam and latitude clamp at the
poles; pixels outside the viewport rectangle are transparent black (aspect
letterbox), alpha is carried straight from the source unless `fillAlphaOne`
is set. The half-float decoder in the header is pure bit manipulation so the
CPU, CUDA and OpenCL builds of the function execute the same arithmetic
(`test_reframe`, CUDA parity >= 60 dB).

## Equirect layouts

* **Standard**: `lon = (px/W - 0.5) 2pi`, `lat = (0.5 - py/H) pi`,
  `d = (sin lon cos lat, cos lon cos lat, sin lat)`; the centre column looks
  along +Y (master).
* **PolarAxis**: `d = (cos lat sin lon, sin lat, cos lat cos lon)`; the lens
  axes sit at the poles and the seam is the equator. Used by the seam analysis
  so parallax becomes a 1-D vertical problem per column.

## Seam correction

`searchSeam` measures, per longitude column, the row disparity between the
two lenses in the polar-axis band. Positive values mean a near object appears
farther from both axes than the calibration predicts; the kernel then samples
half that angle farther from each axis (`osvShiftTowardAxis` with a negative
angle). Parallax for a 2.5 cm baseline is 1.9 degrees at 0.75 m, 0.5 degrees
at 3 m.

## IMU attitude

Each frame carries a batch of 16-17 fused quaternions (about 995 Hz); the
per-frame `camera_attitude` equals batch entry 4. With `f0..f3` the four
stored floats in file order, the reading is

```
q' = (w = f3;  x = -f1,  y = f2,  z = f0)        body -> world, world up = +Z
```

i.e. the floats read (x, y, z, w) are a **world-to-body** rotation in the
IMU's own axes, relabelled into the rig's axes by `K: (x, y, z) -> (y, -z,
-x)` on both sides (`AttitudeConvention{XYZW, WorldToBody, Z, rigAxes}`, the
default; `osvtool --attitude-convention xyzw-w2b-z-rig`, what `auto` resolves
to). Its world is level by construction, so Horizon Lock needs no
accelerometer. It was settled against image truth on two car-mounted 8K
clips, where the car turns about the true vertical:

| Check | 0.5.0 reading | This reading |
|---|---|---|
| Zenith fitted to lamp poles / building edges (Horizon Lock render), day | 27.8-28.1 deg off | 0.8-3.8 deg |
| The same, night | 7.3 deg (pole trace), 11.4 deg (building trace) | 3.1 deg, 1.6 deg |
| Sunset sun elevation over five frames through a 106 deg turn (ephemeris 6.3-7.1 deg) | +33 / +36 / -2 / -1 / +38 deg | +9.9 / +8.1 / +9.6 / +9.7 / +6.3 deg |
| The sun's latitude in a Full (locked) render, 1000 frames apart | +73 vs -33 deg | -19 vs -21 deg |

The zenith is the smallest eigenvector of the traced verticals' great-circle
normals on the sphere (Collins and Weiss 1990, `docs/CITATIONS.md`); the
render's rotation is measured by registering it against the stab-off render
of the same frame, so the numbers describe the shipped engine, not a model of
it.

The reading 0.4.x / 0.5.0 used, components (x, y, z, w) as **body-to-world**
with a world up of **-Y** (`xyzw-b2w-ny`), is that rotation transposed and
relabelled. It was checked by eye on the airborne sample clip, whose attitude
changes by only 4.9 degrees over the whole clip: at that one pose the two
readings agree within 0.1-0.5 degrees (`test_attitude`, every frame of the
.OSV and the .LRF), so the sample could not tell them apart and its horizon
does not move (0.4 px at 2048x1024). On a car that turns, the transposed
rotation turns the wrong way (Full and Smooth swing) and its world is not
level. 0.5.0 then levelled on the accelerometer taken as a world-frame
vector, `(a.z, a.y, -a.x)`, which on a pure yaw puts the render's up on the
body's -X every frame: off by the whole mount tilt.

`camera_acc` is a **body-frame** specific force in its own axis order:
`(a_y, a_x, -a_z)` in rig axes. On a car or a tripod its clip mean is the
gravity reaction and lands within 1 degree of this reading's body up (0.2-0.8
degrees over 256 frames, both drives, .OSV and .LRF). `ConventionProbe::
autoDetect` keeps it only as a canary: the angle is logged with the reading,
and above 15 degrees on a clean gravity reaction (mean 0.6-1.4 g, frames
within 35 degrees of the mean) it is logged as a warning. It never moves the
levelling. An airborne clip measures its flight, not gravity, and is reported
without a verdict. `--attitude-convention` still exposes the 16 plain
readings (order x sense x up in {y, z, ny, nz}) and each with `-rig`; pitch
and roll dynamics beyond a car's, and the Avata 360's lens-up mount, are not
yet verified against image truth.
