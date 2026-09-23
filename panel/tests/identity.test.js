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

// ---------------------------------------------------------------------------
//  [WP-EASING] Keyframe Animation, Manual Framing, Stabilisation
// ---------------------------------------------------------------------------

test('the Keyframe Animation presets are the effect\'s Keyframe Easing popup, in order', () => {
    assert.deepEqual(core.EASINGS.map((e) => e.label), easingItems());
    assert.equal(define(params, 'OSV_REFRAME_EASING_COUNT'), core.EASINGS.length);
    assert.equal(define(params, 'OSV_REFRAME_EASING_DEFAULT'), core.easingById('none').entry);
    core.EASINGS.forEach((e, i) => assert.equal(e.entry, i + 1, e.id));
    assert.match(effect, new RegExp('PF_ADD_POPUPX\\("' + core.PARAM_NAMES.keyframeEasing + '"'));
});

/** OSV_REFRAME_EASING_ITEMS, a macro continued onto a second line. */
function easingItems() {
    const m = /#define\s+OSV_REFRAME_EASING_ITEMS\s*\\\s*\n\s*("[^"]*")/.exec(params);
    assert.ok(m, 'OSV_REFRAME_EASING_ITEMS is defined');
    return JSON.parse(m[1]).split('|');
}

test('the Manual Framing presets carry the effect\'s preset table, number for number', () => {
    // kPresetTable rows: {Preset::X, "Label", fov, distortion, tilt, writes, djiLandscape, dji916, dji34, correction}
    const rows = [...params.matchAll(/\{Preset::(\w+), "([^"]+)", ([-\d.]+), ([-\d.]+), ([-\d.]+), (true|false), ([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+)\}/g)];
    const byLabel = {};
    rows.forEach((r) => {
        byLabel[r[2]] = {
            classicFov: Number(r[3]), distortion: Number(r[4]), tilt: Number(r[5]),
            dji: { landscape: Number(r[7]), portrait916: Number(r[8]), portrait34: Number(r[9]) }, correction: Number(r[10])
        };
    });
    const items = define(params, 'OSV_REFRAME_PRESET_ITEMS').split('|');
    assert.equal(core.FRAMING_PRESETS.length, 5);
    for (const p of core.FRAMING_PRESETS) {
        // The panel says "Dewarp" (DJI Studio's button); the effect's entry is "Dewarping".
        const label = p.label === 'Dewarp' ? 'Dewarping' : p.label;
        assert.ok(byLabel[label], 'kPresetTable has ' + label);
        assert.equal(items.indexOf(label) + 1, p.entry, label + ' entry');
        assert.equal(p.classicFov, byLabel[label].classicFov, label);
        assert.equal(p.distortion, byLabel[label].distortion, label);
        assert.equal(p.tilt, byLabel[label].tilt, label);
        assert.deepEqual(Object.assign({}, p.dji), byLabel[label].dji, label);
        assert.equal(p.correction, byLabel[label].correction, label);
    }
    assert.equal(items.indexOf('Custom') + 1, core.PRESET_CUSTOM_ENTRY);
});

test('the zoom path, its limits and the Classic FOV range are the effect\'s', () => {
    assert.equal(define(params, 'OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION'), core.DJI_ZOOM.fovPerCorrection);
    assert.equal(define(params, 'OSV_REFRAME_DJI_STUDIO_FOV_MIN'), core.DJI_ZOOM.fovMin);
    assert.equal(define(params, 'OSV_REFRAME_DJI_STUDIO_FOV_MAX'), core.DJI_ZOOM.fovMax);
    assert.equal(define(params, 'OSV_REFRAME_DJI_STUDIO_CORRECTION_MAX'), core.DJI_ZOOM.correctionMax);
    assert.equal(define(params, 'OSV_REFRAME_CORRECTION_VALID_MIN'), core.DJI_ZOOM.correctionMin);
    assert.equal(define(params, 'OSV_REFRAME_FOV_VALID_MIN'), core.CLASSIC_FOV.min);
    assert.equal(define(params, 'OSV_REFRAME_FOV_VALID_MAX'), core.CLASSIC_FOV.max);
    // The Zoom control's own default is DJI Studio's formula at the default lens on 16:9.
    assert.equal(Math.round(core.djiZoomDeg(define(params, 'OSV_REFRAME_DJI_FOV_DEFAULT'),
                                            define(params, 'OSV_REFRAME_CORRECTION_DEFAULT'), 16 / 9) * 10) / 10,
                 define(params, 'OSV_REFRAME_ZOOM_DEFAULT'));
});

test('the framing controls the panel names are registered under exactly those names', () => {
    for (const role of ['pan', 'tilt', 'roll']) {
        assert.match(effect, new RegExp('PF_ADD_ANGLE\\("' + core.PARAM_NAMES[role] + '"'), role);
    }
    for (const role of ['fov', 'djiFov', 'distortion', 'zoom', 'correction']) {
        assert.match(effect, new RegExp('PF_ADD_FLOAT_SLIDERX\\("' + core.PARAM_NAMES[role] + '"'), role);
    }
    assert.match(effect, new RegExp('PF_ADD_POPUPX\\("' + core.PARAM_NAMES.outputResolution + '"'));
    // The DJI FOV is SHOWN as "FOV" (kLensControls), which is why the panel
    // tells the two FOVs apart by their neighbours.
    assert.match(params, /\{kIndexDjiFov, CameraModel::Dji, "FOV"\}/);
    // And the popup entry counts the numbering is learned from.
    assert.equal(core.REFRAME_POPUP_COUNTS['Output Resolution'], define(params, 'OSV_REFRAME_RESOLUTION_COUNT'));
    assert.equal(core.REFRAME_POPUP_COUNTS.Preset, define(params, 'OSV_REFRAME_PRESET_COUNT'));
    assert.equal(core.REFRAME_POPUP_COUNTS.Lens, define(params, 'OSV_REFRAME_LENS_COUNT'));
    assert.equal(core.REFRAME_POPUP_COUNTS['Keyframe Easing'], define(params, 'OSV_REFRAME_EASING_COUNT'));
});

test('the Stabilisation choices land on the Source Settings entries they name', () => {
    const ss = read('plugins/sourcesettings/SourceSettingsParams.h');
    const ssMain = read('plugins/sourcesettings/SourceSettingsMain.cpp');
    const identity = read('plugins/common/SourceSettingsIdentity.h');
    assert.equal(define(identity, 'OSV_SOURCE_SETTINGS_MATCH_NAME'), core.SOURCE_SETTINGS_MATCH_NAME);
    const items = define(ss, 'OSV_SS_STAB_ITEMS').split('|');
    assert.deepEqual(items, ['Off', 'Horizon Lock', 'Full', 'Smooth']);
    assert.equal(items[core.stabilizationById('off').entry - 1], 'Off');
    assert.equal(items[core.stabilizationById('rocksteady').entry - 1], 'Smooth');
    assert.equal(items[core.stabilizationById('horizon').entry - 1], 'Horizon Lock');
    // The default choice is the Source Settings default.
    assert.equal(core.sanitizeSettings(null).stabilization, 'horizon');
    // (That define carries a trailing comment, so it is read as a leading integer.)
    const stabDefault = /#define\s+OSV_SS_STAB_DEFAULT\s+(\d+)/.exec(ss);
    assert.ok(stabDefault);
    assert.equal(Number(stabDefault[1]), core.stabilizationById('horizon').entry);
    // Every popup the numbering is learned from, by name and entry count.
    const counts = {
        'Colour Output': 'OSV_SS_COLOR_COUNT', 'Output Size': 'OSV_SS_SIZE_COUNT', 'Stabilisation': 'OSV_SS_STAB_COUNT',
        'Calibration': 'OSV_SS_CALIB_COUNT', 'D-Log M Curve': 'OSV_SS_FIT_COUNT', 'Render Device': 'OSV_SS_DEVICE_COUNT'
    };
    for (const [name, macro] of Object.entries(counts)) {
        assert.equal(core.SOURCE_POPUP_COUNTS[name], define(ss, macro), name);
        assert.ok(ssMain.includes('PF_ADD_POPUPX("' + name + '"'), name + ' is registered under that name');
    }
    assert.equal(core.SOURCE_PARAM_NAMES.stabilization, 'Stabilisation');
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
