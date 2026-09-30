# VEGAS Pro (OpenFX + extension)

**Status: a preview, and nobody has run it inside VEGAS yet.** Everything here
is built and checked against a strict mock OpenFX host that plays VEGAS's
part (`tests/ofx`), and the install script is exercised against scratch
folders. The first live test in VEGAS Pro is pending: what this page says
VEGAS does is the design, and the
[live-test checklist](#what-is-tested-and-what-isnt) is what confirms it. If
you try it, please report what you see, or send a fix: see
[Reporting a problem](#reporting-a-problem).

VEGAS Pro is an OpenFX host, so the same `OpenOSV.ofx.bundle` that serves
DaVinci Resolve ([`RESOLVE.md`](RESOLVE.md)) loads in it: the generator
**OpenOSV Source** and the filter **OpenOSV 360 Reframe**, both compiled from
the Premiere plug-ins' own source. Stitch, seam, parallax, sky seam fix, lens
shading, colour and the camera are the Premiere ones.

An OpenFX generator can't say how long it is and has no audio, and VEGAS
won't do that part for you. So VEGAS gets one more piece, an **Application
Extension** written against the VEGAS scripting API, which does the setup from
outside:

| Piece | What it does |
|---|---|
| **OpenOSV Source** | Generator (Media Generators). Opens a DJI Osmo 360 `.OSV` (or its `.LRF` proxy) and stitches it. Outputs a reframed view or the whole 360 sphere. |
| **OpenOSV 360 Reframe** | Filter (Video FX). Points the virtual camera into any equirectangular clip. |
| **The extension** | **Tools > Extensions > Import OSV...**, and a dock panel. Makes the generator media at the clip's exact length and size, sets its file and levels, extracts the audio and groups it with the video. Also applies 360 Reframe to equirect events, framing looks, easing presets and stabilisation. |

## Install

VEGAS scans plug-ins when it starts, and it holds them open while it runs.
Close it first: the installer refuses while any `vegas*` process is running,
and tells you which.

**From the release:** download `OpenOSV-x.y.z-vegas-windows-x64.zip`, unzip
it and double-click **`Install.cmd`**. It asks for admin rights once.
**`Uninstall.cmd`** takes everything back out. The Premiere Pro and DaVinci
Resolve zips are separate downloads; the three install independently.

**From source.** The bundle needs no Adobe SDK, and no VEGAS either: every
build makes it. The *extension* is C# and compiles against
`ScriptPortal.Vegas.dll`, which comes from a VEGAS Pro install on your machine
(it is VEGAS's, so it is never committed or redistributed). With VEGAS
installed, a normal build stages the extension beside the bundle:

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset windows-msvc-cuda-release
cmake --build --preset windows-msvc-cuda-release
scripts\install_vegas.ps1
```

`install_vegas.ps1` does three things, in this order:

1. **The OpenFX bundle**, by calling `install_ofx.ps1`, into
   `C:\Program Files\Common Files\OFX\Plugins\`. VEGAS scans that folder as
   well as its own `OFX Video Plug-Ins`, and Resolve reads it too: one
   installed bundle serves both editors.
2. **The extension:** `OpenOSV.Vegas.dll` and `OpenOSV.Vegas.Core.dll` into
   `%ProgramData%\VEGAS Pro\Application Extensions\`, then unblocked. A DLL
   that came out of a downloaded zip carries the "from the internet" mark,
   and .NET can refuse to load it.
3. **A clean scan.** VEGAS keeps its plug-in list in per-user caches and does
   not look again on its own. For every version folder under
   `%LOCALAPPDATA%\VEGAS Pro\<version>\` the script deletes
   `svfx_plugin_cache.bin`, `plugin_manager_cache.bin` and OpenOSV's own
   `svfx_Ofx*org.openosv*.log` describe dumps. Nothing else in those folders
   is touched. (Deleting the caches with VEGAS closed is the standard advice
   for a plug-in that doesn't show up.) The first start afterwards takes a
   little longer: it is scanning.

It lists the VEGAS versions it finds, elevates once through UAC (and replays
what the elevated run printed), and works on Windows PowerShell 5.1 and
PowerShell 7. Options:

| Option | What |
|---|---|
| `-Uninstall` | Removes exactly what the install put in place: the bundle, the two DLLs and the caches |
| `-SkipBundle` | Leaves the OpenFX bundle alone (it is shared with Resolve): extension and caches only. With `-Uninstall`, keeps Resolve working |
| `-DryRun` | Prints what would happen and changes nothing |
| `-StageDir`, `-ExtensionSource` | Where to take the bundle and the extension from |
| `-BundleDestination`, `-ExtensionDestination`, `-LocalAppData`, `-ProgramFilesDir` | Point the script at scratch folders |
| `-NoElevate` | Fail instead of asking for rights |

VEGAS Pro 17 or later is the recommendation; 14 to 16 are best effort. VEGAS
Pro 2026 installs under `C:\Program Files\BorisFX\Vegas Pro 2026`; the script
finds it, and clears the cache folder that matches it.

## Editing an .OSV clip in VEGAS

1. **Tools > Extensions > Import OSV...** and pick the clip. (The same
   command is in the dock panel, **View > Extensions > OpenOSV**.)
2. The extension reads the clip's length, frame rate and size with
   `osvtool probe`, creates **OpenOSV Source** media at exactly that length
   and size, sets the clip as its file, and sets **Output Levels** for your
   project (see [Levels and colour](#levels-and-colour)).
3. It extracts the clip's audio to a WAV and groups it with the video.
4. Frame the shot with the generator's controls: Preset, Lens, Pan / Tilt /
   Roll, FOV, Correction Angle and Zoom (DJI lens), Classic FOV / Distortion
   (Classic lens), Keyframe Easing and Smooth Keyframes. They keyframe like any
   other effect parameter, and behave exactly as in Premiere.

**Without the extension**, by hand:

1. Drag **OpenOSV Source** from **Media Generators** onto the timeline.
2. In its controls, choose the `.OSV` with **Choose .OSV File...** (or paste
   the path).
3. **Clip** shows the clip's length. Set the event to that length: a generator
   can't declare one. Past the end of the clip it renders transparent black.
4. Set **Output Levels** to match the project ([below](#levels-and-colour)).
5. Audio: see [Audio](#audio).

## Output

- **Reframed view** (the default) renders the camera's view straight from the
  clip's native sphere, at the project's size.
- **360 equirect** renders the whole sphere at the project's size. Use it for a
  2:1 project, a 360 export, or to feed **OpenOSV 360 Reframe** yourself.

**Start Frame** slides the clip under the generator, in the clip's own frames.

## Levels and colour

**Output Levels** exists only under VEGAS, and has two choices: **Full range
(0-255)** and **Studio RGB (16-235)**. VEGAS's video-levels projects (8-bit,
and 32-bit video levels) work in studio RGB, and VEGAS never converts a
generator's output between levels. A generator that hands over full-range
pixels into such a project shows lifted blacks and clipped whites, so the
generator does the conversion itself. The extension picks the choice from the
project's pixel format; by hand, pick **Studio RGB** for a video-levels project
and **Full range** for a full-range (32-bit full range) one. The default is
Studio RGB.

**Colour Output** defaults to **Rec. 709** (with DJI's look), and Rec. 709 is
what to use. The other outputs (BT.2100 PQ / HLG, D-Log M with no transform)
behave as in Resolve; see [`RESOLVE.md`](RESOLVE.md#colour) and
[`COLOR.md`](COLOR.md). **How VEGAS treats HDR and ACES projects with generated
media is to be confirmed**, so this page makes no promise about them yet.

## Audio

A generator has no audio, so **Import OSV...** extracts the clip's track to a
32-bit float WAV, with the codec's priming removed so it lines up with the
picture the way it does in Premiere, and groups it with the video. By hand:

```powershell
osvtool extract CAM_0001.OSV --audio CAM_0001.wav
```

Put it under the generator, lined up with the generator's first frame (or with
**Start Frame** if you moved it).

## Speed

VEGAS hands an OpenFX plug-in **CPU images only**, never GPU ones. So the
plug-ins don't hand the pixels back and forth: decode, stitch, framing, the
levels and the final packing all stay on the GPU (CUDA on NVIDIA), and only the
finished view crosses back to VEGAS's memory. An NVIDIA card is the
recommendation here, more than it is in Premiere; without a usable one the
effects still run, on a slower path. Not measured inside VEGAS yet. If
playback stutters:

- set **Sphere Size** to 4K or 2K while cutting;
- lower the Preview window's quality, or use VEGAS's proxies and RAM preview;
- use the `.LRF` proxy switch ([below](#lrf-proxies));
- turn off **Seam Search** in Stitching.

## Reframing other 360 footage

**OpenOSV 360 Reframe** treats the whole image it is given as the sphere. The
event's effects must therefore see the stretched, project-size frame: put the
effect **after Pan/Crop**, with **Maintain aspect ratio** off, so a 2:1
equirect fills the project frame. The extension's **Apply 360 Reframe** command
sets this up for the selected events. It costs some resolution, because the
sphere is squeezed to the project's size first; for `.OSV` clips use OpenOSV
Source in Reframed view instead, which frames from the native sphere.

## The dock panel

**View > Extensions > OpenOSV** holds the same commands as the **Tools >
Extensions** menu: import, the LRF proxy toggle, 360 project setup, apply 360
Reframe to equirect events, framing looks, easing presets, stabilisation, make
framing unique, and relink.

## LRF proxies

The camera writes a small `.LRF` proxy beside each `.OSV`, and OpenOSV Source
opens either. The extension's LRF proxy toggle switches the generators
between the two: proxies while you cut, the `.OSV` for the final render.

## Reporting a problem

Please open an issue, or better, a pull request, at
<https://github.com/Kemerd/OpenOSV/issues>. Attach:

- `%LOCALAPPDATA%\OpenOSV\OpenOSVOfx.log`: everything the effects do. Set
  `OSV_PLUGIN_LOG_LEVEL=debug` before starting VEGAS for more.
- `%LOCALAPPDATA%\OpenOSV\OpenOSVVegas.log`: what the extension did.
- The describe logs VEGAS wrote for our effects:
  `%LOCALAPPDATA%\VEGAS Pro\<version>\svfx_Ofx1_1_plugin_x64-'org.openosv.*'*.log`.
  They show what VEGAS made of our descriptors.
- Your VEGAS version and whether the project is 8-bit, 32-bit video levels or
  32-bit full range.

An effect missing from the list: close VEGAS, run `install_vegas.ps1` again
(it clears the caches), start VEGAS and let it scan.

## What is tested, and what isn't

Checked on every build:

- The two effects against the mock OpenFX host, with the VEGAS host profile:
  descriptors, contexts, parameters, and images in VEGAS's formats (byte and
  float, RGBA and BGRA), compared with the Premiere effect's own render. The
  other hosts' descriptors are unchanged.
- `install_vegas.ps1` and the release script's VEGAS functions, against
  scratch folders: install, uninstall, dry run, cache clearing that leaves
  every other file alone.

**The live-test checklist**, everything below still to be run in VEGAS Pro:

- [ ] Install; the effects appear in Media Generators and Video FX after the
      first scan; the describe logs are written and clean.
- [ ] **Tools > Extensions > Import OSV...** imports a clip at the right length
      and size, with audio in sync.
- [ ] Output Levels: blacks and whites correct in an 8-bit project, a 32-bit
      video-levels project and a 32-bit full-range one.
- [ ] OpenOSV 360 Reframe on equirect footage, after Pan/Crop.
- [ ] Playback speed on an NVIDIA GPU, and the fallback without one.
- [ ] Rendering to a file: the same picture as the preview.
- [ ] VEGAS Pro 14 to 16, and VEGAS Pro 2026 (its data folder, presumably
      `%LOCALAPPDATA%\VEGAS Pro\2026.0`).
- [ ] HDR / ACES projects and generated media.
- [ ] The instances VEGAS clones per render thread: a clone of OpenOSV Source
      must not open its own decoder.

## Where the code is

| Path | What |
|---|---|
| `plugins/vegas/` | The Application Extension (C#): `OpenOSV.Vegas.dll` and `OpenOSV.Vegas.Core.dll` |
| `scripts/vegas/` | A smoke-test script and its README |
| `plugins/ofx/OfxHost.h` | The host profile: every VEGAS difference keys on `hostProfile()` |
| `plugins/ofx/OfxHostImage.h` | VEGAS's pixel formats and the one levels definition |
| `plugins/ofx/OfxGpuView.*` | The GPU path for hosts that give CPU images |
| `tools/osvtool/CmdProbe.cpp`, `CmdExtract.cpp` | `probe --json -` and `extract --audio` for the extension |
| `scripts/install_vegas.ps1` | Install / uninstall |
| `scripts/package_release.ps1` | `Write-VegasCommands` and the VEGAS zip |

Under any other host, Resolve included, the effects are what they were before
VEGAS support: the differences are keyed on the host profile and on nothing
else.

## Trademarks

VEGAS and VEGAS Pro are trademarks of their respective owners. OpenOSV is not
affiliated with, endorsed by or sponsored by them; the names are used only to
say what these plug-ins work with. Report problems with OpenOSV to OpenOSV, not
to them. See [`LEGAL.md`](LEGAL.md).
