# Releasing OpenOSV

Windows binaries are built on the maintainer's machine by one script and
uploaded to GitHub Releases by hand. There is no release workflow on purpose:
the plug-ins compile against Adobe's Premiere Pro and After Effects SDKs,
which nobody may redistribute, so a hosted runner cannot build them.

What people download is one zip, `OpenOSV-x.y.z-windows-x64.zip`: unzip,
double-click `Install.cmd`, launch Premiere holding Shift.

## What you need

* The development setup from [`BUILDING.md`](BUILDING.md): Visual Studio 2022,
  CUDA 12.8 or newer (12.9 on the development machine; `sm_120` needs 12.8),
  and both Adobe SDKs at the preset's paths (or pass `-PremiereSdkDir` and
  `-AeSdkDir`).
* vcpkg as a git checkout at `VCPKG_ROOT` (default `C:\vcpkg`), **outside your
  user profile**: vcpkg's build paths end up inside the third-party DLLs, and
  the package check rejects any `C:\Users\` path.
* The [GitHub CLI](https://cli.github.com/), logged in (`gh auth status`).
* Time for the first run: every vcpkg port, FFmpeg included, is built from
  source once (see step 4). Later runs reuse it.

## 1. Pick the version

The version lives in one place: `project(openosv VERSION x.y.z)` in
`CMakeLists.txt`. `Version.h`, `osvtool --version`, the plug-ins, the zip's
name and the tag all follow it. Keep `version-string` in `vcpkg.json` equal
(the script warns when they differ). Semantic versioning.

The OpenOSV panel has its own version, in three places a test keeps equal:
`panel/uxp/manifest.json`, `panel/cep/CSXS/manifest.xml` and
`OsvCore.PANEL_VERSION` in `panel/shared/osvcore.js`. Bump it only when the
panel changed.

## 2. Move the changelog

In `CHANGELOG.md`, move the `[Unreleased]` entries under a new
`## [x.y.z] - YYYY-MM-DD` heading and leave an empty `## [Unreleased]` above
it. The release notes draft takes its "What's new" list from that heading;
without it, the script falls back to `[Unreleased]` and says so.

## 3. Test and commit

Full build and the full suite on your development build, with the sample clip:

```powershell
scripts\vsdev.cmd cmake --build --preset windows-msvc-premiere-release
$env:OSV_SAMPLE_FILE = "<path>\example_footage_dlogm.OSV"
scripts\vsdev.cmd ctest --preset premiere
```

Commit the version bump and the changelog, and push `main`. The package
records the commit it was built from and warns about uncommitted changes.

## 4. Build the package

```powershell
scripts\package_release.ps1
```

It prints every step. What it makes, in `dist\` (git-ignored):

| File | What |
|---|---|
| `OpenOSV-x.y.z-windows-x64\` | the package, unzipped, for a look before upload |
| `OpenOSV-x.y.z-windows-x64.zip` | the upload; its SHA-256 is printed |
| `RELEASE_NOTES.md` | a draft for the release page |

Inside the zip: `Install.cmd`, `Uninstall.cmd`, `README.txt`, `LICENSE`,
`NOTICE`, `CHANGELOG.md`, `SHA256SUMS.txt`, `plugins\OpenOSV\` (the three
modules and their DLLs), `luts\`, `presets\`, `panel\` (CEP and UXP sources
plus the `.ccx`), `scripts\install_plugins.ps1`, `cli\` (`osvtool.exe` and its
DLLs) and `licenses\`. When the build made the DaVinci Resolve bundle
(`OSV_BUILD_OFX`, on in the preset), the zip also carries
`plugins\OpenOSV.ofx.bundle\` (`OpenOSV.ofx` and its own import closure),
`scripts\install_ofx.ps1` and `Install-Resolve.cmd` / `Uninstall-Resolve.cmd`,
and `licenses\` gains the OpenFX headers' licence.

**Why it builds its own tree.** FFmpeg records its whole configure line in its
DLLs (it is what `avutil_configuration()` returns), and vcpkg's port passes
the install directory in it. The development presets install vcpkg packages
inside the checkout, so a zip made from a development build would carry your
source path to every user. The script therefore configures the
`windows-msvc-premiere-release` preset into `build\release-package` with:

* `VCPKG_INSTALLED_DIR=<VCPKG_ROOT>\installed-openosv-release`, outside the
  checkout;
* `VCPKG_BINARY_SOURCES=clear`: vcpkg's package hash does not include the
  install directory, so the binary cache would hand back the FFmpeg built for
  the old tree. The first run builds every port from source; after that the
  tree is simply there;
* `OSV_BUILD_TESTS=OFF`, so nothing from `tests\` can ship;
* `OSV_ENABLE_ONNXRUNTIME=OFF`: the neural-flow runtime is about 1.4 GB and is
  not shipped, so its backend is not built into a binary that could never
  load it. Auto then uses the classical flow.

Options: `-SkipBuild` re-packages `build\release-package` as it is;
`-BuildDir <dir>` packages another Release build (brought up to date, not
reconfigured); `-OutDir`, `-VcpkgRoot`, `-VcpkgInstalledDir`, `-Jobs`.

## 5. What fails the package

Nothing is zipped when any of these fails:

* **The build.** Release (a Debug build imports the debug CRT, which may not be
  redistributed), plug-ins on, `osvtool --version` equal to the CMake version.
* **The GPU list.** `CMAKE_CUDA_ARCHITECTURES` must be the list the texts
  describe (`75-real;86-real;89-real;120-real;120-virtual`: Turing, Ampere,
  Ada, Blackwell, plus PTX for later 12.x parts). Changing the architectures
  means changing the GPU text in the script and in `README.md` too.
* **Dependencies.** The modules' and `osvtool`'s DLLs are their import
  closure, walked with `dumpbin`. Every import must resolve from the package,
  Windows or the NVIDIA driver. A DLL a stale build left in the stage folder
  is reported and left out.
* **The CLI.** `cli\osvtool.exe --version` must run with `PATH` cut down to
  Windows' own folders. The LUTs are generated by that same packaged
  `osvtool` (`scripts\gen_luts.ps1`); if they differ from the committed
  `luts\`, the script warns: regenerate and commit them.
* **The panel.** The `.ccx` is built by the packaged `install_plugins.ps1` in
  a dry run, so the panel is known to install from the package layout.
* **Licences.** Every shipped DLL must map to a vcpkg port with a licence
  text; a new dependency cannot ship until its entry is added to the table
  in the script. FFmpeg must have no `--enable-gpl`, `--enable-nonfree` or
  `--enable-version3`, and its port must be the one vcpkg's baseline names.
* **Hygiene.** Every printable string, ASCII and UTF-16, of every shipped
  file and of the release notes is checked for user-profile paths, paths into
  the folder that holds the checkout (any slash style, MSYS too), and
  anything in the maintainer's private rules file
  (`%LOCALAPPDATA%\OpenOSV\release-hygiene.txt`, never committed; format in
  the script's `-PrivateRules` help). The licence texts are exempt from the
  private phrases only. Each hit is printed with its file, offset
  and context. A source path in one of our own binaries is fixed at the
  cause: whatever compiled the path in (a `__FILE__`, a CMake path baked into
  a string), not the package.

## 6. Check it

Read `dist\RELEASE_NOTES.md`: it is a draft. The highlights come from the
README's "What it is", the change list from the changelog heading; trim the
list to what a user cares about.

Install from the zip without touching Premiere, into scratch folders, from an
elevated prompt (otherwise the script asks for administrator rights):

```powershell
$test = "$env:TEMP\osv release test"
Expand-Archive dist\OpenOSV-x.y.z-windows-x64.zip $test
$pkg = "$test\OpenOSV-x.y.z-windows-x64"
powershell -NoProfile -ExecutionPolicy Bypass -File "$pkg\scripts\install_plugins.ps1" `
    -Destination "$test\MediaCore" -PresetDestination "$test\presets" -PanelDestination "$test\panel"
& "$pkg\cli\osvtool.exe" probe <clip>.OSV
```

Best of all, install it for real on a test machine or VM with Premiere:
double-click `Install.cmd`, launch holding Shift, drop a clip.

## 7. Publish

```powershell
gh release create vX.Y.Z dist/OpenOSV-X.Y.Z-windows-x64.zip --title "OpenOSV X.Y.Z" --notes-file dist/RELEASE_NOTES.md
```

`gh` creates the tag at the head of `main` when it does not exist yet, which
is why the release commit is pushed first. The notes link the asset at
`releases/download/vX.Y.Z/`, and the README's Download section points at
`releases/latest`, so neither needs editing per release.

**FFmpeg's source, beside the binary.** `licenses\FFmpeg-BUILD.txt` names the
exact upstream tag and vcpkg's recipe with its patches. To serve the source
from the same place as the binaries, as LGPL-2.1 section 6(d) puts it, attach
the archive vcpkg built from and the recipe it used:

```powershell
$v = "<ffmpeg version>"      # FFmpeg-BUILD.txt, "What"
$tree = "<git tree>"         # FFmpeg-BUILD.txt, "Source code"
Compress-Archive "$env:VCPKG_ROOT\buildtrees\versioning_\versions\ffmpeg\$tree\*" "dist\ffmpeg-$v-vcpkg-port.zip"
gh release upload vX.Y.Z "$env:VCPKG_ROOT\downloads\ffmpeg-ffmpeg-n$v.tar.gz" "dist\ffmpeg-$v-vcpkg-port.zip"
```
