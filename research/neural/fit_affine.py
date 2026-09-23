"""Is the lens disagreement multiplicative (vignetting / exposure) or affine
(multiplicative + additive veiling glare)?

Per latitude bin, fits lensTop = g * lensBottom + o per channel by least
squares over co-visible pixels pooled from the sky AND the ground halves,
and compares the residual against a pure gain fit (o = 0).  A pure-gain
model that fits the ground but not the sky, where an affine one fits both,
says a flat additive term (glare / flare / black-level) differs between the
lenses - which a multiplicative gain can never remove.

Usage: python fit_affine.py [frame]
"""
import sys

import numpy as np

import bandio


def main():
    frame = int(sys.argv[1]) if len(sys.argv) > 1 else 32
    th = bandio.theta()
    lat = bandio.lat_deg()
    w = bandio.meta()["w"]
    top = 0 if th[0, w // 4, 0] < th[0, w // 4, 1] else 1
    a = bandio.load(frame, f"lens{top}")
    b = bandio.load(frame, f"lens{1 - top}")
    valid = (a[..., 3] > 0.5) & (b[..., 3] > 0.5)
    cols = np.zeros(w, bool)
    cols[int(0.04 * w):int(0.46 * w)] = True   # sky
    cols[int(0.54 * w):int(0.83 * w)] = True   # ground
    valid &= cols[None, :]
    sky = np.zeros(w, bool)
    sky[int(0.04 * w):int(0.46 * w)] = True

    print(f"frame {frame}: top lens {top}. Per 1-deg latitude bin, per channel:")
    print("  lat   ch   medSky(bot) medGnd(bot) | gainOnly: g  relRMS | affine: g    o(x1e3) relRMS")
    for lo in np.arange(-6.0, 6.0, 1.0):
        rows = (lat >= lo) & (lat < lo + 1.0)
        sel = valid[rows]
        if sel.sum() < 1000:
            continue
        A = a[rows][sel][:, :3].astype(np.float64)
        B = b[rows][sel][:, :3].astype(np.float64)
        isSky = np.broadcast_to(sky[None, :], valid.shape)[rows][sel]
        for c in range(3):
            x, y = B[:, c], A[:, c]
            # gain only (through origin)
            g0 = (x @ y) / (x @ x)
            r0 = y - g0 * x
            # affine
            M = np.stack([x, np.ones_like(x)], 1)
            (g1, o1), *_ = np.linalg.lstsq(M, y, rcond=None)
            r1 = y - (g1 * x + o1)
            ym = np.mean(np.abs(y))
            print(f"  {lo + 0.5:+.1f}  {'RGB'[c]}   {np.median(x[isSky]):.4f}     {np.median(x[~isSky]):.4f}"
                  f"     | {g0:6.3f} {np.sqrt(np.mean(r0 ** 2)) / ym:6.3f} | {g1:6.3f} {o1 * 1e3:+7.2f}"
                  f"  {np.sqrt(np.mean(r1 ** 2)) / ym:6.3f}")


if __name__ == "__main__":
    main()
