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

## Index table

The second `free` box holds three 16-byte entries. For `camd` the offset is
the payload start and the size the payload length; for `covr`/`snal` the
offset points at the `ilst` item box while the size is the raw JPEG length
(the JPEG begins at offset + 24). OpenOSV verifies every entry against the
box tree and flags mismatches instead of trusting the table.
