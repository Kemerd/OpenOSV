# VEGAS Pro scripts

## OpenOSV Smoke Test.cs

A VEGAS Pro script that checks, inside a running VEGAS, every scripting-API
fact the OpenOSV extension (`plugins/vegas`) relies on. It is the first thing
to run after installing OpenOSV into a VEGAS that nobody has tried it in yet.

**Run it**

1. Install the OpenOSV OpenFX bundle and restart VEGAS (it only looks for
   plug-ins at launch). The extension itself is optional for this test.
2. Open or create a project. Its pixel format decides the levels the test
   checks: 8-bit or 32-bit video levels expect Studio RGB, 32-bit full range
   expects Full range.
3. **Tools > Scripting > Run Script...** and pick `OpenOSV Smoke Test.cs`.
4. Pick one `.OSV` (or `.LRF`) when asked.

It imports the clip the way the extension does, then reads everything back:

| Checked | Why it matters |
|---|---|
| OpenOSV Source listed as `{Svfx:org.openosv.OSVSource}`, and every OpenOSV entry VEGAS lists | the extension looks the generator up by that id |
| `osvtool probe` states frames and a rational rate | the exact length comes from it |
| `new Media(generator)`, its OpenFX parameters, `ParameterChanged()` | how every value is written |
| the generator's Clip read-out refreshed | whether VEGAS forwards scripted edits to the plug-in (informational: the extension writes every value itself) |
| `outputLevels` present and set from the pixel format | VEGAS never level-converts a generator |
| `Media.Length` exact to the unit, `VideoStream.Size` settable | length and frame size of generated media |
| `Media.CustomData` round trip | where the extension remembers a clip's proxy state |
| `osvtool extract --audio` writes a WAV VEGAS opens; the audio against the video length | sync-exact audio |
| video + audio events, one group | the timeline the import builds |
| `MaintainAspectRatio` settable, the pan/crop bounds | what "Apply 360 Reframe" changes |

**The report** goes to `%LOCALAPPDATA%\OpenOSV\vegas-smoke-report.txt`, beside
the extension's own log (`OpenOSVVegas.log`) and the OpenFX bundle's
(`OpenOSVOfx.log`). A dialog shows the totals. Everything the test adds is one
undo step: **Ctrl+Z** removes it.

VEGAS compiles scripts itself, so the script is C# 5 only (checked with
`csc -langversion:5` against the VEGAS 16 and 17 scripting APIs).

**What it cannot check** - do these by hand once:

- `Media.CustomData` surviving a save and reopen: import a clip with the
  extension, switch it to LRF proxies, save, reopen, switch back to full
  quality; the Start Frame should be exactly what it was.
- Colour in HDR (HDR10 / HLG) and ACES projects: nobody has compared a
  generated clip against a known-good one yet.
- Playback speed and the look of the dock panel in VEGAS's own skins.
