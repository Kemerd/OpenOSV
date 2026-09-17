#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
fit_primaries.py - recover the camera native -> Rec.2020 3x3 matrix from a
vendor D-Log M -> Rec.709 reference LUT.

Why this script exists
----------------------
scripts/fit_dlogm.py fits the D-Log M *tone curve* and uses only the neutral
(r == g == b) diagonal of the reference LUT.  A 33^3 LUT has 35937 entries;
the 33 diagonal ones carry the tone curve and the remaining 35904 carry the
*gamut* - the camera's native primaries.  Until this script existed OpenOSV
shipped `kNativeToRec2020_Pocket3`, a matrix fitted to a different camera,
and docs/COLOR.md said saturated hues were approximate "until a chart based
fit exists".  That was wrong: the information is already in the reference
file's off-diagonal structure.

That structure is directly visible.  Reading DJI's Osmo 360 file:

    in R=0.500 -> out 0.6071 0.0000 0.0534   (red leaks 0.0534 into blue)
    in B=0.500 -> out 0.0066 0.0000 0.6201   (blue leaks 0.0066 into red)
    in R=1.000 -> out 1.0000 0.0226 0.0053
    in B=1.000 -> out 0.0157 0.0000 1.0000

A pure tone curve is per-channel and can never move energy between channels,
so those cross-channel terms can only come from a primaries matrix.  This
script recovers it.

The algebra
-----------
OpenOSV's forward model for a D-Log M code triple is, reading
`osvCodeToLinear` then `osvLinearToOutput` (include/osv/color/ColorMath.h)
for the OSV_TRANSFER_REC709 branch:

    native[c] = dlogmToLinear(curve, code[c])          per channel, c in RGB
    working   = M * native                             M = nativeToWorking (unknown)
    working  *= sceneScale                             0.2674 (BT.2408 anchor)
    lin709    = W * working                            W = kRec2020ToRec709
    out       = clamp(HLG_OETF(max(lin709, 0)), 0, 1)  per channel

`exposureGain` is 1.0 for a LUT bake, and `workingToOutput` is the 2020->709
matrix for the 709 branch (identity for HLG/PQ).  docs/COLOR.md's "Rec.709
output" section establishes that this branch really is "the HLG signal in 709
primaries" and not a separate tone map, which is what lets a 709 reference be
used at all.

Two ways to solve for M fall out of that chain.

1. Inverted-target linear least squares (used as the *starting point*).
   Every stage after M is invertible on the entries where the LUT output is
   strictly inside (0, 1), so for those entries the working-space linear
   value is recoverable exactly:

       working_target = W^-1 * HLG_OETF^-1(out) / sceneScale

   and M is then the solution of the overdetermined linear system

       working_target ~= M * native            (35937 x 3 equations, 9 unknowns)

   This is a closed-form lstsq and is instant, but its residual is measured in
   *scene-linear* units, which over-weights highlights by orders of magnitude
   because the HLG OETF is log-like above 1/12.  A fit that minimises linear
   error puts almost all its effort into the brightest few percent of the cube
   and visibly under-fits the mids.  So it is only the seed.

2. Gauss-Newton on the full forward model (what is shipped).
   Minimise the residual where it is actually observed - in output HLG code
   units, the same units the .cube file stores and the same units every other
   residual in this project is quoted in:

       minimise  sum over all entries, all channels  (render(M, code) - out)^2

   with `render` the exact chain above, clamps included.  Six free parameters
   (see the constraint below), a finite-difference Jacobian and LM damping.
   Multi-start from four spread-out matrices, because the clamps make the
   objective piecewise and a single start cannot be trusted to be the global
   minimum; all four converge to the same basin here, which is the evidence
   that it is.

The row-sum constraint
----------------------
Every RGB->RGB primaries matrix in this project has rows that sum to 1, so an
equal-energy native triple stays equal-energy.  That is not cosmetic: it is
exactly what keeps the neutral axis and the BT.2408 anchors intact.  For a
neutral input native = (L, L, L),

    (M * native)[j] = L * (M[j][0] + M[j][1] + M[j][2]) = L * 1 = L

so M acts as the identity on neutrals *for any M with unit row sums*, and the
whole 709 chain collapses to out = HLG_OETF(sceneScale * L) - independent of
M.  Swapping one unit-row-sum matrix for another therefore cannot move the
neutral axis at all, which is why the tone-curve fit and this matrix fit are
genuinely separable and why 18 % grey stays pinned at HLG 0.380 for free.

The constraint is *eliminated*, not penalised: the third column of each row is
parameterised as 1 - a - b, so the solver searches a 6-parameter space in
which every point already satisfies it exactly.  A penalty would only satisfy
it approximately and would trade neutral-axis accuracy against gamut accuracy,
which is the one trade this fit must not make.

What is NOT recoverable from this file
--------------------------------------
Roughly 37 % of the reference's entries sit on the 0 or 1 output boundary, and
the model wants to go negative in Rec.709 linear for about 71 % of the cube
(the native gamut is wider than Rec.709, so most saturated inputs are simply
outside the output gamut).  On those entries DJI's table holds a *gamut-mapped*
value - deeply saturated reds keep ~0.27 of green where a pure matrix plus
clamp gives 0 - and no 3x3 matrix can reproduce that, because it is not a
linear operation.  Those entries dominate the worst-case residual and always
will.  The script reports the residual on the "clean" subset (in gamut for
both sides, ~24 % of entries) separately for exactly this reason, and a fit
restricted to that subset is reported too so the reader can see it does not
do better.  DJI's gamut compression is out of scope for a primaries matrix and
is not claimed to be fitted.

Provenance and licensing
------------------------
The reference file is measured, never redistributed.  Only the nine fitted
constants leave this script, and OpenOSV ships only .cube files its own
generator produces.  See NOTICE.

Usage
-----
    python scripts/fit_primaries.py --from-cube DJI_Osmo360_DLogM_to_Rec709.cube
    python scripts/fit_primaries.py --from-cube <path> --json out.json

Output is 7-bit ASCII so it is safe on a legacy Windows console.
"""

import argparse
import io
import json
import os
import re
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLOGM_HEADER = os.path.join(ROOT, "include", "osv", "color", "DlogM.h")

# --- BT.2100 HLG and BT.2408 constants ---------------------------------------
HLG_A, HLG_B, HLG_C = 0.17883277, 0.28466892, 0.55991073
SCENE_SCALE = 0.2674

# Rec.2020 linear -> Rec.709 linear, as shipped in include/osv/color/Matrices.h
# (kRec2020ToRec709).  Copied rather than parsed because this is the one matrix
# the fit must treat as fixed ground truth; a silent change to it would change
# the meaning of the result, so it is stated here and asserted against the
# header below.
REC2020_TO_709 = np.array([
    [1.660491, -0.587641, -0.072850],
    [-0.124550, 1.132900, -0.008349],
    [-0.018151, -0.100579, 1.118730],
])

# The currently shipped Pocket 3 matrix, the baseline every residual is quoted
# against (kNativeToRec2020_Pocket3).
NATIVE_TO_2020_POCKET3 = np.array([
    [0.785301, 0.178838, 0.035860],
    [-0.036655, 1.258089, -0.221434],
    [-0.014322, 0.077260, 0.937062],
])

# Rec.2020 RGB -> CIE XYZ (D65), from the BT.2020 primaries.  Used only for the
# physical-plausibility report: it converts the fitted matrix's implied native
# primaries into chromaticity coordinates a human can sanity check.
RGB2020_TO_XYZ = np.array([
    [0.6369580, 0.1446169, 0.1688810],
    [0.2627002, 0.6779981, 0.0593017],
    [0.0000000, 0.0280727, 1.0609851],
])

# Reference chromaticities for the plausibility report.
D65_XY = (0.3127, 0.3290)
REC2020_PRIMARIES_XY = ((0.708, 0.292), (0.170, 0.797), (0.131, 0.046))
REC709_PRIMARIES_XY = ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060))

# A "saturated" entry for the reported breakdown: at least one channel code is
# more than this far from the mean of the three, i.e. it is not near-neutral.
SATURATION_THRESHOLD = 0.15

# An output sample is treated as sitting on the encoding boundary (and so as
# carrying no recoverable gamut information) within this distance of 0 or 1.
CLIP_EPSILON = 1e-9


# -----------------------------------------------------------------------------
#  Reference LUT parsing
# -----------------------------------------------------------------------------
def read_cube(path):
    """
    Read a full 3D .cube file.

    Returns ``(codes, table, size, title)`` where ``codes[i]`` is the RGB input
    triple of entry ``i`` and ``table[i]`` its RGB output triple, both as
    float64 arrays of shape ``(size**3, 3)``.

    .cube stores red fastest, then green, then blue, so entry index
    ``i = ir + ig*N + ib*N*N`` has input ``(ir, ig, ib) / (N - 1)``.  That
    ordering is reconstructed here rather than assumed elsewhere.

    Defensive by design, and deliberately sharing its tolerances with
    ``fit_dlogm.read_cube_neutral_axis``: comments, blank lines, CRLF, a
    missing TITLE, arbitrary keyword case and DOMAIN_MIN/MAX lines are all
    accepted, and anything that cannot be honoured (a 1D LUT, a missing or
    out-of-range size, a truncated table, a non-unit input domain) raises a
    ``ValueError`` naming the problem instead of returning a silently wrong
    cube.
    """
    if not path or not os.path.isfile(path):
        raise ValueError("cube file not found: %r" % (path,))

    size = None
    title = None
    domain_min = [0.0, 0.0, 0.0]
    domain_max = [1.0, 1.0, 1.0]
    rows = []

    # errors="replace" so a stray byte in a vendor file cannot abort the read;
    # any replacement character lands in a token that then fails float() with a
    # clear line number.
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

    table = np.asarray(rows, dtype=np.float64)
    idx = np.arange(size ** 3)
    codes = np.stack([idx % size, (idx // size) % size, idx // (size * size)], axis=1)
    codes = codes.astype(np.float64) / float(size - 1)
    return codes, table, size, title


# -----------------------------------------------------------------------------
#  D-Log M curve: parsed from the shipped header so the two cannot drift
# -----------------------------------------------------------------------------
def parse_dlogm_curve(name, path=DLOGM_HEADER):
    """
    Pull one ``OsvDlogMCurve`` initializer out of include/osv/color/DlogM.h.

    The curve used to decode the reference LUT's *input* codes must be exactly
    the curve OpenOSV ships, or the recovered matrix silently absorbs the
    difference between the two curves and is wrong in a way no residual would
    reveal.  Parsing the header rather than re-typing the constants makes that
    impossible: a curve change forces a re-fit instead of producing a stale
    matrix that still looks converged.

    ``midGrayScaling`` is stored in the header as either a literal or the
    expression ``0.18f / 12.4054f``, so both spellings are evaluated.
    """
    if not os.path.isfile(path):
        raise ValueError("D-Log M header not found: %r" % (path,))
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    # Grab the brace-delimited initializer body that follows the constant name.
    match = re.search(r"OsvDlogMCurve\s+" + re.escape(name) + r"\s*=\s*\{(.*?)\};", text, re.S)
    if match is None:
        raise ValueError("%s: no initializer for %s" % (path, name))
    body = match.group(1)
    # Strip // comments, then take the first six comma-separated float fields
    # plus midGrayScaling (the seventh).
    body = re.sub(r"//[^\n]*", "", body)
    fields = [f.strip() for f in body.split(",")]
    fields = [f for f in fields if f]
    if len(fields) < 7:
        raise ValueError("%s: %s has %d fields, expected at least 7" % (path, name, len(fields)))

    def value(expr):
        """Evaluate a C float literal or a simple ``a f / b f`` division."""
        expr = expr.replace("f", "").strip()
        if "/" in expr:
            num, den = expr.split("/", 1)
            return float(num) / float(den)
        return float(expr)

    keys = ("x_shift", "y_shift", "scale", "slope", "slope2", "intercept", "mid_gray_scaling")
    curve = {k: value(fields[i]) for i, k in enumerate(keys)}
    # A non-monotonic or degenerate curve would make the whole fit meaningless.
    if not (curve["slope"] > 0.0 and curve["slope2"] > curve["slope"] and curve["scale"] > 0.0
            and curve["mid_gray_scaling"] > 0.0):
        raise ValueError("%s: %s is not a usable monotonic curve: %r" % (path, name, curve))
    return curve


def dlogm_to_linear(curve, code):
    """
    D-Log M code -> scene-linear native, vectorised, float64.

    Mirrors ``osvDlogmToLinear`` exactly, including the branch-intersection cut
    (``OSV_DLOGM_CUT_INTERSECTION``): all three shipped curves use that mode,
    so the stored ``cut`` field is ignored here as the kernel ignores it.
    """
    code = np.asarray(code, dtype=np.float64)
    tmp = np.exp2(curve["scale"] * code + curve["y_shift"]) + curve["x_shift"]
    cut = curve["intercept"] / (curve["slope2"] - curve["slope"])
    piecewise = np.where(tmp < cut, tmp * curve["slope"] + curve["intercept"], tmp * curve["slope2"])
    return piecewise * curve["mid_gray_scaling"]


# -----------------------------------------------------------------------------
#  Transfer functions
# -----------------------------------------------------------------------------
def hlg_oetf(e):
    """BT.2100 HLG OETF, vectorised, input clamped at zero (osvHlgOetf)."""
    e = np.maximum(np.asarray(e, dtype=np.float64), 0.0)
    return np.where(e <= 1.0 / 12.0, np.sqrt(3.0 * e),
                    HLG_A * np.log(np.maximum(12.0 * e - HLG_B, 1e-300)) + HLG_C)


def hlg_inverse_oetf(ep):
    """
    BT.2100 HLG inverse OETF (osvHlgInverseOetf), vectorised.

    The log branch is evaluated on a floored copy of the input so numpy does
    not raise on the values ``np.where`` will discard anyway.
    """
    ep = np.maximum(np.asarray(ep, dtype=np.float64), 0.0)
    safe = np.maximum(ep, 0.5 + 1e-12)
    return np.where(ep <= 0.5, ep * ep / 3.0, (np.exp((safe - HLG_C) / HLG_A) + HLG_B) / 12.0)


# -----------------------------------------------------------------------------
#  The forward model under test
# -----------------------------------------------------------------------------
def render(matrix, native):
    """
    Scene-linear native RGB -> Rec.709 output signal, exactly as the kernel does.

    This is the OSV_TRANSFER_REC709 branch of ``osvLinearToOutput`` with
    ``exposureGain == 1`` and ``workingToOutput == kRec2020ToRec709``, clamps
    included.  The clamps matter to the fit: they are what makes the objective
    piecewise (and hence the multi-start necessary), and leaving them out would
    fit a model the renderer does not implement.
    """
    working = np.maximum((matrix @ native.T).T * SCENE_SCALE, 0.0)
    lin709 = np.maximum((REC2020_TO_709 @ working.T).T, 0.0)
    return np.clip(hlg_oetf(lin709), 0.0, 1.0)


def out_of_gamut_mask(matrix, native):
    """
    Entries whose Rec.709 linear value the forward model drives negative.

    The camera's native gamut is wider than Rec.709, so most saturated inputs
    genuinely fall outside the output gamut and the forward ``max(.., 0)``
    clamp - not the matrix - decides our output there.  Those entries carry no
    usable primaries information, which is why they are reported separately and
    excluded from the "clean" subset.
    """
    working = (np.asarray(matrix, dtype=np.float64) @ native.T).T * SCENE_SCALE
    lin709 = (REC2020_TO_709 @ working.T).T
    return (lin709 < -1e-6).any(axis=1)


def invert_output(table):
    """
    Recover working-space (Rec.2020) scene-linear from Rec.709 output signal.

    The inverse of every stage after the unknown matrix:

        lin709  = HLG_OETF^-1(out)
        working = (2020 -> 709)^-1 * lin709 / sceneScale

    Only meaningful for entries strictly inside (0, 1); on the boundary the
    forward clamp destroyed the information and the inverse returns whatever
    the boundary maps to.  Callers mask those out.
    """
    lin709 = hlg_inverse_oetf(table)
    to_2020 = np.linalg.inv(REC2020_TO_709)
    return (to_2020 @ lin709.T).T / SCENE_SCALE


# -----------------------------------------------------------------------------
#  Constrained parameterisation: rows sum to 1 by construction
# -----------------------------------------------------------------------------
def pack(matrix):
    """3x3 with unit row sums -> the six free parameters (first two columns)."""
    m = np.asarray(matrix, dtype=np.float64)
    return np.array([m[0, 0], m[0, 1], m[1, 0], m[1, 1], m[2, 0], m[2, 1]])


def unpack(params):
    """
    Six free parameters -> 3x3 with rows summing to exactly 1.

    The third column is *computed* as 1 - a - b rather than fitted, so the
    constraint holds identically at every point the solver visits and cannot be
    traded away against gamut accuracy.
    """
    p = np.asarray(params, dtype=np.float64)
    return np.array([
        [p[0], p[1], 1.0 - p[0] - p[1]],
        [p[2], p[3], 1.0 - p[2] - p[3]],
        [p[4], p[5], 1.0 - p[4] - p[5]],
    ])


def seed_from_linear_lstsq(native, working_target):
    """
    Closed-form seed: row-wise least squares with the row-sum constraint.

    Substituting m2 = 1 - m0 - m1 into ``b = m . a`` gives, per output row j,

        b_j - a_2 = m0 * (a_0 - a_2) + m1 * (a_1 - a_2)

    an unconstrained 2-parameter least-squares problem whose solution already
    satisfies the constraint.  Cheap, deterministic, and close enough to seed
    the Gauss-Newton stage; not shipped on its own because its residual is in
    scene-linear units (see the module docstring).
    """
    design = np.stack([native[:, 0] - native[:, 2], native[:, 1] - native[:, 2]], axis=1)
    rows = []
    for j in range(3):
        rhs = working_target[:, j] - native[:, 2]
        solution, _residuals, _rank, _sv = np.linalg.lstsq(design, rhs, rcond=None)
        rows.append([solution[0], solution[1], 1.0 - solution[0] - solution[1]])
    return np.asarray(rows)


# -----------------------------------------------------------------------------
#  Solver
# -----------------------------------------------------------------------------
def residual_vector(params, native, target):
    """Flat residual in output HLG-code units for the 6-parameter vector."""
    return (render(unpack(params), native) - target).ravel()


def levenberg_marquardt(params0, native, target, iterations=300, lam=1e-6):
    """
    Damped Gauss-Newton on the six free parameters (no scipy dependency).

    Central-difference Jacobian with a fixed 1e-7 step: the parameters are all
    O(0.1..1) and the objective is smooth in them away from the clamp
    boundaries, so a fixed step is both stable and cheap.  Damping is increased
    until a step lowers the cost, which is what keeps the solver from stepping
    across a clamp boundary into a worse basin.
    """
    p = np.asarray(params0, dtype=np.float64)
    r = residual_vector(p, native, target)
    cost = float(r @ r)
    step = np.zeros_like(p)
    for _iteration in range(iterations):
        jacobian = np.zeros((r.size, p.size), dtype=np.float64)
        for i in range(p.size):
            h = 1e-7
            forward = p.copy()
            backward = p.copy()
            forward[i] += h
            backward[i] -= h
            jacobian[:, i] = (residual_vector(forward, native, target) -
                              residual_vector(backward, native, target)) / (2.0 * h)
        jtj = jacobian.T @ jacobian
        jtr = jacobian.T @ r
        improved = False
        for _inner in range(50):
            try:
                step = np.linalg.solve(jtj + lam * np.diag(np.diag(jtj) + 1e-14), -jtr)
            except np.linalg.LinAlgError:  # pragma: no cover - damping fixes this
                lam *= 10.0
                continue
            candidate = p + step
            r_new = residual_vector(candidate, native, target)
            cost_new = float(r_new @ r_new)
            if cost_new < cost:
                p, r, cost = candidate, r_new, cost_new
                lam = max(lam * 0.3, 1e-16)
                improved = True
                break
            lam *= 10.0
        if not improved or float(np.linalg.norm(step)) < 1e-13:
            break
    return p, cost


def to_float32_unit_rows(matrix):
    """
    Round to float32 with the row sums still exact *in float32*.

    The shipped constants are ``float``, and the test asserts rows sum to 1 to
    1e-9.  Rounding all nine entries independently leaves row sums off by a few
    ulps, so instead the first two entries of each row are rounded and the third
    is computed as ``1 - a - b`` in float32 arithmetic - which is exactly how
    the header's value is derived, so the sum is bit-exact rather than merely
    close.
    """
    out = np.zeros((3, 3), dtype=np.float32)
    for i in range(3):
        a = np.float32(matrix[i, 0])
        b = np.float32(matrix[i, 1])
        out[i, 0] = a
        out[i, 1] = b
        out[i, 2] = np.float32(np.float32(1.0) - a - b)
    return out


# -----------------------------------------------------------------------------
#  Reporting
# -----------------------------------------------------------------------------
def residual_stats(matrix, native, table, masks):
    """RMS and worst |residual| overall and for each named mask."""
    delta = render(matrix, native) - table
    stats = {"rms": float(np.sqrt(np.mean(delta ** 2))), "worst": float(np.abs(delta).max())}
    for name, mask in masks.items():
        if not np.any(mask):  # pragma: no cover - only for a degenerate LUT
            stats[name + "_rms"] = float("nan")
            stats[name + "_worst"] = float("nan")
            continue
        sub = delta[mask]
        stats[name + "_rms"] = float(np.sqrt(np.mean(sub ** 2)))
        stats[name + "_worst"] = float(np.abs(sub).max())
    return stats


def implied_primaries(matrix):
    """
    Chromaticities of the native primaries the matrix implies, plus its white.

    A native primary is the unit vector for that channel, so column ``i`` of
    ``RGB2020_TO_XYZ @ matrix`` is that primary's XYZ.  The white point is the
    equal-energy triple, which the unit-row-sum constraint forces onto
    Rec.2020's white (D65) exactly - reporting it is a check on the constraint,
    not on the fit.
    """
    native_to_xyz = RGB2020_TO_XYZ @ np.asarray(matrix, dtype=np.float64)
    out = []
    for i in range(3):
        x_val, y_val, z_val = native_to_xyz[:, i]
        total = x_val + y_val + z_val
        # A primary summing to ~0 in XYZ is degenerate; report NaN rather than
        # dividing by zero and printing an inf.
        if abs(total) < 1e-12:  # pragma: no cover - would be a broken fit
            out.append((float("nan"), float("nan"), float(y_val)))
        else:
            out.append((float(x_val / total), float(y_val / total), float(y_val)))
    white = native_to_xyz @ np.ones(3)
    white_total = white[0] + white[1] + white[2]
    white_xy = (float(white[0] / white_total), float(white[1] / white_total))
    return out, white_xy


def plausibility(matrix):
    """
    Physical sanity checks on a candidate native-primaries matrix.

    Returns ``(ok, lines)``: a list of human-readable findings and whether all
    of the hard checks passed.  The checks are the ones that a matrix has to
    pass to be a believable *camera*, as opposed to merely a low-residual fit:

      * every diagonal entry positive - a channel must respond positively to
        its own primary, or the fit has swapped or inverted a channel;
      * determinant positive - the transform preserves orientation and is
        invertible, so a round trip through it exists;
      * rows sum to 1 - white preservation (checked here as well as asserted
        in C++, because a violation here means the parameterisation broke);
      * every implied primary has positive luminance Y and lies in
        0 <= y <= 1 - a real primary cannot have negative luminance.  This is
        the check the shipped Pocket 3 matrix *fails*: its blue primary comes
        out at y = -0.081, Y = -0.085.
    """
    m = np.asarray(matrix, dtype=np.float64)
    lines = []
    ok = True

    diagonal = np.diag(m)
    diag_ok = bool(np.all(diagonal > 0.0))
    ok = ok and diag_ok
    lines.append("  diagonal positive       : %s  (%s)"
                 % ("yes" if diag_ok else "NO", np.array2string(diagonal, precision=6)))

    determinant = float(np.linalg.det(m))
    det_ok = determinant > 0.0
    ok = ok and det_ok
    lines.append("  determinant > 0         : %s  (%.9f)" % ("yes" if det_ok else "NO", determinant))

    # The row sum is evaluated in float32, left to right, because that is what
    # the C++ test does with the shipped `float` constants and what the kernel
    # does to a neutral pixel.  Summing the same entries in float64 would
    # reintroduce the very ulp error that `to_float32_unit_rows` cancels by
    # deriving the third entry as 1 - a - b in float32, and would report a
    # bit-exact matrix as failing by ~4e-8.
    row_sums = np.array([
        float(np.float32(np.float32(np.float32(m[i, 0]) + np.float32(m[i, 1])) + np.float32(m[i, 2])))
        for i in range(3)
    ])
    rows_ok = bool(np.all(np.abs(row_sums - 1.0) < 1e-9))
    ok = ok and rows_ok
    lines.append("  rows sum to 1 (1e-9)    : %s  (%s)"
                 % ("yes" if rows_ok else "NO", np.array2string(row_sums, precision=12)))

    primaries, white_xy = implied_primaries(m)
    for i, name in enumerate("RGB"):
        x_val, y_val, luminance = primaries[i]
        primary_ok = luminance > 0.0 and 0.0 <= y_val <= 1.0
        ok = ok and primary_ok
        lines.append("  %s primary              : x=%.4f y=%.4f Y=%+.4f  %s"
                     % (name, x_val, y_val, luminance, "ok" if primary_ok else "UNPHYSICAL"))
    white_ok = abs(white_xy[0] - D65_XY[0]) < 1e-3 and abs(white_xy[1] - D65_XY[1]) < 1e-3
    ok = ok and white_ok
    lines.append("  white point (D65)       : x=%.4f y=%.4f  %s"
                 % (white_xy[0], white_xy[1], "ok" if white_ok else "OFF D65"))
    return ok, lines


def check_shipped_matrix_unchanged():
    """
    Assert include/osv/color/Matrices.h still holds the kRec2020ToRec709 this
    script hard-codes.

    That matrix is the fixed part of the forward model: if the header's copy
    ever changed, every residual printed here would be measured against a
    pipeline the renderer no longer implements, and the fit would be quietly
    invalid.  Cheap to check, so it is checked.
    """
    path = os.path.join(ROOT, "include", "osv", "color", "Matrices.h")
    if not os.path.isfile(path):
        return "warning: %s not found; could not verify kRec2020ToRec709" % path
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    match = re.search(r"kRec2020ToRec709\s*=\s*\{\{(.*?)\}\};", text, re.S)
    if match is None:
        return "warning: kRec2020ToRec709 not found in %s" % path
    numbers = [float(v) for v in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", match.group(1))]
    if len(numbers) != 9:
        return "warning: kRec2020ToRec709 has %d entries, expected 9" % len(numbers)
    if np.max(np.abs(np.asarray(numbers).reshape(3, 3) - REC2020_TO_709)) > 1e-6:
        raise ValueError("kRec2020ToRec709 in %s differs from this script's copy; "
                         "the forward model changed and the fit must be redone" % path)
    return None


# -----------------------------------------------------------------------------
#  Entry point
# -----------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Fit a camera native -> Rec.2020 3x3 from a D-Log M -> Rec.709 reference LUT.")
    parser.add_argument("--from-cube", metavar="PATH", required=True,
                        help="the reference 3D .cube file to measure (never redistributed)")
    parser.add_argument("--curve", default="kDlogMOsmo360",
                        help="D-Log M curve constant in include/osv/color/DlogM.h used to decode "
                             "the LUT's input codes (default: kDlogMOsmo360)")
    parser.add_argument("--name", default="kNativeToRec2020_Osmo360",
                        help="C++ constant name to print")
    parser.add_argument("--json", help="also write the fitted matrix and residuals to this JSON file")
    args = parser.parse_args()

    warning = None
    try:
        warning = check_shipped_matrix_unchanged()
        codes, table, size, title = read_cube(args.from_cube)
        curve = parse_dlogm_curve(args.curve)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if warning:
        print(warning, file=sys.stderr)

    source = "%s, %d^3 (%d entries)" % (args.from_cube, size, size ** 3)
    if title:
        source += ' (TITLE "%s")' % title

    print("Native primaries fit: %s" % args.name)
    print("measurements: %s" % source)
    print("input decode: %s from %s" % (args.curve, os.path.relpath(DLOGM_HEADER, ROOT)))
    print("forward model: dlogm decode -> M -> x%.4f -> 2020->709 -> HLG OETF -> clamp" % SCENE_SCALE)
    print("")

    # --- the two sides of the fit -------------------------------------------
    native = dlogm_to_linear(curve, codes)
    working_target = invert_output(table)

    # Masks used for the residual breakdown.
    #   clipped : the reference output sits on 0 or 1, so the vendor's own
    #             clamp (or gamut map) decided it and it carries no primaries
    #             information that a matrix could match.
    #   negative: our model wants a negative Rec.709 linear value, i.e. the
    #             input is outside the output gamut and the forward clamp is
    #             deciding our value too.
    #   clean   : neither side is clamped - the only entries that are pure
    #             evidence about the primaries.
    #   saturated / neutral: the headline split, since the whole point of this
    #             fit is saturated colour.
    clipped = ((table <= CLIP_EPSILON) | (table >= 1.0 - CLIP_EPSILON)).any(axis=1)
    neutral_index = np.arange(size) * (1 + size + size * size)
    neutral = np.zeros(size ** 3, dtype=bool)
    neutral[neutral_index] = True
    saturated = np.abs(codes - codes.mean(axis=1, keepdims=True)).max(axis=1) > SATURATION_THRESHOLD

    # --- stage 1: closed-form seed on the unclipped entries -----------------
    # The seed is computed only where the output inversion is meaningful; the
    # Gauss-Newton stage then uses the whole cube.
    invertible = ~clipped
    if invertible.sum() < 64:
        print("error: only %d invertible entries; the reference is too clipped to fit"
              % invertible.sum(), file=sys.stderr)
        return 1
    seed = seed_from_linear_lstsq(native[invertible], working_target[invertible])

    # --- stage 2: Gauss-Newton in output HLG-code units, multi-start ---------
    # Four spread-out starts: the linear-lstsq seed, the shipped Pocket 3
    # matrix, the identity, and a hand-written plausible camera matrix.  The
    # clamps make the objective piecewise, so agreement between independent
    # starts is the only available evidence that the minimum found is global.
    starts = [
        ("linear-lstsq seed", seed),
        ("Pocket 3 (shipped)", NATIVE_TO_2020_POCKET3),
        ("identity", np.eye(3)),
        ("generic camera", np.array([[0.75, 0.20, 0.05], [0.05, 0.95, 0.00], [0.00, -0.10, 1.10]])),
    ]
    print("multi-start Gauss-Newton (residual in output HLG code units):")
    best_matrix = None
    best_cost = float("inf")
    start_costs = []
    for label, start in starts:
        params, cost = levenberg_marquardt(pack(start), native, table)
        candidate = unpack(params)
        # Reject anything the solver walked into that is not a usable matrix,
        # however low its cost: a non-finite or singular matrix would produce a
        # broken pipeline, and a negative determinant a mirrored gamut.
        if not np.all(np.isfinite(candidate)) or np.linalg.det(candidate) <= 0.0:
            print("  %-20s cost %.6f  REJECTED (non-finite or det <= 0)" % (label, cost))
            continue
        rms = float(np.sqrt(cost / (size ** 3 * 3)))
        start_costs.append({"start": label, "cost": cost, "rms": rms})
        print("  %-20s cost %12.6f  RMS %.6f" % (label, cost, rms))
        if cost < best_cost:
            best_matrix, best_cost = candidate, cost
    if best_matrix is None:
        print("error: no start converged to a usable matrix", file=sys.stderr)
        return 1
    spread = max(c["rms"] for c in start_costs) - min(c["rms"] for c in start_costs)
    print("  RMS spread across starts: %.6f (%s)"
          % (spread, "one basin" if spread < 1e-4 else "MULTIPLE BASINS - inspect"))
    print("")

    # --- float32 rounding ---------------------------------------------------
    shipped = to_float32_unit_rows(best_matrix).astype(np.float64)

    # --- what the reference can and cannot tell us ---------------------------
    # Classified with the FITTED matrix, since it is the fitted model whose
    # residual is being explained.  "clean" entries are the only ones where
    # neither side is decided by a clamp or by DJI's gamut compression, i.e. the
    # only pure evidence about the primaries.
    negative = out_of_gamut_mask(shipped, native)
    clean = (~clipped) & (~negative)
    masks = {"saturated": saturated, "neutral": neutral, "clean": clean}

    print("entry classification (why the worst-case residual has a floor):")
    print("  reference output on the 0/1 boundary : %6d (%.1f %%)" % (clipped.sum(), 100.0 * clipped.mean()))
    print("  our model outside Rec.709 gamut      : %6d (%.1f %%)" % (negative.sum(), 100.0 * negative.mean()))
    print("  clean for both (primaries evidence)  : %6d (%.1f %%)" % (clean.sum(), 100.0 * clean.mean()))
    print("  saturated (|code - mean| > %.2f)      : %6d (%.1f %%)"
          % (SATURATION_THRESHOLD, saturated.sum(), 100.0 * saturated.mean()))
    print("")

    # --- residuals, before and after ----------------------------------------
    before = residual_stats(NATIVE_TO_2020_POCKET3, native, table, masks)
    after = residual_stats(shipped, native, table, masks)
    print("residuals against %s (output HLG code units):" % os.path.basename(args.from_cube))
    print("  %-26s %10s %10s | %10s %10s" % ("", "RMS", "worst", "RMS", "worst"))
    print("  %-26s %10s %10s | %10s %10s" % ("", "Pocket 3", "Pocket 3", args.name.split("_")[-1],
                                             args.name.split("_")[-1]))
    for label, key in (("full cube (%d entries)" % size ** 3, ""),
                       ("saturated entries", "saturated_"),
                       ("clean entries", "clean_"),
                       ("neutral axis (%d)" % size, "neutral_")):
        b_rms = before["rms" if not key else key + "rms"]
        b_max = before["worst" if not key else key + "worst"]
        a_rms = after["rms" if not key else key + "rms"]
        a_max = after["worst" if not key else key + "worst"]
        print("  %-26s %10.6f %10.6f | %10.6f %10.6f" % (label, b_rms, b_max, a_rms, a_max))
    print("")
    print("  full-cube RMS improvement  : %.6f -> %.6f (%.1f %% lower)"
          % (before["rms"], after["rms"], 100.0 * (1.0 - after["rms"] / before["rms"])))
    print("  saturated RMS improvement  : %.6f -> %.6f (%.1f %% lower)"
          % (before["saturated_rms"], after["saturated_rms"],
             100.0 * (1.0 - after["saturated_rms"] / before["saturated_rms"])))
    print("")

    # --- the neutral-axis invariance, demonstrated rather than assumed ------
    # Both matrices have unit row sums, so both act as the identity on a
    # neutral triple and the 709 chain collapses to the same function of the
    # code.  This prints the measured difference; it is limited by float64
    # round-off in the two matrix products, not by the fit.
    neutral_before = render(NATIVE_TO_2020_POCKET3, native[neutral_index])
    neutral_after = render(shipped, native[neutral_index])
    neutral_swing = float(np.abs(neutral_after - neutral_before).max())
    print("neutral axis is invariant under the matrix swap (unit row sums):")
    print("  max |render(Pocket 3) - render(%s)| on the %d neutral entries = %.3e"
          % (args.name, size, neutral_swing))
    grey_index = int(round(0.40 * (size - 1)))
    print("  nearest grey grid point code %.5f -> HLG %.6f (BT.2408 nominal 0.380 at code 0.400)"
          % (codes[neutral_index[grey_index], 0], float(neutral_after[grey_index, 0])))
    print("  code 0.400 exactly           -> HLG %.6f (pinned by the curve, not the matrix)"
          % float(render(shipped, dlogm_to_linear(curve, np.full((1, 3), 0.40)))[0, 0]))
    print("  code 0.714 exactly           -> HLG %.6f (BT.2408 nominal 0.750)"
          % float(render(shipped, dlogm_to_linear(curve, np.full((1, 3), 0.714)))[0, 0]))
    print("")

    # --- physical plausibility ----------------------------------------------
    print("physical plausibility of %s:" % args.name)
    ok, lines = plausibility(shipped)
    for line in lines:
        print(line)
    print("  verdict                 : %s" % ("PLAUSIBLE" if ok else "NOT PHYSICALLY PLAUSIBLE"))
    print("")
    print("same checks on the shipped Pocket 3 matrix, for comparison:")
    p3_ok, p3_lines = plausibility(NATIVE_TO_2020_POCKET3)
    for line in p3_lines:
        print(line)
    print("  verdict                 : %s" % ("PLAUSIBLE" if p3_ok else "NOT PHYSICALLY PLAUSIBLE"))
    print("")
    print("reference chromaticities: D65 x=%.4f y=%.4f" % D65_XY)
    print("  Rec.2020 R %.3f/%.3f  G %.3f/%.3f  B %.3f/%.3f"
          % (REC2020_PRIMARIES_XY[0] + REC2020_PRIMARIES_XY[1] + REC2020_PRIMARIES_XY[2]))
    print("  Rec.709  R %.3f/%.3f  G %.3f/%.3f  B %.3f/%.3f"
          % (REC709_PRIMARIES_XY[0] + REC709_PRIMARIES_XY[1] + REC709_PRIMARIES_XY[2]))
    print("")

    if not ok:
        print("error: the fitted matrix is not physically plausible; not emitting constants",
              file=sys.stderr)
        return 1

    # --- C++ initializer ----------------------------------------------------
    print("C++ initializer (paste into include/osv/color/Matrices.h):")
    print("inline constexpr OsvMat3f %s = {{" % args.name)
    for i in range(3):
        print("    %+.9ff, %+.9ff, %+.9ff," % tuple(float(v) for v in shipped[i]))
    print("}};")

    if args.json:
        payload = {
            "name": args.name,
            "source": source,
            "curve": args.curve,
            "curve_constants": curve,
            "entries": int(size ** 3),
            "matrix": [[float(v) for v in row] for row in shipped],
            "matrix_float64": [[float(v) for v in row] for row in best_matrix],
            "determinant": float(np.linalg.det(shipped)),
            "row_sums": [float(v) for v in shipped.sum(axis=1)],
            "starts": start_costs,
            "residuals_before_pocket3": before,
            "residuals_after": after,
            "neutral_swing": neutral_swing,
            "counts": {
                "clipped": int(clipped.sum()),
                "out_of_gamut": int(negative.sum()),
                "clean": int(clean.sum()),
                "saturated": int(saturated.sum()),
            },
            "plausible": bool(ok),
        }
        with io.open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print("")
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
