// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvcore.test.js - the pure rules of the panel.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const core = require('../shared/osvcore.js');
const { OSV_PATHS, NOT_OSV_PATHS } = require('./fixtures/paths.js');
const { fakeTimers } = require('./fixtures/timers.js');

// ---------------------------------------------------------------------------
//  Media detection
// ---------------------------------------------------------------------------

test('isOsvMediaPath recognises .osv and .lrf in any case and with either separator', () => {
    for (const p of OSV_PATHS) {
        assert.equal(core.isOsvMediaPath(p), true, JSON.stringify(p));
    }
});

test('isOsvMediaPath rejects other media, folders named .osv and near misses', () => {
    for (const p of NOT_OSV_PATHS) {
        assert.equal(core.isOsvMediaPath(p), false, JSON.stringify(p));
    }
});

test('isOsvMediaPath rejects anything that is not a string', () => {
    for (const v of [null, undefined, 0, 1, true, {}, [], ['a.osv'], () => 'a.osv', { toString: () => 'a.osv' }]) {
        assert.equal(core.isOsvMediaPath(v), false);
    }
    assert.equal(core.isOsvMediaPath('a'.repeat(40000) + '.osv'), false, 'absurdly long paths are refused');
});

// ---------------------------------------------------------------------------
//  Effect identity
// ---------------------------------------------------------------------------

test('the reframe effect is recognised with and without the AE. registration prefix', () => {
    assert.equal(core.REFRAME_HOST_MATCH_NAME, 'AE.OpenOSV.Open360Reframe');
    assert.equal(core.isReframeMatchName('AE.OpenOSV.Open360Reframe'), true);
    assert.equal(core.isReframeMatchName('OpenOSV.Open360Reframe'), true);
    assert.equal(core.isReframeMatchName('  AE.OpenOSV.Open360Reframe '), true);
    assert.equal(core.isReframeMatchName('AE.OpenOSV.SourceSettings'), false);
    assert.equal(core.isReframeMatchName('AE.AE.OpenOSV.Open360Reframe'), false, 'one prefix only');
    assert.equal(core.isReframeMatchName('ae.openosv.open360reframe'), false, 'match names are exact');
    assert.equal(core.isReframeMatchName(null), false);
    assert.equal(core.isReframeMatchName(42), false);
});

test('hasReframeEffect / countReframeEffects look through a whole chain', () => {
    const chain = ['AE.ADBE Opacity', 'AE.ADBE Motion', 'AE.ADBE Lumetri', 'AE.OpenOSV.Open360Reframe'];
    assert.equal(core.hasReframeEffect(chain), true);
    assert.equal(core.countReframeEffects(chain.concat(['OpenOSV.Open360Reframe'])), 2);
    assert.equal(core.hasReframeEffect(['AE.ADBE Opacity', 'AE.ADBE Motion']), false);
    assert.equal(core.hasReframeEffect([]), false);
    assert.equal(core.hasReframeEffect(null), false);
    assert.equal(core.hasReframeEffect('AE.OpenOSV.Open360Reframe'), false, 'a string is not a chain');
    assert.equal(core.hasReframeEffect([null, undefined, 7, {}]), false);
});

test('pickHostMatchName prefers the registered name and returns null when it is not installed', () => {
    assert.equal(core.pickHostMatchName(['PR.ADBE Solarize', 'AE.OpenOSV.Open360Reframe']), 'AE.OpenOSV.Open360Reframe');
    assert.equal(core.pickHostMatchName(['OpenOSV.Open360Reframe']), 'OpenOSV.Open360Reframe');
    assert.equal(core.pickHostMatchName(['PR.ADBE Solarize']), null);
    assert.equal(core.pickHostMatchName(null), null);
});

// ---------------------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------------------

test('settings default to auto-apply on, DJI lens, drag sensitivity untouched at 2.0', () => {
    // [WP-EASING] The newer settings: no easing picked yet, Horizon Leveling
    // (the Source Settings default), the controls card open for a new user,
    // and the popup numbering not learned yet.
    assert.deepEqual(core.sanitizeSettings(null), {
        autoApply: true, lens: 'dji', dragEnabled: false, dragSensitivity: 2, easing: 'none', stabilization: 'horizon', hintOpen: true, popupBase: null
    });
    assert.deepEqual(core.sanitizeSettings(undefined), core.sanitizeSettings({}));
});

test('settings are repaired field by field', () => {
    const s = core.sanitizeSettings({ autoApply: 'yes', lens: 'CLASSIC', dragEnabled: 1, dragSensitivity: 'fast',
                                      easing: 'bounce', stabilization: 'gyro', hintOpen: 'no', popupBase: 2 });
    assert.deepEqual(s, { autoApply: true, lens: 'dji', dragEnabled: false, dragSensitivity: 2, easing: 'none', stabilization: 'horizon', hintOpen: true, popupBase: null });
    assert.equal(core.sanitizeSettings({ easing: 'slow-in-slow-out' }).easing, 'slow-in-slow-out');
    assert.equal(core.sanitizeSettings({ stabilization: 'rocksteady' }).stabilization, 'rocksteady');
    assert.equal(core.sanitizeSettings({ hintOpen: false }).hintOpen, false);
    assert.equal(core.sanitizeSettings({ popupBase: 0 }).popupBase, 0);
    assert.equal(core.sanitizeSettings({ popupBase: 1 }).popupBase, 1);
    assert.equal(core.sanitizeSettings({ autoApply: false }).autoApply, false);
    assert.equal(core.sanitizeSettings({ lens: 'classic' }).lens, 'classic');
    assert.equal(core.sanitizeSettings({ dragSensitivity: 99 }).dragSensitivity, 10);
    assert.equal(core.sanitizeSettings({ dragSensitivity: -3 }).dragSensitivity, 0.1);
    assert.equal(core.sanitizeSettings({ dragSensitivity: 1.23456 }).dragSensitivity, 1.23);
    assert.equal(core.sanitizeSettings({ dragSensitivity: NaN }).dragSensitivity, 2);
    assert.equal(core.sanitizeSettings({ dragSensitivity: Infinity }).dragSensitivity, 2);
    assert.equal(core.sanitizeSettings({ dragSensitivity: true }).dragSensitivity, 2, 'a boolean is not a number');
    assert.equal(core.sanitizeSettings({ dragSensitivity: null }).dragSensitivity, 2);
});

test('stored settings round-trip, and garbage storage gives the defaults', () => {
    const s = { autoApply: false, lens: 'classic', dragEnabled: true, dragSensitivity: 3.5, easing: 'linear-smooth',
                stabilization: 'off', hintOpen: false, popupBase: 0 };
    assert.deepEqual(core.parseSettings(core.serializeSettings(s)), s);
    for (const junk of [null, '', '{', 'null', '42', '"text"', '[]', 'x'.repeat(5000)]) {
        assert.deepEqual(core.parseSettings(junk), core.sanitizeSettings(null), String(junk).slice(0, 20));
    }
});

// ---------------------------------------------------------------------------
//  "What is new"
// ---------------------------------------------------------------------------

/** An item the way an adapter describes it. */
function item(key, track, start, end, pi, inT, outT) {
    return {
        key: key,
        trackIndex: track,
        startTicks: String(start),
        endTicks: String(end),
        inTicks: String(inT === undefined ? 0 : inT),
        outTicks: String(outT === undefined ? end - start : outT),
        projectItemId: pi,
        name: key
    };
}

test('diffItems: a first look at an empty sequence reports everything as added', () => {
    const now = [item('a', 0, 0, 100, 'p1'), item('b', 1, 0, 100, 'p2')];
    const d = core.diffItems([], now);
    assert.deepEqual(d.added.map((i) => i.key), ['a', 'b']);
    assert.equal(d.carried.length, 0);
});

test('diffItems: an unchanged sequence has nothing new', () => {
    const now = [item('a', 0, 0, 100, 'p1')];
    const d = core.diffItems(now, now.map((i) => Object.assign({}, i)));
    assert.equal(d.added.length, 0);
    assert.equal(d.carried.length, 1);
});

test('diffItems: a dropped clip is new, whatever else is on the timeline', () => {
    const before = [item('a', 0, 0, 100, 'p1')];
    const now = before.concat([item('b', 0, 100, 200, 'p2')]);
    assert.deepEqual(core.diffItems(before, now).added.map((i) => i.key), ['b']);
});

test('diffItems: a multi-clip drop is all new at once', () => {
    const now = [item('a', 0, 0, 100, 'p1'), item('b', 0, 100, 200, 'p2'), item('c', 0, 200, 300, 'p3')];
    assert.equal(core.diffItems([], now).added.length, 3);
});

test('diffItems: moving a clip along its track is not new (UXP keys change with the start)', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1')];
    const now = [item('0:500:p1', 0, 500, 600, 'p1')];
    const d = core.diffItems(before, now);
    assert.equal(d.added.length, 0);
    assert.equal(d.carried.length, 1);
    assert.equal(d.vanished.length, 0);
});

test('diffItems: moving a clip to another track is not new', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1')];
    const now = [item('2:0:p1', 2, 0, 100, 'p1')];
    assert.equal(core.diffItems(before, now).added.length, 0);
});

test('diffItems: trimming the head (start and in point change) is not new', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1', 0, 100)];
    const now = [item('0:30:p1', 0, 30, 100, 'p1', 30, 100)];
    assert.equal(core.diffItems(before, now).added.length, 0);
});

test('diffItems: a razor cut is not new - the halves continue the same source', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1', 0, 100)];
    const now = [item('0:0:p1', 0, 0, 40, 'p1', 0, 40), item('0:40:p1', 0, 40, 100, 'p1', 40, 100)];
    const d = core.diffItems(before, now);
    assert.equal(d.added.length, 0);
    assert.equal(d.carried.length, 2);
});

test('diffItems: the same clip dropped twice end to end IS new (the source restarts)', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1', 0, 100)];
    const now = before.concat([item('0:100:p1', 0, 100, 200, 'p1', 0, 100)]);
    assert.deepEqual(core.diffItems(before, now).added.map((i) => i.key), ['0:100:p1']);
});

test('diffItems: a move and a fresh drop of the same clip - one carried, one new', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1')];
    const now = [item('0:300:p1', 0, 300, 400, 'p1'), item('0:1000:p1', 0, 1000, 1100, 'p1')];
    const d = core.diffItems(before, now);
    assert.equal(d.added.length, 1, 'one vanished item explains one newcomer');
    assert.equal(d.added[0].key, '0:1000:p1', 'the nearest start is the moved one');
});

test('diffItems: a move is paired on the same track before another track', () => {
    const before = [item('0:0:p1', 0, 0, 100, 'p1')];
    const now = [item('3:0:p1', 3, 0, 100, 'p1'), item('0:900:p1', 0, 900, 1000, 'p1')];
    const d = core.diffItems(before, now);
    assert.deepEqual(d.added.map((i) => i.key), ['3:0:p1']);
});

test('diffItems: stable CEP keys - a moved nodeId is simply carried', () => {
    const before = [item('node-7', 0, 0, 100, 'p1')];
    const now = [item('node-7', 1, 500, 600, 'p1')];
    assert.equal(core.diffItems(before, now).added.length, 0);
});

test('diffItems: deleted clips are reported as vanished and never as added', () => {
    const before = [item('a', 0, 0, 100, 'p1'), item('b', 0, 100, 200, 'p2')];
    const d = core.diffItems(before, [before[0]]);
    assert.equal(d.added.length, 0);
    assert.deepEqual(d.vanished.map((i) => i.key), ['b']);
});

test('diffItems survives garbage input', () => {
    const d = core.diffItems(null, [null, 7, {}, { key: '' }, item('ok', 0, 0, 1, 'p')]);
    assert.deepEqual(d.added.map((i) => i.key), ['ok']);
    assert.deepEqual(core.diffItems(undefined, undefined), { added: [], carried: [], vanished: [] });
});

// ---------------------------------------------------------------------------
//  Parameter plan
// ---------------------------------------------------------------------------

/** A fresh instance's parameter list as a host numbering popups from `base` reports it. */
function freshParams(base) {
    return [
        { index: 0, name: 'Output Resolution', value: base, timeVarying: false },
        { index: 2, name: 'Preset', value: base + 3, timeVarying: false },
        { index: 6, name: 'FOV', value: 120, timeVarying: false },
        { index: 9, name: 'Camera Model', value: true, timeVarying: false },
        { index: 11, name: 'FOV', value: 60, timeVarying: false },
        { index: 13, name: 'Drag Sensitivity', value: 2, timeVarying: false },
        { index: 14, name: 'Lens', value: base, timeVarying: false }
    ];
}

test('planParamWrites: DJI with default drag writes nothing (the effect defaults already)', () => {
    for (const base of [0, 1]) {
        const plan = core.planParamWrites(freshParams(base), { lens: 'dji', dragEnabled: false });
        assert.deepEqual(plan, { writes: [], notes: [] });
    }
});

test('planParamWrites: Classic on a host counting popups from 0', () => {
    const plan = core.planParamWrites(freshParams(0), { lens: 'classic' });
    assert.deepEqual(plan.writes, [
        { index: 14, name: 'Lens', value: 1 },
        { index: 2, name: 'Preset', value: 0 },
        { index: 9, name: 'Camera Model', value: false }
    ]);
    assert.deepEqual(plan.notes, []);
});

test('planParamWrites: Classic on a host counting popups from 1', () => {
    const plan = core.planParamWrites(freshParams(1), { lens: 'classic' });
    assert.deepEqual(plan.writes.map((w) => [w.name, w.value]), [['Lens', 2], ['Preset', 1], ['Camera Model', false]]);
});

test('planParamWrites: a numeric checkbox gets a numeric off', () => {
    const params = freshParams(0).map((p) => (p.name === 'Camera Model' ? Object.assign({}, p, { value: 1 }) : p));
    const plan = core.planParamWrites(params, { lens: 'classic' });
    assert.deepEqual(plan.writes.filter((w) => w.name === 'Camera Model')[0].value, 0);
});

test('planParamWrites: an unreadable lens default leaves the lens alone and says so', () => {
    for (const odd of [2, -1, 0.5, '0', null, undefined, true]) {
        const params = freshParams(0).map((p) => (p.name === 'Lens' ? Object.assign({}, p, { value: odd }) : p));
        const plan = core.planParamWrites(params, { lens: 'classic' });
        assert.equal(plan.writes.length, 0, String(odd));
        assert.deepEqual(plan.notes, ['lens-unreadable']);
    }
});

test('planParamWrites: missing or keyframed parameters are never written', () => {
    const noLens = freshParams(0).filter((p) => p.name !== 'Lens');
    assert.deepEqual(core.planParamWrites(noLens, { lens: 'classic' }).notes, ['lens-missing']);
    const keyed = freshParams(0).map((p) => (p.name === 'Lens' ? Object.assign({}, p, { timeVarying: true }) : p));
    assert.deepEqual(core.planParamWrites(keyed, { lens: 'classic' }), { writes: [], notes: ['lens-keyframed'] });
    const keyedDrag = freshParams(0).map((p) => (p.name === 'Drag Sensitivity' ? Object.assign({}, p, { timeVarying: true }) : p));
    assert.deepEqual(core.planParamWrites(keyedDrag, { dragEnabled: true, dragSensitivity: 3 }).notes, ['drag-keyframed']);
});

test('planParamWrites: drag sensitivity is written clamped, and only when it differs', () => {
    assert.deepEqual(core.planParamWrites(freshParams(0), { dragEnabled: true, dragSensitivity: 3.5 }).writes,
                     [{ index: 13, name: 'Drag Sensitivity', value: 3.5 }]);
    assert.deepEqual(core.planParamWrites(freshParams(0), { dragEnabled: true, dragSensitivity: 50 }).writes[0].value, 10);
    assert.deepEqual(core.planParamWrites(freshParams(0), { dragEnabled: true, dragSensitivity: 2 }).writes, []);
    assert.deepEqual(core.planParamWrites(freshParams(0), { dragEnabled: false, dragSensitivity: 5 }).writes, []);
});

test('planParamWrites survives garbage', () => {
    assert.deepEqual(core.planParamWrites(null, null), { writes: [], notes: [] });
    assert.deepEqual(core.planParamWrites([null, 7, { name: 'Lens' }, { name: 'Lens', index: -1, value: 0 }], { lens: 'classic' }),
                     { writes: [], notes: ['lens-missing'] });
});

// ---------------------------------------------------------------------------
//  Debounce
// ---------------------------------------------------------------------------

test('createDebouncer collapses a burst into one call after the quiet period', () => {
    const t = fakeTimers();
    let calls = 0;
    const d = core.createDebouncer(() => { calls += 1; }, { waitMs: 300, maxWaitMs: 2000 }, t);
    for (let i = 0; i < 10; i += 1) {
        d.trigger();
        t.advance(50);
    }
    assert.equal(calls, 0, 'still inside the burst');
    t.advance(300);
    assert.equal(calls, 1);
    assert.equal(d.pending(), false);
});

test('createDebouncer never delays a continuous storm beyond maxWait', () => {
    const t = fakeTimers();
    let calls = 0;
    const d = core.createDebouncer(() => { calls += 1; }, { waitMs: 300, maxWaitMs: 1000 }, t);
    for (let i = 0; i < 40; i += 1) {
        d.trigger();
        t.advance(100);
    }
    // 4000 ms of events every 100 ms: served about once per second, not never.
    assert.ok(calls >= 3 && calls <= 5, 'calls = ' + calls);
});

test('createDebouncer flush / cancel, and a throwing callback is contained', () => {
    const t = fakeTimers();
    const errors = [];
    let calls = 0;
    const d = core.createDebouncer(() => { calls += 1; throw new Error('boom'); }, { waitMs: 100, onError: (e) => errors.push(e.message) }, t);
    d.trigger();
    d.flush();
    assert.equal(calls, 1);
    assert.deepEqual(errors, ['boom']);
    d.trigger();
    d.cancel();
    t.advance(1000);
    assert.equal(calls, 1);
});

// ---------------------------------------------------------------------------
//  Status copy
// ---------------------------------------------------------------------------

test('summarize: the lines the status bar shows', () => {
    assert.deepEqual(core.summarize({ applied: 1 }, 'auto'), { tone: 'ok', text: 'Applied to 1 clip.', quiet: false });
    assert.equal(core.summarize({ applied: 3, already: 2 }, 'all').text, 'Applied to 3 clips. 2 clips already had it.');
    assert.equal(core.summarize({ applied: 0, already: 0 }, 'auto').quiet, true, 'an automatic pass that did nothing is silent');
    assert.equal(core.summarize({ already: 4 }, 'all').text, 'Nothing to do. 4 clips have it already.');
    assert.equal(core.summarize({ already: 1 }, 'selected').text, 'Nothing to do. 1 clip has it already.');
    assert.equal(core.summarize({ notOsv: 2 }, 'selected').text, 'No OSV clips selected. 2 selected clips aren\'t OSV.');
    assert.equal(core.summarize({}, 'selected').text, 'Select an OSV clip on the timeline first.');
    assert.equal(core.summarize({}, 'all').text, 'No OSV clips in this sequence.');
    const fail = core.summarize({ failed: 2, errors: ['Premiere refused the change'] }, 'selected');
    assert.equal(fail.tone, 'error');
    assert.equal(fail.text, 'Couldn\'t apply: Premiere refused the change.');
    const mixed = core.summarize({ applied: 2, failed: 1, errors: ['x'] }, 'all');
    assert.equal(mixed.tone, 'warn');
    assert.equal(mixed.text, 'Applied to 2 clips. 1 clip failed: x.');
    assert.equal(core.summarize({ missingEffect: true }, 'auto').tone, 'error');
    assert.match(core.summarize({ applied: 1, notes: ['lens-unreadable'] }, 'auto').text, /Lens left at DJI/);
    assert.match(core.summarize({ applied: 1, notes: ['params-failed'] }, 'auto').text, /didn't stick/);
    assert.equal(core.summarize(null, 'all').text, 'No OSV clips in this sequence.');
});

test('shortError keeps the status line short and single-line', () => {
    assert.equal(core.shortError('  a\n\nb  '), 'a b');
    assert.equal(core.shortError(''), 'unknown error');
    assert.equal(core.shortError('x'.repeat(500)).length, 140);
});

test('clockLabel is HH:MM', () => {
    assert.equal(core.clockLabel(new Date(2026, 8, 23, 9, 5)), '09:05');
    assert.equal(core.clockLabel(new Date(2026, 8, 23, 23, 59)), '23:59');
    assert.match(core.clockLabel(new Date(NaN)), /^\d\d:\d\d$/);
});

// ---------------------------------------------------------------------------
//  ExtendScript literal
// ---------------------------------------------------------------------------

test('toExtendScriptLiteral escapes the two characters ES3 cannot hold in a string', () => {
    const text = core.toExtendScriptLiteral({ path: 'C:\\a\u2028b\u2029c"d' });
    assert.equal(text.indexOf('\u2028'), -1);
    assert.equal(text.indexOf('\u2029'), -1);
    assert.deepEqual(JSON.parse(text), { path: 'C:\\a\u2028b\u2029c"d' });
    // It must also be valid ES3-era source: evaluate it as an expression.
    assert.deepEqual(new Function('return ' + text)(), { path: 'C:\\a\u2028b\u2029c"d' });
    const cyclic = {};
    cyclic.self = cyclic;
    assert.equal(core.toExtendScriptLiteral(cyclic), 'null');
    assert.equal(core.toExtendScriptLiteral(undefined), 'null');
});
