/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * view.js - the OpenOSV panel's interface, shared by the UXP and CEP panels.
 *
 * DESIGN (Apple Human Interface Guidelines, in spirit)
 * ----------------------------------------------------
 *   - One primary control.  The auto-apply switch sits alone in the first
 *     card, the way a Settings screen leads with its master toggle.
 *   - Grouped, inset cards with hairline separators ("inset grouped" lists),
 *     a small caps section title, generous 12-14 px rhythm.
 *   - Standard controls with their standard meanings: a switch for on/off,
 *     a segmented control for one-of-two, a slider for a continuous value,
 *     a filled button for the likely action and a tinted one beside it.
 *   - Motion that explains state: every change springs (spring.js) rather
 *     than jumps - the switch knob stretches while pressed and slides with a
 *     touch of overshoot, the segmented selection glides, new status text
 *     rises into place.
 *   - Colour follows Premiere's theme (the panels pass it in), system
 *     colours for meaning: green on, blue act, orange warn, red fail.
 *   - The layout is one column that stacks further on a narrow panel and
 *     puts the buttons side by side on a wide one (panel.css).
 *
 * The DOM is built here, not in the two index.html files, so both panels
 * render the identical interface from one description.  Only properties UXP
 * documents as supported are animated: left, width, opacity, top and colours.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvView = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /**
     * The colours that animate, per theme; the static ones live in panel.css.
     * [WP-EASING] tileOff / tileOn are a preset tile's fill unpicked and
     * picked, iconOff / iconOn its curve's stroke, chevron the disclosure
     * arrow of the controls card - all mixed by springs, so they are here
     * rather than in the stylesheet.
     */
    var PALETTES = {
        dark: {
            switchOff: '#48484a', switchOn: '#30d158',
            tileOff: '#323234', tileOn: '#17314f', iconOff: '#aeaeb2', iconOn: '#0a84ff', chevron: '#aeaeb2'
        },
        light: {
            switchOff: '#d1d1d6', switchOn: '#34c759',
            tileOff: '#f2f2f7', tileOn: '#e3efff', iconOff: '#6e6e73', iconOn: '#007aff', chevron: '#6e6e73'
        }
    };

    /** The SVG namespace, for the curve icons and the chevron. */
    var SVG_NS = 'http://www.w3.org/2000/svg';

    /** A preset tile's icon box, in px. */
    var ICON_W = 30;
    var ICON_H = 18;

    /** Switch geometry, in step with panel.css. */
    var SWITCH_SIZES = {
        regular: { width: 44, knob: 22, pad: 2, stretch: 6 },
        small: { width: 36, knob: 18, pad: 2, stretch: 5 }
    };

    /** Slider thumb diameter, in step with panel.css. */
    var THUMB = 20;

    /** Copy.  Short, confident, plain. */
    var COPY = {
        title: 'OpenOSV',
        subtitle: 'Open 360 Reframe on every OSV drop.',
        autoLabel: 'Auto-apply to OSV clips',
        autoCaption: 'Drop an .OSV or .LRF on the timeline. The reframe goes on by itself.',
        section: 'New effects start with',
        lensLabel: 'Lens',
        dragLabel: 'Drag sensitivity',
        dragCaptionOff: 'Effect default (2.0).',
        dragCaptionOn: 'How fast a Program Monitor drag turns the view.',
        applySelected: 'Apply to selected clips',
        applyAll: 'Apply to all OSV clips in this sequence',
        pillOn: 'Watching',
        pillOff: 'Off',
        // [WP-EASING] The Program Monitor controls card: one line per
        // gesture, each true to plugins/reframe/ReframeUi.cpp
        // (resolveDragMode, applyDrag) and the HUD it draws.
        hintTitle: 'Program Monitor controls',
        hintNote: 'Select Open 360 Reframe in Effect Controls to show the overlay.',
        hints: [
            { keys: ['Drag'], text: 'Pan and tilt. Left-right pans, up-down tilts.' },
            { keys: ['Shift', 'drag'], text: 'Lock to one axis.' },
            { keys: ['Ctrl', 'drag'], text: 'Zoom. Down widens. On the DJI lens, along DJI Studio\'s zoom path.' },
            { keys: ['Alt', 'drag'], text: 'Roll. So does dragging the ring.' },
            { keys: ['Corner'], text: 'Drag a corner grip up or down to zoom.' }
        ],
        // The Manual Framing card.
        framingSection: 'Manual Framing',
        framingZoom: 'Zoom',
        framingNarrower: 'Zoom in',
        framingWider: 'Zoom out',
        framingReasons: {
            'no-sequence': 'Open a sequence.',
            'no-selection': 'Select an OSV clip to frame it.',
            'many-selected': 'Select just one clip to frame it.',
            'not-osv': 'The selected clip isn\'t OSV.',
            'no-effect': 'No Open 360 Reframe on the selected clip.',
            'outside': 'Move the playhead over the selected clip.',
            'unknown': 'Reading the selected clip...'
        },
        // The Keyframe Animation card.
        easingSection: 'Keyframe Animation',
        easingCaption: 'How the view moves between keyframes.',
        easingSelected: 'Apply to selected clips',
        easingAll: 'Apply to all OSV clips in this sequence',
        easingUndoNote: 'This Premiere undoes it one clip at a time.',
        // The Stabilisation card: DJI Studio's two switches, then what Apply
        // writes ({entry} is the Source Settings entry the pair spells).
        stabilizationSection: 'Stabilisation',
        rockSteadyLabel: 'RockSteady',
        rockSteadyCaption: 'Irons out the shake. The view still turns with you.',
        horizonLabel: 'Horizon Leveling',
        horizonCaption: 'Keeps the horizon level, whatever the camera does.',
        stabilizationCaption: 'Apply sets Stabilisation to {entry} on each selected clip\'s master clip.',
        stabilizationApply: 'Apply to selected clips',
        stabilizationUnsupported: 'This Premiere can\'t reach Source Settings from a panel. Set it in the master clip\'s Source Settings.'
    };

    // ---- colour ---------------------------------------------------------------

    /** '#rrggbb' to [r, g, b]; black for anything unreadable. */
    function hexToRgb(hex) {
        var m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex));
        return m ? [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)] : [0, 0, 0];
    }

    /** Blend two '#rrggbb' colours; t is clamped, so overshoot never leaves the range. */
    function mixColor(a, b, t) {
        var p = Math.max(0, Math.min(1, isFinite(t) ? t : 0));
        var x = hexToRgb(a);
        var y = hexToRgb(b);
        var c = [0, 1, 2].map(function (i) { return Math.round(x[i] + (y[i] - x[i]) * p); });
        return 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
    }

    // ---- DOM helpers ----------------------------------------------------------

    function make(doc, tag, className, text) {
        var node = doc.createElement(tag);
        if (className) {
            node.className = className;
        }
        if (text !== undefined && text !== null) {
            node.textContent = String(text);
        }
        return node;
    }

    function px(value) {
        return (isFinite(value) ? Math.round(value * 100) / 100 : 0) + 'px';
    }

    /** Toggle a class without relying on classList.toggle's second argument. */
    function setClass(node, name, on) {
        if (!node || !node.classList) {
            return;
        }
        if (on) {
            node.classList.add(name);
        } else {
            node.classList.remove(name);
        }
    }

    /** Run a handler; a throwing handler must not break the panel. */
    function guard(fn) {
        return function (event) {
            try {
                fn(event);
            } catch (err) {
                if (typeof console !== 'undefined' && console && typeof console.error === 'function') {
                    console.error('OpenOSV panel:', err);
                }
            }
        };
    }

    /** Enter / Space activates, as for a native button or switch. */
    function isActivationKey(event) {
        var k = event && event.key;
        return k === 'Enter' || k === ' ' || k === 'Spacebar';
    }

    /**
     * Mount the interface into `rootEl`.
     *
     * @param {Element} rootEl
     * @param {object} deps  { controller, core, spring, scheduler? }
     * @returns {{ render: function(state), setTheme: function(name, background?), relayout: function }}
     */
    function mountView(rootEl, deps) {
        if (!rootEl || !deps || !deps.controller || !deps.core || !deps.spring) {
            throw new Error('OsvView.mount needs a root element, controller, core and spring');
        }
        var doc = rootEl.ownerDocument || document;
        var controller = deps.controller;
        var core = deps.core;
        var spring = deps.spring;
        var animator = spring.createAnimator(deps.scheduler);
        var palette = PALETTES.dark;
        var themeName = 'dark';

        // ---------------------------------------------------------------------
        //  Controls
        // ---------------------------------------------------------------------

        /** An iOS-style switch. */
        function makeSwitch(id, size, label, onToggle) {
            var geo = SWITCH_SIZES[size] || SWITCH_SIZES.regular;
            var node = make(doc, 'div', 'osv-switch' + (size === 'small' ? ' osv-switch-small' : ''));
            var knob = make(doc, 'div', 'osv-switch-knob');
            node.appendChild(knob);
            node.setAttribute('role', 'switch');
            node.setAttribute('tabindex', '0');
            node.setAttribute('aria-label', label);
            var on = false;
            var pressed = false;

            // Position from the two springs: progress p (0 off .. 1 on) and
            // the press stretch s (0 .. 1) that widens the knob.
            function draw() {
                var p = animator.value(id + '.p');
                var s = animator.value(id + '.s');
                p = isFinite(p) ? p : (on ? 1 : 0);
                s = isFinite(s) ? Math.max(0, s) : 0;
                var knobW = geo.knob + geo.stretch * s;
                var travel = geo.width - 2 * geo.pad - knobW;
                knob.style.width = px(knobW);
                knob.style.left = px(geo.pad + travel * p);
                node.style.backgroundColor = mixColor(palette.switchOff, palette.switchOn, p);
            }

            function set(value, animate) {
                on = value === true;
                node.setAttribute('aria-checked', on ? 'true' : 'false');
                if (animate) {
                    animator.animate(id + '.p', on ? 1 : 0, { config: spring.PRESETS.snappy, onFrame: draw });
                } else {
                    animator.set(id + '.p', on ? 1 : 0);
                    draw();
                }
            }

            function press(down) {
                if (pressed === down) {
                    return;
                }
                pressed = down;
                animator.animate(id + '.s', down ? 1 : 0, { config: spring.PRESETS.snappy, onFrame: draw });
            }

            animator.set(id + '.s', 0);
            node.addEventListener('pointerdown', guard(function () { press(true); }));
            node.addEventListener('pointerup', guard(function () { press(false); }));
            node.addEventListener('pointerleave', guard(function () { press(false); }));
            node.addEventListener('pointercancel', guard(function () { press(false); }));
            node.addEventListener('click', guard(function () { onToggle(!on); }));
            node.addEventListener('keydown', guard(function (e) {
                if (isActivationKey(e)) {
                    if (typeof e.preventDefault === 'function') {
                        e.preventDefault();
                    }
                    onToggle(!on);
                }
            }));
            return { node: node, set: set, redraw: draw };
        }

        /** A two-segment control with a sliding selection. */
        function makeSegmented(id, options, onPick) {
            var node = make(doc, 'div', 'osv-segmented');
            node.setAttribute('role', 'radiogroup');
            var pill = make(doc, 'div', 'osv-seg-pill');
            node.appendChild(pill);
            var segs = options.map(function (opt) {
                var seg = make(doc, 'div', 'osv-seg', opt.label);
                seg.setAttribute('role', 'radio');
                seg.setAttribute('tabindex', '0');
                // The whole label as a tooltip, for a segment a narrow panel
                // shortens with an ellipsis.
                seg.setAttribute('title', opt.label);
                seg.dataset.value = opt.value;
                seg.addEventListener('click', guard(function () { onPick(opt.value); }));
                seg.addEventListener('keydown', guard(function (e) {
                    if (isActivationKey(e)) {
                        if (typeof e.preventDefault === 'function') {
                            e.preventDefault();
                        }
                        onPick(opt.value);
                    }
                }));
                node.appendChild(seg);
                return seg;
            });
            var current = options.length > 0 ? options[0].value : '';
            var laidOut = false;

            function draw() {
                var l = animator.value(id + '.left');
                var w = animator.value(id + '.width');
                if (isFinite(l) && isFinite(w)) {
                    pill.style.left = px(l);
                    pill.style.width = px(Math.max(0, w));
                }
            }

            function place(animate) {
                var target = null;
                segs.forEach(function (seg) {
                    var selected = seg.dataset.value === current;
                    setClass(seg, 'is-selected', selected);
                    seg.setAttribute('aria-checked', selected ? 'true' : 'false');
                    if (selected) {
                        target = seg;
                    }
                });
                if (!target) {
                    return;
                }
                var left = Number(target.offsetLeft) || 0;
                var width = Number(target.offsetWidth) || 0;
                if (width <= 0) {
                    return;   // not laid out yet (hidden panel); relayout() retries
                }
                if (animate && laidOut) {
                    animator.animate(id + '.left', left, { config: spring.PRESETS.snappy, scale: 40, onFrame: draw });
                    animator.animate(id + '.width', width, { config: spring.PRESETS.snappy, scale: 40, onFrame: draw });
                } else {
                    animator.set(id + '.left', left);
                    animator.set(id + '.width', width);
                    draw();
                    laidOut = true;
                }
            }

            return {
                node: node,
                set: function (value, animate) {
                    var changed = value !== current;
                    current = value;
                    place(animate && changed);
                },
                relayout: function () { place(false); }
            };
        }

        /** A slider with a thumb that follows the pointer. */
        function makeSlider(id, min, max, step, onInput) {
            var node = make(doc, 'div', 'osv-slider');
            node.setAttribute('role', 'slider');
            node.setAttribute('tabindex', '0');
            node.setAttribute('aria-valuemin', String(min));
            node.setAttribute('aria-valuemax', String(max));
            var track = make(doc, 'div', 'osv-slider-track');
            var fill = make(doc, 'div', 'osv-slider-fill');
            track.appendChild(fill);
            var thumb = make(doc, 'div', 'osv-slider-thumb');
            node.appendChild(track);
            node.appendChild(thumb);
            var value = min;
            var enabled = true;
            var dragging = false;

            function width() {
                var w = 0;
                try {
                    w = node.getBoundingClientRect().width;
                } catch (err) {
                    w = 0;
                }
                return (isFinite(w) && w > 0) ? w : (Number(node.offsetWidth) || 0);
            }

            function snap(v) {
                var s = Math.round((v - min) / step) * step + min;
                return Math.round(Math.max(min, Math.min(max, s)) * 100) / 100;
            }

            function draw() {
                var f = animator.value(id + '.f');
                f = isFinite(f) ? Math.max(0, Math.min(1, f)) : 0;
                var travel = Math.max(0, width() - THUMB);
                thumb.style.left = px(travel * f);
                fill.style.width = px(travel * f + THUMB / 2);
            }

            function fraction(v) {
                return max > min ? (v - min) / (max - min) : 0;
            }

            function setValue(v, animate) {
                value = snap(v);
                node.setAttribute('aria-valuenow', String(value));
                if (animate) {
                    animator.animate(id + '.f', fraction(value), { config: spring.PRESETS.snappy, onFrame: draw });
                } else {
                    animator.set(id + '.f', fraction(value));
                    draw();
                }
            }

            function fromPointer(e) {
                var rect = null;
                try {
                    rect = node.getBoundingClientRect();
                } catch (err) {
                    rect = null;
                }
                if (!rect || !isFinite(rect.left) || !(rect.width > THUMB)) {
                    return value;
                }
                var f = (e.clientX - rect.left - THUMB / 2) / (rect.width - THUMB);
                return snap(min + Math.max(0, Math.min(1, f)) * (max - min));
            }

            node.addEventListener('pointerdown', guard(function (e) {
                if (!enabled) {
                    return;
                }
                dragging = true;
                try {
                    node.setPointerCapture(e.pointerId);
                } catch (err) {
                    // Capture is a nicety; the drag still works inside the slider.
                }
                var v = fromPointer(e);
                setValue(v, true);   // a click on the track glides there
                onInput(value, false);
            }));
            node.addEventListener('pointermove', guard(function (e) {
                if (!dragging || !enabled) {
                    return;
                }
                setValue(fromPointer(e), false);   // a drag follows the finger exactly
                onInput(value, false);
            }));
            var end = guard(function (e) {
                if (!dragging) {
                    return;
                }
                dragging = false;
                try {
                    node.releasePointerCapture(e.pointerId);
                } catch (err) {
                    // Already released.
                }
                onInput(value, true);
            });
            node.addEventListener('pointerup', end);
            node.addEventListener('pointercancel', end);
            node.addEventListener('keydown', guard(function (e) {
                if (!enabled) {
                    return;
                }
                var delta = 0;
                var big = e.shiftKey ? 5 : 1;
                if (e.key === 'ArrowRight' || e.key === 'ArrowUp') {
                    delta = step * big;
                } else if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') {
                    delta = -step * big;
                } else if (e.key === 'Home') {
                    delta = min - value;
                } else if (e.key === 'End') {
                    delta = max - value;
                }
                if (delta !== 0) {
                    if (typeof e.preventDefault === 'function') {
                        e.preventDefault();
                    }
                    setValue(value + delta, true);
                    onInput(value, true);
                }
            }));

            return {
                node: node,
                set: function (v, animate) {
                    if (!dragging) {
                        setValue(v, animate);
                    }
                },
                setEnabled: function (on) {
                    enabled = on === true;
                    node.setAttribute('aria-disabled', enabled ? 'false' : 'true');
                },
                relayout: draw
            };
        }

        /** A button that dims while pressed and springs back. */
        function makeButton(id, className, label, onPress) {
            var node = make(doc, 'div', 'osv-button ' + className, label);
            node.setAttribute('role', 'button');
            node.setAttribute('tabindex', '0');
            var enabled = true;
            function drawOpacity() {
                var o = animator.value(id + '.o');
                node.style.opacity = String(isFinite(o) ? Math.max(0, Math.min(1, o)) : 1);
            }
            function target(pressed) {
                if (!enabled) {
                    return 0.45;
                }
                return pressed ? 0.62 : 1;
            }
            animator.set(id + '.o', 1);
            node.addEventListener('pointerdown', guard(function () {
                animator.animate(id + '.o', target(true), { config: spring.PRESETS.snappy, onFrame: drawOpacity });
            }));
            var release = guard(function () {
                animator.animate(id + '.o', target(false), { config: spring.PRESETS.gentle, onFrame: drawOpacity });
            });
            node.addEventListener('pointerup', release);
            node.addEventListener('pointerleave', release);
            node.addEventListener('pointercancel', release);
            node.addEventListener('click', guard(function () {
                if (enabled) {
                    onPress();
                }
            }));
            node.addEventListener('keydown', guard(function (e) {
                if (enabled && isActivationKey(e)) {
                    if (typeof e.preventDefault === 'function') {
                        e.preventDefault();
                    }
                    onPress();
                }
            }));
            return {
                node: node,
                setEnabled: function (on) {
                    if (enabled === (on === true)) {
                        return;
                    }
                    enabled = on === true;
                    node.setAttribute('aria-disabled', enabled ? 'false' : 'true');
                    animator.animate(id + '.o', target(false), { config: spring.PRESETS.gentle, onFrame: drawOpacity });
                },
                setLabel: function (text) {
                    if (node.textContent !== text) {
                        node.textContent = text;
                    }
                }
            };
        }

        // ---------------------------------------------------------------------
        //  [WP-EASING] Vector drawing
        // ---------------------------------------------------------------------

        /**
         * An SVG element, or null when this engine cannot make one (the
         * callers then fall back to text).  Attributes are set one by one;
         * nothing here relies on CSS the smaller engine lacks.
         */
        function svg(tag, attrs) {
            if (typeof doc.createElementNS !== 'function') {
                return null;
            }
            var node = null;
            try {
                node = doc.createElementNS(SVG_NS, tag);
            } catch (err) {
                return null;
            }
            if (!node) {
                return null;
            }
            Object.keys(attrs || {}).forEach(function (k) {
                node.setAttribute(k, String(attrs[k]));
            });
            return node;
        }

        /** Points as an SVG path. */
        function pathOf(points) {
            return points.map(function (p, i) { return (i === 0 ? 'M' : 'L') + p[0] + ' ' + p[1]; }).join(' ');
        }

        /**
         * A preset's curve icon: its speed profile between two keyframe dots,
         * from OsvCore.easingIconPoints() - the same polynomials the effect
         * renders - or a crossed circle for None.  Returns {node, colour(c)}
         * so the tile can spring the stroke with its selection.
         */
        function makeEasingIcon(id) {
            var box = svg('svg', { width: ICON_W, height: ICON_H, viewBox: '0 0 ' + ICON_W + ' ' + ICON_H });
            if (!box) {
                // No SVG: a text glyph keeps the tile readable.
                var glyph = make(doc, 'div', 'osv-ease-glyph', id === 'none' ? '⊘' : '∼');
                return { node: glyph, colour: function (c) { glyph.style.color = c; } };
            }
            var strokes = [];
            var fills = [];
            var drawing = core.easingIconPoints(id, ICON_W, ICON_H);
            if (drawing.kind === 'none') {
                var r = ICON_H / 2 - 2;
                var cx = ICON_W / 2;
                var cy = ICON_H / 2;
                strokes.push(box.appendChild(svg('circle', { cx: cx, cy: cy, r: r, fill: 'none', 'stroke-width': 1.5 })));
                var d = r * 0.7071;
                strokes.push(box.appendChild(svg('path', {
                    d: 'M' + (cx - d) + ' ' + (cy - d) + ' L' + (cx + d) + ' ' + (cy + d),
                    fill: 'none', 'stroke-width': 1.5, 'stroke-linecap': 'round'
                })));
            } else {
                strokes.push(box.appendChild(svg('path', {
                    d: pathOf(drawing.points), fill: 'none', 'stroke-width': 1.5,
                    'stroke-linecap': 'round', 'stroke-linejoin': 'round'
                })));
                drawing.dots.forEach(function (p) {
                    fills.push(box.appendChild(svg('circle', { cx: p[0], cy: p[1], r: 1.8 })));
                });
            }
            return {
                node: box,
                colour: function (c) {
                    strokes.forEach(function (s) { if (s) { s.setAttribute('stroke', c); } });
                    fills.forEach(function (f) { if (f) { f.setAttribute('fill', c); } });
                }
            };
        }

        /**
         * A disclosure chevron that turns with a spring: its two strokes are
         * rotated in script (the smaller engine has no CSS transforms), from
         * pointing right (closed, p = 0) to pointing down (open, p = 1).
         */
        function makeChevron(id) {
            var size = 12;
            var box = svg('svg', { width: size, height: size, viewBox: '0 0 ' + size + ' ' + size });
            var fallback = null;
            var line = null;
            if (box) {
                line = box.appendChild(svg('path', {
                    d: '', fill: 'none', 'stroke-width': 1.6, 'stroke-linecap': 'round', 'stroke-linejoin': 'round'
                }));
            } else {
                fallback = make(doc, 'div', 'osv-chevron-glyph', '›');
            }
            // A right-pointing chevron around the box centre.
            var base = [[-1.8, -3.6], [1.8, 0], [-1.8, 3.6]];
            function draw() {
                var p = animator.value(id);
                p = isFinite(p) ? p : 0;
                if (fallback) {
                    fallback.textContent = p > 0.5 ? '˅' : '›';
                    fallback.style.color = palette.chevron;
                    return;
                }
                var a = Math.max(-0.2, Math.min(1.2, p)) * Math.PI / 2;
                var cos = Math.cos(a);
                var sin = Math.sin(a);
                var pts = base.map(function (q) {
                    return [Math.round((size / 2 + q[0] * cos - q[1] * sin) * 100) / 100,
                            Math.round((size / 2 + q[0] * sin + q[1] * cos) * 100) / 100];
                });
                line.setAttribute('d', pathOf(pts));
                line.setAttribute('stroke', palette.chevron);
            }
            return { node: box || fallback, draw: draw };
        }

        // ---------------------------------------------------------------------
        //  [WP-EASING] Composite controls
        // ---------------------------------------------------------------------

        /**
         * One tile of the Keyframe Animation grid: the curve over a two-line
         * label, a spring-mixed fill and stroke for the pick, a dip while
         * pressed.  A radio in the grid's radiogroup.
         */
        function makeEasingTile(easing, onPick) {
            var id = 'ease.' + easing.id;
            var node = make(doc, 'div', 'osv-ease-tile');
            node.setAttribute('role', 'radio');
            node.setAttribute('tabindex', '0');
            node.setAttribute('aria-label', easing.label);
            node.setAttribute('title', easing.label);
            node.dataset.value = easing.id;
            var icon = makeEasingIcon(easing.id);
            var iconWrap = make(doc, 'div', 'osv-ease-icon');
            iconWrap.appendChild(icon.node);
            node.appendChild(iconWrap);
            node.appendChild(make(doc, 'div', 'osv-ease-line', easing.lines[0]));
            node.appendChild(make(doc, 'div', 'osv-ease-line osv-ease-line-2', easing.lines[1] || ' '));
            var picked = false;

            function draw() {
                var p = animator.value(id + '.p');
                var o = animator.value(id + '.o');
                p = isFinite(p) ? p : (picked ? 1 : 0);
                node.style.backgroundColor = mixColor(palette.tileOff, palette.tileOn, p);
                node.style.opacity = String(isFinite(o) ? Math.max(0, Math.min(1, o)) : 1);
                icon.colour(mixColor(palette.iconOff, palette.iconOn, p));
            }

            animator.set(id + '.o', 1);
            node.addEventListener('pointerdown', guard(function () {
                animator.animate(id + '.o', 0.7, { config: spring.PRESETS.snappy, onFrame: draw });
            }));
            var release = guard(function () {
                animator.animate(id + '.o', 1, { config: spring.PRESETS.gentle, onFrame: draw });
            });
            node.addEventListener('pointerup', release);
            node.addEventListener('pointerleave', release);
            node.addEventListener('pointercancel', release);
            node.addEventListener('click', guard(function () { onPick(easing.id); }));
            node.addEventListener('keydown', guard(function (e) {
                if (isActivationKey(e)) {
                    if (typeof e.preventDefault === 'function') {
                        e.preventDefault();
                    }
                    onPick(easing.id);
                }
            }));
            return {
                node: node,
                set: function (on, animate) {
                    picked = on === true;
                    setClass(node, 'is-selected', picked);
                    node.setAttribute('aria-checked', picked ? 'true' : 'false');
                    if (animate) {
                        animator.animate(id + '.p', picked ? 1 : 0, { config: spring.PRESETS.snappy, onFrame: draw });
                    } else {
                        animator.set(id + '.p', picked ? 1 : 0);
                        draw();
                    }
                },
                redraw: draw
            };
        }

        /**
         * A card whose body folds away under its header, on a spring: the
         * body's height and opacity follow one progress value, the chevron
         * turns with it, and once open the height is released so the card
         * reflows with the panel.  `onToggle(open)` reports a click.
         */
        function makeDisclosureCard(id, title, onToggle) {
            var card = make(doc, 'div', 'osv-card osv-disclosure');
            var head = make(doc, 'div', 'osv-disclosure-head');
            head.setAttribute('role', 'button');
            head.setAttribute('tabindex', '0');
            var chevron = makeChevron(id + '.chev');
            var chevronWrap = make(doc, 'div', 'osv-chevron');
            chevronWrap.appendChild(chevron.node);
            head.appendChild(make(doc, 'div', 'osv-label', title));
            head.appendChild(chevronWrap);
            var body = make(doc, 'div', 'osv-disclosure-body');
            card.appendChild(head);
            card.appendChild(body);
            var open = true;
            var natural = 0;

            /** The body's full height, measured with the height released. */
            function measure() {
                var saved = body.style.height;
                body.style.height = '';
                var h = Number(body.scrollHeight) || Number(body.offsetHeight) || 0;
                body.style.height = saved;
                return h > 0 ? h : natural;
            }

            function draw() {
                var p = animator.value(id + '.p');
                p = isFinite(p) ? p : (open ? 1 : 0);
                var landed = Math.abs(p - (open ? 1 : 0)) < 1e-3;
                if (open && landed) {
                    body.style.height = '';       // reflow freely once open
                } else {
                    body.style.height = px(Math.max(0, natural * Math.max(0, Math.min(1, p))));
                }
                body.style.opacity = String(Math.max(0, Math.min(1, p)));
                chevron.draw();
            }

            function set(value, animate) {
                var next = value === true;
                if (animate) {
                    natural = measure();
                }
                open = next;
                head.setAttribute('aria-expanded', open ? 'true' : 'false');
                setClass(card, 'is-open', open);
                if (animate && natural > 0) {
                    animator.animate(id + '.p', open ? 1 : 0, { config: spring.PRESETS.gentle, onFrame: draw });
                    animator.animate(id + '.chev', open ? 1 : 0, { config: spring.PRESETS.snappy, onFrame: draw });
                } else {
                    animator.set(id + '.p', open ? 1 : 0);
                    animator.set(id + '.chev', open ? 1 : 0);
                    if (!open) {
                        body.style.height = px(0);
                        body.style.opacity = '0';
                        chevron.draw();
                    } else {
                        draw();
                    }
                }
            }

            head.addEventListener('click', guard(function () { onToggle(!open); }));
            head.addEventListener('keydown', guard(function (e) {
                if (isActivationKey(e)) {
                    if (typeof e.preventDefault === 'function') {
                        e.preventDefault();
                    }
                    onToggle(!open);
                }
            }));
            return { node: card, body: body, set: set, redraw: draw };
        }

        /** A key cap: the name of a key, or of a gesture, in a small rounded box. */
        function keyCap(text) {
            return make(doc, 'span', 'osv-key', text);
        }

        /** A read-out: a small caps label over a tabular number. */
        function makeReadout(label) {
            var node = make(doc, 'div', 'osv-readout');
            var name = make(doc, 'div', 'osv-readout-label', label);
            var value = make(doc, 'div', 'osv-readout-value', '—');
            node.appendChild(name);
            node.appendChild(value);
            return {
                node: node,
                setLabel: function (t) {
                    if (name.textContent !== t) {
                        name.textContent = t;
                    }
                },
                setValue: function (t) {
                    if (value.textContent !== t) {
                        value.textContent = t;
                    }
                }
            };
        }

        // ---------------------------------------------------------------------
        //  Layout
        // ---------------------------------------------------------------------

        var shell = make(doc, 'div', 'osv theme-dark');

        // Header: title, subtitle, and the watching / off pill.
        var header = make(doc, 'div', 'osv-header');
        var titles = make(doc, 'div', 'osv-titles');
        titles.appendChild(make(doc, 'div', 'osv-title', COPY.title));
        titles.appendChild(make(doc, 'div', 'osv-subtitle', COPY.subtitle));
        var pill = make(doc, 'div', 'osv-pill');
        var pillDot = make(doc, 'div', 'osv-pill-dot');
        var pillText = make(doc, 'div', 'osv-pill-text', COPY.pillOn);
        pill.appendChild(pillDot);
        pill.appendChild(pillText);
        header.appendChild(titles);
        header.appendChild(pill);
        shell.appendChild(header);

        // The primary card: the one switch that matters.
        var primary = make(doc, 'div', 'osv-card osv-card-primary');
        var autoRow = make(doc, 'div', 'osv-row');
        var autoText = make(doc, 'div', 'osv-row-text');
        autoText.appendChild(make(doc, 'div', 'osv-label', COPY.autoLabel));
        autoText.appendChild(make(doc, 'div', 'osv-caption', COPY.autoCaption));
        var autoSwitch = makeSwitch('auto', 'regular', COPY.autoLabel, function (on) { controller.setAutoApply(on); });
        autoRow.appendChild(autoText);
        autoRow.appendChild(autoSwitch.node);
        primary.appendChild(autoRow);
        shell.appendChild(primary);

        // [WP-EASING] The Program Monitor controls, right under the one
        // switch that matters: open for a new user, then as they left it.
        var hint = makeDisclosureCard('hint', COPY.hintTitle, function (open) { controller.setHintOpen(open); });
        COPY.hints.forEach(function (h) {
            var line = make(doc, 'div', 'osv-hint-line');
            var keys = make(doc, 'div', 'osv-hint-keys');
            h.keys.forEach(function (k, i) {
                if (i > 0) {
                    keys.appendChild(make(doc, 'span', 'osv-key-plus', '+'));
                }
                keys.appendChild(k === 'drag' ? make(doc, 'span', 'osv-key-word', 'drag') : keyCap(k));
            });
            line.appendChild(keys);
            line.appendChild(make(doc, 'div', 'osv-hint-text', h.text));
            hint.body.appendChild(line);
        });
        hint.body.appendChild(make(doc, 'div', 'osv-caption osv-hint-note', COPY.hintNote));
        shell.appendChild(hint.node);

        // The defaults card: lens and drag sensitivity for new effects.
        shell.appendChild(make(doc, 'div', 'osv-section-title', COPY.section));
        var defaults = make(doc, 'div', 'osv-card');

        var lensRow = make(doc, 'div', 'osv-row osv-row-compact');
        lensRow.appendChild(make(doc, 'div', 'osv-label', COPY.lensLabel));
        var lens = makeSegmented('lens', [
            { value: core.LENS.dji, label: 'DJI' },
            { value: core.LENS.classic, label: 'Classic' }
        ], function (value) { controller.setLens(value); });
        lensRow.appendChild(lens.node);
        defaults.appendChild(lensRow);
        defaults.appendChild(make(doc, 'div', 'osv-divider'));

        var dragRow = make(doc, 'div', 'osv-row');
        var dragText = make(doc, 'div', 'osv-row-text');
        dragText.appendChild(make(doc, 'div', 'osv-label', COPY.dragLabel));
        var dragCaption = make(doc, 'div', 'osv-caption', COPY.dragCaptionOff);
        dragText.appendChild(dragCaption);
        var dragSwitch = makeSwitch('drag', 'small', COPY.dragLabel, function (on) { controller.setDragEnabled(on); });
        dragRow.appendChild(dragText);
        dragRow.appendChild(dragSwitch.node);
        defaults.appendChild(dragRow);

        var sliderRow = make(doc, 'div', 'osv-slider-row');
        var valueLabel = make(doc, 'div', 'osv-slider-value', core.DRAG.defaultValue.toFixed(2));
        // The label follows the thumb live; the setting is stored on release.
        var slider = makeSlider('dragSlider', core.DRAG.sliderMin, core.DRAG.sliderMax, 0.05, function (value, commit) {
            valueLabel.textContent = value.toFixed(2);
            if (commit) {
                controller.setDragSensitivity(value);
            }
        });
        sliderRow.appendChild(slider.node);
        sliderRow.appendChild(valueLabel);
        defaults.appendChild(sliderRow);
        shell.appendChild(defaults);

        // The two actions.
        var actions = make(doc, 'div', 'osv-actions');
        var applySelected = makeButton('btnSelected', 'osv-button-primary', COPY.applySelected,
                                       function () { controller.applySelected(); });
        var applyAll = makeButton('btnAll', 'osv-button-secondary', COPY.applyAll,
                                  function () { controller.applyAll(); });
        actions.appendChild(applySelected.node);
        actions.appendChild(applyAll.node);
        shell.appendChild(actions);

        // ---- [WP-EASING] Manual Framing --------------------------------------
        shell.appendChild(make(doc, 'div', 'osv-section-title osv-section-spaced', COPY.framingSection));
        var framing = make(doc, 'div', 'osv-card osv-card-padded');
        var framingClip = make(doc, 'div', 'osv-caption osv-framing-clip', COPY.framingReasons['no-selection']);
        framing.appendChild(framingClip);
        var presetRow = make(doc, 'div', 'osv-chips');
        var presetButtons = core.FRAMING_PRESETS.map(function (p) {
            var b = makeButton('preset.' + p.id, 'osv-chip', p.label, function () { controller.framingPreset(p.id); });
            presetRow.appendChild(b.node);
            return b;
        });
        framing.appendChild(presetRow);
        // Zoom: a stepper around DJI Studio's read-out.
        var zoomRow = make(doc, 'div', 'osv-zoom-row');
        zoomRow.appendChild(make(doc, 'div', 'osv-label', COPY.framingZoom));
        var zoomStepper = make(doc, 'div', 'osv-stepper');
        var zoomMinus = makeButton('zoomMinus', 'osv-step', '−', function () { controller.zoomStep(-1); });
        zoomMinus.node.setAttribute('aria-label', COPY.framingNarrower);
        var zoomValue = make(doc, 'div', 'osv-zoom-value', '—');
        var zoomPlus = makeButton('zoomPlus', 'osv-step', '+', function () { controller.zoomStep(1); });
        zoomPlus.node.setAttribute('aria-label', COPY.framingWider);
        zoomStepper.appendChild(zoomMinus.node);
        zoomStepper.appendChild(zoomValue);
        zoomStepper.appendChild(zoomPlus.node);
        zoomRow.appendChild(zoomStepper);
        framing.appendChild(zoomRow);
        // Read-outs.
        var readoutRow = make(doc, 'div', 'osv-readouts');
        var readouts = {
            fov: makeReadout('FOV'),
            second: makeReadout('Correction'),
            pan: makeReadout('Pan'),
            tilt: makeReadout('Tilt'),
            roll: makeReadout('Roll')
        };
        ['fov', 'second', 'pan', 'tilt', 'roll'].forEach(function (k) { readoutRow.appendChild(readouts[k].node); });
        framing.appendChild(readoutRow);
        shell.appendChild(framing);

        // ---- [WP-EASING] Keyframe Animation ----------------------------------
        shell.appendChild(make(doc, 'div', 'osv-section-title', COPY.easingSection));
        var easingCard = make(doc, 'div', 'osv-card osv-card-padded');
        easingCard.appendChild(make(doc, 'div', 'osv-caption osv-card-caption', COPY.easingCaption));
        var easingGrid = make(doc, 'div', 'osv-ease-grid');
        easingGrid.setAttribute('role', 'radiogroup');
        easingGrid.setAttribute('aria-label', COPY.easingSection);
        var easingTiles = core.EASINGS.map(function (e) {
            var tile = makeEasingTile(e, function (id) { controller.setEasing(id); });
            easingGrid.appendChild(tile.node);
            return { id: e.id, tile: tile };
        });
        easingCard.appendChild(easingGrid);
        var easingActions = make(doc, 'div', 'osv-actions osv-card-actions');
        var easingSelected = makeButton('btnEaseSelected', 'osv-button-primary', COPY.easingSelected,
                                        function () { controller.applyEasingSelected(); });
        var easingAll = makeButton('btnEaseAll', 'osv-button-secondary', COPY.easingAll,
                                   function () { controller.applyEasingAll(); });
        easingActions.appendChild(easingSelected.node);
        easingActions.appendChild(easingAll.node);
        easingCard.appendChild(easingActions);
        var easingNote = make(doc, 'div', 'osv-caption osv-card-note', '');
        easingCard.appendChild(easingNote);
        shell.appendChild(easingCard);

        // ---- [WP-EASING] Stabilisation ---------------------------------------
        // DJI Studio's two independent switches, RockSteady and Horizon
        // Leveling, each on its own row like the defaults card's switches.
        // Together they spell one Source Settings entry, which the caption
        // under them names, so Apply never writes a surprise.
        shell.appendChild(make(doc, 'div', 'osv-section-title', COPY.stabilizationSection));
        var stabCard = make(doc, 'div', 'osv-card osv-card-padded');

        /**
         * One switch row of the card: the label, what it does, the switch.
         * `top` marks the first row, which starts flush with the card's padding.
         */
        function makeStabRow(id, label, caption, top, onToggle) {
            var row = make(doc, 'div', 'osv-row osv-stab-row' + (top ? ' osv-stab-row-top' : ''));
            var text = make(doc, 'div', 'osv-row-text');
            text.appendChild(make(doc, 'div', 'osv-label', label));
            text.appendChild(make(doc, 'div', 'osv-caption', caption));
            var sw = makeSwitch(id, 'small', label, onToggle);
            row.appendChild(text);
            row.appendChild(sw.node);
            stabCard.appendChild(row);
            return sw;
        }
        var rockSteadySwitch = makeStabRow('rockSteady', COPY.rockSteadyLabel, COPY.rockSteadyCaption, true,
                                           function (on) { controller.setRockSteady(on); });
        stabCard.appendChild(make(doc, 'div', 'osv-divider'));
        var horizonSwitch = makeStabRow('horizonLeveling', COPY.horizonLabel, COPY.horizonCaption, false,
                                        function (on) { controller.setHorizonLeveling(on); });
        // What Apply writes; filled in by render().
        var stabCaption = make(doc, 'div', 'osv-caption osv-card-caption osv-stab-caption', '');
        stabCard.appendChild(stabCaption);
        var stabActions = make(doc, 'div', 'osv-actions osv-card-actions');
        var stabApply = makeButton('btnStab', 'osv-button-secondary', COPY.stabilizationApply,
                                   function () { controller.applyStabilization(); });
        stabActions.appendChild(stabApply.node);
        stabCard.appendChild(stabActions);
        shell.appendChild(stabCard);

        // The status line.
        var status = make(doc, 'div', 'osv-status');
        var statusDot = make(doc, 'div', 'osv-status-dot');
        var statusText = make(doc, 'div', 'osv-status-text', '');
        var statusTime = make(doc, 'div', 'osv-status-time', '');
        status.appendChild(statusDot);
        status.appendChild(statusText);
        status.appendChild(statusTime);
        shell.appendChild(status);

        // Replace whatever the page held (the "Loading" placeholder).
        while (rootEl.firstChild) {
            rootEl.removeChild(rootEl.firstChild);
        }
        rootEl.appendChild(shell);

        // ---------------------------------------------------------------------
        //  Rendering state
        // ---------------------------------------------------------------------

        var rendered = null;      // the last state drawn
        var statusKey = '';

        function drawStatusIn() {
            var v = animator.value('status.in');
            v = isFinite(v) ? v : 1;
            status.style.opacity = String(Math.max(0, Math.min(1, v)));
            status.style.top = px((1 - v) * 6);
        }

        function drawSlider() {
            var e = animator.value('dragSlider.enabled');
            sliderRow.style.opacity = String(isFinite(e) ? Math.max(0.3, Math.min(1, e)) : 1);
        }

        function drawPill() {
            var v = animator.value('pill.pop');
            pill.style.opacity = String(isFinite(v) ? Math.max(0, Math.min(1, v)) : 1);
        }

        /**
         * [WP-EASING] The Manual Framing card from the controller's read-outs:
         * the clip line, the Zoom read-out (its number glides to a new value
         * on a spring) and the five read-outs, dimmed when there is no clip.
         */
        var zoomShown = NaN;
        function drawZoom() {
            var v = animator.value('zoom.v');
            zoomValue.textContent = (isFinite(zoomShown) && isFinite(v)) ? degrees(v) : '—';
        }
        /** An angle read-out, or an em dash when there is no number. */
        function degrees(v) {
            return (typeof v === 'number' && isFinite(v)) ? core.formatReadout(v, 1) + '°' : '—';
        }
        function renderFraming(f, usable, animate) {
            var ok = f.ok === true;
            var clipText = ok ? (f.name || 'Selected clip') : (COPY.framingReasons[f.reason] || COPY.framingReasons.unknown);
            if (framingClip.textContent !== clipText) {
                framingClip.textContent = clipText;
            }
            setClass(framing, 'is-idle', !ok);
            var canAct = usable && ok;
            presetButtons.forEach(function (b) { b.setEnabled(canAct); });
            zoomMinus.setEnabled(canAct);
            zoomPlus.setEnabled(canAct);
            // Zoom: DJI Studio's read-out on the DJI lens, the FOV on Classic.
            var zoom = ok && typeof f.zoom === 'number' && isFinite(f.zoom) ? f.zoom : NaN;
            if (!isFinite(zoom)) {
                zoomShown = NaN;
                drawZoom();
            } else if (zoom !== zoomShown) {
                var glide = animate && isFinite(zoomShown);
                zoomShown = zoom;
                if (glide) {
                    animator.animate('zoom.v', zoom, { config: spring.PRESETS.gentle, scale: 60, onFrame: drawZoom });
                } else {
                    animator.set('zoom.v', zoom);
                    drawZoom();
                }
            }
            var classic = f.lens === core.LENS.classic;
            readouts.second.setLabel(classic ? 'Distortion' : 'Correction');
            var dash = '—';
            readouts.fov.setValue(ok ? degrees(f.fov) : dash);
            if (classic) {
                readouts.second.setValue(ok && isFinite(f.distortion) ? core.formatReadout(f.distortion, 1) + '%' : dash);
            } else {
                readouts.second.setValue(ok ? core.formatReadout(f.correction, 2) : dash);
            }
            readouts.pan.setValue(ok ? degrees(f.pan) : dash);
            readouts.tilt.setValue(ok ? degrees(f.tilt) : dash);
            readouts.roll.setValue(ok ? degrees(f.roll) : dash);
        }

        function render(state) {
            if (!state || !state.settings) {
                return;
            }
            var s = state.settings;
            var first = rendered === null;
            var animate = !first;

            // Switches, lens, slider.
            if (first || rendered.settings.autoApply !== s.autoApply) {
                autoSwitch.set(s.autoApply, animate);
                pillText.textContent = s.autoApply ? COPY.pillOn : COPY.pillOff;
                pill.setAttribute('data-state', s.autoApply ? 'on' : 'off');
                if (animate) {
                    animator.set('pill.pop', 0.35);
                    drawPill();
                    animator.animate('pill.pop', 1, { config: spring.PRESETS.gentle, onFrame: drawPill });
                }
            }
            if (first || rendered.settings.lens !== s.lens) {
                lens.set(s.lens, animate);
            }
            if (first || rendered.settings.dragEnabled !== s.dragEnabled) {
                dragSwitch.set(s.dragEnabled, animate);
                dragCaption.textContent = s.dragEnabled ? COPY.dragCaptionOn : COPY.dragCaptionOff;
                slider.setEnabled(s.dragEnabled);
                var target = s.dragEnabled ? 1 : 0.35;
                if (animate) {
                    animator.animate('dragSlider.enabled', target, { config: spring.PRESETS.gentle, onFrame: drawSlider });
                } else {
                    animator.set('dragSlider.enabled', target);
                    drawSlider();
                }
            }
            if (first || rendered.settings.dragSensitivity !== s.dragSensitivity) {
                slider.set(s.dragSensitivity, animate);
                valueLabel.textContent = Number(s.dragSensitivity).toFixed(2);
            }

            // Buttons: disabled while an action runs or when there is no host.
            // Only the running action's own button shows the progress label
            // (busyAction; an older controller sent none, and then the apply
            // buttons are the running ones).
            var usable = !state.busy && state.host !== 'error';
            var running = state.busy ? (state.busyAction || 'apply') : '';
            var busyText = state.busyLabel || '';
            applySelected.setEnabled(usable);
            applyAll.setEnabled(usable);
            applySelected.setLabel(running === 'apply' && busyText.indexOf('selection') !== -1
                ? busyText : COPY.applySelected);
            applyAll.setLabel(running === 'apply' && busyText.indexOf('sequence') !== -1
                ? busyText : COPY.applyAll);

            // [WP-EASING] The Program Monitor controls card.
            if (first || rendered.settings.hintOpen !== s.hintOpen) {
                hint.set(s.hintOpen !== false, animate);
            }

            // [WP-EASING] The Keyframe Animation grid and its buttons.
            if (first || rendered.settings.easing !== s.easing) {
                easingTiles.forEach(function (t) { t.tile.set(t.id === s.easing, animate); });
            }
            easingSelected.setEnabled(usable);
            easingAll.setEnabled(usable);
            easingSelected.setLabel(running === 'easing' && busyText.indexOf('selection') !== -1
                ? busyText : COPY.easingSelected);
            easingAll.setLabel(running === 'easing' && busyText.indexOf('sequence') !== -1
                ? busyText : COPY.easingAll);
            var caps = state.capabilities || {};
            var undoNote = caps.undoGroups === false ? COPY.easingUndoNote : '';
            if (easingNote.textContent !== undoNote) {
                easingNote.textContent = undoNote;
            }
            setClass(easingNote, 'is-empty', undoNote === '');

            // [WP-EASING] Stabilisation: each switch springs on its own, and
            // the caption names the entry the pair now spells.
            if (first || rendered.settings.rockSteady !== s.rockSteady) {
                rockSteadySwitch.set(s.rockSteady, animate);
            }
            if (first || rendered.settings.horizonLeveling !== s.horizonLeveling) {
                horizonSwitch.set(s.horizonLeveling, animate);
            }
            var reachable = caps.stabilization !== false;
            stabApply.setEnabled(usable && reachable);
            stabApply.setLabel(running === 'stabilization' ? busyText : COPY.stabilizationApply);
            var stabChoice = core.stabilizationChoice(s.rockSteady, s.horizonLeveling);
            var stabText = reachable ? COPY.stabilizationCaption.replace('{entry}', stabChoice.name)
                                     : COPY.stabilizationUnsupported;
            if (stabCaption.textContent !== stabText) {
                stabCaption.textContent = stabText;
            }

            // [WP-EASING] Manual Framing: the clip, the zoom and the read-outs.
            renderFraming(state.framing || {}, usable, animate);

            // Status line: new text rises into place.
            var st = state.status || { tone: 'info', text: '' };
            var key = st.tone + '|' + st.text + '|' + st.at;
            if (key !== statusKey) {
                statusKey = key;
                status.setAttribute('data-tone', st.tone || 'info');
                statusText.textContent = st.text || '';
                statusTime.textContent = st.stamped ? core.clockLabel(new Date(st.at)) : '';
                if (animate) {
                    // Hide the new text before the first frame, so it never
                    // flashes in at full strength and then drops.
                    animator.set('status.in', 0);
                    drawStatusIn();
                    animator.animate('status.in', 1, { config: spring.PRESETS.gentle, onFrame: drawStatusIn });
                } else {
                    animator.set('status.in', 1);
                    drawStatusIn();
                }
            }
            rendered = state;
        }

        /** Re-measure the controls that depend on layout (resize, first show). */
        function relayout() {
            lens.relayout();
            slider.relayout();
        }

        /** 'dark' or 'light'; `background` (a CSS colour) matches the host panel. */
        function setTheme(name, background) {
            themeName = name === 'light' ? 'light' : 'dark';
            palette = PALETTES[themeName];
            setClass(shell, 'theme-dark', themeName === 'dark');
            setClass(shell, 'theme-light', themeName === 'light');
            if (typeof background === 'string' && background.length > 0) {
                try {
                    doc.body.style.backgroundColor = background;
                } catch (err) {
                    // The stylesheet's background stays.
                }
            }
            autoSwitch.redraw();
            dragSwitch.redraw();
            // [WP-EASING] The spring-mixed colours of the new controls.
            rockSteadySwitch.redraw();
            horizonSwitch.redraw();
            easingTiles.forEach(function (t) { t.tile.redraw(); });
            hint.redraw();
        }

        // Keep the measured controls right as the panel is resized or docked.
        if (typeof ResizeObserver === 'function') {
            try {
                new ResizeObserver(guard(function () { relayout(); })).observe(shell);
            } catch (err) {
                // Without it, the first render's layout stays.
            }
        }
        if (typeof window !== 'undefined' && window && typeof window.addEventListener === 'function') {
            window.addEventListener('resize', guard(function () { relayout(); }));
        }

        render(controller.getState());
        // The first layout pass happens after the browser has laid the
        // shell out; measure again on the next tick.
        setTimeout(guard(relayout), 0);

        return { render: render, setTheme: setTheme, relayout: relayout, theme: function () { return themeName; } };
    }

    return Object.freeze({ mount: mountView, mixColor: mixColor, COPY: COPY, PALETTES: PALETTES });
}));
