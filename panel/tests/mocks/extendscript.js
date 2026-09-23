// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// extendscript.js - a small ExtendScript world for running host.jsx in Node.
//
// It models the parts of Premiere's ExtendScript DOM that host.jsx touches,
// as documented (ppro-scripting.docsforadobe.dev) and as Adobe's PProPanel
// sample uses them: collections with numItems / numTracks / numSequences and
// index access, Time objects carrying `ticks` as a string, methods where the
// DOM has methods (isSelected(), getMediaPath(), isSequence(), getValue(),
// setValue(), isTimeVarying()) and properties where it has properties.
//
// The QE side models the documented community knowledge the host relies on:
// a QE track lists the GAPS between clips as items of type "Empty", so its
// indices differ from the DOM's; getVideoEffectByName(name, true) looks up a
// match name; addVideoEffect() appends to the clip.  It can also be told to
// misbehave (an addVideoEffect that silently does nothing, QE items without
// a readable start) so the host's verification paths are exercised.
//
// Everything runs inside a vm context; `world` is shared with the test.
'use strict';

const vm = require('node:vm');
const fs = require('node:fs');

/** An array that also answers numItems / numTracks / numSequences, live. */
function collection(array, countName) {
    Object.defineProperty(array, countName, { get() { return this.length; }, enumerable: false });
    return array;
}

function time(ticks) {
    return { ticks: String(ticks), seconds: Number(ticks) / 254016000000 };
}

function createWorld(options) {
    const opts = options || {};
    const popupBase = opts.popupBase === 1 ? 1 : 0;
    const world = {
        installed: opts.installed !== false,
        qeAddsNothing: false,     // addVideoEffect returns but changes nothing
        qeNoStart: false,         // QE items have no readable start time
        qeEnabled: false,
        dispatched: [],           // CSXS events host.jsx dispatched
        bound: {},                // app.bind registrations
        setValueCalls: [],
        project: null,
        sequences: [],
        activeIndex: 0,
        nextNode: 1000
    };

    function reframeComponent() {
        const b = popupBase;
        const params = [
            ['Output Resolution', b + 0], ['Camera', null], ['Preset', b + 3], ['Pan', 0], ['Tilt', 0], ['Roll', 0],
            ['FOV', 120], ['Distortion', 15], ['Smooth Keyframes', false], ['Camera Model', true], ['Zoom', 142.4],
            ['FOV', 60], ['Correction Angle', 0.6], ['Drag Sensitivity', 2], ['Lens', b + 0]
        ].map(([name, value]) => makeParam(name, value));
        return { matchName: 'AE.OpenOSV.Open360Reframe', displayName: 'Open 360 Reframe', properties: collection(params, 'numItems') };
    }

    function makeParam(name, value) {
        const p = {
            displayName: name,
            _value: value,
            _timeVarying: false,
            getValue() { return this._value; },
            setValue(v, updateUI) {
                world.setValueCalls.push([name, v, updateUI]);
                this._value = v;
                return 0;
            },
            isTimeVarying() { return this._timeVarying; }
        };
        return p;
    }

    function makeClip(spec) {
        const clip = {
            nodeId: String(world.nextNode++),
            name: spec.name || 'clip',
            start: time(spec.start || 0),
            end: time(spec.end === undefined ? 100 : spec.end),
            inPoint: time(spec.inPoint || 0),
            outPoint: time(spec.outPoint === undefined ? 100 : spec.outPoint),
            _selected: spec.selected === true,
            isSelected() { return this._selected; },
            projectItem: spec.projectItem === null ? null : {
                nodeId: spec.projectItemId || ('pi-' + world.nextNode++),
                name: spec.name || 'clip',
                _path: spec.path || 'C:/media/clip.mp4',
                _isSequence: spec.isSequence === true,
                getMediaPath() { return this._isSequence ? '' : this._path; },
                isSequence() { return this._isSequence; }
            },
            components: collection([
                { matchName: 'AE.ADBE Opacity', displayName: 'Opacity', properties: collection([], 'numItems') },
                { matchName: 'AE.ADBE Motion', displayName: 'Motion', properties: collection([], 'numItems') }
            ], 'numItems')
        };
        return clip;
    }

    // ---- DOM ------------------------------------------------------------------
    function addSequence(id, name, trackCount) {
        const tracks = [];
        for (let i = 0; i < trackCount; i += 1) {
            tracks.push({ name: 'V' + (i + 1), clips: collection([], 'numItems') });
        }
        const seq = { sequenceID: id, name: name, videoTracks: collection(tracks, 'numTracks') };
        world.sequences.push(seq);
        return seq;
    }

    function addClip(seq, trackIndex, spec) {
        const clip = makeClip(spec || {});
        const clips = seq.videoTracks[trackIndex].clips;
        clips.push(clip);
        clips.sort((a, b) => Number(a.start.ticks) - Number(b.start.ticks));
        return clip;
    }

    const sequences = collection([], 'numSequences');
    const app = {
        version: '26.2.2',
        project: {
            documentID: 'doc-1',
            path: 'C:/p.prproj',
            sequences: sequences,
            get activeSequence() { return world.sequences[world.activeIndex] || null; }
        },
        enableQE() {
            world.qeEnabled = true;
            context.qe = qe;
            return true;
        },
        bind(name, fn) {
            world.bound[name] = fn;
            return true;
        }
    };
    // Keep app.project.sequences in step with world.sequences.
    const origPush = world.sequences.push.bind(world.sequences);
    world.sequences.push = function (s) { sequences.push(s); return origPush(s); };

    // ---- QE ---------------------------------------------------------------------
    function qeTrackFor(seq, trackIndex) {
        const clips = seq.videoTracks[trackIndex].clips;
        const items = [];
        let cursor = 0;
        for (let i = 0; i < clips.length; i += 1) {
            const c = clips[i];
            const start = Number(c.start.ticks);
            if (start > cursor) {
                // The gap before this clip is an item of its own.
                items.push({ type: 'Empty', name: '', start: world.qeNoStart ? undefined : time(cursor) });
            }
            items.push({
                type: 'Clip',
                name: c.name,
                start: world.qeNoStart ? undefined : time(start),
                addVideoEffect(fx) {
                    if (!fx || fx.__kind !== 'effect') {
                        throw new Error('bad effect');
                    }
                    if (!world.qeAddsNothing) {
                        c.components.push(reframeComponent());
                    }
                    return true;
                }
            });
            cursor = Number(c.end.ticks);
        }
        return { numItems: items.length, getItemAt(i) { return items[i] || null; } };
    }

    const qe = {
        project: {
            getVideoEffectByName(name, byMatchName) {
                if (!world.installed) {
                    return null;
                }
                const ok = byMatchName ? name === 'AE.OpenOSV.Open360Reframe' : name === 'Open 360 Reframe';
                return ok ? { __kind: 'effect', name: 'Open 360 Reframe' } : null;
            },
            getActiveSequence() {
                const seq = world.sequences[world.activeIndex];
                if (!seq) {
                    return null;
                }
                return { name: seq.name, getVideoTrackAt(i) { return qeTrackFor(seq, i); } };
            }
        }
    };

    // ---- globals the host expects ---------------------------------------------------
    function CSXSEvent() {
        this.type = '';
        this.data = '';
    }
    CSXSEvent.prototype.dispatch = function () {
        world.dispatched.push({ type: this.type, data: this.data });
    };
    function ExternalObject(spec) {
        this.spec = spec;
    }

    const context = vm.createContext({
        app: app,
        Folder: { fs: 'Windows' },
        CSXSEvent: CSXSEvent,
        ExternalObject: ExternalObject
    });
    context.$ = { global: context };

    /** Evaluate host.jsx (or any source) in the world. */
    function load(file) {
        vm.runInContext(fs.readFileSync(file, 'utf8'), context, { filename: file });
    }

    /** Evaluate an expression the way CEP's evalScript does: the result as a string. */
    function evalScript(script) {
        try {
            const result = vm.runInContext(script, context);
            return result === undefined ? 'undefined' : String(result);
        } catch (err) {
            world.lastEvalError = err;
            return 'EvalScript error.';
        }
    }

    return {
        world: world,
        context: context,
        addSequence: addSequence,
        addClip: addClip,
        reframeComponent: reframeComponent,
        load: load,
        evalScript: evalScript,
        /** Fire an app.bind() event the way Premiere would. */
        fire(name, a, b) {
            const fn = world.bound[name];
            if (typeof fn === 'function') {
                fn(a, b);
            }
        },
        reframeCount(clip) {
            let n = 0;
            for (let i = 0; i < clip.components.numItems; i += 1) {
                if (clip.components[i].matchName === 'AE.OpenOSV.Open360Reframe') {
                    n += 1;
                }
            }
            return n;
        },
        lastReframe(clip) {
            for (let i = clip.components.numItems - 1; i >= 0; i -= 1) {
                if (clip.components[i].matchName === 'AE.OpenOSV.Open360Reframe') {
                    return clip.components[i];
                }
            }
            return null;
        },
        paramValue(component, name) {
            for (let i = 0; i < component.properties.numItems; i += 1) {
                if (component.properties[i].displayName === name) {
                    return component.properties[i].getValue();
                }
            }
            return undefined;
        }
    };
}

module.exports = { createWorld };
