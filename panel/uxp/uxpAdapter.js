/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * uxpAdapter.js - the panel's link to Premiere Pro through UXP (25.6+).
 *
 * Every call here is a documented, supported Premiere UXP API
 * (developer.adobe.com/premiere-pro/uxp/ppro-reference; docs/PANEL.md lists
 * each one with its "Since" version).  Nothing unofficial is needed on this
 * route:
 *
 *   effect insertion   VideoFilterFactory.createComponent(matchName)
 *                      + VideoComponentChain.createAppendComponentAction()
 *                      inside Project.lockedAccess() + executeTransaction()
 *   "already has it"   VideoComponentChain.getComponentAtIndex(i).getMatchName()
 *   parameters         Component.getParam(i).displayName / getStartValue() /
 *                      createKeyframe() / createSetValueAction()
 *   media path         ClipProjectItem.cast(projectItem).getMediaFilePath()
 *   clips added        EventManager + Constants.VideoTrackEvent.TRACK_CHANGED
 *                      (fires when "a clip is added to the track (drag from
 *                      Project panel, paste, overwrite edit)", per Adobe's
 *                      premiere-api sample), plus sequence / project events
 *
 * The `premierepro` module is INJECTED rather than required here, so
 * panel/tests/uxpAdapter.test.js can run this file under Node against a mock
 * that follows the same documented shapes.
 *
 * Adapter contract (shared with cep/cepAdapter.js; controller.js is the only
 * caller):
 *   init(onEvent)            attach to host events; throws if there is no host
 *   dispose()                detach
 *   getActiveSequence()      -> {id, name, projectId} | null
 *   getSequenceIds()         -> string[] for the active project
 *   signature()              -> string that changes when clips are added
 *   scan(seq, {selectedOnly})-> {items: OSV items, otherCount}
 *   apply(seq, items, settings) -> {applied, already, failed, errors[], notes[], missingEffect}
 *   checkEffect()            -> {available: true | false | null}
 *   capabilities()           -> {undoGroups, stabilization}
 *   setEasing(seq, items, {entry, popupBase})
 *                            -> {updated, unchanged, missing, failed, errors[], notes[], perClipUndo, learnedBase}
 *   readFraming(seq, {popupBase}) -> {ok, reason, name, lens, pan, tilt, roll, fov, distortion, djiFov,
 *                                     correction, zoom, aspect, learnedBase}
 *   writeFraming(seq, {action: 'preset'|'zoom', preset, direction, popupBase})
 *                            -> {ok, reason, error, label, keyframed, learnedBase}
 *   setStabilization(seq, items, {entry, popupBase})
 *                            -> {unsupported, updated, unchanged, missing, failed, replacedFull, errors[], notes[],
 *                                learnedBase}
 *
 * UNDO.  Every change this adapter makes is ONE Project.executeTransaction()
 * - "Execute undoable transaction by passing compound action" (Adobe's
 * Project reference) - so each button press is one Ctrl+Z, however many
 * clips it touched: the Keyframe Animation Apply, a Manual Framing preset or
 * zoom press, and the Stabilisation Apply.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvUxpAdapter = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /** Undo-history names of the transactions this adapter commits. */
    var UNDO_APPLY = 'Apply Open 360 Reframe';
    var UNDO_PARAMS = 'Set Open 360 Reframe lens';
    // [WP-EASING] One named step per button press.
    var UNDO_EASING = 'Set Open 360 Reframe keyframe easing';
    var UNDO_FRAMING = 'Frame Open 360 Reframe';
    var UNDO_STABILIZATION = 'Set OpenOSV stabilisation';

    /** Project items remembered per project; plenty for any real edit. */
    var PATH_CACHE_LIMIT = 20000;

    function messageOf(err) {
        if (err && typeof err.message === 'string' && err.message.length > 0) {
            return err.message;
        }
        return typeof err === 'string' && err.length > 0 ? err : 'unknown error';
    }

    /** Await a value that the docs call synchronous but a host might wrap. */
    function settle(value) {
        return (value && typeof value.then === 'function') ? value : Promise.resolve(value);
    }

    /**
     * @param {object} ppro     require('premierepro')
     * @param {object} core     OsvCore
     * @param {object} [options] { log(message) }
     */
    function createUxpAdapter(ppro, core, options) {
        if (!ppro || (typeof ppro !== 'object' && typeof ppro !== 'function')) {
            throw new Error('the premierepro module is not available');
        }
        if (!core) {
            throw new Error('OsvCore is not loaded');
        }
        var opts = options || {};
        var log = typeof opts.log === 'function' ? opts.log : function () {};

        var onEvent = null;
        var trackListeners = [];   // {track, name, handler}
        var globalListeners = [];  // {name, handler}
        var trackSig = '';
        var live = new Map();      // item key -> VideoClipTrackItem from the latest scan
        var pathCache = new Map(); // projectItemId -> {osv, projectItemId, path}
        var cacheProject = null;
        var hostMatchName = null;  // resolved once found

        // ---- small accessors -------------------------------------------------

        function constants() {
            return (ppro.Constants && typeof ppro.Constants === 'object') ? ppro.Constants : {};
        }

        function eventManager() {
            return ppro.EventManager || null;
        }

        /** Constants.TrackItemType.CLIP; the docs give its value as 1. */
        function clipType() {
            var t = constants().TrackItemType;
            return (t && t.CLIP !== undefined) ? t.CLIP : 1;
        }

        /** VideoTrackEvent.TRACK_CHANGED, or the class constant, or nothing. */
        function trackChangedName() {
            var c = constants();
            if (c.VideoTrackEvent && c.VideoTrackEvent.TRACK_CHANGED) {
                return c.VideoTrackEvent.TRACK_CHANGED;
            }
            if (ppro.VideoTrack && ppro.VideoTrack.EVENT_TRACK_CHANGED) {
                return ppro.VideoTrack.EVENT_TRACK_CHANGED;
            }
            return null;
        }

        /** A Guid (or string) as text; '' when there is none. */
        function guidText(guid) {
            try {
                if (guid === null || guid === undefined) {
                    return '';
                }
                if (typeof guid === 'string') {
                    return guid;
                }
                if (typeof guid.toString === 'function') {
                    var s = guid.toString();
                    return (typeof s === 'string' && s !== '[object Object]') ? s : '';
                }
            } catch (err) {
                log('guid: ' + messageOf(err));
            }
            return '';
        }

        /** A TickTime's ticks as a string; '' when unreadable. */
        function ticksOf(tickTime) {
            if (!tickTime) {
                return '';
            }
            if (typeof tickTime.ticks === 'string') {
                return tickTime.ticks;
            }
            if (typeof tickTime.ticksNumber === 'number' && isFinite(tickTime.ticksNumber)) {
                return String(tickTime.ticksNumber);
            }
            return '';
        }

        /** Call an async getter; any failure becomes `fallback`. */
        function safeCall(fn, fallback) {
            try {
                return settle(fn()).then(function (v) { return v; }, function () { return fallback; });
            } catch (err) {
                return Promise.resolve(fallback);
            }
        }

        function fire(reason) {
            if (typeof onEvent !== 'function') {
                return;
            }
            try {
                onEvent(reason);
            } catch (err) {
                log('event handler: ' + messageOf(err));
            }
        }

        // ---- the active project and sequence -----------------------------------

        /** {project, seq, id, name, projectId} for the active sequence, or null. */
        function activeContext() {
            if (!ppro.Project || typeof ppro.Project.getActiveProject !== 'function') {
                return Promise.resolve(null);
            }
            var project = null;
            return settle(ppro.Project.getActiveProject())
                .then(function (p) {
                    project = p || null;
                    if (!project || typeof project.getActiveSequence !== 'function') {
                        return null;
                    }
                    return project.getActiveSequence();
                })
                .then(function (seq) {
                    if (!project || !seq) {
                        return null;
                    }
                    var projectId = guidText(project.guid) || String(project.path || '');
                    if (projectId !== cacheProject) {
                        // Another project: its project items are different objects.
                        pathCache.clear();
                        cacheProject = projectId;
                    }
                    return {
                        project: project,
                        seq: seq,
                        id: guidText(seq.guid),
                        name: typeof seq.name === 'string' ? seq.name : '',
                        projectId: projectId
                    };
                });
        }

        /** The active context, or an error when the user switched sequence. */
        function contextFor(seq) {
            return activeContext().then(function (ctx) {
                if (!ctx || !seq || ctx.id !== seq.id) {
                    throw new Error('the active sequence changed. Try again');
                }
                return ctx;
            });
        }

        // ---- track listeners -----------------------------------------------------

        function detachTracks() {
            var em = eventManager();
            for (var i = 0; i < trackListeners.length; i += 1) {
                var l = trackListeners[i];
                try {
                    if (em && typeof em.removeEventListener === 'function') {
                        em.removeEventListener(l.track, l.name, l.handler);
                    }
                } catch (err) {
                    log('remove track listener: ' + messageOf(err));
                }
            }
            trackListeners = [];
            trackSig = '';
        }

        /**
         * Listen for TRACK_CHANGED on every video track of the active
         * sequence, re-attaching when the sequence or its track count changes
         * (a clip dropped above the top track creates a track, and a new
         * track has no listener until this runs).  Returns the track count.
         */
        function syncTracks(ctx) {
            return safeCall(function () { return ctx.seq.getVideoTrackCount(); }, 0).then(function (count) {
                var n = Math.max(0, Math.floor(Number(count) || 0));
                var sig = ctx.id + '#' + n;
                if (sig === trackSig) {
                    return n;
                }
                detachTracks();
                trackSig = sig;
                var em = eventManager();
                var name = trackChangedName();
                if (!em || typeof em.addEventListener !== 'function' || !name) {
                    return n;
                }
                var chain = Promise.resolve();
                for (var t = 0; t < n; t += 1) {
                    (function (index) {
                        chain = chain.then(function () {
                            return safeCall(function () { return ctx.seq.getVideoTrack(index); }, null);
                        }).then(function (track) {
                            if (!track) {
                                return;
                            }
                            var handler = function () { fire('track'); };
                            try {
                                em.addEventListener(track, name, handler);
                                trackListeners.push({ track: track, name: name, handler: handler });
                            } catch (err) {
                                log('track listener: ' + messageOf(err));
                            }
                        });
                    }(t));
                }
                return chain.then(function () { return n; });
            });
        }

        /** The clips of one video track (an array, whatever the host returned). */
        function clipsOf(track) {
            return safeCall(function () { return track.getTrackItems(clipType(), false); }, []).then(function (list) {
                return Array.isArray(list) ? list : [];
            });
        }

        // ---- media ---------------------------------------------------------------------

        /** {osv, projectItemId, path} for a track item; cached per project item. */
        function mediaInfo(trackItem) {
            return safeCall(function () { return trackItem.getProjectItem(); }, null).then(function (pi) {
                if (!pi) {
                    return null;
                }
                var id = '';
                try {
                    id = typeof pi.getId === 'function' ? String(pi.getId() || '') : '';
                } catch (err) {
                    id = '';
                }
                if (id && pathCache.has(id)) {
                    return pathCache.get(id);
                }
                var clip = null;
                try {
                    clip = (ppro.ClipProjectItem && typeof ppro.ClipProjectItem.cast === 'function')
                        ? ppro.ClipProjectItem.cast(pi) : null;
                } catch (err) {
                    clip = null;
                }
                if (!clip) {
                    return { osv: false, projectItemId: id, path: '' };
                }
                var isSequence = false;
                return safeCall(function () { return clip.isSequence(); }, false)
                    .then(function (seqFlag) {
                        isSequence = seqFlag === true;
                        // A nested sequence has no media file of its own.
                        return isSequence ? '' : safeCall(function () { return clip.getMediaFilePath(); }, '');
                    })
                    .then(function (path) {
                        var p = typeof path === 'string' ? path : '';
                        var info = {
                            osv: !isSequence && core.isOsvMediaPath(p),
                            projectItemId: id || ('path:' + p),
                            path: p
                        };
                        if (id) {
                            if (pathCache.size >= PATH_CACHE_LIMIT) {
                                pathCache.clear();
                            }
                            pathCache.set(id, info);
                        }
                        return info;
                    });
            });
        }

        // ---- effects -------------------------------------------------------------------

        /** The match names of every component in a chain, in order. */
        function chainMatchNames(chain) {
            var count = 0;
            try {
                count = Math.max(0, Math.floor(Number(chain.getComponentCount()) || 0));
            } catch (err) {
                count = 0;
            }
            var names = [];
            var seq = Promise.resolve();
            for (var i = 0; i < count; i += 1) {
                (function (index) {
                    seq = seq.then(function () {
                        var comp = null;
                        try {
                            comp = chain.getComponentAtIndex(index);
                        } catch (err) {
                            comp = null;
                        }
                        if (!comp) {
                            names.push('');
                            return undefined;
                        }
                        return safeCall(function () { return comp.getMatchName(); }, '').then(function (n) {
                            names.push(typeof n === 'string' ? n : '');
                        });
                    });
                }(i));
            }
            return seq.then(function () { return names; });
        }

        /** The LAST Open 360 Reframe in a chain (the one just appended), or null. */
        function lastReframeComponent(chain) {
            return chainMatchNames(chain).then(function (names) {
                for (var i = names.length - 1; i >= 0; i -= 1) {
                    if (core.isReframeMatchName(names[i])) {
                        try {
                            return chain.getComponentAtIndex(i) || null;
                        } catch (err) {
                            return null;
                        }
                    }
                }
                return null;
            });
        }

        /** Resolve (and remember) the name Premiere registered the effect under. */
        function resolveMatchName() {
            if (hostMatchName) {
                return Promise.resolve(hostMatchName);
            }
            var factory = ppro.VideoFilterFactory;
            if (!factory || typeof factory.getMatchNames !== 'function') {
                return Promise.resolve(null);
            }
            return safeCall(function () { return factory.getMatchNames(); }, null).then(function (names) {
                if (!Array.isArray(names)) {
                    // The list is unavailable; creating by the registered name
                    // is still the documented call, and it fails loudly if wrong.
                    return core.REFRAME_HOST_MATCH_NAME;
                }
                hostMatchName = core.pickHostMatchName(names);
                return hostMatchName;
            });
        }

        /** The parameters of a component that the panel may write. */
        function readParams(component) {
            var wanted = [];
            var names = core.PARAM_NAMES;
            Object.keys(names).forEach(function (k) { wanted.push(names[k]); });
            var count = 0;
            try {
                count = Math.max(0, Math.floor(Number(component.getParamCount()) || 0));
            } catch (err) {
                count = 0;
            }
            var params = [];
            var seq = Promise.resolve();
            for (var i = 0; i < count; i += 1) {
                (function (index) {
                    seq = seq.then(function () {
                        var p = null;
                        try {
                            p = component.getParam(index);
                        } catch (err) {
                            p = null;
                        }
                        var name = p && typeof p.displayName === 'string' ? p.displayName.trim() : '';
                        if (!p || wanted.indexOf(name) === -1) {
                            return undefined;
                        }
                        var timeVarying = false;
                        try {
                            timeVarying = p.isTimeVarying() === true;
                        } catch (err) {
                            timeVarying = false;
                        }
                        return safeCall(function () { return p.getStartValue(); }, null).then(function (kf) {
                            var value;
                            if (kf && kf.value !== null && typeof kf.value === 'object' && 'value' in kf.value) {
                                value = kf.value.value;
                            }
                            params.push({ index: index, name: name, value: value, timeVarying: timeVarying });
                        });
                    });
                }(i));
            }
            return seq.then(function () { return params; });
        }

        /** True when the settings ask for anything beyond the effect's defaults. */
        function wantsParams(settings) {
            var s = core.sanitizeSettings(settings);
            return s.lens === core.LENS.classic || s.dragEnabled;
        }

        /**
         * Write the lens / drag sensitivity into freshly applied effects.
         * One transaction for all of them, after the effects exist - the
         * parameters of a component can only be reached once it is in a chain.
         */
        function writeParams(ctx, chains, settings, result) {
            var writes = [];
            var seq = Promise.resolve();
            chains.forEach(function (chain) {
                seq = seq.then(function () {
                    return lastReframeComponent(chain).then(function (component) {
                        if (!component) {
                            return undefined;
                        }
                        return readParams(component).then(function (params) {
                            var plan = core.planParamWrites(params, settings);
                            plan.notes.forEach(function (n) {
                                if (result.notes.indexOf(n) === -1) {
                                    result.notes.push(n);
                                }
                            });
                            plan.writes.forEach(function (w) {
                                writes.push({ component: component, index: w.index, value: w.value });
                            });
                        });
                    });
                });
            });
            return seq.then(function () {
                if (writes.length === 0) {
                    return;
                }
                var ok = false;
                var problem = '';
                try {
                    // Actions are created inside the lock (required since 26.3).
                    ctx.project.lockedAccess(function () {
                        var actions = [];
                        writes.forEach(function (w) {
                            try {
                                var param = w.component.getParam(w.index);
                                var keyframe = param.createKeyframe(w.value);
                                actions.push(param.createSetValueAction(keyframe, true));
                            } catch (err) {
                                problem = messageOf(err);
                            }
                        });
                        if (actions.length === 0) {
                            return;
                        }
                        ok = ctx.project.executeTransaction(function (compound) {
                            actions.forEach(function (a) { compound.addAction(a); });
                        }, UNDO_PARAMS) === true;
                    });
                } catch (err) {
                    problem = messageOf(err);
                }
                if (!ok) {
                    result.notes.push('params-failed');
                    result.errors.push('the effect is on, but its lens settings could not be set' +
                                       (problem ? ' (' + problem + ')' : ''));
                }
            });
        }

        // ---- [WP-EASING] reading and writing an effect's controls -----------------------

        /** Every Open 360 Reframe in a chain, in chain order. */
        function reframeComponents(chain) {
            return chainMatchNames(chain).then(function (names) {
                var out = [];
                for (var i = 0; i < names.length; i += 1) {
                    if (core.isReframeMatchName(names[i])) {
                        try {
                            var c = chain.getComponentAtIndex(i);
                            if (c) {
                                out.push(c);
                            }
                        } catch (err) {
                            log('component ' + i + ': ' + messageOf(err));
                        }
                    }
                }
                return out;
            });
        }

        /** [{index, name, param}] of a component, in the host's order. */
        function paramList(component) {
            var count = 0;
            try {
                count = Math.max(0, Math.floor(Number(component.getParamCount()) || 0));
            } catch (err) {
                count = 0;
            }
            var list = [];
            for (var i = 0; i < count; i += 1) {
                var p = null;
                try {
                    p = component.getParam(i);
                } catch (err) {
                    p = null;
                }
                if (p && typeof p.displayName === 'string') {
                    list.push({ index: i, name: p.displayName.trim(), param: p });
                }
            }
            return list;
        }

        /** The param of a list by display name, or null. */
        function byName(list, name) {
            for (var i = 0; i < list.length; i += 1) {
                if (list[i].name === name) {
                    return list[i];
                }
            }
            return null;
        }

        /** The param of a list by host index, or null. */
        function byIndex(list, index) {
            for (var i = 0; i < list.length; i += 1) {
                if (list[i].index === index) {
                    return list[i];
                }
            }
            return null;
        }

        /**
         * A param's value: at `time` (a TickTime) when given, else its start
         * value.  Anything the host will not say is undefined.
         */
        function valueOf(param, time) {
            if (!param) {
                return Promise.resolve(undefined);
            }
            if (time && typeof param.getValueAtTime === 'function') {
                return safeCall(function () { return param.getValueAtTime(time); }, undefined);
            }
            return safeCall(function () { return param.getStartValue(); }, null).then(function (kf) {
                return (kf && kf.value !== null && typeof kf.value === 'object' && 'value' in kf.value)
                    ? kf.value.value : undefined;
            });
        }

        /** True when a param is keyframed. */
        function timeVarying(param) {
            try {
                return !!param && param.isTimeVarying() === true;
            } catch (err) {
                return false;
            }
        }

        /**
         * Readings of the popups in `counts` (display name -> entry count)
         * that can settle how the host numbers popups.
         */
        function popupReadings(list, counts, time) {
            var names = Object.keys(counts);
            var readings = [];
            var chain = Promise.resolve();
            names.forEach(function (name) {
                chain = chain.then(function () {
                    var p = byName(list, name);
                    if (!p) {
                        return undefined;
                    }
                    return valueOf(p.param, time).then(function (v) {
                        readings.push({ value: v, count: counts[name] });
                    });
                });
            });
            return chain.then(function () { return readings; });
        }

        /**
         * Last resort for the popup numbering: a FRESH Open 360 Reframe - a
         * component created and never added to a clip - sits on its Lens
         * default, DJI, entry 1, so its reading is the base itself.
         */
        function probeBaseFromFreshEffect() {
            return resolveMatchName().then(function (name) {
                if (!name || !ppro.VideoFilterFactory || typeof ppro.VideoFilterFactory.createComponent !== 'function') {
                    return null;
                }
                return safeCall(function () { return ppro.VideoFilterFactory.createComponent(name); }, null)
                    .then(function (component) {
                        if (!component) {
                            return null;
                        }
                        var lens = byName(paramList(component), core.PARAM_NAMES.lens);
                        return lens ? valueOf(lens.param, null).then(core.popupBaseFromDefault) : null;
                    });
            });
        }

        /** A TickTime for a decimal tick string, or null. */
        function tickTime(ticks) {
            if (!ticks || !ppro.TickTime || typeof ppro.TickTime.createWithTicks !== 'function') {
                return null;
            }
            try {
                return ppro.TickTime.createWithTicks(String(ticks)) || null;
            } catch (err) {
                return null;
            }
        }

        /**
         * The action that puts `value` into a param: a keyframe at `time`
         * when the param is keyframed (Premiere adds it, or updates the one
         * already there), otherwise a plain value.  Must run inside the lock.
         */
        function valueAction(param, value, time) {
            var keyframe = param.createKeyframe(value);
            if (timeVarying(param) && time) {
                keyframe.position = time;
                return { action: param.createAddKeyframeAction(keyframe), keyed: true };
            }
            return { action: param.createSetValueAction(keyframe, true), keyed: false };
        }

        /**
         * Run a list of writes - {param, value, time} - as ONE undoable
         * transaction.  Resolves {ok, keyed, error}.
         */
        function commitWrites(ctx, writes, undoName) {
            if (writes.length === 0) {
                return Promise.resolve({ ok: true, keyed: false, error: '' });
            }
            var ok = false;
            var keyed = false;
            var problem = '';
            try {
                ctx.project.lockedAccess(function () {
                    var actions = [];
                    writes.forEach(function (w) {
                        try {
                            var a = valueAction(w.param, w.value, w.time);
                            keyed = keyed || a.keyed;
                            actions.push(a.action);
                        } catch (err) {
                            problem = messageOf(err);
                        }
                    });
                    if (actions.length === 0 || problem) {
                        return;
                    }
                    ok = ctx.project.executeTransaction(function (compound) {
                        actions.forEach(function (a) { compound.addAction(a); });
                    }, undoName) === true;
                });
            } catch (err) {
                problem = messageOf(err);
            }
            return Promise.resolve({ ok: ok && !problem, keyed: keyed, error: problem || (ok ? '' : 'Premiere refused the change') });
        }

        /**
         * The one OSV clip selected in the active sequence, with where it
         * sits: {ti, name, start, end, inPoint} or {reason}.
         */
        function selectedOsvClip(ctx) {
            return syncTracks(ctx).then(function (count) {
                var picks = [];
                var selectedOther = 0;
                var chain = Promise.resolve();
                for (var t = 0; t < count; t += 1) {
                    (function (trackIndex) {
                        chain = chain.then(function () {
                            return safeCall(function () { return ctx.seq.getVideoTrack(trackIndex); }, null);
                        }).then(function (track) {
                            return track ? clipsOf(track) : [];
                        }).then(function (list) {
                            var inner = Promise.resolve();
                            list.forEach(function (ti) {
                                inner = inner.then(function () {
                                    return safeCall(function () { return ti.getIsSelected(); }, false).then(function (sel) {
                                        if (sel !== true) {
                                            return undefined;
                                        }
                                        return mediaInfo(ti).then(function (info) {
                                            if (!info || !info.osv) {
                                                selectedOther += 1;
                                                return undefined;
                                            }
                                            return Promise.all([
                                                safeCall(function () { return ti.getStartTime(); }, null),
                                                safeCall(function () { return ti.getEndTime(); }, null),
                                                safeCall(function () { return ti.getInPoint(); }, null),
                                                safeCall(function () { return ti.getName(); }, '')
                                            ]).then(function (v) {
                                                picks.push({
                                                    ti: ti,
                                                    start: ticksOf(v[0]),
                                                    end: ticksOf(v[1]),
                                                    inPoint: ticksOf(v[2]),
                                                    name: typeof v[3] === 'string' ? v[3] : ''
                                                });
                                            });
                                        });
                                    });
                                });
                            });
                            return inner;
                        });
                    }(t));
                }
                return chain.then(function () {
                    if (picks.length === 1) {
                        return picks[0];
                    }
                    if (picks.length > 1) {
                        return { reason: 'many-selected' };
                    }
                    return { reason: selectedOther > 0 ? 'not-osv' : 'no-selection' };
                });
            });
        }

        /**
         * Everything the Manual Framing card needs about the selected clip's
         * Open 360 Reframe at the playhead: the component, its located
         * params, the popup base, the lens and the frame shape.
         */
        function framingContext(ctx, hintBase) {
            return selectedOsvClip(ctx).then(function (clip) {
                if (clip.reason) {
                    return clip;
                }
                var out = { clip: clip, component: null, list: [], roles: {}, base: hintBase, time: null };
                return safeCall(function () { return ctx.seq.getPlayerPosition(); }, null).then(function (pos) {
                    var playhead = ticksOf(pos);
                    if (!core.ticksInside(playhead, clip.start, clip.end)) {
                        return { reason: 'outside' };
                    }
                    out.time = tickTime(core.componentTicks(playhead, clip.start, clip.inPoint));
                    return safeCall(function () { return clip.ti.getComponentChain(); }, null);
                }).then(function (chain) {
                    if (out.reason || (chain && chain.reason)) {
                        return chain;
                    }
                    if (!chain) {
                        return { reason: 'no-effect' };
                    }
                    return reframeComponents(chain).then(function (list) {
                        if (list.length === 0) {
                            return { reason: 'no-effect' };
                        }
                        out.component = list[list.length - 1];
                        out.list = paramList(out.component);
                        out.roles = core.locateReframeParams(out.list);
                        return popupReadings(out.list, core.REFRAME_POPUP_COUNTS, out.time).then(function (readings) {
                            out.base = core.learnPopupBase(readings, hintBase);
                            return safeCall(function () { return ctx.seq.getFrameSize(); }, null);
                        }).then(function (size) {
                            out.seqWidth = size ? Number(size.width) : 0;
                            out.seqHeight = size ? Number(size.height) : 0;
                            return out;
                        });
                    });
                });
            });
        }

        /** Read one role's value at the framing context's time. */
        function roleValue(fc, role) {
            var p = fc.roles[role] !== undefined ? byIndex(fc.list, fc.roles[role]) : null;
            return p ? valueOf(p.param, fc.time) : Promise.resolve(undefined);
        }

        /** framingState() of a framing context, its controls read at its time. */
        function readFramingValues(fc) {
            var roles = ['lens', 'cameraModel', 'outputResolution', 'pan', 'tilt', 'roll', 'fov', 'distortion', 'djiFov',
                         'correction'];
            var v = {};
            var chain = Promise.resolve();
            roles.forEach(function (role) {
                chain = chain.then(function () {
                    return roleValue(fc, role).then(function (value) { v[role] = value; });
                });
            });
            return chain.then(function () {
                return core.framingState(v, fc.base, fc.seqWidth, fc.seqHeight);
            });
        }

        // ---- the adapter ------------------------------------------------------------

        return {
            init: function (handler) {
                if (!ppro.Project || typeof ppro.Project.getActiveProject !== 'function') {
                    throw new Error('this Premiere has no UXP project API');
                }
                onEvent = typeof handler === 'function' ? handler : null;
                var em = eventManager();
                var c = constants();
                var add = function (name, reason, capture) {
                    if (!em || typeof em.addGlobalEventListener !== 'function' || !name) {
                        return;
                    }
                    var fn = function () { fire(reason); };
                    try {
                        if (capture) {
                            em.addGlobalEventListener(name, fn, true);
                        } else {
                            em.addGlobalEventListener(name, fn);
                        }
                        globalListeners.push({ name: name, handler: fn });
                    } catch (err) {
                        log('global listener ' + reason + ': ' + messageOf(err));
                    }
                };
                // A sequence or project switch changes what "the timeline" is;
                // a dirty project is the broadest "something was edited".
                // Registered exactly as Adobe's premiere-api sample registers
                // them: the project events in the capture phase.
                if (c.SequenceEvent) {
                    add(c.SequenceEvent.ACTIVATED, 'sequence', false);
                    // [WP-EASING] Another clip selected: the Manual Framing
                    // read-outs follow it at once instead of at the next poll.
                    add(c.SequenceEvent.SELECTION_CHANGED, 'selection', false);
                }
                if (c.ProjectEvent) {
                    add(c.ProjectEvent.ACTIVATED, 'project', true);
                    add(c.ProjectEvent.OPENED, 'project', true);
                    add(c.ProjectEvent.DIRTY, 'dirty', true);
                }
            },

            dispose: function () {
                detachTracks();
                var em = eventManager();
                globalListeners.forEach(function (l) {
                    try {
                        if (em && typeof em.removeGlobalEventListener === 'function') {
                            em.removeGlobalEventListener(l.name, l.handler);
                        }
                    } catch (err) {
                        log('remove global listener: ' + messageOf(err));
                    }
                });
                globalListeners = [];
                onEvent = null;
                live = new Map();
            },

            getActiveSequence: function () {
                return activeContext().then(function (ctx) {
                    if (!ctx || !ctx.id) {
                        return null;
                    }
                    // Keep the track listeners on the sequence the user is in.
                    return syncTracks(ctx).then(function () {
                        return { id: ctx.id, name: ctx.name, projectId: ctx.projectId };
                    });
                });
            },

            getSequenceIds: function () {
                return settle(ppro.Project.getActiveProject()).then(function (project) {
                    if (!project || typeof project.getSequences !== 'function') {
                        return null;
                    }
                    return settle(project.getSequences()).then(function (list) {
                        if (!Array.isArray(list)) {
                            return null;
                        }
                        return list.map(function (s) { return s ? guidText(s.guid) : ''; })
                                   .filter(function (id) { return id.length > 0; });
                    });
                });
            },

            signature: function () {
                return activeContext().then(function (ctx) {
                    if (!ctx) {
                        return 'none';
                    }
                    return syncTracks(ctx).then(function (count) {
                        var parts = [ctx.id, String(count)];
                        var chain = Promise.resolve();
                        for (var t = 0; t < count; t += 1) {
                            (function (index) {
                                chain = chain.then(function () {
                                    return safeCall(function () { return ctx.seq.getVideoTrack(index); }, null);
                                }).then(function (track) {
                                    return track ? clipsOf(track) : [];
                                }).then(function (list) {
                                    parts.push(String(list.length));
                                });
                            }(t));
                        }
                        return chain.then(function () { return parts.join('|'); });
                    });
                });
            },

            scan: function (seq, scanOptions) {
                var selectedOnly = !!(scanOptions && scanOptions.selectedOnly);
                return contextFor(seq).then(function (ctx) {
                    return syncTracks(ctx).then(function (count) {
                        var items = [];
                        var other = 0;
                        var nextLive = new Map();
                        var chain = Promise.resolve();
                        for (var t = 0; t < count; t += 1) {
                            (function (trackIndex) {
                                chain = chain.then(function () {
                                    return safeCall(function () { return ctx.seq.getVideoTrack(trackIndex); }, null);
                                }).then(function (track) {
                                    return track ? clipsOf(track) : [];
                                }).then(function (list) {
                                    var inner = Promise.resolve();
                                    list.forEach(function (ti) {
                                        inner = inner.then(function () {
                                            if (!ti) {
                                                return undefined;
                                            }
                                            var info = null;
                                            return mediaInfo(ti).then(function (m) {
                                                info = m;
                                                return selectedOnly
                                                    ? safeCall(function () { return ti.getIsSelected(); }, false)
                                                    : true;
                                            }).then(function (selected) {
                                                if (selected !== true) {
                                                    return undefined;
                                                }
                                                if (!info || !info.osv) {
                                                    other += 1;
                                                    return undefined;
                                                }
                                                return Promise.all([
                                                    safeCall(function () { return ti.getStartTime(); }, null),
                                                    safeCall(function () { return ti.getEndTime(); }, null),
                                                    safeCall(function () { return ti.getInPoint(); }, null),
                                                    safeCall(function () { return ti.getOutPoint(); }, null),
                                                    safeCall(function () { return ti.getName(); }, '')
                                                ]).then(function (v) {
                                                    var start = ticksOf(v[0]);
                                                    var item = {
                                                        key: core.makeItemKey(trackIndex, start, info.projectItemId),
                                                        trackIndex: trackIndex,
                                                        startTicks: start,
                                                        endTicks: ticksOf(v[1]),
                                                        inTicks: ticksOf(v[2]),
                                                        outTicks: ticksOf(v[3]),
                                                        projectItemId: info.projectItemId,
                                                        name: typeof v[4] === 'string' ? v[4] : '',
                                                        mediaPath: info.path
                                                    };
                                                    items.push(item);
                                                    nextLive.set(item.key, ti);
                                                });
                                            });
                                        });
                                    });
                                    return inner;
                                });
                            }(t));
                        }
                        return chain.then(function () {
                            live = nextLive;
                            // A full scan counts every non-OSV clip, which the
                            // status line has no use for; only a selection
                            // scan reports them.
                            return { items: items, otherCount: selectedOnly ? other : 0 };
                        });
                    });
                });
            },

            apply: function (seq, items, settings) {
                var result = { applied: 0, already: 0, failed: 0, errors: [], notes: [], missingEffect: false };
                var targets = Array.isArray(items) ? items : [];
                if (targets.length === 0) {
                    return Promise.resolve(result);
                }
                function fail(message) {
                    result.failed += 1;
                    if (result.errors.length < 5) {
                        result.errors.push(message);
                    }
                }
                var ctx = null;
                var matchName = null;
                var plans = [];
                return contextFor(seq)
                    .then(function (c) {
                        ctx = c;
                        return resolveMatchName();
                    })
                    .then(function (name) {
                        matchName = name;
                        if (!matchName) {
                            result.missingEffect = true;
                            return undefined;
                        }
                        // ---- 1. look at every target outside the lock --------
                        var chain = Promise.resolve();
                        targets.forEach(function (item) {
                            chain = chain.then(function () {
                                var ti = item && live.get(item.key);
                                if (!ti) {
                                    fail('a clip moved before it could be updated');
                                    return undefined;
                                }
                                var plan = { item: item, chain: null, component: null, count: -1 };
                                return safeCall(function () { return ti.getComponentChain(); }, null)
                                    .then(function (componentChain) {
                                        if (!componentChain) {
                                            fail('Premiere gave no effect list for "' + (item.name || 'a clip') + '"');
                                            return undefined;
                                        }
                                        plan.chain = componentChain;
                                        return chainMatchNames(componentChain).then(function (names) {
                                            if (core.hasReframeEffect(names)) {
                                                result.already += 1;
                                                return undefined;
                                            }
                                            plan.count = names.length;
                                            return settle(ppro.VideoFilterFactory.createComponent(matchName))
                                                .then(function (component) {
                                                    if (!component) {
                                                        fail('Premiere did not create the effect');
                                                        return;
                                                    }
                                                    plan.component = component;
                                                    plans.push(plan);
                                                }, function (err) {
                                                    fail(messageOf(err));
                                                });
                                        });
                                    });
                            });
                        });
                        return chain;
                    })
                    .then(function () {
                        if (!matchName || plans.length === 0) {
                            return undefined;
                        }
                        // ---- 2. one undoable transaction for every append ---
                        var ok = false;
                        var committed = [];
                        var lockError = '';
                        try {
                            ctx.project.lockedAccess(function () {
                                var actions = [];
                                plans.forEach(function (p) {
                                    var count = -1;
                                    try {
                                        count = Number(p.chain.getComponentCount());
                                    } catch (err) {
                                        count = -1;
                                    }
                                    // The project cannot change inside the lock, so
                                    // an unchanged count means nothing was added
                                    // since we looked - not by the user, not by a
                                    // second copy of this panel.
                                    if (count !== p.count) {
                                        p.raced = true;
                                        return;
                                    }
                                    try {
                                        actions.push(p.chain.createAppendComponentAction(p.component));
                                        committed.push(p);
                                    } catch (err) {
                                        p.error = messageOf(err);
                                    }
                                });
                                if (actions.length === 0) {
                                    return;
                                }
                                ok = ctx.project.executeTransaction(function (compound) {
                                    actions.forEach(function (a) { compound.addAction(a); });
                                }, UNDO_APPLY) === true;
                            });
                        } catch (err) {
                            lockError = messageOf(err);
                        }

                        // ---- 3. verify through the chain itself ---------------------
                        var verified = [];
                        var chain = Promise.resolve();
                        plans.forEach(function (p) {
                            chain = chain.then(function () {
                                if (p.error) {
                                    fail(p.error);
                                    return undefined;
                                }
                                // Not raced, but its append never went through:
                                // the lock threw or the transaction said no.
                                if (!p.raced && (committed.indexOf(p) === -1 || !ok)) {
                                    fail(lockError || 'Premiere refused the change');
                                    return undefined;
                                }
                                return chainMatchNames(p.chain).then(function (names) {
                                    var has = core.hasReframeEffect(names);
                                    if (p.raced) {
                                        // Someone else changed this clip meanwhile.
                                        if (has) {
                                            result.already += 1;
                                        } else {
                                            fail('the clip changed while the effect was being applied');
                                        }
                                        return;
                                    }
                                    if (has) {
                                        result.applied += 1;
                                        verified.push(p.chain);
                                    } else {
                                        fail(lockError || 'Premiere did not add the effect');
                                    }
                                });
                            });
                        });
                        return chain.then(function () {
                            // ---- 4. the lens / drag sensitivity, if asked ----------
                            if (verified.length > 0 && wantsParams(settings)) {
                                return writeParams(ctx, verified, settings, result);
                            }
                            return undefined;
                        });
                    })
                    .then(function () { return result; });
            },

            checkEffect: function () {
                return resolveMatchName().then(function (name) {
                    if (name === core.REFRAME_HOST_MATCH_NAME && !hostMatchName) {
                        // Resolved by fallback, not by the host's list: unknown.
                        return { available: null };
                    }
                    return { available: !!name };
                });
            },

            /** What this route can do: grouped undo, and reaching Source Settings. */
            capabilities: function () {
                return {
                    undoGroups: true,
                    stabilization: !!(ppro.ClipProjectItem && typeof ppro.ClipProjectItem.cast === 'function')
                };
            },

            /**
             * [WP-EASING] Set the Keyframe Easing popup of every Open 360
             * Reframe on `items` to `request.entry` (1-based), in one
             * transaction.  Clips without the effect are counted as missing.
             */
            setEasing: function (seq, items, request) {
                var req = request || {};
                var entry = Math.floor(Number(req.entry));
                var result = { updated: 0, unchanged: 0, missing: 0, failed: 0, errors: [], notes: [], perClipUndo: false,
                               learnedBase: null };
                var targets = Array.isArray(items) ? items : [];
                if (!(entry >= 1 && entry <= core.EASINGS.length) || targets.length === 0) {
                    return Promise.resolve(result);
                }
                var ctx = null;
                var plans = [];     // {item, params: [param]} per clip with the effect
                var readings = [];
                return contextFor(seq).then(function (c) {
                    ctx = c;
                    // ---- 1. every clip's effects, and what their popups read --------
                    var chain = Promise.resolve();
                    targets.forEach(function (item) {
                        chain = chain.then(function () {
                            var ti = item && live.get(item.key);
                            if (!ti) {
                                result.failed += 1;
                                result.errors.push('a clip moved before it could be updated');
                                return undefined;
                            }
                            return safeCall(function () { return ti.getComponentChain(); }, null).then(function (cc) {
                                return cc ? reframeComponents(cc) : [];
                            }).then(function (components) {
                                if (components.length === 0) {
                                    result.missing += 1;
                                    return undefined;
                                }
                                var plan = { item: item, params: [] };
                                var inner = Promise.resolve();
                                components.forEach(function (component) {
                                    inner = inner.then(function () {
                                        var list = paramList(component);
                                        var p = byName(list, core.PARAM_NAMES.keyframeEasing);
                                        return popupReadings(list, core.REFRAME_POPUP_COUNTS, null).then(function (r) {
                                            readings = readings.concat(r);
                                            if (p) {
                                                return valueOf(p.param, null).then(function (v) {
                                                    plan.params.push({ param: p.param, value: v });
                                                });
                                            }
                                            return undefined;
                                        });
                                    });
                                });
                                return inner.then(function () {
                                    if (plan.params.length === 0) {
                                        result.failed += 1;
                                        result.errors.push('this Open 360 Reframe predates Keyframe Easing. Update the plug-ins');
                                        return;
                                    }
                                    plans.push(plan);
                                });
                            });
                        });
                    });
                    return chain;
                }).then(function () {
                    if (plans.length === 0) {
                        return result;
                    }
                    // ---- 2. the numbering: what was read, what was learned, a probe --
                    var base = core.learnPopupBase(readings, req.popupBase);
                    return (base === null ? probeBaseFromFreshEffect() : Promise.resolve(base)).then(function (b) {
                        if (b !== 0 && b !== 1) {
                            result.notes.push('base-unknown');
                            return result;
                        }
                        result.learnedBase = b;
                        var target = core.popupValue(entry, b);
                        // ---- 3. one transaction for every write -----------------------
                        var writes = [];
                        var touched = [];
                        plans.forEach(function (plan) {
                            var needs = plan.params.filter(function (x) { return x.value !== target; });
                            if (needs.length === 0) {
                                result.unchanged += 1;
                                return;
                            }
                            touched.push(plan);
                            needs.forEach(function (x) { writes.push({ param: x.param, value: target, time: null }); });
                        });
                        return commitWrites(ctx, writes, UNDO_EASING).then(function (done) {
                            if (done.ok) {
                                result.updated += touched.length;
                            } else {
                                result.failed += touched.length;
                                result.errors.push(done.error);
                            }
                            return result;
                        });
                    });
                }).then(function () { return result; });
            },

            /**
             * [WP-EASING] The selected clip's framing at the playhead, for the
             * Manual Framing read-outs.
             */
            readFraming: function (seq, request) {
                var req = request || {};
                return contextFor(seq).then(function (ctx) {
                    return framingContext(ctx, req.popupBase);
                }).then(function (fc) {
                    if (fc.reason) {
                        return { ok: false, reason: fc.reason };
                    }
                    return readFramingValues(fc).then(function (state) {
                        return Object.assign({ ok: true, name: fc.clip.name, learnedBase: fc.base }, state);
                    });
                });
            },

            /**
             * [WP-EASING] A Manual Framing action on the selected clip: a
             * preset (every value the effect's own Preset writes) or one zoom
             * press along DJI Studio's zoom path.  One transaction; a keyframed
             * control gets a keyframe at the playhead instead of a new value.
             */
            writeFraming: function (seq, request) {
                var req = request || {};
                var ctx = null;
                return contextFor(seq).then(function (c) {
                    ctx = c;
                    return framingContext(ctx, req.popupBase);
                }).then(function (fc) {
                    if (fc.reason) {
                        return { ok: false, reason: fc.reason };
                    }
                    return readFramingValues(fc).then(function (state) {
                        var plan = core.planFramingWrites(req, state, fc.base);
                        if (plan.reason) {
                            return { ok: false, reason: plan.reason, label: plan.label };
                        }
                        // Roles to this clip's params.  Numbers are keyed at
                        // the playhead when the control is keyframed; popups
                        // and the checkbox are never animatable.
                        var writes = [];
                        plan.writes.forEach(function (w) {
                            var p = fc.roles[w.role] !== undefined ? byIndex(fc.list, fc.roles[w.role]) : null;
                            if (p) {
                                writes.push({ param: p.param, value: w.value, time: w.kind === 'number' ? fc.time : null });
                            }
                        });
                        var label = plan.label;
                        return commitWrites(ctx, writes, UNDO_FRAMING).then(function (done) {
                            return {
                                ok: done.ok,
                                reason: done.ok ? '' : 'failed',
                                error: done.error,
                                label: label,
                                keyframed: done.keyed,
                                learnedBase: fc.base
                            };
                        });
                    });
                });
            },

            /**
             * [WP-EASING] Set the Stabilisation of the master clips behind
             * `items` - the OpenOSV Source Settings effect on each clip's
             * project item (ClipProjectItem.getComponentChain) - in one
             * transaction.  Several timeline clips of one master clip count
             * once.
             */
            setStabilization: function (seq, items, request) {
                var req = request || {};
                var entry = Math.floor(Number(req.entry));
                var result = { unsupported: false, updated: 0, unchanged: 0, missing: 0, failed: 0, replacedFull: 0,
                               errors: [], notes: [], learnedBase: null };
                if (!ppro.ClipProjectItem || typeof ppro.ClipProjectItem.cast !== 'function') {
                    result.unsupported = true;
                    return Promise.resolve(result);
                }
                var targets = Array.isArray(items) ? items : [];
                if (!(entry >= 1 && entry <= core.SOURCE_POPUP_COUNTS.Stabilisation) || targets.length === 0) {
                    return Promise.resolve(result);
                }
                var mediaVideo = (constants().MediaType && constants().MediaType.VIDEO !== undefined)
                    ? constants().MediaType.VIDEO : undefined;
                var ctx = null;
                var seen = {};
                var plans = [];
                var readings = [];
                return contextFor(seq).then(function (c) {
                    ctx = c;
                    var chain = Promise.resolve();
                    targets.forEach(function (item) {
                        chain = chain.then(function () {
                            var ti = item && live.get(item.key);
                            if (!ti) {
                                result.failed += 1;
                                result.errors.push('a clip moved before it could be updated');
                                return undefined;
                            }
                            var id = String(item.projectItemId || '');
                            if (id && seen[id]) {
                                return undefined;   // one master clip, several timeline clips
                            }
                            seen[id] = true;
                            return safeCall(function () { return ti.getProjectItem(); }, null).then(function (pi) {
                                var clip = null;
                                try {
                                    clip = pi ? ppro.ClipProjectItem.cast(pi) : null;
                                } catch (err) {
                                    clip = null;
                                }
                                if (!clip || typeof clip.getComponentChain !== 'function') {
                                    result.unsupported = true;
                                    return null;
                                }
                                return safeCall(function () { return clip.getComponentChain(mediaVideo); }, null);
                            }).then(function (cc) {
                                if (!cc) {
                                    if (!result.unsupported) {
                                        result.missing += 1;
                                    }
                                    return undefined;
                                }
                                return chainMatchNames(cc).then(function (names) {
                                    var at = -1;
                                    for (var i = 0; i < names.length; i += 1) {
                                        if (core.normaliseMatchName(names[i]) === core.SOURCE_SETTINGS_MATCH_NAME) {
                                            at = i;
                                        }
                                    }
                                    var component = null;
                                    try {
                                        component = at >= 0 ? cc.getComponentAtIndex(at) : null;
                                    } catch (err) {
                                        component = null;
                                    }
                                    var list = component ? paramList(component) : [];
                                    var p = byName(list, core.SOURCE_PARAM_NAMES.stabilization);
                                    if (!p) {
                                        result.missing += 1;
                                        return undefined;
                                    }
                                    return popupReadings(list, core.SOURCE_POPUP_COUNTS, null).then(function (r) {
                                        readings = readings.concat(r);
                                        return valueOf(p.param, null);
                                    }).then(function (value) {
                                        plans.push({ param: p.param, value: value });
                                    });
                                });
                            });
                        });
                    });
                    return chain;
                }).then(function () {
                    if (plans.length === 0) {
                        return result;
                    }
                    var base = core.learnPopupBase(readings, req.popupBase);
                    if (base === null) {
                        result.notes.push('base-unknown');
                        return result;
                    }
                    result.learnedBase = base;
                    var target = core.popupValue(entry, base);
                    // Full has no pair of switches; count the clips this
                    // press takes off it, so the status line can say so.
                    var full = core.popupValue(core.STABILIZATION_ENTRIES.full, base);
                    var writes = [];
                    var wasFull = 0;
                    plans.forEach(function (plan) {
                        if (plan.value === target) {
                            result.unchanged += 1;
                        } else {
                            writes.push({ param: plan.param, value: target, time: null });
                            wasFull += plan.value === full ? 1 : 0;
                        }
                    });
                    return commitWrites(ctx, writes, UNDO_STABILIZATION).then(function (done) {
                        if (done.ok) {
                            result.updated += writes.length;
                            result.replacedFull = wasFull;
                        } else {
                            result.failed += writes.length;
                            result.errors.push(done.error);
                        }
                        return result;
                    });
                }).then(function () { return result; });
            }
        };
    }

    return Object.freeze({
        createUxpAdapter: createUxpAdapter,
        UNDO_APPLY: UNDO_APPLY,
        UNDO_PARAMS: UNDO_PARAMS,
        UNDO_EASING: UNDO_EASING,
        UNDO_FRAMING: UNDO_FRAMING,
        UNDO_STABILIZATION: UNDO_STABILIZATION
    });
}));
