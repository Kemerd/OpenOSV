# VEGAS Pro (OpenFX + extension)

**Status: experimental, working in VEGAS Pro 17.** Everything here is built
and checked against a strict mock OpenFX host that plays VEGAS's part
(`tests/ofx`), and 0.4.1 has run live in VEGAS Pro 17:

- **Works:** VEGAS finds both effects and describes them cleanly (the
  generator render-unsafe, the filter in the Filter context only, 8-bit and
  float in both channel orders), and loads the extension. **Import OSV...**
  puts a clip on the timeline at its length and size with its audio, the
  camera controls start where they should, a parameter set from a script
  reaches the plug-in, and playback shows every frame, on the GPU.
- **Fixed in 0.4.1:** creating OpenOSV Source media crashed VEGAS 17 in
  0.4.0, and imported clips froze on their last frame after a few frames
  (see the [changelog](../CHANGELOG.md)).
- **Not verified yet:** HDR and ACES projects. Stay on **Rec. 709** for now
  ([Levels and colour](#levels-and-colour)).

**Save your project before you try it,** and please report what happens:
see [Reporting a problem](#reporting-a-problem). With
`OSV_PLUGIN_LOG_LEVEL=debug` set before VEGAS starts, the effects' log traces
every OpenFX action VEGAS sends and the frame each render maps to. The
[live-test checklist](#what-is-tested-and-what-isnt) says what has been
confirmed in VEGAS and what is still the design.

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
| **The extension** | **Tools > Extensions > OpenOSV > Import OSV...**, and a dock panel. Makes the generator media at the clip's exact length and size, sets its file and levels, extracts the audio and groups it with the video. Also applies 360 Reframe to equirect events, framing looks, easing presets and stabilisation. |

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

1. **Tools > Extensions > OpenOSV > Import OSV...** and pick one clip or
   many. (The same command is in the dock panel, **View > Extensions >
   OpenOSV**, which also takes `.OSV` files dropped on it from Explorer.)
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
2. In its controls, pick the `.OSV` with the **Browse** button of **OSV
   File** (or paste the path).
3. **Clip** shows the clip's length. Set the event to that length: a generator
   can't declare one. Past the end of the clip it renders transparent black.
4. Set **Output Levels** to match the project ([below](#levels-and-colour)).
5. Audio: see [Audio](#audio).

## Output

- **Reframed view** (the default) renders the camera's view at the project's
  size, straight from the two fisheyes on an NVIDIA GPU (from the clip's
  native sphere on the CPU path).
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
effects still run, on a slower path.

**The Preview window's quality decides how hard OpenOSV Source works.** VEGAS
names a quality with every frame it asks for, and OpenOSV reads it:

| Quality | What OpenOSV Source renders |
|---|---|
| **Draft**, **Preview** (VEGAS's default) | Playback. The `.LRF` proxy beside the `.OSV` when **Playback Proxy** is on ([below](#lrf-proxies)). No waiting on analyses, no per-frame seam search, parallax or sun ghost fit |
| **Good**, **Best** | The final picture: the `.OSV`, every analysis, exactly what a file render gets |

**File > Render As** uses the template's rendering quality, Good by default,
so a render always gets the full stitch. To judge the stitch itself while
cutting, set the Preview window to Good or Best.

**The reframed view comes straight from the fisheyes.** Only the pixels the
camera shows are stitched: 2 MP for a 1080p view, not the 29.5 MP of an 8K
sphere, and no 472 MB sphere in VRAM. They are resampled once instead of
twice, so the view is sharper too. `OPENOSV_OFX_DIRECT=0` frames the view
out of the stitched sphere instead, as 0.4.1 did.

The GPU path is on by default in VEGAS only. Set `OPENOSV_OFX_GPU=0` before
starting VEGAS to switch it off (every frame then takes the CPU path), or
`OPENOSV_OFX_GPU=1` to use it in other OpenFX hosts too. The first frame of
every effect instance logs which path served it. With
`OSV_PLUGIN_LOG_LEVEL=debug`, every frame logs where its time went: the
quality VEGAS asked for, then a `frame-cost path=device` line (decode,
analyses, stitch, packing) and the readback.

If playback still stutters:

- check that **Playback Proxy** is on (Advanced) and the `.LRF` sits beside
  the `.OSV` with the same name;
- set the Preview window to Draft;
- use VEGAS's own RAM preview;
- with no NVIDIA GPU, set **Sphere Size** to 4K or 2K and turn off
  **Seam Search** in Stitching.

## Reframing other 360 footage

**OpenOSV 360 Reframe** treats the whole image it is given as the sphere. The
event's effects must therefore see the stretched, project-size frame: put the
effect **after Pan/Crop**, with **Maintain aspect ratio** off, so a 2:1
equirect fills the project frame. The extension's **Apply 360 Reframe** command
sets this up for the selected events. It costs some resolution, because the
sphere is squeezed to the project's size first; for `.OSV` clips use OpenOSV
Source in Reframed view instead, which frames from the fisheyes themselves.

## The dock panel

**View > Extensions > OpenOSV** docks like any VEGAS window and takes VEGAS's
own skin colours. One primary action, **Import OSV...**, and a drop zone for
`.OSV` / `.LRF` files dragged in from Explorer. Below them:

- every OpenOSV clip of the project, each with its **Output**, **Colour**,
  **Levels**, stabilisation and **Start Frame** editable in place;
- the LRF proxy toggle;
- DJI Studio's framing looks and keyframe easing presets, for the selected
  clips;
- a status line with the last action.

It lays out in one column when docked narrow and two when wide.

**Tools > Extensions > OpenOSV** has every command:

| Command | What it does |
|---|---|
| Import OSV... | See [above](#editing-an-osv-clip-in-vegas) |
| Edit with LRF proxies / Full quality | Switches every OpenOSV clip between its `.LRF` and its `.OSV` ([below](#lrf-proxies)) |
| 360 project setup | 360 output on, a 2:1 project, the selected clips in 360 equirect |
| Apply 360 Reframe to selected events | For other equirect footage ([above](#reframing-other-360-footage)) |
| Framing look, Keyframe easing | DJI Studio's five looks and seven easing presets, on the selected clips |
| Stabilisation | RockSteady and Horizon Leveling, together or apart, or off |
| Make framing unique | Gives the selected events their own copy of the generator media (see below) |
| Relink moved OSVs... | Finds moved clips by name under a folder you pick |
| Match levels to project | Sets **Output Levels** on every clip from the project's pixel format |

**One camera per media.** A generator's controls belong to its media, so every
event cut from one imported clip shares one camera: keyframes over time frame
each part. For two different framings of the same moment, **Make framing
unique** gives the selected events a copy of the media, with every control and
keyframe copied.

## LRF proxies

The camera writes a small `.LRF` proxy beside each `.OSV`, and OpenOSV Source
opens either. Two ways to use it:

- **Playback Proxy** (OpenOSV Source > Advanced, on by default). Frames VEGAS
  plays at Draft or Preview quality are stitched from the `.LRF`, at the same
  moment on the camera's clock; Good and Best stitch the `.OSV`. Nothing
  about the clip changes, so there is nothing to switch back before a render.
  Turn it off to play the `.OSV` at every quality.
- **The extension's LRF proxy toggle** swaps the file itself: the `.LRF` at
  every quality, renders included, until you switch back to full quality.
  Start Frame is kept on the camera's clock, so the cuts stay where they are
  both ways.

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
- The extension's logic (`OpenOSV.Vegas.Tests`): the probe output, exact
  rational timing, levels from the pixel format, the `.OSV` / `.LRF` pairing
  and proxy timing, relinking, the WAV cache, and that every OpenFX name it
  writes still exists in the plug-in's headers. The smoke-test script is
  compiled the way VEGAS compiles it (C# 5), and the built DLLs are checked
  for local paths.
- `osvtool probe --json -` and `extract --audio .wav`: the JSON's stable keys,
  and the WAV sample for sample against the importer's audio decoder.
- The bundle's layout: `OpenOSV.ofx` alone in `Contents\Win64`, and every DLL
  it and `osvtool.exe` need in `Contents\Libraries\Win64`.
- `install_vegas.ps1` and the release script's VEGAS functions, against
  scratch folders: install, uninstall, dry run, cache clearing that leaves
  every other file alone.

Written, and waiting for a GPU run: the GPU path against the CPU path in every
format, levels and render window (`tests/ofx_gpu`, and the `[cuda]` tests of
`tests/ofx`); the direct view's framing and sharpness against the CPU framing;
and Preview playback from the `.LRF` against Good from the `.OSV` (a `[sample]`
test of the VEGAS profile).

**The live-test checklist**, run in VEGAS Pro 17; the open items are still to
be run. The smoke-test script ([`scripts/vegas/README.md`](../scripts/vegas/README.md))
checks most of the scripting-API items and writes
`%LOCALAPPDATA%\OpenOSV\vegas-smoke-report.txt`.

- [x] Install; VEGAS 17 finds both effects on its first scan; the describe
      logs are written and clean; the extension loads.
- [x] **Creating OpenOSV Source media** from a script without a crash (fixed
      in 0.4.1), with the camera controls at their defaults afterwards.
- [x] **Tools > Extensions > OpenOSV > Import OSV...** imports a clip at the
      right length and size, with its audio grouped to it.
- [x] Playback shows every frame of an imported clip (fixed in 0.4.1).
- [x] A scripted parameter change reaches the plug-in (the **Clip** read-out
      refreshes), and the lens controls show and hide with **Lens**.
- [ ] Audio in sync by ear; the float WAV opens elsewhere.
- [ ] Inserting OpenOSV Source by hand from the Media Generators window.
- [ ] The dock panel comes back with the layout.
- [ ] The LRF proxy toggle survives a save and reopen: Start Frame exact.
- [ ] Output Levels: blacks and whites correct in an 8-bit project, a 32-bit
      video-levels project and a 32-bit full-range one.
- [ ] OpenOSV 360 Reframe on equirect footage, after Pan/Crop.
- [ ] Playback speed on an NVIDIA GPU, and the fallback without one.
- [ ] VEGAS names its quality on every render: the first-render log line
      reads `preview quality (interactive, draft)` in the Preview window and
      `good quality (exact, full stitch)` in a file render.
- [ ] Playback Proxy: at Preview quality the log says the clip `plays from its
      proxy`, and the picture follows the `.OSV`'s cuts; at Good it stitches
      the `.OSV`.
- [ ] Rendering to a file: the same picture as the preview.
- [ ] VEGAS Pro 14 to 16, and VEGAS Pro 2026 (its data folder, presumably
      `%LOCALAPPDATA%\VEGAS Pro\2026.0`).
- [ ] HDR / ACES projects and generated media.
- [ ] OpenOSV Source is declared render-unsafe under VEGAS, so VEGAS should
      not clone it per render thread: one decoder per clip in the log.

## Where the code is

| Path | What |
|---|---|
| `plugins/vegas/` | The Application Extension (C#): `OpenOSV.Vegas.dll` and `OpenOSV.Vegas.Core.dll` |
| `scripts/vegas/` | A smoke-test script and its README |
| `plugins/ofx/OfxHost.h` | The host profile: every VEGAS difference keys on `hostProfile()` |
| `plugins/ofx/OfxHostImage.h` | VEGAS's pixel formats and the one levels definition |
| `plugins/ofx/OfxGpuView.*`, `OfxGpuPipeline.*` | The GPU path for hosts that give CPU images: policy, device frames, framing / levels / pack kernels, pinned readback |
| `tests/ofx/`, `tests/ofx_gpu/` | The mock host in its generic, Resolve and VEGAS profiles; the GPU pipeline in every format |
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
