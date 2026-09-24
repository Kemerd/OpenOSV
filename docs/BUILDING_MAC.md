# Building OpenOSV on macOS

> **Status: untested on real hardware.** Everything below builds, and most
> of it is tested, on GitHub's macOS runners (Apple Silicon virtual
> machines). Nobody has yet run the plug-ins inside Premiere Pro on a Mac.
> The table says exactly what has and has not been checked.

## What is verified, and how

| Part | Built on CI | Tested on CI | Notes |
|---|---|---|---|
| Library, `osvtool` | yes | yes, the whole suite | Tests that need the sample clip SKIP (the clip is never on CI) |
| Metal renderer (`--device metal`) | yes, kernels compiled ahead of time | yes, against the CPU reference on synthetic fisheyes (same 60 dB bar as CUDA / OpenCL) | Runs on the runner's virtual GPU |
| OpenCL renderer (Apple's framework) | yes | no (the runner exposes no OpenCL device) | Deprecated by Apple; kept as a fallback |
| VideoToolbox hardware decode | yes | no (decode tests need the sample clip) | `--hw videotoolbox`, or `auto` |
| Plug-in pieces that need no Adobe SDK | yes | yes (`tests/macos`) | The log, the text and number helpers, the Metal GPU path of the effect against its CPU path, the importer's engine hooks, and the bundle packaging (a probe bundle is built, loaded and inspected) |
| Premiere plug-ins (importer, effect, Source Settings) | only when the SDK secret is set (below) | only the tests above | Never loaded into Premiere Pro on a Mac |
| Release zip | yes | the packaged `osvtool` is started once | |

## Toolchain

| Tool | Version | Notes |
|---|---|---|
| macOS | 13.3 or newer, Apple Silicon | The deployment target; `std::format` of floating point needs 13.3 |
| Xcode | 16 or newer (15.3+ should work) | AppleClang, libc++, the Metal compiler and Rez. Without the Metal compiler (newer Xcodes download it separately: `xcodebuild -downloadComponent MetalToolchain`) the Metal renderer compiles its kernels at run time instead |
| CMake | 3.28+ | |
| Ninja | any | `brew install ninja` |
| vcpkg | any recent checkout | Set `VCPKG_ROOT` |

## Configure, build, test

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-release
cmake --build --preset macos-release
ctest --preset macos
```

The first configure installs the vcpkg manifest (FFmpeg takes about five
minutes on an M1). Presets:

| Preset | What |
|---|---|
| `macos-release` / `macos-debug` | Library, `osvtool`, tests; Metal and OpenCL renderers |
| `macos-premiere-release` | The same plus the Premiere Pro plug-ins (needs the SDKs) |
| `ci-macos` | What GitHub Actions builds |

vcpkg uses the overlay triplet `cmake/triplets/arm64-osx-openosv.cmake`:
the small C++ libraries are static, FFmpeg is a shared library, as on
Windows (OpenOSV links FFmpeg dynamically under the LGPL everywhere). No CUDA
on a Mac: the Metal renderer takes its place, and `--device auto` picks
Metal, then OpenCL, then the CPU.

## The Premiere Pro plug-ins

They need the Premiere Pro C++ SDK and the After Effects C++ SDK, which
Adobe's licence does not allow anyone to redistribute, so the repository
has neither (see [BUILDING.md](BUILDING.md)). Put them where the preset
looks, or pass the two paths:

```sh
cmake --preset macos-premiere-release \
      "-DOSV_PREMIERE_SDK_DIR=$HOME/Adobe/Premiere Pro 26.0 C++ SDK" \
      "-DOSV_AE_SDK_DIR=$HOME/Adobe/AfterEffectsSDK"
cmake --build --preset macos-premiere-release
ctest --preset macos-premiere
scripts/install_plugins.sh        # asks for your password (sudo) for the Adobe folder
```

On a Mac the PiPL goes through Rez (from Xcode), not `PiPLtool.exe`, so any
AE SDK download with `Examples/Headers` and `Examples/Resources/AE_General.r`
works. The build produces, in `build/macos-premiere-release/plugins/OpenOSV/`:

| Bundle | What |
|---|---|
| `OpenOSVImporter.bundle` | The importer (package type `BNDL`, IMPT resource from `plugins/importer/OpenOSVImporter.r`) |
| `Open360Reframe.plugin` | The effect; its GPU path is Metal |
| `OpenOSVSourceSettings.plugin` | The master clip Source Settings effect |

Each bundle carries its own FFmpeg in `Contents/Frameworks`, renamed
`OpenOSV_lib*.dylib` so the loader can never hand it a copy some other part
of the host loaded under the usual name (the Mac counterpart of the Windows
delay-load hook), exports only its entry points, and is signed ad hoc.
`cmake/OsvMacBundle.cmake` builds that layout.

### What is different from Windows

* **GPU.** Premiere hands GPU effects Metal on a Mac. The effect's Metal
  kernel (`plugins/reframe/ReframeKernel.metal`) wraps the same shared
  `osvReframeEquirectPixel()` as the CUDA kernel. The *direct path*
  (rendering the view straight from the fisheyes, [DIRECT_GPU.md](DIRECT_GPU.md))
  is CUDA only and is not built: every view is rendered from the importer's
  equirect. The importer stitches on Metal.
* **Render device.** The Source Settings "Render Device" entry that says
  CUDA on Windows says Metal on a Mac; the stored value is the same, so a
  project moved between the two keeps meaning "the GPU".
* **Source Settings.** There is no modal Source Settings dialog on a Mac;
  the OpenOSV Source Settings effect in the Effect Controls panel is where
  the settings are changed. `imGetPrefs8` accepts the clip's settings as
  they stand.
* **Hardware decode.** VideoToolbox instead of D3D11VA / NVDEC.
* **Files.** Logs go to `~/Library/Logs/OpenOSV/`, the user defaults to
  `~/Library/Application Support/OpenOSV/defaults.json`.
* **Not on a Mac yet:** the neural optical flow backend (ONNX Runtime) and
  the mock-host plug-in tests (`tests/premiere`, Win32 throughout).

### Installing

`scripts/install_plugins.sh` is the Mac twin of `install_plugins.ps1`:

* the bundles into
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/OpenOSV/`
  (through `sudo`), with the quarantine attribute removed and an ad-hoc
  signature, and the LUTs into `LUTs/` beside them;
* the sequence presets into
  `~/Documents/Adobe/Premiere Pro/<version>/Profile-<user>/Settings/SequencePresets/OpenOSV/`;
* the companion panel: the CEP build into
  `~/Library/Application Support/Adobe/CEP/extensions/com.openosv.panel`
  with `defaults write com.adobe.CSXS.<n> PlayerDebugMode 1` (the previous
  value is recorded and `--uninstall` puts it back), or the UXP build through
  Adobe's plug-in installer when it is present.

`--help` lists every option (`--uninstall`, `--panel-only`, `--no-presets`,
dry runs with `--destination` / `--preset-destination` / `--panel-destination`).
Hold **Shift** while Premiere Pro starts after an install so it rescans its
plug-ins.

## CI and the SDK secret

`.github/workflows/macos.yml` runs on every push to the `mac` branch, on
`macos-14` (Apple Silicon). It builds and tests everything above, caches
vcpkg's binary packages so FFmpeg is built once, and uploads
`OpenOSV-<version>-macos-arm64.zip`: `osvtool` with its FFmpeg dylibs, the
LUTs, the sequence presets, the panel, `scripts/install_plugins.sh`, this
document and the licences - and the three plug-in bundles when they were
built.

The plug-ins are built only when the repository secret
**`OSV_ADOBE_SDK_ARCHIVE_URL`** is set: a private URL of a zip or tar
archive that contains the Premiere Pro C++ SDK and the After Effects C++ SDK
anywhere inside it (they are found by `Examples/Headers/PrSDKImport.h` and
`Examples/Resources/AE_General.r`). The archive is downloaded into the
runner's temporary folder, used for the build and deleted by the last step;
nothing from it is uploaded or logged. Without the secret those steps are
skipped and the job is still green.
