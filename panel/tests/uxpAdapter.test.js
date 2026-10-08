// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// uxpAdapter.test.js - the UXP route, against a mock of Premiere's documented
// UXP API (mocks/premierepro.js), alone and driven by the real controller.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const core = require('../shared/osvcore.js');
const { createUxpAdapter, UNDO_APPLY, UNDO_PARAMS } = require('../uxp/uxpAdapter.js');
const { createController } = require('../shared/controller.js');
const { createMockPremiere } = require('./mocks/premierepro.js');
const { fakeTimers, settle } = require('./fixtures/timers.js');

const REFRAME = 'AE.OpenOSV.Open360Reframe';

function osvClip(m, seq, track, start, extra) {
    return m.addClip(seq, track, Object.assign({
        name: 'CAM_' + start + '.OSV',
        start: start,
        end: start + 1000,
        inPoint: 0,
        outPoint: 1000,
        projectItem: { id: 'osv-' + track + '-' + start, path: 'C:\\DCIM\\CAM_' + start + '.OSV', isSequence: false }
    }, extra || {}));
}

function world(options) {
    const m = createMockPremiere(options);
    const seq = m.addSequence('seq-1', 'Main', 2);
    const adapter = createUxpAdapter(m.ppro, core);
    return { m, seq, adapter };
}

test('scan lists only OSV / LRF clips, keyed by track, start and project item', async () => {
    const { m, seq, adapter } = world();
    osvClip(m, seq, 0, 0);
    m.addClip(seq, 0, { name: 'b-roll.mp4', start: 1000, end: 2000 });
    m.addClip(seq, 1, { name: 'proxy', start: 0, end: 500, projectItem: { id: 'lrf', path: 'D:/x/CAM.LRF', isSequence: false } });
    m.addClip(seq, 1, { name: 'nested', start: 600, end: 900, projectItem: { id: 'nest', path: 'nested.osv', isSequence: true } });
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    assert.deepEqual(active, { id: 'seq-1', name: 'Main', projectId: 'proj-1' });
    const r = await adapter.scan(active, { selectedOnly: false });
    assert.deepEqual(r.items.map((i) => i.key), ['0:0:osv-0-0', '1:0:lrf']);
    assert.equal(r.items[0].mediaPath, 'C:\\DCIM\\CAM_0.OSV');
    assert.equal(r.items[0].endTicks, '1000');
    assert.equal(r.items[1].outTicks, '100');
});

test('scan of the selection counts the selected clips that are not OSV', async () => {
    const { m, seq, adapter } = world();
    osvClip(m, seq, 0, 0, { selected: true });
    osvClip(m, seq, 0, 5000, { selected: false });
    m.addClip(seq, 1, { name: 'b-roll', start: 0, end: 10, selected: true });
    adapter.init(() => {});
    const r = await adapter.scan(await adapter.getActiveSequence(), { selectedOnly: true });
    assert.deepEqual(r.items.map((i) => i.startTicks), ['0']);
    assert.equal(r.otherCount, 1);
});

test('apply appends the effect in one named undo step, created inside lockedAccess', async () => {
    const { m, seq, adapter } = world();
    const a = osvClip(m, seq, 0, 0);
    const b = osvClip(m, seq, 1, 0);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const scanned = await adapter.scan(active, {});
    const r = await adapter.apply(active, scanned.items, { lens: 'dji' });
    assert.deepEqual(r, { applied: 2, already: 0, failed: 0, errors: [], notes: [], missingEffect: false });
    assert.equal(m.reframeCount(a), 1);
    assert.equal(m.reframeCount(b), 1);
    assert.deepEqual(m.world.undo, [UNDO_APPLY], 'one transaction for the whole batch, no parameter step for DJI');
    // Appended after the fixed Opacity / Motion components.
    assert.equal(a.components[a.components.length - 1].matchName, REFRAME);
});

test('apply never doubles the effect on a clip that has it', async () => {
    const { m, seq, adapter } = world();
    const a = osvClip(m, seq, 0, 0);
    a.components.push({ matchName: REFRAME, params: m.reframeParams(0) });
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const r = await adapter.apply(active, (await adapter.scan(active, {})).items, {});
    assert.equal(r.applied, 0);
    assert.equal(r.already, 1);
    assert.equal(m.reframeCount(a), 1);
    assert.deepEqual(m.world.undo, []);
});

for (const base of [0, 1]) {
    test('Classic is written in the host\'s popup numbering (base ' + base + ')', async () => {
        const { m, seq, adapter } = world({ popupBase: base });
        const a = osvClip(m, seq, 0, 0);
        adapter.init(() => {});
        const active = await adapter.getActiveSequence();
        const r = await adapter.apply(active, (await adapter.scan(active, {})).items,
                                      { lens: 'classic', dragEnabled: true, dragSensitivity: 3.25 });
        assert.equal(r.applied, 1);
        assert.deepEqual(r.notes, []);
        const fx = m.lastReframe(a);
        assert.equal(m.paramValue(fx, 'Lens'), base + 1, 'Classic');
        assert.equal(m.paramValue(fx, 'Preset'), base, 'Custom');
        assert.equal(m.paramValue(fx, 'Camera Model'), false, 'the hidden mirror follows');
        assert.equal(m.paramValue(fx, 'Drag Sensitivity'), 3.25);
        assert.deepEqual(m.world.undo, [UNDO_APPLY, UNDO_PARAMS]);
    });
}

test('a Premiere without the effect installed is reported, and nothing is touched', async () => {
    const { m, seq, adapter } = world({ installed: false });
    const a = osvClip(m, seq, 0, 0);
    adapter.init(() => {});
    assert.deepEqual(await adapter.checkEffect(), { available: false });
    const active = await adapter.getActiveSequence();
    const r = await adapter.apply(active, (await adapter.scan(active, {})).items, {});
    assert.equal(r.missingEffect, true);
    assert.equal(m.reframeCount(a), 0);
});

test('a refused transaction is a failure, not a success', async () => {
    const { m, seq, adapter } = world();
    const a = osvClip(m, seq, 0, 0);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const items = (await adapter.scan(active, {})).items;
    m.world.failTransaction = true;
    const r = await adapter.apply(active, items, {});
    assert.equal(r.applied, 0);
    assert.equal(r.failed, 1);
    assert.equal(m.reframeCount(a), 0);
});

test('a clip that gained the effect between the scan and the lock is not doubled', async () => {
    const { m, seq, adapter } = world();
    const a = osvClip(m, seq, 0, 0);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const items = (await adapter.scan(active, {})).items;
    // Another copy of the panel (or the user) adds it while this one prepares:
    // the chain grows between the look and the lock.
    const original = m.ppro.VideoFilterFactory.createComponent;
    m.ppro.VideoFilterFactory.createComponent = async (name) => {
        a.components.push({ matchName: REFRAME, params: m.reframeParams(0) });
        return original(name);
    };
    const r = await adapter.apply(active, items, {});
    assert.equal(m.reframeCount(a), 1);
    assert.equal(r.applied, 0);
    assert.equal(r.already, 1);
});

test('the active sequence changing between scan and apply is refused', async () => {
    const { m, seq, adapter } = world();
    osvClip(m, seq, 0, 0);
    m.addSequence('seq-2', 'Other', 1);
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const items = (await adapter.scan(active, {})).items;
    m.world.project.active = 1;
    await assert.rejects(adapter.apply(active, items, {}), /active sequence changed/);
});

test('track listeners follow the sequence, including a track created by a drop', async () => {
    const { m, seq, adapter } = world();
    let events = 0;
    adapter.init(() => { events += 1; });
    assert.equal(m.globalCount(), 5, 'sequence activated / selection changed, project activated / opened / dirty');
    await adapter.getActiveSequence();
    assert.equal(m.listenerCount(), 2);
    assert.equal(m.fireTrackChanged(seq, 1), 1);
    assert.equal(events, 1);
    // A clip dropped above the top track makes a third track.
    osvClip(m, seq, 2, 0);
    await adapter.signature();
    assert.equal(m.listenerCount(), 3, 'the new track is listened to');
    adapter.dispose();
    assert.equal(m.listenerCount(), 0);
    assert.equal(m.globalCount(), 0);
});

test('signature changes when a clip lands, and only then', async () => {
    const { m, seq, adapter } = world();
    osvClip(m, seq, 0, 0);
    adapter.init(() => {});
    const s1 = await adapter.signature();
    assert.equal(await adapter.signature(), s1);
    osvClip(m, seq, 1, 0);
    assert.notEqual(await adapter.signature(), s1);
});

test('init without a UXP project API throws a readable reason', () => {
    const adapter = createUxpAdapter({ Constants: {} }, core);
    assert.throws(() => adapter.init(() => {}), /no UXP project API/);
    assert.throws(() => createUxpAdapter(null, core), /premierepro module/);
});

// ---------------------------------------------------------------------------
//  End to end: the controller + this adapter + the mock Premiere
// ---------------------------------------------------------------------------

test('end to end: drop OSV clips on the timeline and Open 360 Reframe lands on each, once', async () => {
    const m = createMockPremiere({ popupBase: 0 });
    const seq = m.addSequence('seq-1', 'Main', 1);
    const existing = osvClip(m, seq, 0, 0);          // there before the panel: left alone
    const timers = fakeTimers();
    const store = new Map([['openosv.panel.settings.v1', JSON.stringify({ lens: 'classic' })]]);
    const states = [];
    const ctl = createController({
        core,
        adapter: createUxpAdapter(m.ppro, core),
        timers,
        storage: { get: (k) => store.get(k) || null, set: (k, v) => store.set(k, v) },
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

    // A two-clip drop, one of them on a new track, with an event per clip.
    const d1 = osvClip(m, seq, 0, 5000);
    const d2 = osvClip(m, seq, 1, 0);
    const other = m.addClip(seq, 0, { name: 'music-video.mp4', start: 9000, end: 9500 });
    m.fireTrackChanged(seq);
    await step(100);
    m.fireTrackChanged(seq);
    await step(1000);
    await step(3000);                                // the poll picks up the new track too

    assert.equal(m.reframeCount(existing), 0, 'the pre-existing clip was not retro-fitted');
    assert.equal(m.reframeCount(d1), 1);
    assert.equal(m.reframeCount(d2), 1);
    assert.equal(m.reframeCount(other), 0, 'not OSV');
    assert.equal(m.paramValue(m.lastReframe(d1), 'Lens'), 1, 'Classic, as the panel was set');
    assert.match(states[states.length - 1].status.text, /^Applied to (1 clip|2 clips)\.$/);

    // The user removes the effect from d1 on purpose, then moves it: it stays off.
    d1.components = d1.components.filter((c) => c.matchName !== REFRAME);
    d1.start = 7000;
    d1.end = 8000;
    m.fireTrackChanged(seq, 0);
    await step(1000);
    assert.equal(m.reframeCount(d1), 0);

    // "Apply to all OSV clips" is the explicit way to retro-fit.
    ctl.applyAll();
    await step(0);
    assert.equal(m.reframeCount(existing), 1);
    assert.equal(m.reframeCount(d1), 1);
    assert.equal(m.reframeCount(d2), 1, 'still exactly one');
    ctl.stop();
});

// ---------------------------------------------------------------------------
//  The camera's .LRF as the proxy (ClipProjectItem.attachProxy, since 25.6)
// ---------------------------------------------------------------------------

/** A fileExists() over a fixed list, as main.js builds one on UXP's fs. */
function filesThatExist(list) {
    return async (p) => list.indexOf(p) !== -1;
}

/** A listFolder() over the same list: the names directly inside a folder. */
function namesIn(list) {
    return async (folder) => {
        const prefix = folder.replace(/[\\/]+$/, '') + '\\';
        return list.filter((f) => f.indexOf(prefix) === 0 && f.substring(prefix.length).indexOf('\\') === -1)
            .map((f) => f.substring(prefix.length));
    };
}

test('attachProxies attaches the .LRF beside each .OSV once, as the proxy, and keeps one that is there', async () => {
    const m = createMockPremiere();
    const seq = m.addSequence('seq-1', 'Main', 2);
    const files = ['C:\\DCIM\\CAM_X_D-001.OSV', 'C:\\DCIM\\CAM_X_D.LRF', 'C:\\DCIM\\CAM_Y.OSV', 'C:\\DCIM\\CAM_Y.LRF',
                   'C:\\DCIM\\CAM_Z.OSV'];
    const adapter = createUxpAdapter(m.ppro, core, { fileExists: filesThatExist(files), listFolder: namesIn(files) });
    const shared = { id: 'pi-x', path: 'C:\\DCIM\\CAM_X_D-001.OSV', isSequence: false };
    m.addClip(seq, 0, { name: 'x', start: 0, end: 1000, projectItem: shared });
    m.addClip(seq, 1, { name: 'x again', start: 0, end: 1000, projectItem: shared });
    const mine = { id: 'pi-y', path: 'C:\\DCIM\\CAM_Y.OSV', isSequence: false, proxyPath: 'D:\\mine.mov' };
    m.addClip(seq, 0, { name: 'y', start: 2000, end: 3000, projectItem: mine });
    m.addClip(seq, 0, { name: 'z', start: 4000, end: 5000,
                        projectItem: { id: 'pi-z', path: 'C:\\DCIM\\CAM_Z.OSV', isSequence: false } });
    adapter.init(() => {});
    const active = await adapter.getActiveSequence();
    const scanned = await adapter.scan(active, { selectedOnly: false });
    const r = await adapter.attachProxies(active, scanned.items);
    assert.deepEqual(r, { attached: 1, already: 1, missing: 1, failed: 0, unsupported: 0, ambiguous: 0,
                          noFileAccess: false, errors: [] });
    // As the proxy (isHiRes false), not as a Team Projects alternate link, once.
    assert.deepEqual(m.world.proxyCalls, [{ item: 'pi-x', path: 'C:\\DCIM\\CAM_X_D.LRF', isHiRes: false,
                                            alternateLink: false }]);
    assert.equal(shared.proxyPath, 'C:\\DCIM\\CAM_X_D.LRF');
    assert.equal(mine.proxyPath, 'D:\\mine.mov', 'a proxy already there is kept');
});

test('attachProxies: a refusal fails, no file access or no proxy API is said, never guessed', async () => {
    const make = (options, adapterOptions) => {
        const m = createMockPremiere(options);
        const seq = m.addSequence('seq-1', 'Main', 1);
        m.addClip(seq, 0, { name: 'x', start: 0, end: 1000,
                            projectItem: { id: 'pi-x', path: 'C:\\DCIM\\CAM_X.OSV', isSequence: false } });
        const adapter = createUxpAdapter(m.ppro, core, adapterOptions);
        adapter.init(() => {});
        return { m, adapter };
    };
    const run = async (t) => {
        const active = await t.adapter.getActiveSequence();
        const scanned = await t.adapter.scan(active, { selectedOnly: false });
        return t.adapter.attachProxies(active, scanned.items);
    };
    // Premiere answers false.
    const refused = make({}, { fileExists: filesThatExist(['C:\\DCIM\\CAM_X.LRF']) });
    refused.m.world.proxyRefuses = true;
    const r1 = await run(refused);
    assert.equal(r1.failed, 1);
    assert.match(r1.errors[0], /did not take C:\\DCIM\\CAM_X\.LRF as the proxy/);
    // No file system: nothing is attached on a guess.
    const blind = make({}, {});
    const r2 = await run(blind);
    assert.equal(r2.noFileAccess, true);
    assert.equal(r2.attached + r2.missing + r2.failed, 0);
    assert.equal(blind.m.world.proxyCalls.length, 0);
    // The file system cannot tell (no permission): the same, not "missing".
    const unsure = make({}, { fileExists: async () => null });
    const r3 = await run(unsure);
    assert.equal(r3.noFileAccess, true);
    assert.equal(r3.missing, 0);
    // A host without the proxy calls.
    const old = make({}, { fileExists: filesThatExist(['C:\\DCIM\\CAM_X.LRF']) });
    old.m.world.noProxyApi = true;
    const r4 = await run(old);
    assert.equal(r4.unsupported, 1);
});

test('auto-apply attaches the .LRF of a dropped .OSV once; the button reports every clip', async () => {
    const m = createMockPremiere();
    const seq = m.addSequence('seq-1', 'Main', 2);
    const files = ['C:\\DCIM\\CAM_D-001.OSV', 'C:\\DCIM\\CAM_D.LRF'];
    const adapter = createUxpAdapter(m.ppro, core, { fileExists: filesThatExist(files), listFolder: namesIn(files) });
    const timers = fakeTimers();
    const ctl = createController({ core, adapter, timers, storage: null });
    const step = async (ms) => {
        await settle(40);
        timers.advance(ms);
        await settle(40);
        await ctl.idle();
        await settle(40);
    };
    ctl.start();
    await step(0);
    // The drop.
    m.addClip(seq, 0, { name: 'drop', start: 0, end: 1000,
                        projectItem: { id: 'pi-d', path: 'C:\\DCIM\\CAM_D-001.OSV', isSequence: false } });
    m.fireTrackChanged(seq, 0);
    await step(1000);
    assert.equal(m.reframeCount(seq.tracks[0][0]), 1, 'the effect went on');
    assert.deepEqual(m.world.proxyCalls.map((c) => c.path), ['C:\\DCIM\\CAM_D.LRF']);
    assert.match(ctl.getState().status.text, /Attached the \.LRF proxy to 1 clip\./);
    // The same master clip dropped again: not asked again.
    m.addClip(seq, 1, { name: 'again', start: 0, end: 1000, projectItem: seq.tracks[0][0].projectItem });
    m.fireTrackChanged(seq, 1);
    await step(1000);
    assert.equal(m.world.proxyCalls.length, 1);
    // The button looks at every clip: it has its proxy now.
    await ctl.attachProxies();
    assert.equal(ctl.getState().status.text, 'The OSV clip already has its proxy.');
    ctl.stop();
});

test('attachProxies: an .LRF reached through a copy suffix needs the one copy, and a folder it can read', async () => {
    const run = async (files, listing) => {
        const m = createMockPremiere();
        const seq = m.addSequence('seq-1', 'Main', 1);
        m.addClip(seq, 0, { name: 'x', start: 0, end: 1000,
                            projectItem: { id: 'pi-x', path: 'C:\\DCIM\\CAM_X_D-001.OSV', isSequence: false } });
        const adapter = createUxpAdapter(m.ppro, core, { fileExists: filesThatExist(files), listFolder: listing });
        adapter.init(() => {});
        const active = await adapter.getActiveSequence();
        const scanned = await adapter.scan(active, { selectedOnly: false });
        const r = await adapter.attachProxies(active, scanned.items);
        return { r, calls: m.world.proxyCalls.length };
    };
    const two = ['C:\\DCIM\\CAM_X_D-001.OSV', 'C:\\DCIM\\CAM_X_D-002.OSV', 'C:\\DCIM\\CAM_X_D.LRF'];
    const ambiguous = await run(two, namesIn(two));
    assert.equal(ambiguous.r.ambiguous, 1);
    assert.equal(ambiguous.calls, 0);
    const unread = await run(two.filter((f) => !f.endsWith('-002.OSV')), async () => null);
    assert.equal(unread.r.ambiguous, 1, 'a folder that cannot be read proves nothing');
    assert.equal(unread.calls, 0);
    const one = two.filter((f) => !f.endsWith('-002.OSV'));
    const single = await run(one, namesIn(one));
    assert.equal(single.r.attached, 1);
});
