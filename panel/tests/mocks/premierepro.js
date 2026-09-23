// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// premierepro.js - a stand-in for Premiere's UXP `premierepro` module.
//
// Every class and method here follows the shapes documented in Adobe's
// Premiere UXP reference (developer.adobe.com/premiere-pro/uxp/ppro-reference,
// 25.6): which calls return Promises and which are synchronous, Guid.toString,
// TickTime.ticks as a string, Keyframe.value.value, and - the part that
// matters most - that Actions must be created inside Project.lockedAccess()
// (enforced since 26.3) and are only applied by Project.executeTransaction().
// The mock throws where Premiere would refuse, so the adapter is tested
// against the rules and not just the happy path.
//
// The timeline model is plain data the tests build and mutate:
//   world.project.sequences[i] = { guid, name, tracks: [[item, ...], ...] }
//   item = { name, start, end, inPoint, outPoint, selected, projectItem, components: [component] }
//   projectItem = { id, path, isSequence }
//   component = { matchName, params: [{ displayName, value, timeVarying }] }
'use strict';

/** The parameter list of a fresh Open 360 Reframe, as the host would list it. */
function reframeParams(popupBase) {
    const b = popupBase;
    return [
        { displayName: 'Output Resolution', value: b + 0, timeVarying: false },
        { displayName: 'Camera', value: null, timeVarying: false },
        { displayName: 'Preset', value: b + 3, timeVarying: false },
        { displayName: 'Pan', value: 0, timeVarying: false },
        { displayName: 'Tilt', value: 0, timeVarying: false },
        { displayName: 'Roll', value: 0, timeVarying: false },
        { displayName: 'FOV', value: 120, timeVarying: false },
        { displayName: 'Distortion', value: 15, timeVarying: false },
        { displayName: 'Smooth Keyframes', value: false, timeVarying: false },
        { displayName: 'Camera Model', value: true, timeVarying: false },
        { displayName: 'Zoom', value: 142.4, timeVarying: false },
        { displayName: 'FOV', value: 60, timeVarying: false },
        { displayName: 'Correction Angle', value: 0.6, timeVarying: false },
        { displayName: 'Drag Sensitivity', value: 2.0, timeVarying: false },
        { displayName: 'Lens', value: b + 0, timeVarying: false }
    ];
}

function createMockPremiere(options) {
    const opts = options || {};
    const popupBase = opts.popupBase === 1 ? 1 : 0;
    const log = [];                  // every mutation, for assertions
    const undo = [];                 // executeTransaction undo names
    let locked = false;
    const listeners = [];            // {target, name, fn}
    const globals = [];              // {name, fn}
    const installed = opts.installed !== false;
    const matchNames = ['AE.ADBE Motion', 'PR.ADBE Gamma Correction'].concat(installed ? ['AE.OpenOSV.Open360Reframe'] : []);

    const world = {
        project: { guid: 'proj-1', name: 'Project', sequences: [], active: 0 },
        failTransaction: false,
        log: log,
        undo: undo
    };

    // ---- value classes ------------------------------------------------------
    class Guid {
        constructor(text) { this._t = String(text); }
        toString() { return this._t; }
        static fromString(s) { return new Guid(s); }
    }
    class TickTime {
        constructor(ticks) { this.ticksNumber = Number(ticks); this.ticks = String(ticks); this.seconds = Number(ticks) / 254016000000; }
    }
    const Constants = {
        TrackItemType: { EMPTY: 0, CLIP: 1, TRANSITION: 2, PREVIEW: 3, FEEDBACK: 4 },
        VideoTrackEvent: { TRACK_CHANGED: 'ppro.videotrack.trackchanged', INFO_CHANGED: 'ppro.videotrack.info', LOCK_CHANGED: 'ppro.videotrack.lock' },
        SequenceEvent: { ACTIVATED: 'ppro.sequence.activated', CLOSED: 'ppro.sequence.closed', SELECTION_CHANGED: 'ppro.sequence.selection' },
        ProjectEvent: { OPENED: 'ppro.project.opened', CLOSED: 'ppro.project.closed', DIRTY: 'ppro.project.dirty', ACTIVATED: 'ppro.project.activated' }
    };

    function requireLock(what) {
        if (!locked) {
            throw new Error(what + ' must be created inside project.lockedAccess()');
        }
    }

    // ---- wrappers over the plain model --------------------------------------
    // Wrappers are created fresh on every call, like the real API hands out
    // new JS objects; identity is the underlying model object.
    function action(apply, label) {
        return { _apply: apply, _label: label };
    }

    function paramWrap(component, index) {
        const p = component.params[index];
        return {
            displayName: p.displayName,
            getStartValue: async () => ({ value: { value: p.value }, position: new TickTime(0) }),
            isTimeVarying: () => p.timeVarying === true,
            createKeyframe: (value) => {
                if (typeof value !== typeof p.value && !(p.value === null)) {
                    throw new Error('value type does not match the parameter');
                }
                return { value: { value: value }, position: new TickTime(0) };
            },
            createSetValueAction: (keyframe, safe) => {
                requireLock('createSetValueAction');
                return action(() => {
                    p.value = keyframe.value.value;
                    log.push(['setValue', p.displayName, p.value]);
                }, 'setValue');
            }
        };
    }

    function componentWrap(component) {
        return {
            _model: component,
            getMatchName: async () => component.matchName,
            getDisplayName: async () => component.matchName,
            getParamCount: () => component.params.length,
            getParam: (i) => {
                if (i < 0 || i >= component.params.length) {
                    throw new Error('param index out of range');
                }
                return paramWrap(component, i);
            }
        };
    }

    function chainWrap(item) {
        return {
            getComponentCount: () => item.components.length,
            getComponentAtIndex: (i) => (i >= 0 && i < item.components.length) ? componentWrap(item.components[i]) : null,
            createAppendComponentAction: (component) => {
                requireLock('createAppendComponentAction');
                if (!component || !component._model) {
                    throw new Error('not a component');
                }
                return action(() => {
                    item.components.push(component._model);
                    log.push(['append', item.name, component._model.matchName]);
                }, 'append');
            },
            createInsertComponentAction: (component, index) => {
                requireLock('createInsertComponentAction');
                return action(() => { item.components.splice(index, 0, component._model); }, 'insert');
            }
        };
    }

    function projectItemWrap(pi) {
        return { _model: pi, name: pi.path, type: 1, getId: () => pi.id };
    }

    function itemWrap(item, trackIndex) {
        return {
            _model: item,
            getProjectItem: async () => item.projectItem ? projectItemWrap(item.projectItem) : null,
            getStartTime: async () => new TickTime(item.start),
            getEndTime: async () => new TickTime(item.end),
            getInPoint: async () => new TickTime(item.inPoint),
            getOutPoint: async () => new TickTime(item.outPoint),
            getName: async () => item.name,
            getIsSelected: async () => item.selected === true,
            getTrackIndex: async () => trackIndex,
            getComponentChain: async () => chainWrap(item),
            isAdjustmentLayer: async () => false
        };
    }

    function trackWrap(seq, index) {
        return {
            _seq: seq,
            _index: index,
            id: index,
            name: 'V' + (index + 1),
            getIndex: async () => index,
            getTrackItems: (type, includeEmpty) => {
                if (type !== Constants.TrackItemType.CLIP) {
                    return [];
                }
                return (seq.tracks[index] || []).map((it) => itemWrap(it, index));
            }
        };
    }

    function sequenceWrap(seq) {
        return {
            _model: seq,
            guid: new Guid(seq.guid),
            name: seq.name,
            getVideoTrackCount: async () => seq.tracks.length,
            getVideoTrack: async (i) => (i >= 0 && i < seq.tracks.length) ? trackWrap(seq, i) : null
        };
    }

    const projectWrap = {
        guid: new Guid(world.project.guid),
        name: 'Project',
        path: 'C:/p.prproj',
        getActiveSequence: async () => {
            const s = world.project.sequences[world.project.active];
            return s ? sequenceWrap(s) : null;
        },
        getSequences: async () => world.project.sequences.map(sequenceWrap),
        lockedAccess: (cb) => {
            if (locked) {
                throw new Error('lockedAccess is not re-entrant');
            }
            locked = true;
            try {
                cb();
            } finally {
                locked = false;
            }
        },
        executeTransaction: (cb, undoName) => {
            if (!locked) {
                throw new Error('executeTransaction outside lockedAccess');
            }
            const actions = [];
            cb({ addAction: (a) => { actions.push(a); return true; } });
            if (world.failTransaction) {
                return false;
            }
            actions.forEach((a) => a._apply());
            undo.push(undoName);
            return true;
        }
    };

    // ---- the module ------------------------------------------------------------
    const ppro = {
        Constants: Constants,
        Guid: Guid,
        TickTime: TickTime,
        Project: {
            getActiveProject: async () => projectWrap
        },
        ClipProjectItem: {
            cast: (pi) => {
                if (!pi || !pi._model) {
                    return null;
                }
                const m = pi._model;
                return {
                    getId: () => m.id,
                    isSequence: async () => m.isSequence === true,
                    getMediaFilePath: async () => m.isSequence ? '' : m.path
                };
            }
        },
        VideoFilterFactory: {
            getMatchNames: async () => matchNames.slice(),
            createComponent: async (name) => {
                if (matchNames.indexOf(name) === -1) {
                    throw new Error('no such effect: ' + name);
                }
                const params = name === 'AE.OpenOSV.Open360Reframe' ? reframeParams(popupBase) : [];
                return componentWrap({ matchName: name, params: params });
            }
        },
        EventManager: {
            addEventListener: (target, name, fn) => { listeners.push({ target, name, fn }); },
            removeEventListener: (target, name, fn) => {
                const i = listeners.findIndex((l) => l.target === target && l.name === name && l.fn === fn);
                if (i >= 0) {
                    listeners.splice(i, 1);
                }
            },
            addGlobalEventListener: (name, fn) => { globals.push({ name, fn }); },
            removeGlobalEventListener: (name, fn) => {
                const i = globals.findIndex((l) => l.name === name && l.fn === fn);
                if (i >= 0) {
                    globals.splice(i, 1);
                }
            }
        }
    };

    // ---- test helpers ------------------------------------------------------------
    const helpers = {
        world: world,
        ppro: ppro,
        reframeParams: reframeParams,
        /** Add a sequence and return its model. */
        addSequence(guid, name, trackCount) {
            const seq = { guid: guid, name: name, tracks: [] };
            for (let i = 0; i < trackCount; i += 1) {
                seq.tracks.push([]);
            }
            world.project.sequences.push(seq);
            return seq;
        },
        /** Put a clip on a track and return its model. */
        addClip(seq, trackIndex, spec) {
            const item = Object.assign({
                name: 'clip',
                start: 0,
                end: 100,
                inPoint: 0,
                outPoint: 100,
                selected: false,
                projectItem: { id: 'pi-' + Math.random().toString(36).slice(2), path: 'C:/media/clip.mp4', isSequence: false },
                components: [
                    { matchName: 'AE.ADBE Opacity', params: [] },
                    { matchName: 'AE.ADBE Motion', params: [] }
                ]
            }, spec);
            while (seq.tracks.length <= trackIndex) {
                seq.tracks.push([]);
            }
            seq.tracks[trackIndex].push(item);
            seq.tracks[trackIndex].sort((a, b) => a.start - b.start);
            return item;
        },
        /** Fire TRACK_CHANGED on every listener attached to a track of `seq`. */
        fireTrackChanged(seq, trackIndex) {
            let fired = 0;
            listeners.slice().forEach((l) => {
                if (l.name === Constants.VideoTrackEvent.TRACK_CHANGED && l.target && l.target._seq === seq &&
                    (trackIndex === undefined || l.target._index === trackIndex)) {
                    l.fn({ track: l.target });
                    fired += 1;
                }
            });
            return fired;
        },
        fireGlobal(name) {
            globals.slice().forEach((g) => { if (g.name === name) { g.fn({}); } });
        },
        listenerCount() { return listeners.length; },
        globalCount() { return globals.length; },
        reframeCount(item) {
            return item.components.filter((c) => c.matchName === 'AE.OpenOSV.Open360Reframe').length;
        },
        lastReframe(item) {
            const list = item.components.filter((c) => c.matchName === 'AE.OpenOSV.Open360Reframe');
            return list[list.length - 1] || null;
        },
        paramValue(component, name) {
            const p = component.params.filter((x) => x.displayName === name)[0];
            return p ? p.value : undefined;
        }
    };
    return helpers;
}

module.exports = { createMockPremiere };
