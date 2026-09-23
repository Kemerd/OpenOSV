// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// cepAdapter.test.js - the CEP route end to end: panel adapter -> evalScript
// bridge -> the real host.jsx -> a mock ExtendScript / QE DOM.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const core = require('../shared/osvcore.js');
const { createCepAdapter, EVENT_TYPE, CALL_TIMEOUT_MS } = require('../cep/cepAdapter.js');
const { createController } = require('../shared/controller.js');
const { createWorld } = require('./mocks/extendscript.js');
const { fakeTimers, settle } = require('./fixtures/timers.js');

const HOST = path.join(__dirname, '..', 'cep', 'host', 'host.jsx');

/**
 * A CEP bridge over the ExtendScript world: evalScript answers on a later
 * turn, like CEP, and CSXS events the host dispatches reach the listeners.
 */
function bridgeFor(w) {
    const listeners = [];
    let delivered = 0;
    const deliver = () => {
        while (delivered < w.world.dispatched.length) {
            const e = w.world.dispatched[delivered++];
            listeners.filter((l) => l.type === e.type).forEach((l) => l.fn({ type: e.type, data: e.data }));
        }
    };
    return {
        calls: [],
        evalScript(script, cb) {
            this.calls.push(script);
            const answer = w.evalScript(script);
            setImmediate(() => { cb(answer); deliver(); });
        },
        addEventListener(type, fn) { listeners.push({ type, fn }); },
        removeEventListener(type, fn) {
            const i = listeners.findIndex((l) => l.type === type && l.fn === fn);
            if (i >= 0) {
                listeners.splice(i, 1);
            }
        },
        deliver: deliver,
        listenerCount: () => listeners.length
    };
}

function boot(options) {
    const w = createWorld(options);
    const seq = w.addSequence('seq-1', 'Main', 2);
    w.load(HOST);
    const bridge = bridgeFor(w);
    const adapter = createCepAdapter(bridge, core);
    return { w, seq, bridge, adapter };
}

function osv(w, seq, track, start) {
    return w.addClip(seq, track, {
        name: 'CAM_' + start + '.OSV', start: start, end: start + 1000,
        path: 'C:\\DCIM\\CAM_' + start + '.OSV', projectItemId: 'pi-' + track + '-' + start
    });
}

test('init binds the host events and a Premiere event reaches the panel', async () => {
    const { w, bridge, adapter } = boot();
    const events = [];
    adapter.init((reason) => events.push(reason));
    await settle();
    assert.equal(bridge.listenerCount(), 1);
    assert.ok(w.world.bound.onActiveSequenceTrackItemAdded, 'bound through host.jsx');
    w.fire('onActiveSequenceTrackItemAdded', {}, {});
    bridge.deliver();
    assert.deepEqual(events, ['onActiveSequenceTrackItemAdded']);
    adapter.dispose();
    assert.equal(bridge.listenerCount(), 0);
});

test('queries pass through and come back as plain values', async () => {
    const { w, seq, adapter } = boot();
    osv(w, seq, 0, 0);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    assert.deepEqual(active, { id: 'seq-1', name: 'Main', projectId: 'doc-1' });
    assert.deepEqual(await adapter.getSequenceIds(), ['seq-1']);
    assert.equal(await adapter.signature(), 'seq-1|2|1|0');
    const r = await adapter.scan(active, { selectedOnly: false });
    assert.equal(r.items.length, 1);
    assert.deepEqual(await adapter.checkEffect(), { available: true });
});

test('apply with the DJI lens adds the effect and writes no parameters', async () => {
    const { w, seq, bridge, adapter } = boot();
    const a = osv(w, seq, 0, 3000);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const items = (await adapter.scan(active, {})).items;
    const r = await adapter.apply(active, items, { lens: 'dji' });
    assert.deepEqual(r, { applied: 1, already: 0, failed: 0, errors: [], notes: [], missingEffect: false });
    assert.equal(w.reframeCount(a), 1);
    assert.equal(bridge.calls.some((c) => c.indexOf('setParams') !== -1), false);
});

for (const base of [0, 1]) {
    test('apply with the Classic lens sets Lens, Preset and the mirror (popups from ' + base + ')', async () => {
        const { w, seq, adapter } = boot({ popupBase: base });
        const a = osv(w, seq, 0, 0);
        adapter.init(() => {});
        const active = await adapter.getActiveSequence();
        const r = await adapter.apply(active, (await adapter.scan(active, {})).items,
                                      { lens: 'classic', dragEnabled: true, dragSensitivity: 0.75 });
        assert.equal(r.applied, 1);
        assert.deepEqual(r.notes, []);
        const fx = w.lastReframe(a);
        assert.equal(w.paramValue(fx, 'Lens'), base + 1);
        assert.equal(w.paramValue(fx, 'Preset'), base);
        assert.equal(w.paramValue(fx, 'Camera Model'), false);
        assert.equal(w.paramValue(fx, 'Drag Sensitivity'), 0.75);
    });
}

test('a missing host script, an ExtendScript error and a timeout each read clearly', async () => {
    // No host.jsx loaded at all.
    const w = createWorld();
    w.addSequence('seq-1', 'Main', 1);
    const adapter = createCepAdapter(bridgeFor(w), core);
    await assert.rejects(adapter.getActiveSequence(), /host script didn't load/);

    // A call that throws inside ExtendScript: CEP answers "EvalScript error."
    const broken = createCepAdapter({ evalScript: (s, cb) => setImmediate(() => cb('EvalScript error.')) }, core);
    await assert.rejects(broken.signature(), /script engine refused/);

    // Garbage.
    const garbage = createCepAdapter({ evalScript: (s, cb) => setImmediate(() => cb('<html>')) }, core);
    await assert.rejects(garbage.signature(), /unreadable/);

    // A host that never answers.
    const timers = fakeTimers();
    const silent = createCepAdapter({ evalScript: () => {} }, core, { timers });
    const pending = silent.signature();
    timers.advance(CALL_TIMEOUT_MS);
    await assert.rejects(pending, /didn't answer in time/);

    // Outside CEP entirely.
    assert.throws(() => createCepAdapter(null, core).init(() => {}), /not running inside Premiere Pro/);
});

test('the panel event name is the one host.jsx dispatches', () => {
    const src = require('node:fs').readFileSync(HOST, 'utf8');
    assert.ok(src.includes("var EVENT_TYPE = '" + EVENT_TYPE + "'"));
});

test('end to end: OSV clips dropped on the timeline get the effect once; the rest are left alone', async () => {
    const { w, seq, bridge } = boot({ popupBase: 0 });
    const existing = osv(w, seq, 0, 0);
    const timers = fakeTimers();
    const states = [];
    const ctl = createController({
        core,
        adapter: createCepAdapter(bridge, core, { timers: { setTimeout: setTimeout, clearTimeout: clearTimeout } }),
        timers,
        storage: { get: () => JSON.stringify({ lens: 'dji' }), set: () => {} },
        onChange: (s) => states.push(s)
    });
    const step = async (ms) => {
        await settle(40);
        timers.advance(ms);
        await settle(40);
        await ctl.idle();
        await settle(40);
    };
    ctl.start();
    await step(0);

    // Two OSV clips and a music clip dropped; Premiere fires one event per item.
    const a = osv(w, seq, 0, 5000);
    const b = osv(w, seq, 1, 0);
    const mp4 = w.addClip(seq, 0, { name: 'song.mp4', start: 9000, end: 9900, path: 'C:/song.mp4' });
    for (let i = 0; i < 3; i += 1) {
        w.fire('onActiveSequenceTrackItemAdded', {}, {});
        bridge.deliver();
        await step(50);
    }
    await step(1500);

    assert.equal(w.reframeCount(existing), 0);
    assert.equal(w.reframeCount(a), 1);
    assert.equal(w.reframeCount(b), 1);
    assert.equal(w.reframeCount(mp4), 0);
    assert.equal(states[states.length - 1].status.text, 'Applied to 2 clips.');

    // More events for the same clips change nothing.
    w.fire('onActiveSequenceStructureChanged');
    bridge.deliver();
    await step(1500);
    assert.equal(w.reframeCount(a), 1);
    ctl.stop();
});
