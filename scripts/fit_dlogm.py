#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
fit_dlogm.py - derive the "DJI refit" D-Log M curve constants.

Background
----------
The Pocket 3 D-Log M -> linear curve is public and has the form

    tmp = 2 ** (scale * code + y_shift) + x_shift
    lin = (tmp * slope + intercept   if tmp < cut
           tmp * slope2              otherwise) * mid_gray_scaling

with `cut` defined as the intersection of the two branches
(cut = intercept / (slope2 - slope)) so the curve is C0 continuous.

For the Osmo 360 we have 64 neutral-axis measurements of DJI's own D-Log M
to HLG conversion (grey code in -> HLG signal out, input i/63).  This script
re-fits the SAME seven-parameter form to those measurements so the library
can reproduce DJI's tonal placement (grey 0.40 -> HLG 0.380, 0.714 -> 0.75)
while keeping a closed-form, invertible, continuous curve.

Fit definition
--------------
  * Model: hlg_pred(code) = HLG_OETF(0.2674 * lin(code))   (BT.2408 anchor)
  * Residuals are taken in HLG-code space.
  * Pinned: lin(0.40) == 0.18 exactly.  This is enforced by solving
    mid_gray_scaling = 0.18 / piecewise(0.40) inside the model, which removes
    one degree of freedom (six free parameters remain).
  * Weights: 1.0 for codes >= 0.24; the toe below that is crushed 8-bit data
    in DJI's LUT and is weighted 0.15 (the very first sample 0.05).
  * Bounded kink: slope2 / slope <= MAX_SLOPE_RATIO (3.0; Pocket 3 has 1.85).
    The cut is only C0, so the slope ratio is the size of the derivative jump
    at the cut.  Unconstrained, the solver imitates DJI's crushed toe with a
    hinge of ratio 8.8 at code 0.23 (about 3.5 stops under grey), which shows
    up as a derivative discontinuity in shadow gradients and as 3-4x larger
    trilinear error in the .cube LUTs.  Bounding the ratio to 3 leaves the
    residuals for code >= 0.30 unchanged (max 0.0110 vs 0.0111) and costs
    0.0005 RMS above code 0.24; only the down-weighted toe fits worse.
  * Solver: Levenberg-Marquardt in plain numpy (no scipy dependency), with a
    finite-difference Jacobian, started from the Pocket 3 constants.

Output
------
Prints the residual table, RMS / max residual and a C++ initializer block
that is pasted into include/osv/color/DlogM.h (kDlogMDjiRefit).  The output
is 7-bit ASCII so it is safe on any Windows console.

Usage
-----
    python scripts/fit_dlogm.py            # prints everything
    python scripts/fit_dlogm.py --json out.json   # also dumps the result
"""

import argparse
import json
import sys

import numpy as np

# -----------------------------------------------------------------------------
#  Measured data: DJI-matched neutral-axis samples (D-Log M code -> HLG signal)
#  Input is i/63 for i in 0..63.
# -----------------------------------------------------------------------------
HLG_TABLE = [
    0.0118, 0.0157, 0.0157, 0.0196, 0.0235, 0.0275, 0.0353, 0.0444,
    0.0549, 0.0627, 0.0706, 0.0863, 0.1020, 0.1137, 0.1294, 0.1451,
    0.1647, 0.1804, 0.2013, 0.2248, 0.2484, 0.2745, 0.2980, 0.3216,
    0.3503, 0.3752, 0.4013, 0.4248, 0.4484, 0.4719, 0.4954, 0.5163,
    0.5425, 0.5621, 0.5817, 0.5974, 0.6131, 0.6288, 0.6484, 0.6601,
    0.6758, 0.6915, 0.7072, 0.7229, 0.7386, 0.7503, 0.7660, 0.7778,
    0.7935, 0.8052, 0.8183, 0.8327, 0.8418, 0.8601, 0.8719, 0.8850,
    0.8967, 0.9124, 0.9242, 0.9373, 0.9490, 0.9647, 0.9791, 0.9922,
]

# BT.2408 scene-linear -> HLG anchor (0.18 * 0.2674 = 0.04813 -> HLG 0.380).
SCENE_SCALE = 0.2674

# Pocket 3 constants (public), used as the starting point of the fit.
POCKET3 = dict(
    x_shift=-2.428226947784424,
    y_shift=0.9327186346054077,
    scale=5.612990379333496,
    slope=1.0151796340942383,
    slope2=1.8734303712844849,
    intercept=0.5178895592689514,
    mid_gray_scaling=0.18 / 12.4054,
)

# Pin: this code must map to scene-linear 0.18 exactly.
PIN_CODE = 0.40
PIN_LINEAR = 0.18

# Toe weighting: codes below this are crushed 8-bit data in the reference LUT.
TOE_CODE = 0.24
TOE_WEIGHT = 0.15
FIRST_SAMPLE_WEIGHT = 0.05

# Largest allowed slope2 / slope (the derivative jump at the C0 cut).
MAX_SLOPE_RATIO = 3.0
MAX_SLOPE_RATIO_PENALTY = 50.0

# HLG OETF constants (BT.2100).
HLG_A = 0.17883277
HLG_B = 0.28466892
HLG_C = 0.55991073


def hlg_oetf(e):
    """BT.2100 HLG OETF, vectorised, input clamped at zero."""
    e = np.maximum(np.asarray(e, dtype=np.float64), 0.0)
    return np.where(e <= 1.0 / 12.0, np.sqrt(3.0 * e), HLG_A * np.log(np.maximum(12.0 * e - HLG_B, 1e-30)) + HLG_C)


def piecewise(p, code):
    """Un-scaled D-Log M curve (before mid_gray_scaling) for parameters p."""
    x_shift, y_shift, scale, slope, slope2, intercept = p
    tmp = np.exp2(scale * np.asarray(code, dtype=np.float64) + y_shift) + x_shift
    cut = intercept / (slope2 - slope)
    return np.where(tmp < cut, tmp * slope + intercept, tmp * slope2)


def mid_gray_scaling(p):
    """Scale factor that pins lin(PIN_CODE) == PIN_LINEAR."""
    return PIN_LINEAR / float(piecewise(p, PIN_CODE))


def linear(p, code):
    """Scene-linear output of the full curve."""
    return piecewise(p, code) * mid_gray_scaling(p)


def model(p, code):
    """Predicted HLG signal for a grey D-Log M code."""
    return hlg_oetf(SCENE_SCALE * linear(p, code))


def residuals(p, codes, targets, weights):
    """Weighted residual vector plus soft penalties keeping the fit sane."""
    x_shift, y_shift, scale, slope, slope2, intercept = p
    r = weights * (model(p, codes) - targets)
    # Soft constraints: positive slopes, slope2 > slope (positive cut), and
    # the toe must not go negative at code 0 (lin(0) >= 0).
    pen = []
    pen.append(10.0 * max(0.0, 1e-3 - slope))
    pen.append(10.0 * max(0.0, 1e-3 - (slope2 - slope)))
    # The toe must stay non-negative: weight this one hard so lin(0) >= 0.
    pen.append(2000.0 * max(0.0, -float(linear(p, 0.0))))
    # Bound the derivative jump at the cut (see the module docstring).
    ratio = slope2 / slope if slope > 1e-9 else 1e9
    pen.append(MAX_SLOPE_RATIO_PENALTY * max(0.0, ratio - MAX_SLOPE_RATIO))
    return np.concatenate([r, np.asarray(pen)])


def numeric_jacobian(fun, p, *args):
    """Central-difference Jacobian of fun at p."""
    p = np.asarray(p, dtype=np.float64)
    f0 = fun(p, *args)
    jac = np.zeros((f0.size, p.size))
    for i in range(p.size):
        h = 1e-6 * max(1.0, abs(p[i]))
        pp = p.copy()
        pm = p.copy()
        pp[i] += h
        pm[i] -= h
        jac[:, i] = (fun(pp, *args) - fun(pm, *args)) / (2.0 * h)
    return jac


def levenberg_marquardt(fun, p0, args, iterations=400, lam=1e-3):
    """Minimal damped Gauss-Newton solver (no scipy dependency)."""
    p = np.asarray(p0, dtype=np.float64)
    r = fun(p, *args)
    cost = float(r @ r)
    for _ in range(iterations):
        jac = numeric_jacobian(fun, p, *args)
        jtj = jac.T @ jac
        jtr = jac.T @ r
        improved = False
        # Try increasing damping until a step lowers the cost.
        for _inner in range(30):
            step = np.linalg.solve(jtj + lam * np.diag(np.diag(jtj) + 1e-12), -jtr)
            p_new = p + step
            r_new = fun(p_new, *args)
            cost_new = float(r_new @ r_new)
            if cost_new < cost:
                p, r, cost = p_new, r_new, cost_new
                lam = max(lam * 0.3, 1e-12)
                improved = True
                break
            lam *= 10.0
        if not improved or np.linalg.norm(step) < 1e-14:
            break
    return p, cost


def main():
    parser = argparse.ArgumentParser(description="Fit the DJI refit D-Log M curve.")
    parser.add_argument("--json", help="write the fitted constants and residuals to this JSON file")
    args = parser.parse_args()

    codes = np.arange(64, dtype=np.float64) / 63.0
    targets = np.asarray(HLG_TABLE, dtype=np.float64)
    weights = np.where(codes >= TOE_CODE, 1.0, TOE_WEIGHT)
    weights[0] = FIRST_SAMPLE_WEIGHT

    p0 = [POCKET3["x_shift"], POCKET3["y_shift"], POCKET3["scale"], POCKET3["slope"], POCKET3["slope2"],
          POCKET3["intercept"]]
    p, cost = levenberg_marquardt(residuals, p0, (codes, targets, weights))

    x_shift, y_shift, scale, slope, slope2, intercept = p
    mgs = mid_gray_scaling(p)
    cut = intercept / (slope2 - slope)

    pred = model(p, codes)
    resid = pred - targets
    used = codes >= TOE_CODE
    rms_all = float(np.sqrt(np.mean(resid ** 2)))
    rms_used = float(np.sqrt(np.mean(resid[used] ** 2)))
    max_used = float(np.max(np.abs(resid[used])))
    max_all = float(np.max(np.abs(resid)))

    # Code at which the exponential part crosses the cut (where the kink sits).
    cut_code = (np.log2(cut - x_shift) - y_shift) / scale if cut - x_shift > 0 else float("nan")

    print("DJI refit of the D-Log M curve (7-parameter Pocket 3 form)")
    print("pin: lin(%.3f) = %.4f, cut = intercept/(slope2-slope) (C0 continuous)" % (PIN_CODE, PIN_LINEAR))
    print("slope2/slope = %.3f (bound %.2f), cut = %.6f reached at code %.4f" % (slope2 / slope, MAX_SLOPE_RATIO, cut,
                                                                              cut_code))
    print("")
    print("  code    target    fitted    resid   weight")
    for c, t, f, r, w in zip(codes, targets, pred, resid, weights):
        print("  %.4f  %.4f  %.4f  %+.4f  %.2f" % (c, t, f, r, w))
    print("")
    print("RMS residual (all 64 points)     : %.5f HLG" % rms_all)
    print("RMS residual (code >= %.2f)      : %.5f HLG" % (TOE_CODE, rms_used))
    print("max |residual| (code >= %.2f)    : %.5f HLG" % (TOE_CODE, max_used))
    print("max |residual| (all)             : %.5f HLG" % max_all)
    print("final weighted cost              : %.6e" % cost)
    print("")
    print("anchors: code 0.400 -> lin %.5f -> HLG %.4f" % (float(linear(p, 0.4)), float(model(p, 0.4))))
    print("         code 0.714 -> lin %.5f -> HLG %.4f" % (float(linear(p, 0.714)), float(model(p, 0.714))))
    print("         code 1.000 -> lin %.5f -> HLG %.4f" % (float(linear(p, 1.0)), float(model(p, 1.0))))
    print("         code 0.000 -> lin %.6f" % float(linear(p, 0.0)))

    # Monotonicity check over a dense grid.
    dense = np.linspace(0.0, 1.0, 4097)
    lin_dense = linear(p, dense)
    diffs = np.diff(lin_dense)
    print("monotonic over 4097 samples      : %s (min step %.3e, max step %.3e)"
          % ("yes" if np.all(diffs > 0) else "NO", float(diffs.min()), float(diffs.max())))
    print("")
    print("C++ initializer (paste into include/osv/color/DlogM.h):")
    print("inline constexpr OsvDlogMCurve kDlogMDjiRefit = {")
    print("    %.9ff,  // xShift" % x_shift)
    print("    %.9ff,  // yShift" % y_shift)
    print("    %.9ff,  // scale" % scale)
    print("    %.9ff,  // slope" % slope)
    print("    %.9ff,  // slope2" % slope2)
    print("    %.9ff,  // intercept" % intercept)
    print("    %.11ff,  // midGrayScaling (pins code 0.40 -> 0.18)" % mgs)
    print("    %.9ff,  // cut (== intercept / (slope2 - slope))" % cut)
    print("    1  // cutMode: branch intersection")
    print("};")

    if args.json:
        payload = {
            "x_shift": x_shift,
            "y_shift": y_shift,
            "scale": scale,
            "slope": slope,
            "slope2": slope2,
            "intercept": intercept,
            "mid_gray_scaling": mgs,
            "cut": cut,
            "cut_code": float(cut_code),
            "slope_ratio": slope2 / slope,
            "max_slope_ratio": MAX_SLOPE_RATIO,
            "rms_all": rms_all,
            "rms_used": rms_used,
            "max_used": max_used,
            "residuals": [float(v) for v in resid],
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
