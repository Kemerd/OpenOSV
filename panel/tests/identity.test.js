// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// identity.test.js - the panel's constants against the plug-ins' sources and
// the two manifests, so a rename on either side fails a test instead of
// shipping a panel that quietly never finds its effect.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const core = require('../shared/osvcore.js');

const REPO = path.join(__dirname, '..', '..');
const read = (rel) => fs.readFileSync(path.join(REPO, rel), 'utf8');

/** The value of `#define NAME value` in a C header. */
function define(src, name) {
    const m = new RegExp('#define\\s+' + name + '\\s+(.+?)\\s*$', 'm').exec(src);
    assert.ok(m, name + ' is defined');
    const v = m[1].trim();
    return v.startsWith('"') ? JSON.parse(v) : Number(v);
}

const params = read('plugins/reframe/ReframeParams.h');
const effect = read('plugins/reframe/EffectMain.cpp');

test('the effect identity matches plugins/reframe/ReframeParams.h', () => {
    assert.equal(define(params, 'OSV_REFRAME_MATCH_NAME'), core.REFRAME_MATCH_NAME);
    assert.equal(define(params, 'OSV_REFRAME_DISPLAY_NAME'), core.REFRAME_DISPLAY_NAME);
    // Premiere registers every AE-API effect with an "AE." prefix; the header says so.
    assert.ok(params.includes('"AE.' + core.REFRAME_MATCH_NAME + '"'));
    assert.equal(core.REFRAME_HOST_MATCH_NAME, 'AE.' + core.REFRAME_MATCH_NAME);
});

test('the lens popup is "DJI|Classic" with DJI first and default, and Preset starts with Custom', () => {
    assert.equal(define(params, 'OSV_REFRAME_LENS_ITEMS'), 'DJI|Classic');
    assert.equal(define(params, 'OSV_REFRAME_LENS_DEFAULT'), 1);
    assert.equal(define(params, 'OSV_REFRAME_CAMERA_MODEL_DEFAULT'), 1, 'the hidden mirror defaults to DJI too');
    assert.equal(define(params, 'OSV_REFRAME_PRESET_ITEMS').split('|')[0], 'Custom');
    assert.deepEqual(Object.values(core.LENS), ['dji', 'classic']);
});

test('Drag Sensitivity ranges and default match the effect', () => {
    assert.equal(define(params, 'OSV_REFRAME_DRAG_SENSITIVITY_VALID_MIN'), core.DRAG.validMin);
    assert.equal(define(params, 'OSV_REFRAME_DRAG_SENSITIVITY_VALID_MAX'), core.DRAG.validMax);
    assert.equal(define(params, 'OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MIN'), core.DRAG.sliderMin);
    assert.equal(define(params, 'OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MAX'), core.DRAG.sliderMax);
    assert.equal(define(params, 'OSV_REFRAME_DRAG_SENSITIVITY_DEFAULT'), core.DRAG.defaultValue);
});

test('the parameters the panel writes are registered under exactly these names', () => {
    assert.match(effect, new RegExp('PF_ADD_POPUPX\\("' + core.PARAM_NAMES.lens + '"'));
    assert.match(effect, new RegExp('PF_ADD_POPUPX\\("' + core.PARAM_NAMES.preset + '"'));
    assert.match(effect, new RegExp('PF_ADD_CHECKBOX\\("' + core.PARAM_NAMES.cameraModel + '"'));
    assert.match(effect, new RegExp('PF_ADD_FLOAT_SLIDERX\\("' + core.PARAM_NAMES.dragSensitivity + '"'));
});

test('the media extensions are the importer\'s', () => {
    const importer = read('plugins/importer/ImporterEntry.cpp');
    assert.ok(importer.includes('kExtensions[] = "osv\\0lrf\\0\\0"'));
    assert.deepEqual(core.OSV_EXTENSIONS, ['osv', 'lrf']);
});

test('the UXP manifest is a valid v5 Premiere panel at the panel version', () => {
    const m = JSON.parse(read('panel/uxp/manifest.json'));
    assert.equal(m.manifestVersion, 5);
    assert.equal(m.version, core.PANEL_VERSION);
    assert.match(m.version, /^\d+\.\d+\.\d+$/);
    assert.equal(m.id, 'com.openosv.panel');
    assert.equal(m.main, 'index.html');
    // A production .ccx must name ONE host (Adobe's packaging guide).
    assert.equal(Array.isArray(m.host), false);
    assert.equal(m.host.app, 'premierepro');
    assert.equal(m.host.minVersion, '25.6.0', 'the release that made the UXP DOM official');
    assert.equal(m.entrypoints.length, 1);
    assert.equal(m.entrypoints[0].type, 'panel');
    assert.equal(m.entrypoints[0].id, 'openosvPanel');
    // main.js wires that entrypoint id.
    assert.ok(read('panel/uxp/main.js').includes('openosvPanel:'));
});

test('the CEP manifest targets Premiere at the panel version and loads host.jsx', () => {
    const x = read('panel/cep/CSXS/manifest.xml');
    assert.match(x, /ExtensionBundleId="com\.openosv\.panel"/);
    assert.match(x, new RegExp('ExtensionBundleVersion="' + core.PANEL_VERSION.replace(/\./g, '\\.') + '"'));
    assert.match(x, /<Host Name="PPRO" Version="\[22\.0,99\.9\]"\/>/);
    assert.match(x, /<ScriptPath>\.\/host\/host\.jsx<\/ScriptPath>/);
    assert.match(x, /<MainPath>\.\/index\.html<\/MainPath>/);
    assert.match(x, /<Type>Panel<\/Type>/);
});

test('both pages load the shared scripts in dependency order', () => {
    const order = ['shared/osvcore.js', 'shared/spring.js', 'shared/controller.js', 'shared/view.js', 'shared/boot.js'];
    for (const [page, adapter] of [['panel/uxp/index.html', 'uxpAdapter.js'], ['panel/cep/index.html', 'cepAdapter.js']]) {
        const html = read(page);
        const srcs = [...html.matchAll(/<script src="([^"]+)"/g)].map((m) => m[1]);
        assert.deepEqual(srcs, order.concat([adapter, 'main.js']), page);
        assert.ok(html.includes('href="shared/panel.css"'), page);
        for (const s of srcs) {
            const file = s.startsWith('shared/') ? path.join('panel', s) : path.join(path.dirname(page), s);
            assert.ok(fs.existsSync(path.join(REPO, file)), page + ' -> ' + file);
        }
    }
});
