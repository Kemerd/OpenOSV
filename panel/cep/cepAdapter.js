/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * cepAdapter.js - the panel's link to Premiere Pro through CEP + ExtendScript
 * (Premiere 22 to 26; Adobe plans to retire CEP about a year after 25.6).
 *
 * The panel JavaScript cannot touch Premiere's DOM itself; it evaluates
 * calls into host/host.jsx, which answers every call with JSON text.  This
 * file turns those calls into the adapter contract controller.js expects
 * (see uxp/uxpAdapter.js for the contract), and keeps the decisions on the
 * panel side where OsvCore makes them:
 *
 *   scan    -> host.jsx lists the OSV clips (facts only)
 *   apply   -> host.jsx adds the effect (QE) and reports each new instance's
 *              parameters; OsvCore.planParamWrites() decides the lens writes;
 *              host.jsx setParams() carries them out through the official DOM
 *
 * The CEP bridge is INJECTED - { evalScript(script, callback),
 * addEventListener(type, fn), removeEventListener(type, fn) }, which is what
 * window.__adobe_cep__ provides and what Adobe's CSInterface.js wraps - so
 * panel/tests/cepAdapter.test.js runs this file against host.jsx itself,
 * evaluated in a mock ExtendScript world.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvCepAdapter = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /** The CSXS event host.jsx dispatches when the timeline changes. */
    var EVENT_TYPE = 'com.openosv.panel.hostchange';

    /** How long a host call may take before the panel gives up on it. */
    var CALL_TIMEOUT_MS = 15000;

    /** What CEP hands back when ExtendScript threw at the top level. */
    var EVAL_ERROR = 'EvalScript error.';

    function messageOf(err) {
        if (err && typeof err.message === 'string' && err.message.length > 0) {
            return err.message;
        }
        return typeof err === 'string' && err.length > 0 ? err : 'unknown error';
    }

    /**
     * @param {object} bridge   window.__adobe_cep__ (or a test double)
     * @param {object} core     OsvCore
     * @param {object} [options] { timers: {setTimeout, clearTimeout}, log(message) }
     */
    function createCepAdapter(bridge, core, options) {
        if (!core) {
            throw new Error('OsvCore is not loaded');
        }
        var opts = options || {};
        var log = typeof opts.log === 'function' ? opts.log : function () {};
        var timers = opts.timers || {};
        var setT = typeof timers.setTimeout === 'function' ? timers.setTimeout : setTimeout;
        var clearT = typeof timers.clearTimeout === 'function' ? timers.clearTimeout : clearTimeout;

        var onEvent = null;
        var listener = null;

        /**
         * Evaluate `OpenOSVHost.<fn>(<arg>)` and parse the JSON it returns.
         * Rejects with a readable message for a missing host script, an
         * ExtendScript error, a non-JSON answer, {ok:false} or a timeout.
         */
        function call(fn, arg) {
            return new Promise(function (resolve, reject) {
                if (!bridge || typeof bridge.evalScript !== 'function') {
                    reject(new Error('this panel is not running inside Premiere Pro'));
                    return;
                }
                var argText = (arg === undefined) ? '' : core.toExtendScriptLiteral(arg);
                // A missing host script answers with a clear error, not
                // "OpenOSVHost is undefined" swallowed into EvalScript error.
                var script = '(typeof OpenOSVHost === "undefined")' +
                             ' ? \'{"ok":false,"error":"host-script-missing"}\'' +
                             ' : OpenOSVHost.' + fn + '(' + argText + ')';
                var done = false;
                var timer = setT(function () {
                    if (!done) {
                        done = true;
                        reject(new Error('Premiere didn\'t answer in time'));
                    }
                }, CALL_TIMEOUT_MS);
                var finish = function (text) {
                    if (done) {
                        return;
                    }
                    done = true;
                    clearT(timer);
                    if (typeof text !== 'string' || text.length === 0 || text === EVAL_ERROR) {
                        reject(new Error('Premiere\'s script engine refused the call'));
                        return;
                    }
                    var parsed = null;
                    try {
                        parsed = JSON.parse(text);
                    } catch (err) {
                        reject(new Error('Premiere answered with something unreadable'));
                        return;
                    }
                    if (!parsed || typeof parsed !== 'object') {
                        reject(new Error('Premiere answered with something unreadable'));
                        return;
                    }
                    if (parsed.ok !== true) {
                        var why = typeof parsed.error === 'string' ? parsed.error : 'the host script failed';
                        if (why === 'host-script-missing') {
                            why = 'the panel\'s host script didn\'t load. Reinstall the panel';
                        }
                        reject(new Error(why));
                        return;
                    }
                    resolve(parsed);
                };
                try {
                    bridge.evalScript(script, finish);
                } catch (err) {
                    if (!done) {
                        done = true;
                        clearT(timer);
                        reject(new Error(messageOf(err)));
                    }
                }
            });
        }

        function wantsParams(settings) {
            var s = core.sanitizeSettings(settings);
            return s.lens === core.LENS.classic || s.dragEnabled;
        }

        return {
            init: function (handler) {
                if (!bridge || typeof bridge.evalScript !== 'function') {
                    throw new Error('this panel is not running inside Premiere Pro');
                }
                onEvent = typeof handler === 'function' ? handler : null;
                listener = function (event) {
                    if (typeof onEvent !== 'function') {
                        return;
                    }
                    try {
                        onEvent(event && event.data ? String(event.data) : 'host');
                    } catch (err) {
                        log('event handler: ' + messageOf(err));
                    }
                };
                try {
                    if (typeof bridge.addEventListener === 'function') {
                        bridge.addEventListener(EVENT_TYPE, listener);
                    }
                } catch (err) {
                    log('addEventListener: ' + messageOf(err));
                }
                // Bind the ExtendScript events; if it fails, the poll covers it.
                call('bindEvents').then(null, function (err) { log('bindEvents: ' + messageOf(err)); });
            },

            dispose: function () {
                try {
                    if (listener && bridge && typeof bridge.removeEventListener === 'function') {
                        bridge.removeEventListener(EVENT_TYPE, listener);
                    }
                } catch (err) {
                    log('removeEventListener: ' + messageOf(err));
                }
                listener = null;
                onEvent = null;
            },

            getActiveSequence: function () {
                return call('activeSequence').then(function (r) {
                    var s = r.sequence;
                    if (!s || typeof s !== 'object' || !s.id) {
                        return null;
                    }
                    return { id: String(s.id), name: String(s.name || ''), projectId: String(s.projectId || '') };
                });
            },

            getSequenceIds: function () {
                return call('sequenceIds').then(function (r) {
                    return Array.isArray(r.ids) ? r.ids.map(String) : null;
                });
            },

            signature: function () {
                return call('signature').then(function (r) {
                    return typeof r.signature === 'string' ? r.signature : '';
                });
            },

            scan: function (seq, scanOptions) {
                var selectedOnly = !!(scanOptions && scanOptions.selectedOnly);
                return call('scan', { sequenceId: seq ? seq.id : '', selectedOnly: selectedOnly }).then(function (r) {
                    return {
                        items: Array.isArray(r.items) ? r.items : [],
                        otherCount: Math.max(0, Number(r.otherCount) || 0)
                    };
                });
            },

            apply: function (seq, items, settings) {
                var result = { applied: 0, already: 0, failed: 0, errors: [], notes: [], missingEffect: false };
                var keys = (Array.isArray(items) ? items : []).map(function (i) { return i ? String(i.key) : ''; })
                    .filter(function (k) { return k.length > 0; });
                if (keys.length === 0) {
                    return Promise.resolve(result);
                }
                var sequenceId = seq ? seq.id : '';
                return call('apply', { sequenceId: sequenceId, keys: keys }).then(function (r) {
                    var applied = Array.isArray(r.applied) ? r.applied : [];
                    result.missingEffect = r.missingEffect === true;
                    result.applied = applied.length;
                    result.already = Math.max(0, Number(r.already) || 0);
                    result.failed = Math.max(0, Number(r.failed) || 0);
                    result.errors = Array.isArray(r.errors) ? r.errors.map(String) : [];
                    if (applied.length === 0 || !wantsParams(settings)) {
                        return result;
                    }
                    // Decide the parameter writes here, where OsvCore lives.
                    var writes = [];
                    applied.forEach(function (a) {
                        var plan = core.planParamWrites(a && a.params, settings);
                        plan.notes.forEach(function (n) {
                            if (result.notes.indexOf(n) === -1) {
                                result.notes.push(n);
                            }
                        });
                        plan.writes.forEach(function (w) {
                            writes.push({ key: String(a.key), index: w.index, name: w.name, value: w.value });
                        });
                    });
                    if (writes.length === 0) {
                        return result;
                    }
                    return call('setParams', { sequenceId: sequenceId, writes: writes }).then(function (w) {
                        if ((Number(w.failed) || 0) > 0) {
                            result.notes.push('params-failed');
                            var detail = Array.isArray(w.errors) && w.errors.length > 0 ? ' (' + String(w.errors[0]) + ')' : '';
                            result.errors.push('the effect is on, but its lens settings could not be set' + detail);
                        }
                        return result;
                    }, function (err) {
                        result.notes.push('params-failed');
                        result.errors.push('the effect is on, but its lens settings could not be set (' + messageOf(err) + ')');
                        return result;
                    });
                });
            },

            checkEffect: function () {
                return call('effectInfo').then(function (r) {
                    return { available: r.available === true ? true : (r.available === false ? false : null) };
                });
            }
        };
    }

    return Object.freeze({ createCepAdapter: createCepAdapter, EVENT_TYPE: EVENT_TYPE, CALL_TIMEOUT_MS: CALL_TIMEOUT_MS });
}));
