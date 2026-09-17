# Building OpenOSV

## Prerequisites (Windows)

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 | 17.14 (MSVC 14.44) or newer with the "Desktop development with C++" workload | The Ninja presets need the x64 developer environment; use `scripts\vsdev.cmd` or the Visual Studio generator preset |
| CMake | 3.28+ | Presets v6 |
| vcpkg | any recent checkout | Set `VCPKG_ROOT`; the manifest pins a baseline, so the checkout only needs to be at least that new |
| CUDA Toolkit | 12.8+ (12.9 recommended) | Optional. Required for the CUDA renderer and for RTX 50-series (`sm_120`) |
| OpenCL runtime | any ICD (NVIDIA, AMD, Intel) | Optional at run time; headers come from vcpkg |
| Python | 3.10+ with numpy | Only for regenerating golden data and fixtures |
| ffmpeg.exe | any build | Only for `osvtool render --out *.mp4` (piped, never linked) |

## Configure, build, test

From any prompt:

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
scripts\vsdev.cmd cmake --preset windows-msvc-cuda-release
scripts\vsdev.cmd cmake --build --preset windows-msvc-cuda-release
scripts\vsdev.cmd ctest --preset all
```

`scripts\vsdev.cmd <command>` runs the command inside the Visual Studio x64
developer environment (it looks for `VSDEVCMD`, then the 2022 edition folders).
`scripts\run_ci_local.ps1` chains the three steps exactly like GitHub Actions.

Presets:

| Preset | Generator | GPU backends |
|---|---|---|
| `windows-msvc-cuda-release` / `-debug` | Ninja | CUDA + OpenCL |
| `windows-msvc-premiere-release` / `-debug` | Ninja | CUDA + OpenCL, plus the Premiere Pro plug-ins |
| `windows-msvc-cpu-only` | Ninja | none |
| `windows-vs2022-cuda` | Visual Studio 17 2022 | CUDA + OpenCL (works from a plain prompt) |
| `ci-windows` | Ninja | CUDA + OpenCL, warnings as errors |

The first configure runs `vcpkg install` from the manifest. FFmpeg takes about
six minutes on a fast machine; a binary cache (`VCPKG_BINARY_SOURCES`) makes
later builds instant.

## Premiere Pro plug-ins

The two plug-ins (`OpenOSVImporter.prm`, `Open360Reframe.aex`) are built only
when `OSV_BUILD_PREMIERE=ON`. They need two Adobe SDKs, and Adobe's licence
forbids redistributing either, so the repository contains no SDK file and the
build refuses to guess where they are.

### Getting the SDKs

| SDK | Version | Where | What is used from it |
|---|---|---|---|
| Premiere Pro C++ SDK | 26.0 | <https://developer.adobe.com/console/servicesandapis/pr> | `Examples/Headers` |
| After Effects C++ SDK | 25.6 | <https://developer.adobe.com/console/servicesandapis/ae> | `Examples/Headers`, `Examples/Util`, `Examples/Resources` (`PiPLtool.exe`, `AE_General.r`) |

Download both with an Adobe ID and extract them anywhere. The
`windows-msvc-premiere-*` presets look for them at two git-ignored, repo-local
paths, which is the least surprising place to put them:

```
<repo>\Premiere Pro 26.0 C++ SDK\        Examples\Headers\PrSDKImport.h, ...
<repo>\third_party\ae-sdk\               Examples\Headers\AE_Effect.h, ...
```

A junction or symlink works just as well as a copy. Point the two cache
variables somewhere else if you keep the SDKs elsewhere:

```powershell
scripts\vsdev.cmd cmake --preset windows-msvc-premiere-release `
    "-DOSV_PREMIERE_SDK_DIR=D:\Adobe\Premiere Pro 26.0 C++ SDK" `
    "-DOSV_AE_SDK_DIR=D:\Adobe\AfterEffectsSDK"
```

`cmake/OsvPremiereSdk.cmake` checks for every file the build later needs
(`PrSDKImport.h`, `AE_Effect.h`, `PiPLtool.exe`, `AE_General.r`,
`AEFX_SuiteHelper.c`) and fails the configure with one clear message naming the
missing file and the download URL, rather than a compiler error deep inside a
plug-in source.

### Build and test

```powershell
scripts\vsdev.cmd cmake --preset windows-msvc-premiere-release
scripts\vsdev.cmd cmake --build --preset windows-msvc-premiere-release
scripts\vsdev.cmd ctest --preset premiere
```

The `premiere` test preset runs the whole suite: the milestone 1 library tests
(`osv_tests`), the mock-host tests (`osv_premiere_common_tests`) and the
plug-in tests (`osv_importer_tests`, and `osv_reframe_tests` once the effect
exists).

`osv_importer_tests` does not link the importer's objects: it loads the built
`OpenOSVImporter.prm` with `LoadLibraryW` and drives `xImportEntry` through the
mock host, so what it proves is the binary that goes into `MediaCore\OpenOSV` -
its exports, its resources, its delay-load table and its behaviour. Its
`[sample]`-tagged cases need the clip named by `OSV_SAMPLE_FILE` and report SKIP
without it.

Cache variables:

| Variable | Default | Meaning |
|---|---|---|
| `OSV_BUILD_PREMIERE` | `OFF` | Build `plugins/`; the presets set it |
| `OSV_PREMIERE_SDK_DIR` | preset | Premiere Pro C++ SDK root |
| `OSV_AE_SDK_DIR` | preset | After Effects C++ SDK root |
| `OSV_PREMIERE_IMPORTER` | `ON` | Build `OpenOSVImporter.prm` |
| `OSV_PREMIERE_REFRAME` | `ON` | Build `Open360Reframe.aex` |
| `OSV_PLUGIN_STAGE_DIR` | `<build>/plugins/OpenOSV` | Where the modules and their runtime DLLs are staged |

Set both plug-in options to `OFF` to build and test only the shared layer and
the mock host - useful while working on `plugins/common`:

```powershell
scripts\vsdev.cmd cmake --preset windows-msvc-premiere-release `
    -DOSV_PREMIERE_IMPORTER=OFF -DOSV_PREMIERE_REFRAME=OFF
```

### What the build produces

Everything lands in the stage directory, ready to be copied as one folder:

* `OpenOSVImporter.prm` and `Open360Reframe.aex` (MODULE libraries with Adobe's
  extensions, no `lib` prefix, `/SUBSYSTEM:WINDOWS`, dynamic CRT);
* every runtime DLL the modules import, directly or delay-loaded, found by
  walking `dumpbin /DEPENDENTS` recursively (FFmpeg, `OpenCL.dll`, fmt, spdlog,
  zlib) - system DLLs are left alone;
* the PDBs.

The effect's PiPL resource goes through the After Effects three-step pipeline
(`cl /EP` -> `PiPLtool.exe` -> `cl /EP`) driven by `osv_add_pipl()`; the first
step records its includes in a depfile, so editing `AE_General.r` or a project
header regenerates the resource.

### Installing

```powershell
scripts\install_plugins.ps1            # newest build, needs admin
scripts\install_plugins.ps1 -StageDir build\windows-msvc-premiere-release\plugins\OpenOSV
scripts\install_plugins.ps1 -Uninstall
```

The script copies the stage folder into
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\`, which Premiere
Pro, Media Encoder and After Effects all scan. That path is under Program
Files, so the script relaunches itself through UAC (with an explanation of why)
unless you pass `-NoElevate`. It refuses to overwrite a module while a host
process is running, prints the path of Premiere's `Plugin Loading.log` so you
can check the plug-ins were accepted, and reminds you to hold **Shift** while
Premiere Pro launches - without that, the cached plug-in list in the registry
means a new or replaced module is never noticed.

### Runtime switches

Two environment variables change how a plug-in behaves at run time. Both are
read by the module itself, so they work the same in Premiere, in Media Encoder
and in the tests.

| Variable | Effect |
|---|---|
| `OPENOSV_IMPORTER_NO_DIALOG=1` | `imGetPrefs8` accepts the current (or default) source settings instead of showing the modal "OpenOSV Source Settings" dialog. Needed for unattended rendering and for any automated test: a modal dialog pumps messages until a human clicks something, so without this a render farm or a CI job blocks forever. Any value other than `0` enables it. |
| `OSV_PLUGIN_LOG_LEVEL=debug\|trace` | Lowers the plug-in log's threshold. The log is `%LOCALAPPDATA%\OpenOSV\OpenOSVImporter.log`, rotated at 4 MB, and every line is also sent to `OutputDebugStringW` so DebugView shows it live. The file is opened with `_SH_DENYWR`, so it can be read while a host holds it. |

### Adobe SDK gotchas

* Every Adobe header is `#pragma pack(push, 1)`. Plug-in code never defines a
  struct inside that region and never redefines an SDK struct.
* `PrSDKTypes.h` unconditionally `#define`s `NOMINMAX`, which collides with the
  project-wide definition from `osv_warnings`. `osv_premiere_sdk` therefore
  disables C4005 for the targets that include Adobe headers, and only for
  those.
* The plug-ins must use the **dynamic** CRT (`/MD`). Adobe's loader shares
  fiber-local storage slots between plug-ins and a static CRT exhausts them
  (SDK guide 3.10.4). Every target in `plugins/` and `tests/premiere/` sets
  `MSVC_RUNTIME_LIBRARY` accordingly.
* Premiere ships its own, older FFmpeg in the application directory. The
  plug-ins delay-load theirs and resolve it from their own folder through the
  hook in `plugins/common/DelayLoad.cpp`, so the loader can never bind us to
  Adobe's copy. A post-build step (`cmake/OsvCheckDelayLoad.cmake`) runs
  `dumpbin /DEPENDENTS` on every linked module and **fails the build** if it
  imports anything outside the OS and the CRT directly, so a new dependency
  cannot silently start binding to Adobe's copy.
* Premiere loads the importer and the effect into one process, and both
  statically link the OpenOSV library while spdlog lives in its own DLL. The
  library therefore keeps its logger out of spdlog's global registry: the
  registry is shared by every module in the process, and
  `spdlog::stderr_color_mt("osv")` throws `"logger with name 'osv' already
  exists"` the second time a module initialises. See `src/osv/core/Log.cpp`.
* FFmpeg's `avcodec_flush_buffers()` is not enough to make a backward audio
  seek bit-exact with a continuous decode of the same range: the AAC decoder
  keeps state across a flush that a freshly opened context does not have. The
  importer re-opens the decoder (and the swresample context) on every seek, so
  audio reached by scrubbing and audio reached by conforming are the same
  samples.

## Stale `nvcc` on PATH

Machines that accumulated several CUDA toolkits often have an old `nvcc` first
on `PATH`. The presets force `CMAKE_CUDA_COMPILER` to `%CUDA_PATH%\bin\nvcc.exe`
so the toolkit selected by the NVIDIA installer wins. Override with
`-DCMAKE_CUDA_COMPILER=<path>` if you keep several toolkits.

## Sample footage and test tags

The test-suite uses a real Osmo 360 clip for the `[sample]` tests. Point
`OSV_SAMPLE_FILE` (CMake cache variable or environment variable) at a 6K D-Log M
`.OSV`; the `.LRF` next to it is picked up automatically. When the clip is
absent those tests report SKIP. Other tags: `[cuda]`, `[opencl]` (need a
device), `[hwaccel]` (hardware decoder), `[ffmpeg-exe]` (ffmpeg on PATH or
`OSV_FFMPEG_EXE`). `ctest --preset ci` excludes everything a GPU-less runner
cannot execute.

## Golden data

`tests/golden/*.json` is produced by the independent Python implementations in
`scripts/` (`gen_golden.py`, `gen_lens_tables.py`, `colour_reference.py`).
Regenerate only with the scripts; the C++ tests must agree with Python, never
the other way round.

## Licensing checks

The build refuses the `gpl`/`nonfree` vcpkg FFmpeg features
(`cmake/OsvFFmpeg.cmake`). `osvtool --version` prints the FFmpeg configuration
it is linked against and flags GPL builds.
