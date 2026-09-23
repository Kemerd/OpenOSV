// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// easing.test.js - the Keyframe Animation, Manual Framing and Stabilisation
// cards: OsvCore's rules, both host routes (the UXP adapter against the UXP
// mock; the CEP adapter through the real host.jsx against the ExtendScript
// mock) and the controller's flows.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const core = require('../shared/osvcore.js');
const { createUxpAdapter, UNDO_EASING, UNDO_FRAMING, UNDO_STABILIZATION } = require('../uxp/uxpAdapter.js');
const { createCepAdapter } = require('../cep/cepAdapter.js');
const { createController, STORAGE_KEY, FRAMING_POLL_MS } = require('../shared/controller.js');
const { createMockPremiere } = require('./mocks/premierepro.js');
const { createWorld } = require('./mocks/extendscript.js');
const { fakeTimers, settle } = require('./fixtures/timers.js');

const HOST = path.join(__dirname, '..', 'cep', 'host', 'host.jsx');

// ===========================================================================
//  OsvCore: the curves, the numbering, the controls, DJI's camera
// ===========================================================================

test('the preset speeds are the effect\'s polynomials', () => {
    // Exactly the numbers plugins/reframe/ReframeEasing.cpp is tested for.
    assert.equal(core.easeSpeed('slow-in-slow-out', 0), 0);
    assert.equal(core.easeSpeed('slow-in-slow-out', 0.5), 1.5);
    assert.equal(core.easeSpeed('fast-in-fast-out', 0), 2);
    assert.equal(core.easeSpeed('fast-in-fast-out', 0.5), 0.5);
    assert.equal(core.easeSpeed('fast-in-slow-out', 0), 2);
    assert.equal(core.easeSpeed('fast-in-slow-out', 1), 0);
    assert.equal(core.easeSpeed('slow-in-fast-out', 0), 0);
    assert.equal(core.easeSpeed('slow-in-fast-out', 1), 2);
    assert.equal(core.easeSpeed('linear', 0.3), 1);
    // Garbage u is clamped; NaN is the start.
    assert.equal(core.easeSpeed('slow-in-slow-out', NaN), 0);
    assert.equal(core.easeSpeed('fast-in-slow-out', 9), 0);
});

test('a tile icon is the speed profile between two keyframe dots, None a crossed circle', () => {
    assert.deepEqual(core.easingIconPoints('none', 30, 18), { kind: 'none' });
    const w = 30;
    const h = 18;
    for (const e of core.EASINGS.filter((x) => x.id !== 'none')) {
        const d = core.easingIconPoints(e.id, w, h);
        assert.equal(d.kind, 'curve', e.id);
        assert.equal(d.points.length, 25, e.id);
        assert.deepEqual(d.dots, [d.points[0], d.points[24]], e.id);
        for (const [x, y] of d.points) {
            assert.ok(x >= 0 && x <= w && y >= 0 && y <= h, e.id + ' stays in the box');
        }
    }
    // Speed 2 at the top margin, 0 at the bottom one: Fast In, Slow Out
    // starts high and ends low; Linear is flat.
    const fiso = core.easingIconPoints('fast-in-slow-out', w, h);
    assert.ok(fiso.points[0][1] < fiso.points[24][1]);
    const linear = core.easingIconPoints('linear', w, h);
    assert.ok(linear.points.every((p) => p[1] === linear.points[0][1]));
});

test('the popup numbering is learned only from readings that settle it', () => {
    assert.equal(core.learnPopupBase([{ value: 0, count: 2 }], null), 0);
    assert.equal(core.learnPopupBase([{ value: 7, count: 7 }], null), 1);
    assert.equal(core.learnPopupBase([{ value: 1, count: 2 }, { value: 3, count: 6 }], null), null, 'ambiguous');
    assert.equal(core.learnPopupBase([{ value: 1, count: 2 }], 0), 0, 'what was learned stays');
    assert.equal(core.learnPopupBase([{ value: 0, count: 5 }, { value: 7, count: 7 }], 1), 1, 'contradiction: keep');
    assert.equal(core.learnPopupBase([{ value: 'x', count: 2 }, { value: 1.5, count: 2 }, null], null), null);
    assert.equal(core.popupValue(3, 0), 2);
    assert.equal(core.popupValue(3, 1), 3);
    assert.equal(core.popupEntry(2, 0), 3);
    assert.ok(Number.isNaN(core.popupEntry(2, null)));
});

test('the two FOVs are told apart by their neighbours, whichever name the host shows', () => {
    // As a host that keeps the effect's display rename lists them (DJI FOV
    // shown as "FOV"), with the Camera topic and no Source group.
    const renamed = ['Output Resolution', 'Camera', 'Preset', 'Pan', 'Tilt', 'Roll', 'FOV', 'Distortion',
                     'Smooth Keyframes', 'Camera Model', 'Zoom', 'FOV', 'Correction Angle', 'Drag Sensitivity', 'Lens',
                     'Keyframe Easing'].map((name, index) => ({ index, name }));
    const a = core.locateReframeParams(renamed);
    assert.equal(a.fov, 6);
    assert.equal(a.djiFov, 11);
    assert.equal(a.distortion, 7);
    assert.equal(a.zoom, 10);
    assert.equal(a.correction, 12);
    assert.equal(a.lens, 14);
    assert.equal(a.keyframeEasing, 15);
    // As registered ("DJI FOV"), with group markers and the Source controls.
    const registered = ['Output Resolution', 'Camera', 'Preset', 'Pan', 'Tilt', 'Roll', 'FOV', 'Distortion', '',
                        'Source', 'Source Pan', 'Source Tilt', 'Source Roll', '', 'Smooth Keyframes', 'Camera Model',
                        'Zoom', 'DJI FOV', 'Correction Angle', 'Drag Sensitivity', 'Lens', 'Keyframe Easing']
        .map((name, index) => ({ index: index + 1, name }));
    const b = core.locateReframeParams(registered);
    assert.equal(b.pan, 4);
    assert.equal(b.fov, 7);
    assert.equal(b.djiFov, 18);
    assert.equal(b.correction, 19);
    // A list without the DJI block has no DJI FOV to find.
    const old = registered.slice(0, 15);
    assert.equal(core.locateReframeParams(old).djiFov, undefined);
    assert.deepEqual(core.locateReframeParams(null), {});
});

test('DJI Studio\'s Zoom read-out and zoom path', () => {
    // DJI Studio's four Manual Framing read-outs on a 16:9 canvas.
    for (const [zoom, fov, corr] of [[45.5, 25.4, 0.04], [101.4, 52.5, 0.25], [221.5, 112.1, 0.71], [256.6, 128.3, 0.83]]) {
        assert.ok(Math.abs(core.djiZoomDeg(fov, corr, 16 / 9) - zoom) < 1.2, zoom + ' deg');
    }
    assert.equal(core.djiZoomDeg(NaN, 0.6, 16 / 9), 0);
    assert.equal(core.djiZoomDeg(60, -1, 16 / 9), 0);
    // One press moves FOV 130x as far as Correction.
    const a = core.djiZoomStep(60, 0.6, 1);
    assert.equal(a.fov, 66.5);
    assert.equal(a.correction, 0.65);
    const b = core.djiZoomStep(60, 0.6, -1);
    assert.equal(b.fov, 53.5);
    assert.equal(b.correction, 0.55);
    // DJI Studio's limits...
    assert.deepEqual(core.djiZoomStep(20, 0, -1), { fov: 20, correction: 0 });
    assert.deepEqual(core.djiZoomStep(148, 0.99, 1), { fov: 150, correction: 1 });
    // ...widened to where the lens started, so a Crystal Ball is not snapped.
    assert.deepEqual(core.djiZoomStep(75, 1.8, -1), { fov: 68.5, correction: 1.75 });
    assert.equal(core.classicZoomStep(120, 1), 126.5);
    assert.equal(core.classicZoomStep(12, -1), 10);
});

test('a framing preset writes every value the effect\'s own Preset writes, per frame shape', () => {
    const wideLandscape = core.framingPresetValues('wide', 16 / 9);
    assert.deepEqual(wideLandscape, {
        presetEntry: 4, classicFov: 120, distortion: 15, tilt: 0, djiFov: 60, correction: 0.6, zoom: 142.4
    });
    assert.equal(core.framingPresetValues('wide', 9 / 16).djiFov, 90);
    assert.equal(core.framingPresetValues('wide', 3 / 4).djiFov, 72);
    assert.equal(core.framingPresetValues('asteroid', 16 / 9).tilt, -90);
    assert.equal(core.framingPresetValues('nope', 1), null);
    // The frame shape: a named resolution, else the sequence, else 16:9.
    assert.equal(core.framingAspect(4, 1080, 1920), 1920 / 1080);
    assert.equal(core.framingAspect(1, 1080, 1920), 1080 / 1920);
    assert.equal(core.framingAspect(NaN, 0, 0), 16 / 9);
});

test('planFramingWrites: a preset needs the numbering, a zoom press only skips Preset without it', () => {
    const state = core.framingState({ lens: 0, cameraModel: true, outputResolution: 0, djiFov: 60, correction: 0.6,
                                      fov: 120, distortion: 15, pan: 5, tilt: 0, roll: 0 }, 0, 1920, 1080);
    assert.equal(state.lens, 'dji');
    assert.ok(Math.abs(state.zoom - 142.397) < 0.01);
    const preset = core.planFramingWrites({ action: 'preset', preset: 'ultra-wide' }, state, 0);
    assert.equal(preset.reason, '');
    assert.equal(preset.label, 'Ultra Wide');
    const byRole = {};
    preset.writes.forEach((w) => { byRole[w.role] = w; });
    assert.deepEqual(Object.keys(byRole).sort(),
                     ['cameraModel', 'correction', 'distortion', 'djiFov', 'fov', 'lens', 'preset', 'tilt', 'zoom']);
    assert.equal(byRole.djiFov.value, 78);
    assert.equal(byRole.correction.value, 0.5);
    assert.equal(byRole.lens.value, 0, 'DJI, 0-based');
    assert.equal(byRole.preset.value, 4, 'Ultra Wide is entry 5, 0-based');
    assert.equal(byRole.cameraModel.kind, 'bool');
    assert.equal(core.planFramingWrites({ action: 'preset', preset: 'wide' }, state, null).reason, 'base-unknown');

    const zoom = core.planFramingWrites({ action: 'zoom', direction: 1 }, state, 1);
    assert.deepEqual(zoom.writes.map((w) => w.role), ['djiFov', 'correction', 'zoom', 'preset']);
    assert.equal(zoom.writes[3].value, 1, 'Custom, 1-based');
    assert.match(zoom.label, /^Zoom \d+\.\d\u00b0$/);
    assert.deepEqual(core.planFramingWrites({ action: 'zoom', direction: 1 }, state, null).writes.map((w) => w.role),
                     ['djiFov', 'correction', 'zoom']);
    // Classic: the FOV itself.
    const classic = core.framingState({ lens: 1, fov: 120 }, 0, 1920, 1080);
    assert.equal(classic.lens, 'classic');
    assert.deepEqual(core.planFramingWrites({ action: 'zoom', direction: -1 }, classic, 0).writes[0],
                     { role: 'fov', value: 113.5, kind: 'number' });
    // Without the numbering the mirror decides the lens.
    assert.equal(core.framingState({ lens: 1, cameraModel: false }, null, 0, 0).lens, 'classic');
    assert.equal(core.framingState({ lens: 1, cameraModel: true }, null, 0, 0).lens, 'dji');
});

test('component time is the playhead\'s distance into the clip plus its in point, exact past 2^53', () => {
    assert.equal(core.componentTicks('1000', '400', '50'), '650');
    assert.equal(core.componentTicks('9007199254740993000', '1', '0'), '9007199254740992999');
    assert.equal(core.componentTicks('x', '1', '0'), '');
    assert.equal(core.ticksInside('10', '0', '20'), true);
    assert.equal(core.ticksInside('20', '0', '20'), false);
    assert.equal(core.formatReadout(NaN, 1), '\u2014');
    assert.equal(core.formatReadout(12.345, 2), '12.35');
});

test('the status lines for the new actions', () => {
    const label = 'Slow In, Slow Out';
    assert.equal(core.summarizeEasing({ updated: 3, missing: 2, perClipUndo: true }, label, 'selected', 0).text,
                 'Slow In, Slow Out on 3 clips. 2 clips have no Open 360 Reframe; skipped. Undo takes one Ctrl+Z per clip.');
    assert.equal(core.summarizeEasing({ updated: 1, unchanged: 1 }, label, 'all', 0).tone, 'ok');
    assert.equal(core.summarizeEasing({ missing: 1 }, label, 'all', 0).tone, 'info');
    assert.equal(core.summarizeEasing({ notes: ['base-unknown'] }, label, 'all', 0).tone, 'warn');
    assert.equal(core.summarizeEasing({}, label, 'selected', 2).text, 'No OSV clips selected. 2 selected clips aren\'t OSV.');
    assert.equal(core.summarizeFraming({ ok: true, label: 'Wide', keyframed: true }).text, 'Wide - keyed at the playhead.');
    assert.equal(core.summarizeFraming({ ok: false, reason: 'outside' }).text, 'Move the playhead over the selected clip.');
    assert.equal(core.summarizeFraming({ ok: false, reason: 'failed', error: 'nope' }).tone, 'error');
    assert.match(core.summarizeStabilization({ unsupported: true }, 'Off', 0).text, /Source Settings/);
    assert.equal(core.summarizeStabilization({ updated: 2 }, 'RockSteady', 0).text, 'RockSteady on 2 master clips.');
});

// ===========================================================================
//  The UXP route
// ===========================================================================

/** A UXP world: one sequence, OSV clips with the effect, selected or not. */
function uxpWorld(options) {
    const m = createMockPremiere(options);
    const seq = m.addSequence('seq-1', 'Main', 2);
    const adapter = createUxpAdapter(m.ppro, core);
    adapter.init(() => {});
    const clip = (track, start, extra) => m.addClip(seq, track, Object.assign({
        name: 'CAM_' + start + '.OSV',
        start: start,
        end: start + 1000,
        inPoint: 100,
        outPoint: 1100,
        projectItem: { id: 'osv-' + track + '-' + start, path: 'C:\\DCIM\\CAM_' + start + '.OSV', isSequence: false }
    }, extra || {}));
    return { m, seq, adapter, clip };
}

async function selectedItems(adapter) {
    const active = await adapter.getActiveSequence();
    return { active, items: (await adapter.scan(active, { selectedOnly: true })).items };
}

test('UXP: one Keyframe Easing apply is ONE undo step, on every instance, clips without the effect skipped', async () => {
    const w = uxpWorld();
    const a = w.clip(0, 0, { selected: true });
    const b = w.clip(1, 0, { selected: true });
    const bare = w.clip(0, 5000, { selected: true });
    a.components.push(w.m.reframeComponent());
    b.components.push(w.m.reframeComponent(), w.m.reframeComponent());
    const { active, items } = await selectedItems(w.adapter);
    const r = await w.adapter.setEasing(active, items, { entry: 6, popupBase: null });
    assert.equal(r.updated, 2);
    assert.equal(r.missing, 1);
    assert.equal(r.perClipUndo, false);
    assert.equal(r.learnedBase, 0, 'the untouched popups read 0 on this host');
    assert.deepEqual(w.m.world.undo, [UNDO_EASING]);
    for (const c of [a.components[2], b.components[2], b.components[3]]) {
        assert.equal(w.m.paramValue(c, 'Keyframe Easing'), 5, 'Slow In, Slow Out, entry 6, 0-based');
    }
    assert.equal(bare.components.length, 2, 'nothing was added to the bare clip');
    // Again: nothing to change, no second undo step.
    const again = await w.adapter.setEasing(active, items, { entry: 6, popupBase: 0 });
    assert.equal(again.unchanged, 2);
    assert.equal(again.updated, 0);
    assert.deepEqual(w.m.world.undo, [UNDO_EASING]);
});

test('UXP: on a host that counts from 1 an untouched effect settles nothing, so a fresh one is asked', async () => {
    const w = uxpWorld({ popupBase: 1 });
    const a = w.clip(0, 0, { selected: true });
    a.components.push(w.m.reframeComponent());
    const { active, items } = await selectedItems(w.adapter);
    const r = await w.adapter.setEasing(active, items, { entry: 3, popupBase: null });
    assert.equal(r.learnedBase, 1);
    assert.equal(w.m.paramValue(a.components[2], 'Keyframe Easing'), 3);
});

test('UXP: a Manual Framing preset is one undo step; a keyframed control gets a keyframe at the playhead', async () => {
    const w = uxpWorld();
    const a = w.clip(0, 0, { selected: true });
    const fx = w.m.reframeComponent();
    a.components.push(fx);
    // Tilt keyframed: 0 at component time 100, 20 at 900.
    const tilt = fx.params.filter((p) => p.displayName === 'Tilt')[0];
    tilt.timeVarying = true;
    tilt.keys = [{ t: 100, v: 0 }, { t: 900, v: 20 }];
    w.m.world.playhead = 400;   // component time 400 - 0 + 100 = 500
    const active = await w.adapter.getActiveSequence();
    const r = await w.adapter.writeFraming(active, { action: 'preset', preset: 'asteroid', popupBase: 0 });
    assert.equal(r.ok, true);
    assert.equal(r.keyframed, true);
    assert.equal(r.label, 'Asteroid');
    assert.deepEqual(w.m.world.undo, [UNDO_FRAMING]);
    assert.deepEqual(tilt.keys.filter((k) => k.t === 500), [{ t: 500, v: -90 }]);
    assert.equal(w.m.paramValue(fx, 'Correction Angle'), 1);
    assert.equal(w.m.paramValue(fx, 'Preset'), 2, 'Asteroid, entry 3, 0-based');
    assert.equal(w.m.paramValue(fx, 'Lens'), 0);
    assert.equal(w.m.paramValue(fx, 'Camera Model'), true);
    // DJI FOV is the "FOV" between Zoom and Correction Angle, not the Classic one.
    const fovs = fx.params.filter((p) => p.displayName === 'FOV').map((p) => p.value);
    assert.deepEqual(fovs, [300, 138]);
});

test('UXP: a zoom press walks DJI Studio\'s zoom path and reads back', async () => {
    const w = uxpWorld();
    const a = w.clip(0, 0, { selected: true });
    const fx = w.m.reframeComponent();
    a.components.push(fx);
    w.m.world.playhead = 10;
    const active = await w.adapter.getActiveSequence();
    const before = await w.adapter.readFraming(active, { popupBase: null });
    assert.equal(before.ok, true);
    assert.equal(before.lens, 'dji');
    assert.equal(before.djiFov, 60);
    assert.ok(Math.abs(before.zoom - 142.397) < 0.01);
    const r = await w.adapter.writeFraming(active, { action: 'zoom', direction: -1, popupBase: 0 });
    assert.equal(r.ok, true);
    const after = await w.adapter.readFraming(active, { popupBase: 0 });
    assert.equal(after.djiFov, 53.5);
    assert.equal(after.correction, 0.55);
    assert.equal(w.m.paramValue(fx, 'Preset'), 0, 'Custom');
    assert.deepEqual(w.m.world.undo, [UNDO_FRAMING]);
});

test('UXP: the framing read-outs say why there is nothing to frame', async () => {
    const w = uxpWorld();
    const active = await w.adapter.getActiveSequence();
    assert.equal((await w.adapter.readFraming(active, {})).reason, 'no-selection');
    const other = w.m.addClip(w.seq, 0, { name: 'b-roll.mp4', start: 0, end: 50, selected: true });
    assert.equal((await w.adapter.readFraming(active, {})).reason, 'not-osv');
    other.selected = false;
    const a = w.clip(0, 2000, { selected: true });
    w.m.world.playhead = 2500;
    assert.equal((await w.adapter.readFraming(active, {})).reason, 'no-effect');
    a.components.push(w.m.reframeComponent());
    w.m.world.playhead = 5000;
    assert.equal((await w.adapter.readFraming(active, {})).reason, 'outside');
    w.clip(1, 2000, { selected: true });
    assert.equal((await w.adapter.readFraming(active, {})).reason, 'many-selected');
});

test('UXP: Stabilisation reaches each master clip\'s Source Settings once, in one undo step', async () => {
    const w = uxpWorld();
    const pi = { id: 'shared', path: 'C:\\DCIM\\CAM.OSV', isSequence: false, components: [w.m.sourceSettingsComponent()] };
    w.clip(0, 0, { selected: true, projectItem: pi });
    w.clip(0, 3000, { selected: true, projectItem: pi });
    w.clip(1, 0, { selected: true });   // a master clip showing no Source Settings
    const { active, items } = await selectedItems(w.adapter);
    const r = await w.adapter.setStabilization(active, items, { entry: 4, popupBase: null });
    assert.equal(r.updated, 1, 'one master clip, two timeline clips');
    assert.equal(r.missing, 1);
    assert.equal(w.m.paramValue(pi.components[0], 'Stabilisation'), 3, 'Smooth, entry 4, 0-based');
    assert.deepEqual(w.m.world.undo, [UNDO_STABILIZATION]);
});

test('UXP: a host whose project items have no component chain cannot set Stabilisation', async () => {
    const w = uxpWorld();
    w.m.world.noMasterChains = true;
    w.clip(0, 0, { selected: true });
    const { active, items } = await selectedItems(w.adapter);
    assert.equal((await w.adapter.setStabilization(active, items, { entry: 1 })).unsupported, true);
    assert.deepEqual(w.adapter.capabilities(), { undoGroups: true, stabilization: true });
});

// ===========================================================================
//  The CEP route (the real host.jsx in the ExtendScript mock)
// ===========================================================================

function cepWorld(options) {
    const w = createWorld(options);
    const seq = w.addSequence('seq-1', 'Main', 2);
    w.load(HOST);
    const bridge = {
        evalScript(script, cb) {
            const answer = w.evalScript(script);
            setImmediate(() => cb(answer));
        },
        addEventListener() {},
        removeEventListener() {}
    };
    const adapter = createCepAdapter(bridge, core);
    const osv = (track, start, extra) => w.addClip(seq, track, Object.assign({
        name: 'CAM_' + start + '.OSV',
        start: start,
        end: start + 1000,
        inPoint: 100,
        path: 'C:\\DCIM\\CAM_' + start + '.OSV',
        projectItemId: 'pi-' + track + '-' + start
    }, extra || {}));
    return { w, seq, adapter, osv };
}

test('CEP: Keyframe Easing on every instance, each value its own history step', async () => {
    const c = cepWorld();
    const a = c.osv(0, 0, { selected: true });
    a.components.push(c.w.reframeComponent(), c.w.reframeComponent());
    c.osv(1, 0, { selected: true });   // no effect
    const active = await c.adapter.getActiveSequence();
    const { items } = await c.adapter.scan(active, { selectedOnly: true });
    const r = await c.adapter.setEasing(active, items, { entry: 7, popupBase: null });
    assert.equal(r.updated, 1);
    assert.equal(r.missing, 1);
    assert.equal(r.perClipUndo, true);
    assert.equal(r.learnedBase, 0);
    assert.equal(c.w.paramValue(a.components[2], 'Keyframe Easing'), 6, 'Linear, entry 7, 0-based');
    assert.equal(c.w.paramValue(a.components[3], 'Keyframe Easing'), 6);
    assert.deepEqual(c.w.world.setValueCalls.map((x) => x[2]), [1, 1], 'updateUI on every write');
    assert.deepEqual(c.adapter.capabilities(), { undoGroups: false, stabilization: true });
});

test('CEP: a framing preset keys a keyframed control at the playhead (addKey + setValueAtKey)', async () => {
    const c = cepWorld();
    const a = c.osv(0, 0, { selected: true });
    const fx = c.w.reframeComponent();
    a.components.push(fx);
    const pan = fx.properties.filter((p) => p.displayName === 'Pan')[0];
    const tilt = fx.properties.filter((p) => p.displayName === 'Tilt')[0];
    tilt._timeVarying = true;
    tilt._keys = [{ t: 100, v: 0 }, { t: 900, v: 30 }];
    c.w.world.playhead = 300;   // component time 300 - 0 + 100 = 400
    const active = await c.adapter.getActiveSequence();
    const read = await c.adapter.readFraming(active, {});
    assert.equal(read.ok, true);
    assert.equal(read.tilt, 11.25);
    assert.equal(read.pan, pan.getValue());
    const r = await c.adapter.writeFraming(active, { action: 'preset', preset: 'dewarping', popupBase: null });
    assert.equal(r.ok, true);
    assert.equal(r.keyframed, true);
    assert.deepEqual(c.w.world.keyCalls, [['addKey', 'Tilt', '400'], ['setValueAtKey', 'Tilt', '400', 0, 1]]);
    assert.equal(c.w.paramValue(fx, 'Preset'), 5, 'Dewarping, entry 6, 0-based');
    assert.equal(c.w.paramValue(fx, 'Correction Angle'), 0.2);
});

test('CEP: Stabilisation through ProjectItem.videoComponents(), and a DOM without it says so', async () => {
    const c = cepWorld();
    const a = c.osv(0, 0, { selected: true });
    const active = await c.adapter.getActiveSequence();
    const { items } = await c.adapter.scan(active, { selectedOnly: true });
    const r = await c.adapter.setStabilization(active, items, { entry: 1, popupBase: null });
    assert.equal(r.updated, 1);
    assert.equal(c.w.paramValue(a.projectItem._components[0], 'Stabilisation'), 0, 'Off, 0-based');

    const d = cepWorld();
    const b = d.osv(0, 0, { selected: true });
    delete b.projectItem.videoComponents;
    const active2 = await d.adapter.getActiveSequence();
    const scanned = await d.adapter.scan(active2, { selectedOnly: true });
    assert.equal((await d.adapter.setStabilization(active2, scanned.items, { entry: 1 })).unsupported, true);
});

// ===========================================================================
//  The controller
// ===========================================================================

function osvItem(key) {
    return { key: key, trackIndex: 0, startTicks: '0', endTicks: '100', inTicks: '0', outTicks: '100',
             projectItemId: 'pi-' + key, name: key, mediaPath: 'C:/m/' + key + '.OSV' };
}

function controllerWith(adapterExtras, storageInit) {
    const calls = [];
    const s = { onEvent: null, selected: [osvItem('a'), osvItem('b')], framing: { ok: false, reason: 'no-selection' } };
    const adapter = Object.assign({
        init(fn) { s.onEvent = fn; },
        dispose() {},
        getActiveSequence: async () => ({ id: 's1', name: 'S', projectId: 'p' }),
        getSequenceIds: async () => ['s1'],
        signature: async () => 'sig',
        scan: async (seq, o) => ({ items: o && o.selectedOnly ? s.selected.slice() : [], otherCount: 0 }),
        apply: async () => ({ applied: 0 }),
        checkEffect: async () => ({ available: true }),
        capabilities: () => ({ undoGroups: false, stabilization: true }),
        setEasing: async (seq, items, req) => {
            calls.push(['setEasing', items.map((i) => i.key), req]);
            return { updated: items.length, missing: 0, perClipUndo: true, learnedBase: 1 };
        },
        setStabilization: async (seq, items, req) => {
            calls.push(['setStabilization', items.map((i) => i.key), req]);
            return { updated: 1 };
        },
        writeFraming: async (seq, req) => {
            calls.push(['writeFraming', req]);
            return { ok: true, label: 'Wide', keyframed: false };
        },
        readFraming: async () => {
            calls.push(['readFraming']);
            return s.framing;
        }
    }, adapterExtras || {});
    const timers = fakeTimers();
    const map = new Map(Object.entries(storageInit || {}));
    const storage = { get: (k) => (map.has(k) ? map.get(k) : null), set: (k, v) => map.set(k, v) };
    const states = [];
    const ctl = createController({ core, adapter, timers, storage, onChange: (st) => states.push(st) });
    const step = async (ms) => {
        await settle();
        timers.advance(ms || 0);
        await settle();
        await ctl.idle();
        await settle();
    };
    return { ctl, calls, s, step, timers, map, last: () => states[states.length - 1], states };
}

test('controller: Apply of a Keyframe Animation preset reports, remembers the numbering, busy only on its card',
     async () => {
    const t = controllerWith();
    t.ctl.start();
    await t.step(0);
    t.ctl.setEasing('fast-in-slow-out');
    const running = t.ctl.applyEasingSelected();
    assert.equal(t.last().busyAction, 'easing');
    await running;
    await t.step(0);
    assert.deepEqual(t.calls.filter((c) => c[0] === 'setEasing')[0],
                     ['setEasing', ['a', 'b'], { entry: 3, popupBase: null }]);
    assert.equal(t.last().status.text, 'Fast In, Slow Out on 2 clips. Undo takes one Ctrl+Z per clip.');
    assert.equal(t.last().settings.popupBase, 1, 'learned');
    assert.equal(t.last().busyAction, '');
    assert.deepEqual(t.last().capabilities, { undoGroups: false, stabilization: true });
    await t.step(300);
    assert.equal(JSON.parse(t.map.get(STORAGE_KEY)).easing, 'fast-in-slow-out');
    t.ctl.stop();
});

test('controller: the framing read-outs follow a poll and the selection event, and pause while an action runs',
     async () => {
    const t = controllerWith();
    t.ctl.start();
    await t.step(0);
    t.s.framing = { ok: true, name: 'CAM', lens: 'dji', zoom: 142.4, djiFov: 60, correction: 0.6, pan: 1, tilt: 2, roll: 3 };
    await t.step(FRAMING_POLL_MS);
    assert.equal(t.last().framing.ok, true);
    assert.equal(t.last().framing.fov, 60, 'the DJI FOV on the DJI lens');
    t.s.framing = { ok: false, reason: 'outside' };
    t.s.onEvent('selection');
    await t.step(0);
    assert.deepEqual(t.last().framing, { ok: false, reason: 'outside' });
    // A preset press writes, then reads back.
    const before = t.calls.filter((c) => c[0] === 'readFraming').length;
    await t.ctl.framingPreset('wide');
    await t.step(0);
    assert.deepEqual(t.calls.filter((c) => c[0] === 'writeFraming')[0], ['writeFraming', { popupBase: null, action: 'preset', preset: 'wide' }]);
    assert.ok(t.calls.filter((c) => c[0] === 'readFraming').length > before);
    assert.equal(t.last().status.text, 'Wide.');
    await t.ctl.zoomStep(-4);
    assert.deepEqual(t.calls.filter((c) => c[0] === 'writeFraming')[1][1].direction, -1);
    t.ctl.stop();
    assert.equal(t.timers.pendingCount(), 0, 'every timer stopped');
});

test('controller: Stabilisation and the controls card are remembered', async () => {
    const t = controllerWith();
    t.ctl.start();
    await t.step(0);
    t.ctl.setStabilization('rocksteady');
    t.ctl.setHintOpen(false);
    await t.ctl.applyStabilization();
    await t.step(300);
    assert.deepEqual(t.calls.filter((c) => c[0] === 'setStabilization')[0], ['setStabilization', ['a', 'b'], { entry: 4, popupBase: null }]);
    assert.equal(t.last().status.text, 'RockSteady on 1 master clip.');
    const stored = JSON.parse(t.map.get(STORAGE_KEY));
    assert.equal(stored.stabilization, 'rocksteady');
    assert.equal(stored.hintOpen, false);
    t.ctl.stop();

    const t2 = controllerWith({}, { [STORAGE_KEY]: t.map.get(STORAGE_KEY) });
    t2.ctl.start();
    await t2.step(0);
    assert.equal(t2.last().settings.hintOpen, false);
    t2.ctl.stop();
});

test('controller: an adapter without the new calls fails into the status line, never throws', async () => {
    const t = controllerWith({ setEasing: undefined, capabilities: undefined });
    t.ctl.start();
    await t.step(0);
    await t.ctl.applyEasingSelected();
    await t.step(0);
    assert.equal(t.last().status.tone, 'error');
    assert.match(t.last().status.text, /cannot do that/);
    t.ctl.stop();
});
