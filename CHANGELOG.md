# Changelog

All notable changes to OpenOSV are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

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
