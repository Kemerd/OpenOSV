# The OpenOSV companion panel

Drop an `.OSV` (or its `.LRF` proxy) on a timeline and **Open 360 Reframe goes
on by itself.** A plug-in can't do that. Premiere effects and importers never
see the timeline, so this is a small Premiere panel called **OpenOSV**:

* **Auto-apply to OSV clips**: a switch, on by default, remembered between
  sessions.
* **New effects start with**: the Lens (DJI or Classic) and, optionally, a
  Drag Sensitivity for every effect the panel applies.
* **Apply to selected clips** and **Apply to all OSV clips in this sequence**,
  for clips that were already on the timeline.
* A status line with the last action, how many clips it touched, and when.

It ships in two builds, **UXP** and **CEP**, that share everything except the
few hundred lines that talk to Premiere.

## Install

```powershell
scripts\install_plugins.ps1                 # plug-ins, presets and the panel
scripts\install_plugins.ps1 -PanelOnly      # just the panel, no admin rights
scripts\install_plugins.ps1 -NoPanel        # everything but the panel
scripts\install_plugins.ps1 -PanelOnly -Uninstall
```

`-PanelFlavor Auto` (the default) picks whichever build installs **without a
single click**:

| Machine | Build | What the script does |
|---|---|---|
| Adobe's plug-in installer (UPIA) present | **UXP** | Builds `OpenOSV-panel-<version>.ccx` and runs `UnifiedPluginInstallerAgent.exe /install` on it. Removes a CEP copy left from before. |
| No UPIA | **CEP** | Copies the panel to `%APPDATA%\Adobe\CEP\extensions\com.openosv.panel\` and sets `PlayerDebugMode = "1"` under `HKCU\Software\Adobe\CSXS.<n>` (see below). The `.ccx` is still built, for later. |

Force a build with `-PanelFlavor Uxp` or `-PanelFlavor Cep`. `-PanelFlavor Uxp`
on a machine without UPIA opens the `.ccx` with whatever is registered for it
(normally the Creative Cloud app), which asks for **one confirmation**. That is
Adobe's documented route for a plug-in that doesn't come from the Marketplace.
`-PanelDestination <dir>` is a dry run: the panel (or the `.ccx`) is staged
there and nothing is registered.

**PlayerDebugMode.** A CEP panel that is not signed only loads when the current
user has `PlayerDebugMode = "1"` (a string) under
`HKCU\Software\Adobe\CSXS.<n>`, where `<n>` is the CEP major version of the
Premiere build (Adobe's PProPanel guide). The script reads `<n>` from each
installed Premiere's `PlugPlug.dll`: Premiere 2026 ships CEP 12.0.1, so it is
`CSXS.12`. It sets the value only where it isn't `1` already, and records the
value it replaced in `%LOCALAPPDATA%\OpenOSV\panel\install-state.json`.
`-Uninstall` puts back exactly that record and nothing else, so other unsigned
panels you rely on keep working. The switch is per user and only affects CEP
panels.

**Open it** from **Window > Extensions > OpenOSV** (CEP) or
**Window > UXP Plugins > OpenOSV** (UXP). Restart Premiere first if it was
running. Dock it once: Premiere reopens docked panels with the workspace.

### By hand

* **UXP.** Double-click `%LOCALAPPDATA%\OpenOSV\panel\OpenOSV-panel-<version>.ccx`
  and confirm. Or run
  `"C:\Program Files\Common Files\Adobe\Adobe Desktop Common\RemoteComponents\UPI\UnifiedPluginInstallerAgent\UnifiedPluginInstallerAgent.exe" /install <that .ccx>`.
  Remove it in the Creative Cloud app (Stock & Marketplace > Plugins > Manage
  plugins) or with `UnifiedPluginInstallerAgent.exe /remove OpenOSV`.
* **CEP.** Copy `panel\shared` to `...\CEP\extensions\com.openosv.panel\shared`
  and the contents of `panel\cep` to `...\CEP\extensions\com.openosv.panel\`,
  then add the `PlayerDebugMode` string value above.

Install **one** build. Two copies would both watch the timeline. They can't
double an effect (see "Never twice" below), but you'd have two panels.

## What auto-apply does

**It applies Open 360 Reframe to OSV clips that are added to a sequence while
the panel is watching.** That includes the first clips of a sequence created
while it's watching ("New Sequence From Clip", or dragging a clip onto an
empty timeline). It never retro-fits an existing edit:

* The first time the panel sees a sequence that already existed, it records
  that sequence's clips as a **baseline** and touches none of them. The same
  happens to every sequence of a project opened later, and to everything on
  the timeline at the moment the switch is turned back on.
* **Apply to all OSV clips in this sequence** is the explicit retro-fit.

**A clip is new only when it really is new.** The rule lives in
`OsvCore.diffItems()`:

* **moved or trimmed**: a clip vanished and one playing the same master clip
  appeared. Same track first, nearest start next. That's the same clip.
* **razor cut**: the new piece touches an existing piece of the same master
  clip, and the source continues across the cut (`out point == in point`).
  That's the same clip too.
* **a second drop of the same clip**: new, even placed end to end, because a
  fresh drop restarts at its in point.

That's why you can **remove the effect from one clip on purpose and it stays
removed**: moving or cutting that clip afterwards doesn't make it new.

**Never twice.** A clip whose effect list already has `OpenOSV.Open360Reframe`
(with or without Premiere's `AE.` prefix) is counted as "already had it". The
check is repeated at the last moment:

* **UXP**: inside `Project.lockedAccess()`, where Premiere guarantees the
  project can't change. If a clip's component count moved since the panel
  looked, it's re-read instead of appended to.
* **CEP**: inside one synchronous ExtendScript call.

**Which clips count as OSV.** The master clip's media path must end in `.osv`
or `.lrf`, in any case. This is the importer's own list (`ImporterEntry.cpp`),
and a test holds the two together. A folder named `trip.osv\`,
`clip.osv.mp4`, a nested sequence, an adjustment layer or a clip without media
never counts. Clips *inside* a nested sequence are handled in that sequence,
not through the nest.

**Event storms.** Premiere sends one event per item, audio included, so a
ten-clip drop is a burst. The panel waits until the burst has been quiet for
400 ms (never more than 2 s after the first event) and does one pass. A cheap
**signature poll** every 2.5 s backs the events up. It checks the track count
and the clips per track, and catches anything they missed: a new track that
had no listener yet, or another CEP panel that took over an ExtendScript
event.

**Undo.**

* **UXP**: each pass is one undo step, "Apply Open 360 Reframe". The Classic
  lens or a drag sensitivity adds a second one, "Set Open 360 Reframe lens",
  because a component's parameters can only be reached once it's on the clip.
* **CEP**: Premiere records each effect and each value as its own step.

**Where it goes in the effect list.** At the end (UXP
`createAppendComponentAction`; QE `addVideoEffect` does the same). Premiere
renders its fixed Motion / Opacity after the standard effects either way.

**Failure never reaches Premiere.** Every host call is guarded. Anything that
fails turns into one line in the status bar, for example "Couldn't apply: the
active sequence changed. Try again." A clip that failed in an automatic pass
isn't retried in a loop. The buttons retry.

### The lens and drag sensitivity

**DJI** is the effect's own default, so choosing it writes nothing.

**Classic** mirrors what the effect does when a user picks Classic in its own
popup (`EffectMain.cpp`, `USER_CHANGED_PARAM`):

* **Lens** = Classic;
* **Preset** = Custom (the effect keeps Classic beside Custom; see
  `ReframeParams.h`, `OSV_REFRAME_LENS_ITEMS`);
* the hidden **Camera Model** mirror = off.

A fresh instance's Classic FOV and Distortion already hold the Classic "Wide"
look that matches the DJI "Wide" default, so nothing else moves.

**Popup numbering.** Premiere numbers popups from 0 on its GPU side, while
After Effects numbers them from 1. Nothing documents which numbering the UXP
and ExtendScript parameter APIs use. So the panel reads the Lens of the
effect it has just applied: a fresh instance is on DJI, entry 1, so the reading
*is* the host's base. Then:

* Lens = base + 1 (Classic);
* Preset = base (Custom).

If the reading is anything but 0 or 1, the lens is left at DJI and the status
line says so. A wrong guess would pick the wrong lens silently.

Parameters are found by **display name** ("Lens", "Preset", "Camera Model",
"Drag Sensitivity"), never by index, because neither API promises to list the
effect's group markers. A keyframed parameter is never written. Drag
Sensitivity is clamped to the effect's 0.1-10 range and written only when it
differs from the effect's value.

## Which route, and why

The job has three parts, and the two platforms differ on each:

| | UXP (Premiere 25.6+) | CEP + ExtendScript (Premiere 22-26) |
|---|---|---|
| Add an effect | **Official**: `VideoFilterFactory.createComponent(matchName)` + `VideoComponentChain.createAppendComponentAction()`, committed by `Project.executeTransaction()` inside `Project.lockedAccess()` | **Unofficial**: the QE DOM (`qe.project.getVideoEffectByName` + `QETrackItem.addVideoEffect`). The official ExtendScript DOM has no call that adds an effect. |
| Notice a drop | **Official**, indirect: no "item added" event, but `Constants.VideoTrackEvent.TRACK_CHANGED` fires when "a clip is added to the track (drag from Project panel, paste, overwrite edit)" (Adobe's premiere-api sample) | **Official**: `app.bind('onActiveSequenceTrackItemAdded', ...)`, as Adobe's PProPanel sample does |
| Install without clicks | Only with Adobe's plug-in installer (UPIA). A UXP plug-in "must be installed via either double-click *or* UPIA in order to correctly update a database file". It can't be copied into a folder. | **Yes**: copy a folder and set one per-user registry string |
| Future | The platform Adobe is moving to | "The plan is to support both CEP and UXP for a calendar year, after which we will remove support for CEP" (Adobe, November 2025, with Premiere 25.6) |

**UXP is the better panel for Premiere 26.** Every call it makes is supported,
effect insertion is transactional and undoable, and it's the one that will
keep working. It's also the only route into the Premiere after CEP is gone.

**CEP is the one that installs itself on the machine it was written for.**
That machine runs Premiere Pro 26.2.2, which still bundles CEP 12
(`CEPHtmlEngine.exe` and `PlugPlug.dll` 12.0.1) and Adobe's own CEP panels. It
has no UPIA at the documented path. Its `.ccx` association belongs to a
third-party installer. So a UXP install there needs a confirmation click, and
a CEP install needs none.

Building both was cheap, because only the adapter differs:

* `shared/`: the rules, controller, view and styles, used by both.
* `uxp/uxpAdapter.js`: about 900 lines. `cep/cepAdapter.js` + `cep/host/host.jsx`:
  about 1,250 lines, most of it ES3 guards.

So the script installs whichever build needs no click (`Auto`), always builds
the `.ccx`, and switching later is one command: `-PanelFlavor Uxp`.

**What relies on something unofficial.** One call: QE's `addVideoEffect` in the
CEP build. Adobe staff call the QE DOM unsupported and note that it "has some
issues updating itself, in response to user interaction". The community
documents that a QE track lists the gaps between clips as items of type
"Empty", so its indices aren't the official DOM's. The host script works
around both:

* It finds the QE item by start time. It falls back to the n-th non-empty item
  only when the names agree.
* It never trusts the QE result: after every `addVideoEffect()` it re-reads the
  clip's components through the official DOM. The clip only counts as done if
  Open 360 Reframe is really there.

The UXP build relies on nothing unofficial.

### Developer mode is not needed

* **UXP.** Premiere's "Enable developer mode" preference is only needed for the
  UXP Developer Tool to load a plug-in from a folder, and that load doesn't
  survive a restart. A `.ccx` installed by UPIA or by double-click is a normal
  installation.
* **CEP.** `PlayerDebugMode` is the only switch, and the script handles it.

## How it's built

```
panel/
  shared/                 used by both builds
    osvcore.js            the pure rules (OSV paths, match names, diff, param plan, debounce, copy)
    controller.js         the auto-apply state machine: baseline, events, poll, buttons, settings
    view.js               the interface, built in script so both builds render the same thing
    spring.js             damped-spring physics for every animation
    panel.css             tokens per theme, flexbox layout, responsive rules
    boot.js               wires core + controller + view + an adapter
  uxp/                    Premiere 25.6+
    manifest.json         manifest v5, host premierepro >= 25.6.0, one panel entrypoint
    index.html, main.js   page + start-up (theme from document.theme / uxp.host)
    uxpAdapter.js         the UXP DOM calls
  cep/                    Premiere 22+ while CEP lasts
    CSXS/manifest.xml     PPRO [22.0,99.9], CSXS 10.0+, ScriptPath host/host.jsx
    index.html, main.js   page + start-up (talks to window.__adobe_cep__; no Adobe file shipped)
    cepAdapter.js         evalScript calls into host.jsx, JSON back, 15 s timeout
    host/host.jsx         ExtendScript (ES3): facts and actions, JSON text out, never throws
  tests/                  node:test suites, mocks of both Premiere DOMs, run.js
```

**The adapter contract** (`uxpAdapter.js` has the full comment):
`init(onEvent)`, `dispose()`, `getActiveSequence()`, `getSequenceIds()`,
`signature()`, `scan(seq, {selectedOnly})`, `apply(seq, items, settings)` and
`checkEffect()`. The controller is the only caller, and it runs everything on
one promise chain, so passes never overlap each other or a button press.

**Identity.** The effect's PiPL match name is `OpenOSV.Open360Reframe`, and
Premiere registers it as `AE.OpenOSV.Open360Reframe`, the name
`createComponent` and `getVideoEffectByName(name, true)` take. The display name
is "Open 360 Reframe". A test reads all of these from
`plugins/reframe/ReframeParams.h` and `EffectMain.cpp`, so a rename fails the
build instead of shipping a panel that never finds its effect.

### Design, after Apple's Human Interface Guidelines

* **One primary control.** The auto-apply switch sits alone in the first card,
  the way a Settings screen leads with its master switch. A pill in the header
  says **Watching** or **Off** at a glance.
* **Standard controls with their standard meanings:**
  * a switch for on/off;
  * a segmented control for DJI | Classic;
  * a slider with a live value for drag sensitivity, dimmed while it's off;
  * a filled button for the likely action and a tinted one for the broader one.
* **Inset grouped cards** with hairline separators and a small section title
  ("New effects start with"), on a 12-14 px rhythm. The typeface is SF on a
  Mac and Segoe UI on Windows.
* **Motion that explains state, on springs** (response 0.32 s, damping 0.78
  for controls; 0.45 s / 0.9 for text):
  * the switch knob stretches while pressed and slides with a touch of
    overshoot;
  * the segment selection glides;
  * buttons dim on press and spring back;
  * new status text rises into place.

  Every animation is a spring integrated in script (`spring.js`), because UXP's
  CSS has no transitions or transforms. If Chromium stops delivering frames to
  a hidden CEP panel, a timer finishes the motion.
* **Theme.** The panel follows Premiere's theme: `document.theme` in UXP, the
  host skin in CEP. It takes the exact panel grey, so it sits flush with its
  neighbours. Colours follow the system palette: green on, blue act, orange
  warn, red fail.
* **Responsive, in one column:**
  * a wide panel (520 px+) puts the buttons side by side;
  * a narrow one (under 280 px) drops the subtitle and gives the Lens control
    the full width under its label.
  * Switches stay beside their labels at every width.
  * Keyboard works throughout (Tab, Enter/Space, arrows on the slider, Shift
    for big steps), with a visible focus ring.

## Tests

`node panel/tests/run.js` (Node 18+, no npm packages) runs 125 tests. ctest
registers them as `panel.js` only when Node 18+ is found, so a machine without
Node still passes the suite.

| File | What it proves |
|---|---|
| `osvcore.test.js` | OSV detection on a corpus of paths, match names, settings repair, the new-versus-moved/trimmed/cut diff, the Classic plan on hosts counting from 0 and from 1, the debouncer on a fake clock, every status line |
| `spring.test.js` | springs settle, overshoot is bounded, a stalled frame can't make one jump, a throwing frame stops only its own spring |
| `controller.test.js` | baseline, new drops, storms, new versus existing sequences, off/on, the poll, both buttons, host errors, storage, project switch, stop() |
| `uxpAdapter.test.js` | the UXP route against a mock of the documented UXP DOM that enforces the `lockedAccess` rule; never doubles, refused transactions, the race check, track listeners following new tracks, and a full drop-to-effect run through the real controller |
| `hostjsx.test.js` | host.jsx in a mock ExtendScript + QE world: ES3-only source, QE items across gaps, the DOM double-check, name-checked writes, JSON escaping, never throwing |
| `cepAdapter.test.js` | the CEP route end to end: adapter, evalScript bridge, the real host.jsx and the mock DOM, including a drop-to-effect run |
| `view.test.js` | the interface in a fake DOM: every control reaches the controller, busy and error states, the status stamp, themes, springs landing on exact pixels |
| `identity.test.js` / `lint.test.js` | constants against the C++ sources and both manifests; no `?.` / `??` (CEP 10 is Chromium 74), no CSS UXP lacks, no raw U+2028 |

**Verified without launching Premiere:**

* the tests above;
* the interface rendered in headless Edge at 240, 320, 340 and 600 px, in dark
  and light;
* installer dry runs under Windows PowerShell 5.1 and PowerShell 7 (the CEP
  folder, the `.ccx` entries and their contents, uninstall);
* the `PlayerDebugMode` set and restore logic against a scratch `HKCU` key.

## Limits, and what needs a first live check

Each of these is documented but hasn't been observed in Premiere yet:

1. **UXP `TRACK_CHANGED` on a drop.** Adobe's sample documents it, and the
   2.5 s poll covers the case where it doesn't fire.
2. **UXP plug-in lifetime.** The controller starts when the plug-in loads, not
   when the panel is shown. Whether Premiere loads a UXP panel plug-in at
   launch while its panel is closed decides whether auto-apply runs before the
   panel is first opened. (Adobe notes that the `hide()`/`destroy()` panel
   hooks "are not working as expected yet" in Premiere.)
3. **Popup numbering in the parameter APIs.** Learned at run time (see above).
   The first Classic apply confirms it. If the status line says "Lens left at
   DJI", the host reported something other than 0 or 1.
4. **CEP `app.bind` exclusivity.** If `app.bind` keeps one handler per event,
   another panel binding the same event later silences ours. The poll still
   applies the effect, within 2.5 s.
5. **CEP panels run while they're open.** A docked tab in the background
   counts; a closed panel doesn't. Premiere doesn't start a visible CEP panel
   on its own (`StartOn` / `ApplicationActivate` is not sent at launch).
6. **UPIA's exit code.** Adobe documents the commands, not the exit codes.
   The script treats 0 as success and prints UPIA's own output either way.
7. **The Creative Cloud confirmation** for a `.ccx` that isn't from the
   Marketplace is expected, per Adobe's install guide.

## Sources

* Premiere UXP API reference (docs repo at 2026-09-21):
  * [VideoFilterFactory](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/videofilterfactory/) (`createComponent`, `getMatchNames`, since 25.6)
  * [VideoComponentChain](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/videocomponentchain/)
  * [Component](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/component/)
  * [ComponentParam](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/componentparam/)
  * [Project](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/project/) (`lockedAccess`: "project state will not change during the execution of callback function")
  * [EventManager](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/eventmanager/)
  * [Constants](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/constants/) (`VideoTrackEvent`, `SequenceEvent`, `ProjectEvent`, `TrackItemType`)
  * [VideoTrack](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/videotrack/)
  * [ClipProjectItem](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/clipprojectitem/)
* [Premiere UXP changelog](https://developer.adobe.com/premiere-pro/uxp/changelog/): official in 25.6.0; since 26.3, actions must be created inside `lockedAccess`.
* Adobe's UXP sample panel, premiere-api (repo at 2026-09-22):
  * [eventManager.ts](https://github.com/AdobeDocs/uxp-premiere-pro-samples/blob/main/sample-panels/premiere-api/src/eventManager.ts): what `TRACK_CHANGED` fires for, and the capture phase for project events.
  * [effects.ts](https://github.com/AdobeDocs/uxp-premiere-pro-samples/blob/main/sample-panels/premiere-api/src/effects.ts): `createComponent` + insert action inside `lockedAccess`/`executeTransaction`.
* UXP manifest, packaging and install:
  * [Manifest](https://developer.adobe.com/premiere-pro/uxp/plugins/concepts/manifest/): v5, one host per `.ccx`.
  * [Package](https://developer.adobe.com/premiere-pro/uxp/plugins/distribution/package/): "A `.ccx` file is a regular ZIP file under the hood", no signature.
  * [Install](https://developer.adobe.com/premiere-pro/uxp/plugins/distribution/install/): double-click with a warning dialog, or UPIA, at its documented path.
  * [Entrypoints](https://developer.adobe.com/premiere-pro/uxp/plugins/concepts/entrypoints/)
  * [CSS styling and theme](https://developer.adobe.com/premiere-pro/uxp/resources/recipes/css-styling/): "UXP is not a browser", no CSS grid, `document.theme`.
* Adobe Tech Blog, [How to install UXP plugins using command line tools](https://blog.developer.adobe.com/en/publish/2022/03/how-to-install-uxp-plugins-using-command-line-tools): UXP plug-ins can't be written into the folders directly.
* Adobe CEP samples:
  * [PProPanel ReadMe](https://github.com/Adobe-CEP/Samples/blob/master/PProPanel/ReadMe.md): CEP superseded as of 25.6, supported for a calendar year; `PlayerDebugMode` per CSXS version.
  * [PProPanel Premiere.jsx](https://github.com/Adobe-CEP/Samples/blob/master/PProPanel/jsx/PPRO/Premiere.jsx): `app.bind('onActiveSequenceTrackItemAdded' / 'onActiveSequenceStructureChanged' / 'onActiveSequenceChanged')`, CSXSEvent dispatch.
* [Premiere Pro Scripting Guide](https://ppro-scripting.docsforadobe.dev/): `app.bind`, `app.enableQE`, TrackItem, ComponentParam `setValue`.
* QE, unofficial:
  * Adobe forum, [ExtendScript: No longer able to add effects using QE DOM](https://community.adobe.com/t5/premiere-pro/extendscript-no-longer-able-to-add-effects-using-qe-dom/m-p/11353312) (Adobe's Bruce Bullis on QE reliability, and the `getVideoEffectByName` + `addVideoEffect` pattern).
  * Adobe forum, [How to find a trackitem with QE scripting](https://community.adobe.com/t5/premiere-pro-discussions/how-to-find-a-trackitem-with-qe-scripting/m-p/12455360): gaps are QE items.
  * [pymiere documentation](https://github.com/qmasingarbe/pymiere/blob/master/example_and_documentation.md): "QE list empty spaces as items".
  * [Vakago Tools, adding an effect with QE](https://vakago-tools.com/extendscript-tutorial-adding-effect-to-a-clip-in-premiere-pro/): `getVideoEffectByName(matchName, true)`.
* Hyper Brew, [UXP plugins in Premiere 2026](https://hyperbrew.co/blog/uxp-plugins-in-premiere-2026/): only Premiere 2026 has UXP; keep CEP builds for older versions.
