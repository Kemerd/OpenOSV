// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeEasing.h - the Keyframe Easing presets of Open 360 Reframe.
//
// DJI Studio's Keyframe Animation section offers seven ways for the view to
// travel between two keyframes: None, Linear Smooth, Fast In / Slow Out,
// Slow In / Fast Out, Fast In / Fast Out, Slow In / Slow Out and Linear, with
// an "Apply to all" button.  Premiere cannot express any of them: its
// scripting APIs set a keyframe's interpolation TYPE (linear, hold, Bezier),
// never an arbitrary curve.  So the effect draws the curve itself - the
// "Keyframe Easing" popup (ReframeParams.h, id 22) picks one, and the readers
// of the host's parameters (EffectMain.cpp on the CPU, GpuFilter.cpp on the
// GPU and, through the same Settings, the direct path) replace the host's
// in-between value of each camera control with the curve's.
//
// WHAT THIS FILE HOLDS, AND WHAT IT DOES NOT
// ------------------------------------------
// The maths and the keyframe walk, with no Adobe type anywhere, so the unit
// tests compile the very source the .aex contains and the CPU and GPU paths
// cannot drift apart: both hand this file an adapter over their own host API
// (KeyframeTrack below) and get back the same number.
//
// THE CURVES
// ----------
// Each preset is defined by its SPEED PROFILE across one keyframe interval -
// how fast the value moves at every instant, relative to the average speed
// of a straight line between the two keyframes.  DJI Studio's own preset
// icons draw exactly that: a speed graph between two keyframe dots (a flat
// line for Linear, a bell for Slow In / Slow Out, a valley for Fast In /
// Fast Out, an S-shaped fall or rise for the two asymmetric presets, and a
// tilde for Linear Smooth).  The formulas below reproduce those shapes with
// the simplest smooth polynomials; with u in [0, 1] the fraction of the
// interval travelled and s(u) the fraction of the value change completed:
//
//   Linear              s = u                     speed 1 throughout
//   Slow In, Slow Out   s = 3u^2 - 2u^3           speed 6u(1-u): 0 at both keys
//                                                 (smoothstep)
//   Fast In, Fast Out   s = 2u - 3u^2 + 2u^3      speed 2 - 6u(1-u): 2 at the
//                                                 keys, 1/2 halfway
//   Fast In, Slow Out   s = 2u - 2u^3 + u^4       speed 2(1 - smoothstep(u)):
//                                                 2 -> 0, flat at both ends
//   Slow In, Fast Out   s = 2u^3 - u^4            speed 2 smoothstep(u):
//                                                 0 -> 2, flat at both ends
//
// Every one of them is monotone (speed never below zero), starts at 0, ends
// at 1, and moves the same total distance as a straight line.  The pairs are
// exact mirror images: Slow In / Slow Out and Fast In / Fast Out are
// point-symmetric about the interval's midpoint, and Slow In / Fast Out is
// Fast In / Slow Out played backwards.  The tests assert all of that.
//
// Linear Smooth is not a time curve but a spline: the value follows a cubic
// Hermite segment whose tangent at each keyframe is the average of the
// straight-line speeds on either side of it (the finite-difference, or
// non-uniform Catmull-Rom, tangent).  The view therefore passes through
// every keyframe at a steady speed instead of changing speed abruptly there,
// and a two-keyframe move - or any run of evenly spaced, evenly stepped
// keyframes - is exactly linear, which is what the name promises.
//
// EXACT OR STANDARD?
// ------------------
// The seven presets, their names and their order are DJI Studio's own.  The
// SHAPES are DJI's (its preset icons plot the speed profiles described
// above).  The exact NUMBERS DJI Studio uses for each curve could not be
// established from DJI's public material, so every curve is the standard
// polynomial with that shape; docs/PREMIERE.md, "Keyframe Easing", records
// this per preset.
//
// HOW IT MEETS PREMIERE'S KEYFRAMES
// ---------------------------------
// Between two keyframes k0 <= t < k1 of a control, the effect reads the
// control's value AT the two keyframes (and, for Linear Smooth, at the
// keyframes on either side of them) and computes
//
//     v(t) = v0 + (v1 - v0) * s((t - k0) / (k1 - k0))
//
// itself.  For a control with Premiere's default Linear keyframes that is
// exactly the value Premiere would give at the remapped time
// k0 + s(u) (k1 - k0) - the time remap this design started from - but it
// does not depend on the host being able to sample a time between frames
// (the CPU path's time grid can be as coarse as one unit per frame), and it
// is the same arithmetic on both paths, which is what makes CPU / GPU parity
// hold by construction.  The consequence, documented for the user: while a
// preset is chosen, the preset alone decides how an eased control moves
// between its keyframes (as in DJI Studio, where the keyframe connection
// does); Premiere's own Bezier handles or Hold on those keyframes are not
// consulted.  Before the first keyframe and after the last, and for a
// control with fewer than two keyframes, the host's value is kept - there is
// nothing to ease.
#ifndef OSV_REFRAME_EASING_H
#define OSV_REFRAME_EASING_H

#include "ReframeParams.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace osv::reframe {

// ---------------------------------------------------------------------------
//  The curves
// ---------------------------------------------------------------------------

/// The fraction of the value change completed after a fraction `u` of the
/// interval, for the time-curve presets (Linear, Fast In / Slow Out,
/// Slow In / Fast Out, Fast In / Fast Out, Slow In / Slow Out).
///
/// `u` is clamped to [0, 1] (infinities included) and a NaN `u` reads as 0,
/// so the result is always in [0, 1] and finite.  None and Linear Smooth return `u` itself:
/// None does not ease at all, and Linear Smooth is a spline in the VALUE
/// domain (easedValue() below), whose time axis is linear.
[[nodiscard]] double easeProgress(KeyframeEasing easing, double u) noexcept;

/// The curve's SPEED at `u` - the derivative of easeProgress(), relative to
/// the straight line's speed (1).  Used by the tests to prove monotonicity
/// and the documented speed profiles; clamped and sanitised like `u` above.
[[nodiscard]] double easeSpeed(KeyframeEasing easing, double u) noexcept;

/// The cubic Hermite value between (k0, v0) and (k1, v1) with slopes m0 and
/// m1 (value per unit of time) at the two ends, at time t.  Returns v0 for a
/// degenerate interval (k1 <= k0) or any non-finite input, never NaN.
[[nodiscard]] double hermiteValue(double k0, double v0, double m0, double k1, double v1, double m1,
                                  double t) noexcept;

// ---------------------------------------------------------------------------
//  The host's keyframes, behind one small interface
// ---------------------------------------------------------------------------

/// One animatable control's keyframes, as a host can report them.
///
/// Times are in any unit the adapter likes - Premiere ticks on the GPU path,
/// the effect's time scale on the CPU path - as long as it is the same unit
/// for every call on one track.  Every method may fail (a host that refuses a
/// query, a control with no keyframes); a failure is std::nullopt and the
/// easing then leaves the host's own value in place.
class KeyframeTrack {
public:
    virtual ~KeyframeTrack() = default;
    /// The latest keyframe at or before `t`.
    [[nodiscard]] virtual std::optional<double> keyAtOrBefore(double t) = 0;
    /// The latest keyframe strictly before `t`.
    [[nodiscard]] virtual std::optional<double> keyBefore(double t) = 0;
    /// The earliest keyframe strictly after `t`.
    [[nodiscard]] virtual std::optional<double> keyAfter(double t) = 0;
    /// The control's value at `t`.  Only ever asked at a time the track itself
    /// reported as a keyframe.
    [[nodiscard]] virtual std::optional<double> valueAt(double t) = 0;
};

/// The value `easing` gives the control of `track` at time `t`, or
/// std::nullopt when the host's own value should be kept: easing None, `t`
/// before the first or after the last keyframe (Premiere holds there), fewer
/// than two keyframes, a degenerate interval, or any query the host refused.
///
/// Queries made: keyAtOrBefore and keyAfter once each, valueAt at the two
/// keyframes, and - for Linear Smooth only - keyBefore / keyAfter and
/// valueAt for the neighbouring keyframes (whose absence simply makes that
/// end's tangent the interval's own slope).
[[nodiscard]] std::optional<double> easedValue(KeyframeEasing easing, KeyframeTrack& track, double t) noexcept;

/// Premiere's GPU side can only walk keyframes FORWARDS: the Video Segment
/// Suite's GetNextKeyframeTime answers "the first keyframe strictly after
/// t".  This finds the latest keyframe at or before `t` with that primitive
/// alone.
///
/// `nextAfter(x)` is the host's query; `earliest` a time before any keyframe
/// the host can hold (the walk's starting point); `firstStep` the width of
/// the first backwards probe (one frame is a good choice).  The search
/// probes backwards from `t` with a doubling step until a probe lands on a
/// keyframe at or before `t` - O(log) host calls to reach a keyframe however
/// far back it is - then walks forwards to the last one at or before `t`.
/// `maxCalls` bounds the whole search; exceeding it, a host that answers
/// out of order, or an exception from the callback all return std::nullopt.
[[nodiscard]] std::optional<std::int64_t> keyAtOrBeforeFromNext(
    const std::function<std::optional<std::int64_t>(std::int64_t)>& nextAfter, std::int64_t t, std::int64_t earliest,
    std::int64_t firstStep, int maxCalls) noexcept;

/// Host calls one keyAtOrBeforeFromNext() search may make before it gives up.
/// Far above any real keyframe density (it is spent mostly on the forward
/// walk inside one probe window), and low enough that a misbehaving host
/// cannot stall a render.
inline constexpr int kMaxKeyframeQueries = 4096;

}  // namespace osv::reframe

#endif  // OSV_REFRAME_EASING_H
