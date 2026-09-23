/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * main.js - start-up of the CEP panel.
 *
 * Talks to CEP through window.__adobe_cep__, the native object Adobe's
 * CSInterface.js wraps (evalScript, addEventListener, getHostEnvironment,
 * getSystemPath), so the panel ships no Adobe file.
 */
(function () {
    'use strict';

    var g = (typeof globalThis !== 'undefined') ? globalThis : window;
    var cep = (typeof window !== 'undefined' && window.__adobe_cep__) ? window.__adobe_cep__ : null;

    /** CEP's theme-change event, as CSInterface names it. */
    var THEME_EVENT = 'com.adobe.csxs.events.ThemeColorChanged';

    function log(message) {
        if (typeof console !== 'undefined' && console && typeof console.log === 'function') {
            console.log('[OpenOSV] ' + message);
        }
    }

    /**
     * Premiere's panel colour from the host environment: the exact grey for
     * the page background, and dark or light by its luminance.
     */
    function readTheme() {
        var fallback = { name: 'dark' };
        if (!cep || typeof cep.getHostEnvironment !== 'function') {
            return fallback;
        }
        try {
            var env = JSON.parse(cep.getHostEnvironment());
            var c = env && env.appSkinInfo && env.appSkinInfo.panelBackgroundColor &&
                    env.appSkinInfo.panelBackgroundColor.color;
            if (!c || !isFinite(c.red) || !isFinite(c.green) || !isFinite(c.blue)) {
                return fallback;
            }
            var r = Math.round(c.red);
            var gr = Math.round(c.green);
            var b = Math.round(c.blue);
            // Rec. 709 luma of the panel grey: Premiere's light themes sit well above the middle.
            var luma = 0.2126 * r + 0.7152 * gr + 0.0722 * b;
            return { name: luma > 128 ? 'light' : 'dark', background: 'rgb(' + r + ',' + gr + ',' + b + ')' };
        } catch (err) {
            return fallback;
        }
    }

    /** The extension folder as a path ExtendScript's $.evalFile accepts. */
    function extensionPath() {
        if (!cep || typeof cep.getSystemPath !== 'function') {
            return '';
        }
        try {
            var p = decodeURI(String(cep.getSystemPath('extension')));
            // "file:///C:/..." on Windows, "file:///Users/..." on macOS.
            if (/^file:\/\/\/[A-Za-z]:/.test(p)) {
                return p.replace(/^file:\/\/\//, '');
            }
            return p.replace(/^file:\/\//, '');
        } catch (err) {
            return '';
        }
    }

    /**
     * Premiere evaluates host/host.jsx itself (manifest ScriptPath).  If it
     * did not - an engine reset, a failed first load - evaluate it now, so
     * the panel does not sit there reporting a missing host script.
     */
    function ensureHostScript(done) {
        if (!cep || typeof cep.evalScript !== 'function') {
            done();
            return;
        }
        try {
            cep.evalScript('typeof OpenOSVHost', function (answer) {
                if (answer !== 'undefined') {
                    done();
                    return;
                }
                var file = extensionPath();
                if (!file) {
                    done();
                    return;
                }
                var literal = g.OsvCore ? g.OsvCore.toExtendScriptLiteral(file + '/host/host.jsx') : '""';
                cep.evalScript('$.evalFile(' + literal + ')', function () { done(); });
            });
        } catch (err) {
            log('ensureHostScript: ' + (err && err.message ? err.message : err));
            done();
        }
    }

    ensureHostScript(function () {
        if (!g.OsvBoot) {
            return;
        }
        g.OsvBoot.start({
            root: document.getElementById('app'),
            createAdapter: function () {
                if (!cep) {
                    throw new Error('open this panel from Premiere Pro (Window > Extensions > OpenOSV)');
                }
                return g.OsvCepAdapter.createCepAdapter(cep, g.OsvCore, { log: log });
            },
            theme: {
                read: readTheme,
                subscribe: function (cb) {
                    if (cep && typeof cep.addEventListener === 'function') {
                        cep.addEventListener(THEME_EVENT, function () { cb(readTheme()); });
                    }
                }
            },
            log: log
        });
    });
}());
