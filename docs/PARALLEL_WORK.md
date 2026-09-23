# Parallel work harness

**Status (2026-09-23): complete.** Every package is merged into main in the
order below, each merge followed by a full build and ctest (759 tests at the
end), then a continuity pass (engine ABI version 2, PrefsBlob byte ranges
folded, docs). The contract stays here as the record, and as the template for
the next parallel round.

Ten agents work on OpenOSV at the same time, each in its own git worktree on
its own branch. This file is the contract that keeps them from stepping on
each other. Read it before touching anything; if your work needs something
this file does not allow, say so in your final report instead of doing it.

## Ground rules

1. **Your branch only.** Commit to the branch of your worktree. Never push,
   never commit in the main checkout (`L:\Dev\premiere_360_reframe`), never
   touch another agent's worktree. The lead merges every branch at the end.
2. **Never launch Premiere Pro. Never run `scripts/install_plugins.ps1`.**
   Nothing may be installed into `C:\Program Files\Adobe`.
3. **Own your files.** Edit the files your package owns (table below). A file
   marked SHARED may be edited only inside your package's marked region, or by
   appending, as the shared-file protocol below says. Anything else: read, do
   not write.
4. **Keep everything green.** The full ctest suite must pass on your branch
   before your final commit. Do not weaken or delete an existing test to make
   your change pass - if an existing expectation is genuinely obsolete because
   of your change, update it and say so in the commit message and the report.
5. **Code rules (the user's):** heavy Doxygen, a comment every few lines,
   defensive checks on every input, Result/Status error handling, no
   exceptions across C boundaries, no TODOs, no stubs, no fake data, never
   mention AI in comments or docs, no persona text in code. Match the
   surrounding style. Commit messages explain WHY, with measured numbers.
6. **Legal:** studying DJI's publicly distributed software to understand its
   behaviour, for interoperability, is allowed (the user's position); copying
   DJI code, data files or model weights into the product is not. Adobe SDK headers are never committed. `ref/` is
   git-ignored and stays that way. Any third-party model or code must be
   Apache/MIT/BSD (commercial-friendly) and recorded in NOTICE.

## Build recipe (isolated, does not rebuild FFmpeg)

Ten parallel vcpkg builds of FFmpeg would take hours and fight over
`C:\vcpkg`. Every worktree reuses the main checkout's installed packages:

```
robocopy L:\Dev\premiere_360_reframe\vcpkg_installed vcpkg_installed /MIR /NFL /NDL /NJH /NJS /NP
cmd /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat\" >nul && set VCPKG_ROOT=C:\vcpkg && cmake --preset windows-msvc-premiere-release -DVCPKG_MANIFEST_INSTALL=OFF -DOSV_PREMIERE_SDK_DIR=\"L:/Dev/premiere_360_reframe/Premiere Pro 26.0 C++ SDK\" -DOSV_AE_SDK_DIR=L:/Dev/premiere_360_reframe/third_party/ae-sdk"
cmd /c "call ...vcvars64.bat >nul && cmake --build --preset windows-msvc-premiere-release -j 4"
ctest --test-dir build/windows-msvc-premiere-release -j 4
```

* `-j 4`: ten agents share 32 cores. Benchmarks taken while the others build
  are noisy - say so next to any number you report, and prefer ratios measured
  back to back in one process over absolute times.
* The sample clip is git-ignored: `set OSV_SAMPLE_FILE=L:/Dev/premiere_360_reframe/example_footage_dlogm.OSV`
  (65 frames, dual 3000x3000 10-bit HEVC, D-Log M). The neural flow model
  lives in the main checkout's `models/` (git-ignored) if you need it.
* Tests never write to the user's real plug-in logs: any new test main calls
  `osv::premiere::testsupport::isolatePluginLogs()` first.

## Packages and file ownership

| Package | Owns (may edit freely) |
|---|---|
| **WP-SEAM** - DP seam carving + seam-guided narrow 2-band blend + temporal seam coherence (doubled fin / fringing at the seam) | new `src/osv/render/SeamCarve*`, `include/osv/render/SeamCarve.h`, new CUDA files for it under `src/osv/render/cuda/`, `osv_kernel.h` **[WP-SEAM] region**, the renderers' plumbing for a new blend-seam table (`CpuRenderer.cpp`, `cuda/CudaRenderer.cpp`, `cuda/CudaKernel.cu`, `opencl/*`), `RenderJob.h` / `RenderParamsBuilder.*` (append), `ImporterInstance::applyAnalyses` (seam hook only), `OsvEngineAbi.h` + `Engine.cpp` **[WP-SEAM] fields**, `plugins/reframe/DirectRender.*` / `DirectLaunch.*` / `ReframeKernel.cu` (pass-through of the table) |
| **WP-CAMERA** - DJI-identical camera: Zoom + FOV + Correction angle (match DJI Studio's behaviour), drag-sensitivity parameter, DJI preset values | `plugins/reframe/ReframeParams.h`, `EffectMain.cpp` (PARAMS_SETUP / readSettings / USER_CHANGED_PARAM), `ReframeCpu.*` (`buildView`), `ReframeUi.*`, `ReframeUiEvent.cpp`, `osv_kernel.h` **[WP-CAMERA] region** (view ray / projection), `include/osv/geom/VirtualCamera.h`, `src/osv/geom/VirtualCamera.cpp`, `Presets.*`, `GpuFilter.cpp` **readSettings / parameter probe only** |
| **WP-SETTINGS** - Source Settings reach the Program monitor on the direct path (exposure, fit, calibration...), and Premiere re-renders when they change; colour-space semantics option | `plugins/importer/Engine.cpp` **[WP-SETTINGS] region**, `OsvEngineAbi.h` **[WP-SETTINGS] fields**, `plugins/reframe/DirectPath.*` (invalidation / generation), `ImporterInstance::applyPrefsLocked`, Source Settings effect module (`plugins/sourcesettings/*`) if the fix lives there |
| **WP-CALIB** - lens guard / ND filter / underwater calibration: which sets the file holds, which is used, auto-select from the recorded accessory, UI that tells the truth | `src/osv/meta/CalibrationSelector.*`, `FormatDetector.*` (accessory / lens mode), `ImporterInstance::rebuildRig`, `plugins/importer/PrefsMapping.cpp`, `SourceSettingsDialog.cpp`, `PrefsBlob.h` (**append a field only**; never reorder), `osvtool probe` output |
| **WP-LOOK** - match DJI Studio's colour/tone (Rec.709 and HDR looks), highlight roll-off so sun ghosts sit as subtly as DJI's | `src/osv/color/*`, `include/osv/color/*` (ColorMath.h is SHARED by the kernel: keep it dialect-clean, append functions, never change existing ones' results without a documented reason), `scripts/fit_*.py`, colour tests |
| **WP-FLARE** - lens flare / sun ghost reduction: dual-lens ghost detection in the overlap, veiling-glare estimate, optional removal pass | new `src/osv/render/Flare*`, `include/osv/render/Flare*.h`, new CUDA files for it, a **cost hook** WP-SEAM can call (define a pure function `flareCost(...)`; do not edit SeamCarve) |
| **WP-IMPORTER** - the importer's own frame: cheap when the direct path serves the clip, high bit depth (no 8-bit PQ), fewer copies | `plugins/importer/ImporterVideo.cpp`, `plugins/common/PixelCopy.*`, `ImporterInstance::renderFrame`, `Engine.cpp` **[WP-IMPORTER] region** (a "direct path active" signal) |
| **WP-REOPEN** - decoder open / reopen cost (~200 ms per quiet and per Source Settings change): container-sample feeding, shared hardware device, keep what survives a quiet | `src/osv/video/*`, `include/osv/video/*` (except `GpuClipDecoder.*`, read-only), `ImporterInstance::ensureReader` / `releaseHeavy` / `readPair` |
| **WP-E2E** - end-to-end test of the effect's direct path through the mock host (segment-graph node walk to the media path, engine module loaded in the same process), fix the false "parameter types do not match" probe error, clip-time -> media-time on trimmed / sped-up clips | `tests/premiere/mockhost/*`, `tests/premiere/reframe/*` (new files), `GpuFilter.cpp` **probeParams only** (coordinate: WP-CAMERA owns the parameter list itself) |
| **WP-NEURAL** - research: neural / learned stitching, especially the sky | `docs/research/*`, `research/neural/*` only (phase 1) - done, merged |
| **WP-PHOTO** - photometric seam field (docs/research/NEURAL_STITCHING.md section 8): render-only blend inset, trusted-pixel gain, per-longitude rim map + 2-D log-gain field | new `include/osv/render/PhotoSeam.h`, `src/osv/render/PhotoSeam.cpp`, `tests/unit/test_photoseam.cpp`, `SeamAnalysis.cpp` **estimateGain only**, `osv_kernel.h` **[WP-PHOTO] region** + a marked hook in the shade function, the renderers' plumbing for one new photo table (append beside WP-SEAM's), `OsvEngineAbi.h` + `Engine.cpp` **[WP-PHOTO] fields**, `ImporterInstance` new `m_renderBlend` + new `applyPhotoSeam()` + its one-line call, `osvtool render` blend flags |

### Shared-file protocol

* **`include/osv/render/osv_kernel.h`**: each package adds its fields to
  `OsvRenderParams` / `OsvReframeParams` inside a comment-delimited region
  named after the package, appended at the end of the struct, and its
  functions in a new region at the end of the file. Keep the block <= 4 KB
  (tests assert it) and keep every dialect (C99 / C++ / CUDA / OpenCL)
  compiling. Existing renders must not change unless your package's feature
  is enabled.
* **`plugins/common/OsvEngineAbi.h`**: append fields to the END of
  `OsvEngineFrame` inside your package's comment-delimited region. Do NOT
  bump `OSV_ENGINE_ABI_VERSION` - the lead bumps it once at merge.
* **`ImporterInstance.*`, `Engine.cpp`, `GpuFilter.cpp`**: touch only the
  functions / regions your row names; put anything new in new functions.
* **`PrefsBlob.h`**: append-only, from the reserved block, with a
  static_assert on the new offset; the blob is stored in project files.
  Several packages add prefs at once, so each owns a fixed byte range (the
  reserved block starts at offset 22). Put your fields at your range: pad the
  bytes before it with `std::uint8_t padBefore<Package>[n] = {};`, then shrink
  `reserved` so the struct stays 128 bytes. Zero means "default" in every new
  field, so old projects keep rendering. The lead folds the padding into the
  real fields at merge.

  | Package | Offsets |
  |---|---|
  | WP-CALIB | 22-23 |
  | WP-SETTINGS | 24-25 |
  | WP-SEAM | 26-27 |
  | WP-LOOK | 28-29 |
  | WP-FLARE | 30-31 |
  | WP-PHOTO | 32-37 |
  | WP-IMPORTER | 38-39 |
* **CMake**: add your new files to the existing lists; do not reorganise.

## Final report (every agent)

End with: branch name and commit list; what you changed and why; measured
results (with the noise caveat); test results; anything you could not do or
that needs another package or the user; any interface you added that another
package or the lead must wire up.

## Merge order (the lead)

WP-REOPEN, WP-IMPORTER, WP-CALIB, WP-SETTINGS, WP-LOOK, WP-CAMERA, WP-FLARE,
WP-SEAM, WP-PHOTO, WP-E2E (WP-NEURAL's research is already on main) - full build and full ctest after
each merge, then a continuity pass over the whole tree (ABI version, shared
structs, duplicated helpers, docs), then install for the user to test.
