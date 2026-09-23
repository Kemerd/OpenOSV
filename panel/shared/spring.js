/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * spring.js - spring physics for the panel's animations.
 *
 * WHY JAVASCRIPT AND NOT CSS
 * --------------------------
 * UXP is not a browser.  Its CSS reference lists no `transition`, no
 * `animation` and no `transform` (developer.adobe.com/premiere-pro/uxp,
 * "UXP is not a browser"; the supported-properties list under
 * uxp-api/reference-css/styles).  What it does support is setting `left`,
 * `width`, `height`, `opacity` and colours from script.  So every animation in
 * the panel is a damped spring integrated here and written to one of those
 * properties - which also gives the same motion in the CEP panel, where CSS
 * transitions would have worked but would have looked different.
 *
 * THE MODEL
 * ---------
 * A unit mass on a damped spring:  x'' = -k (x - target) - c x'.
 * Designers think in "response" (seconds per oscillation) and "damping
 * fraction" (1 = critically damped, < 1 = a little overshoot) - the
 * parameterisation SwiftUI's spring uses - so springConfig() converts:
 *
 *     k = (2 pi / response)^2          c = 4 pi * dampingFraction / response
 *
 * Integration is semi-implicit Euler in fixed sub-steps of at most 1/240 s,
 * which is stable for every stiffness used here and independent of how
 * irregular the host's frame timer is.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvSpring = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /** Largest integration sub-step, seconds. */
    var MAX_STEP = 1 / 240;

    /** Longest frame gap honoured; a stalled timer must not teleport a spring. */
    var MAX_FRAME = 1 / 15;

    /**
     * Stiffness and damping from a response time and damping fraction.
     * Garbage in gives a sensible default spring out, never NaN.
     */
    function springConfig(response, dampingFraction) {
        var r = (typeof response === 'number' && isFinite(response) && response > 0.01) ? response : 0.35;
        var z = (typeof dampingFraction === 'number' && isFinite(dampingFraction) && dampingFraction > 0)
            ? Math.min(dampingFraction, 2) : 0.82;
        var omega = (2 * Math.PI) / r;
        return { stiffness: omega * omega, damping: 4 * Math.PI * z / r };
    }

    /** The panel's two springs: a snappy one for controls, a softer one for text. */
    var PRESETS = Object.freeze({
        snappy: springConfig(0.32, 0.78),
        gentle: springConfig(0.45, 0.9)
    });

    /**
     * Advance a spring by `dt` seconds towards `target`.
     * Returns a NEW state {x, v}; the input is not modified.
     */
    function stepSpring(state, target, dt, config) {
        var x = (state && isFinite(state.x)) ? state.x : 0;
        var v = (state && isFinite(state.v)) ? state.v : 0;
        var goal = isFinite(target) ? target : x;
        var cfg = config || PRESETS.snappy;
        var k = isFinite(cfg.stiffness) ? cfg.stiffness : PRESETS.snappy.stiffness;
        var c = isFinite(cfg.damping) ? cfg.damping : PRESETS.snappy.damping;
        var remaining = Math.min(Math.max(isFinite(dt) ? dt : 0, 0), MAX_FRAME);
        while (remaining > 1e-9) {
            var h = Math.min(MAX_STEP, remaining);
            // Semi-implicit Euler: velocity first, then position with the new velocity.
            var accel = -k * (x - goal) - c * v;
            v += accel * h;
            x += v * h;
            remaining -= h;
        }
        return { x: x, v: v };
    }

    /**
     * True when a spring is close enough to rest that stopping it is
     * invisible.  `scale` is the size of the motion: a 40 px slide and a 0..1
     * colour mix need different absolute tolerances.
     */
    function isSettled(state, target, scale) {
        var s = (isFinite(scale) && scale > 0) ? scale : 1;
        return Math.abs(state.x - target) < 0.002 * s && Math.abs(state.v) < 0.02 * s;
    }

    /**
     * The frame scheduler: requestAnimationFrame when the runtime has it
     * (CEP), a 60 Hz timer when it does not (UXP documents no rAF).
     *
     * Chromium stops delivering animation frames to a page it considers
     * hidden - a CEP panel in a background tab - which would freeze a spring
     * half way (a status line left at 30 % opacity).  So every frame request
     * also arms a timer; whichever fires first draws the frame, the other is
     * ignored.
     */
    function defaultScheduler() {
        var raf = (typeof requestAnimationFrame === 'function') ? requestAnimationFrame : null;
        return {
            request: function (cb) {
                if (!raf) {
                    return setTimeout(cb, 16);
                }
                var fired = false;
                var once = function () {
                    if (!fired) {
                        fired = true;
                        cb();
                    }
                };
                raf(once);
                return setTimeout(once, 34);
            },
            now: function () {
                return (typeof performance !== 'undefined' && performance && typeof performance.now === 'function')
                    ? performance.now() : Date.now();
            }
        };
    }

    /**
     * An animator: any number of named springs, one frame loop.
     *
     *   animate(id, to, {from, config, scale, onFrame, onDone})
     *     Moves spring `id` towards `to`.  An `id` already moving keeps its
     *     position and velocity - retargeting mid-flight is what makes a
     *     double-tapped toggle feel physical instead of restarting.
     *   set(id, value)   jump without animating (initial layout, resize).
     *   value(id)        current position.
     *
     * The loop runs only while something moves.
     */
    function createAnimator(scheduler) {
        var sched = scheduler || defaultScheduler();
        var springs = Object.create(null);
        var running = false;
        var last = 0;

        function tick() {
            var t = sched.now();
            var dt = Math.min(Math.max((t - last) / 1000, 0), MAX_FRAME);
            last = t;
            var active = 0;
            var ids = Object.keys(springs);
            for (var i = 0; i < ids.length; i += 1) {
                var s = springs[ids[i]];
                if (!s.moving) {
                    continue;
                }
                var next = stepSpring(s.state, s.target, dt, s.config);
                s.state = next;
                if (isSettled(next, s.target, s.scale)) {
                    s.state = { x: s.target, v: 0 };
                    s.moving = false;
                }
                // A throwing frame callback must not stop every other spring.
                try {
                    if (typeof s.onFrame === 'function') {
                        s.onFrame(s.state.x);
                    }
                    if (!s.moving && typeof s.onDone === 'function') {
                        s.onDone();
                    }
                } catch (err) {
                    s.moving = false;
                }
                if (s.moving) {
                    active += 1;
                }
            }
            if (active > 0) {
                sched.request(tick);
            } else {
                running = false;
            }
        }

        function ensureRunning() {
            if (!running) {
                running = true;
                last = sched.now();
                sched.request(tick);
            }
        }

        return {
            animate: function (id, to, options) {
                var o = options || {};
                if (!isFinite(to)) {
                    return;
                }
                var s = springs[id];
                if (!s) {
                    var from = isFinite(o.from) ? o.from : to;
                    s = springs[id] = { state: { x: from, v: 0 }, target: to, moving: false };
                }
                s.target = to;
                s.config = o.config || PRESETS.snappy;
                s.scale = isFinite(o.scale) && o.scale > 0 ? o.scale : 1;
                s.onFrame = o.onFrame;
                s.onDone = o.onDone;
                if (isSettled(s.state, to, s.scale)) {
                    // Already there: draw once so the caller's styles are exact.
                    s.state = { x: to, v: 0 };
                    s.moving = false;
                    try {
                        if (typeof o.onFrame === 'function') {
                            o.onFrame(to);
                        }
                    } catch (err) {
                        // Drawing is best effort.
                    }
                    return;
                }
                s.moving = true;
                ensureRunning();
            },
            set: function (id, value) {
                if (!isFinite(value)) {
                    return;
                }
                var s = springs[id];
                if (!s) {
                    springs[id] = { state: { x: value, v: 0 }, target: value, moving: false };
                    return;
                }
                s.state = { x: value, v: 0 };
                s.target = value;
                s.moving = false;
            },
            value: function (id) {
                var s = springs[id];
                return s ? s.state.x : NaN;
            },
            isMoving: function (id) {
                var s = springs[id];
                return !!(s && s.moving);
            }
        };
    }

    return Object.freeze({
        springConfig: springConfig,
        PRESETS: PRESETS,
        stepSpring: stepSpring,
        isSettled: isSettled,
        createAnimator: createAnimator,
        defaultScheduler: defaultScheduler
    });
}));
