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
* **Program Monitor controls**: every gesture of the effect's overlay, one
  line each, open for a new user and folded away once you've read it.
* **Manual Framing**: DJI Studio's five looks, a Zoom stepper along DJI
  Studio's zoom path, and live FOV / Correction / Pan / Tilt / Roll of the
  selected clip at the playhead.
* **Keyframe Animation**: DJI Studio's seven easing presets, for the selected
  clips or every OSV clip of the sequence.
* **Stabilisation**: DJI Studio's two independent switches, RockSteady and
  Horizon Leveling, both on by default, set on the master clips' OpenOSV
  Source Settings.
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
active sequence changed. Try again." (from a button). If you switch sequences
in the middle of an automatic pass, the panel says nothing: the switch starts
the next pass itself. A clip that failed in an automatic pass isn't retried in
a loop. The buttons retry.

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

## Framing, easing and stabilisation

Four cards bring DJI Studio's per-clip tools into Premiere: the overlay's
controls near the top, then Manual Framing, Keyframe Animation and
Stabilisation under the apply buttons. They work on clips, not through Effect
Controls, so Keyframe Animation and Stabilisation handle twenty clips as
easily as one.

### Program Monitor controls

A short card right under the auto-apply switch lists what the effect's overlay
does, one line per gesture, each with a key-cap label:

| Keys | What it does |
|---|---|
| `Drag` | Pan and tilt. Left-right pans, up-down tilts. |
| `Shift` + drag | Lock to one axis. |
| `Ctrl` + drag | Zoom. Down widens. On the DJI lens, along DJI Studio's zoom path. |
| `Alt` + drag | Roll. So does dragging the ring. |
| `Corner` | Drag a corner grip up or down to zoom. |

and ends with the one thing everybody trips on: the overlay only shows while
Open 360 Reframe is selected in Effect Controls. Every line is taken from the
overlay's own code (`ReframeUi.cpp`, `resolveDragMode` and `applyDrag`).

The card is **open the first time the panel opens** and remembers being folded
from then on (`hintOpen` in the stored settings). Folding springs the height
and fades the lines; the chevron turns with it.

**What the panel doesn't do, and why:**

* **Scroll-wheel zoom.** The effect SDK has no mouse-wheel event (see
  `PREMIERE.md`, "Program Monitor overlay"), and the only other route, a
  global Windows mouse hook inside Premiere's process, is fragile for what it
  buys. `Ctrl` + drag zooms along the same path. There are no hotkeys either.
* **Selecting the effect for you.** A toggle that selects Open 360 Reframe in
  Effect Controls whenever an OSV clip is selected would save a click, but
  neither API can select a component. UXP's `Component` offers
  `getDisplayName`, `getMatchName`, `getParam` and `getParamCount`;
  ExtendScript's has `displayName`, `matchName` and `properties`. The QE DOM
  is undocumented. So there is no toggle.

### Manual Framing

The card follows the **one selected OSV clip** at the playhead: it re-reads on
Premiere's selection event (`SequenceEvent.SELECTION_CHANGED` in UXP,
`onActiveSequenceSelectionChanged` in CEP) and once a second while the panel
runs, and pauses while an action runs. With nothing to frame it says why (no
sequence, no selection, more than one clip, not OSV, no effect, playhead
outside the clip) and dims its buttons.

* **Read-outs**: FOV, Correction (Distortion on Classic), Pan, Tilt and Roll,
  the host's values at the playhead. The Zoom figure is DJI Studio's read-out
  on the DJI lens (the effect's own `djiZoomDeg`) and the FOV on Classic.
* **Crystal Ball, Asteroid, Ultra Wide, Wide, Dewarp**: write exactly what the
  effect writes when its own Preset popup changes (`EffectMain.cpp`,
  `USER_CHANGED_PARAM`): both lenses' numbers for the sequence's shape, Tilt,
  the Zoom read-out, Lens = DJI and the Preset entry. The panel writes them all
  because nothing documents that a scripted Preset change reaches the
  effect's supervision.
* **Zoom -/+**: one press is DJI Studio's zoom path, FOV +/- 6.5 degrees and
  Correction +/- 0.05, clamped to DJI Studio's limits widened to wherever the
  lens started (a Crystal Ball's 1.8 isn't snapped to 1.0). On Classic the
  same 6.5 degree FOV step. Preset becomes Custom, as it does for a hand edit.
* **Keyframes**: a control that is already keyframed gets a keyframe at the
  playhead (added, or updated if one is there); any other control is set.
  The status line says "keyed at the playhead" when that happened. Effect
  keyframes live on the clip's media time, so the panel keys at
  `playhead - clip start + in point`.

### Keyframe Animation

Seven tiles, DJI Studio's presets in DJI Studio's order: None, Linear Smooth,
Fast In / Slow Out, Slow In / Fast Out, Fast In / Fast Out, Slow In /
Slow Out, Linear. Each tile draws its speed profile between two keyframe dots,
from the same polynomials the effect renders (`osvcore.js` `easeSpeed` and
`ReframeEasing.cpp`, held together by a test); None is a crossed circle. Pick
one, then:

* **Apply to selected clips** or **Apply to all OSV clips in this sequence**
  set the Keyframe Easing popup of every Open 360 Reframe on those clips.
  Clips without the effect are skipped and counted ("2 clips have no Open 360
  Reframe; skipped."), and clips that already had the preset are counted too.

What the presets do, and which are exact, is in `PREMIERE.md`, "Keyframe
Easing (id 22)".

### Stabilisation

Two independent switches, exactly as DJI Studio has them: **RockSteady** and
**Horizon Leveling**, both on by default, each on its own row with a line of
what it does. They are applied to the **master clips** of the selected OSV
clips, because stabilisation lives in the importer's Source Settings, not on
the timeline. Together the pair picks one entry of the Source Settings
"Stabilisation" popup, and the caption under the switches names it
("Apply sets Stabilisation to Smooth + Horizon Lock on each selected clip's
master clip"):

| RockSteady | Horizon Leveling | Source Settings entry | What the view does |
|---|---|---|---|
| off | off | 1 Off | follows the camera |
| off | on | 2 Horizon Lock | heading follows the camera, pitch and roll level |
| on | off | 4 Smooth | the shake is gone, the view keeps turning with the camera's heading (as RockSteady does on a 360 clip; Full would lock it to the first frame's direction) |
| on | on | 5 Smooth + Horizon Lock | the smoothed heading with a level horizon (the Source Settings default) |

**Full (entry 3)** is the one entry no pair of switches spells. The card
never reads a clip's current entry - the switches are the choice Apply
writes, remembered between sessions - so a master clip set to Full in Source
Settings keeps it until Apply is pressed on it. Apply then writes the
switches' entry like any other, and the status line says so: "RockSteady +
Horizon Leveling on 2 master clips. Full replaced on 1 master clip."

**Remembered.** The switches are stored as two booleans. A panel that
remembered the older single choice starts from its switches: Off as both off,
RockSteady as RockSteady alone, and Horizon Leveling - the old default, which
every untouched card had stored - as the new default, both on.

**How it reaches Source Settings.** The Source Settings effect is a master
clip's effect, so the panel looks for `OpenOSV.SourceSettings` in the master
clip's own video effects: `ClipProjectItem.getComponentChain(MediaType.VIDEO)`
in UXP, `ProjectItem.videoComponents()` in ExtendScript ("Video components
for the 'Master Clip'"). It writes the Stabilisation popup there, never
anything else. A Premiere whose API has neither call gets a card that says
so, and Source Settings stay one right-click away.

### Popup numbering, again

Every new action writes a popup (Keyframe Easing, Preset, Lens,
Stabilisation), so the panel keeps what it learned about the host's popup
numbering between sessions (`popupBase`). It learns only from readings that
settle it: a 0 anywhere, or a popup reading its own entry count. An untouched
effect on a host that counts from 1 settles nothing, so UXP briefly makes a
fresh effect component (never added to a clip) and reads its Lens. If the
numbering is still unknown the action writes nothing and the status line says
to apply the effect with the panel once.

### Undo, per route

| Action | UXP | CEP |
|---|---|---|
| Keyframe Animation, any number of clips | **one** step, "Set Open 360 Reframe keyframe easing" | one step per effect changed |
| A Manual Framing preset | **one** step, "Frame Open 360 Reframe" | one step per value written (9 at most), one more for each new keyframe |
| A Zoom press | **one** step, "Frame Open 360 Reframe" | one step per value written (2 to 4), one more for each new keyframe |
| Stabilisation, any number of clips | **one** step, "Set OpenOSV stabilisation" | one step per master clip changed |

**UXP** builds every write of a press as actions (`createSetValueAction`,
or `createAddKeyframeAction` for a keyframed control) and commits them in one
`Project.executeTransaction()` ("Execute undoable transaction by passing
compound action") inside `lockedAccess()`. One Ctrl+Z undoes the press.

**CEP** has no undo grouping to use: the Scripting Guide's Application,
Project and ComponentParam pages document none, so each `setValue` /
`setValueAtKey` is expected to be its own History step (limit 11 below). The
Keyframe Animation card says
"This Premiere undoes it one clip at a time", and a multi-clip apply adds
"Undo takes one Ctrl+Z per clip." to the status line.

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
`checkEffect()`, plus `capabilities()` (`{undoGroups, stabilization}`),
`setEasing(seq, items, {entry, popupBase})`, `readFraming(seq, {popupBase})`,
`writeFraming(seq, request)` and `setStabilization(seq, items, {entry,
popupBase})` (which also reports `replacedFull`, the master clips it took off
Full) for the four cards. The controller is the only caller, and it
runs everything on one promise chain, so passes never overlap each other or a
button press. An adapter without one of the newer calls turns a press into a
status line, never an exception.

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
  * a filled button for the likely action and a tinted one for the broader one;
  * a disclosure card with a turning chevron for the controls list, key caps
    for its keys;
  * a grid of picture tiles for the easing presets (a radio group; Tab to a
    tile, Enter or Space picks it), chips for the framing looks, a stepper
    for Zoom, and small read-outs whose Zoom figure glides to a new value.
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

`node panel/tests/run.js` (Node 18+, no npm packages) runs 169 tests. ctest
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
| `easing.test.js` | the four cards: the tile curves against the effect's polynomials, popup numbering learned only from settling readings, the two FOVs told apart by their neighbours, DJI Studio's zoom path and preset values, component time; on UXP one undo step per press, keyframes at the playhead, a fresh effect asked for the numbering, Stabilisation through the master clip's chain (entry 5 for both switches, a clip on Full reported when Apply replaces it); on CEP `addKey` + `setValueAtKey` and `videoComponents()`; the controller's read-out poll, selection event and remembered choices, all four pairs of Stabilisation switches and the older single choice carried over |
| `view.test.js` | the interface in a fake DOM: every control reaches the controller, busy and error states, the status stamp, themes, springs landing on exact pixels, the controls card open for a new user, the preset grid, the framing read-outs and their reasons, the CEP undo note, the two Stabilisation switches and the entry their caption names |
| `identity.test.js` / `lint.test.js` | constants against the C++ sources and both manifests (the easing popup's entries, DJI Studio's preset table, the zoom path, the Source Settings popups); no `?.` / `??` (CEP 10 is Chromium 74), no CSS UXP lacks, no raw U+2028 |

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
8. **Source Settings in the master clip's effects.** The Stabilisation card
   relies on Premiere listing the importer's Source Settings effect in
   `ClipProjectItem.getComponentChain(MediaType.VIDEO)` (UXP) and
   `ProjectItem.videoComponents()` (ExtendScript), and on a scripted change
   reaching the importer the way a change in the Source Settings dialog does.
   If Premiere keeps it elsewhere, the status line says the master clip
   "shows no OpenOSV Source Settings to the panel" and nothing is written.
9. **Component time.** Keyframes are placed at
   `playhead - clip start + in point`, the clip's media time. That ignores
   clip speed, so on a sped-up or slowed clip a keyframe may land off the
   playhead.
10. **Inline SVG in UXP.** The tile icons and chevrons are SVG made with
    `createElementNS`. Without it a tile shows a text glyph; if UXP has the
    call but draws nothing, the tiles still carry their names.
11. **CEP undo.** Measured against the Scripting Guide, not a live History
    panel: each `setValue` / `setValueAtKey` is expected to be its own step.

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
  * [ClipProjectItem](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/clipprojectitem/) (`getComponentChain(mediaType)`: the master clip's effects)
  * [Sequence](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/sequence/) (`getPlayerPosition`, `getFrameSize`)
  * [Keyframe](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/keyframe/) (`position`) and [TickTime](https://developer.adobe.com/premiere-pro/uxp/ppro-reference/classes/ticktime/) (`createWithTicks`)
  * `Project.executeTransaction`: "Execute undoable transaction by passing compound action"
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
  * [ComponentParam](https://ppro-scripting.docsforadobe.dev/sequence/componentparam/): `addKey`, `setValueAtKey`, `findNearestKey`, `getValueAtTime`, `isTimeVarying`; no undo grouping.
  * [Component](https://ppro-scripting.docsforadobe.dev/sequence/component/): `displayName`, `matchName`, `properties`; nothing selects one.
  * [ProjectItem.videoComponents()](https://ppro-scripting.docsforadobe.dev/item/projectitem/#projectitemvideocomponents): "Video components for the 'Master Clip'".
  * [Application](https://ppro-scripting.docsforadobe.dev/application/application/): no undo grouping.
* Adobe's PProPanel sample binds `onActiveSequenceSelectionChanged`, which the CEP build uses to follow the selection.
* QE, unofficial:
  * Adobe forum, [ExtendScript: No longer able to add effects using QE DOM](https://community.adobe.com/t5/premiere-pro/extendscript-no-longer-able-to-add-effects-using-qe-dom/m-p/11353312) (Adobe's Bruce Bullis on QE reliability, and the `getVideoEffectByName` + `addVideoEffect` pattern).
  * Adobe forum, [How to find a trackitem with QE scripting](https://community.adobe.com/t5/premiere-pro-discussions/how-to-find-a-trackitem-with-qe-scripting/m-p/12455360): gaps are QE items.
  * [pymiere documentation](https://github.com/qmasingarbe/pymiere/blob/master/example_and_documentation.md): "QE list empty spaces as items".
  * [Vakago Tools, adding an effect with QE](https://vakago-tools.com/extendscript-tutorial-adding-effect-to-a-clip-in-premiere-pro/): `getVideoEffectByName(matchName, true)`.
* Hyper Brew, [UXP plugins in Premiere 2026](https://hyperbrew.co/blog/uxp-plugins-in-premiere-2026/): only Premiere 2026 has UXP; keep CEP builds for older versions.
