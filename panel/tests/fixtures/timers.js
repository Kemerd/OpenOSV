// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// timers.js - a fake clock for the debouncer, the poll and the controller.
//
// advance(ms) runs every timer due in that window in time order, moving the
// clock to each timer's due time before calling it, exactly as a real event
// loop would (minus the microtasks, which a test awaits itself).
'use strict';

function fakeTimers() {
    let now = 0;
    let seq = 0;
    const pending = new Map();   // id -> {at, fn, every}
    const api = {
        setTimeout(fn, ms) {
            const id = ++seq;
            pending.set(id, { at: now + Math.max(0, Number(ms) || 0), fn: fn, every: 0 });
            return id;
        },
        clearTimeout(id) { pending.delete(id); },
        setInterval(fn, ms) {
            const id = ++seq;
            const every = Math.max(1, Number(ms) || 1);
            pending.set(id, { at: now + every, fn: fn, every: every });
            return id;
        },
        clearInterval(id) { pending.delete(id); },
        now() { return now; },
        pendingCount() { return pending.size; },
        advance(ms) {
            const end = now + ms;
            for (;;) {
                let next = null;
                for (const [id, t] of pending) {
                    if (t.at <= end && (next === null || t.at < next[1].at)) {
                        next = [id, t];
                    }
                }
                if (!next) {
                    break;
                }
                const [id, t] = next;
                now = t.at;
                if (t.every > 0) {
                    t.at += t.every;
                } else {
                    pending.delete(id);
                }
                t.fn();
            }
            now = end;
        }
    };
    return api;
}

/** Let every queued promise callback run (several turns deep). */
async function settle(turns) {
    for (let i = 0; i < (turns || 20); i += 1) {
        await new Promise((resolve) => setImmediate(resolve));
    }
}

module.exports = { fakeTimers, settle };
