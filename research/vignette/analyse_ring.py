# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""What the sky band at the seam is: each lens's own radial structure,
measured from its own sky (WP-VIGNETTE; NEURAL_STITCHING.md, section 9).

Input: the per-lens dumps of vigprobe (research/vignette/vigprobe.cpp):

    research\\vignette\\build_vigprobe.cmd          (in a VS developer shell)
    research\\vignette\\bin\\vigprobe.exe lens clip.OSV <dir> 2048 0 0 32 64

Each lens alone over the whole sphere, polar-axis layout (lens axes at the
poles, the seam on the equator), native scene-linear light, alpha = the
occlusion factor, plus every pixel's angle from both lens axes.  Then

    python research/vignette/analyse_ring.py <dir>

prints, per lens and per 15 deg longitude group of the open sky (polar
longitudes -165..-15), for frames 0 / 32 / 64:

  * where the lens's luma dips below a smooth sky along its own theta, and
    how deep (log2 luma minus a quadratic fitted to theta in [70, 94] with
    the zone [82.5, 90] left out);
  * the same dip per channel in stops AND in scene-linear light at 86 +- 1
    deg: a multiplicative vignette dims R, G and B by the same number of
    stops; an additive deficit (missing veiling glare) removes the same
    LIGHT from each, which in a blue sky is three to five times deeper in
    red stops than in blue.

ASCII console output; needs numpy.
"""
import json
import os
import sys

import numpy as np

LUMA = np.array([0.2627, 0.6780, 0.0593])
FRAMES = (0, 32, 64)
GROUPS = range(-165, -15, 15)


def load(d, f, lens, w, h):
    rgba = np.fromfile(os.path.join(d, "f%d_lens%d.f32" % (f, lens)), np.float32).reshape(h, w, 4)
    theta = np.fromfile(os.path.join(d, "f%d_theta.f32" % f), np.float32).reshape(h, w, 2)[..., lens]
    return rgba.astype(np.float64), theta.astype(np.float64)


def column_dip(t, y, fit_mask, eval_mask):
    """Quadratic of y against theta on fit_mask; y minus it on eval_mask."""
    if fit_mask.sum() < 30 or eval_mask.sum() < 3:
        return None
    p = np.polyfit(t[fit_mask] - 85.0, y[fit_mask], 2)
    return y[eval_mask] - np.polyval(p, t[eval_mask] - 85.0), np.polyval(p, t[eval_mask] - 85.0)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    meta = json.load(open(os.path.join(d, "meta.json")))
    w, h = meta["w"], meta["h"]
    lon = (np.arange(w) + 0.5) / w * 360.0 - 180.0
    bins = np.arange(78.0, 94.01, 0.25)
    centres = 0.5 * (bins[1:] + bins[:-1])
    for lens in (0, 1):
        print("lens %d (%s)" % (lens, "slave" if lens == 0 else "master"))
        print("  lon group  frame   dip at deg  dip luma   log2 R / G / B at 86      linear R / G / B at 86")
        for g in GROUPS:
            for f in FRAMES:
                rgba, theta = load(d, f, lens, w, h)
                cols = np.nonzero((lon >= g) & (lon < g + 15))[0]
                prof_rows = []
                chan_log = [[], [], []]
                chan_lin = [[], [], []]
                for c in cols:
                    t = theta[:, c]
                    ok = (rgba[:, c, 3] > 0.99) & np.all(rgba[:, c, :3] > 1e-5, axis=1) & (t > 70) & (t < 94)
                    fit = ok & ~((t > 82.5) & (t < 90.0))
                    ring = ok & (np.abs(t - 86.0) < 1.0)
                    y = np.log2(np.maximum(rgba[:, c, :3] @ LUMA, 1e-6))
                    res = column_dip(t, y, fit, ok)
                    if res is None:
                        continue
                    r = res[0]
                    idx = np.digitize(t[ok], bins) - 1
                    prof_rows.append([np.median(r[idx == k]) if np.any(idx == k) else np.nan
                                      for k in range(len(centres))])
                    for ch in range(3):
                        yc = np.log2(np.maximum(rgba[:, c, ch], 1e-6))
                        rc = column_dip(t, yc, fit, ring)
                        if rc is not None:
                            chan_log[ch].append(np.median(rc[0]))
                            chan_lin[ch].append(np.median(rgba[ring, c, ch] - 2.0 ** rc[1]))
                if len(prof_rows) < 5:
                    continue
                prof = np.nanmedian(np.array(prof_rows), axis=0)
                k = int(np.nanargmin(np.where((centres > 82) & (centres < 91), prof, np.nan)))
                print("  %5d..%-4d  %3d    %6.2f     %+6.3f    %+.2f / %+.2f / %+.2f      %+.4f / %+.4f / %+.4f"
                      % (g, g + 15, f, centres[k], prof[k], np.median(chan_log[0]), np.median(chan_log[1]),
                         np.median(chan_log[2]), np.median(chan_lin[0]), np.median(chan_lin[1]),
                         np.median(chan_lin[2])))


if __name__ == "__main__":
    main()
