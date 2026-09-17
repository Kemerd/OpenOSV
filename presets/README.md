# OpenOSV sequence presets

Three Premiere Pro sequence presets, so starting a 360 edit is one click
instead of hand-typing a frame size and a frame rate that has to be exactly
59.94 and not 60.

| File | Frame size | Shape | What it is for |
|---|---|---|---|
| `OpenOSV 2560x1440 59.94.sqpreset` | 2560 x 1440 | 16:9 | **The default.** Reframed delivery. Pairs with the importer's default 2560 x 1280 equirect output. |
| `OpenOSV 3840x2160 59.94.sqpreset` | 3840 x 2160 | 16:9 | 4K delivery. Set Output Size to Native or 4K in Source Settings first, or the reframe has nothing to crop from. |
| `OpenOSV 360 equirect 2560x1280 59.94.sqpreset` | 2560 x 1280 | 2:1 | Working on the sphere itself, or exporting a 360 VR file. Declares monoscopic equirectangular VR. |

All three are square pixels (1:1), progressive, 59.94 fps and 48 kHz stereo
with four mono audio tracks, which is what an Osmo 360 records.

## Where they install and where they appear

`scripts\install_plugins.ps1` copies them into a per-user `OpenOSV` folder
under Premiere's own settings root:

    %USERPROFILE%\Documents\Adobe\Premiere Pro\26.0\Profile-<user>\Settings\SequencePresets\OpenOSV\

They then appear in **File > New > Sequence** under a group called
**OpenOSV** - the subfolder name is the group heading, which is how Adobe's
own presets are organised (`HD 1080p`, `UHD (4K)`, `Social`, `Legacy`).

The script finds the `Profile-*` directory itself rather than assuming a user
name, creates `SequencePresets\OpenOSV\` if it does not exist, and refuses to
guess if it cannot find a settings root. Pass `-NoPresets` to skip the
presets entirely, or `-PresetDestination <dir>` to install them somewhere
else.

Premiere caches the preset list, so a running instance has to be **restarted**
before new presets show up.

### How that path was established

It is not documented, so it was derived from the installed application rather
than guessed:

1. The per-user settings root is `Documents\Adobe\Premiere Pro\<ver>\Profile-<user>\Settings\`.
   That directory exists on this machine for 11.0, 24.0 and 26.0, and already
   holds `EssentialSound`, `Export Destinations`, `Ingest Presets`,
   `Overlay Presets`, `Project View Presets`, `Source Patcher Presets`,
   `Timecode Presets` and `Track Height Presets` - written by the application
   itself, not by us.
2. The subfolder for sequence presets is `SequencePresets`, spelled without a
   space. Every one of the folder names above appears as a literal string
   inside `Adobe Premiere Pro 2026\Mezzanine.dll`, the module whose exports
   include `SequenceSettingsCache::GetSequencePresetsFromCache` and
   `SequencePreviewPresets::CollectSequencePresets`; `SequencePresets` appears
   there too, while `Sequence Presets` (with a space) does not. It is also
   exactly the folder name the shipped presets live in under
   `Program Files`.
3. `.sqpreset` is confirmed as the extension by the string table in
   `ScriptLayerPProQE.dll`, where `SequencePresets` and `sqpreset` sit
   adjacent.

What is **not** verified is the end-to-end behaviour, because Premiere Pro was
not launched: that the application picks these three files up from that
folder, shows the group as "OpenOSV", and creates a sequence with the right
settings. Everything up to and including the file format and the install
location is evidence-based; the final click is not.

## Why 59.94 and not 60

A DJI Osmo 360 records 59.94 fps, which is 60000/1001, not 60. Premiere
stores a frame duration in ticks, at 254016000000 ticks per second, so one
59.94 fps frame is exactly

    254016000000 * 1001 / 60000 = 4237833600 ticks

That is the `<VideoFrameRate>` value in all three files, and it is the same
integer the importer reports from `imGetInfo8` and the same one
`tests/premiere/common` pins. A sequence built by hand at "60" is 4233600000
ticks and drifts against the media by one frame every 1000 - which shows up
as audio sync slipping half a second across a 15 minute clip.

## The XML

The schema is not documented by Adobe; it was read off the presets the
installed application ships, under

    C:\Program Files\Adobe\Adobe Premiere Pro 2026\Settings\SequencePresets\

Two of them were used as templates, and nothing here is invented:

* `HD 1080p\HD 1080p 59.94 fps.sqpreset` and
  `UHD (4K)\UHD (4K) 2160p 59.94 fps.sqpreset` for the 2026 layout
  (`Version="9"`, which is the one that carries `WorkingColorSpace`,
  `SequenceWorkingColorSpace` and `AutoToneMapEnabled`) and for the 59.94
  tick value;
* `Legacy\VR\Monoscopic 29.97\3840x1920.sqpreset` for the
  `ImmersiveVideoVRConfiguration` payload that declares equirectangular VR
  (`"projectionType":1`, `"capturedHorizontalView":360`,
  `"capturedVerticalView":180`). That file is `Version="8"`, so the VR field
  was lifted from it and dropped into the Version 9 body rather than the
  whole file being copied.

Fields worth knowing about:

| Field | Meaning |
|---|---|
| `ClassID` | `5e73dd7e-...` identifies the record as a sequence preset. Identical in every preset Adobe ships; not ours to change. |
| `Version="9"` | The record layout, not our version. Premiere 2026 writes 9. |
| `VideoFrameRate` | Frame DURATION in ticks (see above), not a rate. |
| `VideoFrameSize` | `left,top,right,bottom`, so the last two are the width and height. |
| `AudioFrameRate` | Sample duration in ticks: 254016000000 / 48000 = 5292000. |
| `VideoTimeDisplay` | `106` = 59.94 fps non-drop-frame timecode. |
| `AudioTimeDisplay` | `200` = audio samples. |
| `VideoFieldType` | `0` = progressive. |
| `EditingModeGUID.Win` | `9678AF98-...` = the "Custom" editing mode, which is what allows an arbitrary frame size. |
| `VideoUseMaxBitDepth` | `true` here, unlike Adobe's stock presets: the importer hands Premiere 32-bit float frames, and an 8-bit sequence would quantise the sphere before the reframe resamples it, banding every gradient in the sky. |
| `VideoAllowLinearCompositing` | `true`, as in every stock 2026 preset. |
| `InitialNumberOfVideoTracks` | 3, matching the stock presets. |
| `AudioTracks` | Four mono tracks, verbatim from the stock 2026 layout. |

The files are UTF-8 with CRLF line endings and no trailing newline after
`</PremiereData>`, byte-for-byte matching what Adobe's own presets use.
