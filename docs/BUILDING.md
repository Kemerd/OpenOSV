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
| `windows-msvc-cpu-only` | Ninja | none |
| `windows-vs2022-cuda` | Visual Studio 17 2022 | CUDA + OpenCL (works from a plain prompt) |
| `ci-windows` | Ninja | CUDA + OpenCL, warnings as errors |

The first configure runs `vcpkg install` from the manifest. FFmpeg takes about
six minutes on a fast machine; a binary cache (`VCPKG_BINARY_SOURCES`) makes
later builds instant.

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
