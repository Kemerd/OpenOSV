# Changelog

All notable changes to OpenOSV are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

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
  125 Node tests; ctest runs them when Node 18+ is found. See
  `docs/PANEL.md`.
* **Carved stitch seam (WP-SEAM).** Inside the overlap each lens now shows
  only on its own side of a seam carved where the lenses agree (dynamic
  programming over a closed longitude ring, stick mask and flare / rim costs
  steering it), with a 0.35-1.5 deg feather instead of the old 4 deg 50/50
  mix. The doubled wing fin is gone: ghost energy 76 -> 6 (x1000 luma), seam
  motion 0.0001 deg/frame. On whenever Seam search is on.
* **DJI's camera in the reframe effect (WP-CAMERA).** Camera Model "DJI" adds
  DJI Studio's FOV (vertical pinhole angle), Correction Angle (eye distance
  behind the sphere centre) and its read-out Zoom, recovered from DJI Studio
  and DJI's Premiere plug-in; the same numbers give the same framing. DJI
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
  The path was derived from the installed application (the folder-name
  literals in `Mezzanine.dll` alongside the `Settings\` subfolders Premiere
  itself creates), not guessed; the script discovers the version and
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
