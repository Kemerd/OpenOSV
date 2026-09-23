// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// spring.test.js - the physics behind every animation in the panel.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const spring = require('../shared/spring.js');

/** Integrate `seconds` at 60 Hz and return every position. */
function run(cfg, from, to, seconds) {
    let s = { x: from, v: 0 };
    const xs = [];
    for (let t = 0; t < seconds; t += 1 / 60) {
        s = spring.stepSpring(s, to, 1 / 60, cfg);
        xs.push(s.x);
    }
    return { state: s, xs: xs };
}

test('springConfig turns response and damping fraction into stiffness and damping', () => {
    const c = spring.springConfig(0.5, 1);
    assert.ok(Math.abs(c.stiffness - Math.pow(2 * Math.PI / 0.5, 2)) < 1e-9);
    assert.ok(Math.abs(c.damping - 4 * Math.PI / 0.5) < 1e-9);
    for (const bad of [[NaN, NaN], [-1, -1], [0, 0], ['x', {}]]) {
        const g = spring.springConfig(bad[0], bad[1]);
        assert.ok(isFinite(g.stiffness) && g.stiffness > 0 && isFinite(g.damping) && g.damping > 0);
    }
});

test('the snappy spring settles within a second with a touch of overshoot', () => {
    const r = run(spring.PRESETS.snappy, 0, 1, 1.0);
    assert.ok(Math.abs(r.state.x - 1) < 0.005, 'x = ' + r.state.x);
    const peak = Math.max.apply(null, r.xs);
    assert.ok(peak > 1.0, 'it overshoots a little, which reads as physical');
    assert.ok(peak < 1.06, 'but never more than a few percent: ' + peak);
});

test('the gentle spring barely overshoots', () => {
    const r = run(spring.PRESETS.gentle, 0, 1, 1.5);
    assert.ok(Math.abs(r.state.x - 1) < 0.005);
    assert.ok(Math.max.apply(null, r.xs) < 1.01);
});

test('a stalled frame timer never makes a spring jump or explode', () => {
    const s = spring.stepSpring({ x: 0, v: 0 }, 1, 30, spring.PRESETS.snappy);
    assert.ok(isFinite(s.x) && s.x > 0 && s.x < 1.2, 'x = ' + s.x);
});

test('stepSpring survives garbage', () => {
    const s = spring.stepSpring(null, NaN, NaN, null);
    assert.deepEqual(s, { x: 0, v: 0 });
    const t = spring.stepSpring({ x: NaN, v: Infinity }, 1, 1 / 60, { stiffness: NaN, damping: NaN });
    assert.ok(isFinite(t.x) && isFinite(t.v));
});

/** A scheduler driven by the test. */
function manualScheduler() {
    let now = 0;
    let queue = [];
    return {
        request(cb) { queue.push(cb); return queue.length; },
        now() { return now; },
        frame(ms) {
            now += ms;
            const q = queue;
            queue = [];
            q.forEach((cb) => cb());
            return q.length;
        },
        waiting() { return queue.length; }
    };
}

test('the animator draws every frame, settles exactly on the target and then stops', () => {
    const sched = manualScheduler();
    const a = spring.createAnimator(sched);
    const frames = [];
    let done = 0;
    a.set('k', 0);
    a.animate('k', 10, { scale: 10, onFrame: (x) => frames.push(x), onDone: () => { done += 1; } });
    for (let i = 0; i < 200 && sched.waiting() > 0; i += 1) {
        sched.frame(16);
    }
    assert.equal(sched.waiting(), 0, 'the loop stops once nothing moves');
    assert.equal(frames[frames.length - 1], 10, 'the last frame is exactly the target');
    assert.equal(done, 1);
    assert.equal(a.isMoving('k'), false);
});

test('retargeting mid-flight keeps position and velocity (no restart)', () => {
    const sched = manualScheduler();
    const a = spring.createAnimator(sched);
    a.set('k', 0);
    a.animate('k', 1, {});
    for (let i = 0; i < 6; i += 1) {
        sched.frame(16);
    }
    const mid = a.value('k');
    assert.ok(mid > 0 && mid < 1);
    a.animate('k', 0, {});
    sched.frame(16);
    // Still moving up for a moment: the velocity carried over.
    assert.ok(a.value('k') >= mid - 0.05, 'no teleport back to the start');
});

test('animating to where a spring already rests draws once and schedules nothing', () => {
    const sched = manualScheduler();
    const a = spring.createAnimator(sched);
    const frames = [];
    a.set('k', 5);
    a.animate('k', 5, { onFrame: (x) => frames.push(x) });
    assert.deepEqual(frames, [5]);
    assert.equal(sched.waiting(), 0);
});

test('a throwing frame callback stops only its own spring', () => {
    const sched = manualScheduler();
    const a = spring.createAnimator(sched);
    a.set('bad', 0);
    a.set('good', 0);
    a.animate('bad', 1, { onFrame: () => { throw new Error('draw failed'); } });
    a.animate('good', 1, {});
    for (let i = 0; i < 200 && sched.waiting() > 0; i += 1) {
        sched.frame(16);
    }
    assert.equal(a.value('good'), 1);
    assert.equal(a.isMoving('bad'), false);
});
