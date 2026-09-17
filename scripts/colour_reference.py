#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
colour_reference.py - float64 reference evaluation of the colour pipeline.

Writes tests/golden/colour_reference.json, an independent numpy evaluation of
every transfer function, both D-Log M curves and the grey-axis pipeline that
tests/unit/test_color.cpp compares the float kernel math against.

The D-Log M constants are parsed from include/osv/color/DlogM.h so this file
and the header can never drift apart: the JSON records the parsed constants
and the C++ test asserts the header still holds the same numbers.

Windows-console safe: ASCII output only, UTF-8 file IO.

Usage:
    python scripts/colour_reference.py [--out tests/golden/colour_reference.json]
"""

import argparse
import json
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLOGM_HEADER = os.path.join(ROOT, "include", "osv", "color", "DlogM.h")

# --- BT.2100 / ST 2084 / BT.709 constants ------------------------------------
HLG_A, HLG_B, HLG_C = 0.17883277, 0.28466892, 0.55991073
PQ_M1, PQ_M2 = 0.1593017578125, 78.84375
PQ_C1, PQ_C2, PQ_C3 = 0.8359375, 18.8515625, 18.6875
SCENE_SCALE = 0.2674
PEAK_NITS = 1000.0
OOTF_GAMMA = 1.2
SDR_PEAK_NITS = 100.0

# --- matrices (row-major) -----------------------------------------------------
NATIVE_TO_2020 = np.array([[0.785301, 0.178838, 0.035860],
                           [-0.036655, 1.258089, -0.221434],
                           [-0.014322, 0.077260, 0.937062]])
REC2020_TO_709 = np.array([[1.660491, -0.587641, -0.072850],
                           [-0.124550, 1.132900, -0.008349],
                           [-0.018151, -0.100579, 1.118730]])
REC709_TO_2020 = np.array([[0.627404, 0.329283, 0.043313],
                           [0.069097, 0.919540, 0.011362],
                           [0.016391, 0.088013, 0.895595]])
IDENTITY = np.eye(3)


# -----------------------------------------------------------------------------
#  Transfer functions (float64, scalar or array)
# -----------------------------------------------------------------------------
def hlg_oetf(e):
    e = np.maximum(np.asarray(e, dtype=np.float64), 0.0)
    return np.where(e <= 1.0 / 12.0, np.sqrt(3.0 * e), HLG_A * np.log(np.maximum(12.0 * e - HLG_B, 1e-300)) + HLG_C)


def hlg_inverse_oetf(ep):
    ep = np.maximum(np.asarray(ep, dtype=np.float64), 0.0)
    return np.where(ep <= 0.5, ep * ep / 3.0, (np.exp((ep - HLG_C) / HLG_A) + HLG_B) / 12.0)


def pq_inverse_eotf(nits):
    y = np.clip(np.asarray(nits, dtype=np.float64) / 10000.0, 0.0, 1.0)
    ym1 = np.power(y, PQ_M1)
    return np.power((PQ_C1 + PQ_C2 * ym1) / (1.0 + PQ_C3 * ym1), PQ_M2)


def pq_eotf(code):
    ep = np.power(np.clip(np.asarray(code, dtype=np.float64), 0.0, 1.0), 1.0 / PQ_M2)
    num = np.maximum(ep - PQ_C1, 0.0)
    den = PQ_C2 - PQ_C3 * ep
    return np.power(num / den, 1.0 / PQ_M1) * 10000.0


def rec709_oetf(e):
    e = np.clip(np.asarray(e, dtype=np.float64), 0.0, 1.0)
    return np.where(e < 0.018, 4.5 * e, 1.099 * np.power(np.maximum(e, 1e-300), 0.45) - 0.099)


def rec709_inverse_oetf(v):
    v = np.clip(np.asarray(v, dtype=np.float64), 0.0, 1.0)
    return np.where(v < 0.081, v / 4.5, np.power((v + 0.099) / 1.099, 1.0 / 0.45))


def bt2390_eetf(pq, src_peak, dst_peak):
    pq = np.asarray(pq, dtype=np.float64)
    if dst_peak >= src_peak:
        return pq.copy()
    src_max = float(pq_inverse_eotf(src_peak))
    e1 = np.clip(pq / src_max, 0.0, 1.0)
    max_lum = float(pq_inverse_eotf(dst_peak)) / src_max
    ks = 1.5 * max_lum - 0.5
    t = np.clip((e1 - ks) / (1.0 - ks), 0.0, 1.0)
    t2, t3 = t * t, t * t * t
    spline = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum
    e2 = np.where(e1 > ks, spline, e1)
    return np.clip(e2, 0.0, max_lum) * src_max


# -----------------------------------------------------------------------------
#  D-Log M curve
# -----------------------------------------------------------------------------
def parse_curve(header_text, name):
    """Extract the OsvDlogMCurve initializer `name` from DlogM.h."""
    m = re.search(r"OsvDlogMCurve\s+" + re.escape(name) + r"\s*=\s*\{(.*?)\};", header_text, re.S)
    if not m:
        raise RuntimeError("constant %s not found in %s" % (name, DLOGM_HEADER))
    body = m.group(1)
    # Drop comments, then split on commas.
    body = re.sub(r"//[^\n]*", "", body)
    fields = [f.strip() for f in body.split(",") if f.strip()]
    if len(fields) != 9:
        raise RuntimeError("expected 9 fields for %s, got %d" % (name, len(fields)))
    values = []
    for f in fields[:8]:
        # Allow simple expressions like "0.18f / 12.4054f".
        expr = f.replace("f", "")
        values.append(float(eval(expr, {"__builtins__": {}}, {})))
    cut_mode = 1 if "INTERSECTION" in fields[8] else 0
    keys = ["xShift", "yShift", "scale", "slope", "slope2", "intercept", "midGrayScaling", "cut"]
    curve = dict(zip(keys, values))
    curve["cutMode"] = cut_mode
    # Store the float32-rounded values, which is what the C++ code really uses.
    for k in keys:
        curve[k] = float(np.float32(curve[k]))
    return curve


def curve_cut(c):
    if c["cutMode"] == 1 and abs(c["slope2"] - c["slope"]) > 1e-12:
        return c["intercept"] / (c["slope2"] - c["slope"])
    return c["cut"]


def dlogm_to_linear(c, code):
    code = np.asarray(code, dtype=np.float64)
    tmp = np.exp2(c["scale"] * code + c["yShift"]) + c["xShift"]
    cut = curve_cut(c)
    pw = np.where(tmp < cut, tmp * c["slope"] + c["intercept"], tmp * c["slope2"])
    return pw * c["midGrayScaling"]


def linear_to_dlogm(c, lin):
    """Closed-form inverse used to cross-check the C++ inverse."""
    pw = np.asarray(lin, dtype=np.float64) / c["midGrayScaling"]
    cut = curve_cut(c)
    pw_at_cut = cut * c["slope"] + c["intercept"]
    tmp = np.where(pw < pw_at_cut, (pw - c["intercept"]) / c["slope"], pw / c["slope2"])
    return (np.log2(np.maximum(tmp - c["xShift"], 1e-300)) - c["yShift"]) / c["scale"]


# -----------------------------------------------------------------------------
#  Pipeline for grey inputs (mirrors osvLinearToOutput)
# -----------------------------------------------------------------------------
def linear_to_output(lin_rgb, native_to_working, working_to_output, transfer, exposure_gain=1.0):
    working = native_to_working @ np.asarray(lin_rgb, dtype=np.float64) * exposure_gain
    if transfer == "linear":
        return working_to_output @ working
    working = np.maximum(working * SCENE_SCALE, 0.0)
    if transfer == "hlg":
        return np.clip(hlg_oetf(np.maximum(working_to_output @ working, 0.0)), 0.0, 1.0)
    ys = 0.2627 * working[0] + 0.6780 * working[1] + 0.0593 * working[2]
    mult = PEAK_NITS * np.power(ys, OOTF_GAMMA - 1.0) if ys > 0 else 0.0
    nits = np.maximum(mult * working, 0.0)
    if transfer == "pq":
        return np.clip(pq_inverse_eotf(np.maximum(working_to_output @ nits, 0.0)), 0.0, 1.0)
    # rec709: the HLG signal itself is the SDR rendering (BT.2390 HLG on an
    # SDR display); the 2020 -> 709 matrix is applied in linear light first.
    return np.clip(hlg_oetf(np.maximum(working_to_output @ working, 0.0)), 0.0, 1.0)


def pipeline_grey(curve, codes, transfer):
    out = []
    for code in codes:
        lin = dlogm_to_linear(curve, code)
        rgb = linear_to_output([lin, lin, lin], NATIVE_TO_2020,
                               REC2020_TO_709 if transfer == "rec709" else IDENTITY, transfer)
        out.append([float(code), [float(v) for v in rgb]])
    return out


def pairs(xs, ys):
    return [[float(x), float(y)] for x, y in zip(np.asarray(xs).ravel(), np.asarray(ys).ravel())]


def main():
    parser = argparse.ArgumentParser(description="Generate tests/golden/colour_reference.json")
    parser.add_argument("--out", default=os.path.join(ROOT, "tests", "golden", "colour_reference.json"))
    args = parser.parse_args()

    with open(DLOGM_HEADER, "r", encoding="utf-8") as fh:
        header = fh.read()
    pocket3 = parse_curve(header, "kDlogMPocket3")
    refit = parse_curve(header, "kDlogMDjiRefit")

    codes33 = np.linspace(0.0, 1.0, 33)
    codes65 = np.linspace(0.0, 1.0, 65)
    nits_list = [0.0, 0.1, 1.0, 5.0, 10.0, 26.0, 50.0, 100.0, 203.0, 400.0, 1000.0, 4000.0, 10000.0]

    doc = {
        "_comment": "Generated by scripts/colour_reference.py (float64 numpy). Do not edit by hand.",
        "scene_scale": SCENE_SCALE,
        "hlg_oetf": pairs(codes33, hlg_oetf(codes33)) + [[1.0 / 12.0, 0.5], [1.0, 1.0]],
        "hlg_inverse_oetf": pairs(codes33, hlg_inverse_oetf(codes33)) + [[0.75, float(hlg_inverse_oetf(0.75))]],
        "pq_inverse_eotf": pairs(nits_list, pq_inverse_eotf(nits_list)),
        "pq_eotf": pairs(codes33, pq_eotf(codes33)),
        "rec709_oetf": pairs(codes33, rec709_oetf(codes33)) + [[0.018, float(rec709_oetf(0.018))]],
        "rec709_inverse_oetf": pairs(codes33, rec709_inverse_oetf(codes33)),
        "bt2390_eetf_1000_to_100": pairs(codes33, bt2390_eetf(codes33, PEAK_NITS, SDR_PEAK_NITS)),
        "matrices": {
            "native_to_rec2020_pocket3": NATIVE_TO_2020.tolist(),
            "rec2020_to_rec709": REC2020_TO_709.tolist(),
            "rec709_to_rec2020": REC709_TO_2020.tolist(),
            "row_sums": {
                "native_to_rec2020_pocket3": NATIVE_TO_2020.sum(axis=1).tolist(),
                "rec2020_to_rec709": REC2020_TO_709.sum(axis=1).tolist(),
                "rec709_to_rec2020": REC709_TO_2020.sum(axis=1).tolist(),
            },
        },
        "dlogm": {},
        "pipeline_grey": {},
    }

    for name, curve in (("pocket3", pocket3), ("dji_refit", refit)):
        lin65 = dlogm_to_linear(curve, codes65)
        dense = np.linspace(0.0, 1.0, 4097)
        lin_dense = dlogm_to_linear(curve, dense)
        hlg_dense = hlg_oetf(SCENE_SCALE * lin_dense)
        doc["dlogm"][name] = {
            "constants": curve,
            "cut": curve_cut(curve),
            "samples": pairs(codes65, lin65),
            "code_0_40": float(dlogm_to_linear(curve, 0.40)),
            "code_1_00": float(dlogm_to_linear(curve, 1.0)),
            "hlg_0_40": float(hlg_oetf(SCENE_SCALE * dlogm_to_linear(curve, 0.40))),
            "hlg_0_714": float(hlg_oetf(SCENE_SCALE * dlogm_to_linear(curve, 0.714))),
            "hlg_1_00": float(hlg_oetf(SCENE_SCALE * dlogm_to_linear(curve, 1.0))),
            "monotonic_4097": bool(np.all(np.diff(lin_dense) > 0)),
            "max_adjacent_linear_step_4097": float(np.max(np.diff(lin_dense))),
            "max_adjacent_hlg_step_4097": float(np.max(np.diff(hlg_dense))),
            "inverse_roundtrip_max_err": float(np.max(np.abs(linear_to_dlogm(curve, lin65) - codes65))),
        }
        doc["pipeline_grey"][name] = {
            "hlg": pipeline_grey(curve, codes33, "hlg"),
            "pq": pipeline_grey(curve, codes33, "pq"),
            "rec709": pipeline_grey(curve, codes33, "rec709"),
        }

    # Non-grey spot checks through the full pipeline (DJI refit, HLG and PQ).
    spots = [[0.40, 0.40, 0.40], [0.55, 0.35, 0.30], [0.30, 0.60, 0.45], [0.70, 0.70, 0.20], [0.10, 0.20, 0.90]]
    doc["pipeline_rgb_dji_refit"] = []
    for code in spots:
        lin = [float(dlogm_to_linear(refit, c)) for c in code]
        doc["pipeline_rgb_dji_refit"].append({
            "code": code,
            "hlg": [float(v) for v in linear_to_output(lin, NATIVE_TO_2020, IDENTITY, "hlg")],
            "pq": [float(v) for v in linear_to_output(lin, NATIVE_TO_2020, IDENTITY, "pq")],
            "rec709": [float(v) for v in linear_to_output(lin, NATIVE_TO_2020, REC2020_TO_709, "rec709")],
            "linear": [float(v) for v in linear_to_output(lin, NATIVE_TO_2020, IDENTITY, "linear")],
        })

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")

    # Console summary (ASCII only).
    print("wrote %s" % args.out)
    for name in ("pocket3", "dji_refit"):
        d = doc["dlogm"][name]
        print("%-10s code 0.40 -> lin %.5f (HLG %.4f); 0.714 -> HLG %.4f; 1.0 -> lin %.4f (HLG %.4f)"
              % (name, d["code_0_40"], d["hlg_0_40"], d["hlg_0_714"], d["code_1_00"], d["hlg_1_00"]))
        print("           cut %.7f monotonic %s max lin step %.3e max hlg step %.3e inverse err %.2e"
              % (d["cut"], d["monotonic_4097"], d["max_adjacent_linear_step_4097"],
                 d["max_adjacent_hlg_step_4097"], d["inverse_roundtrip_max_err"]))
    print("PQ(26 nit) = %.4f  PQ(203) = %.4f  PQ(1000) = %.4f" % (pq_inverse_eotf(26.0), pq_inverse_eotf(203.0),
                                                                pq_inverse_eotf(1000.0)))
    g = doc["pipeline_grey"]["dji_refit"]
    idx = 13  # code 13/32 = 0.40625
    print("grey code %.4f -> hlg %.4f pq %.4f 709 %.4f" % (g["hlg"][idx][0], g["hlg"][idx][1][0], g["pq"][idx][1][0],
                                                          g["rec709"][idx][1][0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
