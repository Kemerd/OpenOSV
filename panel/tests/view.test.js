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

function fakeDocument() {
    const doc = { createElement: (tag) => new FakeElement(tag, doc) };
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
        applyAll() { calls.push(['applyAll']); }
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

function mount(state) {
    const doc = fakeDocument();
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
    assert.deepEqual(buttons.map((b) => b.textContent), ['Apply to selected clips', 'Apply to all OSV clips in this sequence']);
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
