#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""Generate tests/golden/lens_tables.json from an independent numpy
implementation of the five-term Kannala-Brandt lens model.

The C++ implementation (osv::geom::KannalaBrandt5) is checked against these
numbers, so nothing here may import or call the library.  Output is ASCII
only so the script behaves on a Windows console.

Contents of the golden file:
  lenses      : the two sample-clip calibrations (calibration pixel units)
  theta_table : theta (deg, 0..100 step 0.25) -> theta_d for both lenses
  unproject   : 200 random pixels per lens with the expected ray angle and
                direction, solved by bisection to ~1e-15 rad
"""

import json
import os
import sys

import numpy as np

# Sample clip calibration (native_refine slots 1 and 2), calibration pixels.
LENSES = {
    "slave": {
        "fx": 1043.8802,
        "fy": 1043.6731,
        "cx": 1917.0421,
        "cy": 1919.1294,
        "k": [0.0667397, -0.0128859, 0.0103815, -0.00677581, 0.00098791],
    },
    "master": {
        "fx": 1043.0103,
        "fy": 1042.9268,
        "cx": 1908.8036,
        "cy": 1918.7257,
        "k": [0.0613421, -0.00480161, 0.00444291, -0.00452633, 0.00066212],
    },
}

THETA_STEP_DEG = 0.25
THETA_MAX_DEG = 100.0
RANDOM_TARGETS = 200
RANDOM_SEED = 20260916
# Random targets stay inside the region where both lenses are monotonic
# (calibration px from the principal point).
TARGET_MAX_RADIUS_PX = 1850.0
# Bisection bracket for theta (rad); both sample lenses are monotonic here.
BRACKET_MAX_RAD = 2.0


def theta_d(theta, k):
    """Forward radial polynomial (explicit powers, not Horner, on purpose)."""
    t = np.asarray(theta, dtype=np.float64)
    return t * (1.0 + k[0] * t ** 2 + k[1] * t ** 4 + k[2] * t ** 6 + k[3] * t ** 8 + k[4] * t ** 10)


def d_theta_d(theta, k):
    """Derivative of the polynomial."""
    t = np.asarray(theta, dtype=np.float64)
    return 1.0 + 3 * k[0] * t ** 2 + 5 * k[1] * t ** 4 + 7 * k[2] * t ** 6 + 9 * k[3] * t ** 8 + 11 * k[4] * t ** 10


def solve_theta(rd, k):
    """Invert theta_d by bisection (independent of the Newton code in C++)."""
    lo, hi = 0.0, BRACKET_MAX_RAD
    if theta_d(hi, k) < rd:
        raise ValueError("target radius outside the bracket")
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if theta_d(mid, k) < rd:
            lo = mid
        else:
            hi = mid
        if hi - lo < 1e-16:
            break
    return 0.5 * (lo + hi)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, "tests", "golden", "lens_tables.json")

    # Sanity: the bracket must be monotonic for every lens or the bisection
    # would silently return garbage.
    grid = np.linspace(0.0, BRACKET_MAX_RAD, 20001)
    for name, lens in LENSES.items():
        if np.any(d_theta_d(grid, lens["k"]) <= 0.0):
            print("ERROR: lens %s is not monotonic on the bisection bracket" % name)
            return 1

    theta_deg = np.arange(0.0, THETA_MAX_DEG + 0.5 * THETA_STEP_DEG, THETA_STEP_DEG)
    theta_rad = np.deg2rad(theta_deg)

    doc = {
        "generator": "scripts/gen_lens_tables.py",
        "model": "theta_d = theta * (1 + k1 t^2 + k2 t^4 + k3 t^6 + k4 t^8 + k5 t^10)",
        "lenses": LENSES,
        "theta_table": {
            "theta_deg": theta_deg.tolist(),
        },
        "unproject": {},
    }

    rng = np.random.default_rng(RANDOM_SEED)
    for name, lens in LENSES.items():
        k = lens["k"]
        doc["theta_table"][name] = theta_d(theta_rad, k).tolist()

        targets = []
        for _ in range(RANDOM_TARGETS):
            # Uniform in radius and azimuth around the principal point.
            r = rng.uniform(0.0, TARGET_MAX_RADIUS_PX)
            a = rng.uniform(-np.pi, np.pi)
            px = lens["cx"] + r * np.cos(a)
            py = lens["cy"] + r * np.sin(a)
            nx = (px - lens["cx"]) / lens["fx"]
            ny = (py - lens["cy"]) / lens["fy"]
            rd = float(np.hypot(nx, ny))
            theta = solve_theta(rd, k)
            if rd > 0.0:
                direction = [np.sin(theta) * nx / rd, np.sin(theta) * ny / rd, np.cos(theta)]
            else:
                direction = [0.0, 0.0, 1.0]
            targets.append({
                "px": [float(px), float(py)],
                "theta": float(theta),
                "dir": [float(v) for v in direction],
            })
        doc["unproject"][name] = targets

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("wrote %s (%d table rows, %d targets per lens)" % (out_path, len(theta_deg), RANDOM_TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
