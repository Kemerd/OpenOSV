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
| World | Y-up (best-supported reading of the IMU quaternions) | Only matters for stabilisation |

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

Usable lens FOV is 195.18 degrees (DJI's own shader constant); the kernel
feathers the last 4 degrees before `thetaMax` with a smoothstep and multiplies
by an occlusion factor derived from the 14-point polygon in the calibration
(the selfie-stick side of the image). Blending happens in scene-linear light.

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
per-frame `camera_attitude` equals batch entry 4. The reading was settled
empirically by rendering the sample clip with horizon lock and looking at the
result: components (x, y, z, w), `R(q)` mapping **body to world**, and a world
"up" of **-Y** level the picture upright (matching the camera's own stitched
cover image). Reading the same quaternions as world-to-body with +Y up, the
earlier hypothesis, does not level the horizon at all. The component order
cannot be told apart on a static clip and stays (x, y, z, w) until a rotation
clip says otherwise; `--attitude-convention` exposes all 16 readings (order x
sense x up in {y, z, ny, nz}). The accelerometer field in the metadata is not a
gravity vector, so `ConventionProbe` only decides when it finds one (mean angle
below 15 degrees); otherwise `auto` uses the default above.
