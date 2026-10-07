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

### Other cameras: the same messages, other numbers

`ClipMeta.1.1` (`proto_file_name`) names the camera's schema. Every DJI camera
builds its `ProductMeta` from the same library messages (`DewarpParams`,
`PanoDewarpParams`, `Quaternion`, the scalar wrappers, the IMU batches), but
numbers the fields of `ClipMeta`, `StreamMeta`, `FrameMeta` and
`FrameMetaOfCamera` its own way. `DjmdDecoder` maps each known schema onto the
Osmo 360 numbering above, so the typed structs and their `present` bits mean
the same on every camera; frames after the first carry no header and are read
in sample 0's schema. `osvtool probe` prints the schema used.

The DJI Avata 360 (`dvtm_AVATA360.proto`), where it differs:

| Avata 360 | Meaning | Osmo 360 |
|---|---|---|
| ClipMeta.6 | `digital_focal_length` | ClipMeta.8 |
| ClipMeta.8 | IMU sampling rate | ClipMeta.10 |
| ClipMeta.12 | sensor resolution | ClipMeta.14 |
| ClipMeta.14 | `flat_res` (not read) | - |
| StreamMeta.2.4 | `color_mode`, inside `camera_stream_meta` | StreamMeta.4 |
| StreamMeta.4 | `fov_type` (empty on every clip seen) | StreamMeta.5 |
| StreamMeta.5 | `PanoDewarpParams` | StreamMeta.6 |
| StreamMeta.6 | `extri_lens_mode` | StreamMeta.7 |
| FrameMeta.4 | `drone_frame_meta` (the aircraft's telemetry; not read) | - |
| FrameMeta.5 | `gimbal_frame_meta` | FrameMeta.4 |
| FrameMetaOfCamera.22 / .23 | `camera_attitude` / `camera_acc` | .9 / .10 |
| FrameMetaOfIMU.4 | a single fused attitude batch, used when field 2 has none | - |

The Avata 360 flies its 360 camera with one lens up and one down, so its lens
axis (the body's +Y) is vertical: Horizon Leveling measures that from the
attitude track and levels about the body's horizontal axis instead
(`levellingMount`, `include/osv/geom/Stabilization.h`).

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

## Dropped frames (variable frame rate)

The camera records at a constant rate, but when it cannot keep up - seen on a
long night drive at high ISO, from 75 s in - it drops frames and writes each
gap into the time table (`stts`) as one longer sample. A 50 fps 8K clip of
326.6 s then holds 16157 samples: 15988 of 20 ms, 167 of 40 ms and 2 of 60 ms
(its `.LRF`: 8083 samples of 40 ms, 163 of 80 ms). What the files say about it:

* **Per-frame capture timestamps** (`FrameMeta.1` timestamp, one `djmd` sample
  per video sample) are the camera's clock: their deltas are exactly the
  dropped periods, and they span the audio's length. The `djmd` sequence
  number has no gaps and cannot show a drop.
* The **`.OSV` time table** follows that clock to within 10 ms. The **`.LRF`
  time table does not**: it writes every 60 ms gap as 80 ms, 3.1 s too long
  over the clip. The first capture timestamps of an `.OSV` and its `.LRF` are
  equal.
* Both lens tracks of an `.OSV` share one time table and one sync table
  (`stss`), and both lenses drop the same frames (the larger first picture
  after each gap sits on the same sample in both tracks). After a 60 ms gap,
  though, the second lens's encoder can place its IDR pictures one sample
  later than the first lens's: on the night clip, from sample 12549 on, every
  listed sync sample of track 2 (72 of them) is a trailing picture, and its
  IDR is the next sample.

How OpenOSV reads such a clip:

* **Frame index = sample index = `djmd` index**, in both decoder modes: a
  decoded picture is matched to its sample by the sample table, never by a
  time index at the average frame rate (which named a different sample than
  the one asked for on such a clip, and none at all around a gap). A listed
  sync sample whose first picture is not a random access point is not used as
  a decode start; the nearest real one before the request is.
* **Hosts see a constant-rate timeline at the NOMINAL rate** - the most common
  sample duration, never sample 0's - with the previous picture held over
  every gap, each sample placed by its capture timestamp (the time table only
  when the timestamps are missing, not one per sample, not increasing, or
  span more than 3 % differently from the table). The night clip's `.OSV`
  becomes 16328 frames at 50 fps (326.56 s against 326.57 s of audio, 171
  frames held), its `.LRF` 8168 at 25 fps. A constant-rate clip is its own
  sample list, unchanged. Premiere, the OpenFX generator (Resolve, VEGAS),
  `osvtool probe` (`frameCount`, `fps`, `durationSeconds`, plus a `timeline`
  object) and `osvtool render --frame / --range / --all` all count this
  timeline; `osvtool extract --frame` addresses samples.
* An `.LRF` presented as its `.OSV`'s proxy takes the original's timeline and
  shows, at every timeline frame, the `.LRF` sample captured nearest the
  moment the original shows there.

`osv::video::ClipTimeline` (`include/osv/video/ClipTimeline.h`) is the one
implementation every host shares.

## Index table

The second `free` box holds three 16-byte entries. For `camd` the offset is
the payload start and the size the payload length; for `covr`/`snal` the
offset points at the `ilst` item box while the size is the raw JPEG length
(the JPEG begins at offset + 24). OpenOSV verifies every entry against the
box tree and flags mismatches instead of trusting the table.

## 360 metadata written by osvtool

`osvtool render --mode equirect` (`.mp4` / `.mov`) and `osvtool spherical`
tag the first video track as monoscopic equirectangular 360 video, in both
of Google's schemes
([V1](https://github.com/google/spatial-media/blob/master/docs/spherical-video-rfc.md),
[V2](https://github.com/google/spatial-media/blob/master/docs/spherical-video-v2-rfc.md)):

```
moov
  trak                      first track with a 'vide' handler
    tkhd, edts, mdia ...    unchanged
      stsd
        hvc1                every sample entry of the track (avc1, apch, ... alike)
          hvcC, colr ...    unchanged
          st3d              FullBox v0: stereo_mode 0 (monoscopic)              13 bytes
          sv3d                                                                  88 bytes
            svhd            FullBox v0: metadata_source "OpenOSV\0"
            proj
              prhd          FullBox v0: pose yaw, pitch, roll 0 (16.16)
              equi          FullBox v0: bounds top, bottom, left, right 0 (0.32)
          pasp, btrt        the optional boxes stay last
    uuid ffcc8263-f855-4a93-8814-587a02521fdd
                            GSpherical RDF/XML: Spherical true, Stitched true,
                            StitchingSoftware OpenOSV, ProjectionType equirectangular
```

V1 is the box Google's Spatial Media Metadata Injector writes and YouTube
reads; V2 is what FFmpeg (`Spherical Mapping` side data), VR players and 360
editors read. Only `moov` is rebuilt: every box on the path gets its new size,
and when `moov` sits before `mdat` (osvtool's ffmpeg writes `+faststart`)
every `stco` / `co64` offset of every track moves by the bytes `moov` grew.
An existing V1 box or `st3d` / `sv3d` pair is replaced, never doubled.
`--mode reframe` is flat video, and `equirect-polar` puts the lens axes at the
poles, a layout neither scheme describes, so neither is tagged.
