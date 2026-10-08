# Changelog

All notable changes to OpenOSV are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

* **One correction for the seam instead of three, and it keeps straight
  lines straight.** 0.5.1 built the seam correction from a 1-D shift table,
  a 2-D flow grid and a per-column switch between the two; every switch put
  a kink into lines crossing the seam, which is the bump the car roof and the
  hood crease showed. The new mesh warp solves the whole correction as one
  smooth field: it follows the measured parallax, keeps detected straight
  edges straight, falls to zero away from the seam, and - across frames -
  holds still on a static scene but follows a real change within a bucket or
  two. It uses the flow on the raw picture and on the picture already
  corrected by the seam table, so it reaches both the fine texture of an
  aerial clip and the large offset of a car body a metre away. On the car
  drives (band-level, plug-in engine, classical flow) detected lines bend
  7-14 times less than with 0.5.1 (0.016-0.029 px RMS against 0.16-0.20 px
  on the day frames), the car body lines up better (0.990-0.994 against
  0.967-0.978) and the whole seam a little better (0.997 against 0.993 on
  the day proxy); it costs 11-15 ms of CPU per measured bucket. Where
  nothing can be measured at all - the stick strip of the aerial sample, the
  thin overlap beside a car mount's blind arc at night - 0.5.1's copied
  correction still scores a little higher (sample frame 60: 0.915 against
  0.920). `osvtool seam --mesh` scores it (`--temporal N` for consecutive
  buckets, `--mesh-prior` for the field exactly as the plug-ins render it).
* `osvtool render --purpose exact|interactive|playback` renders the way
  Premiere asks during playback; `playback` parks on the first frame, plays
  the range twice and writes the second pass, so a byte compare with an
  `exact` render shows whether playback and a parked frame agree.

### Fixed

* **The seam is one smooth field in Premiere, Resolve and VEGAS.** The
  plug-ins now render the mesh correction above instead of the 0.5.1 mix of
  shift table, flow grid and per-column switch, so a roof edge or a pole
  crossing the seam no longer picks up a kink where the two used to hand
  over. Measured on the day drive's proxy, field as the plug-ins render it:
  the car body lines up at 0.992 instead of 0.971 (median of 18 frames),
  detected straight edges bend 0.01-0.04 px instead of 0.06-0.26 px, and the
  whole seam scores higher on every day frame. The seam table is still
  measured - it is the field's starting point - but never rendered on its
  own while Parallax Grid is on.
* **Playback shows the frame you get when you stop.** During playback a
  frame used to borrow corrections measured on itself, so a propeller or a
  roof edge could look different playing than parked. Now each 8-frame
  bucket is measured once, on its first frame, and every frame of it renders
  the same whether playing or parked - byte for byte on the 6K sample (17 of
  17 frames) and the day drive (32 of 32). A frame whose bucket is still
  being measured shows the previous bucket's correction until it lands.
* **The seam moves less from one moment to the next.** Each bucket's field
  leans on the bucket before it, so measurement noise is held still while a
  real change (a car passing) still comes through: the field changes 1.4-1.6
  times less between buckets on the clips measured, and on the day drive the
  stitch at the car body flickers less (frame-to-frame change 0.60 against
  0.71, in thousandths of full scale).
* Parallax Grid Steady and Auto judge the new field: the clip correction is
  the median of nine frames' fields, then kept straight along every line the
  nine frames show. Auto now holds the day drive steady and lets the 6K
  aerial sample follow the scene (the other way round in 0.5.1); the
  sample's propeller blade, which bent at the seam in 0.5.1, renders single
  and straight.

Known limits: on the night drive the car body next to the mount's blind arc
lines up a little worse than 0.5.1 (0.717 against 0.768 at frame 2000,
0.924 against 0.947 at frame 3500); the clip correction takes about 30 %
longer on the CPU (5.5 s against 4.2 s on the 6K sample in `osvtool`), and
one bucket's solve 15-19 ms on four CPU threads.

## [0.5.1] - 2026-10-07

Premiere on a car mount, fixed at the cause. Built on the same two 8K car
drives as 0.5.0, this time against what Premiere Pro actually renders: the
classical flow and the seam-shift table, not osvtool's neural default.

### Fixed

* **Light poles beside the seam are straight again, and the seam no longer
  jumps every eight frames.** Where the parallax warp is refused, which in
  Premiere is most of a car-mounted clip, the seam-shift table is the whole
  correction. It took a shift from every column that matched at all, so
  featureless sky gave random shifts of up to ±4°, and they bent anything
  running along the seam. Each column's match now carries a confidence (the
  match, the texture under it, one clear, bracketed peak, and how much of the
  window both lenses see), and the table is a robust, confidence-weighted
  smooth of the measurements that only a column at least 2 % confident can
  move. On the 8K day clip a lamp pole next to the seam goes from 2.85 px
  RMS wobble (bends up to 15.5°) to 0.19 px and bends of at most 0.22°, the
  same as with no table at all; on its proxy from 3.32 px to 1.01 px.
* **The seam table no longer shifts what neither lens can match.** On a car
  mount the occlusion polygons leave about 98° of the seam ring with no lens
  overlap. The search used to accept chance matches there and spread them
  across the whole arc, moving the car body near the cut by up to 1°. Those
  columns now stay exactly where the calibration puts them, and a near object
  measured beside the arc fades out over its first 8.4°. Known: with those
  chance shifts gone, the lower roof edge where the car roof crosses the
  mount-side seam steps by 2.6° (it was 1.5°, offset by accident); the upper
  edge improves from 0.86° to 0.26°. That crossing has no lens overlap at
  all, so no seam table can fix it; Hide Mount (below) gives the overlap
  back.
* **A table change steps at a bucket edge only when both measurements are
  sure of it.** A near object arriving still switches at once; matching noise
  glides. The applied table's per-frame change at bucket starts drops from
  3.4° (p99) on 16 % of the columns to 0.04° on none, and the overlap match
  after the table rises from 0.990 to 0.992 (NCC, proxy 5872-5920).
* **Horizon Lock is level on mounted clips, in every host.** 0.5.0 still
  left the two 8K car drives leaning: lamp posts 28° off on the sunset
  drive and 7-11° at night, the sunset sun at +36° when it sat 7° above the
  horizon. The camera's stored attitude was being read transposed, and the
  airborne sample, whose attitude barely moves, could not tell the two
  readings apart. The new reading levels both drives within about 4° of
  their lamp posts and keeps the sun at +6 to +10° through a 106° turn,
  with no help from the accelerometer. Full and Smooth now turn with the
  car instead of against it. Premiere, VEGAS and Resolve share the fix, the
  Playback Proxy included. The sample's horizon moves by less than half a
  pixel. Saved projects with Pan keyframes on mounted clips will see their
  horizon move: that is the fix.
* The accelerometer no longer steers the levelling; it is a canary. The
  log names the angle between its gravity and the attitude's up (under 1°
  on the car drives) and warns above 15°.
* **The parallax correction now aligns a car body a metre from the lenses.**
  The classical flow solver (the one Premiere, VEGAS and Resolve run, on the
  CPU and on CUDA) could not reach the 14 px (2.4°) offset a roof rail shows
  along the seam, so an accepted correction left the rails doubled. It now
  searches along the seam direction before it refines, and only moves where
  the match is unambiguous. Measured on the car-mounted day clip with the
  correction applied, the car body goes from 0.84-0.87 to 0.93-0.97 overlap
  NCC and the whole seam band from 0.967-0.987 to 0.987-0.994; the night
  clip's seam band and the 6K sample hold or improve (sample OSV frame 60:
  0.917 to 0.920). The flow solve costs about 10 ms more per bucket on four
  CPU threads (~35 to ~44 ms).
* **The parallax correction no longer switches on and off from one moment
  to the next.** Whether a moment used its 2-D correction depended on how
  much of the whole seam band's flow checked out, and on a car mount most of
  that band is sky, where no solver can measure anything. The share hovered
  around its 25 % bar (19-35 % on the day drive's proxy), so the seam flipped
  between the correction and the seam table: 7 of 12 consecutive moments
  used it. The check now counts only the pixels with detail in both lenses:
  71-82 % on the same moments, all 12 used, and 43-97 % on every measured
  frame of the day, night and sample clips. A moment close to the bar fades
  its correction out and the seam table in, instead of switching. Lens pairs
  from two different moments of the drive score 10-28 % and are all
  refused (the bar is 30 %). The 6K sample and the night drive render byte for byte as
  before; the lamp pole beside the 8K day drive's seam stays straight (0.18
  px RMS) now that the correction applies there. Where the proxy uses the
  correction instead of the seam table, the car's hood matched less well
  across the seam (overlap NCC 0.91-0.97 against the table's 0.96-0.97): the
  flow does not follow the hood's 2.5° offset. Keeping the table under the
  whole correction scores 0.98-0.99 there but costs the 6K sample (OSV frame
  60: 0.913 against 0.920), so instead the seam table now takes over only
  the stretches of the seam the correction could not measure, where the
  table is sure of its match and found an offset the correction missed. The
  hood goes to 0.94-0.98 (median of 15 moments 0.967, from 0.951) and the
  whole seam band to 0.988-0.996 (median 0.993, from 0.989); the 8K
  original gains on its car body too. A stretch goes to the table only
  where it is at least 60 % sure of its match: on the night drive it is at
  most half sure of the hood and wrong there (0.62 and 0.81 against the
  correction's 0.77 and 0.95), so the hood keeps the correction's
  alignment, and the whole band its 0.983-0.991. The 6K sample renders
  byte for byte as before (no stretch qualifies there). Where a moment
  hands a stretch between the correction and the table, the table now
  glides with the correction instead of stepping at the moment's first
  frame, and under a partly trusted correction the table's share steps
  only as far as it carries the column: on the car body the change at
  bucket starts stays within 1.11x the change inside buckets (1.05x
  before, 1.11x with no correction at all).
* A hardware-decoded picture that FFmpeg flags as damaged, or that is
  predicted from one, is decoded again in software instead of being
  delivered. As with any other hardware decode failure, on the importer's
  host path the clip then stays on software decoding until its importer
  instance closes: about 1 s per random-access landing on an 8K .OSV, a few
  ms a frame on a proxy. On the GPU frame path the frame takes the host path,
  and three such frames in a row move the clip to it. A software decode of a
  damaged recording sees the same damage, so this only helps when the
  hardware decoder is at fault.
* **VEGAS and Resolve: non-square projects are no longer stretched.** In an
  HDV 1440 x 1080 project (pixel aspect 4:3), a DV project or an anamorphic
  one, the reframed view came out stretched sideways by the pixel aspect:
  the camera took every pixel as square. It is now built for the picture the
  host displays, in OpenOSV Source and OpenOSV 360 Reframe, on the CPU and
  on the GPU. The Zoom read-out and the presets use the displayed shape too.
  Square-pixel projects render exactly as before. Engine ABI 7.
* **The playback proxy smooths over the same time as the clip.** With Smooth
  or Smooth + Horizon Lock, the `.LRF` proxy (VEGAS Draft and Preview
  playback, an `.LRF` beside its `.OSV` in Premiere) smoothed over twice the
  time the `.OSV` did, so the preview's framing drifted up to 1.2° (day
  drive) and 1.9° (night drive) from the final render. It now smooths over
  the same seconds: at most 0.11° apart. The `.OSV` render is unchanged.
* **A clip without a recorded focal length still opens.** A recording mode
  no rule knows (4K, or anything new) needed the camera's
  `digital_focal_length` to work out its scale, and without it the clip
  refused to open. It now opens on the 3776 px crop every other rule assumes
  when the file says nothing, and the log says the scale is unverified.
* **A proxy never gets its parent's focal length.** An `.LRF` repeats the
  focal length of the full-size clip it was recorded beside. On a sensor
  other than the Osmo 360's 3840 px one, OpenOSV read that number as the
  proxy's own and built a lens about 3.7x too long. A proxy now takes its
  parent's scale, shrunk to its own width, capped so it can never claim to
  show less than 3000 sensor pixels. Every measured mode keeps its scale:
  8K 1.0, 8K LRF 0.2666667, 6K 0.794492, 6K LRF 0.2711864; renders of the
  8K and 6K test clips are byte-identical.
* **A ranged osvtool render plays its own sound.** `osvtool render --range
  A-B` (or `--frame`) into an `.mp4` copied the source audio from 0:00, so
  frames 3000-3020 of a 25 fps clip played the first 0.84 s of sound
  instead of the sound at 2:00. The audio copy now starts at the first
  rendered frame's moment, from the clip's exact frame rate (frame x
  denominator / numerator). Measured on a 25 fps clip, frames 3000 and 3001
  start their sound at 120.000 s and 120.040 s of the source to the sample.
  A render from frame 0, and `--all`, run exactly the ffmpeg command they
  always did.

### Added

* **Hide Mount** (Source Settings, and the OpenOSV Source generator in
  Resolve and VEGAS: On, Off, Auto). Off stops the calibration's occlusion polygons
  from cutting the seam, so the two lenses keep their full overlap where the
  mask used to leave none. Car, helmet or suction mount and a step at the
  seam: set it to Off. The mount itself can show. On is the default and what
  every existing project keeps, bit for bit. On a car-roof clip the roof
  line where it crosses the seam steps less (0.97° to 0.63° on the proxy);
  near parts of the car that cross the seam still step. osvtool `render
  --no-occlusion` now renders Off on the plug-in engine too.
* **Hide Mount: Auto**, the third choice (On stays the default). Auto looks
  at the clip once, on the nine frames the steady seam uses, and keeps the
  occlusion polygons only where the two lenses disagree. Where both see the
  same scene the overlap comes back, as with Off; around the mount the mask
  stays. Where neither lens was fully trusted on the seam, Auto asks, stretch
  by stretch, which lens does not see the mount; where the clip says so
  clearly, that lens covers the seam at full weight, and where it cannot
  tell, the polygons stay exactly as the calibration drew them, so the mount
  never comes out sharper than with On. On a car drive's proxy Auto releases
  21° of the polygons' 120° arc (hood and roof side), the seam ring's stretch
  with no lens overlap shrinks from 98° to 69°, and the suction cup looks the
  same as with On. The roof's lower edge still steps 2.7° at the seam: it
  crosses where neither lens can be shown to be the clean one, so the mask
  stays there. On the 8K original Auto releases 18°; its polygons never
  covered the seam plane, and that roof edge still steps about 3°: near-field
  disparity along the seam, which no mask removes. The 6K sample keeps its
  polygons and its frames, bit for bit; its proxy does not, which is why Auto
  is not the default. Measuring costs 0.35-0.6 s once per clip, off the
  render thread, and the verdict is cached next to `lens-alignment.tsv`, so
  the effect's direct view uses the same polygons. osvtool:
  `--hide-mount on|off|auto`.
* The plug-in logs now carry the library's own messages and FFmpeg's, at the
  log's level. Before, inside Premiere, they went nowhere. FFmpeg lines name
  the codec and the clip they are about, and arrive from WARNING up, each
  repeated message once per clip.
* Diagnostics for a proxy frame with the wrong picture in part of it
  (docs/PREMIERE.md, Troubleshooting). At Debug, every decoded picture and
  every delivered frame gets a fingerprint line. `OPENOSV_VERIFY_HW_DECODE=1`
  checks every hardware-decoded proxy picture against a software decode and
  delivers the software one on a mismatch. `OPENOSV_IMPORTER_LRF_SOFTWARE=1`
  decodes the proxy in software only. `OPENOSV_VERIFY_DELIVERY=1` checks that
  the frame handed to Premiere is the frame that was rendered; its line names
  both the host's frame and the clip's own frame rendered for it, which
  differ for an .LRF presented as its .OSV's proxy and for a clip that
  dropped frames.
* The per-bucket parallax lines in the log name the clip, and a refusal says
  how much of the flow was consistent, over the pixels with detail in both
  lenses and over all of them; an accepted line adds the strength it applies
  at. `osvtool seam --parallax --json` reports the same numbers, on a
  refusal too, and scores `--region` uncorrected and with the seam table
  whether or not the correction is refused.
* The OpenFX generator's log records each instance's output bounds against
  its region of definition, the project's pixel aspect and field order, and
  the first single-field render of an interlaced project.
* The log notes a `digital_focal_length` more than 0.2 % off the Osmo 360
  convention (0.2764537 x the clip's lens width). Log only: a camera or
  firmware that writes something else shows up before its seam does.
* osvtool `seam --search --json` reports each column's confidence and raw
  measurement and the unmeasured and confident column counts. `--dump-bands`
  writes the uncorrected bands as `_raw_` and the table-corrected ones as
  `_table_`; it used to label the corrected pair `_none_`.
* osvtool `seam`: the alternatives table is built from the lens image, as
  the main rig is. On an `.LRF` every alternative used to read -2; the
  "no crop" row is now the lens width over the sensor width (0.78125 on 6K,
  as before).
* osvtool `--attitude-convention` takes a `-rig` suffix: `auto` is
  `xyzw-w2b-z-rig`, and `xyzw-b2w-ny` still gives the old reading, for
  comparisons.

## [0.5.0] - 2026-10-07

8K and night footage, fixed at the cause. Built on a user's car-mounted 8K
clips, a sunset drive and a night drive, which DJI Studio stitched cleanly
and OpenOSV did not.

### Added

* **Scene Light** (Source Settings: Auto, Day, Night). Auto reads the
  camera's own exposure meter. Only when that says dark does it look at the
  sky above the levelled horizon, and Night needs both. Night keeps the Sky
  Seam Fix on the seam itself (it fades out over 6° instead of 20°, and
  clamps at 0.75 stop) and switches off Exposure Match and Lens Shading,
  which street lights fool. Auto decides a daylight clip from its metadata
  alone, without decoding a pixel, and renders it exactly as before.
* **Lens Focal** (Source Settings: Auto, Camera, Calibration): where each
  lens's focal length comes from. Leave it on Auto. The other two are there
  in case a recording mode's focal is wrong for your camera, which shows up
  as doubled straight lines at the seam.
* osvtool `render --scene-light auto|day|night` and `--lens-focal
  auto|camera|calibration`.
* osvtool `render --alpha` writes the coverage alpha into `.exr`, `.tif` and
  `.png` stills. Off by default; videos carry no alpha.
* **A self-check on delivered frames.** The importer checks the first three
  frames of every size it delivers. If a band of rows comes out transparent
  or black, it logs a warning naming the size Premiere asked for and the
  size it got. The log also records every requested size next to the
  delivered one. `docs/PREMIERE.md` has a new Troubleshooting section.

### Fixed

* **8K clips no longer double at the seam.** In 8K mode the focal length
  the camera records is 1.3-2.7 % longer than each lens's own calibration.
  That put every depth in the overlap 3-4° out: doubled lamp posts, stepped
  overpass lines, and S-bends where the parallax warp tried to absorb an
  error it was never built for. Each lens now uses its own calibrated
  focal, unless the recorded one agrees within 0.5 % (6K mode, unchanged).
  The `.LRF` of an 8K clip is now mapped as the full 3840 px frame, not the
  6K crop. The lens overlap's match goes from 0.72-0.87 to 0.92-0.96 (NCC).
* **Horizon Lock levels on the gravity the camera measured.** On a clip
  whose accelerometer reads a clean 1 g (a car or tripod mount), the old
  automatic reading could roll the horizon a quarter turn. And the camera's
  attitude frame is not level: the measured gravity sits about 9° and 29°
  off it on the two 8K clips, so the horizon leaned by that much. Airborne clips, whose accelerometer
  swings, level as before.
* **The night sky halo is gone.** The Sky Seam Fix's brightness field
  reached 20° into the sky, and was measured on a noisy black one. On the
  night drive, the correction 10-30° from the seam drops by about 90 %, and
  its frame-to-frame flicker from about ±4 codes to under 1 (Scene Light,
  above).
* **Clips that dropped frames stay in step with their sound.** Long night
  recordings at high ISO can drop frames. OpenOSV mapped each frame's time
  at the clip's average rate, so it decoded the wrong frame, or none.
  Every frame now decodes as itself. The clip is presented at its recorded
  rate (50 fps, for example), with the previous picture held over each
  dropped frame, timed by the camera's own clock. A 5.5-minute night clip
  that ended 3.4 s before its audio now ends with it, in Premiere, Resolve,
  VEGAS and osvtool. This also ends "presentation time mismatch" failures,
  and a whole clip dropping to software decoding after one bad frame.
* **Sun Ghost Removal no longer takes street lamps for the sun.** On about
  3 % of a night drive's frames it cut dark patches into door reflections,
  lane markings and headlights. A scene the camera metered too dark for the
  sun (EV100 below 6, from the clip's own ISO, shutter and aperture) is now
  left exactly as recorded, and the log says why. The two lenses can no
  longer report two different suns. 8K night clips don't slow down with
  removal on anymore. Daylight footage is unchanged: the sample clip
  renders byte-identically, and a sunset clip's sun is still found on
  every frame.
* **Seam corrections no longer pop when a moment's parallax measurement is
  refused.** The parallax grid fades out while the seam table fades in, and
  seam tables glide between moments where the two agree. A passing pole no
  longer doubles on the frames after a refused moment.
* **An exported or paused frame has the same seam geometry and exposure
  gain wherever playback, scrubbing or the export started.** Each moment's
  analyses are now measured on fixed frames. The Sky Seam Fix, Lens Shading
  and Sun Ghost Removal still carry some history from frame to frame.
* **Parallax Grid Steady and Auto survive a frame the decoder can't read.**
  The clip-wide measurement replaces that frame with a neighbour, or skips
  it, and the log says which. Before, it quietly fell back to per-moment
  corrections.
* **CUDA rendering keeps going after a clip falls back to software
  decoding.** Before, every later frame of that clip failed with "chroma
  pitches differ".

### Changed

* **A clip that dropped frames gets slightly longer.** The night clip went
  from 16157 to 16328 frames. In a project edited in 0.4.2, cuts after the
  clip's first dropped frame move. Clips that dropped nothing are
  byte-identical. osvtool `probe` reports the presented frame count and
  rate, and `render --frame/--range/--all` count presented frames, so an
  `.mp4` stays in step with its audio. `extract --frame` still addresses
  recorded frames.
* **Parallax Grid Auto lets a few small misses through.** A clip-wide
  correction is no longer vetoed by a single misaligned sector. It is kept
  when at most 5 % of the judged sectors fail, and none by more than 0.10
  NCC.
* **Playback borrows only the moment right before.** It used to reach up
  to 32 frames back. Without one, it measures on the frame itself.
* The per-moment schedule's renders differ slightly from 0.4.2. On the
  sample clip, 0.3 % of pixels move by more than one 8-bit code.

## [0.4.2] - 2026-10-01

VEGAS Pro playback, rebuilt for speed.

### Added

* **VEGAS Pro: Playback Proxy** (OpenOSV Source > Advanced, on by default).
  At Draft and Preview quality the generator plays the `.LRF` the camera
  recorded beside the `.OSV`, at the same moment on the camera's clock. Good
  and Best always stitch the `.OSV`, so renders never touch the proxy.
* At `OSV_PLUGIN_LOG_LEVEL=debug`, every VEGAS frame logs where its time
  went: the quality VEGAS asked for, decode, analyses, stitch and packing
  (`frame-cost path=device`), and the readback.

### Changed

* **VEGAS Pro: playback is no longer a final render.** VEGAS names its
  preview quality in a property of its own, which OpenOSV never read, so
  every playback frame waited on every analysis and ran the full seam
  search, parallax and sun ghost fit. Draft and Preview (the Preview
  window's default) now render as playback; Good and Best, what File >
  Render As uses, keep the full stitch.
* **VEGAS Pro: the reframed view renders straight from the fisheyes on the
  GPU.** Only the pixels the camera shows are stitched: 2 MP for a 1080p
  view instead of the 29.5 MP of an 8K sphere, with 472 MB less VRAM. One
  resampling instead of two, so the view is sharper too. `OPENOSV_OFX_DIRECT=0`
  brings back the sphere path.

### Fixed

* A frame asked for twice in a row on the host-decode path (an `.LRF`
  recorded at half the `.OSV`'s rate asks for each of its frames twice) no
  longer seeks back and decodes its whole GOP again.

## [0.4.1] - 2026-09-30

VEGAS Pro 17.0 works: import, the camera and playback, live in VEGAS. Still
experimental.

### Fixed

* **VEGAS Pro: creating OpenOSV Source media no longer crashes VEGAS 17**
  (the 0.4.0 known issue). Right after creating an instance, VEGAS replays
  every parameter as a change. Answering those, the camera opened an undo
  group, which VEGAS 17 can't take at that point. Under VEGAS the camera's
  edits now go without an undo group.
* **VEGAS Pro: the camera starts where it should.** VEGAS's replay after
  creation reads like a user edit, twice. OpenOSV now tells the replay (many
  parameters in one change bracket) from a real edit (one), so it no longer
  switched the lens to Classic or re-applied the preset over new media.
* **VEGAS Pro: imported clips play every frame.** Media made by Import OSV...
  runs on a millisecond clock (frame rate 1000). OpenOSV read that as a frame
  number, so a clip froze on its last frame a few frames in. The time now
  maps to the clip's frame through seconds, at any rate up to 1000, with
  half a millisecond of slack for VEGAS's rounding.
* **The VEGAS extension rebuilds when the version changes,** so its assembly
  version always matches the release.

### Changed

* **VEGAS Pro 17.0 is experimental, not untested.** README Tutorial 4 walks
  through it step by step; `docs/VEGAS.md` has the live-test checklist.
  DaVinci Resolve stays a preview.
* At `OSV_PLUGIN_LOG_LEVEL=debug` the OpenFX trace also names each changed
  parameter and the reason, the time of each render, and the clip frame it
  maps to.

### Known issues

* **VEGAS Pro HDR and ACES projects:** the picture doesn't match the project's
  output colour space yet. Stay on Rec. 709.

## [0.4.0] - 2026-09-30

### Added

* **VEGAS Pro (preview).** OpenOSV Source and OpenOSV 360 Reframe load in
  VEGAS Pro through OpenFX, from the same `OpenOSV.ofx.bundle` as DaVinci
  Resolve (VEGAS scans `Common Files\OFX\Plugins` too), and a VEGAS
  Application Extension does what an OpenFX generator can't:
  **Tools > Extensions > OpenOSV > Import OSV...** creates the generator media at the
  clip's exact length and size, extracts the audio and groups it with the
  video. The extension also applies 360 Reframe to equirect events, and has
  framing looks, easing presets, stabilisation and an `.LRF` proxy toggle,
  in a dock panel as well as the menu. OpenOSV Source gets an output-levels
  choice under VEGAS (full range or studio RGB), because VEGAS never converts
  a generator's levels. Not yet run inside VEGAS: see `docs/VEGAS.md` for
  what is checked and the live-test checklist.
* **`scripts/install_vegas.ps1`** installs the bundle (through
  `install_ofx.ps1`), copies the extension into `%ProgramData%\VEGAS Pro\Application Extensions`
  and unblocks it, and clears VEGAS's plug-in caches so the next start scans
  afresh. It refuses while VEGAS runs, elevates once, offers `-Uninstall`,
  `-SkipBundle` and `-DryRun`, and leaves every other file in VEGAS's
  folders alone.
* **A third release zip, `OpenOSV-x.y.z-vegas-windows-x64.zip`,** with its own
  `Install.cmd` / `Uninstall.cmd`. `scripts/package_release.ps1` fails the
  package when the VEGAS extension isn't built (it compiles against
  `ScriptPortal.Vegas.dll` from a VEGAS Pro install); `-SkipVegas` leaves the
  zip out on purpose.
* **Everything on the GPU for hosts that hand OpenFX plug-ins CPU images**
  (VEGAS always does). OpenOSV Source decodes, stitches, frames, levels and
  packs on the GPU and reads back only the finished view - about 8 MB a frame
  at 1080p 8-bit instead of the 288 MB 6K sphere. OpenOSV 360 Reframe uploads
  VEGAS's image in its own depth and frames it on the GPU. On by default under
  VEGAS only; `OPENOSV_OFX_GPU=0` / `1` switches it off / on for any host.
* **`osvtool probe <clip> --json -`** prints the probe as JSON on stdout, with
  a stable top-level subset (`schema: openosv.probe/1`: frame count, exact
  rational frame rate, duration, stream size, mode, colour mode, audio,
  LRF or not) for scripts and the VEGAS extension.
* **`osvtool extract <clip> --audio out.wav`** writes the audio as 32-bit
  float WAV, decoded exactly as the Premiere importer plays it (priming
  removed, sample 0 on video frame 0). `.aac` still copies the AAC track.

### Changed

* **OpenFX bundle layout (Windows).** `Contents\Win64` now holds
  `OpenOSV.ofx` alone; its DLLs, and `osvtool.exe`, moved to
  `Contents\Libraries\Win64`. VEGAS's plug-in scan loads every DLL under
  `Contents\Win64`. Resolve installs through `install_ofx.ps1` get the new
  layout automatically; a copy made by hand must take the whole bundle.
* **OpenFX plug-in versions 1.1**, so hosts that cache descriptors look again.
* **`osvtool probe --json`:** the top-level `schema` key now names the JSON
  layout (`openosv.probe/1`); the djmd field numbering it used to hold moved
  to `djmdSchema`.

### Known issues

* **VEGAS Pro: creating OpenOSV Source media crashed VEGAS Pro 17** in both
  automated live runs, inside VEGAS right after the generator's Create
  Instance action (Import OSV... creates it the same way). Not diagnosed yet;
  the VEGAS package is experimental. VEGAS did find, describe and load both
  effects and the extension. See `docs/VEGAS.md`.

## [0.3.0] - 2026-09-28

### Added

* **OpenOSV Studio (`osvgui`), a batch app for `osvtool`.** Drop `.OSV`
  files or whole folders on it, pick the output, press Start. 360 equirect
  or a reframed view, size, colour, HDR style, stabilisation, sun ghost
  removal, encoder, quality, audio and the 360 tag are each an `osvtool
  render` option, and the window shows the exact command it runs next (or a
  whole-folder loop to copy). The queue renders one clip at a time with
  progress, time left, pause after the current clip and stop; folders are
  searched all the way down and `.LRF` proxies skipped. It picks the fastest
  HEVC encoder that works on the machine, points to FFmpeg when it's
  missing, and remembers every setting. Windows and macOS; `cli\osvgui.exe`
  in both release zips. See `docs/STUDIO.md`.
* **360 video metadata.** `osvtool render --mode equirect` now tags its `.mp4`
  / `.mov` as 360 video: Spherical Video V1 (the `uuid` box YouTube reads)
  and V2 (`st3d` + `sv3d`, what FFmpeg, VR players and 360 editors read).
  Upload it as it is; Google's Spatial Media Metadata Injector is no longer
  needed. On by default, `--no-spherical-metadata` turns it off. Reframes
  stay flat.
* **`osvtool spherical`** tags an existing equirectangular video the same
  way, such as a Premiere Pro or DaVinci Resolve export:
  `osvtool spherical export.mp4` in place, or `--out tagged.mp4`. Only the
  `moov` box is rebuilt; the media data is streamed to a temporary file that
  replaces the original once complete, so a 50 GB file needs no 50 GB of
  RAM and a failure leaves it untouched. Chunk offsets move with the `moov`.
  A file already tagged is left alone, an older tag is replaced rather than
  doubled, and truncated, malformed or fragmented files are refused.

### Fixed

* **Wavy, stair-stepped lines in the stitch.** The seam correction measured
  along the seam was also applied to every other pixel of both lenses,
  rotating each one toward or away from its lens centre. Wherever the
  correction varied, straight lines far from any seam came out wavy. On an 8K
  clip, a fence thirty metres away became a staircase. The correction now acts
  only near the seam, where it was measured (full within 6 degrees, fading out
  by 12), and leaves the rest of the picture alone.
* **An attached `.LRF` proxy played back glitchy.** Premiere requires an
  attached proxy to match its original's frame rate and duration, and its frame
  size to divide the original's. Anything else is accepted without a warning
  and misbehaves. The camera's `.LRF` is 29.97 fps (25 for a 50 fps clip)
  against a 59.94 fps `.OSV`, and 2048 x 1024 against 6000 x 3000 or
  7680 x 3840. An `.LRF` next to its `.OSV` now presents itself on the
  original's timeline:
  * the original's frame rate and length, each frame showing the moment the
    original shows, matched by the camera's own timestamps;
  * a size that divides the original's (2000 x 1000 for 6K, 1920 x 960 for
    8K).

  A new proxy also starts from the Source Settings its `.OSV` is decoded with,
  so switching proxies on doesn't change the stabilisation or the colour. An
  `.LRF` on its own is unchanged.
* **The panel misread the D-Log M Curve popup** after Avata 360 joined it (four
  entries, not three). It counted the entries to learn how the host numbers
  popups, and the stale count could mistake the default curve for the last
  entry. Panel 1.0.1.

### Changed

* The importer log says how calibration pixels map to stream pixels for each
  clip, including the camera's own focal ratio for 8K, so a seam that's off
  can be told apart from parallax.
* **macOS build, step by step.** `scripts/build_mac.sh` installs what the
  build needs (Homebrew's CMake, Ninja and pkg-config; vcpkg in `~/vcpkg`),
  builds, and with `--install-ofx` installs the DaVinci Resolve bundle. A
  configure without pkg-config, which vcpkg's FFmpeg needs, now stops at once
  and names the fix instead of failing deep inside vcpkg. The README and
  `docs/BUILDING_MAC.md` give the same steps by hand. Thanks to
  [@arkanos](https://github.com/arkanos), who found the missing step and has
  the Resolve plug-ins working on a MacBook Pro M4 with macOS 26.6.1
  ([#2](https://github.com/Kemerd/OpenOSV/issues/2)).
* **The README's command-line section is a how-to now:** setting up
  `osvtool` and FFmpeg, recipes for a YouTube-ready 360 video, HDR, a
  reframed view, a test run and a whole folder, and the options that matter.

## [0.2.2] - 2026-09-27

### Added

* **DJI Avata 360 clips open, stitch and reframe.** The Avata 360 writes the
  same metadata messages as the Osmo 360 under other field numbers, so its
  calibration, focal length, sensor size, colour mode, lens accessory and
  attitude were read from the wrong places and no Avata clip opened. They are
  now read in the Avata 360's own numbering (`osvtool probe` names the schema
  a clip was read with). Not yet checked on Avata 360 footage in this
  repository: reports welcome.
* **Avata 360 colour, fitted to DJI's own Avata 360 LUT.** DJI Studio bundles a
  D-Log M to Rec.709 LUT for the Avata 360 that differs from the Osmo 360 one.
  `--fit avata360` (curve, primaries and Rec.709 look) is fitted to it the way
  the Osmo 360 fit is fitted to its own: 1.68 dE2000 mean from DJI's Avata 360
  rendering, against 2.52 for the Osmo 360 fit. The first Avata 360 curve and
  matrix (contributed in #1, fitted to DJI Studio exports before the LUT was
  found) are replaced. Osmo 360 stays the default.
* **"Avata 360" in the D-Log M Curve menus** of Source Settings (Premiere) and
  OpenOSV Source (Resolve). Saved projects read as before.
* **Avata 360 LUTs.** `luts/` gains a `DJI_Avata360` set beside the Osmo 360 /
  Pocket 3 one: Rec.2100 PQ and HLG in all five HDR styles, and Rec.709 with
  DJI's Avata 360 look or OpenOSV's.

### Fixed

* **The .LRF proxy rendered magenta.** The decoder widens the proxy's 8-bit
  samples to the 10-bit scale every render kernel reads, but the colour
  conversion was built for 8 bits: every sample read four times too bright
  and neutral chroma far off centre, so the picture came out magenta and
  white. The conversion now always follows the decoded sample scale
  (`video::kDecodedSampleBits`). Premiere, DaVinci Resolve and `osvtool`
  all had it. An importer test now checks the proxy's colours in Rec.709.
* **A new, undeletable OpenOSV Source Settings effect appeared on the master
  clip** whenever Premiere re-checked the clip (the Master tab, a sequence
  settings change, attaching a proxy). The importer named the effect by its
  bare match name, while Premiere registers it as `AE.OpenOSV.SourceSettings`,
  so it never recognised the one already there. The importer now gives the
  name Premiere uses. Extra copies a project already collected stay, but no
  new ones are added.
* **Dark patches along the seam of close-up shots.** To close a parallax gap,
  the correction moves each lens's sample toward its rim. That lowered the
  blend weights, and the alpha was taken from them, so a corrected seam went
  partly transparent in proportion to the parallax it fixed. On a camera set
  on the ground that showed as dark cones along the seam, one per row of the
  correction grid. The alpha now comes from what the lenses see of that
  direction without the correction. A direction the correction would push
  off both lenses renders uncorrected instead of as a hole.
* **SDR clips were treated as HDR.** A clip recorded in the Normal colour
  mode:
  * now starts with **Rec.709** output instead of PQ, so its picture isn't
    re-encoded as HDR. Stored settings and an explicit choice still win, and
    PQ and HLG are still in the menu.
  * now comes out of the Rec.709 output as recorded. It used to go through
    the HDR scene-light rendering (white at 75 %) and DJI's D-Log M look,
    which dimmed it and graded it a second time. The look now applies to
    D-Log M clips only.
* **D-Log M passthrough skipped Gain Match.** The exposure match between the
  lenses is now applied to the passthrough's code values, like the lens
  shading and the sky seam fix already were. Without it, a seam could keep a
  brightness step wherever the sky seam field wasn't in force.
* **`osvtool render` looked worse, and ran slower, than Premiere.** It ran a
  research pipeline of its own:
  * every correction was off unless asked for;
  * decoding was in software;
  * a seam was carved for every frame on the CPU.

  `--seam-carve` alone left the sky seams, the sun ghosts and the parallax
  that Premiere removes.
* **The Avata 360's colour mode is read from where it records it**
  (StreamMeta 2.4, #1). Every Avata clip, D-Log M included, was reported as
  Normal.
* **Horizon Leveling holds its heading on a lens-up / lens-down rig.** The
  Avata 360 flies with one lens up and one down; levelling about the vertical
  lens axis sat in gimbal lock, where a small tilt swung the view. OpenOSV
  now measures the mount from the clip's attitude track and levels about the
  body's horizontal axis instead. Clips held lenses-level, every Osmo 360
  clip, render exactly as before.

### Changed

* **`osvtool render` runs the plug-ins' own clip engine** (`--engine plugin`,
  the default). A render is the frame Premiere shows for a new clip with the
  same Source Settings:
  * parallax correction, carved seam, sky seam fix, lens shading;
  * **sun ghost removal**, new on the command line;
  * the steady per-clip analyses and lens alignment;
  * hardware decoding, and the GPU where there is one (CUDA, or OpenCL on
    AMD and Intel).

  Options you don't give start at the Source Settings defaults. That
  includes stabilisation: Smooth + Horizon Lock unless `--stab` says
  otherwise. An SDR clip renders to Rec.709 unless `--color` says otherwise.
  An equirect without `--size` takes Output Size (Native).
* **New options:** `--flare` / `--no-flare`, `--parallax-grid` and
  `--lens-align`.
* **`--engine classic`** keeps the old pipeline. It is the only home of the
  geometry-convention and blend research options (`--lens-fov`, `--hw`,
  `--blend-fov`, `--seam-interval`, `--mode equirect-polar`,
  `--color linear` and the others). The default engine refuses them by name
  rather than ignoring them.
* `osvtool.exe` delay-loads the NVIDIA driver, so it still starts on
  machines without one.

## [0.2.1] - 2026-09-24

### Added

* **Transfer Function (HDR).** A per-clip choice of how D-Log M becomes
  BT.2100 PQ or HLG light, right under Colour Output in Source Settings, the
  importer dialog and the DaVinci Resolve generator:
  * **ACES 2 - Bright (outdoor)**, the new default: a tone scale based on
    the ACES 2.0 tonescale, its contrast and toe fitted to DJI's published
    D-Log M to Rec.709 LUT. Grey sits at BT.2408's 26 nits, diffuse white at
    169, and a soft shoulder ends at 600 nits at the sensor clip.
  * **ACES 2 - Detailed (indoor)**: the same fit at a 1000-nit peak. Grey
    13.8 nits, white 98, clip 374: darker, with the most highlight detail.
  * **BT.2408 - Deep Blacks + Natural** and **+ Punchy**: BT.2408's grey
    (26), reference white (203) and a 1000-nit clip, with DJI's toe. Natural
    tone-maps luminance and keeps the scene's saturation; Punchy works per
    channel, about 40 % more chroma.
  * **BT.2408 - Neutral**: the scene-referred rendering of 0.2.0 and
    earlier, bit for bit.

  Bright and Detailed carry "(outdoor)" / "(indoor)" in their names because
  Premiere's effect controls show no tooltips; the dialog and Resolve show
  the hint. HLG and Normal clips, Rec.709 (it keeps its Look), linear and the
  passthrough ignore the choice. HDR Peak still rolls the PQ output off after
  every style. A project saved by 0.2.0 opens on ACES 2 Bright; pick BT.2408 -
  Neutral to get its HDR output back unchanged. Also in the user defaults
  file (`hdrTone`), osvtool (`render --tone`, `lut --tone`) and the direct
  path. Engine ABI 6.
* **A LUT set per output, in a `LUTs` folder.** Twelve 65^3 tables labelled
  `DJI_Osmo_*`, one set for the Pocket 3 and the Osmo 360, which share DJI's
  D-Log M LUT: `Rec2100_PQ` and `Rec2100_HLG` in all five HDR styles,
  `Rec709` with DJI's look and with OpenOSV's standard rendering, and a
  `README.txt` that says which one to use. They replace the three
  `OpenOSV_Osmo360_*.cube` files; the `BT2408_Neutral` and `DJI_Look` tables
  hold the same data those did. Installed with the plug-ins into
  `OpenOSV\LUTs\` (replacing the old files), and both zips carry the folder.

### Fixed

* **LUTs made with the `pocket3` fit turned blues purple.** On real Pocket 3
  D-Log M footage, `--fit pocket3` shifted hues about 10 degrees (blue toward
  violet, grey sky toward lavender) and flattened contrast: lifted blacks,
  highlights short of peak. The `osmo360` fit is matched to DJI's own D-Log M
  LUT, the same file for the Pocket 3 and the Osmo 360, and stays within 1 to
  2 degrees; the documentation and `osvtool --help` now say it is the one to
  use for both cameras, and the shipped LUT set uses it.

## [0.2.0] - 2026-09-24

### Changed

* **One download per editor.** `OpenOSV-0.2.0-premiere-windows-x64.zip`
  holds the Premiere Pro plug-ins, and `OpenOSV-0.2.0-resolve-windows-x64.zip`
  the DaVinci Resolve ones. Each has its own `Install.cmd`, `README.txt` and
  licence list, and both carry `osvtool` and the LUTs.

### Added

* **DaVinci Resolve (OpenFX), a preview.**
  `OpenOSV.ofx.bundle` holds two OpenFX effects:
  * **OpenOSV Source** is a generator. It opens a `.OSV` or `.LRF` through
    the importer's own clip engine, compiled in unchanged, so the stitch,
    stabilisation and colour are Premiere's. It outputs a reframed view
    taken from the native sphere, or the 360 equirect at the timeline's
    size. A **Choose .OSV File...** button stands in for the Browse button
    Resolve doesn't draw, a **Clip** read-out shows the length to trim to,
    and **Start Frame** slides the clip. Colour Output defaults to Rec. 709
    here, because a generator can't tag a colour space.
  * **OpenOSV 360 Reframe** is a filter for any equirectangular clip. It has
    the Premiere effect's lenses, presets, supervision, Keyframe Easing and
    Smooth Keyframes, and renders on the CPU or on Resolve's CUDA images and
    stream.

  `tests/ofx` loads the built bundle into a strict mock OpenFX host. It
  compares every render with the Premiere effect's CPU render of the same
  picture and controls. It checks the CUDA path against the CPU path, and on
  the sample clip it checks the generator against Premiere's two-step path
  frame for frame.

  The bundle needs no Adobe SDK, so every build makes it (`OSV_BUILD_OFX`,
  on by default): on Windows, and on macOS as `OpenOSV.ofx.bundle` with
  FFmpeg embedded, built and tested on GitHub's Apple Silicon runners. On a
  Mac the stitch runs on Metal, and OpenOSV 360 Reframe renders on the CPU.
  First run inside Resolve: Resolve 21 (free) on Windows, where OpenOSV
  Source stitches and frames a clip; nobody has run it in Resolve on a Mac
  yet. OpenOSV Source states float RGBA output in its clip preferences,
  because Resolve otherwise labels a generator's image
  `OfxImageComponentNone`. `scripts/install_ofx.ps1`
  and `scripts/install_ofx.sh` install it. The OpenFX 1.5.1 headers are
  vendored in `plugins/ofx/openfx` (BSD-3-Clause). See
  [docs/RESOLVE.md](docs/RESOLVE.md).
* **macOS on Apple Silicon (untested in Premiere Pro).** The library,
  `osvtool` and the test-suite build with AppleClang and pass on GitHub's
  macOS runners (`.github/workflows/macos.yml`, presets `macos-*`, vcpkg
  triplet `arm64-osx-openosv`). A Metal renderer (`--device metal`, first
  in `auto`) matches the CPU reference to the same 60 dB bar as CUDA and
  OpenCL; hardware decode is VideoToolbox. The importer, Open 360 Reframe
  and the Source Settings effect build as Mac bundles with their own FFmpeg
  inside, and the effect's GPU path is Metal. The plug-ins build on CI only
  when a private SDK archive is configured, since Adobe's SDKs are never
  committed; the parts that need no SDK - including the effect's Metal path
  against its CPU path and the bundle layout - are tested there.
  `scripts/install_plugins.sh` installs the bundles, LUTs, sequence presets
  and the panel; `scripts/package_macos.sh` makes
  `OpenOSV-<version>-macos-arm64.zip`. Not on a Mac yet: the direct GPU
  path (CUDA), the neural flow backend and the modal Source Settings dialog
  (the Source Settings effect does that job). Windows builds and output are
  unchanged. See [docs/BUILDING_MAC.md](docs/BUILDING_MAC.md).

## [0.1.0] - 2026-09-23

### Added

* **A Windows download.** `OpenOSV-0.1.0-windows-x64.zip` on the GitHub
  Releases page: the three plug-ins, the OpenOSV window, the LUTs, the
  sequence presets and `osvtool`, with a double-click `Install.cmd` /
  `Uninstall.cmd` and every third-party licence. `scripts/package_release.ps1`
  builds it and checks every shipped file before it is zipped
  (`docs/RELEASING.md`).
* **RockSteady and Horizon Leveling together, on by default
  (WP-STABPAIR).** DJI Studio runs its two stabilisation switches
  independently, and both can be on. The Stabilisation popup in Source
  Settings gains "Smooth + Horizon Lock" as its fifth item: the view follows
  the heading of the Gaussian-smoothed orientation (the shake is gone, as in
  Smooth) with pitch and roll levelled to the world horizon (as in Horizon
  Lock) - the Horizon Lock levelling applied to the smoothed pose. New clips
  start on it. On the sample clip it is level on every frame, and the view's
  forward axis swings 0.87 deg in total from frame to frame over the clip,
  against 1.84 deg with Horizon Lock. The item is appended, so a project saved
  before it keeps its entries 1-4 exactly. Also in the importer dialog, the
  user defaults file (`smooth-horizon-lock`), osvtool (`render --stab
  smooth-horizon`; the command line still defaults to `off`) and the engine
  (the smoothed attitude reaches the direct path's frames). The companion
  panel's Stabilisation card is now DJI Studio's two switches, RockSteady and
  Horizon Leveling, both on by default; the pair picks the Source Settings
  entry and the card names it. Full, which no pair spells, is left alone
  until Apply is pressed, and the status line says when Apply replaced it.
  A panel that remembered the older single choice starts from the matching
  switches (its old default, Horizon Leveling, as the new one, both on).
* **Steady seam and Lens Alignment (WP-STEADY).** The "slight movement at
  the seam" was the seam corrections being re-measured every eight frames.
  Rendered with the corrections of every frame on one frozen frame, the
  picture around the sample's nacelle moved by up to 1.29 px per frame at
  6K (p99 of the worst frame pair, 55 of 64 pairs above 0.25 px): the
  parallax grid's glide. The carved seam barely changed the picture there
  (0.14), and the seam-shift table - in force wherever a bucket's grid is
  refused - stepped by up to 12.3 px at a bucket edge. "Parallax Grid" in
  Source Settings (Stitching group) now offers Steady - all three measured
  once per clip on nine fixed frames and their median used for every frame,
  so nothing at the seam moves (0 px) - next to Follows scene (the
  per-moment schedule) and Auto, which holds them still unless a near object
  both lenses see moves past the seam. On the sample Auto holds them and the
  alignment stays: ground 0.925 / 0.932 / 0.930 against each frame's own
  0.922 / 0.932 / 0.932. "Lens Alignment" (Auto / Off) fits the 0.36 deg
  rotation between the two lenses once per clip from three fixed frames
  (they agree to 0.004-0.008 deg) and folds it into the rig before every
  analysis: the ground's lens-to-lens NCC goes from 0.37 to 0.88 before any
  flow, and to 0.932-0.946 with the grid (0.922-0.932 before); whole band
  0.916-0.921 -> 0.923-0.925; the sky 0.976 -> 0.974-0.975. Both are
  measured on a background worker from the clip's first real frame and
  shared by every instance of the clip: Exact frames (export, the Program
  monitor's direct path) wait for them once per clip, never per bucket;
  Interactive frames never wait and render the same pixels as Exact once
  the clip correction exists. Cost, once per clip: 0.16-0.33 s for the
  rotation (remembered on disk, so a reopen is free) and 0.74-0.95 s for the
  clip correction, after which none of the three per-bucket analyses runs.
  Auto / Auto for new clips (and, like every control added to the Source
  Settings effect, for an effect saved before them); a prefs blob written
  before them reads as Follows scene / Off and renders exactly as before.
  Also in the importer dialog, the user defaults file (`parallaxGrid`,
  `lensAlignment`) and osvtool (`seam --lens-align --steady --regions`).
  Engine ABI unchanged (5).
* **HDR Peak Brightness for the PQ output (WP-HDRPEAK).** PQ is rendered for
  a 1000-nit display, so sunlit white is genuinely bright: the sample's white
  aircraft sits at 525 nits median, 98 % of it above diffuse white, and looks
  blown out wherever the display or the conversion cannot show that. "HDR
  Peak (PQ only)" in Source Settings rolls the highlights off into 1000
  (default, unchanged), 600, 400 or 203 nits ("SDR-safe") with the BT.2408
  Annex 5 EETF per component: everything below the knee - 464 / 251 nits for
  600 / 400, so diffuse white, faces and mid-tones - is left bit for bit; 203
  keeps the whole picture under reference white (its knee is 88 nits, so
  diffuse white lands at 159). The aircraft at 600 / 400 / 203: median
  514 / 381 / 200 nits, max 600 / 400 / 203, shading across it 1.10 / 0.54 /
  0.19 stops (1.44 at 1000; DJI Studio's Rec.709 view 0.61). HLG is left
  alone on purpose - it is display-relative, and its peak adaptation is the
  HLG display's own. Also in the importer dialog, the Properties panel,
  osvtool (`render --hdr-peak`, `lut --hdr-peak`), the user defaults file and
  the direct path (a change re-renders the Program monitor). CPU / CUDA /
  OpenCL agree at 115.8 / 112.2 dB. Engine ABI 5.
* **Keyframe Easing (WP-EASING).** DJI Studio's seven Keyframe Animation
  presets - None, Linear Smooth, Fast In / Slow Out, Slow In / Fast Out,
  Fast In / Fast Out, Slow In / Slow Out, Linear - as a new "Keyframe
  Easing" popup on Open 360 Reframe (id 22, appended). Between keyframes of
  Pan, Tilt, Roll and the selected lens's pair the effect draws the curve
  itself, identically on the CPU path, the GPU filter and the direct path
  (one shared `ReframeEasing.cpp`); Premiere's scripting APIs can only set a
  keyframe's interpolation type, never a curve. The names, order and speed
  profiles are DJI Studio's; the exact numbers could not be established, so
  each curve is the standard polynomial with DJI's shape (docs/PREMIERE.md).
  None is the default and renders byte for byte as before (no keyframe query
  is made at all).

* **Panel: Keyframe Animation, Manual Framing, Stabilisation and the
  Program Monitor controls.** A preset grid with curve icons drawn from the
  effect's own curves, applied to the selected clips or to every OSV clip of
  the sequence (clips without the effect are skipped and counted); DJI
  Studio's Manual Framing (Crystal Ball / Asteroid / Ultra Wide / Wide /
  Dewarp, a Zoom stepper along DJI Studio's zoom path, and live FOV /
  Correction / Pan / Tilt / Roll read-outs of the selected clip at the
  playhead, keyframed controls keyed there); DJI Studio's RockSteady and
  Horizon Leveling switches set on the master clips' OpenOSV Source
  Settings; and a card listing
  every Program Monitor gesture, open for a new user. On UXP every button
  press is one undo step; on CEP each value is its own History step, and the
  panel says so (docs/PANEL.md).

* **Lens Shading in Source Settings (WP-VIGNETTE).** The soft darker band
  that stayed on every sky seam crossing after the sky seam fix is a ring in
  the front lens's own image - 83-89 deg from its axis, up to 0.34 stop deep
  on the side facing the sun, and additive (R, G and B lose the same light:
  0.52 / 0.31 / 0.10 stop), so a lens-to-lens ratio could never remove it.
  "Lens Shading" (Auto / Off) measures each lens's rim structure from its own
  sky per bucket of eight frames and adds the missing light back before the
  lenses are blended; "Shading Strength" scales it. On the sample the band's
  dip below the sky trend falls 158 -> 21 millistops (open sky elsewhere
  scores 57-67), line x0.40 / band x0.32 / broad x0.94 / colour x0.86, the
  ground unchanged, the table stable to 0.4 % per frame; 7-10 ms of analysis
  per bucket, nothing measurable in the render kernels. Auto for new clips
  (and, like every control added to the Source Settings effect, for an
  effect saved before it existed); a prefs blob written before it reads as
  Off. Also in the importer dialog, osvtool
  (`--shading`, `--shading-strength`) and the user defaults file. Engine
  ABI 4.
* **Seam tools in Source Settings (WP-SEAMTOOLS).** Five sliders in the
  Stitching group tweak the carved seam; every default is the seam as it
  rendered before (bit-identical), and each changes only the overlap. Seam
  Blend / Parallax Blend set the feather where the lenses agree / disagree
  (1.5 / 0.35 deg; the seam's path never moves). Seam Smoothing is a real
  two-band blend (DJI's multiband): colour blends wide while detail still
  switches at the seam, from a per-frame low band built on the GPU for NVDEC
  frames and the direct path - at 1-2 deg the nacelle's seam edge falls 46-59 %
  (0.32 -> 0.17 / 0.13) with a sharp double image of 2.1-2.5 instead of
  Parallax Blend's 4.9-6.2 at the same edge, for +0.1-0.3 ms per frame on the
  GPU. Near / Far Offset shift content along the seam where the lenses
  disagree / agree (on the sample the carve's mask does not isolate the
  nacelle; see docs/PREMIERE.md). Also in the importer dialog, osvtool
  (`--seam-blend`, `--parallax-blend`, `--seam-smoothing`, `--near-offset`,
  `--far-offset`) and the user defaults file. Engine ABI 3.
* **Defaults for new clips (WP-DEFAULTS).** Set a clip up the way you like,
  open the Source Settings effect's "Defaults" group and click "Save as
  Default for New Clips" (or "Save as Default" in the Source Settings
  dialog): every clip imported afterwards starts with those settings -
  colour output and look, output size, stabilisation, seam / sky seam / sun
  ghost options, calibration, D-Log M curve, exposure, render device,
  Program Monitor Colour. Clips that already have settings keep them.
  "Restore Built-in Defaults" goes back. The settings live in
  `%APPDATA%\OpenOSV\defaults.json` (one named key per setting, written
  atomically; `OPENOSV_DEFAULTS_FILE` overrides the location), and
  `osvtool render --use-user-defaults` renders with them on request. A
  zero-filled prefs buffer is no longer adopted as the built-in defaults, so
  a new clip keeps the settings it started with.
* **Companion panel "OpenOSV" (WP-PANEL): Open 360 Reframe goes on every
  OSV clip you drop.** A small Premiere panel watches the timeline and
  applies the effect to every `.OSV` / `.LRF` clip added to a sequence, once,
  with the Lens (DJI or Classic) and Drag Sensitivity you set. It never
  retro-fits an existing edit, and an effect you remove stays removed.
  "Apply to selected clips" and "Apply to all OSV clips in this sequence"
  handle what was there already. It comes in two builds:
  * UXP for Premiere 25.6+, all official API.
  * CEP for Premiere 22-26, which uses the unofficial QE DOM only to add the
    effect and verifies every result through the official DOM.

  `scripts\install_plugins.ps1` installs whichever needs no clicks:
  * UXP through Adobe's UPIA when it is present;
  * otherwise CEP, with a per-user `PlayerDebugMode` that `-Uninstall`
    restores exactly.

  New switches: `-NoPanel`, `-PanelOnly`, `-PanelFlavor`, `-PanelDestination`.
  126 Node tests; ctest runs them when Node 18+ is found. See
  `docs/PANEL.md`.
* **Carved stitch seam (WP-SEAM).** Inside the overlap each lens now shows
  only on its own side of a seam carved where the lenses agree (dynamic
  programming over a closed longitude ring, stick mask and flare / rim costs
  steering it), with a 0.35-1.5 deg feather instead of the old 4 deg 50/50
  mix. The doubled wing fin is gone: ghost energy 76 -> 6 (x1000 luma), seam
  motion 0.0001 deg/frame. On whenever Seam search is on.
* **DJI's camera in the reframe effect (WP-CAMERA).** Camera Model "DJI" adds
  DJI Studio's FOV (vertical pinhole angle), Correction Angle (eye distance
  behind the sphere centre) and its read-out Zoom, matching DJI Studio and
  DJI's Premiere plug-in; the same numbers give the same framing. DJI
  preset values, a Drag Sensitivity control (default 2.0), and popups read
  correctly whether the host numbers them from 0 or 1.
* **Source Settings reach the Program monitor (WP-SETTINGS).** The engine
  keys clips by file identity (volume serial + file index), the newest
  importer instance's settings win, and every frame carries a settings
  generation. New per-clip "Program Monitor Colour": Sequence space (fast,
  the default) or Match Source monitor.
* **Calibration that tells the truth (WP-CALIB).** Auto follows the recorded
  accessory; the menu says which set each choice really uses; DJI's
  lens-protector field-angle correction (our own fit) is folded into the rig
  for protector clips and verified on frame 0. OK in the Source Settings
  dialog no longer resets hidden settings.
* **DJI Studio's Rec.709 look (WP-LOOK)** as the default Rec.709 rendering:
  a 46-constant fitted model (no DJI data shipped), dE2000 vs DJI 2.15 -> 0.50
  on the sample. "OpenOSV standard" stays selectable.
* **Sun ghost removal (WP-FLARE).** Internal-reflection ghosts are detected,
  fitted and subtracted in linear light before the blend; +23.6 % -> +0.2 %
  on the sample's pill ghost, zero pixels touched elsewhere. On for new
  clips; playback never waits for the fit.
* **Photometric seam field (WP-PHOTO).** Render-only seam edge inset, lens
  gain from trusted pixels only, a per-longitude usable rim and a 2-D log gain
  field per analysis bucket: the sky seam's light line x0.71 and colour step
  x0.34 on the default stitch.
* **HDR at 16/32 bits and the importer's frame on the GPU (WP-IMPORTER).**
  PQ/HLG clips are never handed to Premiere as 8-bit; the importer decodes on
  NVDEC, stitches from VRAM and packs on the GPU (park 83-116 -> 46-64 ms).
* **Warm reopen (WP-REOPEN).** Shared hardware devices, deferred first frame,
  parallel lens opens, and parked readers / NVDEC decoders: a reopen or
  unquiet of a seen clip costs ~0-14 ms instead of 100-430 ms.
* **End-to-end direct-path test (WP-E2E)** through the mock host with the real
  .aex and .prm; the GPU parameter probe now reads Premiere 26.2's real
  parameter list (before, Source Roll read Source Pan's value and several
  controls were ignored on the GPU path).
* **Engine ABI version 2** for all of the above.
* **`video::GpuClipDecoder`: both lenses decoded by NVDEC straight into the
  VRAM of the caller's CUDA context, with a GOP-aware frame cache and
  decode-ahead** (work package A of docs/DIRECT_GPU.md).  A park that used to
  cost 50-60 ms (NVDEC plus a host copy) or ~950 ms (software) is now a
  0.001 ms cache hit inside any GOP already walked, and a cold park decodes
  from the sync sample at ~1.9 ms per frame pair (median 46-48 ms over 12
  scattered landings); forward playback runs at ~520 pairs/s.
  * Frames live in pooled, pitched P010 slots the decoder owns (both lenses
    in one 52.7 MiB allocation for 3000 x 3000); each NVDEC surface is copied
    device-to-device and handed straight back, so FFmpeg's surface pool is
    never pinned by the cache.  Capacity comes from a VRAM budget (default
    min(1.5 GiB, 20 % of free VRAM)), eviction is LRU and never touches a
    leased slot.
  * A random access keeps every frame decoded on the way from the sync
    sample; a decode-ahead worker takes over once the host plays forward and
    yields to any waiting foreground request within one frame pair.
  * `GpuFrameLease::releaseAfter(stream)` records an event on the caller's
    stream and the slot's next overwrite waits for it on the GPU, so a render
    thread never has to synchronise the host.
  * CUDA driver API only; the context is pushed and popped around every call
    and a private context is never created.  With no context supplied the
    primary context is retained without touching its flags.
  * `HevcStreamDecoder` can decode into a caller-supplied CUDA context and
    stream (`DecoderOptions::cudaContext` / `cudaStream`) and reports the GOP
    layout (`previousSyncIndex()`); without a context it behaves as before.
  * `osv_gpu_decode_bench` prints the timings; tests cover bit-exactness with
    the software decoder, the caller's context, LRU/leases/budget,
    stream-ordered release, decode-ahead, destruction mid-run, four
    concurrent threads and a zero-copy render.
* **`kDlogMOsmo360`, a D-Log M curve fitted to a genuine Osmo 360 reference,
  and it is now the default.** The previous default, `kDlogMDjiRefit`, was
  fitted before any Osmo 360 reference existed, against Pocket-3-era
  D-Log M -> HLG measurements; measured against DJI's own Osmo 360
  D-Log M -> Rec.709 LUT it is up to 0.30 stops off through the upper mids and
  0.66 stops too bright in the toe. Neutral-axis error against that reference
  (HLG code units, all 33 samples) drops from 0.0318 RMS / 0.0659 worst to
  0.0233 / 0.0462; above code 0.24, where the reference is not crushed 8-bit
  data, from 0.0259 to 0.0160 RMS.
  * 18 % grey stays pinned exactly (code 0.400 -> linear 0.180 -> HLG 0.380,
    BT.2408). Diffuse white moves from HLG 0.7548 to 0.7433 - 0.0067 below
    BT.2408's nominal 0.750, but DJI's own file reads 0.7404, so the new curve
    is *closer* to the camera manufacturer's placement.
  * `kDlogMDjiRefit` is kept verbatim and still selectable as `--fit dji`, and
    `"dji"` deliberately does **not** follow the default, so a project already
    graded against it renders unchanged. `DlogMFit`/`PrefsDlogmFit` are
    append-only (`Osmo360 = 2`) because the value is persisted in the
    preferences blob, so a project saved by an older build still deserialises
    to the curve that build rendered with.
  * The fit had to be done against a Rec.709 reference because DJI publishes no
    Osmo 360 HLG LUT. That is sound without any inversion: our Rec.709 output
    *is* the HLG signal in Rec.709 primaries, and on the neutral axis both
    primaries matrices are the identity, so the Rec.709 and HLG branches of
    `osvLinearToOutput` are the same function of the code. Inverting DJI's
    diagonal through that expression recovers a smooth, strictly monotonic
    scene-linear curve (3.74 at code 1.0, 18 % grey at code 0.406), confirming
    their 709 rendering is an HLG-in-709 rendering and not a separate tone map.
    **So the curve was what differed from DJI, not our output rendering, and
    only the curve was refitted.** The algebra is in docs/COLOR.md and the
    identity is asserted to 2e-6 in `tests/unit/test_color.cpp`.
  * 0.0160 RMS is the ceiling of this seven-parameter family for this data, not
    a solver failure: relaxing the slope-ratio bound from 3 to unbounded
    (ratio 40.8) changes the RMS above code 0.24 by 0.00002, so the bound is
    kept for the shadow-gradient and .cube-interpolation reasons it was
    introduced for.
* **`scripts/fit_dlogm.py --from-cube <path> --cube-transfer 709|hlg`**, which
  measures a .cube file's neutral diagonal at every one of its grid points
  instead of a hand-copied subset. The built-in 64-point table path is
  unchanged and still reproduces `kDlogMDjiRefit`, because it is that curve's
  provenance record. The solver now multi-starts, since the residual surface
  has several local minima and the Pocket 3 start is only the best basin for
  Pocket-3-like data.
* **`luts/`: three 65^3 .cube files for NLEs that cannot load OpenOSV** -
  D-Log M to Rec.2100 PQ, to Rec.2100 HLG and to Rec.709, each with a TITLE
  naming OpenOSV and the curve. Every byte is generated by our own
  `osvtool lut` from our own fitted curve; no DJI LUT data is redistributed
  (see NOTICE).
  * `scripts/gen_luts.ps1` regenerates all three reproducibly from the built
    osvtool, and `-Check` fails when the committed files are stale.
  * `tests/unit/test_cube.cpp` regenerates each one in-process and compares it
    byte for byte against the committed copy, so a curve change that is not
    followed by a re-run fails the build instead of silently shipping a table
    that no longer matches what the plug-in renders. A second test reads the
    BT.2408 anchors back out of the committed files, so a consistently
    regenerated *wrong* curve is caught too.
  * `scripts/install_plugins.ps1` installs them into
    `...\MediaCore\OpenOSV\LUTs\`, alongside the modules rather than in one of
    Premiere's per-user Lumetri folders, so one copy serves all three hosts and
    `-Uninstall` removes them with everything else.
* **One log line per clip at open stating the colour decisions**, so an
  unexpected preview is answerable from a support log without reproducing it:
  the detected source colour mode, whether it came from metadata or from the
  luma-histogram fallback, the input encoding chosen and the output space
  declared to Premiere.
* **`OpenOSVSourceSettings.aex`, a master clip Source Settings effect.** The
  stitch options are now simply visible in the Effect Controls panel instead
  of hiding behind a modal dialog nobody knows to look for. Premiere attaches
  it to the master clip itself, and it exposes all nine `PrefsBlob` fields:
  colour output, output size, stabilisation, seam search, exposure match,
  calibration slot, D-Log M curve, exposure and render device.
  * `PF_Cmd_GLOBAL_SETUP` calls
    `PF_SourceSettingsSuite::SetIsSourceSettingsEffect`, which is the single
    call that makes Premiere treat the module as master-clip settings rather
    than as a video filter to drag onto a clip. The suite is acquired at v2
    with a v1 fallback, and a host without it is a logged degradation rather
    than a failed setup - a panel minus the automatic attachment beats no
    panel.
  * `PF_Cmd_SEQUENCE_SETUP` calls `PerformSourceSettingsCommand`, which the
    host routes to the importer's new `imPerformSourceSettingsCommand`
    (selector 66). The two halves exchange a `PrefsBlob`, so the panel opens
    showing what the clip is *actually* being decoded with ("as shot") rather
    than snapping every control back to the global default after a project
    reopen. Only the controls whose value really moved are flagged
    `PF_ChangeFlag_CHANGED_VALUE`, because on a master clip effect a spurious
    change means an unnecessary media refresh and a re-stitch of the clip.
  * `PF_Cmd_TRANSLATE_PARAMS_TO_PREFS` writes the 128-byte blob. A buffer
    smaller than the blob is refused outright and logged rather than
    truncated - writing 128 bytes into a smaller buffer is a heap overflow in
    the *host's* allocator - and a larger buffer's tail is left untouched.
  * **Every parameter is `PF_ParamFlag_CANNOT_TIME_VARY`, so the panel shows
    no stopwatch.** That is a correctness requirement, not a style choice: a
    source settings effect is never sent `PF_Cmd_RENDER`, and its values
    reach the importer only as one flat blob with no time axis, so a keyframe
    has nowhere to be stored or read back from. Pan / Tilt / Roll / FOV
    therefore stay in `Open360Reframe.aex`, which is an ordinary timeline
    effect that does receive `PF_Cmd_RENDER` at a time. It is a separate
    module for the same reason the AE SDK gives: "multiple PiPLs in a single
    plug-in" is not supported in Premiere.
  * The match name (`OpenOSV.SourceSettings`) is the entire binding between
    importer and effect - Premiere compares
    `imFileInfoRec8::sourceSettingsMatchName` to the PiPL's match name with
    no handshake and no diagnostic on a mismatch. It therefore lives exactly
    once, in `plugins/common/SourceSettingsIdentity.h`, read by the effect's
    `.r`, the effect's `.cpp` and the importer; tests compare the resource
    read out of the built module and the string read out of a live
    `imGetInfo8` against that one constant.
  * The importer sets `hasSourceSettingsEffect = kPrTrue` and keeps
    `hasSetup = kPrTrue`: the modal dialog still works, because right-click >
    Source Settings is muscle memory and is the only route left on a machine
    where the `.aex` failed to install. Both write the same blob.
  * `osv_source_settings_tests`: 40 cases / 2684 assertions against the built
    `.aex`, plus 4 new importer cases for selector 66 and the match name.
* **`presets/`: three Premiere sequence presets**, so a correct 59.94 fps
  timeline is one click instead of a hand-typed frame size.
  `OpenOSV 2560x1440 59.94` (16:9 delivery, pairs with the importer's default
  2560 x 1280 output), `OpenOSV 3840x2160 59.94` (4K delivery) and
  `OpenOSV 360 equirect 2560x1280 59.94` (the sphere itself / VR export, and
  the only one that declares monoscopic equirectangular VR). They answer the
  "why is my new sequence 2:1?" question: Premiere copies a new sequence's
  frame size from the clip, an equirect sphere is necessarily 2:1, and no
  importer field can ask for a differently shaped sequence.
  * The `.sqpreset` schema is undocumented, so it was read off the presets the
    installed application ships and nothing is invented - the Premiere 2026
    `Version="9"` body from `HD 1080p 59.94 fps` / `UHD (4K) 2160p 59.94 fps`,
    and the `ImmersiveVideoVRConfiguration` payload from
    `Legacy\VR\Monoscopic 29.97\3840x1920`. `presets/README.md` records the
    provenance field by field.
  * `VideoFrameRate` is a frame DURATION in ticks, so 59.94 fps is exactly
    `254016000000 * 1001 / 60000 = 4237833600` - the same integer the
    importer reports and `tests/premiere/common` already pins. A sequence
    typed as "60" is 4233600000 and drifts a frame every thousand.
  * `VideoUseMaxBitDepth` is `true`, unlike Adobe's stock presets, because
    the importer hands Premiere 32-bit float frames and an 8-bit sequence
    would quantise the sphere before the reframe resamples it.
* **`scripts/install_plugins.ps1` installs the presets too**, into
  `Documents\Adobe\Premiere Pro\<ver>\Profile-<user>\Settings\SequencePresets\OpenOSV\`,
  where they appear as a group called OpenOSV under File > New > Sequence.
  `-NoPresets` skips them and `-PresetDestination` overrides the location.
  The path was derived from the installed application (its own files and
  the `Settings\` subfolders Premiere itself creates), not guessed; the script discovers the version and
  `Profile-*` folders rather than assembling them from `$env:USERNAME`, and
  when it finds no settings root it says so and skips instead of inventing a
  path. The presets are installed by the UNELEVATED parent process, because a
  per-user path resolved inside an elevated session belongs to whichever
  account answered the UAC prompt.
* **D-Log M passthrough as a fourth colour output** (`PrefsColorOutput::DLogM`,
  appended as value 3 so saved projects keep their settings), in both the new
  Source Settings effect and the modal dialog. It maps to
  `color::OutputTransfer::Passthrough`, which the library already had and the
  importer never wired up, and it bypasses both the transfer curve and the
  primaries matrix - so the frame arrives in the camera's own D-Log M encoding
  and gamut, ready to be graded once with a LUT or Lumetri instead of being
  converted twice.
  * There is no DJI D-Log M token in `PrSDKColorSpaces.h` (it has Sony S-Log
    spaces and nothing for DJI), so the declaration is necessarily an
    approximation. `imGetIndColorSpace` returns `kPrOverranged2020Scene` -
    full range, RGB, 32f and scene-referred are all exact, and only the
    BT.2020 primaries are approximate (the widest standard gamut, so nothing
    is clipped). It deliberately does NOT claim `kPrOverranged709`, which is
    the one actively harmful answer: the host would treat the flat log curve
    as a finished Rec.709 image and a "Match Source" export would bake that
    in. The choice is logged once with its reasoning so it is not invisible.
  * The modal dialog's colour control became a combo box; a fourth radio
    button would not have fitted the old three-across layout, and every other
    multi-choice setting there was already a combo. Every `fillCombo` count is
    now `std::size(...)` and each list's length is `static_assert`ed against
    its enum's `Count` - a literal `3` against a four-entry list had already
    made the new default output size unreachable in the UI.
* **Interactive Program Monitor overlay for "Open 360 Reframe".** The user
  reframes by dragging the picture and Premiere records the keyframes.
  * Drag open picture to pan and tilt; the world follows the cursor, at a
    rate of `fov / viewportWidth` degrees per pixel so the gesture feels
    identical at 30 and at 150 degrees of field of view.
  * `Shift` constrains to the dominant axis, locked once the drag passes 3 px
    so it cannot swap axes under the hand.
  * A roll ring near the frame edge and four corner FOV grips, with
    `Ctrl` + drag (zoom) and `Alt` + drag (roll) as shortcuts from anywhere.
    A modifier pressed mid-drag retargets the same gesture immediately.
  * Values are committed by writing the `PF_ParamDef` and setting
    `PF_ChangeFlag_CHANGED_VALUE` plus `PF_EO_HANDLED_EVENT`, the documented
    route for a value change from a `PF_Cmd_EVENT`. The host commits at the
    current time, which is what makes the stopwatch record a keyframe.
    `PF_UpdateParamUI` is explicitly NOT used: the SDK documents it as
    cosmetic only and it cannot set a value at all.
  * Drawn with DrawBot: a gapped centre crosshair, the two ring arcs, four
    "L" corner grips and a Pan/Tilt/Roll/FOV readout, every stroke drawn
    twice (a dark 1 px pass under a light one) so it stays legible over both
    bright and dark footage. Nothing is ever filled.
  * `PF_Event_ADJUST_CURSOR` sets the pan / rotate / resize cursor from the
    same hit-test and highlights the hovered handle on the next repaint.
  * Every failure degrades to "no overlay, log once": a missing suite, a null
    drawing reference, a failed pen or path allocation, a degenerate
    viewport. Nothing crashes and nothing blocks rendering.
  * **No scroll-wheel zoom**: the AE SDK has no mouse-wheel event
    (`AE_EffectUI.h:103-117`), and the only wheel field in the headers
    belongs to a stylus struct unreachable from `PF_EventUnion`. `Ctrl` +
    drag is the substitute. See `docs/PREMIERE.md`, "Program Monitor
    overlay".
* `PF_OutFlag_CUSTOM_UI` is now set in both `PF_Cmd_GLOBAL_SETUP` and the
  PiPL (`OSV_REFRAME_OUT_FLAGS` is now `0x06008000`), and `PF_Cmd_PARAMS_SETUP`
  registers a `PF_CustomEFlag_COMP` custom UI.
* The mock host gained a recording custom-UI / DrawBot surface
  (`tests/premiere/mockhost/MockDrawbot.cpp`): the DrawBot suites write down
  every path, stroke, string and object lifetime instead of rendering, so a
  test can assert "a crosshair and a roll ring were drawn", that nothing was
  filled, and that no DrawBot object leaked. `register_ui` now records the
  `PF_CustomUIInfo` the effect asked for.

### Changed

* **Open 360 Reframe: a "Lens" dropdown, DJI by default, and one FOV on
  screen (WP-LENSUI).** "DJI | Classic" replaces the Camera Model checkbox
  and decides what the Effect Controls panel shows: DJI shows Zoom, FOV and
  Correction Angle; Classic shows FOV and Distortion. The two lenses measure
  FOV differently (DJI's is the vertical pinhole angle, Classic's the visible
  angle across the width), so they are no longer shown side by side.
  Switching carries the framing across. Projects saved before this open on
  DJI; the old checkbox stays hidden in the list so they still load.
* **The default D-Log M curve is now `kDlogMOsmo360`** (`--fit osmo360`,
  "Osmo 360" in Source Settings). Existing projects keep their stored curve;
  only new clips pick up the new default. See the Added entry for the
  residuals and for how to pin the old rendering.
* **The source colour mode -> input encoding rule now lives in one place**
  (`color::inputEncodingForColorMode`). It was written out three times - in the
  importer, in `osvtool`'s `--input-encoding auto` and in the auto-detect
  fallback - which is how the two front ends could have come to disagree about
  what a clip *is*. Behaviour is unchanged for the three modes the camera
  writes; the modes with no curve of their own (D-Cinelike, Vivid, D-Log,
  D-Log2) and an out-of-range value now explicitly resolve to D-Log M instead
  of relying on a `default:` label in each copy.
* The importer's default output size is now 2560 x 1280 instead of the native
  6000 x 3000, so a sequence created from an .OSV opens at an editable size.
  Source Settings still offers Native, 4K and 2K. An equirect sphere is always
  2:1; the 16:9 delivery crop is the reframe effect's Output Aspect.

### Fixed

* Sanitising a corrupt preferences blob fell back to enum value 0 (Native),
  so a damaged project silently jumped to the full-resolution sphere. It now
  falls back to the documented default.


* `ThreadPool::parallelFor` could let a worker keep running chunks after the
  owning call had returned. The `Job` lives on the caller's stack, so the next
  caller's job reused the address and the straggler executed chunk bounds from
  the wrong job. In the reframe effect this wrote row 230 of a 120-row frame,
  an out-of-bounds store that crashed about one test run in four. The pool now
  serialises job submission and waits for every worker to leave `runChunks()`
  before returning.
* `Open360Reframe.aex` imported `spdlog.dll` and `fmt.dll` directly instead of
  delay-loading them, so Windows could bind them to Adobe's copies in the
  application directory (which is searched before the plug-in's own folder).


### Added
- Milestone 1: core library (`osv::core`, `container`, `meta`, `geom`,
  `color`, `video`, `render`, `io`), `osvtool` CLI and the Catch2 test-suite.
- CPU reference renderer plus CUDA and OpenCL backends sharing one kernel.
- D-Log M to Rec.2100 PQ / HLG / Rec.709 colour pipeline and `.cube` export.
- Eye-offset projection (`Projection::EyeOffset`, `OSV_PROJ_EYE_OFFSET`,
  `osvtool render --proj eye-offset --distortion d`); the presets now use it
  (Crystal Ball / Asteroid `d = 1`, Wide `0.15`, Ultra Wide `0.4`,
  Dewarping `0`).
- `osvReframeEquirectPixel`: kernel entry point that reframes a stitched
  equirect RGBA frame (32f / 16f, RGBA / BGRA) for the Premiere effect, with
  a CUDA launcher and CPU / CUDA parity tests.
- Milestone 2 groundwork: `plugins/common` (`osv_premiere_common` - host
  suites, plug-in log, delay-load hook, pixel conversions, host context,
  prefs blob), the Adobe SDK build support in `cmake/OsvPremiereSdk.cmake`
  (`osv_add_premiere_plugin`, `osv_add_pipl`, `osv_add_delayload`) and the
  `windows-msvc-premiere-*` presets.
- `tests/premiere`: `osv_premiere_mockhost`, a fake Premiere Pro host serving
  our own implementations of the PPix, PPix Creator (1 and 2), PPix Cache,
  Time, String, App Info, Error, Color Management, Memory Manager, Importer
  File Manager, Sequence Info, Video Segment, GPU Device and the AE-side PF
  suites; plus `osv_premiere_common_tests` covering it and `plugins/common`.
- `scripts/install_plugins.ps1`: installs or removes the plug-ins in
  `MediaCore\OpenOSV`, self-elevating, and points at Premiere's plug-in
  loading log.
- `Open360Reframe.aex`: the "Open 360 Reframe" effect (`plugins/reframe`).
  One module with two entry points - `EffectMain` (After Effects API: the
  Effects-panel entry, 13 host-keyframed parameters with a supervised preset
  popup, and a multi-threaded 32-bit float CPU render) and `xGPUFilterEntry`
  (Premiere's `PrGPUFilter`: the same kernel on the host's own CUDA context
  through the driver API and an embedded fatbin). Both paths call
  `osvReframeEquirectPixel`, so they agree to 151 dB PSNR in 32f and 114 dB
  in 16f. Identity, out-flags, parameter ids and the aspect / preset tables
  live once in `ReframeParams.h`, which the PiPL resource includes.
- `tests/premiere/reframe`: `osv_reframe_tests`, 72 cases that load the built
  `.aex` with `LoadLibraryW` and drive it through the mock host, including
  reading the PiPL resource back out of the module and comparing it to the
  constants that generated it.
- `OpenOSVImporter.prm`: the standard file importer for `.OSV` / `.LRF`
  (`plugins/importer`). Registers the `'OSV_'` file type, decodes both lens
  streams, stitches them with the CUDA / OpenCL / CPU renderer and hands
  Premiere an equirectangular 360 x 180 frame in `BGRA_4444_32f` or `_8u`,
  colour-tagged Rec.2100 PQ, HLG or Rec.709. Declares the clip as monoscopic
  equirectangular VR, reports the frame period as the exact container
  rational in ticks, decodes the AAC track for both random access and the
  sequential conform, and exposes the per-clip stitch / colour options
  through a modal "OpenOSV Source Settings" dialog. Every frame follows the
  same library calls `osvtool render --mode equirect` makes, so a Premiere
  frame and an osvtool frame of the same clip at the same settings are the
  same pixels.
- `tests/premiere/importer`: `osv_importer_tests`, 38 cases that load the
  built `.prm` with `LoadLibraryW` and drive `xImportEntry` through the mock
  host - registration, lifetime and leak checks, `imGetInfo8`/`9`, format and
  size negotiation, colour-space declaration and the Rec.709 connection-space
  fallback, frame content and orientation, the host PPix cache, audio
  (including that random and sequential reads of the same range are
  bit-identical), the prefs protocol and the pure prefs <-> dialog mapping.
- `cmake/OsvCheckDelayLoad.cmake`: a post-build audit that runs
  `dumpbin /DEPENDENTS` on every built plug-in and fails the build if the
  module imports anything outside the OS and the CRT directly instead of
  delay-loading it, so a new dependency cannot silently start binding to the
  FFmpeg that Premiere ships in its own application directory.
- Milestone 2 is integrated: one configure of the
  `windows-msvc-premiere-release` preset builds the library, both plug-ins
  and all four test executables. `ctest --preset premiere` runs 307 tests
  (`osv_tests` 167, `osv_reframe_tests` 72, `osv_importer_tests` 38,
  `osv_premiere_common_tests` 30) and all pass, with the `[cuda]` cases
  executing on the GPU rather than skipping. `docs/PREMIERE.md` gained a
  "Verification results" section recording the counts, the GPU/CPU PSNR,
  the frame timings, the stage-directory contents with the `dumpbin`
  evidence, and the install result.

### Changed
- `cmake/EmbedKernel.cmake` now embeds arbitrary BYTES, not just text: the
  array is `unsigned char` and a `<symbol>_text` alias serves text callers.
  A `char` array cannot hold a byte above 0x7F, which only mattered once a
  binary file (the reframe CUDA fatbin) was embedded.

### Fixed
- **Unbalanced parameter groups in the reframe effect.** `paramsSetup()`
  opened "Camera" and "Source" with `PF_ADD_TOPIC` but never closed either
  with `PF_END_TOPIC`, so every control after "Camera" nested inside it -
  including the whole "Source" group and "Smooth Keyframes" - and collapsing
  Camera would have hidden controls meant to be siblings. Both groups are now
  closed. Because `PF_END_TOPIC` issues its own `PF_ADD_PARAM`, a terminator
  is a real parameter occupying a real index in the middle of the list, so
  the effect no longer treats a parameter's index as equal to its permanent
  id: `ReframeParams.h` spells the index table out by hand (15 parameters,
  not 13) with `kParamIdByIndex` beside it, and `EffectMain.cpp`
  static_asserts the relationship. This keeps the GPU path's
  `GetParam(index - 1)` reading the same control the CPU path reads. A new
  test walks the real parameter list keeping a nesting depth and checks both
  that it balances and that the top-level controls really are at depth 0.
- **Use-after-free of the shared `ThreadPool`.** The effect's `PF_Cmd_RENDER`
  and the importer's frame copy both cached a raw `ThreadPool*` borrowed from
  the `HostContext` singleton. The effect declares
  `PF_OutFlag2_SUPPORTS_THREADED_RENDERING`, so a `GLOBAL_SETDOWN` (or the
  importer's `imShutdown`) on the main thread could delete the context - and
  join the pool's workers - while a render was still inside it. The
  importer's `HostContext::exists()` guard was a TOCTOU window, not a fix,
  since it releases its lock before the pool is ever touched. Both now hold a
  `threadPoolShared()` lease for the duration of the work, which is exactly
  what that accessor exists for.
- **Unsynchronised suite acquisition in the importer.** `ensureSuites()` ran
  on every selector from every host thread as an unguarded check-then-act on
  process-wide state. Two threads could both enter, and `ImporterSuites::
  acquire()` begins with `release()`, so the second would drop the host's
  refcounts on suite pointers the first had already published to an in-flight
  render. `ImporterGlobals` now carries a mutex, `suitesAcquired` is a
  release/acquire atomic that publishes the completed table, and
  `imShutdown` releases under the same lock.
- **Data race on `ImporterInstance::m_reader`.** `nativeGeometry()`,
  `geometryFor()` and `extraMemoryUsage()` read the reader with no lock while
  `imQuietFile` reset it under one, so `imGetInfo8` or
  `imGetPreferredFrameSize` on the UI thread could call `isOpen()` on a
  `DualStreamReader` mid-destruction. All three now take `m_mutex`, with
  `*Locked` variants for the callers that already hold it.
  `m_videoRequests` and `m_importerId`, which selectors touch before taking
  the lock, are now atomics.
- **32-bit overflow in `imFileInfoRec8::vidDuration`.** `frameCount() *
  rateDenominator()` was evaluated in 32-bit unsigned and could wrap to a
  negative duration before the cast. It is now computed in 64 bits and
  saturates with a log line.
- **The reframe effect refused an input format it advertises.** GLOBAL_SETUP
  registers `PrPixelFormat_BGRA_4444_8u`, but `PF_Cmd_RENDER` returned
  `PF_Err_BAD_CALLBACK_PARAM` for an 8-bit input world, relying on an
  undocumented host retry; a host that took the registration at its word got
  a hard error on every frame. An 8u input is now promoted once into a float
  scratch buffer (`promoteBgra8uToFloat`, codes / 255 - the exact inverse of
  the 8-bit store path) and rendered from that. Two new tests build a real 8u
  input world: one checks the render is accepted and produces a picture, the
  other that it matches a float-input render to within one 8-bit step.
  `docs/PREMIERE.md`, which already described this behaviour, is now true.
- **Delay-load hook touched objects that outlive their destructors.** The
  hook can fire at any time a delay-loaded import is first touched, including
  during teardown, but its state lived in namespace-scope objects with
  non-trivial destructors (a `std::mutex`, a `std::wstring`) that the CRT
  destroys at `DLL_PROCESS_DETACH`. It also allocated under the loader lock,
  a documented deadlock risk. The state moved into a deliberately-leaked
  function-local static (the pattern `PluginLog` already uses), the module
  directory is cached in a fixed buffer published with a release store
  instead of a `std::wstring` built under `std::call_once`, and the hook's
  path now builds its full path in a stack buffer, allocating nothing.
- Host suites with several versions whose older structs are prefixes of the
  newer ones (PPix2, PPixCreator2, Error, Memory Manager) are acquired with a
  fallback list instead of one hard-coded version, so an older host still
  gets a working importer. `CreateColorManagedPPix`, a PPixCreator2 v4
  addition, is now gated on the acquired version rather than on a null test
  that would read past the end of a shorter struct.
- The effect's preset table claimed to mirror `osv::geom::kPresets` with
  nothing enforcing it. `EffectMain.cpp` now static_asserts all five shared
  presets and the table sizes, so retuning the library's presets breaks the
  build instead of silently desynchronising the effect from `osvtool`.
- Removed `ImporterInstance::detachFileHandle()`, which had no caller and was
  a lifetime trap: it cleared `m_fileHandle`, so `releaseHeavy()`'s
  `CloseHandle` became a no-op and a caller that dropped the returned value
  would leak a kernel handle per clip open.
- The Smooth Keyframes test could not fail: the mock host returned the same
  value at every time, so `(v+v+v)/3 == v` passed for a sum, a median or code
  that ignored its neighbours. The mock now supports per-time keyframes and
  the test averages three *different* angles, checks the result against an
  independent render at their mean, and proves a centre-only render differs.
  A second case covers the clip-boundary fallback at `current_time` 0.
- The importer's orientation test rested on "top band brighter than bottom".
  It now also pins the measured magnitudes against the osvtool ground truth
  and adds a longitude check - column asymmetry plus seam continuity across
  the +-180 wrap - which the brightness proxy could not see.
- `docs/PREMIERE.md` said the effect uses "Sequence Info Suite v5" where the
  code prefers v9 and falls back 9 -> 8 -> 7 -> 6 -> 5.
- `docs/PREMIERE.md` contained three raw NUL bytes, written literally to
  show the importer's NUL-separated extension list. They made the file
  binary, so `grep`, `diff` and several editors treated it as unreadable
  data. It now uses the same escaped spelling the source code uses
  (`"osv\0lrf\0\0"`), and the file is plain ASCII again.
- `PluginLog` opened its file with no sharing at all (`_wfopen_s`), so nothing
  could read the log while a plug-in was loaded - not a user tailing it and
  not a second host process. It now uses `_wfsopen(..., _SH_DENYWR)`.
- `osv_add_premiere_plugin` now turns vcpkg's `VCPKG_APPLOCAL_DEPS` off for
  the module it creates. vcpkg appends its deployment step to the LINK
  command as a nested `cmd /C "..."`, which swallowed our own POST_BUILD step
  and made the link fail with `'L:' is not recognized as an internal or
  external command`. Our step stages every DLL the module imports anyway.
- The runtime-DLL staging step joined its list arguments with `|`, a cmd.exe
  metacharacter: inside that nested quoted command it broke the quoting and
  produced the same failure. The separator is now `?`, which is illegal in a
  Windows path and means nothing to cmd.
- `osv::log` registered its logger in spdlog's GLOBAL registry
  (`spdlog::stderr_color_mt("osv")`). spdlog lives in its own DLL, so that
  registry is shared by every module in the process and the call throws
  `"logger with name 'osv' already exists"` the second time a module that
  statically links this library initialises - which is exactly what Premiere
  does when it loads both the importer and the effect, and what happens
  again when a host unloads and reloads a plug-in (spdlog.dll stays
  resident). Each module now owns a private, unregistered logger.
