"""Profile each lens near its image-circle rim against the other lens.

For sky columns where both lenses are unoccluded, bins co-visible pixels by
the angle from the lens's own axis (theta, 0.1 deg bins from 88 deg to the
usable limit) and prints median log2(lens / other lens) in G, plus each
lens's own blend weight.  A healthy lens shows a smooth vignetting slope; a
contaminated rim shows a sudden drop (barrel shadow) or rise (rim flare) in
the last fraction of a degree - the thin lines seen in the stitched sky.

Usage: python rim_profile.py [frame]
"""
import sys

import numpy as np

import bandio


def main():
    frame = int(sys.argv[1]) if len(sys.argv) > 1 else 32
    th = bandio.theta()
    w = bandio.meta()["w"]
    L = [bandio.load(frame, "lens0"), bandio.load(frame, "lens1")]
    cols = np.zeros(w, bool)
    cols[int(0.20 * w):int(0.44 * w)] = True  # open sky, both lenses unoccluded
    for me in (0, 1):
        other = 1 - me
        a, b = L[me], L[other]
        ok = (a[..., 3] > 0) & (b[..., 3] > 0) & cols[None, :]
        ok &= (a[..., 1] > 1e-4) & (b[..., 1] > 1e-4)
        t = th[..., me]
        r = np.log2(a[..., 1] / np.maximum(b[..., 1], 1e-6))
        print(f"\nlens {me} vs lens {other}, frame {frame}: theta(lens {me})  log2 ratio G   weight(lens {me})   n")
        for lo in np.arange(88.0, 97.7, 0.25):
            s = ok & (t >= lo) & (t < lo + 0.25)
            if s.sum() < 50:
                continue
            print(f"   {lo + 0.125:6.2f}   {np.median(r[s]):+.3f}   {np.median(a[..., 3][s]):.3f}   {s.sum()}")


if __name__ == "__main__":
    main()
