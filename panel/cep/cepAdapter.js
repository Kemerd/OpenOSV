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

        /**
         * The Manual Framing view of host.jsx's framingInfo answer: the popup
         * numbering it settles, the controls by role, and OsvCore's state.
         */
        function framingFrom(r, hintBase) {
            var params = Array.isArray(r.params) ? r.params : [];
            var roles = core.locateReframeParams(params);
            var byRole = {};
            var values = {};
            Object.keys(roles).forEach(function (role) {
                var p = params.filter(function (x) { return x && x.index === roles[role]; })[0];
                if (p) {
                    byRole[role] = p;
                    values[role] = p.value;
                }
            });
            var readings = [];
            params.forEach(function (p) {
                var count = core.REFRAME_POPUP_COUNTS[p && p.name];
                if (count) {
                    readings.push({ value: p.value, count: count });
                }
            });
            var base = core.learnPopupBase(readings, hintBase);
            return {
                base: base,
                byRole: byRole,
                state: core.framingState(values, base, Number(r.seqWidth) || 0, Number(r.seqHeight) || 0)
            };
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
                        var reason = event && event.data ? String(event.data) : 'host';
                        // [WP-EASING] A selection change is its own kind of event: it
                        // moves the Manual Framing read-outs and nothing else.
                        onEvent(reason === 'onActiveSequenceSelectionChanged' ? 'selection' : reason);
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
            },

            /**
             * What this route can do.  ExtendScript has no transaction: the
             * Scripting Guide's app, Project and ComponentParam pages name no
             * undo group, so each value written is its own History step.
             */
            capabilities: function () {
                return { undoGroups: false, stabilization: true };
            },

            /**
             * [WP-EASING] Set the Keyframe Easing popup of every Open 360
             * Reframe on `items`.  host.jsx reports each instance's controls,
             * OsvCore settles the popup numbering and decides the writes, and
             * host.jsx carries them out, name-checked.
             */
            setEasing: function (seq, items, request) {
                var req = request || {};
                var entry = Math.floor(Number(req.entry));
                var result = { updated: 0, unchanged: 0, missing: 0, failed: 0, errors: [], notes: [], perClipUndo: true,
                               learnedBase: null };
                var keys = (Array.isArray(items) ? items : []).map(function (i) { return i ? String(i.key) : ''; })
                    .filter(function (k) { return k.length > 0; });
                if (!(entry >= 1 && entry <= core.EASINGS.length) || keys.length === 0) {
                    return Promise.resolve(result);
                }
                var sequenceId = seq ? seq.id : '';
                return call('easingTargets', { sequenceId: sequenceId, keys: keys }).then(function (r) {
                    var targets = Array.isArray(r.targets) ? r.targets : [];
                    result.missing = Math.max(0, Number(r.missing) || 0);
                    if ((Number(r.failed) || 0) > 0) {
                        result.failed += Number(r.failed) || 0;
                        result.errors.push('a clip moved before it could be updated');
                    }
                    // ---- the numbering, from every instance's popups -----------------
                    var readings = [];
                    targets.forEach(function (t) {
                        (t.components || []).forEach(function (c) {
                            (c.params || []).forEach(function (p) {
                                var count = core.REFRAME_POPUP_COUNTS[p && p.name];
                                if (count) {
                                    readings.push({ value: p.value, count: count });
                                }
                            });
                        });
                    });
                    var base = core.learnPopupBase(readings, req.popupBase);
                    if (targets.length === 0) {
                        return result;
                    }
                    if (base === null) {
                        result.notes.push('base-unknown');
                        return result;
                    }
                    result.learnedBase = base;
                    var target = core.popupValue(entry, base);
                    // ---- the writes: every instance that differs --------------------
                    var writes = [];
                    var touched = {};
                    targets.forEach(function (t) {
                        var needs = false;
                        var has = false;
                        (t.components || []).forEach(function (c) {
                            (c.params || []).forEach(function (p) {
                                if (p && p.name === core.PARAM_NAMES.keyframeEasing) {
                                    has = true;
                                    if (p.value !== target) {
                                        needs = true;
                                        writes.push({ key: t.key, component: c.ordinal, index: p.index, name: p.name,
                                                      value: target });
                                    }
                                }
                            });
                        });
                        if (!has) {
                            result.failed += 1;
                            result.errors.push('this Open 360 Reframe predates Keyframe Easing. Update the plug-ins');
                        } else if (needs) {
                            touched[t.key] = true;
                        } else {
                            result.unchanged += 1;
                        }
                    });
                    var touchedCount = Object.keys(touched).length;
                    if (writes.length === 0) {
                        return result;
                    }
                    return call('setParams', { sequenceId: sequenceId, writes: writes }).then(function (w) {
                        var failedWrites = Number(w.failed) || 0;
                        if (failedWrites > 0) {
                            result.failed += Math.min(touchedCount, failedWrites);
                            result.updated += Math.max(0, touchedCount - failedWrites);
                            result.errors.push(Array.isArray(w.errors) && w.errors.length > 0 ? String(w.errors[0])
                                                                                               : 'Premiere refused a value');
                        } else {
                            result.updated += touchedCount;
                        }
                        return result;
                    });
                });
            },

            /** [WP-EASING] The selected clip's framing at the playhead. */
            readFraming: function (seq, request) {
                var req = request || {};
                return call('framingInfo', { sequenceId: seq ? seq.id : '' }).then(function (r) {
                    if (r.reason) {
                        return { ok: false, reason: String(r.reason) };
                    }
                    var fc = framingFrom(r, req.popupBase);
                    return Object.assign({ ok: true, name: String(r.name || ''), learnedBase: fc.base }, fc.state);
                });
            },

            /**
             * [WP-EASING] A Manual Framing action on the selected clip.  The
             * writes are OsvCore's (planFramingWrites); keyframed controls get
             * a keyframe at the playhead's component time.
             */
            writeFraming: function (seq, request) {
                var req = request || {};
                var sequenceId = seq ? seq.id : '';
                return call('framingInfo', { sequenceId: sequenceId }).then(function (r) {
                    if (r.reason) {
                        return { ok: false, reason: String(r.reason) };
                    }
                    var fc = framingFrom(r, req.popupBase);
                    var plan = core.planFramingWrites(req, fc.state, fc.base);
                    if (plan.reason) {
                        return { ok: false, reason: plan.reason, label: plan.label };
                    }
                    var writes = [];
                    plan.writes.forEach(function (w) {
                        var p = fc.byRole[w.role];
                        if (p) {
                            writes.push({
                                key: String(r.key),
                                index: p.index,
                                name: p.name,
                                value: w.value,
                                atTicks: w.kind === 'number' ? String(r.componentTicks) : ''
                            });
                        }
                    });
                    if (writes.length === 0) {
                        return { ok: false, reason: 'failed', error: 'the effect\'s controls were not found' };
                    }
                    return call('setParams', { sequenceId: sequenceId, writes: writes }).then(function (w) {
                        var failed = Number(w.failed) || 0;
                        return {
                            ok: failed === 0,
                            reason: failed === 0 ? '' : 'failed',
                            error: failed === 0 ? '' : (Array.isArray(w.errors) && w.errors.length > 0 ? String(w.errors[0]) : ''),
                            label: plan.label,
                            keyframed: (Number(w.keyed) || 0) > 0,
                            learnedBase: fc.base
                        };
                    });
                });
            },

            /**
             * [WP-EASING] Set the Stabilisation of the master clips behind
             * `items` (ProjectItem.videoComponents(), the Source Settings
             * effect there).
             */
            setStabilization: function (seq, items, request) {
                var req = request || {};
                var entry = Math.floor(Number(req.entry));
                var result = { unsupported: false, updated: 0, unchanged: 0, missing: 0, failed: 0, errors: [], notes: [],
                               learnedBase: null };
                var keys = (Array.isArray(items) ? items : []).map(function (i) { return i ? String(i.key) : ''; })
                    .filter(function (k) { return k.length > 0; });
                if (!(entry >= 1 && entry <= 4) || keys.length === 0) {
                    return Promise.resolve(result);
                }
                var sequenceId = seq ? seq.id : '';
                return call('sourceTargets', { sequenceId: sequenceId, keys: keys }).then(function (r) {
                    if (r.unsupported === true) {
                        result.unsupported = true;
                        return result;
                    }
                    var targets = Array.isArray(r.targets) ? r.targets : [];
                    result.missing = Math.max(0, Number(r.missing) || 0);
                    var readings = [];
                    targets.forEach(function (t) {
                        (t.params || []).forEach(function (p) {
                            var count = core.SOURCE_POPUP_COUNTS[p && p.name];
                            if (count) {
                                readings.push({ value: p.value, count: count });
                            }
                        });
                    });
                    if (targets.length === 0) {
                        return result;
                    }
                    var base = core.learnPopupBase(readings, req.popupBase);
                    if (base === null) {
                        result.notes.push('base-unknown');
                        return result;
                    }
                    result.learnedBase = base;
                    var target = core.popupValue(entry, base);
                    var writes = [];
                    targets.forEach(function (t) {
                        var p = (t.params || []).filter(function (x) {
                            return x && x.name === core.SOURCE_PARAM_NAMES.stabilization;
                        })[0];
                        if (!p) {
                            result.missing += 1;
                        } else if (p.value === target) {
                            result.unchanged += 1;
                        } else {
                            writes.push({ key: t.key, index: p.index, name: p.name, value: target });
                        }
                    });
                    if (writes.length === 0) {
                        return result;
                    }
                    return call('setSourceParams', { sequenceId: sequenceId, writes: writes }).then(function (w) {
                        var failed = Number(w.failed) || 0;
                        result.failed += failed;
                        result.updated += Math.max(0, writes.length - failed);
                        if (failed > 0 && Array.isArray(w.errors) && w.errors.length > 0) {
                            result.errors.push(String(w.errors[0]));
                        }
                        return result;
                    });
                });
            }
        };
    }

    return Object.freeze({ createCepAdapter: createCepAdapter, EVENT_TYPE: EVENT_TYPE, CALL_TIMEOUT_MS: CALL_TIMEOUT_MS });
}));
