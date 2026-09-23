/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * boot.js - wires core, controller, view and a host adapter together.
 *
 * Both panels start the same way; only the adapter and the theme source
 * differ, and each panel's main.js passes those in.  Whatever fails here
 * fails into the status line, never into Premiere: an adapter that cannot be
 * built becomes one whose init() reports why.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvBoot = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    function messageOf(err) {
        if (err && typeof err.message === 'string' && err.message.length > 0) {
            return err.message;
        }
        return typeof err === 'string' && err.length > 0 ? err : 'unknown error';
    }

    /** An adapter that only ever says why the real one is missing. */
    function failingAdapter(reason) {
        var fail = function () { return Promise.reject(new Error(reason)); };
        return {
            init: function () { throw new Error(reason); },
            dispose: function () {},
            getActiveSequence: fail,
            getSequenceIds: fail,
            signature: fail,
            scan: fail,
            apply: fail,
            checkEffect: fail,
            // [WP-EASING] The newer actions fail the same way.
            capabilities: function () { return { undoGroups: false, stabilization: false }; },
            setEasing: fail,
            readFraming: fail,
            writeFraming: fail,
            setStabilization: fail
        };
    }

    /** localStorage behind a guard (it can throw when storage is disabled). */
    function localStore() {
        return {
            get: function (key) {
                try {
                    return (typeof localStorage !== 'undefined' && localStorage) ? localStorage.getItem(key) : null;
                } catch (err) {
                    return null;
                }
            },
            set: function (key, text) {
                try {
                    if (typeof localStorage !== 'undefined' && localStorage) {
                        localStorage.setItem(key, text);
                    }
                } catch (err) {
                    // Settings then last for this session only.
                }
            }
        };
    }

    /**
     * Start the panel.
     *
     * @param {object} options
     *   root           the element to mount into
     *   createAdapter  function returning the host adapter (may throw)
     *   theme          { read() -> {name, background?} | Promise, subscribe(cb) }  (optional)
     *   log            function(message)  (optional)
     * @returns {{controller: object, view: object} | null}
     */
    function start(options) {
        var o = options || {};
        var g = (typeof globalThis !== 'undefined') ? globalThis : window;
        var core = g.OsvCore;
        var spring = g.OsvSpring;
        var Controller = g.OsvController;
        var View = g.OsvView;
        var log = typeof o.log === 'function' ? o.log : function () {};
        if (!o.root || !core || !spring || !Controller || !View) {
            if (o.root) {
                o.root.textContent = 'OpenOSV could not load its scripts. Reinstall the panel.';
            }
            return null;
        }

        var adapter;
        try {
            adapter = o.createAdapter();
            if (!adapter) {
                adapter = failingAdapter('no host adapter');
            }
        } catch (err) {
            adapter = failingAdapter(messageOf(err));
        }

        var view = null;
        var controller = Controller.createController({
            core: core,
            adapter: adapter,
            storage: localStore(),
            timers: {
                setTimeout: function (fn, ms) { return setTimeout(fn, ms); },
                clearTimeout: function (h) { clearTimeout(h); },
                setInterval: function (fn, ms) { return setInterval(fn, ms); },
                clearInterval: function (h) { clearInterval(h); },
                now: function () { return Date.now(); }
            },
            onChange: function (state) {
                if (view) {
                    view.render(state);
                }
            }
        });

        try {
            view = View.mount(o.root, { controller: controller, core: core, spring: spring });
        } catch (err) {
            log('view: ' + messageOf(err));
            o.root.textContent = 'OpenOSV could not draw its panel: ' + messageOf(err);
        }

        // Theme: first the current one, then every change.
        var applyTheme = function (t) {
            if (!view || !t) {
                return;
            }
            try {
                view.setTheme(t.name, t.background);
            } catch (err) {
                log('theme: ' + messageOf(err));
            }
        };
        if (o.theme && typeof o.theme.read === 'function') {
            try {
                Promise.resolve(o.theme.read()).then(applyTheme, function (err) { log('theme: ' + messageOf(err)); });
            } catch (err) {
                log('theme: ' + messageOf(err));
            }
        }
        if (o.theme && typeof o.theme.subscribe === 'function') {
            try {
                o.theme.subscribe(applyTheme);
            } catch (err) {
                log('theme subscribe: ' + messageOf(err));
            }
        }

        controller.start();
        return { controller: controller, view: view };
    }

    return Object.freeze({ start: start, failingAdapter: failingAdapter });
}));
