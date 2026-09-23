/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * osvcore.js - the pure half of the OpenOSV companion panel.
 *
 * Everything in this file is a plain function of its arguments: no Premiere
 * API, no DOM, no timers of its own.  That is what lets one copy of it run in
 * three very different places and behave the same in all of them:
 *
 *   1. the UXP panel (Premiere Pro 25.6+), loaded by a <script> tag;
 *   2. the CEP panel (Premiere Pro 22+), loaded by a <script> tag in CEF;
 *   3. Node, where panel/tests/*.test.js exercise every rule below.
 *
 * The things that decide whether the panel does the right thing to a user's
 * edit live here, so they are the things under test:
 *
 *   - is this media an OSV (or LRF) file?                  isOsvMediaPath()
 *   - does this clip already carry Open 360 Reframe?        hasReframeEffect()
 *   - which clips are NEW since the panel last looked?      diffItems()
 *   - which parameter writes give the lens the user chose?  planParamWrites()
 *   - collapsing an event storm into one pass               createDebouncer()
 *   - what the status line says                             summarize()
 *
 * Language level: ES2017 without optional chaining or nullish coalescing.
 * The oldest runtime this ships to is CEP 10 (Chromium 74, Premiere 22), which
 * has neither; a test scans the shared sources for them.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    // Node (the tests) and any CommonJS loader get it as a module...
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    // ...and the two panels get it as a global, because both load it with a
    // plain <script> tag.
    if (root) {
        root.OsvCore = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /* ======================================================================
     *  Identity - every string here mirrors plugins/reframe, and
     *  panel/tests/identity.test.js reads the C++ sources to prove it.
     * ====================================================================== */

    /** Version of the panel.  Kept equal to both manifests by a test. */
    var PANEL_VERSION = '1.0.0';

    /**
     * The PiPL match name of the reframe effect (ReframeParams.h,
     * OSV_REFRAME_MATCH_NAME).  Never changes.
     */
    var REFRAME_MATCH_NAME = 'OpenOSV.Open360Reframe';

    /**
     * What Premiere registers that effect as.  Every After Effects API effect
     * is registered with an "AE." prefix (ReframeParams.h, and the SDK's own
     * SDK_Segment_Utils.cpp comparing against "AE.ADBE Motion"), and that is
     * the name VideoFilterFactory.createComponent() and QE's
     * getVideoEffectByName(name, true) expect.
     */
    var REFRAME_HOST_MATCH_NAME = 'AE.' + REFRAME_MATCH_NAME;

    /** The Effects panel name (OSV_REFRAME_DISPLAY_NAME). */
    var REFRAME_DISPLAY_NAME = 'Open 360 Reframe';

    /** Media the OpenOSV importer registers: DJI's .OSV and its .LRF proxy. */
    var OSV_EXTENSIONS = ['osv', 'lrf'];

    /**
     * Display names of the effect parameters the panel may write, exactly as
     * EffectMain.cpp registers them.  Parameters are found by NAME, never by
     * index: Premiere's UXP and ExtendScript parameter lists do not promise to
     * include the effect's group markers, so an index would be a guess.
     */
    var PARAM_NAMES = Object.freeze({
        lens: 'Lens',
        preset: 'Preset',
        cameraModel: 'Camera Model',
        dragSensitivity: 'Drag Sensitivity',
        // [WP-EASING] The Keyframe Easing popup, and the camera controls the
        // Manual Framing card reads and writes.
        keyframeEasing: 'Keyframe Easing',
        outputResolution: 'Output Resolution',
        pan: 'Pan',
        tilt: 'Tilt',
        roll: 'Roll',
        fov: 'FOV',
        djiFov: 'DJI FOV',
        distortion: 'Distortion',
        zoom: 'Zoom',
        correction: 'Correction Angle'
    });

    /** Drag Sensitivity's ranges and default (OSV_REFRAME_DRAG_SENSITIVITY_*). */
    var DRAG = Object.freeze({
        validMin: 0.1,
        validMax: 10.0,
        sliderMin: 0.25,
        sliderMax: 5.0,
        defaultValue: 2.0
    });

    /** The two lenses, in the order of the effect's popup ("DJI|Classic"). */
    var LENS = Object.freeze({ dji: 'dji', classic: 'classic' });

    /**
     * The Keyframe Easing popup (ReframeParams.h, OSV_REFRAME_EASING_ITEMS):
     * DJI Studio's seven Keyframe Animation presets in DJI Studio's order.
     * `entry` is the 1-based popup entry; `label` is exactly the effect's
     * item text (a test compares the two lists); `lines` is how a tile of the
     * preset grid sets it on two short lines.
     */
    var EASINGS = Object.freeze([
        Object.freeze({ id: 'none', entry: 1, label: 'None', lines: ['None', ''] }),
        Object.freeze({ id: 'linear-smooth', entry: 2, label: 'Linear Smooth', lines: ['Linear', 'Smooth'] }),
        Object.freeze({ id: 'fast-in-slow-out', entry: 3, label: 'Fast In, Slow Out', lines: ['Fast In', 'Slow Out'] }),
        Object.freeze({ id: 'slow-in-fast-out', entry: 4, label: 'Slow In, Fast Out', lines: ['Slow In', 'Fast Out'] }),
        Object.freeze({ id: 'fast-in-fast-out', entry: 5, label: 'Fast In, Fast Out', lines: ['Fast In', 'Fast Out'] }),
        Object.freeze({ id: 'slow-in-slow-out', entry: 6, label: 'Slow In, Slow Out', lines: ['Slow In', 'Slow Out'] }),
        Object.freeze({ id: 'linear', entry: 7, label: 'Linear', lines: ['Linear', ''] })
    ]);

    /**
     * The Manual Framing presets: the effect's Preset popup entries that
     * write a look (ReframeParams.h, kPresetTable - a test reads the table
     * and compares every number).  `entry` is the 1-based popup entry.
     * `classicFov` / `distortion` are the Classic lens's numbers, `dji` the DJI
     * FOV for a landscape, a 9:16 and a 3:4 frame, `correction` DJI's
     * Correction Angle, `tilt` the Tilt the preset sets.
     */
    var FRAMING_PRESETS = Object.freeze([
        Object.freeze({ id: 'crystal-ball', entry: 2, label: 'Crystal Ball', classicFov: 240, distortion: 100, tilt: 0,
                        dji: Object.freeze({ landscape: 75, portrait916: 110, portrait34: 87 }), correction: 1.8 }),
        Object.freeze({ id: 'asteroid', entry: 3, label: 'Asteroid', classicFov: 300, distortion: 100, tilt: -90,
                        dji: Object.freeze({ landscape: 138, portrait916: 147, portrait34: 147 }), correction: 1.0 }),
        Object.freeze({ id: 'wide', entry: 4, label: 'Wide', classicFov: 120, distortion: 15, tilt: 0,
                        dji: Object.freeze({ landscape: 60, portrait916: 90, portrait34: 72 }), correction: 0.6 }),
        Object.freeze({ id: 'ultra-wide', entry: 5, label: 'Ultra Wide', classicFov: 150, distortion: 40, tilt: 0,
                        dji: Object.freeze({ landscape: 78, portrait916: 110, portrait34: 95 }), correction: 0.5 }),
        Object.freeze({ id: 'dewarping', entry: 6, label: 'Dewarp', classicFov: 95, distortion: 0, tilt: 0,
                        dji: Object.freeze({ landscape: 80, portrait916: 112, portrait34: 97 }), correction: 0.2 })
    ]);

    /** Preset popup entry of "Custom" (the one that writes nothing). */
    var PRESET_CUSTOM_ENTRY = 1;

    /**
     * DJI Studio's zoom path and limits (ReframeParams.h,
     * OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION and OSV_REFRAME_DJI_STUDIO_*):
     * one zoom step moves FOV by 130 x the Correction step.  `step` is the
     * Correction change of one press of the panel's zoom buttons (a 6.5
     * degree FOV change).
     */
    var DJI_ZOOM = Object.freeze({
        fovPerCorrection: 130,
        fovMin: 20,
        fovMax: 150,
        correctionMin: 0,
        correctionMax: 1,
        step: 0.05
    });

    /** The Classic FOV's valid range (OSV_REFRAME_FOV_VALID_*). */
    var CLASSIC_FOV = Object.freeze({ min: 10, max: 350 });

    /** Output Resolution popup entries that name a size (entry 1 is Match Sequence). */
    var RESOLUTIONS = Object.freeze([
        null,
        Object.freeze({ w: 3840, h: 2160 }),
        Object.freeze({ w: 2560, h: 1440 }),
        Object.freeze({ w: 1920, h: 1080 }),
        Object.freeze({ w: 1280, h: 720 })
    ]);

    /**
     * Stabilisation choices, as DJI Studio names them, and the OpenOSV
     * Source Settings "Stabilisation" entry each one sets
     * (SourceSettingsParams.h, OSV_SS_STAB_ITEMS "Off|Horizon Lock|Full|Smooth"):
     *
     *   Off               -> Off
     *   RockSteady        -> Smooth: the view follows the camera's heading and
     *                        removes the shake - DJI's RockSteady on a 360 clip
     *                        keeps turning with the rider; Full would lock the
     *                        view to the first frame's direction instead
     *   Horizon Leveling  -> Horizon Lock: yaw follows, pitch and roll level
     */
    var STABILIZATIONS = Object.freeze([
        Object.freeze({ id: 'off', label: 'Off', entry: 1 }),
        Object.freeze({ id: 'rocksteady', label: 'RockSteady', entry: 4 }),
        Object.freeze({ id: 'horizon', label: 'Horizon Leveling', entry: 2 })
    ]);

    /** The Source Settings effect (plugins/common/SourceSettingsIdentity.h). */
    var SOURCE_SETTINGS_MATCH_NAME = 'OpenOSV.SourceSettings';

    /** Display names inside the Source Settings effect the panel reads or writes. */
    var SOURCE_PARAM_NAMES = Object.freeze({
        stabilization: 'Stabilisation',
        colourOutput: 'Colour Output',
        outputSize: 'Output Size',
        calibration: 'Calibration',
        dlogmCurve: 'D-Log M Curve',
        renderDevice: 'Render Device'
    });

    /**
     * Entry counts of the Source Settings popups whose readings can settle
     * how the host numbers popups (learnPopupBase): a 0 anywhere, or a
     * popup's own count, is unambiguous.  D-Log M Curve's default is its LAST
     * entry, so an untouched effect settles a host that counts from 1.
     */
    var SOURCE_POPUP_COUNTS = Object.freeze({
        'Colour Output': 4,
        'Output Size': 4,
        'Stabilisation': 4,
        'Calibration': 4,
        'D-Log M Curve': 3,
        'Render Device': 4
    });

    /** Entry counts of the reframe effect's popups, for the same purpose. */
    var REFRAME_POPUP_COUNTS = Object.freeze({
        'Output Resolution': 5,
        'Preset': 6,
        'Lens': 2,
        'Keyframe Easing': 7
    });

    /* ======================================================================
     *  Small helpers
     * ====================================================================== */

    /** True for a string with at least one non-space character. */
    function isNonEmptyString(value) {
        return typeof value === 'string' && value.trim().length > 0;
    }

    /** A finite number, or the fallback. */
    function finiteOr(value, fallback) {
        var n = (typeof value === 'number') ? value : Number(value);
        return (typeof value !== 'boolean' && value !== null && value !== '' && isFinite(n)) ? n : fallback;
    }

    /** Clamp into [lo, hi]. */
    function clamp(value, lo, hi) {
        return value < lo ? lo : (value > hi ? hi : value);
    }

    /** "1 clip" / "3 clips". */
    function plural(count, singular, pluralForm) {
        var n = Math.max(0, Math.floor(finiteOr(count, 0)));
        return n + ' ' + (n === 1 ? singular : (pluralForm || singular + 's'));
    }

    /* ======================================================================
     *  Media detection
     * ====================================================================== */

    /**
     * True when a project item's media path names an .OSV or .LRF file.
     *
     * The rule is the importer's own: the extension of the FILE NAME,
     * case-insensitive ("CAM_0001.OSV", "clip.osv", "proxy.LRF").  Anything
     * that merely contains ".osv" elsewhere - a folder called "trip.osv\",
     * "clip.osv.mp4", an extensionless file - is not ours.  Both separators
     * are accepted, because Premiere hands back native Windows paths and a
     * project moved from a Mac can still carry forward slashes.
     *
     * @param {*} path  whatever the host returned; anything but a string is false.
     * @returns {boolean}
     */
    function isOsvMediaPath(path) {
        if (typeof path !== 'string') {
            return false;
        }
        // Hosts have been seen to hand back trailing NULs and whitespace.
        var p = path.replace(/\u0000+$/, '').trim();
        if (p.length === 0 || p.length > 32767) {
            return false;
        }
        // The file name is everything after the last separator.  A path that
        // ENDS in a separator names a folder, and yields an empty name here.
        var cut = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
        var name = p.substring(cut + 1);
        var dot = name.lastIndexOf('.');
        // No extension, a dot-file with no base name, or a trailing dot.
        if (dot <= 0 || dot === name.length - 1) {
            return false;
        }
        var ext = name.substring(dot + 1).toLowerCase();
        return OSV_EXTENSIONS.indexOf(ext) !== -1;
    }

    /* ======================================================================
     *  "Already has the effect"
     * ====================================================================== */

    /**
     * The match name without the "AE." registration prefix, trimmed.
     * Non-strings become ''.
     */
    function normaliseMatchName(name) {
        if (typeof name !== 'string') {
            return '';
        }
        var n = name.trim();
        return n.indexOf('AE.') === 0 ? n.substring(3) : n;
    }

    /** True when a component match name is Open 360 Reframe, prefixed or not. */
    function isReframeMatchName(name) {
        return normaliseMatchName(name) === REFRAME_MATCH_NAME;
    }

    /**
     * How many components in a chain are Open 360 Reframe.
     * @param {*} matchNames  array of match names; anything else counts as none.
     */
    function countReframeEffects(matchNames) {
        if (!Array.isArray(matchNames)) {
            return 0;
        }
        var count = 0;
        for (var i = 0; i < matchNames.length; i += 1) {
            if (isReframeMatchName(matchNames[i])) {
                count += 1;
            }
        }
        return count;
    }

    /** True when a component chain already carries the effect. */
    function hasReframeEffect(matchNames) {
        return countReframeEffects(matchNames) > 0;
    }

    /**
     * Pick the name to create the effect by from the host's own list of
     * video filter match names: the exact registered name when it is there,
     * any spelling that normalises to ours otherwise, null when the effect is
     * not installed at all.
     */
    function pickHostMatchName(available) {
        if (!Array.isArray(available)) {
            return null;
        }
        if (available.indexOf(REFRAME_HOST_MATCH_NAME) !== -1) {
            return REFRAME_HOST_MATCH_NAME;
        }
        for (var i = 0; i < available.length; i += 1) {
            if (isReframeMatchName(available[i])) {
                return available[i];
            }
        }
        return null;
    }

    /* ======================================================================
     *  Settings
     * ====================================================================== */

    /** Round Drag Sensitivity to the effect's hundredths and clamp it. */
    function clampDragSensitivity(value) {
        var n = finiteOr(value, DRAG.defaultValue);
        n = Math.round(n * 100) / 100;
        return clamp(n, DRAG.validMin, DRAG.validMax);
    }

    /**
     * The panel's settings, repaired: whatever localStorage held (another
     * version's shape, a hand edit, garbage) comes back as a valid object.
     *
     *   autoApply        - watch the timeline (default ON: it is the point)
     *   lens             - 'dji' (the effect's own default) or 'classic'
     *   dragEnabled      - write a Drag Sensitivity into new effects
     *   dragSensitivity  - the value to write, clamped to the effect's range
     *   easing           - the Keyframe Animation preset picked in the grid
     *                      (an EASINGS id; 'none' until the user picks one)
     *   stabilization    - the Stabilisation choice (a STABILIZATIONS id;
     *                      Horizon Leveling, the Source Settings default)
     *   hintOpen         - the Program Monitor controls card is expanded
     *                      (open for a new user, then as they left it)
     *   popupBase        - how this Premiere numbers effect popups through
     *                      the scripting APIs, once learned: 0, 1 or null
     */
    function sanitizeSettings(raw) {
        var s = (raw !== null && typeof raw === 'object') ? raw : {};
        return {
            autoApply: typeof s.autoApply === 'boolean' ? s.autoApply : true,
            lens: s.lens === LENS.classic ? LENS.classic : LENS.dji,
            dragEnabled: s.dragEnabled === true,
            dragSensitivity: clampDragSensitivity(s.dragSensitivity),
            easing: easingById(s.easing).id,
            stabilization: stabilizationById(s.stabilization).id,
            hintOpen: typeof s.hintOpen === 'boolean' ? s.hintOpen : true,
            popupBase: (s.popupBase === 0 || s.popupBase === 1) ? s.popupBase : null
        };
    }

    /** Parse stored settings text; never throws. */
    function parseSettings(text) {
        if (typeof text !== 'string' || text.length === 0 || text.length > 4096) {
            return sanitizeSettings(null);
        }
        try {
            return sanitizeSettings(JSON.parse(text));
        } catch (err) {
            return sanitizeSettings(null);
        }
    }

    /** Settings to storage text. */
    function serializeSettings(settings) {
        return JSON.stringify(sanitizeSettings(settings));
    }

    /* ======================================================================
     *  Timeline items and "what is new"
     *
     *  Both adapters describe an OSV clip on the timeline the same way:
     *
     *    key            identity of the track item.  CEP: the DOM nodeId,
     *                   which is stable.  UXP: the API exposes no id, so the
     *                   adapter builds "track:start:projectItem" - which moves
     *                   when the clip moves.  diffItems() is written so both
     *                   kinds of key give the same answers.
     *    trackIndex     0-based video track (V1 = 0)
     *    startTicks     sequence time of the first frame, ticks as a string
     *    endTicks       sequence time after the last frame
     *    inTicks        source time of the first frame (the in point)
     *    outTicks       source time of the out point
     *    projectItemId  the project item (master clip) the item plays
     *    name           the track item's name, for messages
     * ====================================================================== */

    /** The UXP adapter's composite key.  Also used by the tests. */
    function makeItemKey(trackIndex, startTicks, projectItemId) {
        return String(trackIndex) + ':' + String(startTicks) + ':' + String(projectItemId);
    }

    /** Ticks string to a number for ranking; NaN-safe. */
    function ticksToNumber(ticks) {
        var n = Number(ticks);
        return isFinite(n) ? n : 0;
    }

    /** Drop anything that is not a usable item description. */
    function cleanItems(list) {
        var out = [];
        if (!Array.isArray(list)) {
            return out;
        }
        for (var i = 0; i < list.length; i += 1) {
            var it = list[i];
            if (it !== null && typeof it === 'object' && isNonEmptyString(it.key)) {
                out.push(it);
            }
        }
        return out;
    }

    /**
     * Split the current OSV items of a sequence into the ones that are NEW
     * since `known` was taken and the ones that were there already.
     *
     * An item whose key was known is old.  An item with an unknown key is old
     * too when it is really an existing clip that changed:
     *
     *   moved / trimmed  - a known item VANISHED and this one plays the same
     *                      project item.  Same track preferred, nearest start
     *                      wins, each vanished item explains one newcomer.
     *   razor cut        - a surviving item plays the same project item on the
     *                      same track, ends exactly where this one starts, and
     *                      its out point is this one's in point (the source
     *                      continues across the cut).  Two separate drops of
     *                      one clip placed end to end are NOT continuous in
     *                      the source, so each drop still counts as new.
     *
     * That distinction is the whole reason auto-apply can respect a user who
     * deliberately removed the effect from a clip: moving or cutting that
     * clip afterwards does not make it "new", so it does not get the effect
     * back.
     *
     * @param {Array} known    items at the previous look (may be empty)
     * @param {Array} current  items now
     * @returns {{added: Array, carried: Array, vanished: Array}}
     */
    function diffItems(known, current) {
        var before = cleanItems(known);
        var now = cleanItems(current);

        // Key sets for the plain "same key" test.
        var beforeByKey = Object.create(null);
        for (var b = 0; b < before.length; b += 1) {
            beforeByKey[before[b].key] = before[b];
        }
        var nowKeys = Object.create(null);
        for (var n = 0; n < now.length; n += 1) {
            nowKeys[now[n].key] = true;
        }

        var carried = [];
        var appeared = [];
        for (var i = 0; i < now.length; i += 1) {
            if (beforeByKey[now[i].key]) {
                carried.push(now[i]);
            } else {
                appeared.push(now[i]);
            }
        }
        var vanished = [];
        for (var v = 0; v < before.length; v += 1) {
            if (!nowKeys[before[v].key]) {
                vanished.push(before[v]);
            }
        }

        // ---- moves and trims: pair newcomers with vanished items ----------
        // Every (newcomer, vanished) pair playing the same project item is a
        // candidate; the cheapest pairs are taken first, each item at most
        // once.  Cost: the distance between the starts, plus a penalty larger
        // than any distance when the tracks differ - so a same-track pairing
        // always wins over a cross-track one, whichever newcomer comes first.
        appeared.sort(function (x, y) { return ticksToNumber(x.startTicks) - ticksToNumber(y.startTicks); });
        var pairs = [];
        for (var a = 0; a < appeared.length; a += 1) {
            for (var c = 0; c < vanished.length; c += 1) {
                if (vanished[c].projectItemId !== appeared[a].projectItemId) {
                    continue;
                }
                var cost = Math.abs(ticksToNumber(vanished[c].startTicks) - ticksToNumber(appeared[a].startTicks));
                pairs.push({
                    a: a,
                    c: c,
                    crossTrack: vanished[c].trackIndex !== appeared[a].trackIndex,
                    cost: cost
                });
            }
        }
        pairs.sort(function (x, y) {
            if (x.crossTrack !== y.crossTrack) {
                return x.crossTrack ? 1 : -1;
            }
            return (x.cost - y.cost) || (x.a - y.a) || (x.c - y.c);
        });
        var consumed = [];
        var matched = [];
        for (var q = 0; q < pairs.length; q += 1) {
            if (!matched[pairs[q].a] && !consumed[pairs[q].c]) {
                matched[pairs[q].a] = true;
                consumed[pairs[q].c] = true;
            }
        }
        var stillNew = [];
        for (var m = 0; m < appeared.length; m += 1) {
            if (matched[m]) {
                carried.push(appeared[m]);
            } else {
                stillNew.push(appeared[m]);
            }
        }

        // ---- razor cuts: a newcomer continuing an existing item's source ---
        var added = [];
        for (var s = 0; s < stillNew.length; s += 1) {
            var candidate = stillNew[s];
            var isCut = false;
            for (var k = 0; k < now.length && !isCut; k += 1) {
                var other = now[k];
                if (other === candidate || other.projectItemId !== candidate.projectItemId ||
                    other.trackIndex !== candidate.trackIndex) {
                    continue;
                }
                // The halves touch in sequence time AND in source time.
                var leftOf = String(other.endTicks) === String(candidate.startTicks) &&
                             String(other.outTicks) === String(candidate.inTicks);
                var rightOf = String(candidate.endTicks) === String(other.startTicks) &&
                              String(candidate.outTicks) === String(other.inTicks);
                isCut = leftOf || rightOf;
            }
            if (isCut) {
                carried.push(candidate);
            } else {
                added.push(candidate);
            }
        }

        var gone = [];
        for (var g = 0; g < vanished.length; g += 1) {
            if (!consumed[g]) {
                gone.push(vanished[g]);
            }
        }
        return { added: added, carried: carried, vanished: gone };
    }

    /* ======================================================================
     *  Parameter writes for a freshly applied effect
     * ====================================================================== */

    /**
     * The numbering base of the host's popups, learned from the Lens popup of
     * a FRESH instance, which sits on its default "DJI" - entry 1.  A host
     * that reads it as 0 counts from 0, one that reads 1 counts from 1
     * (plugins/reframe/ReframeParams.h, decodeHostPopup: Premiere's GPU side
     * counts from 0, After Effects from 1).  Anything else - a string, 2, a
     * fraction - is not a fresh DJI popup and settles nothing.
     */
    function popupBaseFromDefault(value) {
        if (value === 0 || value === 1) {
            return value;
        }
        return null;
    }

    /** The "off" value of a checkbox in the type the host used for it. */
    function uncheckedLike(value) {
        if (typeof value === 'boolean') {
            return false;
        }
        if (typeof value === 'number') {
            return 0;
        }
        return null;
    }

    /**
     * Which parameters to write into an effect that was just applied, so it
     * starts with the lens and drag sensitivity the panel is set to.
     *
     * `params` is what the host reported for the new instance:
     *   [{ index, name, value, timeVarying }]
     *
     * DJI is the effect's own default, so choosing it writes nothing at all.
     * Classic mirrors what the effect itself does when a user picks Classic in
     * its popup (EffectMain.cpp, USER_CHANGED_PARAM): Lens = Classic, Preset =
     * Custom (the effect keeps Classic beside Custom so its GPU path can tell
     * how the host numbers popups), and the hidden Camera Model mirror = off.
     * A fresh instance's Classic FOV / Distortion already hold the Classic
     * "Wide" look that matches the DJI "Wide" default, so nothing else moves.
     *
     * Popups are written in the host's own numbering, learned from the Lens
     * default: DJI = base, Classic = base + 1, Custom (Preset entry 1) = base.
     * When that cannot be learned the lens is left at DJI and a note says so -
     * a wrong guess would select the wrong lens silently.
     *
     * A parameter that is keyframed (time-varying) is never written: that
     * would replace an animation with a constant.
     *
     * @returns {{writes: Array<{index:number,name:string,value:*}>, notes: string[]}}
     */
    function planParamWrites(params, settings) {
        var s = sanitizeSettings(settings);
        var writes = [];
        var notes = [];
        var list = Array.isArray(params) ? params : [];

        // First parameter with a given display name, or null.
        function find(name) {
            for (var i = 0; i < list.length; i += 1) {
                var p = list[i];
                if (p && typeof p === 'object' && typeof p.name === 'string' && p.name.trim() === name &&
                    typeof p.index === 'number' && isFinite(p.index) && p.index >= 0) {
                    return p;
                }
            }
            return null;
        }

        if (s.lens === LENS.classic) {
            var lens = find(PARAM_NAMES.lens);
            if (!lens) {
                notes.push('lens-missing');
            } else if (lens.timeVarying === true) {
                notes.push('lens-keyframed');
            } else {
                var base = popupBaseFromDefault(lens.value);
                if (base === null) {
                    notes.push('lens-unreadable');
                } else {
                    writes.push({ index: lens.index, name: lens.name, value: base + 1 });
                    var preset = find(PARAM_NAMES.preset);
                    if (preset && preset.timeVarying !== true && preset.value !== base) {
                        writes.push({ index: preset.index, name: preset.name, value: base });
                    }
                    var mirror = find(PARAM_NAMES.cameraModel);
                    if (mirror && mirror.timeVarying !== true) {
                        var off = uncheckedLike(mirror.value);
                        if (off !== null && mirror.value !== off) {
                            writes.push({ index: mirror.index, name: mirror.name, value: off });
                        }
                    }
                }
            }
        }

        if (s.dragEnabled) {
            var drag = find(PARAM_NAMES.dragSensitivity);
            var want = clampDragSensitivity(s.dragSensitivity);
            if (!drag) {
                notes.push('drag-missing');
            } else if (drag.timeVarying === true) {
                notes.push('drag-keyframed');
            } else if (typeof drag.value !== 'number' || Math.abs(drag.value - want) > 1e-9) {
                writes.push({ index: drag.index, name: drag.name, value: want });
            }
        }
        return { writes: writes, notes: notes };
    }

    /* ======================================================================
     *  Debounce
     * ====================================================================== */

    /**
     * Collapse a burst of calls into one, `waitMs` after the last - but never
     * later than `maxWaitMs` after the first, so a host that keeps firing
     * (a multi-clip drop, a slider being dragged) still gets served.
     *
     * Timers are injected so the tests drive a fake clock and the panels pass
     * the real one.  `fn` exceptions are swallowed and reported through
     * `onError`: a debounced callback runs on a timer, where a throw would
     * reach nobody.
     *
     * @returns {{trigger: function, flush: function, cancel: function, pending: function}}
     */
    function createDebouncer(fn, options, timers) {
        var opts = options || {};
        var wait = Math.max(0, finiteOr(opts.waitMs, 300));
        var maxWait = Math.max(wait, finiteOr(opts.maxWaitMs, 1500));
        var onError = typeof opts.onError === 'function' ? opts.onError : function () {};
        var t = timers || {};
        var setT = typeof t.setTimeout === 'function' ? t.setTimeout : setTimeout;
        var clearT = typeof t.clearTimeout === 'function' ? t.clearTimeout : clearTimeout;
        var now = typeof t.now === 'function' ? t.now : function () { return Date.now(); };

        var handle = null;
        var firstAt = -1;

        function fire() {
            handle = null;
            firstAt = -1;
            try {
                fn();
            } catch (err) {
                onError(err);
            }
        }

        return {
            /** Note one call; runs `fn` later. */
            trigger: function () {
                var at = now();
                if (firstAt < 0) {
                    firstAt = at;
                }
                if (handle !== null) {
                    clearT(handle);
                }
                // The wait from now, cut short so the burst never exceeds maxWait.
                var delay = Math.min(wait, Math.max(0, firstAt + maxWait - at));
                handle = setT(fire, delay);
            },
            /** Run now if a call is pending. */
            flush: function () {
                if (handle !== null) {
                    clearT(handle);
                    fire();
                }
            },
            /** Forget a pending call. */
            cancel: function () {
                if (handle !== null) {
                    clearT(handle);
                }
                handle = null;
                firstAt = -1;
            },
            /** True while a call is waiting. */
            pending: function () {
                return handle !== null;
            }
        };
    }

    /* ======================================================================
     *  Status line copy
     * ====================================================================== */

    /** Cap a host error message at a length the status line can hold. */
    function shortError(message) {
        var text = (typeof message === 'string') ? message : String(message);
        text = text.replace(/\s+/g, ' ').trim();
        if (text.length === 0) {
            return 'unknown error';
        }
        return text.length > 140 ? text.substring(0, 137) + '...' : text;
    }

    /**
     * What the status line says after a pass.
     *
     * @param {object} result   { applied, already, failed, notOsv, errors[], notes[], missingEffect }
     * @param {string} context  'auto' | 'selected' | 'all'
     * @returns {{tone: string, text: string, quiet: boolean}}
     *   tone  - 'ok' | 'info' | 'warn' | 'error'
     *   quiet - true when an automatic pass did nothing worth saying
     */
    function summarize(result, context) {
        var r = (result !== null && typeof result === 'object') ? result : {};
        var applied = Math.max(0, Math.floor(finiteOr(r.applied, 0)));
        var already = Math.max(0, Math.floor(finiteOr(r.already, 0)));
        var failed = Math.max(0, Math.floor(finiteOr(r.failed, 0)));
        var notOsv = Math.max(0, Math.floor(finiteOr(r.notOsv, 0)));
        var errors = Array.isArray(r.errors) ? r.errors : [];
        var notes = Array.isArray(r.notes) ? r.notes : [];
        var firstError = errors.length > 0 ? shortError(errors[0]) : null;

        if (r.missingEffect === true) {
            return {
                tone: 'error',
                text: 'Open 360 Reframe isn\'t in this Premiere. Install the plug-ins and restart it.',
                quiet: false
            };
        }

        var parts = [];
        var tone = 'ok';
        if (applied > 0) {
            parts.push('Applied to ' + plural(applied, 'clip') + '.');
            if (already > 0) {
                parts.push(plural(already, 'clip') + ' already had it.');
            }
        }
        if (failed > 0) {
            tone = applied > 0 ? 'warn' : 'error';
            parts.push((applied > 0 ? plural(failed, 'clip') + ' failed' : 'Couldn\'t apply') +
                       (firstError ? ': ' + firstError : '') + '.');
        }
        if (applied > 0 && notes.indexOf('lens-unreadable') !== -1) {
            tone = tone === 'ok' ? 'warn' : tone;
            parts.push('Lens left at DJI.');
        }
        if (applied > 0 && notes.indexOf('params-failed') !== -1) {
            tone = tone === 'ok' ? 'warn' : tone;
            parts.push('Its lens settings didn\'t stick.');
        }

        if (parts.length > 0) {
            return { tone: tone, text: parts.join(' '), quiet: false };
        }

        // Nothing applied and nothing failed.
        if (context === 'auto') {
            return { tone: 'info', text: '', quiet: true };
        }
        if (already > 0) {
            return {
                tone: 'info',
                text: 'Nothing to do. ' + plural(already, 'clip') + (already === 1 ? ' has' : ' have') + ' it already.',
                quiet: false
            };
        }
        if (context === 'selected') {
            return {
                tone: 'info',
                text: notOsv > 0 ? 'No OSV clips selected. ' + plural(notOsv, 'selected clip') +
                                   (notOsv === 1 ? ' isn\'t OSV.' : ' aren\'t OSV.')
                                 : 'Select an OSV clip on the timeline first.',
                quiet: false
            };
        }
        return { tone: 'info', text: 'No OSV clips in this sequence.', quiet: false };
    }

    /** "14:02" for a status time stamp. */
    function clockLabel(date) {
        var d = (date instanceof Date && isFinite(date.getTime())) ? date : new Date();
        var hh = d.getHours();
        var mm = d.getMinutes();
        return (hh < 10 ? '0' : '') + hh + ':' + (mm < 10 ? '0' : '') + mm;
    }

    /* ======================================================================
     *  [WP-EASING] Keyframe Animation presets
     * ====================================================================== */

    /** The EASINGS entry for an id; 'none' for anything unknown. */
    function easingById(id) {
        for (var i = 0; i < EASINGS.length; i += 1) {
            if (EASINGS[i].id === id) {
                return EASINGS[i];
            }
        }
        return EASINGS[0];
    }

    /** The STABILIZATIONS entry for an id; Horizon Leveling for anything unknown. */
    function stabilizationById(id) {
        for (var i = 0; i < STABILIZATIONS.length; i += 1) {
            if (STABILIZATIONS[i].id === id) {
                return STABILIZATIONS[i];
            }
        }
        return STABILIZATIONS[2];
    }

    /** The FRAMING_PRESETS entry for an id, or null. */
    function framingPresetById(id) {
        for (var i = 0; i < FRAMING_PRESETS.length; i += 1) {
            if (FRAMING_PRESETS[i].id === id) {
                return FRAMING_PRESETS[i];
            }
        }
        return null;
    }

    /**
     * A preset's SPEED at a fraction `u` of a keyframe interval, relative to
     * a straight line's (1) - the same polynomials as the effect's
     * ReframeEasing.cpp, so the grid's icons draw the curves the effect
     * renders.  `u` is clamped to [0, 1]; NaN reads as 0.
     */
    function easeSpeed(id, u) {
        var x = (typeof u === 'number' && u === u) ? clamp(u, 0, 1) : 0;
        var smooth = x * x * (3 - 2 * x);
        switch (id) {
            case 'slow-in-slow-out': return 6 * x * (1 - x);
            case 'fast-in-fast-out': return 2 - 6 * x * (1 - x);
            case 'fast-in-slow-out': return 2 * (1 - smooth);
            case 'slow-in-fast-out': return 2 * smooth;
            default: return 1;
        }
    }

    /**
     * The drawing of a preset's tile icon in a `w` x `h` box: the speed
     * profile as a polyline between two keyframe dots, the way DJI Studio's
     * preset grid draws its presets (drawn from our own curves).
     *
     *   { kind: 'curve', points: [[x, y], ...], dots: [[x, y], [x, y]] }
     *   { kind: 'none' }   - None: the view draws a crossed circle
     *
     * Speed 0 sits on the bottom margin and speed 2 on the top one; Linear
     * Smooth, whose speed is set by the neighbouring keyframes, is drawn as
     * a gentle wave through the straight line's level.
     */
    function easingIconPoints(id, w, h) {
        var width = finiteOr(w, 24);
        var height = finiteOr(h, 16);
        if (id === 'none') {
            return { kind: 'none' };
        }
        var pad = Math.max(2, Math.min(width, height) * 0.16);
        var left = pad;
        var right = width - pad;
        var top = pad;
        var bottom = height - pad;
        var points = [];
        var steps = 24;
        for (var i = 0; i <= steps; i += 1) {
            var u = i / steps;
            var speed = (id === 'linear-smooth') ? 1 + 0.35 * Math.sin(2 * Math.PI * u) : easeSpeed(id, u);
            var x = left + (right - left) * u;
            var y = bottom - (bottom - top) * (speed / 2);
            points.push([Math.round(x * 100) / 100, Math.round(y * 100) / 100]);
        }
        return { kind: 'curve', points: points, dots: [points[0], points[points.length - 1]] };
    }

    /* ======================================================================
     *  [WP-EASING] How the host numbers effect popups
     * ====================================================================== */

    /**
     * Learn a host's popup numbering from readings whose meaning is certain:
     * a popup that reads 0 can only be counting from 0, and one that reads
     * its own entry count can only be counting from 1 (the rule the effect's
     * GPU reader uses too - ReframeParams.h, decodeHostPopup).
     *
     * @param {Array<{value:*, count:number}>} readings
     * @param {0|1|null} known  what was learned before (kept unless contradicted)
     * @returns {0|1|null}
     */
    function learnPopupBase(readings, known) {
        var base = (known === 0 || known === 1) ? known : null;
        var list = Array.isArray(readings) ? readings : [];
        var sawZero = false;
        var sawCount = false;
        for (var i = 0; i < list.length; i += 1) {
            var r = list[i];
            if (!r || typeof r.value !== 'number' || Math.floor(r.value) !== r.value) {
                continue;
            }
            if (r.value === 0) {
                sawZero = true;
            } else if (typeof r.count === 'number' && r.count > 1 && r.value === r.count) {
                sawCount = true;
            }
        }
        if (sawZero && sawCount) {
            // Contradictory readings: nothing here can be trusted.
            return base;
        }
        if (sawZero) {
            return 0;
        }
        if (sawCount) {
            return 1;
        }
        return base;
    }

    /** A 1-based popup entry in a host's numbering. */
    function popupValue(entry, base) {
        return (base === 0 ? 0 : 1) + entry - 1;
    }

    /** A host popup reading as a 1-based entry (NaN when unreadable). */
    function popupEntry(raw, base) {
        if (typeof raw !== 'number' || !isFinite(raw) || (base !== 0 && base !== 1)) {
            return NaN;
        }
        return raw - base + 1;
    }

    /* ======================================================================
     *  [WP-EASING] Finding the effect's controls by name
     * ====================================================================== */

    /**
     * Where each control sits in a host's parameter list of Open 360 Reframe.
     *
     * `list` is [{index, name}] in the host's order.  Most controls have a
     * unique display name.  The two FOVs do not: the effect registers the DJI
     * lens's as "DJI FOV" but SHOWS it as "FOV" while the DJI lens is on
     * (ReframeParams.h, kLensControls), so a host may report either name.
     * They are told apart by their neighbours, which are fixed by the
     * effect's parameter order: Classic FOV is the "FOV" right before
     * Distortion, DJI FOV the "FOV" / "DJI FOV" between Zoom and Correction
     * Angle.  A control that cannot be placed is simply absent.
     *
     * @returns {object} role -> index (roles as in PARAM_NAMES)
     */
    function locateReframeParams(list) {
        var params = Array.isArray(list) ? list : [];
        var byName = {};
        var order = [];
        for (var i = 0; i < params.length; i += 1) {
            var p = params[i];
            if (!p || typeof p.name !== 'string' || typeof p.index !== 'number' || !isFinite(p.index)) {
                continue;
            }
            var name = p.name.trim();
            order.push({ index: p.index, name: name });
            if (!Object.prototype.hasOwnProperty.call(byName, name)) {
                byName[name] = p.index;
            }
        }
        var out = {};
        var unique = ['lens', 'preset', 'cameraModel', 'dragSensitivity', 'keyframeEasing', 'outputResolution', 'pan',
                      'tilt', 'roll', 'distortion', 'zoom', 'correction'];
        unique.forEach(function (role) {
            var n = PARAM_NAMES[role];
            if (Object.prototype.hasOwnProperty.call(byName, n)) {
                out[role] = byName[n];
            }
        });
        // Position in `order` of a host index.
        function at(index) {
            for (var k = 0; k < order.length; k += 1) {
                if (order[k].index === index) {
                    return k;
                }
            }
            return -1;
        }
        // Classic FOV: the nearest "FOV" before Distortion.
        if (out.distortion !== undefined) {
            for (var c = at(out.distortion) - 1; c >= 0; c -= 1) {
                if (order[c].name === PARAM_NAMES.fov) {
                    out.fov = order[c].index;
                    break;
                }
            }
        }
        // DJI FOV: the nearest "FOV" / "DJI FOV" before Correction Angle,
        // and after Zoom when the host lists Zoom.
        if (out.correction !== undefined) {
            var floor = out.zoom !== undefined ? at(out.zoom) : -1;
            for (var d = at(out.correction) - 1; d > floor; d -= 1) {
                if (order[d].name === PARAM_NAMES.djiFov || order[d].name === PARAM_NAMES.fov) {
                    if (order[d].index !== out.fov) {
                        out.djiFov = order[d].index;
                    }
                    break;
                }
            }
        }
        return out;
    }

    /* ======================================================================
     *  [WP-EASING] Manual Framing: DJI's camera numbers
     * ====================================================================== */

    /**
     * The shape (width / height) the camera frames for: the Output
     * Resolution entry's size, else the sequence's, else DJI Studio's 16:9
     * (ReframeCpu.cpp, framingAspect).
     */
    function framingAspect(resolutionEntry, seqWidth, seqHeight) {
        var fixed = (typeof resolutionEntry === 'number' && resolutionEntry >= 2 &&
                     resolutionEntry <= RESOLUTIONS.length) ? RESOLUTIONS[resolutionEntry - 1] : null;
        if (fixed) {
            return fixed.w / fixed.h;
        }
        var w = finiteOr(seqWidth, 0);
        var h = finiteOr(seqHeight, 0);
        if (w > 0 && h > 0) {
            return w / h;
        }
        return 16 / 9;
    }

    /** A preset's DJI FOV for a frame shape (the library's column rule). */
    function djiPresetFov(preset, aspect) {
        if (!preset || !preset.dji) {
            return NaN;
        }
        var a = finiteOr(aspect, 16 / 9);
        if (!(a > 0) || a >= 1) {
            return preset.dji.landscape;
        }
        // The nearer of 9:16 and 3:4, as a ratio: sqrt(0.5625 * 0.75).
        return a <= 0.649519052838329 ? preset.dji.portrait916 : preset.dji.portrait34;
    }

    /**
     * DJI Studio's Zoom read-out: the visible horizontal angle of a DJI lens
     * on a frame of shape `aspect` (osv::geom::djiZoomDeg, the same formula
     * and the same 0 for anything DJI would answer 0 for).
     */
    function djiZoomDeg(fovDeg, correction, aspect) {
        var fov = finiteOr(fovDeg, NaN);
        var d = finiteOr(correction, NaN);
        var a0 = finiteOr(aspect, NaN);
        if (!isFinite(fov) || !isFinite(d) || !isFinite(a0) || !(a0 > 0) || !(fov > 0) || d < 0) {
            return 0;
        }
        var halfV = 0.5 * Math.min(fov, 180) * Math.PI / 180;
        var a = Math.tan(halfV) * a0;
        if (!(Math.abs(a) >= 2.220446049250313e-16)) {
            return 0;
        }
        var complement = Math.atan(1 / a);
        var s = Math.sqrt(1 + 1 / (a * a));
        var q = clamp(d / s, -1, 1);
        var zoom = 360 - 2 * complement * 180 / Math.PI - 2 * Math.acos(q) * 180 / Math.PI;
        return isFinite(zoom) ? zoom : 0;
    }

    /**
     * One press of a zoom button on DJI's lens: DJI Studio's zoom path,
     * FOV += 130 x step and Correction += step, each clamped to DJI Studio's
     * limits widened to include where the lens started (so a Crystal Ball's
     * 1.8 is not snapped to 1.0) - the rule the effect's Zoom control and its
     * Ctrl + drag follow.  `direction` +1 widens (Zoom up), -1 narrows.
     */
    function djiZoomStep(fovDeg, correction, direction) {
        var dir = direction > 0 ? 1 : -1;
        var fov0 = finiteOr(fovDeg, 60);
        var cor0 = finiteOr(correction, 0.6);
        var step = DJI_ZOOM.step * dir;
        var fovMin = Math.min(DJI_ZOOM.fovMin, fov0);
        var fovMax = Math.max(DJI_ZOOM.fovMax, fov0);
        var corMax = Math.max(DJI_ZOOM.correctionMax, cor0);
        return {
            fov: Math.round(clamp(fov0 + DJI_ZOOM.fovPerCorrection * step, fovMin, fovMax) * 1000) / 1000,
            correction: Math.round(clamp(cor0 + step, DJI_ZOOM.correctionMin, corMax) * 10000) / 10000
        };
    }

    /** One zoom press on the Classic lens: the same FOV step, inside FOV's valid range. */
    function classicZoomStep(fovDeg, direction) {
        var dir = direction > 0 ? 1 : -1;
        var fov0 = finiteOr(fovDeg, 120);
        return Math.round(clamp(fov0 + DJI_ZOOM.fovPerCorrection * DJI_ZOOM.step * dir,
                                CLASSIC_FOV.min, CLASSIC_FOV.max) * 1000) / 1000;
    }

    /**
     * Every value a framing preset writes, for a frame of shape `aspect` -
     * what the effect itself writes when its Preset popup changes
     * (EffectMain.cpp, USER_CHANGED_PARAM): both lenses' numbers, Tilt, the
     * Zoom read-out, the DJI lens and the Preset entry.  The panel writes all
     * of them itself, because nothing documents that a scripted change of the
     * Preset popup reaches the effect's supervision.
     */
    function framingPresetValues(presetId, aspect) {
        var p = framingPresetById(presetId);
        if (!p) {
            return null;
        }
        var fov = djiPresetFov(p, aspect);
        return {
            presetEntry: p.entry,
            classicFov: p.classicFov,
            distortion: p.distortion,
            tilt: p.tilt,
            djiFov: fov,
            correction: p.correction,
            zoom: Math.round(djiZoomDeg(fov, p.correction, aspect) * 10) / 10
        };
    }

    /** A finite number, or undefined. */
    function numberOrUndefined(x) {
        return (typeof x === 'number' && isFinite(x)) ? x : undefined;
    }

    /**
     * The Manual Framing state of a clip from its controls' raw host values
     * at the playhead.
     *
     * `v` maps roles (as in PARAM_NAMES) to what the host read.  The lens is
     * the Lens popup when the numbering is known, else the hidden Camera
     * Model mirror the effect keeps in step with it (ticked = DJI), else
     * DJI, the effect's default.  Zoom is DJI Studio's read-out on the DJI
     * lens and the Classic FOV itself on the Classic one.
     */
    function framingState(v, base, seqWidth, seqHeight) {
        var values = (v !== null && typeof v === 'object') ? v : {};
        var lensEntry = popupEntry(values.lens, base);
        var mirror = values.cameraModel;
        var lens = lensEntry === 2 ? LENS.classic
            : (lensEntry === 1 ? LENS.dji
                : ((mirror === false || mirror === 0) ? LENS.classic : LENS.dji));
        var aspect = framingAspect(popupEntry(values.outputResolution, base), seqWidth, seqHeight);
        var state = {
            lens: lens,
            aspect: aspect,
            pan: numberOrUndefined(values.pan),
            tilt: numberOrUndefined(values.tilt),
            roll: numberOrUndefined(values.roll),
            fov: numberOrUndefined(values.fov),
            distortion: numberOrUndefined(values.distortion),
            djiFov: numberOrUndefined(values.djiFov),
            correction: numberOrUndefined(values.correction)
        };
        state.zoom = lens === LENS.dji
            ? ((state.djiFov !== undefined && state.correction !== undefined)
                ? djiZoomDeg(state.djiFov, state.correction, aspect) : undefined)
            : state.fov;
        return state;
    }

    /**
     * The writes one Manual Framing action makes, by role:
     *
     *   request { action: 'preset', preset: id }   every value the effect's own
     *                                              Preset writes, the DJI lens,
     *                                              the hidden mirror, the entry
     *   request { action: 'zoom', direction: +-1 } one zoom press: DJI's zoom
     *                                              path on the DJI lens, the same
     *                                              FOV step on Classic, and
     *                                              Preset -> Custom
     *
     * @param {object} state  framingState() of the clip
     * @param {0|1|null} base the host's popup numbering
     * @returns {{writes: Array<{role, value, kind}>, label: string, reason: string}}
     *   kind 'number' (keyed at the playhead when the control is keyframed),
     *   'popup' (value already in the host's numbering) or 'bool'.  A preset
     *   needs the numbering; without it `reason` is 'base-unknown' and nothing
     *   is written.  A zoom press without it skips only the Preset popup.
     */
    function planFramingWrites(request, state, base) {
        var req = (request !== null && typeof request === 'object') ? request : {};
        var s = (state !== null && typeof state === 'object') ? state : {};
        var known = base === 0 || base === 1;
        var writes = [];
        var num = function (role, value) {
            if (typeof value === 'number' && isFinite(value)) {
                writes.push({ role: role, value: value, kind: 'number' });
            }
        };
        if (req.action === 'preset') {
            var preset = framingPresetById(req.preset);
            var values = preset ? framingPresetValues(req.preset, s.aspect) : null;
            if (!values) {
                return { writes: [], label: '', reason: 'unknown' };
            }
            if (!known) {
                return { writes: [], label: preset.label, reason: 'base-unknown' };
            }
            num('fov', values.classicFov);
            num('distortion', values.distortion);
            num('tilt', values.tilt);
            num('djiFov', values.djiFov);
            num('correction', values.correction);
            num('zoom', values.zoom);
            writes.push({ role: 'lens', value: popupValue(1, base), kind: 'popup' });
            writes.push({ role: 'preset', value: popupValue(values.presetEntry, base), kind: 'popup' });
            writes.push({ role: 'cameraModel', value: true, kind: 'bool' });
            return { writes: writes, label: preset.label, reason: '' };
        }
        if (req.action === 'zoom') {
            var dir = Number(req.direction) > 0 ? 1 : -1;
            var label;
            if (s.lens === LENS.classic) {
                if (s.fov === undefined) {
                    return { writes: [], label: '', reason: 'unknown' };
                }
                var fov = classicZoomStep(s.fov, dir);
                num('fov', fov);
                label = 'FOV ' + formatReadout(fov, 1) + '°';
            } else {
                if (s.djiFov === undefined || s.correction === undefined) {
                    return { writes: [], label: '', reason: 'unknown' };
                }
                var next = djiZoomStep(s.djiFov, s.correction, dir);
                num('djiFov', next.fov);
                num('correction', next.correction);
                var zoom = djiZoomDeg(next.fov, next.correction, s.aspect);
                num('zoom', Math.round(zoom * 10) / 10);
                label = 'Zoom ' + formatReadout(zoom, 1) + '°';
            }
            // The look is no longer a preset: Custom, as the effect itself
            // does on a hand edit it supervises.
            if (known) {
                writes.push({ role: 'preset', value: popupValue(PRESET_CUSTOM_ENTRY, base), kind: 'popup' });
            }
            return { writes: writes, label: label, reason: '' };
        }
        return { writes: [], label: '', reason: 'unknown' };
    }

    /**
     * The time a clip's effect parameters are keyed in, for a sequence time:
     * the playhead's distance into the clip plus the clip's in point (effect
     * keyframes live on the clip's media time).  Ticks in and out as decimal
     * strings; BigInt keeps a long sequence exact where it exists.
     */
    function componentTicks(playheadTicks, startTicks, inTicks) {
        var parts = [playheadTicks, startTicks, inTicks].map(function (t) { return String(t === undefined ? '' : t); });
        for (var i = 0; i < parts.length; i += 1) {
            if (!/^-?\d+$/.test(parts[i])) {
                return '';
            }
        }
        if (typeof BigInt === 'function') {
            return String(BigInt(parts[0]) - BigInt(parts[1]) + BigInt(parts[2]));
        }
        return String(Number(parts[0]) - Number(parts[1]) + Number(parts[2]));
    }

    /** True when a sequence time falls inside a clip [start, end). */
    function ticksInside(playheadTicks, startTicks, endTicks) {
        var p = Number(playheadTicks);
        var s = Number(startTicks);
        var e = Number(endTicks);
        return isFinite(p) && isFinite(s) && isFinite(e) && p >= s && p < e;
    }

    /** A read-out: fixed decimals, or an em dash for a missing number. */
    function formatReadout(value, decimals) {
        if (typeof value !== 'number' || !isFinite(value)) {
            return '—';
        }
        var n = Math.max(0, Math.min(3, Math.floor(finiteOr(decimals, 1))));
        return value.toFixed(n);
    }

    /* ======================================================================
     *  [WP-EASING] Status lines for the new actions
     * ====================================================================== */

    /**
     * After Apply of a Keyframe Animation preset.
     *
     * @param {object} r        { updated, unchanged, missing, keyframed, failed, errors[], notes[], perClipUndo }
     * @param {string} label    the preset's name
     * @param {string} context  'selected' | 'all'
     * @param {number} notOsv   selected clips that are not OSV
     */
    function summarizeEasing(r, label, context, notOsv) {
        var res = (r !== null && typeof r === 'object') ? r : {};
        var updated = Math.max(0, Math.floor(finiteOr(res.updated, 0)));
        var unchanged = Math.max(0, Math.floor(finiteOr(res.unchanged, 0)));
        var missing = Math.max(0, Math.floor(finiteOr(res.missing, 0)));
        var failed = Math.max(0, Math.floor(finiteOr(res.failed, 0)));
        var errors = Array.isArray(res.errors) ? res.errors : [];
        var notes = Array.isArray(res.notes) ? res.notes : [];
        var parts = [];
        var tone = 'ok';
        if (notes.indexOf('base-unknown') !== -1) {
            return {
                tone: 'warn',
                text: 'Couldn\'t tell how this Premiere numbers effect menus. Apply Open 360 Reframe to any clip ' +
                      'with this panel once, then try again.'
            };
        }
        if (updated > 0) {
            parts.push(label + ' on ' + plural(updated, 'clip') + '.');
        }
        if (unchanged > 0) {
            parts.push(plural(unchanged, 'clip') + ' had it already.');
        }
        if (missing > 0) {
            tone = updated > 0 || unchanged > 0 ? 'warn' : 'info';
            parts.push(plural(missing, 'clip') + (missing === 1 ? ' has' : ' have') + ' no Open 360 Reframe; skipped.');
        }
        if (failed > 0) {
            tone = (updated > 0 || unchanged > 0) ? 'warn' : 'error';
            parts.push((updated > 0 ? plural(failed, 'clip') + ' failed' : 'Couldn\'t set it') +
                       (errors.length > 0 ? ': ' + shortError(errors[0]) : '') + '.');
        }
        if (updated > 1 && res.perClipUndo === true) {
            parts.push('Undo takes one Ctrl+Z per clip.');
        }
        if (parts.length > 0) {
            return { tone: tone, text: parts.join(' ') };
        }
        if (context === 'selected') {
            return {
                tone: 'info',
                text: notOsv > 0 ? 'No OSV clips selected. ' + plural(notOsv, 'selected clip') +
                                   (notOsv === 1 ? ' isn\'t OSV.' : ' aren\'t OSV.')
                                 : 'Select an OSV clip on the timeline first.'
            };
        }
        return { tone: 'info', text: 'No OSV clips in this sequence.' };
    }

    /**
     * After a Manual Framing action (a preset button or a zoom press).
     * @param {object} r { ok, reason, label, keyframed, perStepUndo }
     */
    function summarizeFraming(r) {
        var res = (r !== null && typeof r === 'object') ? r : {};
        if (res.ok === true) {
            var text = String(res.label || 'Framing set') + (res.keyframed === true ? ' - keyed at the playhead.' : '.');
            return { tone: 'ok', text: text };
        }
        var why = {
            'no-sequence': 'Open a sequence first.',
            'no-selection': 'Select one OSV clip on the timeline.',
            'many-selected': 'Select just one clip to frame it.',
            'not-osv': 'The selected clip isn\'t OSV.',
            'no-effect': 'The selected clip has no Open 360 Reframe.',
            'outside': 'Move the playhead over the selected clip.',
            'base-unknown': 'Couldn\'t tell how this Premiere numbers effect menus. Apply Open 360 Reframe with ' +
                            'this panel once, then try again.'
        }[res.reason];
        if (why) {
            return { tone: 'info', text: why };
        }
        return { tone: 'error', text: 'Couldn\'t set the framing' + (res.error ? ': ' + shortError(res.error) : '') + '.' };
    }

    /**
     * After the Stabilisation choice was applied.
     * @param {object} r { unsupported, updated, unchanged, missing, failed, errors[], notes[] }
     */
    function summarizeStabilization(r, label, notOsv) {
        var res = (r !== null && typeof r === 'object') ? r : {};
        if (res.unsupported === true) {
            return {
                tone: 'info',
                text: 'This Premiere can\'t reach Source Settings from a panel. Set Stabilisation in the master ' +
                      'clip\'s Source Settings instead.'
            };
        }
        if ((res.notes || []).indexOf('base-unknown') !== -1) {
            return summarizeEasing(res, label, 'selected', notOsv);
        }
        var updated = Math.max(0, Math.floor(finiteOr(res.updated, 0)));
        var unchanged = Math.max(0, Math.floor(finiteOr(res.unchanged, 0)));
        var missing = Math.max(0, Math.floor(finiteOr(res.missing, 0)));
        var failed = Math.max(0, Math.floor(finiteOr(res.failed, 0)));
        var errors = Array.isArray(res.errors) ? res.errors : [];
        var parts = [];
        var tone = 'ok';
        if (updated > 0) {
            parts.push(label + ' on ' + plural(updated, 'master clip') + '.');
        }
        if (unchanged > 0) {
            parts.push(plural(unchanged, 'master clip') + ' had it already.');
        }
        if (missing > 0) {
            tone = (updated > 0 || unchanged > 0) ? 'warn' : 'info';
            parts.push(plural(missing, 'master clip') + (missing === 1 ? ' shows' : ' show') +
                       ' no OpenOSV Source Settings to the panel.');
        }
        if (failed > 0) {
            tone = (updated > 0 || unchanged > 0) ? 'warn' : 'error';
            parts.push('Couldn\'t set ' + plural(failed, 'master clip') +
                       (errors.length > 0 ? ': ' + shortError(errors[0]) : '') + '.');
        }
        if (parts.length > 0) {
            return { tone: tone, text: parts.join(' ') };
        }
        return {
            tone: 'info',
            text: notOsv > 0 ? 'No OSV clips selected. ' + plural(notOsv, 'selected clip') +
                               (notOsv === 1 ? ' isn\'t OSV.' : ' aren\'t OSV.')
                             : 'Select an OSV clip on the timeline first.'
        };
    }

    /* ======================================================================
     *  ExtendScript literal
     * ====================================================================== */

    /**
     * A value as ExtendScript source text, for evalScript().
     *
     * JSON is a subset of ExtendScript's (ES3) object literal syntax with ONE
     * exception: JSON.stringify leaves U+2028 / U+2029 raw, and ES3 treats a
     * raw one inside a string literal as a line break - a syntax error that
     * would fail the whole call.  Both are escaped here.  A value JSON cannot
     * represent (a cycle, a BigInt) becomes null rather than throwing.
     */
    function toExtendScriptLiteral(value) {
        var text;
        try {
            text = JSON.stringify(value === undefined ? null : value);
        } catch (err) {
            return 'null';
        }
        if (typeof text !== 'string') {
            return 'null';
        }
        // Built from escaped source text so the two separators never appear
        // raw in this file (a raw one would itself be a line break to ES3).
        var lineSeparator = new RegExp('\\u2028', 'g');
        var paragraphSeparator = new RegExp('\\u2029', 'g');
        return text.replace(lineSeparator, '\\u2028').replace(paragraphSeparator, '\\u2029');
    }

    /* ======================================================================
     *  Public surface
     * ====================================================================== */
    return Object.freeze({
        PANEL_VERSION: PANEL_VERSION,
        REFRAME_MATCH_NAME: REFRAME_MATCH_NAME,
        REFRAME_HOST_MATCH_NAME: REFRAME_HOST_MATCH_NAME,
        REFRAME_DISPLAY_NAME: REFRAME_DISPLAY_NAME,
        OSV_EXTENSIONS: Object.freeze(OSV_EXTENSIONS.slice()),
        PARAM_NAMES: PARAM_NAMES,
        DRAG: DRAG,
        LENS: LENS,
        isOsvMediaPath: isOsvMediaPath,
        normaliseMatchName: normaliseMatchName,
        isReframeMatchName: isReframeMatchName,
        countReframeEffects: countReframeEffects,
        hasReframeEffect: hasReframeEffect,
        pickHostMatchName: pickHostMatchName,
        clampDragSensitivity: clampDragSensitivity,
        sanitizeSettings: sanitizeSettings,
        parseSettings: parseSettings,
        serializeSettings: serializeSettings,
        makeItemKey: makeItemKey,
        diffItems: diffItems,
        popupBaseFromDefault: popupBaseFromDefault,
        planParamWrites: planParamWrites,
        createDebouncer: createDebouncer,
        shortError: shortError,
        summarize: summarize,
        clockLabel: clockLabel,
        toExtendScriptLiteral: toExtendScriptLiteral,
        plural: plural,
        // [WP-EASING] Keyframe Animation, Manual Framing and Stabilisation.
        EASINGS: EASINGS,
        FRAMING_PRESETS: FRAMING_PRESETS,
        PRESET_CUSTOM_ENTRY: PRESET_CUSTOM_ENTRY,
        DJI_ZOOM: DJI_ZOOM,
        CLASSIC_FOV: CLASSIC_FOV,
        STABILIZATIONS: STABILIZATIONS,
        SOURCE_SETTINGS_MATCH_NAME: SOURCE_SETTINGS_MATCH_NAME,
        SOURCE_PARAM_NAMES: SOURCE_PARAM_NAMES,
        SOURCE_POPUP_COUNTS: SOURCE_POPUP_COUNTS,
        REFRAME_POPUP_COUNTS: REFRAME_POPUP_COUNTS,
        easingById: easingById,
        stabilizationById: stabilizationById,
        framingPresetById: framingPresetById,
        easeSpeed: easeSpeed,
        easingIconPoints: easingIconPoints,
        learnPopupBase: learnPopupBase,
        popupValue: popupValue,
        popupEntry: popupEntry,
        locateReframeParams: locateReframeParams,
        framingAspect: framingAspect,
        djiPresetFov: djiPresetFov,
        djiZoomDeg: djiZoomDeg,
        djiZoomStep: djiZoomStep,
        classicZoomStep: classicZoomStep,
        framingPresetValues: framingPresetValues,
        framingState: framingState,
        planFramingWrites: planFramingWrites,
        componentTicks: componentTicks,
        ticksInside: ticksInside,
        formatReadout: formatReadout,
        summarizeEasing: summarizeEasing,
        summarizeFraming: summarizeFraming,
        summarizeStabilization: summarizeStabilization
    });
}));
