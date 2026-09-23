/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * controller.js - the auto-apply state machine of the OpenOSV panel.
 *
 * One controller serves both panels.  It talks to Premiere only through an
 * ADAPTER (uxp/uxpAdapter.js or cep/cepAdapter.js), which it is handed, so it
 * runs unchanged under Node against a scripted fake - that is how
 * panel/tests/controller.test.js drives every path below.
 *
 * WHAT "AUTO-APPLY" MEANS HERE
 * ----------------------------
 * The panel applies Open 360 Reframe to OSV clips that are ADDED to a
 * sequence while it is watching, including the first clips of a sequence
 * created while it is watching ("New Sequence From Clip").  It never
 * retro-fits an existing edit: a sequence that already existed is taken as a
 * baseline the first time the panel sees it, and "Apply to all OSV clips in
 * this sequence" is the explicit way to do that.  Together with the move /
 * trim / razor rules in OsvCore.diffItems(), this is what lets a user remove
 * the effect from one clip on purpose and keep it removed.
 *
 * HOW IT NOTICES
 * --------------
 *   - host events (UXP: VideoTrackEvent.TRACK_CHANGED, sequence / project
 *     events; CEP: onActiveSequenceTrackItemAdded and friends) poke a
 *     debouncer, so a multi-clip drop - one event per clip, audio included -
 *     becomes one pass, 400 ms after the last event and never more than 2 s
 *     after the first;
 *   - a cheap signature poll every 2.5 s (track count and clips per track)
 *     catches anything the events miss: a track the listeners were not yet
 *     attached to, another panel that took over an ExtendScript event.
 *
 * Every pass runs on one promise chain, so passes never overlap each other or
 * a button press, and nothing here ever throws into the host: a failure
 * becomes a line in the status bar.
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module !== null && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.OsvController = api;
    }
}(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
    'use strict';

    /** localStorage key; the version suffix lets a future shape start clean. */
    var STORAGE_KEY = 'openosv.panel.settings.v1';

    /** Event debounce: quiet period and the longest a burst may delay a pass. */
    var DEBOUNCE_WAIT_MS = 400;
    var DEBOUNCE_MAX_MS = 2000;

    /** Signature poll period. */
    var POLL_MS = 2500;

    /** How long a settings change waits before it is written to storage. */
    var PERSIST_DELAY_MS = 250;

    /**
     * [WP-EASING] How often the Manual Framing read-outs follow the selected
     * clip and the playhead.  Neither API sends a playhead event, so this is
     * a poll - of reads only, skipped while any action runs.
     */
    var FRAMING_POLL_MS = 1000;

    /** Text of an Error, a string, or anything else. */
    function messageOf(err) {
        if (err && typeof err.message === 'string' && err.message.length > 0) {
            return err.message;
        }
        if (typeof err === 'string' && err.length > 0) {
            return err;
        }
        return 'unknown error';
    }

    /**
     * Build a controller.
     *
     * @param {object} deps
     *   core     - OsvCore
     *   adapter  - the host adapter (see the two adapters for the contract)
     *   timers   - { setTimeout, clearTimeout, setInterval, clearInterval, now }
     *   storage  - { get(key) -> string|null, set(key, text) }  (may throw)
     *   onChange - called with a state snapshot whenever something changes
     */
    function createController(deps) {
        var d = deps || {};
        var core = d.core;
        var adapter = d.adapter;
        if (!core || !adapter) {
            throw new Error('OsvController needs core and adapter');
        }
        var timers = d.timers || {};
        var setT = typeof timers.setTimeout === 'function' ? timers.setTimeout : setTimeout;
        var clearT = typeof timers.clearTimeout === 'function' ? timers.clearTimeout : clearTimeout;
        var setI = typeof timers.setInterval === 'function' ? timers.setInterval : setInterval;
        var clearI = typeof timers.clearInterval === 'function' ? timers.clearInterval : clearInterval;
        var now = typeof timers.now === 'function' ? timers.now : function () { return Date.now(); };
        var storage = d.storage || null;
        var onChange = typeof d.onChange === 'function' ? d.onChange : function () {};

        // ---- state shown by the view --------------------------------------
        var state = {
            settings: core.sanitizeSettings(null),
            status: { tone: 'info', text: 'Starting...', at: 0, stamped: false },
            busy: false,
            busyLabel: '',
            // [WP-EASING] Which card's action is running ('' / 'apply' /
            // 'easing' / 'framing' / 'stabilization'), so only its buttons
            // show the progress.
            busyAction: '',
            host: 'starting',
            // [WP-EASING] What this route can do (grouped undo, Source Settings).
            capabilities: { undoGroups: true, stabilization: true },
            // [WP-EASING] The Manual Framing read-outs: the selected clip's
            // framing at the playhead, or why there is none.
            framing: { ok: false, reason: 'no-selection' }
        };

        // ---- bookkeeping ---------------------------------------------------
        var started = false;
        var stopped = false;
        var queue = Promise.resolve();
        var pollHandle = null;
        var polling = false;
        var lastSignature = null;
        var persistHandle = null;
        // [WP-EASING] The framing poll, and how many queued tasks are running
        // (the poll never reads while one is).
        var framingHandle = null;
        var framingReading = false;
        var pending = 0;

        // Which sequences existed when the panel started looking at the
        // current project.  `ids === null` means the host could not say, and
        // then every unseen sequence is treated as EXISTING - the safe
        // direction, because treating an existing edit as new would put the
        // effect on every OSV clip in it.
        var seqIndex = { projectId: null, ids: null, synced: false };
        // Sequence id -> the OSV items seen at the last pass.
        var knownItems = new Map();

        // ---- helpers ---------------------------------------------------------

        function snapshot() {
            return {
                settings: Object.assign({}, state.settings),
                status: Object.assign({}, state.status),
                busy: state.busy,
                busyLabel: state.busyLabel,
                busyAction: state.busyAction,
                host: state.host,
                capabilities: Object.assign({}, state.capabilities),
                framing: Object.assign({}, state.framing)
            };
        }

        function emit() {
            try {
                onChange(snapshot());
            } catch (err) {
                // The view is not allowed to break the controller.
            }
        }

        function setStatus(tone, text, stamped) {
            state.status = { tone: tone, text: String(text), at: now(), stamped: stamped === true };
            emit();
        }

        /** The resting line for the current settings. */
        function idleStatus() {
            if (state.host === 'error') {
                return;
            }
            if (state.settings.autoApply) {
                setStatus('info', 'Watching the timeline.', false);
            } else {
                setStatus('info', 'Auto-apply is off.', false);
            }
        }

        /** Run `task` after everything already queued; failures become status. */
        function enqueue(task) {
            pending += 1;
            queue = queue.then(function () {
                if (stopped) {
                    return undefined;
                }
                return task();
            }).catch(function (err) {
                setStatus('error', 'Something went wrong: ' + core.shortError(messageOf(err)) + '.', true);
            }).then(function () {
                pending = Math.max(0, pending - 1);
            });
            return queue;
        }

        function loadSettings() {
            if (!storage || typeof storage.get !== 'function') {
                return core.sanitizeSettings(null);
            }
            try {
                return core.parseSettings(storage.get(STORAGE_KEY));
            } catch (err) {
                return core.sanitizeSettings(null);
            }
        }

        function persistSoon() {
            if (!storage || typeof storage.set !== 'function') {
                return;
            }
            if (persistHandle !== null) {
                clearT(persistHandle);
            }
            persistHandle = setT(function () {
                persistHandle = null;
                try {
                    storage.set(STORAGE_KEY, core.serializeSettings(state.settings));
                } catch (err) {
                    // Storage full or unavailable: the setting still holds for this session.
                }
            }, PERSIST_DELAY_MS);
        }

        /** True when a sequence id counts as one that already existed. */
        function isExistingSequence(id) {
            return seqIndex.ids === null ? true : seqIndex.ids.has(id);
        }

        /** Refresh the sequence index when the project changed (or on demand). */
        function syncProject(seq, force) {
            var projectChanged = seq.projectId !== seqIndex.projectId;
            if (!projectChanged && seqIndex.synced && force !== true) {
                return Promise.resolve();
            }
            return Promise.resolve()
                .then(function () { return adapter.getSequenceIds(); })
                .then(function (ids) {
                    seqIndex.ids = Array.isArray(ids) ? new Set(ids.map(String)) : null;
                }, function () {
                    seqIndex.ids = null;
                })
                .then(function () {
                    if (projectChanged) {
                        knownItems.clear();
                    }
                    seqIndex.projectId = seq.projectId;
                    seqIndex.synced = true;
                });
        }

        /** Scan a sequence, defensively shaped. */
        function scan(seq, selectedOnly) {
            return Promise.resolve(adapter.scan(seq, { selectedOnly: selectedOnly === true })).then(function (r) {
                var result = (r !== null && typeof r === 'object') ? r : {};
                return {
                    items: Array.isArray(result.items) ? result.items : [],
                    otherCount: Math.max(0, Number(result.otherCount) || 0)
                };
            });
        }

        // ---- the automatic pass -------------------------------------------------

        /**
         * One look at the active sequence.  With `baselineOnly` the current
         * clips are recorded as known and nothing is applied (start-up, and
         * the moment auto-apply is switched on).
         */
        function autoPass(baselineOnly) {
            if (!baselineOnly && !state.settings.autoApply) {
                return Promise.resolve();
            }
            var seq = null;
            return Promise.resolve(adapter.getActiveSequence())
                .then(function (s) {
                    seq = (s && typeof s === 'object' && s.id) ? s : null;
                    if (!seq) {
                        return null;
                    }
                    return syncProject(seq, false).then(function () { return scan(seq, false); });
                })
                .then(function (scanned) {
                    if (!seq || !scanned) {
                        return undefined;
                    }
                    var items = scanned.items;
                    var known = knownItems.get(seq.id);
                    if (known === undefined) {
                        if (baselineOnly || isExistingSequence(seq.id)) {
                            // First sight of an existing edit: remember, do not touch.
                            knownItems.set(seq.id, items);
                            if (seqIndex.ids !== null) {
                                seqIndex.ids.add(seq.id);
                            }
                            return undefined;
                        }
                        // A sequence created while watching: its clips are new.
                        known = [];
                        if (seqIndex.ids !== null) {
                            seqIndex.ids.add(seq.id);
                        }
                    }
                    if (baselineOnly) {
                        knownItems.set(seq.id, items);
                        return undefined;
                    }
                    var diff = core.diffItems(known, items);
                    // Remember the new state BEFORE applying: a clip that
                    // fails is not retried in a loop - the buttons retry.
                    knownItems.set(seq.id, items);
                    if (diff.added.length === 0) {
                        return undefined;
                    }
                    return Promise.resolve(adapter.apply(seq, diff.added, state.settings)).then(function (result) {
                        var summary = core.summarize(result, 'auto');
                        if (!summary.quiet) {
                            setStatus(summary.tone, summary.text, true);
                        }
                    });
                })
                .catch(function (err) {
                    // The user switched sequence while this pass was looking:
                    // harmless, and the switch itself triggers the next pass.
                    // Only a button press reports it.  Anything else is real.
                    if (/active sequence changed/i.test(messageOf(err))) {
                        return undefined;
                    }
                    throw err;
                });
        }

        var debouncer = core.createDebouncer(function () {
            enqueue(function () { return autoPass(false); });
        }, {
            waitMs: DEBOUNCE_WAIT_MS,
            maxWaitMs: DEBOUNCE_MAX_MS,
            onError: function (err) { setStatus('error', core.shortError(messageOf(err)), true); }
        }, { setTimeout: setT, clearTimeout: clearT, now: now });

        /**
         * A host event: something on the timeline may have changed.  A
         * selection change only moves the Manual Framing read-outs to another
         * clip, so it refreshes them and leaves auto-apply alone.
         */
        function onHostEvent(reason) {
            if (stopped) {
                return;
            }
            if (reason === 'selection') {
                refreshFraming();
                return;
            }
            if (state.settings.autoApply) {
                debouncer.trigger();
            }
        }

        /** The safety-net poll. */
        function pollOnce() {
            if (stopped || polling || !state.settings.autoApply) {
                return;
            }
            polling = true;
            Promise.resolve()
                .then(function () { return adapter.signature(); })
                .then(function (sig) {
                    if (typeof sig !== 'string') {
                        return;
                    }
                    var changed = lastSignature !== null && sig !== lastSignature;
                    lastSignature = sig;
                    if (changed) {
                        debouncer.trigger();
                    }
                }, function () {
                    // A failed poll is simply skipped; the next one tries again.
                })
                .then(function () { polling = false; }, function () { polling = false; });
        }

        function startPolling() {
            if (pollHandle === null) {
                pollHandle = setI(pollOnce, POLL_MS);
            }
        }

        function stopPolling() {
            if (pollHandle !== null) {
                clearI(pollHandle);
                pollHandle = null;
            }
            lastSignature = null;
        }

        // ---- manual actions -----------------------------------------------------

        /** Shared body of the two buttons. */
        function manualApply(selectedOnly) {
            if (state.busy) {
                return queue;
            }
            state.busy = true;
            state.busyAction = 'apply';
            state.busyLabel = selectedOnly ? 'Applying to selection...' : 'Applying to sequence...';
            emit();
            return enqueue(function () {
                var seq = null;
                return Promise.resolve(adapter.getActiveSequence())
                    .then(function (s) {
                        seq = (s && typeof s === 'object' && s.id) ? s : null;
                        if (!seq) {
                            setStatus('info', 'Open a sequence first.', true);
                            return null;
                        }
                        return syncProject(seq, false).then(function () { return scan(seq, selectedOnly); });
                    })
                    .then(function (scanned) {
                        if (!seq || !scanned) {
                            return undefined;
                        }
                        var context = selectedOnly ? 'selected' : 'all';
                        if (scanned.items.length === 0) {
                            var none = core.summarize({ notOsv: scanned.otherCount }, context);
                            setStatus(none.tone, none.text, true);
                            return undefined;
                        }
                        return Promise.resolve(adapter.apply(seq, scanned.items, state.settings)).then(function (result) {
                            var r = (result !== null && typeof result === 'object') ? result : {};
                            r.notOsv = scanned.otherCount;
                            var summary = core.summarize(r, context);
                            setStatus(summary.tone, summary.text, true);
                        });
                    });
            }).then(function () {
                state.busy = false;
                state.busyAction = '';
                state.busyLabel = '';
                emit();
            });
        }

        // ---- [WP-EASING] Keyframe Animation, Manual Framing, Stabilisation ------------------

        /**
         * Keep what an adapter learned about the host's popup numbering, so
         * the next action (and the next session) does not have to learn it
         * again.  Only a settled 0 or 1 is ever stored.
         */
        function rememberBase(result) {
            var b = result && result.learnedBase;
            if ((b === 0 || b === 1) && b !== state.settings.popupBase) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { popupBase: b }));
                persistSoon();
            }
        }

        /** An adapter method, or a stand-in that says the route cannot do it. */
        function adapterCall(name) {
            return typeof adapter[name] === 'function'
                ? adapter[name].bind(adapter)
                : function () { return Promise.reject(new Error('this panel build cannot do that')); };
        }

        /**
         * One button press: busy while it runs (only `action`'s buttons show
         * the label), queued behind everything else, never overlapping.
         */
        function runAction(action, label, task) {
            if (state.busy) {
                return queue;
            }
            state.busy = true;
            state.busyAction = action;
            state.busyLabel = label;
            emit();
            return enqueue(task).then(function () {
                state.busy = false;
                state.busyAction = '';
                state.busyLabel = '';
                emit();
            });
        }

        /** Apply the Keyframe Animation preset picked in the grid. */
        function applyEasing(selectedOnly) {
            var easing = core.easingById(state.settings.easing);
            return runAction('easing', selectedOnly ? 'Setting on selection...' : 'Setting on sequence...', function () {
                var seq = null;
                return Promise.resolve(adapter.getActiveSequence())
                    .then(function (s) {
                        seq = (s && typeof s === 'object' && s.id) ? s : null;
                        if (!seq) {
                            setStatus('info', 'Open a sequence first.', true);
                            return null;
                        }
                        return syncProject(seq, false).then(function () { return scan(seq, selectedOnly); });
                    })
                    .then(function (scanned) {
                        if (!seq || !scanned) {
                            return undefined;
                        }
                        var context = selectedOnly ? 'selected' : 'all';
                        if (scanned.items.length === 0) {
                            var none = core.summarizeEasing({}, easing.label, context, scanned.otherCount);
                            setStatus(none.tone, none.text, true);
                            return undefined;
                        }
                        return Promise.resolve(adapterCall('setEasing')(seq, scanned.items, {
                            entry: easing.entry,
                            popupBase: state.settings.popupBase
                        })).then(function (result) {
                            rememberBase(result);
                            var summary = core.summarizeEasing(result, easing.label, context, scanned.otherCount);
                            setStatus(summary.tone, summary.text, true);
                        });
                    });
            });
        }

        /**
         * Set the Stabilisation the two switches spell (RockSteady, Horizon
         * Leveling) on the selected clips' master clips.
         */
        function applyStabilization() {
            var choice = core.stabilizationChoice(state.settings.rockSteady, state.settings.horizonLeveling);
            return runAction('stabilization', 'Setting stabilisation...', function () {
                var seq = null;
                return Promise.resolve(adapter.getActiveSequence())
                    .then(function (s) {
                        seq = (s && typeof s === 'object' && s.id) ? s : null;
                        if (!seq) {
                            setStatus('info', 'Open a sequence first.', true);
                            return null;
                        }
                        return syncProject(seq, false).then(function () { return scan(seq, true); });
                    })
                    .then(function (scanned) {
                        if (!seq || !scanned) {
                            return undefined;
                        }
                        if (scanned.items.length === 0) {
                            var none = core.summarizeStabilization({}, choice.label, scanned.otherCount);
                            setStatus(none.tone, none.text, true);
                            return undefined;
                        }
                        return Promise.resolve(adapterCall('setStabilization')(seq, scanned.items, {
                            entry: choice.entry,
                            popupBase: state.settings.popupBase
                        })).then(function (result) {
                            rememberBase(result);
                            var summary = core.summarizeStabilization(result, choice.label, scanned.otherCount);
                            setStatus(summary.tone, summary.text, true);
                        });
                    });
            });
        }

        /** A Manual Framing action on the selected clip, then fresh read-outs. */
        function framingAction(request, label) {
            return runAction('framing', label, function () {
                return Promise.resolve(adapter.getActiveSequence()).then(function (s) {
                    var seq = (s && typeof s === 'object' && s.id) ? s : null;
                    if (!seq) {
                        var none = core.summarizeFraming({ ok: false, reason: 'no-sequence' });
                        setStatus(none.tone, none.text, true);
                        return undefined;
                    }
                    var req = Object.assign({ popupBase: state.settings.popupBase }, request);
                    return Promise.resolve(adapterCall('writeFraming')(seq, req)).then(function (result) {
                        rememberBase(result);
                        var summary = core.summarizeFraming(result);
                        setStatus(summary.tone, summary.text, true);
                        return readFramingNow(seq);
                    });
                });
            });
        }

        /** Read the framing of `seq`'s selected clip into the state. */
        function readFramingNow(seq) {
            if (!seq) {
                return Promise.resolve(setFraming({ ok: false, reason: 'no-sequence' }));
            }
            return Promise.resolve(adapterCall('readFraming')(seq, { popupBase: state.settings.popupBase }))
                .then(function (r) {
                    rememberBase(r);
                    setFraming(r);
                }, function (err) {
                    // A sequence switch mid-read is not worth a status line.
                    setFraming({ ok: false, reason: /active sequence changed/i.test(messageOf(err)) ? 'no-sequence' : 'unknown' });
                });
        }

        /** Store new read-outs; the view is only told when they changed. */
        function setFraming(r) {
            var f = (r !== null && typeof r === 'object') ? r : { ok: false, reason: 'unknown' };
            var next = f.ok === true ? {
                ok: true,
                name: String(f.name || ''),
                lens: f.lens === core.LENS.classic ? core.LENS.classic : core.LENS.dji,
                zoom: f.zoom,
                fov: f.lens === core.LENS.classic ? f.fov : f.djiFov,
                correction: f.correction,
                distortion: f.distortion,
                pan: f.pan,
                tilt: f.tilt,
                roll: f.roll
            } : { ok: false, reason: String(f.reason || 'unknown') };
            if (JSON.stringify(next) !== JSON.stringify(state.framing)) {
                state.framing = next;
                emit();
            }
        }

        /**
         * Refresh the read-outs now, unless something is running (a queued
         * action refreshes them itself when it is done) or a read already is.
         */
        function refreshFraming() {
            if (stopped || framingReading || pending > 0 || state.host !== 'ready') {
                return Promise.resolve();
            }
            framingReading = true;
            return Promise.resolve()
                .then(function () { return adapter.getActiveSequence(); })
                .then(function (s) {
                    return readFramingNow((s && typeof s === 'object' && s.id) ? s : null);
                })
                .then(function () { framingReading = false; }, function () { framingReading = false; });
        }

        function startFramingPoll() {
            if (framingHandle === null) {
                framingHandle = setI(function () { refreshFraming(); }, FRAMING_POLL_MS);
            }
        }

        function stopFramingPoll() {
            if (framingHandle !== null) {
                clearI(framingHandle);
                framingHandle = null;
            }
        }

        // ---- public API -------------------------------------------------------------

        return {
            /** Load settings, attach to the host, take the baseline. */
            start: function () {
                if (started) {
                    return queue;
                }
                started = true;
                state.settings = loadSettings();
                emit();
                var attached = true;
                try {
                    adapter.init(onHostEvent);
                } catch (err) {
                    attached = false;
                    state.host = 'error';
                    setStatus('error', 'Can\'t reach Premiere: ' + core.shortError(messageOf(err)) + '.', true);
                }
                if (!attached) {
                    return queue;
                }
                state.host = 'ready';
                // [WP-EASING] What this route can do decides the notes the
                // cards show (grouped undo, a reachable Source Settings).
                try {
                    if (typeof adapter.capabilities === 'function') {
                        var caps = adapter.capabilities() || {};
                        state.capabilities = {
                            undoGroups: caps.undoGroups !== false,
                            stabilization: caps.stabilization !== false
                        };
                    }
                } catch (err) {
                    // The defaults stay.
                }
                idleStatus();
                // Take the baseline first, then tell the user if the effect is missing.
                enqueue(function () { return autoPass(true); });
                enqueue(function () {
                    return Promise.resolve(adapter.checkEffect()).then(function (info) {
                        if (info && info.available === false) {
                            var s = core.summarize({ missingEffect: true }, 'auto');
                            setStatus(s.tone, s.text, false);
                        }
                    }, function () {
                        // Unknown is not an error; the first apply will say.
                    });
                });
                if (state.settings.autoApply) {
                    startPolling();
                }
                startFramingPoll();
                return queue;
            },

            /** Detach from the host and stop every timer. */
            stop: function () {
                stopped = true;
                stopPolling();
                stopFramingPoll();
                debouncer.cancel();
                if (persistHandle !== null) {
                    clearT(persistHandle);
                    persistHandle = null;
                }
                try {
                    adapter.dispose();
                } catch (err) {
                    // Nothing left to report to.
                }
            },

            /** The switch. */
            setAutoApply: function (on) {
                var value = on === true;
                if (value === state.settings.autoApply) {
                    return queue;
                }
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { autoApply: value }));
                persistSoon();
                debouncer.cancel();
                // Anything seen while it was on is stale once it is off, and
                // everything that exists when it comes back on is the new
                // baseline - switching on never touches clips already there.
                knownItems.clear();
                seqIndex.synced = false;
                if (value) {
                    idleStatus();
                    startPolling();
                    return enqueue(function () { return autoPass(true); });
                }
                stopPolling();
                idleStatus();
                return queue;
            },

            setLens: function (lens) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { lens: lens }));
                persistSoon();
                emit();
            },

            setDragEnabled: function (on) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { dragEnabled: on === true }));
                persistSoon();
                emit();
            },

            setDragSensitivity: function (value) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { dragSensitivity: value }));
                persistSoon();
                emit();
            },

            applySelected: function () { return manualApply(true); },
            applyAll: function () { return manualApply(false); },

            // ---- [WP-EASING] --------------------------------------------------------

            /** Pick a Keyframe Animation preset in the grid (remembered). */
            setEasing: function (id) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { easing: id }));
                persistSoon();
                emit();
            },
            applyEasingSelected: function () { return applyEasing(true); },
            applyEasingAll: function () { return applyEasing(false); },

            /** The Stabilisation card's RockSteady switch (remembered). */
            setRockSteady: function (on) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { rockSteady: on === true }));
                persistSoon();
                emit();
            },
            /** The Stabilisation card's Horizon Leveling switch (remembered). */
            setHorizonLeveling: function (on) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings,
                                                                     { horizonLeveling: on === true }));
                persistSoon();
                emit();
            },
            applyStabilization: function () { return applyStabilization(); },

            /** A Manual Framing preset button. */
            framingPreset: function (id) {
                var p = core.framingPresetById(id);
                return p ? framingAction({ action: 'preset', preset: id }, p.label + '...') : queue;
            },
            /** A zoom button: +1 widens (Zoom up), -1 narrows. */
            zoomStep: function (direction) {
                return framingAction({ action: 'zoom', direction: direction > 0 ? 1 : -1 }, 'Zooming...');
            },
            /** Read the framing read-outs now. */
            refreshFraming: function () { return refreshFraming(); },

            /** The Program Monitor controls card was opened or closed (remembered). */
            setHintOpen: function (open) {
                state.settings = core.sanitizeSettings(Object.assign({}, state.settings, { hintOpen: open === true }));
                persistSoon();
                emit();
            },

            /** For the tests and the view's first paint. */
            getState: snapshot,
            /** Resolves when everything queued so far has run. */
            idle: function () { return queue; },
            /** Test hooks: run the debounced / polled work now. */
            _flush: function () { debouncer.flush(); return queue; },
            _poll: pollOnce,
            STORAGE_KEY: STORAGE_KEY
        };
    }

    return Object.freeze({
        createController: createController,
        STORAGE_KEY: STORAGE_KEY,
        DEBOUNCE_WAIT_MS: DEBOUNCE_WAIT_MS,
        DEBOUNCE_MAX_MS: DEBOUNCE_MAX_MS,
        POLL_MS: POLL_MS,
        FRAMING_POLL_MS: FRAMING_POLL_MS
    });
}));
