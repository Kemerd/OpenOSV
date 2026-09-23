// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// controller.test.js - the auto-apply state machine against a scripted host.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const core = require('../shared/osvcore.js');
const { createController, STORAGE_KEY, POLL_MS } = require('../shared/controller.js');
const { fakeTimers, settle } = require('./fixtures/timers.js');

/** An OSV item as an adapter reports it. */
function osv(key, track, start, pi) {
    return {
        key: key, trackIndex: track || 0, startTicks: String(start || 0), endTicks: String((start || 0) + 100),
        inTicks: '0', outTicks: '100', projectItemId: pi || ('pi-' + key), name: key, mediaPath: 'C:/m/' + key + '.OSV'
    };
}

/** A host whose timeline the test scripts. */
function fakeAdapter() {
    const s = {
        seq: { id: 's1', name: 'Seq 1', projectId: 'p1' },
        seqIds: ['s1'],
        items: { s1: [] },
        selected: [],
        otherSelected: 0,
        effectAvailable: true,
        signature: 'sig-0',
        applied: [],
        onEvent: null,
        initError: null,
        scanError: null,
        seqIdsError: null,
        applyResult: null,
        disposed: false
    };
    return {
        s: s,
        init(fn) {
            if (s.initError) {
                throw new Error(s.initError);
            }
            s.onEvent = fn;
        },
        dispose() { s.disposed = true; },
        getActiveSequence: async () => s.seq,
        getSequenceIds: async () => {
            if (s.seqIdsError) {
                throw new Error(s.seqIdsError);
            }
            return s.seqIds.slice();
        },
        signature: async () => s.signature,
        scan: async (seq, o) => {
            if (s.scanError) {
                throw new Error(s.scanError);
            }
            if (o && o.selectedOnly) {
                return { items: s.selected.slice(), otherCount: s.otherSelected };
            }
            return { items: (s.items[seq.id] || []).slice(), otherCount: 0 };
        },
        apply: async (seq, items, settings) => {
            s.applied.push({ seq: seq.id, keys: items.map((i) => i.key), settings: Object.assign({}, settings) });
            return s.applyResult ? s.applyResult(items) : { applied: items.length, already: 0, failed: 0, errors: [], notes: [] };
        },
        checkEffect: async () => ({ available: s.effectAvailable })
    };
}

function memoryStorage(initial) {
    const map = new Map(initial ? Object.entries(initial) : []);
    return { get: (k) => (map.has(k) ? map.get(k) : null), set: (k, v) => { map.set(k, v); }, map: map };
}

/** A controller wired to fakes; `step(ms)` advances time and lets promises run. */
function setup(options) {
    const o = options || {};
    const adapter = fakeAdapter();
    if (o.configure) {
        o.configure(adapter.s);
    }
    const timers = fakeTimers();
    const storage = memoryStorage(o.storage);
    const states = [];
    const ctl = createController({ core, adapter, timers, storage, onChange: (st) => states.push(st) });
    async function step(ms) {
        await settle();
        timers.advance(ms || 0);
        await settle();
        await ctl.idle();
        await settle();
    }
    return { adapter, s: adapter.s, timers, storage, states, ctl, step, last: () => states[states.length - 1] };
}

test('start takes a baseline: clips already on the timeline are never touched', async () => {
    const t = setup({ configure: (s) => { s.items.s1 = [osv('a'), osv('b')]; } });
    t.ctl.start();
    await t.step(0);
    t.s.onEvent('track');
    await t.step(3000);
    assert.equal(t.s.applied.length, 0);
    assert.equal(t.last().status.text, 'Watching the timeline.');
    assert.equal(t.last().host, 'ready');
});

test('a clip dropped after start gets the effect, and only that clip', async () => {
    const t = setup({ configure: (s) => { s.items.s1 = [osv('a')]; } });
    t.ctl.start();
    await t.step(0);
    t.s.items.s1.push(osv('new', 0, 500));
    t.s.onEvent('track');
    await t.step(1000);
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['new']]);
    assert.equal(t.last().status.text, 'Applied to 1 clip.');
    assert.equal(t.last().status.tone, 'ok');
    assert.equal(t.last().status.stamped, true);
});

test('an event storm from a multi-clip drop becomes one pass', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.s.items.s1.push(osv('x', 0, 0), osv('y', 0, 100), osv('z', 1, 0));
    for (let i = 0; i < 12; i += 1) {
        t.s.onEvent('track');   // one event per clip, audio included
        await t.step(20);
    }
    await t.step(1000);
    assert.equal(t.s.applied.length, 1);
    assert.deepEqual(t.s.applied[0].keys.sort(), ['x', 'y', 'z']);
});

test('a clip is applied once: later events do not apply it again', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.s.items.s1.push(osv('once'));
    t.s.onEvent('track');
    await t.step(1000);
    t.s.onEvent('track');
    await t.step(1000);
    assert.equal(t.s.applied.length, 1);
});

test('a sequence created while watching has its first clips applied', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    // "New Sequence From Clip": a new, active sequence that already holds the clip.
    t.s.seq = { id: 's2', name: 'Seq 2', projectId: 'p1' };
    t.s.seqIds.push('s2');
    t.s.items.s2 = [osv('fresh')];
    t.s.onEvent('sequence');
    await t.step(1000);
    assert.deepEqual(t.s.applied.map((a) => [a.seq, a.keys]), [['s2', ['fresh']]]);
});

test('switching to a sequence that already existed only takes its baseline', async () => {
    const t = setup({ configure: (s) => { s.seqIds = ['s1', 'old']; s.items.old = [osv('legacy')]; } });
    t.ctl.start();
    await t.step(0);
    t.s.seq = { id: 'old', name: 'Old', projectId: 'p1' };
    t.s.onEvent('sequence');
    await t.step(1000);
    assert.equal(t.s.applied.length, 0);
    // ...and a drop into it afterwards is applied.
    t.s.items.old.push(osv('dropped', 0, 900));
    t.s.onEvent('track');
    await t.step(1000);
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['dropped']]);
});

test('with auto-apply off nothing is applied, and switching it on never retro-fits', async () => {
    const t = setup({ storage: { [STORAGE_KEY]: JSON.stringify({ autoApply: false }) } });
    t.ctl.start();
    await t.step(0);
    assert.equal(t.last().settings.autoApply, false);
    assert.equal(t.last().status.text, 'Auto-apply is off.');
    t.s.items.s1.push(osv('while-off'));
    t.s.onEvent('track');
    await t.step(3000);
    assert.equal(t.s.applied.length, 0);
    t.ctl.setAutoApply(true);
    await t.step(0);
    t.s.onEvent('track');
    await t.step(1000);
    assert.equal(t.s.applied.length, 0, 'the clip added while off is part of the new baseline');
    t.s.items.s1.push(osv('after-on', 0, 700));
    t.s.onEvent('track');
    await t.step(1000);
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['after-on']]);
});

test('the signature poll catches a drop that no event reported', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    await t.step(POLL_MS);            // first poll records the signature
    t.s.items.s1.push(osv('silent'));
    t.s.signature = 'sig-1';
    await t.step(POLL_MS);            // second poll sees the change
    await t.step(1000);               // debounce
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['silent']]);
});

test('Apply to selected clips applies the selection with the chosen lens', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.ctl.setLens('classic');
    t.s.selected = [osv('sel1'), osv('sel2')];
    t.s.otherSelected = 1;
    t.ctl.applySelected();
    assert.equal(t.last().busy, true);
    await t.step(0);
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['sel1', 'sel2']]);
    assert.equal(t.s.applied[0].settings.lens, 'classic');
    assert.equal(t.last().busy, false);
    assert.equal(t.last().status.text, 'Applied to 2 clips.');
});

test('Apply to selected clips with nothing OSV selected says so', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.s.selected = [];
    t.s.otherSelected = 3;
    t.ctl.applySelected();
    await t.step(0);
    assert.equal(t.s.applied.length, 0);
    assert.equal(t.last().status.text, 'No OSV clips selected. 3 selected clips aren\'t OSV.');
});

test('Apply to all OSV clips applies every OSV clip, and reports the ones that had it', async () => {
    const t = setup({ configure: (s) => { s.items.s1 = [osv('a'), osv('b'), osv('c')]; } });
    t.s.applyResult = (items) => ({ applied: 1, already: items.length - 1, failed: 0, errors: [], notes: [] });
    t.ctl.start();
    await t.step(0);
    t.ctl.applyAll();
    await t.step(0);
    assert.deepEqual(t.s.applied.map((a) => a.keys), [['a', 'b', 'c']]);
    assert.equal(t.last().status.text, 'Applied to 1 clip. 2 clips already had it.');
});

test('no active sequence: the buttons say to open one', async () => {
    const t = setup({ configure: (s) => { s.seq = null; } });
    t.ctl.start();
    await t.step(0);
    t.ctl.applyAll();
    await t.step(0);
    assert.equal(t.last().status.text, 'Open a sequence first.');
});

test('a host that cannot be reached says why in the status line', async () => {
    const t = setup({ configure: (s) => { s.initError = 'this Premiere has no UXP API'; } });
    t.ctl.start();
    await t.step(0);
    assert.equal(t.last().host, 'error');
    assert.equal(t.last().status.tone, 'error');
    assert.match(t.last().status.text, /Can't reach Premiere: this Premiere has no UXP API/);
});

test('a failing scan becomes a status line, never an exception', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.s.scanError = 'the active sequence changed. Try again';
    t.ctl.applyAll();
    await t.step(0);
    assert.equal(t.last().status.tone, 'error');
    assert.match(t.last().status.text, /the active sequence changed/);
    assert.equal(t.last().busy, false, 'busy is cleared after a failure');
});

test('a missing effect is reported at start', async () => {
    const t = setup({ configure: (s) => { s.effectAvailable = false; } });
    t.ctl.start();
    await t.step(0);
    assert.equal(t.last().status.tone, 'error');
    assert.match(t.last().status.text, /Open 360 Reframe isn't in this Premiere/);
});

test('settings persist after a short delay and load on the next start', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.ctl.setLens('classic');
    t.ctl.setDragEnabled(true);
    t.ctl.setDragSensitivity(3.3);
    assert.equal(t.storage.map.has(STORAGE_KEY), false, 'not written on every keystroke');
    await t.step(300);
    assert.deepEqual(JSON.parse(t.storage.map.get(STORAGE_KEY)),
                     { autoApply: true, lens: 'classic', dragEnabled: true, dragSensitivity: 3.3 });

    const t2 = setup({ storage: { [STORAGE_KEY]: t.storage.map.get(STORAGE_KEY) } });
    t2.ctl.start();
    await t2.step(0);
    assert.equal(t2.last().settings.lens, 'classic');
    assert.equal(t2.last().settings.dragSensitivity, 3.3);
});

test('another project: its sequences are a fresh baseline', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.s.seq = { id: 'q1', name: 'Other', projectId: 'p2' };
    t.s.seqIds = ['q1'];
    t.s.items.q1 = [osv('existing-in-p2')];
    t.s.onEvent('project');
    await t.step(1000);
    assert.equal(t.s.applied.length, 0);
});

test('when the host cannot list sequences, an unseen sequence is treated as existing', async () => {
    const t = setup({ configure: (s) => { s.seqIdsError = 'no list'; } });
    t.ctl.start();
    await t.step(0);
    t.s.seq = { id: 'mystery', name: 'Mystery', projectId: 'p1' };
    t.s.items.mystery = [osv('m1'), osv('m2')];
    t.s.onEvent('sequence');
    await t.step(1000);
    assert.equal(t.s.applied.length, 0, 'never guess "new" - that would retro-fit an edit');
});

test('stop() detaches from the host and leaves no timer running', async () => {
    const t = setup();
    t.ctl.start();
    await t.step(0);
    t.ctl.setLens('classic');
    t.ctl.stop();
    assert.equal(t.s.disposed, true);
    assert.equal(t.timers.pendingCount(), 0);
    t.s.items.s1.push(osv('late'));
    t.s.onEvent('track');
    await t.step(3000);
    assert.equal(t.s.applied.length, 0);
});
