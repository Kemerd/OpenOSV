/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * host.jsx - the ExtendScript half of the OpenOSV CEP panel.
 *
 * The panel's decisions (is it OSV, is it new, which parameters to write)
 * are made in panel JavaScript by OsvCore, which is tested.  This file only
 * reports facts about the timeline and carries out the actions it is asked
 * for, returning JSON text for every call - it never throws into Premiere.
 *
 * WHAT IS OFFICIAL AND WHAT IS NOT
 * --------------------------------
 * Official Premiere ExtendScript DOM (ppro-scripting.docsforadobe.dev and
 * Adobe's PProPanel sample):
 *     app.project.activeSequence, sequence.videoTracks, track.clips,
 *     trackItem.nodeId / start / end / inPoint / outPoint / isSelected() /
 *     projectItem / components, projectItem.getMediaPath() / isSequence(),
 *     component.matchName / properties, param.displayName / getValue() /
 *     setValue() / isTimeVarying(), and app.bind() with
 *     onActiveSequenceTrackItemAdded / onActiveSequenceChanged /
 *     onActiveSequenceStructureChanged (all three used by PProPanel).
 *
 * UNOFFICIAL - the QE DOM, used for exactly one thing: putting the effect on
 * a clip.  The official ExtendScript DOM has no call that adds an effect;
 * qe.project.getVideoEffectByName() + QETrackItem.addVideoEffect() is the
 * widely used route, and Adobe staff describe the QE DOM as unsupported and
 * not recommended.  So its result is never trusted: after every
 * addVideoEffect() the official DOM is re-read and the clip only counts as
 * done when its component list really gained Open 360 Reframe.
 *
 * Language: ES3 (ExtendScript).  No JSON object, no Array.indexOf /
 * forEach, no String.trim, no Object.keys - hence the small helpers below.
 * panel/tests/hostjsx.test.js runs this file under Node against a mock DOM
 * and checks it for ES5+ constructs.
 */

var OpenOSVHost = (function () {
    // ---- identity (mirrors panel/shared/osvcore.js, checked by a test) ------
    var VERSION = '1.0.0';
    var MATCH_NAME = 'OpenOSV.Open360Reframe';
    var HOST_MATCH_NAME = 'AE.OpenOSV.Open360Reframe';
    var DISPLAY_NAME = 'Open 360 Reframe';
    var EVENT_TYPE = 'com.openosv.panel.hostchange';

    /** Parameters whose values the panel may need, by display name. */
    var WANTED_PARAMS = { 'Lens': true, 'Preset': true, 'Camera Model': true, 'Drag Sensitivity': true };

    /** Project item nodeId -> media facts; reset when the project changes. */
    var pathCache = {};
    var pathCacheSize = 0;
    var pathCacheProject = '';
    var PATH_CACHE_LIMIT = 20000;

    // ======================================================================
    //  JSON (ExtendScript has none)
    // ======================================================================

    /**
     * True for an array.  The ES3 test that also holds for an array made in
     * another engine context, where `instanceof Array` answers false.
     */
    function isArray(v) {
        return Object.prototype.toString.call(v) === '[object Array]';
    }

    /**
     * Characters a JSON string must escape, plus U+2028 / U+2029, which ES3
     * reads as line breaks.  Built from escaped source text so neither ever
     * appears raw in this file.
     */
    var JSON_ESCAPES = new RegExp('[\\\\"\\u0000-\\u001f\\u007f\\u2028\\u2029]', 'g');

    /** A string as a JSON literal, escaping what JSON and ES3 both need. */
    function jsonString(s) {
        return '"' + String(s).replace(JSON_ESCAPES, function (c) {
            if (c === '"') {
                return '\\"';
            }
            if (c === '\\') {
                return '\\\\';
            }
            var hex = c.charCodeAt(0).toString(16);
            while (hex.length < 4) {
                hex = '0' + hex;
            }
            return '\\u' + hex;
        }) + '"';
    }

    /** Any plain value as JSON text.  Functions and undefined are dropped. */
    function toJson(v) {
        if (v === null || v === undefined) {
            return 'null';
        }
        var t = typeof v;
        if (t === 'boolean') {
            return v ? 'true' : 'false';
        }
        if (t === 'number') {
            return isFinite(v) ? String(v) : 'null';
        }
        if (t === 'string') {
            return jsonString(v);
        }
        var parts = [];
        var i;
        if (isArray(v)) {
            for (i = 0; i < v.length; i += 1) {
                parts.push(toJson(v[i]));
            }
            return '[' + parts.join(',') + ']';
        }
        if (t === 'object') {
            for (var k in v) {
                if (v.hasOwnProperty(k) && typeof v[k] !== 'function' && v[k] !== undefined) {
                    parts.push(jsonString(k) + ':' + toJson(v[k]));
                }
            }
            return '{' + parts.join(',') + '}';
        }
        return 'null';
    }

    function okJson(obj) {
        obj.ok = true;
        return toJson(obj);
    }

    function errorJson(message) {
        return toJson({ ok: false, error: String(message) });
    }

    /** The text of whatever was thrown. */
    function messageOf(e) {
        try {
            if (e && e.message) {
                return String(e.message);
            }
            return String(e);
        } catch (ignored) {
            return 'unknown error';
        }
    }

    // ======================================================================
    //  Small ES3 helpers
    // ======================================================================

    function trimString(s) {
        return String(s).replace(/^\s+|\s+$/g, '');
    }

    /**
     * True for an .osv / .lrf media path.  The SAME rule as
     * OsvCore.isOsvMediaPath(); a test feeds both the same corpus.
     */
    function isOsvPath(path) {
        if (typeof path !== 'string') {
            return false;
        }
        var p = trimString(path.replace(/\u0000+$/, ''));
        if (p.length === 0 || p.length > 32767) {
            return false;
        }
        var cut = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
        var name = p.substring(cut + 1);
        var dot = name.lastIndexOf('.');
        if (dot <= 0 || dot === name.length - 1) {
            return false;
        }
        var ext = name.substring(dot + 1).toLowerCase();
        return ext === 'osv' || ext === 'lrf';
    }

    /** True for our effect's match name, with or without the "AE." prefix. */
    function isOurMatchName(name) {
        if (typeof name !== 'string') {
            return false;
        }
        var n = trimString(name);
        if (n.indexOf('AE.') === 0) {
            n = n.substring(3);
        }
        return n === MATCH_NAME;
    }

    /** Ticks of a Time object as a string; '' when unreadable. */
    function ticksOf(time) {
        try {
            if (time && time.ticks !== undefined && time.ticks !== null) {
                return String(time.ticks);
            }
        } catch (e) {
            // An unreadable time sorts as ''.
        }
        return '';
    }

    // ======================================================================
    //  The project and the active sequence
    // ======================================================================

    function currentProject() {
        try {
            return (app && app.project) ? app.project : null;
        } catch (e) {
            return null;
        }
    }

    function activeSequence() {
        var project = currentProject();
        if (!project) {
            return null;
        }
        try {
            return project.activeSequence || null;
        } catch (e) {
            return null;
        }
    }

    function sequenceIdOf(seq) {
        try {
            return String(seq.sequenceID);
        } catch (e) {
            return '';
        }
    }

    function projectIdOf(project) {
        try {
            if (project.documentID) {
                return String(project.documentID);
            }
            return String(project.path || '');
        } catch (e) {
            return '';
        }
    }

    /** Video track count of a sequence; 0 when unreadable. */
    function videoTrackCount(seq) {
        try {
            return seq.videoTracks ? Number(seq.videoTracks.numTracks) || 0 : 0;
        } catch (e) {
            return 0;
        }
    }

    function videoTrack(seq, index) {
        try {
            return seq.videoTracks[index] || null;
        } catch (e) {
            return null;
        }
    }

    function clipCount(track) {
        try {
            return track.clips ? Number(track.clips.numItems) || 0 : 0;
        } catch (e) {
            return 0;
        }
    }

    function clipAt(track, index) {
        try {
            return track.clips[index] || null;
        } catch (e) {
            return null;
        }
    }

    /** {osv, projectItemId, path} for a clip; cached per project item. */
    function mediaInfo(clip) {
        var project = currentProject();
        var pid = project ? projectIdOf(project) : '';
        if (pid !== pathCacheProject) {
            pathCache = {};
            pathCacheSize = 0;
            pathCacheProject = pid;
        }
        var item = null;
        try {
            item = clip.projectItem;
        } catch (e) {
            item = null;
        }
        if (!item) {
            return null;
        }
        var id = '';
        try {
            id = String(item.nodeId);
        } catch (e) {
            id = '';
        }
        if (id && pathCache.hasOwnProperty(id)) {
            return pathCache[id];
        }
        var isSequence = false;
        try {
            isSequence = item.isSequence() === true;
        } catch (e) {
            isSequence = false;
        }
        var path = '';
        if (!isSequence) {
            try {
                path = String(item.getMediaPath() || '');
            } catch (e) {
                path = '';
            }
        }
        var info = { osv: !isSequence && isOsvPath(path), projectItemId: id || ('path:' + path), path: path };
        if (id) {
            if (pathCacheSize >= PATH_CACHE_LIMIT) {
                pathCache = {};
                pathCacheSize = 0;
            }
            pathCache[id] = info;
            pathCacheSize += 1;
        }
        return info;
    }

    /** The match names of a clip's components, in order. */
    function componentMatchNames(clip) {
        var names = [];
        var comps = null;
        try {
            comps = clip.components;
        } catch (e) {
            comps = null;
        }
        if (!comps) {
            return names;
        }
        var n = 0;
        try {
            n = Number(comps.numItems) || 0;
        } catch (e) {
            n = 0;
        }
        for (var i = 0; i < n; i += 1) {
            try {
                names.push(String(comps[i].matchName));
            } catch (e) {
                names.push('');
            }
        }
        return names;
    }

    function countOurs(clip) {
        var names = componentMatchNames(clip);
        var count = 0;
        for (var i = 0; i < names.length; i += 1) {
            if (isOurMatchName(names[i])) {
                count += 1;
            }
        }
        return count;
    }

    /** The LAST Open 360 Reframe component of a clip (the newest), or null. */
    function lastOurComponent(clip) {
        var names = componentMatchNames(clip);
        for (var i = names.length - 1; i >= 0; i -= 1) {
            if (isOurMatchName(names[i])) {
                try {
                    return clip.components[i] || null;
                } catch (e) {
                    return null;
                }
            }
        }
        return null;
    }

    /** Find a video clip by nodeId: {clip, trackIndex, ordinal} or null. */
    function findClip(seq, nodeId) {
        var tracks = videoTrackCount(seq);
        for (var t = 0; t < tracks; t += 1) {
            var track = videoTrack(seq, t);
            if (!track) {
                continue;
            }
            var n = clipCount(track);
            for (var c = 0; c < n; c += 1) {
                var clip = clipAt(track, c);
                var id = '';
                try {
                    id = clip ? String(clip.nodeId) : '';
                } catch (e) {
                    id = '';
                }
                if (id !== '' && id === nodeId) {
                    return { clip: clip, trackIndex: t, ordinal: c };
                }
            }
        }
        return null;
    }

    /** The panel's description of one OSV clip. */
    function describe(clip, trackIndex, info) {
        var name = '';
        var id = '';
        try {
            name = String(clip.name);
        } catch (e) {
            name = '';
        }
        try {
            id = String(clip.nodeId);
        } catch (e) {
            id = '';
        }
        return {
            key: id,
            trackIndex: trackIndex,
            startTicks: ticksOf(clip.start),
            endTicks: ticksOf(clip.end),
            inTicks: ticksOf(clip.inPoint),
            outTicks: ticksOf(clip.outPoint),
            projectItemId: info.projectItemId,
            name: name,
            mediaPath: info.path
        };
    }

    /** The parameters of a component that the panel may write. */
    function readParams(component) {
        var params = [];
        var props = null;
        try {
            props = component.properties;
        } catch (e) {
            props = null;
        }
        if (!props) {
            return params;
        }
        var n = 0;
        try {
            n = Number(props.numItems) || 0;
        } catch (e) {
            n = 0;
        }
        for (var i = 0; i < n; i += 1) {
            var p = null;
            var name = '';
            try {
                p = props[i];
                name = trimString(p.displayName);
            } catch (e) {
                p = null;
            }
            if (!p || !WANTED_PARAMS.hasOwnProperty(name)) {
                continue;
            }
            var value = null;
            var timeVarying = false;
            try {
                value = p.getValue();
            } catch (e) {
                value = null;
            }
            try {
                timeVarying = p.isTimeVarying() === true;
            } catch (e) {
                timeVarying = false;
            }
            var t = typeof value;
            params.push({
                index: i,
                name: name,
                value: (t === 'number' || t === 'boolean' || t === 'string') ? value : null,
                timeVarying: timeVarying
            });
        }
        return params;
    }

    // ======================================================================
    //  QE (unofficial): the effect object and the clip to put it on
    // ======================================================================

    function enableQE() {
        try {
            app.enableQE();
        } catch (e) {
            // Reported by the caller when qe stays undefined.
        }
        return (typeof qe !== 'undefined' && qe && qe.project) ? qe : null;
    }

    /**
     * The QE effect object: by the registered match name first (the second
     * argument asks for a match-name lookup), then by the bare PiPL name,
     * then by the display name as Adobe's forum examples do.
     */
    function qeEffect() {
        var q = enableQE();
        if (!q) {
            return null;
        }
        var fx = null;
        try {
            fx = q.project.getVideoEffectByName(HOST_MATCH_NAME, true);
        } catch (e) {
            fx = null;
        }
        if (!fx) {
            try {
                fx = q.project.getVideoEffectByName(MATCH_NAME, true);
            } catch (e) {
                fx = null;
            }
        }
        if (!fx) {
            try {
                fx = q.project.getVideoEffectByName(DISPLAY_NAME);
            } catch (e) {
                fx = null;
            }
        }
        return fx || null;
    }

    /**
     * The QE item for a DOM clip.  QE lists the gaps between clips as items
     * of type "Empty", so a DOM index is not a QE index.  Match on the start
     * time; fall back to the n-th non-empty item only when its name agrees.
     */
    function qeItemFor(qeSeq, trackIndex, clip, ordinal) {
        var qeTrack = null;
        try {
            qeTrack = qeSeq.getVideoTrackAt(trackIndex);
        } catch (e) {
            qeTrack = null;
        }
        if (!qeTrack) {
            return null;
        }
        var want = ticksOf(clip.start);
        var clipName = '';
        try {
            clipName = String(clip.name);
        } catch (e) {
            clipName = '';
        }
        var n = 0;
        try {
            n = Number(qeTrack.numItems) || 0;
        } catch (e) {
            n = 0;
        }
        var seen = -1;
        var byOrdinal = null;
        for (var i = 0; i < n; i += 1) {
            var it = null;
            try {
                it = qeTrack.getItemAt(i);
            } catch (e) {
                it = null;
            }
            if (!it) {
                continue;
            }
            var type = '';
            try {
                type = String(it.type);
            } catch (e) {
                type = '';
            }
            if (type === 'Empty') {
                continue;
            }
            seen += 1;
            var start = '';
            try {
                start = it.start ? ticksOf(it.start) : '';
            } catch (e) {
                start = '';
            }
            if (start !== '' && start === want) {
                return it;
            }
            if (seen === ordinal) {
                byOrdinal = it;
            }
        }
        if (byOrdinal) {
            var name = '';
            try {
                name = String(byOrdinal.name);
            } catch (e) {
                name = '';
            }
            if (name === clipName) {
                return byOrdinal;
            }
        }
        return null;
    }

    // ======================================================================
    //  Events -> the panel
    // ======================================================================

    var plugplug = null;

    /** Tell the panel something changed (a CSXS event it listens for). */
    function notify(reason) {
        try {
            if (!plugplug) {
                plugplug = new ExternalObject('lib:' + (Folder.fs === 'Macintosh'
                    ? 'PlugPlugExternalObject' : 'PlugPlugExternalObject.dll'));
            }
            var e = new CSXSEvent();
            e.type = EVENT_TYPE;
            e.data = String(reason);
            e.dispatch();
        } catch (err) {
            // No bridge: the panel's poll still notices.
        }
    }

    // ======================================================================
    //  The API the panel calls (every function returns JSON text)
    // ======================================================================

    var api = {};

    api.ping = function () {
        var version = '';
        try {
            version = String(app.version);
        } catch (e) {
            version = '';
        }
        return okJson({ version: VERSION, app: version });
    };

    api.activeSequence = function () {
        try {
            var seq = activeSequence();
            if (!seq) {
                return okJson({ sequence: null });
            }
            var project = currentProject();
            return okJson({
                sequence: {
                    id: sequenceIdOf(seq),
                    name: String(seq.name),
                    projectId: project ? projectIdOf(project) : ''
                }
            });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    api.sequenceIds = function () {
        try {
            var project = currentProject();
            if (!project || !project.sequences) {
                return okJson({ ids: null });
            }
            var ids = [];
            var n = Number(project.sequences.numSequences) || 0;
            for (var i = 0; i < n; i += 1) {
                try {
                    ids.push(String(project.sequences[i].sequenceID));
                } catch (e) {
                    // One unreadable sequence does not hide the others.
                }
            }
            return okJson({ ids: ids });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /** Sequence id, track count and clips per track: cheap, changes on adds. */
    api.signature = function () {
        try {
            var seq = activeSequence();
            if (!seq) {
                return okJson({ signature: 'none' });
            }
            var parts = [sequenceIdOf(seq)];
            var tracks = videoTrackCount(seq);
            parts.push(String(tracks));
            for (var t = 0; t < tracks; t += 1) {
                var track = videoTrack(seq, t);
                parts.push(String(track ? clipCount(track) : 0));
            }
            return okJson({ signature: parts.join('|') });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /** The OSV clips of the active sequence (or of its selection). */
    api.scan = function (request) {
        try {
            var req = request || {};
            var seq = activeSequence();
            if (!seq || (req.sequenceId && sequenceIdOf(seq) !== String(req.sequenceId))) {
                return errorJson('the active sequence changed. Try again');
            }
            var selectedOnly = req.selectedOnly === true;
            var items = [];
            var other = 0;
            var tracks = videoTrackCount(seq);
            for (var t = 0; t < tracks; t += 1) {
                var track = videoTrack(seq, t);
                if (!track) {
                    continue;
                }
                var n = clipCount(track);
                for (var c = 0; c < n; c += 1) {
                    var clip = clipAt(track, c);
                    if (!clip) {
                        continue;
                    }
                    if (selectedOnly) {
                        var selected = false;
                        try {
                            selected = clip.isSelected() === true;
                        } catch (e) {
                            selected = false;
                        }
                        if (!selected) {
                            continue;
                        }
                    }
                    var info = mediaInfo(clip);
                    if (!info || !info.osv) {
                        if (selectedOnly) {
                            other += 1;
                        }
                        continue;
                    }
                    items.push(describe(clip, t, info));
                }
            }
            return okJson({ items: items, otherCount: other });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /**
     * Put the effect on the clips named by `request.keys` (DOM nodeIds).
     * Each clip is checked again right here, inside this one synchronous
     * call, so a clip that gained the effect since the panel looked is
     * counted as "already", never doubled.
     */
    api.apply = function (request) {
        try {
            var req = request || {};
            var seq = activeSequence();
            if (!seq || sequenceIdOf(seq) !== String(req.sequenceId)) {
                return errorJson('the active sequence changed. Try again');
            }
            var keys = isArray(req.keys) ? req.keys : [];
            var fx = qeEffect();
            if (!fx) {
                return okJson({ missingEffect: true, applied: [], already: 0, failed: 0, errors: [] });
            }
            var q = enableQE();
            var qeSeq = null;
            try {
                qeSeq = q ? q.project.getActiveSequence() : null;
            } catch (e) {
                qeSeq = null;
            }
            if (!qeSeq) {
                return errorJson('Premiere\'s QE interface has no active sequence');
            }
            var applied = [];
            var already = 0;
            var failed = 0;
            var errors = [];
            var fail = function (message) {
                failed += 1;
                if (errors.length < 5) {
                    errors.push(message);
                }
            };
            for (var i = 0; i < keys.length; i += 1) {
                var key = String(keys[i]);
                var found = findClip(seq, key);
                if (!found) {
                    fail('a clip moved before it could be updated');
                    continue;
                }
                var before = countOurs(found.clip);
                if (before > 0) {
                    already += 1;
                    continue;
                }
                var label = '';
                try {
                    label = String(found.clip.name);
                } catch (e) {
                    label = 'a clip';
                }
                // A fresh QE sequence per clip: QE's model is known to lag
                // behind edits, and the previous clip's effect was one.
                try {
                    qeSeq = q.project.getActiveSequence() || qeSeq;
                } catch (e) {
                    // Keep the one we have.
                }
                var qeItem = qeItemFor(qeSeq, found.trackIndex, found.clip, found.ordinal);
                if (!qeItem) {
                    fail('couldn\'t find "' + label + '" in Premiere\'s QE timeline');
                    continue;
                }
                try {
                    qeItem.addVideoEffect(fx);
                } catch (e) {
                    fail(messageOf(e));
                    continue;
                }
                // Trust the official DOM, not the QE call.
                var again = findClip(seq, key);
                if (!again || countOurs(again.clip) <= before) {
                    fail('Premiere didn\'t add the effect to "' + label + '"');
                    continue;
                }
                var component = lastOurComponent(again.clip);
                applied.push({ key: key, params: component ? readParams(component) : [] });
            }
            return okJson({ missingEffect: false, applied: applied, already: already, failed: failed, errors: errors });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /**
     * Write parameter values into the newest Open 360 Reframe of each clip.
     * Every write names the parameter it expects at that index, and is
     * refused if the name differs - an index is only trusted with its name.
     */
    api.setParams = function (request) {
        try {
            var req = request || {};
            var seq = activeSequence();
            if (!seq || sequenceIdOf(seq) !== String(req.sequenceId)) {
                return errorJson('the active sequence changed. Try again');
            }
            var writes = isArray(req.writes) ? req.writes : [];
            var written = 0;
            var failed = 0;
            var errors = [];
            for (var i = 0; i < writes.length; i += 1) {
                var w = writes[i] || {};
                var found = findClip(seq, String(w.key));
                var component = found ? lastOurComponent(found.clip) : null;
                var param = null;
                try {
                    param = component ? component.properties[Number(w.index)] : null;
                } catch (e) {
                    param = null;
                }
                var name = '';
                try {
                    name = param ? trimString(param.displayName) : '';
                } catch (e) {
                    name = '';
                }
                if (!param || name !== String(w.name)) {
                    failed += 1;
                    errors.push('couldn\'t find ' + String(w.name));
                    continue;
                }
                var keyframed = false;
                try {
                    keyframed = param.isTimeVarying() === true;
                } catch (e) {
                    keyframed = false;
                }
                if (keyframed) {
                    failed += 1;
                    errors.push(name + ' is keyframed');
                    continue;
                }
                try {
                    param.setValue(w.value, 1);
                    written += 1;
                } catch (e) {
                    failed += 1;
                    errors.push(messageOf(e));
                }
            }
            return okJson({ written: written, failed: failed, errors: errors });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /** Is Open 360 Reframe installed (as QE sees it)? */
    api.effectInfo = function () {
        try {
            if (!enableQE()) {
                return okJson({ available: null });
            }
            return okJson({ available: qeEffect() !== null });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /**
     * Bind the timeline events once per ExtendScript engine.  The handlers
     * look OpenOSVHost up at call time, so a panel reload that re-evaluates
     * this file keeps working without binding twice.
     */
    api.bindEvents = function () {
        try {
            if ($.global.__openosvBound === true) {
                return okJson({ bound: true, already: true });
            }
            var bound = 0;
            var names = ['onActiveSequenceTrackItemAdded', 'onActiveSequenceChanged', 'onActiveSequenceStructureChanged'];
            for (var i = 0; i < names.length; i += 1) {
                try {
                    var reason = names[i];
                    if (app.bind(reason, makeHandler(reason)) !== false) {
                        bound += 1;
                    }
                } catch (e) {
                    // One missing event does not stop the others.
                }
            }
            $.global.__openosvBound = bound > 0;
            return okJson({ bound: bound > 0, count: bound });
        } catch (e) {
            return errorJson(messageOf(e));
        }
    };

    /** A handler that forwards to whatever OpenOSVHost is current. */
    function makeHandler(reason) {
        return function () {
            try {
                $.global.OpenOSVHost._notify(reason);
            } catch (e) {
                // Nothing to report to.
            }
        };
    }

    // Exposed for the handlers above and for the tests.
    api._notify = notify;
    api._isOsvPath = isOsvPath;
    api._isOurMatchName = isOurMatchName;
    api._toJson = toJson;
    api.VERSION = VERSION;
    return api;
}());

// Make it reachable from the event handlers regardless of how this file was
// evaluated (manifest ScriptPath or $.evalFile).
$.global.OpenOSVHost = OpenOSVHost;
