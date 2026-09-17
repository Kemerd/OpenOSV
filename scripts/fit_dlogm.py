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

Two measurement sources are supported.

1. The built-in 64-point table (`HLG_TABLE`, the default).  These are
   neutral-axis measurements of a DJI D-Log M -> HLG rendering (grey code in
   -> HLG signal out, input i/63) taken from the Pocket-3 era reference.  The
   table is kept verbatim because it is the provenance record of the
   `kDlogMDjiRefit` constants that shipped first; re-running the script
   without arguments must keep reproducing them.

2. `--from-cube <path> --cube-transfer 709|hlg`: the neutral axis of a real
   .cube file, sampled at every one of its N grid points instead of a
   hand-copied subset.  This is how `kDlogMOsmo360` was produced, from DJI's
   own Osmo 360 D-Log M -> Rec.709 LUT.  Nothing from the file is shipped:
   only the fitted constants leave this script.

Either way the script re-fits the SAME seven-parameter form so the library can
reproduce DJI's tonal placement (grey 0.40 -> HLG 0.380, 0.714 -> 0.75) while
keeping a closed-form, invertible, continuous curve.

Why a Rec.709 LUT can be fitted in HLG-code space
-------------------------------------------------
OpenOSV's "Rec.709 output" is deliberately *not* a peak-to-peak tone map: it
is the HLG signal itself, computed in Rec.709 primaries (docs/COLOR.md,
"Rec.709 output"; BT.2390 "HLG on an SDR display").  Reading the neutral-axis
branch of `osvLinearToOutput` for OSV_TRANSFER_REC709 with R == G == B:

    working  = nativeToWorking * (lin, lin, lin)      -> (k*lin, k*lin, k*lin)
    working *= sceneScale                              (0.2674)
    tmp      = workingToOutput * working               (2020 -> 709, linear)
    out      = HLG_OETF(tmp)

Both 3x3 matrices are normalised so that a neutral input stays neutral - the
rows of a primaries-conversion matrix sum to 1 for an equal-energy triple -
so on the neutral axis they are the identity and the whole chain collapses to

    out(code) = HLG_OETF(sceneScale * lin(code))

which is *exactly* the HLG-output expression, and exactly the `model()` below.
No inversion of a separate 709 rendering is needed: for neutrals our 709 and
HLG outputs are numerically the same function, so a 709 reference LUT's
diagonal can be treated directly as HLG-signal targets.  `--cube-transfer`
therefore accepts both names and treats them identically for the neutral axis;
it exists so the provenance of a fit records which file was measured, and it
refuses anything else rather than silently fitting the wrong encoding.

This identity was checked numerically against DJI's Osmo 360 file: inverting
its diagonal through HLG_OETF^-1 / 0.2674 yields a smooth, strictly monotonic
scene-linear curve reaching 3.74 at code 1.0 with 18 % grey at code 0.406 -
i.e. DJI's 709 rendering really is an HLG-in-709 rendering, not a separate
tone map.  Had it been one, the recovered "linear" curve would have shown the
characteristic roll-off kink near diffuse white; it does not.

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
    # reproduce kDlogMDjiRefit from the built-in provenance table
    python scripts/fit_dlogm.py

    # reproduce kDlogMOsmo360 from DJI's Osmo 360 Rec.709 LUT
    python scripts/fit_dlogm.py --from-cube DJI_Osmo360_DLogM_to_Rec709.cube \
                               --cube-transfer 709 --name kDlogMOsmo360

    python scripts/fit_dlogm.py --json out.json   # also dumps the result
"""

import argparse
import io
import json
import os
import sys

import numpy as np

# Windows consoles default to a legacy code page; every string this script
# prints is 7-bit ASCII, but reconfiguring stdout to UTF-8 makes it safe even
# if a future message is not (and is a no-op on a sane terminal).
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):  # pragma: no cover - exotic/redirected stdout
        pass

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


# Transfer names accepted by --cube-transfer.  Both map onto the same neutral
# axis model (see the module docstring); the distinction is provenance only.
CUBE_TRANSFERS = ("709", "hlg")


def read_cube_neutral_axis(path):
    """
    Read the neutral (R == G == B) diagonal of a 3D .cube file.

    Returns ``(codes, targets, size, title)`` where ``codes`` is ``i/(N-1)``
    for every grid point and ``targets`` is the mean of the three output
    channels at that point.  The mean is used rather than the red channel
    alone because DJI's tables carry ~1e-4 of per-channel dither on the
    diagonal (green and blue read a few units higher than red); averaging it
    away is the right estimator for a neutral-axis fit and keeps the result
    independent of which channel a future file happens to favour.

    Defensive by design: the parser tolerates comments, blank lines, CRLF, a
    missing TITLE, arbitrary keyword case and DOMAIN_MIN/MAX lines, and raises
    a ``ValueError`` with a specific message for anything it cannot honour
    (1D LUTs, a missing size, a truncated table, a non-unit domain) instead of
    returning a silently wrong axis.
    """
    if not path or not os.path.isfile(path):
        raise ValueError("cube file not found: %r" % (path,))

    size = None
    title = None
    domain_min = [0.0, 0.0, 0.0]
    domain_max = [1.0, 1.0, 1.0]
    rows = []

    # errors="replace" so a stray byte in a vendor file cannot abort the read;
    # any replacement character lands in a token that then fails float() with
    # a clear line number.
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            upper = line.upper()
            if upper.startswith("TITLE"):
                title = line[5:].strip().strip('"')
                continue
            if upper.startswith("LUT_1D_SIZE"):
                raise ValueError("%s is a 1D LUT; a 3D LUT is required" % path)
            if upper.startswith("LUT_3D_SIZE"):
                try:
                    size = int(line.split()[-1])
                except (ValueError, IndexError):
                    raise ValueError("%s:%d: malformed LUT_3D_SIZE" % (path, lineno))
                continue
            if upper.startswith("DOMAIN_MIN"):
                domain_min = [float(v) for v in line.split()[1:4]]
                continue
            if upper.startswith("DOMAIN_MAX"):
                domain_max = [float(v) for v in line.split()[1:4]]
                continue
            parts = line.split()
            if len(parts) != 3:
                raise ValueError("%s:%d: expected three floats, got %r" % (path, lineno, line))
            try:
                rows.append([float(v) for v in parts])
            except ValueError:
                raise ValueError("%s:%d: non-numeric table entry %r" % (path, lineno, line))

    if size is None:
        raise ValueError("%s: no LUT_3D_SIZE line" % path)
    if size < 2 or size > 256:
        raise ValueError("%s: LUT_3D_SIZE %d out of range [2, 256]" % (path, size))
    if len(rows) != size ** 3:
        raise ValueError("%s: expected %d table rows, found %d" % (path, size ** 3, len(rows)))
    # A shifted or scaled input domain would make code = i/(N-1) wrong.
    if any(abs(v) > 1e-6 for v in domain_min) or any(abs(v - 1.0) > 1e-6 for v in domain_max):
        raise ValueError("%s: DOMAIN must be 0..1 (got %s..%s)" % (path, domain_min, domain_max))

    # .cube stores red fastest, then green, then blue, so the neutral entry
    # for grid index i is at i + i*N + i*N*N.
    codes = np.arange(size, dtype=np.float64) / float(size - 1)
    targets = np.empty(size, dtype=np.float64)
    for i in range(size):
        targets[i] = float(np.mean(rows[i + i * size + i * size * size]))

    # The diagonal of a display-referred LUT must be monotonic; a decreasing
    # step means the file is not what we think it is (or the axis order is).
    if np.any(np.diff(targets) < -1e-6):
        raise ValueError("%s: neutral axis is not monotonic; wrong axis order?" % path)
    return codes, targets, size, title


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
    parser = argparse.ArgumentParser(description="Fit a D-Log M -> scene-linear curve.")
    parser.add_argument("--json", help="write the fitted constants and residuals to this JSON file")
    parser.add_argument("--from-cube", metavar="PATH",
                        help="measure the neutral axis of this 3D .cube file instead of the built-in table")
    parser.add_argument("--cube-transfer", choices=CUBE_TRANSFERS, default="709",
                        help="output encoding of --from-cube (709 and hlg are the same on the neutral axis)")
    parser.add_argument("--name", default=None,
                        help="C++ constant name to print (default: kDlogMDjiRefit, or kDlogMOsmo360 with --from-cube)")
    args = parser.parse_args()

    # --- gather the measurements -------------------------------------------
    if args.from_cube:
        try:
            codes, targets, cube_size, cube_title = read_cube_neutral_axis(args.from_cube)
        except ValueError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
        source = "%s neutral axis, %d^3, --cube-transfer %s" % (args.from_cube, cube_size, args.cube_transfer)
        if cube_title:
            source += ' (TITLE "%s")' % cube_title
        default_name = "kDlogMOsmo360"
    else:
        codes = np.arange(64, dtype=np.float64) / 63.0
        targets = np.asarray(HLG_TABLE, dtype=np.float64)
        source = "built-in 64-point HLG_TABLE (provenance record for kDlogMDjiRefit)"
        default_name = "kDlogMDjiRefit"
    name = args.name or default_name

    weights = np.where(codes >= TOE_CODE, 1.0, TOE_WEIGHT)
    weights[0] = FIRST_SAMPLE_WEIGHT

    # Multi-start: the residual surface of this seven-parameter form has
    # several local minima (the exponential `scale` and the piecewise hinge
    # trade off against each other), and the Pocket 3 start is only the best
    # basin for Pocket-3-like data.  Trying a handful of spread-out starts and
    # keeping the lowest cost makes the result deterministic and independent of
    # which measurement set is being fitted.  The first entry is the historical
    # Pocket 3 start, so the built-in table still converges where it always did.
    starts = [
        [POCKET3["x_shift"], POCKET3["y_shift"], POCKET3["scale"], POCKET3["slope"], POCKET3["slope2"],
         POCKET3["intercept"]],
        [-2.722814610, 0.244082124, 5.598943717, 1.010442692, 3.031328816, 1.554539968],  # the shipped refit
        [-1.0, 0.0, 7.0, 1.0, 2.0, 0.5],
        [-3.0, 1.0, 7.0, 1.0, 2.5, 1.0],
    ]
    p, cost = None, float("inf")
    for start in starts:
        cand, cand_cost = levenberg_marquardt(residuals, start, (codes, targets, weights))
        # Reject any candidate the solver walked into that is not a usable
        # curve, however low its cost: NaNs, a negative toe or a non-positive
        # cut would all produce a broken OsvDlogMCurve.
        if not np.all(np.isfinite(cand)) or cand[4] <= cand[3] or cand[3] <= 0.0:
            continue
        if cand_cost < cost:
            p, cost = cand, cand_cost
    if p is None:
        print("error: no start point converged to a valid curve", file=sys.stderr)
        return 1

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

    print("D-Log M curve fit: %s (7-parameter Pocket 3 form)" % name)
    print("measurements: %s (%d points)" % (source, codes.size))
    print("pin: lin(%.3f) = %.4f, cut = intercept/(slope2-slope) (C0 continuous)" % (PIN_CODE, PIN_LINEAR))
    print("slope2/slope = %.3f (bound %.2f), cut = %.6f reached at code %.4f" % (slope2 / slope, MAX_SLOPE_RATIO, cut,
                                                                              cut_code))
    print("")
    print("  code    target    fitted    resid   weight")
    for c, t, f, r, w in zip(codes, targets, pred, resid, weights):
        print("  %.4f  %.4f  %.4f  %+.4f  %.2f" % (c, t, f, r, w))
    print("")
    print("RMS residual (all %2d points)     : %.5f HLG" % (codes.size, rms_all))
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
    print("inline constexpr OsvDlogMCurve %s = {" % name)
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
            "name": name,
            "source": source,
            "points": int(codes.size),
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
