// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// hostjsx.test.js - the ExtendScript half of the CEP panel, run in Node
// against a mock of Premiere's ExtendScript + QE DOM (mocks/extendscript.js).
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const core = require('../shared/osvcore.js');
const { createWorld } = require('./mocks/extendscript.js');
const { OSV_PATHS, NOT_OSV_PATHS } = require('./fixtures/paths.js');

const HOST = path.join(__dirname, '..', 'cep', 'host', 'host.jsx');

/** A world with host.jsx loaded and one sequence of two tracks. */
function boot(options) {
    const w = createWorld(options);
    const seq = w.addSequence('seq-1', 'Main', 2);
    w.load(HOST);
    const call = (fn, arg) => {
        const text = w.evalScript('OpenOSVHost.' + fn + '(' + (arg === undefined ? '' : core.toExtendScriptLiteral(arg)) + ')');
        return JSON.parse(text);
    };
    return { w, seq, call };
}

function osv(w, seq, track, start, extra) {
    return w.addClip(seq, track, Object.assign({
        name: 'CAM_' + start + '.OSV',
        start: start,
        end: start + 1000,
        path: 'C:\\DCIM\\CAM_' + start + '.OSV',
        projectItemId: 'pi-' + track + '-' + start
    }, extra || {}));
}

test('host.jsx is ES3: no constructs ExtendScript cannot parse or run', () => {
    const src = fs.readFileSync(HOST, 'utf8')
        // Strip comments and strings first, so prose cannot trip the scan.
        .replace(/\/\*[\s\S]*?\*\//g, '')
        .replace(/\/\/[^\n]*/g, '')
        .replace(/'(?:\\.|[^'\\\n])*'/g, "''")
        .replace(/"(?:\\.|[^"\\\n])*"/g, '""');
    const forbidden = [
        [/\blet\s/, 'let'], [/\bconst\s/, 'const'], [/=>/, 'arrow functions'], [/`/, 'template literals'],
        [/\bclass\s/, 'class'], [/\.forEach\s*\(/, 'Array.forEach'], [/\.map\s*\(/, 'Array.map'],
        [/\.filter\s*\(/, 'Array.filter'], [/\.trim\s*\(/, 'String.trim'], [/\bJSON\./, 'JSON'],
        [/Object\.keys/, 'Object.keys'], [/Object\.assign/, 'Object.assign'], [/Array\.isArray/, 'Array.isArray'],
        [/\.includes\s*\(/, 'includes'], [/\?\./, 'optional chaining'], [/\?\?/, 'nullish coalescing'],
        [/\.\.\./, 'spread'], [/\bPromise\b/, 'Promise'], [/\bnew Map\b/, 'Map'], [/\bnew Set\b/, 'Set']
    ];
    for (const [re, what] of forbidden) {
        assert.equal(re.test(src), false, 'host.jsx uses ' + what);
    }
    // Array.indexOf is ES5; String.indexOf is fine.  Every .indexOf in the
    // host must be on a string - checked by listing them.
    const uses = src.match(/[\w.\]\)]+\.indexOf\(/g) || [];
    for (const u of uses) {
        assert.match(u, /^(n|p|name|path)\.indexOf\($/, 'indexOf on something that may be an array: ' + u);
    }
});

test('host.jsx identity matches OsvCore', () => {
    const { w } = boot();
    const host = w.context.OpenOSVHost;
    assert.equal(host.VERSION, core.PANEL_VERSION);
    const src = fs.readFileSync(HOST, 'utf8');
    assert.ok(src.includes("var MATCH_NAME = '" + core.REFRAME_MATCH_NAME + "'"));
    assert.ok(src.includes("var HOST_MATCH_NAME = '" + core.REFRAME_HOST_MATCH_NAME + "'"));
    assert.ok(src.includes("var DISPLAY_NAME = '" + core.REFRAME_DISPLAY_NAME + "'"));
    for (const name of Object.values(core.PARAM_NAMES)) {
        assert.ok(src.includes("'" + name + "': true"), 'WANTED_PARAMS lists ' + name);
    }
});

test('the ExtendScript OSV rule agrees with OsvCore on the whole corpus', () => {
    const { w } = boot();
    const isOsv = w.context.OpenOSVHost._isOsvPath;
    for (const p of OSV_PATHS.concat(NOT_OSV_PATHS)) {
        assert.equal(isOsv(p), core.isOsvMediaPath(p), JSON.stringify(p));
    }
    for (const v of [null, undefined, 3, {}]) {
        assert.equal(isOsv(v), false);
    }
    for (const n of ['AE.OpenOSV.Open360Reframe', 'OpenOSV.Open360Reframe', 'AE.ADBE Motion', '', null]) {
        assert.equal(w.context.OpenOSVHost._isOurMatchName(n), core.isReframeMatchName(n), String(n));
    }
});

test('toJson escapes everything JSON.parse and ES3 need', () => {
    const { w } = boot();
    const toJson = w.context.OpenOSVHost._toJson;
    const nasty = 'q"b\\s\n\t\u0001' + String.fromCharCode(0x2028) + String.fromCharCode(0x2029) + '\u007f';
    const text = toJson({ a: [1, 'x', true, null, undefined, NaN], s: nasty, f: function () {} });
    assert.equal(text.indexOf(String.fromCharCode(0x2028)), -1);
    assert.deepEqual(JSON.parse(text), { a: [1, 'x', true, null, null, null], s: nasty });
});

test('activeSequence, sequenceIds and signature describe the timeline', () => {
    const { w, seq, call } = boot();
    osv(w, seq, 0, 0);
    assert.deepEqual(call('activeSequence'), { ok: true, sequence: { id: 'seq-1', name: 'Main', projectId: 'doc-1' } });
    assert.deepEqual(call('sequenceIds'), { ok: true, ids: ['seq-1'] });
    const sig = call('signature').signature;
    assert.equal(sig, 'seq-1|2|1|0');
    osv(w, seq, 1, 0);
    assert.notEqual(call('signature').signature, sig);
    w.world.activeIndex = 5;
    assert.deepEqual(call('activeSequence'), { ok: true, sequence: null });
});

test('scan returns OSV clips only, with nodeId keys and source times', () => {
    const { w, seq, call } = boot();
    const a = osv(w, seq, 0, 0, { inPoint: 250, outPoint: 1250 });
    w.addClip(seq, 0, { name: 'b-roll', start: 2000, end: 3000, path: 'C:/b.mp4' });
    w.addClip(seq, 1, { name: 'nested', start: 0, end: 10, path: 'x.osv', isSequence: true });
    w.addClip(seq, 1, { name: 'orphan', start: 20, end: 30, projectItem: null });
    const r = call('scan', { sequenceId: 'seq-1' });
    assert.equal(r.ok, true);
    assert.equal(r.items.length, 1);
    assert.deepEqual(r.items[0], {
        key: a.nodeId, trackIndex: 0, startTicks: '0', endTicks: '1000', inTicks: '250', outTicks: '1250',
        projectItemId: 'pi-0-0', name: 'CAM_0.OSV', mediaPath: 'C:\\DCIM\\CAM_0.OSV'
    });
});

test('scan of the selection counts non-OSV selected clips', () => {
    const { w, seq, call } = boot();
    osv(w, seq, 0, 0, { selected: true });
    osv(w, seq, 0, 5000, { selected: false });
    w.addClip(seq, 1, { name: 'b', start: 0, end: 5, selected: true });
    const r = call('scan', { sequenceId: 'seq-1', selectedOnly: true });
    assert.equal(r.items.length, 1);
    assert.equal(r.otherCount, 1);
});

test('scan refuses a sequence that is no longer active', () => {
    const { call } = boot();
    assert.deepEqual(call('scan', { sequenceId: 'other' }), { ok: false, error: 'the active sequence changed. Try again' });
});

test('apply finds the right QE item across gaps and verifies through the DOM', () => {
    const { w, seq, call } = boot();
    // Gaps before and between clips: QE lists them as "Empty" items.
    const a = osv(w, seq, 0, 3000);
    const b = osv(w, seq, 0, 9000);
    const r = call('apply', { sequenceId: 'seq-1', keys: [b.nodeId] });
    assert.equal(r.ok, true);
    assert.equal(r.applied.length, 1);
    assert.equal(w.reframeCount(a), 0, 'the neighbour is untouched');
    assert.equal(w.reframeCount(b), 1);
    // The new instance's parameters come back for the panel to plan with.
    const lens = r.applied[0].params.filter((p) => p.name === 'Lens')[0];
    assert.deepEqual(lens, { index: 14, name: 'Lens', value: 0, timeVarying: false });
    // [WP-EASING] The panel now reads the camera controls too.
    assert.deepEqual(r.applied[0].params.map((p) => p.name).sort(),
                     ['Camera Model', 'Correction Angle', 'Distortion', 'Drag Sensitivity', 'FOV', 'FOV', 'Keyframe Easing',
                      'Lens', 'Output Resolution', 'Pan', 'Preset', 'Roll', 'Tilt', 'Zoom']);
});

test('apply matches by position among clips when QE gives no start time', () => {
    const { w, seq, call } = boot();
    const a = osv(w, seq, 0, 3000);
    const b = osv(w, seq, 0, 9000);
    w.world.qeNoStart = true;
    const r = call('apply', { sequenceId: 'seq-1', keys: [a.nodeId, b.nodeId] });
    assert.equal(r.applied.length, 2);
    assert.equal(w.reframeCount(a), 1);
    assert.equal(w.reframeCount(b), 1);
});

test('apply never doubles an existing effect', () => {
    const { w, seq, call } = boot();
    const a = osv(w, seq, 0, 0);
    a.components.push(w.reframeComponent());
    const r = call('apply', { sequenceId: 'seq-1', keys: [a.nodeId] });
    assert.equal(r.already, 1);
    assert.equal(r.applied.length, 0);
    assert.equal(w.reframeCount(a), 1);
});

test('a QE call that silently does nothing is caught by the DOM check', () => {
    const { w, seq, call } = boot();
    const a = osv(w, seq, 0, 0);
    w.world.qeAddsNothing = true;
    const r = call('apply', { sequenceId: 'seq-1', keys: [a.nodeId] });
    assert.equal(r.applied.length, 0);
    assert.equal(r.failed, 1);
    assert.match(r.errors[0], /didn't add the effect/);
});

test('apply reports a missing effect and a vanished clip plainly', () => {
    const none = boot({ installed: false });
    const a = osv(none.w, none.seq, 0, 0);
    assert.equal(none.call('apply', { sequenceId: 'seq-1', keys: [a.nodeId] }).missingEffect, true);
    assert.deepEqual(none.call('effectInfo'), { ok: true, available: false });

    const { call } = boot();
    const r = call('apply', { sequenceId: 'seq-1', keys: ['no-such-node'] });
    assert.equal(r.failed, 1);
    assert.match(r.errors[0], /moved before it could be updated/);
});

test('setParams writes by index only when the name agrees, never over keyframes', () => {
    const { w, seq, call } = boot();
    const a = osv(w, seq, 0, 0);
    call('apply', { sequenceId: 'seq-1', keys: [a.nodeId] });
    const fx = w.lastReframe(a);
    let r = call('setParams', {
        sequenceId: 'seq-1',
        writes: [{ key: a.nodeId, index: 14, name: 'Lens', value: 1 }, { key: a.nodeId, index: 13, name: 'Lens', value: 1 }]
    });
    assert.equal(r.written, 1);
    assert.equal(r.failed, 1, 'index 13 is Drag Sensitivity, not Lens');
    assert.equal(w.paramValue(fx, 'Lens'), 1);
    assert.equal(w.paramValue(fx, 'Drag Sensitivity'), 2);
    fx.properties[13]._timeVarying = true;
    r = call('setParams', { sequenceId: 'seq-1', writes: [{ key: a.nodeId, index: 13, name: 'Drag Sensitivity', value: 4 }] });
    assert.equal(r.written, 0);
    assert.match(r.errors[0], /keyframed/);
});

test('bindEvents binds the timeline events once, and they reach the panel', () => {
    const { w, call } = boot();
    // [WP-EASING] The selection event moves the Manual Framing read-outs.
    assert.deepEqual(call('bindEvents'), { ok: true, bound: true, count: 4 });
    assert.deepEqual(Object.keys(w.world.bound).sort(),
                     ['onActiveSequenceChanged', 'onActiveSequenceSelectionChanged', 'onActiveSequenceStructureChanged',
                      'onActiveSequenceTrackItemAdded']);
    assert.equal(call('bindEvents').already, true, 'a reloaded panel does not bind twice');
    w.fire('onActiveSequenceTrackItemAdded', {}, {});
    assert.deepEqual(w.world.dispatched, [{ type: 'com.openosv.panel.hostchange', data: 'onActiveSequenceTrackItemAdded' }]);
});

test('every entry point answers JSON even when the DOM throws', () => {
    const { w, call } = boot();
    Object.defineProperty(w.context.app, 'project', { get() { throw new Error('DOM exploded'); } });
    for (const fn of ['activeSequence', 'sequenceIds', 'signature']) {
        const r = call(fn);
        assert.equal(typeof r.ok, 'boolean', fn);
    }
    assert.equal(call('scan', { sequenceId: 'seq-1' }).ok, false);
    assert.equal(call('apply', { sequenceId: 'seq-1', keys: ['1'] }).ok, false);
});
