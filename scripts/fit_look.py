#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
fit_look.py - fit OpenOSV's "DJI Studio" Rec.709 look to DJI's own D-Log M
to Rec.709 reference LUT.

Why this script exists
----------------------
DJI Studio renders an Osmo 360 D-Log M clip by applying its bundled
``DJI Osmo 360 D-Log M to Rec.709 V1.cube`` (the clip's "D-LOG M" filter,
slug ``LOG_Osmo360_DLogM``, service ``mika.lut2``, strength 1.0).  OpenOSV's
original Rec.709 output was the HLG signal in Rec.709 primaries: correct on
the neutral axis to ~0.016 of signal, but it does not reproduce that file's
*look* - its crushed toe, its S-shaped mid-tones, its highlight shoulder, its
saturation and its gamut handling.  Measured over the whole 33^3 input cube
the old rendering is 2.81 dE2000 mean / 6.57 p95 / 15.8 max away from DJI's,
and 2.15 mean / 4.86 p95 on the sample clip's real pixel distribution.

This script fits a compact parametric model - the one the kernel implements
in ``osvLookApply`` (include/osv/color/ColorMath.h) - to that reference, and
prints the constants that include/osv/color/Look.h ships.

The model
---------
For a scene-linear input (18 % grey = 0.18), stage by stage:

    x   = N * lin                          3x3, rows sum to 1 (6 free values)
    u_c = shaper(x_c)                      the kDlogMOsmo360 curve inverted:
                                           scene-linear -> D-Log M code, so a
                                           neutral input lands on the code the
                                           reference LUT is indexed by; below
                                           lin(code 0) it continues linearly
    y_c = T(u_c)                           monotone cubic Hermite through K
                                           uniform knots (Fritsch-Carlson
                                           tangents), T(0) = 0, T(1) = 1
    y   = y + a * (yh - y)                 highlight hue preservation: yh is
                                           the tone of max(x) scaled by
                                           (x_c / max)^e, a = smoothstep over
                                           the tone of max(x) (4 values)
    y   = S * y                            display-signal 3x3, rows sum to 1
    y   = compress(y)                      per-channel soft gamut compression
                                           of the distance below max(y), the
                                           ACES reference gamut compression
                                           curve (7 values)
    out = clamp(y, 0, 1)

Both matrices have unit row sums and the two non-linear colour stages are the
identity on neutrals, so the whole neutral axis is exactly T: the fit can put
18 % grey precisely where DJI puts it without any colour stage disturbing it.

Every stage is continuous (C1 except where a clamp or the smoothstep ends),
T is strictly increasing because it is parameterised by strictly positive
increments, and nothing hard-clips before the final [0, 1] clamp - the gamut
compression bends out-of-range values back in rather than cutting them.

Stages were chosen by measurement, not by taste (see the report the script
prints and docs in Look.h): a per-channel tone curve after a primaries matrix
explains the reference's shadow saturation (a D-Log M colour 3 stops under
grey renders with chroma ratios ten times wider than a ratio-preserving tone
map would give), the display matrix explains its extra saturation in the
mid-tones and the sky, the hue-preservation blend its orange-stays-orange
highlights, and the gamut compression its non-zero minor channel on colours
far outside Rec.709 ((1.0, 0.4, 0.4) renders with 0.19 of green where a
matrix and clamp give 0).  Additive-model tests in three domains show the
reference is not separable in any single domain, which is why no two-stage
(curve + matrix) model gets below ~2 dE2000 mean.

Objective
---------
Least squares in CIELAB (display: BT.1886, gamma 2.4, Rec.709 primaries,
D65), so the residual is close to perceptual, over

  * all 35937 reference entries (the whole input cube, uniform coverage), and
  * optionally a set of real D-Log M pixels (--samples), compared against the
    reference LUT trilinearly interpolated at the same codes, weighted to the
    same total as the cube,

plus a strong term that keeps the 33 neutral entries on the reference, and a
small curvature regulariser on the knots.

Provenance and licensing
------------------------
The reference LUT is measured, never redistributed.  Only the fitted model
constants leave this script (see NOTICE); OpenOSV ships only .cube files its
own generator produces from them.

Usage
-----
    python scripts/fit_look.py
    python scripts/fit_look.py --cube "<path to DJI Osmo 360 D-Log M to Rec.709 V1.cube>"
    python scripts/fit_look.py --samples frame0.tif frame20.tif --json look.json

``--samples`` takes 16-bit TIFFs rendered with
``osvtool render <clip> --mode equirect --color dlogm --out frame.tif`` (read
with OpenCV when it is installed) or ``.npy`` arrays of D-Log M codes (N x 3).
Equirect rows are weighted by cos(latitude) so the poles do not dominate.

Output is 7-bit ASCII so it is safe on a legacy Windows console.
"""

import argparse
import io
import json
import os
import re
import sys
import time

import numpy as np

# Windows consoles default to a legacy code page; every string this script
# prints is 7-bit ASCII, but reconfiguring stdout makes that robust.
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):  # pragma: no cover - exotic/redirected stdout
        pass

SCRIPTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPTS)
sys.path.insert(0, SCRIPTS)

# The cube reader, the header parser and the shipped forward model are shared
# with fit_primaries.py so the three fits can never disagree about them.
import fit_primaries as fp  # noqa: E402

MATRICES_HEADER = os.path.join(ROOT, "include", "osv", "color", "Matrices.h")

# DJI Studio installs the reference beside its other camera LUTs.
DEFAULT_CUBE = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "DJI Studio",
                            "1.0.0.24724", "filter", "LUT", "LOG_Osmo360_DLogM",
                            "DJI Osmo 360 D-Log M to Rec.709 V1.cube")

# Number of tone knots the kernel stores (OSV_LOOK_MAX_KNOTS in ColorMath.h).
MAX_KNOTS = 17

# Smallest increment between two tone knots: keeps T strictly increasing (and
# hence invertible), which a flat toe would not be.
MIN_KNOT_STEP = 1e-3

# Weight of the 33 neutral entries (signal units) against the whole-cube Lab
# residual.  The neutral axis is where a tone error is most visible (clouds,
# the sun, anything white).  Measured trade-off on the Osmo 360 reference with
# the sample clip's pixels: weight 20 leaves the neutral axis 0.76 dE2000 off
# at worst (samples 0.47 mean), 100 holds it to 0.11 but costs the samples
# 0.13 dE mean (0.59), and 40 holds it to 0.42 worst / 0.006 of signal for
# 0.03 of sample dE (0.50) - the balance shipped.
NEUTRAL_WEIGHT = 40.0

# Rec.709 / BT.1886 display for the perceptual metric.
DISPLAY_GAMMA = 2.4
RGB709_TO_XYZ = np.array([[0.4123908, 0.3575843, 0.1804808],
                          [0.2126390, 0.7151687, 0.0721923],
                          [0.0193308, 0.1191948, 0.9505322]])
WHITE_XYZ = RGB709_TO_XYZ.sum(axis=1)


# -----------------------------------------------------------------------------
#  Header parsing
# -----------------------------------------------------------------------------
def parse_matrix(name, path=MATRICES_HEADER):
    """
    Pull one ``OsvMat3f`` initializer out of include/osv/color/Matrices.h.

    Used for the *baseline* (the currently shipped standard rendering), so the
    before/after numbers are always quoted against what the header really
    holds rather than a copy that could go stale.
    """
    if not os.path.isfile(path):
        raise ValueError("matrix header not found: %r" % (path,))
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    match = re.search(r"OsvMat3f\s+" + re.escape(name) + r"\s*=\s*\{\{(.*?)\}\};", text, re.S)
    if match is None:
        raise ValueError("%s: no initializer for %s" % (path, name))
    body = re.sub(r"//[^\n]*", "", match.group(1))
    values = [float(v.strip().rstrip("f")) for v in body.split(",") if v.strip()]
    if len(values) != 9:
        raise ValueError("%s: %s has %d values, expected 9" % (path, name, len(values)))
    return np.array(values, dtype=np.float64).reshape(3, 3)


# -----------------------------------------------------------------------------
#  Perceptual metric
# -----------------------------------------------------------------------------
def signal_to_lab(sig):
    """Rec.709 display signal -> CIELAB (BT.1886 gamma 2.4, D65 white)."""
    lin = np.clip(sig, 0.0, 1.0) ** DISPLAY_GAMMA
    xyz = lin @ RGB709_TO_XYZ.T
    r = xyz / WHITE_XYZ
    eps = (6.0 / 29.0) ** 3
    f = np.where(r > eps, np.cbrt(r), r / (3.0 * (6.0 / 29.0) ** 2) + 4.0 / 29.0)
    return np.stack([116.0 * f[..., 1] - 16.0,
                     500.0 * (f[..., 0] - f[..., 1]),
                     200.0 * (f[..., 1] - f[..., 2])], axis=-1)


def delta_e2000(lab1, lab2):
    """CIEDE2000 (Sharma, Wu, Dalal 2005), vectorised."""
    l1, a1, b1 = lab1[..., 0], lab1[..., 1], lab1[..., 2]
    l2, a2, b2 = lab2[..., 0], lab2[..., 1], lab2[..., 2]
    c1 = np.hypot(a1, b1)
    c2 = np.hypot(a2, b2)
    cbar7 = ((c1 + c2) / 2.0) ** 7
    g = 0.5 * (1.0 - np.sqrt(cbar7 / (cbar7 + 25.0 ** 7)))
    a1p = (1.0 + g) * a1
    a2p = (1.0 + g) * a2
    c1p = np.hypot(a1p, b1)
    c2p = np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360.0
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360.0
    dlp = l2 - l1
    dcp = c2p - c1p
    dh = h2p - h1p
    dh = np.where(c1p * c2p == 0, 0.0, np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh)))
    dhp = 2.0 * np.sqrt(c1p * c2p) * np.sin(np.radians(dh / 2.0))
    lbp = (l1 + l2) / 2.0
    cbp = (c1p + c2p) / 2.0
    hs = h1p + h2p
    hbp = np.where(c1p * c2p == 0, hs,
                   np.where(np.abs(h1p - h2p) <= 180, hs / 2.0, np.where(hs < 360, (hs + 360) / 2.0, (hs - 360) / 2.0)))
    t = (1 - 0.17 * np.cos(np.radians(hbp - 30)) + 0.24 * np.cos(np.radians(2 * hbp))
         + 0.32 * np.cos(np.radians(3 * hbp + 6)) - 0.20 * np.cos(np.radians(4 * hbp - 63)))
    dtheta = 30.0 * np.exp(-((hbp - 275.0) / 25.0) ** 2)
    rc = 2.0 * np.sqrt(cbp ** 7 / (cbp ** 7 + 25.0 ** 7))
    sl = 1 + 0.015 * (lbp - 50) ** 2 / np.sqrt(20 + (lbp - 50) ** 2)
    sc = 1 + 0.045 * cbp
    sh = 1 + 0.015 * cbp * t
    rt = -np.sin(np.radians(2 * dtheta)) * rc
    return np.sqrt((dlp / sl) ** 2 + (dcp / sc) ** 2 + (dhp / sh) ** 2 + rt * (dcp / sc) * (dhp / sh))


def summary(de, weights=None):
    """mean / median / p95 / p99 / max of a dE array (optionally weighted)."""
    de = np.asarray(de, dtype=np.float64)
    if weights is None:
        return {"mean": float(de.mean()), "median": float(np.median(de)), "p95": float(np.percentile(de, 95)),
                "p99": float(np.percentile(de, 99)), "max": float(de.max())}
    w = np.asarray(weights, dtype=np.float64)
    w = w / w.sum()
    order = np.argsort(de)
    cum = np.cumsum(w[order])

    def q(p):
        return float(de[order][min(np.searchsorted(cum, p), len(de) - 1)])
    return {"mean": float((de * w).sum()), "median": q(0.5), "p95": q(0.95), "p99": q(0.99),
            "max": float(de[w > 0].max())}


def fmt(stats):
    """Compact one-line rendering of a summary() dict."""
    return "mean %.3f  median %.3f  p95 %.3f  p99 %.3f  max %.3f" % (
        stats["mean"], stats["median"], stats["p95"], stats["p99"], stats["max"])


# -----------------------------------------------------------------------------
#  Reference access
# -----------------------------------------------------------------------------
def trilinear(table, size, rgb):
    """Trilinear lookup into a .cube table (red fastest), as DJI Studio samples it."""
    t = table.reshape(size, size, size, 3)
    x = np.clip(rgb, 0.0, 1.0) * (size - 1)
    i0 = np.minimum(np.floor(x).astype(int), size - 2)
    f = x - i0
    r0, g0, b0 = i0[..., 0], i0[..., 1], i0[..., 2]
    fr, fg, fb = f[..., 0:1], f[..., 1:2], f[..., 2:3]

    def at(db, dg, dr):
        return t[b0 + db, g0 + dg, r0 + dr]
    c00 = at(0, 0, 0) * (1 - fr) + at(0, 0, 1) * fr
    c01 = at(0, 1, 0) * (1 - fr) + at(0, 1, 1) * fr
    c10 = at(1, 0, 0) * (1 - fr) + at(1, 0, 1) * fr
    c11 = at(1, 1, 0) * (1 - fr) + at(1, 1, 1) * fr
    c0 = c00 * (1 - fg) + c01 * fg
    c1 = c10 * (1 - fg) + c11 * fg
    return c0 * (1 - fb) + c1 * fb


def load_samples(paths, per_file, seed):
    """
    Real D-Log M code triples from equirect TIFF renders or .npy arrays.

    Equirect rows cover less solid angle towards the poles, so rows are drawn
    with probability proportional to cos(latitude); a plain uniform draw would
    let the stretched nadir and zenith dominate the distribution.
    """
    rng = np.random.default_rng(seed)
    out = []
    for path in paths:
        if not os.path.isfile(path):
            raise ValueError("sample file not found: %r" % (path,))
        if path.lower().endswith(".npy"):
            codes = np.load(path).astype(np.float64).reshape(-1, 3)
            weights = np.full(len(codes), 1.0 / len(codes))
        else:
            try:
                import cv2  # noqa: WPS433 - optional dependency, only for TIFF input
            except ImportError:
                raise ValueError("reading %s needs OpenCV (pip install opencv-python) or pass a .npy" % path)
            img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
            if img is None or img.ndim != 3 or img.shape[2] < 3:
                raise ValueError("%s: not an RGB image" % path)
            scale = 65535.0 if img.dtype == np.uint16 else 255.0
            h, w = img.shape[:2]
            codes = img[..., 2::-1].reshape(-1, 3).astype(np.float64) / scale  # BGR -> RGB
            lat = (0.5 - (np.arange(h) + 0.5) / h) * np.pi
            weights = np.repeat(np.cos(lat), w)
            weights /= weights.sum()
        take = min(per_file, len(codes))
        idx = rng.choice(len(codes), take, replace=False, p=weights)
        out.append(codes[idx])
    return np.concatenate(out) if out else np.zeros((0, 3))


# -----------------------------------------------------------------------------
#  The model (mirrors osvLookApply in include/osv/color/ColorMath.h)
# -----------------------------------------------------------------------------
class Shaper:
    """
    Scene-linear -> D-Log M code, the inverse of the shipped kDlogMOsmo360.

    Above lin(code 0) this is the closed-form inverse of both branches; below
    it the curve continues along the tangent at code 0, so the shaper is
    defined (and C1) for every real input, including the negative values a
    wide-gamut matrix produces.  osvDlogmToCode + osvLookShaper do the same.
    """

    def __init__(self, curve):
        self.c = curve
        self.cut = curve["intercept"] / (curve["slope2"] - curve["slope"])
        self.x0 = float(fp.dlogm_to_linear(curve, 0.0))
        # d(lin)/d(code) at code 0 on the toe branch (the branch code 0 is on
        # for every shipped curve: tmp(0) = 2^yShift + xShift < cut).
        tmp0 = 2.0 ** curve["y_shift"] + curve["x_shift"]
        slope_branch = curve["slope"] if tmp0 < self.cut else curve["slope2"]
        self.dlin_dcode0 = (np.log(2.0) * curve["scale"] * 2.0 ** curve["y_shift"]
                            * slope_branch * curve["mid_gray_scaling"])

    def __call__(self, x):
        c = self.c
        pw = x / c["mid_gray_scaling"]
        lin_cut = self.cut * c["slope2"]
        tmp = np.where(pw < lin_cut, (pw - c["intercept"]) / c["slope"], pw / c["slope2"])
        arg = np.maximum(tmp - c["x_shift"], 1e-30)
        u = (np.log2(arg) - c["y_shift"]) / c["scale"]
        return np.where(x >= self.x0, u, (x - self.x0) / self.dlin_dcode0)


def fc_tangents(v):
    """Fritsch-Carlson monotone tangents on a uniform knot grid over [0, 1]."""
    k = len(v)
    h = 1.0 / (k - 1)
    d = np.diff(v) / h
    m = np.zeros(k)
    m[0] = d[0]
    m[-1] = d[-1]
    for i in range(1, k - 1):
        # Harmonic mean of the neighbouring secants (0 at a local extremum):
        # the classic choice that keeps a monotone data set monotone.
        m[i] = 0.0 if d[i - 1] * d[i] <= 0.0 else 2.0 / (1.0 / d[i - 1] + 1.0 / d[i])
    return m


def tone(v, m, u):
    """Cubic Hermite through (k/(K-1), v_k) with tangents m; linear outside [0, 1]."""
    k = len(v)
    h = 1.0 / (k - 1)
    uc = np.clip(u, 0.0, 1.0)
    i = np.minimum((uc / h).astype(int), k - 2)
    t = uc / h - i
    t2 = t * t
    t3 = t2 * t
    y = ((2 * t3 - 3 * t2 + 1) * v[i] + (t3 - 2 * t2 + t) * h * m[i]
         + (-2 * t3 + 3 * t2) * v[i + 1] + (t3 - t2) * h * m[i + 1])
    y = np.where(u < 0.0, v[0] + m[0] * u, y)
    return np.where(u > 1.0, v[-1] + m[-1] * (u - 1.0), y)


def unit_rows(a):
    """3x3 from six values: the third column of each row is 1 - a - b."""
    return np.array([[a[0], a[1], 1 - a[0] - a[1]],
                     [a[2], a[3], 1 - a[2] - a[3]],
                     [a[4], a[5], 1 - a[4] - a[5]]])


def gamut_scale(thr, lim, power):
    """ACES RGC scale so that distance `lim` lands exactly on 1 after compression."""
    return (lim - thr) / np.power(np.power((1.0 - thr) / (lim - thr), -power) - 1.0, 1.0 / power)


class LookModel:
    """Parameter vector <-> named look constants, and the forward model."""

    def __init__(self, knots, shaper):
        self.knots = knots
        self.shaper = shaper
        self.layout = [("N", 6), ("steps", knots - 2), ("hue", 4), ("S", 6), ("gamut", 7)]

    def unpack(self, p):
        out = {}
        i = 0
        for name, count in self.layout:
            out[name] = np.asarray(p[i:i + count])
            i += count
        # Knots: strictly increasing from T(0) = 0; the last one is pinned to 1
        # so the neutral axis reaches exactly white at code 1.0, as DJI's does.
        steps = np.maximum(out["steps"], MIN_KNOT_STEP)
        v = np.concatenate([[0.0], np.cumsum(steps)])
        v = np.concatenate([v, [max(1.0, v[-1] + MIN_KNOT_STEP)]])
        out["knots"] = v
        out["N3"] = unit_rows(out["N"])
        out["S3"] = unit_rows(out["S"])
        return out

    def forward(self, p, lin, q=None):
        q = self.unpack(p) if q is None else q
        v = q["knots"]
        m = fc_tangents(v)
        x = lin @ q["N3"].T
        y = tone(v, m, self.shaper(x))
        # Highlight hue preservation.
        start, width, amount, expo = q["hue"]
        nrm = np.max(x, axis=-1, keepdims=True)
        pos = nrm > 1e-9
        tn = tone(v, m, self.shaper(np.where(pos, nrm, 1e-9)))
        ratio = np.clip(x / np.where(pos, nrm, 1.0), 0.0, 1.0)
        yh = tn * np.power(ratio, expo)
        a = np.clip((tn - start) / width, 0.0, 1.0)
        a = np.where(pos, a * a * (3.0 - 2.0 * a) * amount, 0.0)
        y = y + a * (yh - y)
        # Display matrix.
        y = y @ q["S3"].T
        # Soft gamut compression of the distance below max(y).
        g = q["gamut"]
        thr, lim, power = g[0:3], g[3:6], g[6]
        scale = gamut_scale(thr, lim, power)
        ach = np.max(y, axis=-1, keepdims=True)
        ok = ach > 1e-6
        safe = np.where(ok, ach, 1.0)
        dist = (ach - y) / safe
        over = np.maximum(dist - thr, 0.0) / scale
        comp = np.where(dist > thr, thr + (dist - thr) / np.power(1.0 + np.power(over, power), 1.0 / power), dist)
        y = np.where(ok, ach - comp * safe, y)
        return np.clip(y, 0.0, 1.0)

    def initial(self, neutral_codes, neutral_values, n_seed):
        """Start: the shipped matrix, DJI's neutral axis, identity colour stages."""
        from scipy.interpolate import PchipInterpolator
        neu = PchipInterpolator(neutral_codes, neutral_values)
        v = neu(np.linspace(0.0, 1.0, self.knots))
        steps = np.maximum(np.diff(v)[:-1], 2.0 * MIN_KNOT_STEP)
        p = [n_seed[0, 0], n_seed[0, 1], n_seed[1, 0], n_seed[1, 1], n_seed[2, 0], n_seed[2, 1]]
        lo = [-3.0] * 6
        hi = [3.0] * 6
        p += list(steps)
        lo += [MIN_KNOT_STEP] * len(steps)
        hi += [0.5] * len(steps)
        p += [0.5, 0.15, 0.4, 1.0 / 2.4]
        lo += [0.0, 0.02, 0.0, 0.1]
        hi += [1.2, 1.0, 1.0, 1.5]
        p += [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]
        lo += [-3.0] * 6
        hi += [3.0] * 6
        p += [0.8, 0.8, 0.8, 1.3, 1.3, 1.3, 1.5]
        lo += [0.1] * 3 + [1.001] * 3 + [1.0]
        hi += [0.999] * 3 + [4.0] * 3 + [10.0]
        p = np.clip(np.array(p), np.array(lo) + 1e-9, np.array(hi) - 1e-9)
        return p, np.array(lo), np.array(hi)


# -----------------------------------------------------------------------------
#  Checks
# -----------------------------------------------------------------------------
def monotonic_report(model, p):
    """
    Neutral axis strictly increasing, and exposure ramps never darkening.

    The second check is the one that matters for a look with colour stages: a
    hue scaled up in scene-linear light must never render *darker* (in BT.709
    luminance) than the same hue one step less exposed.
    """
    q = model.unpack(p)
    codes = np.linspace(0.0, 1.0, 4097)
    lin = fp.dlogm_to_linear(model.shaper.c, codes)
    grey = model.forward(p, np.stack([lin] * 3, axis=1), q)[:, 1]
    neutral_ok = bool(np.all(np.diff(grey[grey < 1.0]) > 0.0))
    rng = np.random.default_rng(7)
    worst = 0.0
    luma = np.array([0.2126, 0.7152, 0.0722])
    for _ in range(400):
        base = rng.uniform(0.02, 1.0, 3)
        stops = np.linspace(-9.0, 5.0, 561)
        ramp = base[None, :] * 0.18 * np.exp2(stops)[:, None]
        # Rendered luminance in display-linear light.
        y = model.forward(p, ramp, q) ** DISPLAY_GAMMA @ luma
        worst = max(worst, float(np.max(-np.diff(y))))
    return neutral_ok, worst


# -----------------------------------------------------------------------------
#  Output
# -----------------------------------------------------------------------------
def cpp_float(v):
    """A C++ float literal with enough digits to round-trip a float32."""
    return "%.9ef" % float(v)


def emit_cpp(q):
    """Print the constants Look.h ships, in its initializer order."""
    lines = ["// ---- generated by scripts/fit_look.py -------------------------------"]
    lines.append("toNative-look matrix N (rows sum to 1):")
    for r in q["N3"]:
        lines.append("    %s, %s, %s," % tuple(cpp_float(x) for x in r))
    lines.append("tone knots (%d):" % len(q["knots"]))
    for i in range(0, len(q["knots"]), 4):
        lines.append("    " + " ".join(cpp_float(x) + "," for x in q["knots"][i:i + 4]))
    lines.append("hue: start, width, amount, exponent:")
    lines.append("    " + ", ".join(cpp_float(x) for x in q["hue"]) + ",")
    lines.append("display matrix S (rows sum to 1):")
    for r in q["S3"]:
        lines.append("    %s, %s, %s," % tuple(cpp_float(x) for x in r))
    g = q["gamut"]
    lines.append("gamut threshold / limit / power:")
    lines.append("    {%s, %s, %s}," % tuple(cpp_float(x) for x in g[0:3]))
    lines.append("    {%s, %s, %s}," % tuple(cpp_float(x) for x in g[3:6]))
    lines.append("    %s," % cpp_float(g[6]))
    print("\n".join(lines))


# -----------------------------------------------------------------------------
#  Main
# -----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--cube", default=DEFAULT_CUBE, help="DJI Osmo 360 D-Log M -> Rec.709 reference .cube")
    ap.add_argument("--samples", nargs="*", default=[], help="D-Log M TIFF renders or .npy code arrays")
    ap.add_argument("--per-file", type=int, default=60000, help="pixels drawn from each sample file")
    ap.add_argument("--fit-pixels", type=int, default=20000, help="sample pixels used inside the fit")
    ap.add_argument("--knots", type=int, default=MAX_KNOTS, help="tone knots (2..%d)" % MAX_KNOTS)
    ap.add_argument("--max-nfev", type=int, default=600, help="solver evaluation budget per pass")
    ap.add_argument("--restarts", type=int, default=8, help="extra solver passes from the last solution")
    ap.add_argument("--json", help="write the fitted constants and the report here")
    args = ap.parse_args()

    if not 2 < args.knots <= MAX_KNOTS:
        ap.error("--knots must be in 3..%d" % MAX_KNOTS)
    try:
        from scipy.optimize import least_squares
    except ImportError:
        print("error: this fit needs scipy (pip install scipy)")
        return 2

    # ---- reference and the shipped curve / matrix ---------------------------
    try:
        codes, table, size, title = fp.read_cube(args.cube)
        curve = fp.parse_dlogm_curve("kDlogMOsmo360")
        m_osmo = parse_matrix("kNativeToRec2020_Osmo360")
        samples = load_samples(args.samples, args.per_file, seed=1)
    except ValueError as exc:
        print("error: %s" % exc)
        return 2
    print("reference: %s (%d^3, %s)" % (args.cube, size, title or "no TITLE"))
    shaper = Shaper(curve)
    lin = fp.dlogm_to_linear(curve, codes)
    ref_lab = signal_to_lab(table)
    neutral = [i * (1 + size + size * size) for i in range(size)]

    # ---- baseline: the shipped standard rendering ---------------------------
    base = fp.render(m_osmo, lin)
    base_de = delta_e2000(ref_lab, signal_to_lab(base))
    report = {"reference": os.path.basename(args.cube), "cube_before": summary(base_de)}
    print("\nstandard rendering vs reference, whole cube : " + fmt(report["cube_before"]))
    if len(samples):
        s_ref = trilinear(table, size, samples)
        s_lin = fp.dlogm_to_linear(curve, samples)
        s_base = delta_e2000(signal_to_lab(s_ref), signal_to_lab(fp.render(m_osmo, s_lin)))
        report["samples_before"] = summary(s_base)
        print("standard rendering vs reference, samples    : " + fmt(report["samples_before"]))

    # ---- fit ---------------------------------------------------------------
    model = LookModel(args.knots, shaper)
    rec709 = fp.REC2020_TO_709
    p0, lo, hi = model.initial(np.linspace(0.0, 1.0, size), table[neutral, 1], rec709 @ m_osmo)
    rng = np.random.default_rng(0)
    if len(samples):
        pick = rng.choice(len(samples), min(args.fit_pixels, len(samples)), replace=False)
        f_lin = s_lin[pick]
        f_lab = signal_to_lab(s_ref[pick])
        f_weight = np.sqrt(len(codes) / float(len(pick)))
    ref_neutral = table[neutral]

    def residuals(p):
        q = model.unpack(p)
        out = model.forward(p, lin, q)
        parts = [(signal_to_lab(out) - ref_lab).ravel() * 0.01]
        if len(samples):
            parts.append((signal_to_lab(model.forward(p, f_lin, q)) - f_lab).ravel() * 0.01 * f_weight)
        parts.append(np.diff(q["knots"], 2) * 0.3)
        parts.append((out[neutral] - ref_neutral).ravel() * NEUTRAL_WEIGHT)
        return np.concatenate(parts)

    # The clamps make the objective piecewise, so the trust region regularly
    # stops on a step-size criterion well before the real optimum.  Restarting
    # from the last solution (a fresh trust radius) keeps descending; stop once
    # a restart gains less than 0.1 % of the cost.
    t0 = time.time()
    p = p0
    best = None
    evaluations = 0
    for restart in range(args.restarts + 1):
        res = least_squares(residuals, p, bounds=(lo, hi), method="trf", x_scale="jac",
                            max_nfev=args.max_nfev, diff_step=1e-6, ftol=1e-12, xtol=1e-12, gtol=1e-12)
        evaluations += res.nfev
        improved = best is None or res.cost < best * (1.0 - 1e-3)
        if best is None or res.cost < best:
            best = res.cost
            p = res.x
        print("  pass %d: cost %.5f (status %d)" % (restart, res.cost, res.status))
        if not improved:
            break
    print("\nfit: %d evaluations, %.1f s, cost %.5f" % (evaluations, time.time() - t0, best))
    q = model.unpack(p)

    # ---- after -------------------------------------------------------------
    after = model.forward(p, lin, q)
    after_de = delta_e2000(ref_lab, signal_to_lab(after))
    report["cube_after"] = summary(after_de)
    print("DJI look vs reference, whole cube           : " + fmt(report["cube_after"]))
    report["neutral_after"] = summary(after_de[neutral])
    print("DJI look vs reference, neutral axis         : " + fmt(report["neutral_after"]))
    if len(samples):
        s_after = delta_e2000(signal_to_lab(s_ref), signal_to_lab(model.forward(p, s_lin, q)))
        report["samples_after"] = summary(s_after)
        print("DJI look vs reference, samples              : " + fmt(report["samples_after"]))

    print("\nneutral axis (code -> reference / look):")
    for i in range(0, size, 2):
        print("  %.5f  %.4f  %.4f" % (codes[neutral[i], 0], table[neutral[i], 1], after[neutral[i], 1]))

    neutral_ok, worst_drop = monotonic_report(model, p)
    report["neutral_strictly_increasing"] = neutral_ok
    report["worst_exposure_ramp_drop"] = worst_drop
    print("\nneutral axis strictly increasing: %s; worst luminance drop along 400 exposure ramps: %.2e"
          % ("yes" if neutral_ok else "NO", worst_drop))

    print("")
    emit_cpp(q)
    if args.json:
        report["constants"] = {"N": q["N3"].tolist(), "knots": q["knots"].tolist(), "hue": q["hue"].tolist(),
                               "S": q["S3"].tolist(), "gamut": q["gamut"].tolist()}
        with io.open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=1)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
