// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// view.test.js - the shared interface, mounted into a small fake DOM.
//
// Not a pixel test: it proves view.js builds the controls, wires every one to
// the controller, reflects state (busy, tone, time stamp, theme) and that the
// springs land exactly where the layout says - without a browser.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const core = require('../shared/osvcore.js');
const spring = require('../shared/spring.js');
const View = require('../shared/view.js');

// ---------------------------------------------------------------------------
//  A fake DOM: just what view.js touches
// ---------------------------------------------------------------------------
class FakeElement {
    constructor(tag, doc) {
        this.tagName = String(tag).toUpperCase();
        this.ownerDocument = doc;
        this.children = [];
        this.parentNode = null;
        this.className = '';
        this.textContent = '';
        this.style = {};
        this.attributes = {};
        this.dataset = {};
        this.listeners = {};
        const self = this;
        this.classList = {
            add(n) { const s = new Set(self.className.split(/\s+/).filter(Boolean)); s.add(n); self.className = [...s].join(' '); },
            remove(n) { self.className = self.className.split(/\s+/).filter((c) => c && c !== n).join(' '); },
            contains(n) { return self.className.split(/\s+/).indexOf(n) !== -1; }
        };
    }
    get firstChild() { return this.children[0] || null; }
    appendChild(c) { c.parentNode = this; this.children.push(c); return c; }
    removeChild(c) { this.children = this.children.filter((x) => x !== c); c.parentNode = null; return c; }
    setAttribute(k, v) { this.attributes[k] = String(v); }
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attributes, k) ? this.attributes[k] : null; }
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
    fire(type, extra) {
        const ev = Object.assign({ type: type, pointerId: 1, clientX: 0, key: '', shiftKey: false, preventDefault() {} }, extra || {});
        (this.listeners[type] || []).forEach((fn) => fn(ev));
    }
    // Layout: every segment is 64 px wide inside a 2 px padding; sliders are 200 px.
    get offsetWidth() { return this.classList.contains('osv-seg') ? 64 : 200; }
    get offsetLeft() {
        if (!this.parentNode) {
            return 0;
        }
        const segs = this.parentNode.children.filter((c) => c.classList.contains('osv-seg'));
        return 2 + 64 * Math.max(0, segs.indexOf(this));
    }
    getBoundingClientRect() { return { left: 0, top: 0, width: this.offsetWidth, height: 22 }; }
    // [WP-EASING] A folding card measures its body; 120 px of hints.
    get scrollHeight() { return this.classList.contains('osv-disclosure-body') ? 120 : 0; }
    setPointerCapture() {}
    releasePointerCapture() {}
    /** Depth-first search by class. */
    find(cls) {
        if (this.classList.contains(cls)) {
            return this;
        }
        for (const c of this.children) {
            const hit = c.find(cls);
            if (hit) {
                return hit;
            }
        }
        return null;
    }
    findAll(cls, out) {
        const list = out || [];
        if (this.classList.contains(cls)) {
            list.push(this);
        }
        this.children.forEach((c) => c.findAll(cls, list));
        return list;
    }
}

function fakeDocument(options) {
    const doc = { createElement: (tag) => new FakeElement(tag, doc) };
    // [WP-EASING] SVG for the curve icons and the chevron, unless a test
    // wants the text fallback.
    if (!(options && options.noSvg)) {
        doc.createElementNS = (ns, tag) => {
            const el = new FakeElement(tag, doc);
            el.namespaceURI = ns;
            return el;
        };
    }
    doc.body = new FakeElement('body', doc);
    return doc;
}

/** A frame scheduler the test steps by hand. */
function manualScheduler() {
    let now = 0;
    let queue = [];
    return {
        request(cb) { queue.push(cb); },
        now() { return now; },
        runAll() {
            for (let i = 0; i < 500 && queue.length > 0; i += 1) {
                now += 16;
                const q = queue;
                queue = [];
                q.forEach((cb) => cb());
            }
        }
    };
}

/** A controller stand-in that records calls and hands out a state. */
function stubController(state) {
    const calls = [];
    return {
        calls: calls,
        state: state,
        getState() { return this.state; },
        setAutoApply(v) { calls.push(['setAutoApply', v]); },
        setLens(v) { calls.push(['setLens', v]); },
        setDragEnabled(v) { calls.push(['setDragEnabled', v]); },
        setDragSensitivity(v) { calls.push(['setDragSensitivity', v]); },
        applySelected() { calls.push(['applySelected']); },
        applyAll() { calls.push(['applyAll']); },
        // [WP-EASING]
        setHintOpen(v) { calls.push(['setHintOpen', v]); },
        setEasing(v) { calls.push(['setEasing', v]); },
        applyEasingSelected() { calls.push(['applyEasingSelected']); },
        applyEasingAll() { calls.push(['applyEasingAll']); },
        setRockSteady(v) { calls.push(['setRockSteady', v]); },
        setHorizonLeveling(v) { calls.push(['setHorizonLeveling', v]); },
        applyStabilization() { calls.push(['applyStabilization']); },
        framingPreset(v) { calls.push(['framingPreset', v]); },
        zoomStep(v) { calls.push(['zoomStep', v]); }
    };
}

function baseState(overrides) {
    return Object.assign({
        settings: core.sanitizeSettings(null),
        status: { tone: 'info', text: 'Watching the timeline.', at: 0, stamped: false },
        busy: false,
        busyLabel: '',
        host: 'ready'
    }, overrides || {});
}

function mount(state, options) {
    const doc = fakeDocument(options);
    const root = doc.createElement('div');
    root.appendChild(doc.createElement('div'));      // the "Loading" placeholder
    const ctl = stubController(state || baseState());
    const sched = manualScheduler();
    const view = View.mount(root, { controller: ctl, core, spring, scheduler: sched });
    return { doc, root, ctl, sched, view, shell: root.children[0] };
}

test('mount replaces the placeholder with the panel and renders the first state', () => {
    const { root, shell } = mount();
    assert.equal(root.children.length, 1);
    assert.ok(shell.classList.contains('osv'));
    assert.ok(shell.classList.contains('theme-dark'));
    assert.equal(shell.find('osv-title').textContent, 'OpenOSV');
    assert.equal(shell.find('osv-status-text').textContent, 'Watching the timeline.');
    assert.equal(shell.find('osv-pill-text').textContent, 'Watching');
    const buttons = shell.findAll('osv-button');
    // The auto-apply buttons come first; [WP-EASING] then the Manual Framing
    // presets and zoom stepper, the Keyframe Animation pair and the
    // Stabilisation apply.
    assert.deepEqual(buttons.map((b) => b.textContent), [
        'Apply to selected clips', 'Apply to all OSV clips in this sequence',
        'Crystal Ball', 'Asteroid', 'Wide', 'Ultra Wide', 'Dewarp', '−', '+',
        'Apply to selected clips', 'Apply to all OSV clips in this sequence',
        'Apply to selected clips'
    ]);
});

test('the switch starts on, drawn without animation, and springs to off', () => {
    const t = mount();
    const sw = t.shell.find('osv-switch');
    const knob = sw.find('osv-switch-knob');
    assert.equal(knob.style.left, '20px', 'on: 44 - 2 - 22');
    assert.equal(sw.attributes['aria-checked'], 'true');
    sw.fire('click');
    assert.deepEqual(t.ctl.calls, [['setAutoApply', false]]);
    t.ctl.state = baseState({ settings: core.sanitizeSettings({ autoApply: false }) });
    t.view.render(t.ctl.state);
    t.sched.runAll();
    assert.equal(knob.style.left, '2px');
    assert.equal(sw.style.backgroundColor, 'rgb(72,72,74)', 'the off grey of the dark theme');
    assert.equal(t.shell.find('osv-pill-text').textContent, 'Off');
});

test('pressing the switch stretches the knob and releasing restores it', () => {
    const t = mount();
    const sw = t.shell.find('osv-switch');
    const knob = sw.find('osv-switch-knob');
    sw.fire('pointerdown');
    t.sched.runAll();
    assert.equal(knob.style.width, '28px', '22 + 6');
    sw.fire('pointerup');
    t.sched.runAll();
    assert.equal(knob.style.width, '22px');
});

test('the lens control picks a lens and the selection glides to it', () => {
    const t = mount();
    const segs = t.shell.findAll('osv-seg');
    const pill = t.shell.find('osv-seg-pill');
    t.view.relayout();
    assert.equal(pill.style.left, '2px');
    assert.equal(pill.style.width, '64px');
    segs[1].fire('click');
    assert.deepEqual(t.ctl.calls, [['setLens', 'classic']]);
    t.view.render(baseState({ settings: core.sanitizeSettings({ lens: 'classic' }) }));
    t.sched.runAll();
    assert.equal(pill.style.left, '66px');
    assert.ok(segs[1].classList.contains('is-selected'));
    assert.equal(segs[0].classList.contains('is-selected'), false);
    segs[0].fire('keydown', { key: 'Enter' });
    assert.deepEqual(t.ctl.calls[1], ['setLens', 'dji']);
});

test('drag sensitivity: the switch enables the slider; a drag sets a snapped value on release', () => {
    const t = mount();
    const switches = t.shell.findAll('osv-switch');
    const slider = t.shell.find('osv-slider');
    assert.equal(slider.attributes['aria-disabled'], 'true', 'off by default');
    slider.fire('pointerdown', { clientX: 100 });
    slider.fire('pointerup', { clientX: 100 });
    assert.deepEqual(t.ctl.calls, [], 'a disabled slider ignores the pointer');
    switches[1].fire('keydown', { key: ' ' });
    assert.deepEqual(t.ctl.calls, [['setDragEnabled', true]]);
    t.view.render(baseState({ settings: core.sanitizeSettings({ dragEnabled: true }) }));
    assert.equal(slider.attributes['aria-disabled'], 'false');
    slider.fire('pointerdown', { clientX: 110 });   // (110 - 10) / 180 of 0.25..5
    slider.fire('pointermove', { clientX: 190 });   // the right end
    slider.fire('pointerup', { clientX: 190 });
    const last = t.ctl.calls[t.ctl.calls.length - 1];
    assert.equal(last[0], 'setDragSensitivity');
    assert.equal(last[1], 5);
    assert.equal(t.shell.find('osv-slider-value').textContent, '5.00');
    slider.fire('keydown', { key: 'ArrowLeft' });
    assert.deepEqual(t.ctl.calls[t.ctl.calls.length - 1], ['setDragSensitivity', 4.95]);
});

test('the buttons call the controller, and are inert while busy', () => {
    const t = mount();
    const [sel, all] = t.shell.findAll('osv-button');
    sel.fire('click');
    all.fire('keydown', { key: 'Enter' });
    assert.deepEqual(t.ctl.calls, [['applySelected'], ['applyAll']]);
    t.view.render(baseState({ busy: true, busyLabel: 'Applying to selection...' }));
    assert.equal(sel.textContent, 'Applying to selection...');
    assert.equal(all.textContent, 'Apply to all OSV clips in this sequence');
    sel.fire('click');
    assert.equal(t.ctl.calls.length, 2);
    t.view.render(baseState({ host: 'error' }));
    all.fire('click');
    assert.equal(t.ctl.calls.length, 2, 'no host, no action');
});

test('the status line shows tone, text and a time stamp, and rises in', () => {
    const t = mount();
    const status = t.shell.find('osv-status');
    const at = new Date(2026, 8, 23, 14, 2).getTime();
    t.view.render(baseState({ status: { tone: 'ok', text: 'Applied to 3 clips.', at: at, stamped: true } }));
    assert.equal(status.attributes['data-tone'], 'ok');
    assert.equal(t.shell.find('osv-status-text').textContent, 'Applied to 3 clips.');
    assert.equal(t.shell.find('osv-status-time').textContent, '14:02');
    assert.equal(status.style.opacity, '0', 'starts hidden...');
    t.sched.runAll();
    assert.equal(status.style.opacity, '1', '...and springs in');
    assert.equal(status.style.top, '0px');
});

test('the theme follows Premiere, background included', () => {
    const t = mount();
    t.view.setTheme('light', 'rgb(232,232,232)');
    assert.ok(t.shell.classList.contains('theme-light'));
    assert.equal(t.shell.classList.contains('theme-dark'), false);
    assert.equal(t.doc.body.style.backgroundColor, 'rgb(232,232,232)');
    assert.equal(t.shell.find('osv-switch').style.backgroundColor, 'rgb(52,199,89)', 'the light theme green');
    t.view.setTheme('anything else');
    assert.ok(t.shell.classList.contains('theme-dark'));
});

test('mount refuses to start without its dependencies', () => {
    assert.throws(() => View.mount(null, {}), /needs a root element/);
    assert.equal(View.mixColor('#000000', '#ffffff', 2), 'rgb(255,255,255)', 'overshoot is clamped');
    assert.equal(View.mixColor('junk', '#ffffff', 0), 'rgb(0,0,0)');
});

// ===========================================================================
//  [WP-EASING] The new cards
// ===========================================================================

/** A state with the framing read-outs of a selected clip. */
function framedState(overrides) {
    return baseState(Object.assign({
        framing: { ok: true, name: 'CAM_0001.OSV', lens: 'dji', zoom: 142.397, fov: 60, correction: 0.6, pan: 12.345,
                   tilt: -3, roll: 0 }
    }, overrides || {}));
}

test('the Program Monitor controls card: open for a new user, one key-capped line per gesture', () => {
    const t = mount();
    const card = t.shell.find('osv-disclosure');
    assert.ok(card.classList.contains('is-open'));
    const head = card.find('osv-disclosure-head');
    assert.equal(head.attributes['aria-expanded'], 'true');
    const lines = card.findAll('osv-hint-line');
    assert.equal(lines.length, 5);
    assert.deepEqual(card.findAll('osv-key').map((k) => k.textContent), ['Drag', 'Shift', 'Ctrl', 'Alt', 'Corner']);
    assert.match(card.find('osv-hint-note').textContent, /Effect Controls/);
    // The chevron is an SVG path, turned to point down while open.
    const chevron = card.find('osv-chevron').children[0];
    assert.equal(chevron.namespaceURI, 'http://www.w3.org/2000/svg');
    assert.ok(chevron.children[0].attributes.d.startsWith('M'));
    head.fire('click');
    assert.deepEqual(t.ctl.calls, [['setHintOpen', false]]);
});

test('closing the controls card folds its body on a spring, and a closed card starts closed', () => {
    const t = mount();
    const card = t.shell.find('osv-disclosure');
    const body = card.find('osv-disclosure-body');
    const chevronPath = card.find('osv-chevron').children[0].children[0];
    const openPath = chevronPath.attributes.d;
    t.view.render(baseState({ settings: core.sanitizeSettings({ hintOpen: false }) }));
    assert.equal(card.classList.contains('is-open'), false);
    t.sched.runAll();
    assert.equal(body.style.height, '0px');
    assert.equal(body.style.opacity, '0');
    assert.notEqual(chevronPath.attributes.d, openPath, 'the chevron turned');
    // And back open: the height is released once the spring lands.
    t.view.render(baseState({ settings: core.sanitizeSettings({ hintOpen: true }) }));
    t.sched.runAll();
    assert.equal(body.style.height, '');
    assert.equal(body.style.opacity, '1');
    card.find('osv-disclosure-head').fire('keydown', { key: 'Enter' });
    assert.deepEqual(t.ctl.calls, [['setHintOpen', false]]);
    // A panel loaded closed starts closed, without animating.
    const closed = mount(baseState({ settings: core.sanitizeSettings({ hintOpen: false }) }));
    assert.equal(closed.shell.find('osv-disclosure-body').style.height, '0px');
});

test('the Keyframe Animation grid: seven tiles with curve icons, one picked, springing to a new pick', () => {
    const t = mount();
    const tiles = t.shell.findAll('osv-ease-tile');
    assert.equal(tiles.length, 7);
    assert.deepEqual(tiles.map((x) => x.attributes['aria-label']), core.EASINGS.map((e) => e.label));
    assert.equal(tiles[0].attributes['aria-checked'], 'true', 'None, until a preset is picked');
    // Every tile draws an SVG: a crossed circle for None, a curve and two
    // keyframe dots for the others.
    const none = tiles[0].find('osv-ease-icon').children[0];
    assert.deepEqual(none.children.map((c) => c.tagName), ['CIRCLE', 'PATH']);
    const siso = tiles[5].find('osv-ease-icon').children[0];
    assert.deepEqual(siso.children.map((c) => c.tagName), ['PATH', 'CIRCLE', 'CIRCLE']);
    assert.equal(siso.children[0].attributes.stroke, 'rgb(174,174,178)', 'unpicked grey');
    tiles[5].fire('click');
    tiles[2].fire('keydown', { key: ' ' });
    assert.deepEqual(t.ctl.calls, [['setEasing', 'slow-in-slow-out'], ['setEasing', 'fast-in-slow-out']]);
    t.view.render(baseState({ settings: core.sanitizeSettings({ easing: 'slow-in-slow-out' }) }));
    t.sched.runAll();
    assert.equal(tiles[5].attributes['aria-checked'], 'true');
    assert.ok(tiles[5].classList.contains('is-selected'));
    assert.equal(tiles[0].attributes['aria-checked'], 'false');
    assert.equal(siso.children[0].attributes.stroke, 'rgb(10,132,255)', 'the accent once picked');
    assert.equal(tiles[5].style.backgroundColor, 'rgb(23,49,79)', 'the tinted fill');
    // A tile dims while pressed and springs back.
    tiles[3].fire('pointerdown');
    t.sched.runAll();
    assert.equal(tiles[3].style.opacity, '0.7');
    tiles[3].fire('pointerup');
    t.sched.runAll();
    assert.equal(tiles[3].style.opacity, '1');
});

test('without SVG the tiles and the chevron fall back to text glyphs', () => {
    const t = mount(baseState(), { noSvg: true });
    assert.equal(t.shell.findAll('osv-ease-glyph').length, 7);
    assert.ok(t.shell.find('osv-chevron-glyph'));
});

test('the Keyframe Animation buttons, the busy label on its own card, and the undo note on CEP', () => {
    const t = mount();
    const buttons = t.shell.findAll('osv-button');
    const easeSel = buttons[9];
    const easeAll = buttons[10];
    easeSel.fire('click');
    easeAll.fire('click');
    assert.deepEqual(t.ctl.calls, [['applyEasingSelected'], ['applyEasingAll']]);
    const note = t.shell.find('osv-card-note');
    assert.ok(note.classList.contains('is-empty'), 'UXP: one undo step, nothing to say');
    t.view.render(baseState({ busy: true, busyAction: 'easing', busyLabel: 'Setting on selection...',
                              capabilities: { undoGroups: false, stabilization: true } }));
    assert.equal(easeSel.textContent, 'Setting on selection...');
    assert.equal(buttons[0].textContent, 'Apply to selected clips', 'the auto-apply button keeps its label');
    easeAll.fire('click');
    assert.equal(t.ctl.calls.length, 2, 'inert while busy');
    assert.equal(note.textContent, 'This Premiere undoes it one clip at a time.');
    assert.equal(note.classList.contains('is-empty'), false);
});

test('Manual Framing: read-outs of the selected clip, preset chips and the zoom stepper', () => {
    const t = mount(framedState());
    const card = t.shell.find('osv-framing-clip').parentNode;
    assert.equal(t.shell.find('osv-framing-clip').textContent, 'CAM_0001.OSV');
    assert.equal(card.classList.contains('is-idle'), false);
    assert.equal(t.shell.find('osv-zoom-value').textContent, '142.4°');
    const values = t.shell.findAll('osv-readout-value').map((v) => v.textContent);
    assert.deepEqual(values, ['60.0°', '0.60', '12.3°', '-3.0°', '0.0°']);
    assert.equal(t.shell.findAll('osv-readout-label')[1].textContent, 'Correction');
    const buttons = t.shell.findAll('osv-button');
    buttons[2].fire('click');                      // Crystal Ball
    buttons[6].fire('keydown', { key: 'Enter' });  // Dewarp
    buttons[7].fire('click');                      // zoom in (narrower)
    buttons[8].fire('click');                      // zoom out (wider)
    assert.deepEqual(t.ctl.calls, [['framingPreset', 'crystal-ball'], ['framingPreset', 'dewarping'], ['zoomStep', -1],
                                   ['zoomStep', 1]]);
    // A new Zoom glides to its number on a spring.
    t.view.render(framedState({ framing: Object.assign({}, framedState().framing, { zoom: 156.1 }) }));
    t.sched.runAll();
    assert.equal(t.shell.find('osv-zoom-value').textContent, '156.1°');
    // The Classic lens shows its Distortion instead of Correction.
    t.view.render(framedState({ framing: { ok: true, name: 'x', lens: 'classic', zoom: 120, fov: 120, distortion: 15,
                                           pan: 0, tilt: 0, roll: 0 } }));
    assert.equal(t.shell.findAll('osv-readout-label')[1].textContent, 'Distortion');
    assert.equal(t.shell.findAll('osv-readout-value')[1].textContent, '15.0%');
});

test('Manual Framing with nothing to frame: the reason, dashes, and inert buttons', () => {
    const t = mount(baseState({ framing: { ok: false, reason: 'outside' } }));
    assert.equal(t.shell.find('osv-framing-clip').textContent, 'Move the playhead over the selected clip.');
    assert.ok(t.shell.find('osv-framing-clip').parentNode.classList.contains('is-idle'));
    assert.equal(t.shell.find('osv-zoom-value').textContent, '—');
    assert.ok(t.shell.findAll('osv-readout-value').every((v) => v.textContent === '—'));
    const buttons = t.shell.findAll('osv-button');
    buttons[2].fire('click');
    buttons[7].fire('click');
    assert.deepEqual(t.ctl.calls, []);
});

test('Stabilisation: DJI Studio\'s two switches, both on, the entry they spell, and an unreachable route says so',
     () => {
    const t = mount();
    // The card's switches follow the auto-apply and drag switches.
    const switches = t.shell.findAll('osv-switch');
    assert.equal(switches.length, 4);
    const [rockSteady, horizon] = switches.slice(2);
    assert.equal(rockSteady.attributes['aria-label'], 'RockSteady');
    assert.equal(horizon.attributes['aria-label'], 'Horizon Leveling');
    assert.equal(rockSteady.attributes.role, 'switch');
    assert.equal(rockSteady.attributes.tabindex, '0', 'reachable from the keyboard');
    // Both on out of the box, drawn without animation.
    assert.equal(rockSteady.attributes['aria-checked'], 'true');
    assert.equal(horizon.attributes['aria-checked'], 'true');
    assert.equal(rockSteady.find('osv-switch-knob').style.left, '16px', 'on: 36 - 2 - 18');
    // The labels and what each one does sit beside them.
    const rows = t.shell.findAll('osv-stab-row');
    assert.equal(rows.length, 2);
    assert.equal(rows[0].find('osv-label').textContent, 'RockSteady');
    assert.equal(rows[1].find('osv-label').textContent, 'Horizon Leveling');
    assert.ok(rows[0].classList.contains('osv-stab-row-top'));
    assert.equal(rows[1].classList.contains('osv-stab-row-top'), false);
    // The caption names the Source Settings entry Apply writes.
    const caption = t.shell.find('osv-stab-caption');
    assert.equal(caption.textContent,
                 'Apply sets Stabilisation to Smooth + Horizon Lock on each selected clip\'s master clip.');
    // Each switch toggles on its own: click and the keyboard.
    rockSteady.fire('click');
    horizon.fire('keydown', { key: ' ' });
    assert.deepEqual(t.ctl.calls, [['setRockSteady', false], ['setHorizonLeveling', false]]);
    // All four pairs: the switches and the caption follow the state.
    const names = [[true, true, 'Smooth + Horizon Lock'], [true, false, 'Smooth'], [false, true, 'Horizon Lock'],
                   [false, false, 'Off']];
    for (const [rs, hl, name] of names) {
        t.view.render(baseState({ settings: core.sanitizeSettings({ rockSteady: rs, horizonLeveling: hl }) }));
        t.sched.runAll();
        assert.equal(rockSteady.attributes['aria-checked'], String(rs));
        assert.equal(horizon.attributes['aria-checked'], String(hl));
        assert.equal(horizon.find('osv-switch-knob').style.left, hl ? '16px' : '2px', 'the knob springs to its end');
        assert.ok(caption.textContent.indexOf(' ' + name + ' on ') !== -1, name);
    }
    // Apply.
    const apply = t.shell.findAll('osv-button')[11];
    apply.fire('click');
    assert.deepEqual(t.ctl.calls[2], ['applyStabilization']);
    // A route that cannot reach Source Settings says so, and Apply is inert.
    t.view.render(baseState({ capabilities: { undoGroups: true, stabilization: false } }));
    assert.match(t.shell.findAll('osv-card-caption')[1].textContent, /can't reach Source Settings/);
    apply.fire('click');
    assert.equal(t.ctl.calls.length, 3, 'unreachable: the button is inert');
    // The theme reaches the card's switches too (both back on by now).
    t.sched.runAll();
    t.view.setTheme('light');
    assert.equal(rockSteady.style.backgroundColor, 'rgb(52,199,89)', 'the light theme green');
});
