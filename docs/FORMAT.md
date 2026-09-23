# The .OSV container

An `.OSV` file is an ISO base media file (`isom/iso2/mp41`) with a few DJI
additions. Layout of a 6K clip:

```
ftyp                      isom
free (8 bytes)
free (4052 bytes @ 36)    index table: 16-byte entries  tag | u64 offset | u32 size  (covr, snal, camd)
mdat                      interleaved samples: djmd, djmd, dbgi, dbgi, video 1, video 2 per frame; audio chunks later
moov
  trak 1  hvc1 3000x3000 HEVC Main10 (slave lens, stream 0)   tkhd flags 0x3
  trak 2  hvc1 3000x3000 HEVC Main10 (master lens, stream 1)  tkhd flags 0x2
  trak 3  mp4a AAC-LC 48 kHz stereo
  trak 4  djmd  "CAM meta"  one protobuf sample per video frame (calibration + IMU)
  trak 5  djmd  "CAM meta"  stripped copy (no calibration, no IMU)
  trak 6  dbgi  "CAM dbgi"  per-lens ISP debug (ignored)
  trak 7  dbgi  "CAM dbgi"
  udta    (c)uid, dbpm, dbcm, btec (ascii), fsid (ascii), meta/ilst (covr, snal, tnal JPEGs, (c)too), Xtra
  meta    empty (mdta)
camd                      nested MP4 (ftyp/free/mdat/moov) with two djmd tracks of 120 samples:
                          the first 2 s of metadata plus post-roll; un-indexed records follow the indexed ones
```

Resolutions per lens: 1920 (4K mode), 3000 (6K), 3840 (8K). The HEVC streams
are coded 3008 with a 4-pixel conformance window, 3 slices per picture, GOP
60, no B-frames, and are tagged `colr` 1/1/1 (BT.709) even for D-Log M.

The `.LRF` proxy next to the clip is a single 2048x1024 H.264 track holding
both lenses side by side (left = slave, right = master), plus the same `djmd`
metadata and its own `camd`.

## djmd protobuf

Each `djmd` sample is one `ProductMeta { 1 ClipMeta, 2 StreamMeta, 3 FrameMeta }`
in proto3 wire format; sample 0 carries all three, later samples only
`FrameMeta`. `proto/dvtm_osmo360.proto` is OpenOSV's transcription of the
schema. The fields that matter:

| Path | Meaning |
|---|---|
| ClipMeta.1 (header) | proto file name, versions, serial, firmware, clip timestamp, product name |
| ClipMeta.8 | `digital_focal_length` (stream-space focal length, 829.3612 at 6K) |
| ClipMeta.10 | IMU sampling rate (1000 Hz) |
| ClipMeta.14 | sensor resolution (3840 x 3840) |
| StreamMeta.3 | video width/height/fps/bit depth/codec |
| StreamMeta.4 | `color_mode` (19 = D-Log M, 9 = HLG, 0 = Normal) |
| StreamMeta.6 | `PanoDewarpParams`: 24 `DewarpParams` slots (native_refine slave/master = 1/2, native = 11/12, lens guards 5/6, underwater 9/10, far presets 13-24) |
| StreamMeta.7 | `extri_lens_mode` (0 native, 1 lens guards, 2 underwater) |
| DewarpParams | fx, fy, cx, cy, k1..k5, width/height (3840), occlusion polygon (14 points), `cam_extri_q` (w, x, y, z) |
| FrameMeta.1 | sequence number (+2 per frame), timestamp (us since power-up) |
| FrameMeta.2 | ISO, exposure, white balance, `camera_attitude`, `camera_acc`, sensor temperature |
| FrameMeta.3 | `DeviceMultiAttitude`: batch of 16-17 fused quaternions, timestamp, vsync, offset |

Repeated scalars may be packed or unpacked; unknown fields are skipped by
wire type; every field is optional.

### Calibration sets and lens accessories

The camera writes **every** `PanoDewarpParams` slot; the sets it has no
numbers for are zero-filled placeholders.  On the sample clip only
`native_refine` (1/2), `native` (11/12) and the six far presets (13-24) carry
data; 3-10 (native_refine_far, lens guards, above / under water) are 159-byte
all-zero records.  `osvtool probe` prints the table, the differences against
native_refine and what every calibration choice would stitch with.

`extri_lens_mode` (StreamMeta.7) is the camera's **Lens Protection Mode**
switch (control centre, "Transparent Lens Protectors"); the proto3 default
(an empty field 7) means bare lenses.  There is no ND-filter field anywhere
in the format: ND filters that mount like the protectors are declared through
the same switch (Freewell's instructions say so).  `FrameMeta.2.8`
`underwater_confidence` is written but unused by DJI's tools.

What DJI's own tools do (DJI's Premiere importer and DJI Studio, studied for
interoperability; understanding only, nothing copied):

* **Slot choice** ignores `extri_lens_mode`.  For library version 02.01.07 and
  later (the sample is 02.01.15) they start at **`far_11` (17/18)**, fall back
  per lens to native_refine_far (3/4) and then native_refine (1/2), and never
  read `native` (11/12) or the lens-guard / water slots.  OpenOSV stays on
  `native_refine`: far_11 differs by 2.6 px focal / 0.14 px centre / 0.006 deg
  in calibration space and measured +0.0004 overlap NCC on the sample - noise.
  `--stitch-distance 1.1` selects it explicitly.
* **Lens protectors** are a field-angle correction, not a calibration slot:
  DJI's importer bends every ray's angle from the lens axis through a
  measured curve (+0.52 deg at 30, +1.28 deg at 90, +1.66 deg at 98 deg)
  *before* projecting with the native calibration.  OpenOSV folds its own
  smooth fit of that curve into the lens model (`geom/LensProtector.h`) when
  the choice resolves to lens guards and the clip has no dedicated set, and
  checks the direction once per clip on frame 0
  (`render/LensProtectorCheck.h`).  DJI Studio defaults its Lens Protector
  option from `extri_lens_mode == 1`, which is what OpenOSV's Auto does.
* **Underwater / above water** use similar curves (much larger: 90 deg maps to
  86.5 / 78.6 deg) that OpenOSV does not model yet.

## Index table

The second `free` box holds three 16-byte entries. For `camd` the offset is
the payload start and the size the payload length; for `covr`/`snal` the
offset points at the `ilst` item box while the size is the raw JPEG length
(the JPEG begins at offset + 24). OpenOSV verifies every entry against the
box tree and flags mismatches instead of trusting the table.
