/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * main.js - start-up of the UXP panel.
 *
 * Runs when Premiere loads the plug-in.  The controller starts here, not in
 * the panel's show() hook, so auto-apply follows the plug-in's lifetime
 * rather than the panel tab's.
 */
(function () {
    'use strict';

    var g = (typeof globalThis !== 'undefined') ? globalThis : window;

    function log(message) {
        if (typeof console !== 'undefined' && console && typeof console.log === 'function') {
            console.log('[OpenOSV] ' + message);
        }
    }

    /** require() that returns null instead of throwing. */
    function tryRequire(name) {
        try {
            return (typeof require === 'function') ? require(name) : null;
        } catch (err) {
            log('require ' + name + ': ' + (err && err.message ? err.message : err));
            return null;
        }
    }

    var ppro = tryRequire('premierepro');
    var uxp = tryRequire('uxp');
    // UXP's file system module (manifest: localFileSystem "fullAccess"), for
    // one question only: is the camera's .LRF beside an .OSV.
    var fs = tryRequire('fs');

    /** True when an error says the file is not there (rather than "cannot look"). */
    function isNotFound(err) {
        var text = String((err && (err.code || err.message)) || err || '');
        return /ENOENT|not\s*found|no such|does not exist|cannot find/i.test(text);
    }

    /**
     * Whether `path` names an existing file: Promise<true | false | null>,
     * null when the file system cannot be asked (no fs module, no
     * permission).  A native path is tried first, then its file: URL form,
     * which every UXP fs accepts.
     */
    function fileExists(path) {
        if (!fs || typeof fs.lstat !== 'function' || typeof path !== 'string' || path.length === 0) {
            return Promise.resolve(null);
        }
        var forms = [path, 'file:' + path.replace(/\\/g, '/')];
        // What the forms said: something there (a file, or a folder of that
        // name, which is not the .LRF), or "not there".
        var verdict = null;
        var chain = Promise.resolve();
        forms.forEach(function (form) {
            chain = chain.then(function () {
                if (verdict === true || verdict === false) {
                    return undefined;
                }
                return new Promise(function (resolve) { resolve(fs.lstat(form)); }).then(function (stats) {
                    verdict = !(stats && typeof stats.isFile === 'function' && stats.isFile() === false);
                }, function (err) {
                    if (isNotFound(err)) {
                        verdict = 'missing';
                    }
                });
            });
        });
        return chain.then(function () {
            if (verdict === true || verdict === false) {
                return verdict;
            }
            return verdict === 'missing' ? false : null;
        });
    }

    /**
     * The file names in `folder`: Promise<string[] | null>, null when the
     * folder cannot be read.  Native path first, then its file: URL form.
     */
    function listFolder(folder) {
        if (!fs || typeof fs.readdir !== 'function' || typeof folder !== 'string' || folder.length === 0) {
            return Promise.resolve(null);
        }
        var plain = folder.replace(/[\\\/]+$/, '');
        var forms = [plain, 'file:' + plain.replace(/\\/g, '/')];
        var names = null;
        var chain = Promise.resolve();
        forms.forEach(function (form) {
            chain = chain.then(function () {
                if (names !== null) {
                    return undefined;
                }
                return new Promise(function (resolve) { resolve(fs.readdir(form)); }).then(function (list) {
                    if (Array.isArray(list)) {
                        names = list.map(function (n) { return String(n); });
                    }
                }, function () {
                    // The next form, or "cannot tell".
                });
            });
        });
        return chain.then(function () { return names; });
    }

    /**
     * Close to Premiere's panel grey per theme, for hosts older than 26.5,
     * which cannot say the exact colour.  Only a fallback: without it a light
     * theme would show the light cards on the stylesheet's dark page.
     */
    var APPROXIMATE_GREY = {
        darkest: 'rgb(29,29,29)',
        dark: 'rgb(35,35,35)',
        medium: 'rgb(50,50,50)',
        light: 'rgb(232,232,232)',
        lightest: 'rgb(245,245,245)'
    };

    /**
     * Premiere's theme: document.theme.getCurrent() names it ("light",
     * "lightest", "dark", "darkest"), and on 26.5+ uxp.host.getBackgroundColor()
     * gives the exact panel grey so the panel sits flush with its neighbours.
     */
    var theme = {
        read: function () {
            var current = 'dark';
            try {
                current = (document.theme && typeof document.theme.getCurrent === 'function')
                    ? String(document.theme.getCurrent()) : 'dark';
            } catch (err) {
                current = 'dark';
            }
            var name = current.indexOf('light') !== -1 ? 'light' : 'dark';
            var fallback = {
                name: name,
                background: APPROXIMATE_GREY[current] || (name === 'light' ? APPROXIMATE_GREY.light : APPROXIMATE_GREY.dark)
            };
            var host = uxp && uxp.host;
            if (!host || typeof host.getBackgroundColor !== 'function') {
                return fallback;
            }
            return Promise.resolve(host.getBackgroundColor()).then(function (text) {
                var parsed = typeof text === 'string' ? JSON.parse(text) : text;
                var v = parsed && parsed.value;
                if (!v || !isFinite(v.red) || !isFinite(v.green) || !isFinite(v.blue)) {
                    return fallback;
                }
                var to255 = function (c) { return Math.round(Math.max(0, Math.min(1, c)) * 255); };
                return { name: name, background: 'rgb(' + to255(v.red) + ',' + to255(v.green) + ',' + to255(v.blue) + ')' };
            }).then(null, function () { return fallback; });
        },
        subscribe: function (cb) {
            if (document.theme && document.theme.onUpdated && typeof document.theme.onUpdated.addListener === 'function') {
                document.theme.onUpdated.addListener(function () {
                    Promise.resolve(theme.read()).then(cb, function () {});
                });
            }
        }
    };

    var app = g.OsvBoot ? g.OsvBoot.start({
        root: document.getElementById('app'),
        createAdapter: function () {
            if (!ppro) {
                throw new Error('this Premiere has no UXP API (Premiere Pro 25.6 or later is needed)');
            }
            return g.OsvUxpAdapter.createUxpAdapter(ppro, g.OsvCore, { log: log, fileExists: fileExists, listFolder: listFolder });
        },
        theme: theme,
        log: log
    }) : null;

    // Panel lifecycle.  show() re-measures (a panel docked while hidden has
    // no layout until it is shown); destroy() detaches from the host.
    var entrypoints = uxp && uxp.entrypoints;
    if (entrypoints && typeof entrypoints.setup === 'function') {
        try {
            entrypoints.setup({
                plugin: {
                    destroy: function () {
                        if (app && app.controller) {
                            app.controller.stop();
                        }
                    }
                },
                panels: {
                    openosvPanel: {
                        show: function () {
                            if (app && app.view) {
                                app.view.relayout();
                            }
                        }
                    }
                }
            });
        } catch (err) {
            log('entrypoints.setup: ' + (err && err.message ? err.message : err));
        }
    }
}());
