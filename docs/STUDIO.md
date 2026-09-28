# OpenOSV Studio (`osvgui`)

A small desktop app in front of [`osvtool`](../README.md#the-command-line-tool):
drop `.OSV` clips or whole folders on it, pick the output, press **Start**.
It renders the queue one clip at a time with the plug-ins' own engine, so a
render is the frame Premiere shows.

![OpenOSV Studio rendering a queue](../img/osvgui.png)

## Get it

* **Windows:** `cli\osvgui.exe` in either release zip, beside `osvtool.exe`.
  Unzip the `cli` folder somewhere permanent and double-click `osvgui.exe`.
* **From source:** it builds with everything else when vcpkg provides the
  `imgui` (`glfw-binding`, `opengl3-binding`) and `glfw3` ports, which
  `vcpkg.json` asks for on Windows and macOS. `OSV_BUILD_GUI=OFF` leaves it
  out. On macOS it is `build/<preset>/bin/osvgui`, started from a terminal or
  Finder; it is not an `.app` bundle yet.

It finds `osvtool` next to itself first, then on your `PATH`, and says so
when there is none.

## Use it

1. **Queue clips.** Drag `.OSV` files or folders onto the window, or use
   **Add files** / **Add folder**. Folders are searched all the way down;
   `.LRF` proxies, empty files and macOS `._` metadata twins are skipped, and
   a clip already queued is not added twice.
2. **Pick the output** on the right. Every control is an `osvtool render`
   option (table below).
3. **Check the command.** The **Command** box shows exactly what runs for
   the next clip in the queue. **Whole folder** shows the same render as a
   `for` loop over the clip's folder (cmd on Windows, sh on macOS). **Copy**
   puts either on the clipboard.
4. **Start.** Each clip shows its progress ring, frames, speed and time left;
   the footer shows the whole batch. **Pause after this clip** lets the
   current clip finish and stops there (**Resume** carries on). **Stop** ends
   the clip now and deletes its unfinished file. **Log** opens osvtool's own
   output.

While a batch runs the settings are locked: every clip in it renders the
same way. The machine is kept from sleeping, the taskbar button shows the
progress, and the window asks for attention when the batch is done.

**In the queue:** click selects, `Delete` removes, drag reorders,
double-click opens a finished render (or shows the clip), right-click has
*Show in folder*, *Open render*, *Render again*, *Move to top* and *Remove*.
`Ctrl+O` adds files, `Ctrl+Shift+O` a folder, `Ctrl+Enter` starts.

## The controls and the options they set

| Control | `osvtool render` |
|---|---|
| 360 equirect / Reframed | `--mode equirect` / `--mode reframe` |
| Size | `--size WxH`; Native leaves it out (the camera's own resolution) |
| View | `--preset crystal-ball\|asteroid\|wide\|ultra-wide\|dewarping`, or Custom: `--fov` |
| Pan / Tilt / Roll | `--yaw` / `--pitch` / `--roll` (left out at 0) |
| Colour | Auto leaves `--color` out (D-Log M comes out HDR10, SDR stays SDR); HDR10 / HLG / SDR: `--color pq\|hlg\|709` |
| HDR style | `--tone` (with Auto, HDR10, HLG) |
| SDR look | `--look dji\|standard` (with Auto, SDR) |
| Stabilisation | `--stab off\|horizon\|full\|smooth\|smooth-horizon` (Direction lock is `full`) |
| Remove sun ghosts | `--flare` / `--no-flare` |
| Use my Premiere defaults | `--use-user-defaults`, and colour, stabilisation and sun ghosts are left to the saved defaults |
| Encoder | `--codec`; Auto is the encoder the start-up test found working |
| Quality | `--crf` (18 looks like the source) |
| Keep audio | off: `--no-audio` |
| Tag as 360 video | off: `--no-spherical-metadata` (shown when osvtool has the option; equirect only) |
| FFmpeg (Advanced) | `--ffmpeg` |
| Destination, File name | `--out`: next to each clip or in one folder; `{name}`, `{mode}`, `{preset}`, `{size}`; `.mp4` when there is no extension |
| Extra arguments (Advanced) | appended to every command |

Every render gets `--all` unless the extra arguments pick frames
themselves (`--range 0-299`, `--frame`). An option in the extra arguments
replaces the same option from the controls (either spelling, so
`--no-flare` there beats the switch), because osvtool refuses an option
given twice. Two clips that would write the same file in one batch get
` (2)`, ` (3)` added instead of overwriting each other. **Skip clips already
rendered** passes over a clip whose output is already there.

## FFmpeg and the encoder

Video goes out through FFmpeg, as with the command line. The app looks for
it where osvtool does (the FFmpeg you chose, `OSV_FFMPEG_EXE`, `PATH`) and in
the usual install places (winget, Scoop, Chocolatey, `C:\ffmpeg\bin`;
Homebrew and MacPorts on macOS), passing `--ffmpeg` when it is somewhere
osvtool would not look. Missing, it shows a banner with a download link and
a **Locate** button.

**Auto** picks the encoder by trying them: one tiny 10-bit test encode each
with `hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `hevc_videotoolbox`, then
`libx265`, with the options osvtool uses, and the first that works wins.
The answer is remembered until the ffmpeg file changes.

## Where things are kept

Everything you set, the window's place and the last command line:

* Windows: `%APPDATA%\OpenOSV\osvgui.json`
* macOS: `~/Library/Application Support/OpenOSV/osvgui.json`
* `OPENOSV_GUI_SETTINGS` names another file.

Saved a moment after each change and on exit; a damaged file is replaced
by the defaults, never trusted.

## Command line

```text
osvgui [files or folders...] [--start] [--screenshot out.png] [--screenshot-delay seconds]
```

Paths are queued as if dropped. `--start` starts the batch once they are
queued. `--screenshot` saves one frame of the settled window as a PNG and
quits (documentation, visual checks); `--screenshot-delay` waits that much
longer first.
