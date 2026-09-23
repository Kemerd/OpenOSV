// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeEasing.cpp - the Keyframe Easing curves and the keyframe walk
// (declared, and explained, in ReframeEasing.h).
//
// Nothing here names an Adobe type: the CPU and GPU readers each wrap their
// host's keyframe API in a KeyframeTrack, and the unit tests drive the same
// functions with synthetic keyframes.

#include "ReframeEasing.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace osv::reframe {

namespace {

/// `u` made safe for the polynomials: clamped to [0, 1] (so -inf is the
/// start and +inf the end), and NaN - which has no position at all - is the
/// start.  Every curve below is only defined on the unit interval, and a
/// value outside it would extrapolate the polynomial into a picture nobody
/// keyed.
[[nodiscard]] double unitInterval(double u) noexcept {
    if (std::isnan(u)) {
        return 0.0;
    }
    return std::clamp(u, 0.0, 1.0);
}

/// Smoothstep, 3u^2 - 2u^3: the Slow In / Slow Out curve, and the building
/// block of the two asymmetric presets' speed profiles.
[[nodiscard]] double smoothstep(double u) noexcept { return u * u * (3.0 - 2.0 * u); }

/// True for a time value an adapter may hand us: finite.  A NaN key time
/// would poison every comparison below silently (NaN compares false), so it
/// is treated as "no keyframe".
[[nodiscard]] bool finiteTime(const std::optional<double>& t) noexcept { return t.has_value() && std::isfinite(*t); }

}  // namespace

// ---------------------------------------------------------------------------
//  The curves
// ---------------------------------------------------------------------------

double easeProgress(KeyframeEasing easing, double u) noexcept {
    const double x = unitInterval(u);
    switch (easing) {
        case KeyframeEasing::SlowInSlowOut:
            // Speed 6u(1-u): zero at both keyframes, 1.5 halfway.
            return smoothstep(x);
        case KeyframeEasing::FastInFastOut:
            // 2u - smoothstep(u): speed 2 - 6u(1-u), i.e. 2 at the keyframes
            // and 0.5 halfway - quick out of a keyframe, slowest mid-move,
            // quick into the next one, never stopping.
            return x * (2.0 - 3.0 * x + 2.0 * x * x);
        case KeyframeEasing::FastInSlowOut:
            // Integral of 2 (1 - smoothstep): 2u - 2u^3 + u^4.  Leaves at
            // twice the straight line's speed and settles into the next
            // keyframe at zero speed, with the speed curve flat at both ends.
            return x * (2.0 - x * x * (2.0 - x));
        case KeyframeEasing::SlowInFastOut:
            // Integral of 2 smoothstep: 2u^3 - u^4.  The mirror image of the
            // preset above: SlowInFastOut(u) == 1 - FastInSlowOut(1 - u).
            return x * x * x * (2.0 - x);
        case KeyframeEasing::None:
        case KeyframeEasing::LinearSmooth:
        case KeyframeEasing::Linear:
        default:
            // Linear: constant speed.  None never reaches the curve (the
            // readers keep the host's value) and Linear Smooth eases in the
            // value domain, so both are the identity here.
            return x;
    }
}

double easeSpeed(KeyframeEasing easing, double u) noexcept {
    const double x = unitInterval(u);
    switch (easing) {
        case KeyframeEasing::SlowInSlowOut:
            return 6.0 * x * (1.0 - x);
        case KeyframeEasing::FastInFastOut:
            return 2.0 - 6.0 * x * (1.0 - x);
        case KeyframeEasing::FastInSlowOut:
            return 2.0 * (1.0 - smoothstep(x));
        case KeyframeEasing::SlowInFastOut:
            return 2.0 * smoothstep(x);
        case KeyframeEasing::None:
        case KeyframeEasing::LinearSmooth:
        case KeyframeEasing::Linear:
        default:
            return 1.0;
    }
}

double hermiteValue(double k0, double v0, double m0, double k1, double v1, double m1, double t) noexcept {
    // Every input is host-derived; any non-finite one, or an interval with no
    // length, answers with the start value rather than a NaN that would reach
    // the camera.
    if (!std::isfinite(k0) || !std::isfinite(v0) || !std::isfinite(m0) || !std::isfinite(k1) ||
        !std::isfinite(v1) || !std::isfinite(m1) || !std::isfinite(t) || !(k1 > k0)) {
        return std::isfinite(v0) ? v0 : 0.0;
    }
    const double span = k1 - k0;
    const double u = std::clamp((t - k0) / span, 0.0, 1.0);
    const double u2 = u * u;
    const double u3 = u2 * u;
    // The four cubic Hermite basis functions.
    const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 = u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 = u3 - u2;
    // Tangents are value-per-time, so they are scaled by the interval length.
    const double value = h00 * v0 + h10 * span * m0 + h01 * v1 + h11 * span * m1;
    return std::isfinite(value) ? value : v0;
}

// ---------------------------------------------------------------------------
//  One control's eased value
// ---------------------------------------------------------------------------

std::optional<double> easedValue(KeyframeEasing easing, KeyframeTrack& track, double t) noexcept {
    try {
        if (easing == KeyframeEasing::None || !std::isfinite(t)) {
            return std::nullopt;  // Premiere's own interpolation stays
        }

        // ---- the interval around t --------------------------------------
        // k0 <= t < k1.  Missing either side means t is before the first or
        // after the last keyframe (or the control is not animated), where
        // Premiere holds the end value - nothing to ease.
        const std::optional<double> k0 = track.keyAtOrBefore(t);
        const std::optional<double> k1 = track.keyAfter(t);
        if (!finiteTime(k0) || !finiteTime(k1) || !(*k1 > *k0) || *k0 > t) {
            return std::nullopt;
        }
        const std::optional<double> v0 = track.valueAt(*k0);
        const std::optional<double> v1 = track.valueAt(*k1);
        if (!v0 || !v1 || !std::isfinite(*v0) || !std::isfinite(*v1)) {
            return std::nullopt;
        }
        const double span = *k1 - *k0;
        const double u = (t - *k0) / span;

        // ---- the time curves ---------------------------------------------
        if (easing != KeyframeEasing::LinearSmooth) {
            const double eased = *v0 + (*v1 - *v0) * easeProgress(easing, u);
            return std::isfinite(eased) ? std::optional<double>(eased) : std::nullopt;
        }

        // ---- Linear Smooth: a Hermite segment with averaged tangents -----
        // The interval's own straight-line slope, and the neighbours' where
        // they exist.  A missing neighbour (the first or last interval) makes
        // that end's tangent the interval's own slope, so a two-keyframe move
        // is exactly linear.
        const double slope = (*v1 - *v0) / span;
        double m0 = slope;
        double m1 = slope;
        const std::optional<double> kPrev = track.keyBefore(*k0);
        if (finiteTime(kPrev) && *kPrev < *k0) {
            const std::optional<double> vPrev = track.valueAt(*kPrev);
            if (vPrev && std::isfinite(*vPrev)) {
                m0 = 0.5 * ((*v0 - *vPrev) / (*k0 - *kPrev) + slope);
            }
        }
        const std::optional<double> kNext = track.keyAfter(*k1);
        if (finiteTime(kNext) && *kNext > *k1) {
            const std::optional<double> vNext = track.valueAt(*kNext);
            if (vNext && std::isfinite(*vNext)) {
                m1 = 0.5 * (slope + (*vNext - *v1) / (*kNext - *k1));
            }
        }
        return hermiteValue(*k0, *v0, m0, *k1, *v1, m1, t);
    } catch (...) {
        // An adapter that threw (it should not) leaves the host's value.
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
//  The backwards search over a forwards-only host query
// ---------------------------------------------------------------------------

std::optional<std::int64_t> keyAtOrBeforeFromNext(
    const std::function<std::optional<std::int64_t>(std::int64_t)>& nextAfter, std::int64_t t, std::int64_t earliest,
    std::int64_t firstStep, int maxCalls) noexcept {
    try {
        if (!nextAfter || maxCalls <= 0 || earliest >= t) {
            return std::nullopt;
        }
        // Keep t - earliest representable: a starting point more than half
        // the int64 range behind t is moved up to that bound.  No host holds
        // keyframes 2^62 ticks (about 58 years) apart, so nothing is lost.
        constexpr std::int64_t kMaxSpan = std::numeric_limits<std::int64_t>::max() / 2;
        if (t > 0 && earliest < t - kMaxSpan) {
            earliest = t - kMaxSpan;
        } else if (t <= 0 && earliest < std::numeric_limits<std::int64_t>::min() / 2) {
            earliest = std::numeric_limits<std::int64_t>::min() / 2;
        }
        int calls = 0;
        // One counted host call; a host that answers with a time at or before
        // the one asked about is answering out of order, and a walk built on
        // that could loop forever - so it ends the search.
        auto next = [&](std::int64_t after, bool* ok) -> std::optional<std::int64_t> {
            *ok = (++calls <= maxCalls);
            if (!*ok) {
                return std::nullopt;
            }
            const std::optional<std::int64_t> key = nextAfter(after);
            if (key && *key <= after) {
                *ok = false;
                return std::nullopt;
            }
            return key;
        };
        bool ok = true;

        // The first keyframe of all.  None, or one after t: nothing at or
        // before t.
        const std::optional<std::int64_t> first = next(earliest, &ok);
        if (!ok || !first || *first > t) {
            return std::nullopt;
        }

        // ---- probe backwards with a doubling window ----------------------
        // A probe from `from` answers "the first keyframe after from"; when
        // that lands at or before t, the window (from, t] holds a keyframe and
        // the forward walk below finds the last one.  Otherwise the window is
        // empty and doubles.  Once the window reaches back past the first
        // keyframe, that keyframe is the walk's start.
        std::int64_t step = std::max<std::int64_t>(firstStep, 1);
        std::int64_t candidate = *first;
        for (;;) {
            // t - step, guarded against running below `earliest` (and so
            // against overflow for a huge step).
            const bool pastFirst = (t - earliest) <= step || (t - step) < *first;
            if (pastFirst) {
                candidate = *first;
                break;
            }
            const std::optional<std::int64_t> probe = next(t - step, &ok);
            if (!ok) {
                return std::nullopt;
            }
            if (probe && *probe <= t) {
                candidate = *probe;
                break;
            }
            // No keyframe in (t - step, t]: look twice as far back.
            step = (step > std::numeric_limits<std::int64_t>::max() / 2) ? std::numeric_limits<std::int64_t>::max()
                                                                          : step * 2;
        }

        // ---- walk forwards to the last keyframe at or before t ------------
        for (;;) {
            const std::optional<std::int64_t> after = next(candidate, &ok);
            if (!ok) {
                return std::nullopt;
            }
            if (!after || *after > t) {
                return candidate;
            }
            candidate = *after;
        }
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace osv::reframe
