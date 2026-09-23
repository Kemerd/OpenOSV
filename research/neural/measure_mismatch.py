"""Measure the photometric disagreement between the two lenses in the overlap.

For every co-visible pixel (both single-lens renders valid) computes the
per-channel log2 ratio lensTop / lensBottom and reports it binned by latitude,
separately for the SKY half of the band (longitude -180..0 on the sample clip)
and the GROUND half (0..180), per frame.

If the ratio is the same function of latitude on sky and ground and on every
frame, the disagreement is lens shading (vignetting / colour shading), not a
scene or exposure effect - and a static per-rig radial model fixes it.

Usage: python measure_mismatch.py [frames...]
"""
import sys

import numpy as np

import bandio


def main():
    frames = [int(a) for a in sys.argv[1:]] or [0, 16, 32, 48, 64]
    th = bandio.theta()
    lat = bandio.lat_deg()
    m = bandio.meta()
    w = m["w"]
    # Which lens looks at +lat (the top of the band)?
    top = 0 if th[0, w // 4, 0] < th[0, w // 4, 1] else 1
    bot = 1 - top
    print(f"top-of-band lens = {top}; theta at row0: {th[0, w // 4]}, at last row: {th[-1, w // 4]}")

    # Column ranges: sky = lon -180..0 minus the wing near the ends,
    # ground = lon 0..180 minus the wing.
    regions = {
        "sky": (int(0.04 * w), int(0.46 * w)),
        "ground": (int(0.54 * w), int(0.83 * w)),
    }
    edges = np.arange(-8.0, 8.01, 1.0)
    print("lat bin centres:", " ".join(f"{e + 0.5:+.1f}" for e in edges[:-1]))
    summary = {}
    for f in frames:
        a = bandio.load(f, f"lens{top}")
        b = bandio.load(f, f"lens{bot}")
        valid = (a[..., 3] > 0.5) & (b[..., 3] > 0.5)
        valid &= (a[..., :3].min(-1) > 1e-4) & (b[..., :3].min(-1) > 1e-4)
        r = np.log2(a[..., :3] / np.maximum(b[..., :3], 1e-6))
        for name, (c0, c1) in regions.items():
            line = []
            for i in range(len(edges) - 1):
                rows = (lat >= edges[i]) & (lat < edges[i + 1])
                sel = valid[rows, c0:c1]
                if sel.sum() < 200:
                    line.append("   n/a          ")
                    continue
                v = r[rows, c0:c1][sel]  # (n, 3)
                med = np.median(v, axis=0)
                line.append(f"{med[0]:+.2f}/{med[1]:+.2f}/{med[2]:+.2f}")
                summary.setdefault((name, i), []).append(med)
            print(f"f{f:02d} {name:6s} log2(top/bot) R/G/B per lat bin:")
            print("   " + " | ".join(line))

    print("\nMean over frames (G channel, stops) by latitude bin:")
    for name in regions:
        vals = []
        for i in range(len(edges) - 1):
            if (name, i) in summary:
                vals.append(f"{edges[i] + 0.5:+.1f}:{np.mean([s[1] for s in summary[(name, i)]]):+.3f}")
        print(f"  {name:6s} " + "  ".join(vals))


if __name__ == "__main__":
    main()
