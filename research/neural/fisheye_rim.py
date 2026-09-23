"""Where is each lens's real image-circle edge, azimuth by azimuth?

Reads the raw fisheye luma (osvtool extract --frame N --lens L --out x.pgm)
and the kernel's lens intrinsics (bandprobe's lenses.json), then for a ring
of azimuths reports the angle theta at which the luma falls to half of its
value at theta = 88 deg.  If the usable rim measured in the stitched band
(rim_profile.py: lens 0 ~92.8 deg in the sky columns) matches the image
circle's own edge here, the rim limit is a static property of the lens and
mode - measurable once per clip from the fisheye alone.

Usage: python fisheye_rim.py <dir with lenses.json, fish0.pgm, fish1.pgm>
"""
import json
import os
import sys

import cv2
import numpy as np


def theta_d(k, t):
    t2 = t * t
    return t * (1 + k[0] * t2 + k[1] * t2 ** 2 + k[2] * t2 ** 3 + k[3] * t2 ** 4 + k[4] * t2 ** 5)


def main():
    d = sys.argv[1]
    lenses = json.load(open(os.path.join(d, "lenses.json")))
    for L in (0, 1):
        q = lenses[L]
        img = cv2.imread(os.path.join(d, f"fish{L}.pgm"), cv2.IMREAD_UNCHANGED).astype(np.float64)
        img = cv2.GaussianBlur(img, (0, 0), 2.0)
        R = np.array(q["R"]).reshape(3, 3)
        thetas = np.radians(np.arange(85.0, 99.01, 0.1))
        print(f"\nlens {L}: fx {q['fx']:.1f} c ({q['cx']:.1f},{q['cy']:.1f}); r(90)={q['fx'] * theta_d(q['k'], np.pi / 2):.1f}"
              f" r(97.59)={q['fx'] * theta_d(q['k'], q['thetaMax']):.1f} px")
        print("  band lon (deg, polar map) -> lens azimuth; theta where luma < 50% / 25% of its theta=88 value;"
              " frame-edge theta")
        for lon_deg in range(-180, 180, 20):
            # the direction on the equator of the polar-axis map at this longitude
            lon = np.radians(lon_deg + 0.5)
            dbody = np.array([np.sin(lon), 0.0, np.cos(lon)])
            dl = R @ dbody
            phi = np.arctan2(dl[1], dl[0])
            vals, edge = [], None
            for t in thetas:
                r = q["fx"] * theta_d(q["k"], t)
                u = q["cx"] + r * np.cos(phi)
                v = q["cy"] + r * np.sin(phi)
                if not (0 <= u < q["w"] - 1 and 0 <= v < q["h"] - 1):
                    if edge is None:
                        edge = np.degrees(t)
                    vals.append(np.nan)
                    continue
                vals.append(img[int(v), int(u)])
            vals = np.array(vals)
            ref = vals[np.argmin(np.abs(np.degrees(thetas) - 88.0))]
            half = quarter = None
            for t, v in zip(np.degrees(thetas), vals):
                if t < 88 or not np.isfinite(v):
                    continue
                if half is None and v < 0.5 * ref:
                    half = t
                if quarter is None and v < 0.25 * ref:
                    quarter = t
            fmt = lambda x: f"{x:6.2f}" if x is not None else "  none"
            print(f"  lon {lon_deg:+4d} -> az {np.degrees(phi):+7.1f}: 50% at {fmt(half)}  25% at {fmt(quarter)}"
                  f"  frame edge {fmt(edge)}  (luma@88 = {ref:.0f})")


if __name__ == "__main__":
    main()
