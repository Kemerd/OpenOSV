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
        dragSensitivity: 'Drag Sensitivity'
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
     */
    function sanitizeSettings(raw) {
        var s = (raw !== null && typeof raw === 'object') ? raw : {};
        return {
            autoApply: typeof s.autoApply === 'boolean' ? s.autoApply : true,
            lens: s.lens === LENS.classic ? LENS.classic : LENS.dji,
            dragEnabled: s.dragEnabled === true,
            dragSensitivity: clampDragSensitivity(s.dragSensitivity)
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
        plural: plural
    });
}));
