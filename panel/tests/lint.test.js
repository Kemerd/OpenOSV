// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// lint.test.js - what the two runtimes cannot take, and the house rules.
//
//   - Panel JavaScript runs in CEP 10 (Chromium 74) at the oldest: no
//     optional chaining, no nullish coalescing.
//   - panel.css runs in UXP, whose CSS has no grid, transitions, transforms,
//     flex gap or clamp() - motion lives in spring.js instead.
//   - No raw U+2028 / U+2029 anywhere (a line break to ExtendScript).
//   - Every source file carries the SPDX header; no leftover markers.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const PANEL = path.join(__dirname, '..');

/** Every file under panel/ with one of the extensions. */
function files(exts) {
    const out = [];
    const walk = (dir) => {
        for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
            const p = path.join(dir, e.name);
            if (e.isDirectory()) {
                walk(p);
            } else if (exts.some((x) => e.name.endsWith(x))) {
                out.push(p);
            }
        }
    };
    walk(PANEL);
    return out;
}

/** Source with comments and string / template literals blanked out. */
function codeOnly(src) {
    return src
        .replace(/\/\*[\s\S]*?\*\//g, '')
        .replace(/(^|[^:\\])\/\/[^\n]*/g, '$1')
        .replace(/'(?:\\.|[^'\\\n])*'/g, "''")
        .replace(/"(?:\\.|[^"\\\n])*"/g, '""')
        .replace(/`(?:\\.|[^`\\])*`/g, '``');
}

const RUNTIME_JS = files(['.js']).filter((f) => !f.includes(path.sep + 'tests' + path.sep));

test('panel JavaScript runs on Chromium 74: no ?. and no ??', () => {
    assert.ok(RUNTIME_JS.length >= 9, 'found the runtime sources');
    for (const f of RUNTIME_JS) {
        const code = codeOnly(fs.readFileSync(f, 'utf8'));
        assert.equal(/\?\.(?!\d)/.test(code), false, path.relative(PANEL, f) + ' uses optional chaining');
        assert.equal(/\?\?/.test(code), false, path.relative(PANEL, f) + ' uses nullish coalescing');
    }
});

test('panel.css stays inside what UXP supports', () => {
    const css = fs.readFileSync(path.join(PANEL, 'shared', 'panel.css'), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
    for (const [re, what] of [
        [/\btransition\s*:/, 'transition'], [/\banimation\s*:/, 'animation'], [/\btransform\s*:/, 'transform'],
        [/display\s*:\s*grid/, 'grid'], [/(^|[\s;{])gap\s*:/, 'flex gap'], [/clamp\(/, 'clamp()'],
        [/@keyframes/, '@keyframes']
    ]) {
        assert.equal(re.test(css), false, 'panel.css uses ' + what);
    }
});

test('no raw line or paragraph separators in any panel file', () => {
    const ls = String.fromCharCode(0x2028);
    const ps = String.fromCharCode(0x2029);
    for (const f of files(['.js', '.jsx', '.html', '.css', '.json', '.xml'])) {
        const text = fs.readFileSync(f, 'utf8');
        assert.equal(text.includes(ls) || text.includes(ps), false, path.relative(PANEL, f));
    }
});

test('every source file carries the licence header and no leftover markers', () => {
    const marker = new RegExp('\\b(TO' + 'DO|FIX' + 'ME|XX' + 'X)\\b');
    for (const f of files(['.js', '.jsx', '.html', '.css', '.xml'])) {
        const text = fs.readFileSync(f, 'utf8');
        assert.ok(text.includes('SPDX-License-Identifier: Apache-2.0'), path.relative(PANEL, f) + ' has no SPDX header');
        assert.equal(marker.test(text), false, path.relative(PANEL, f) + ' has a leftover marker');
    }
});
