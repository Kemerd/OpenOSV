"""Compact before/after figure for docs/research/NEURAL_STITCHING.md.

Crops the open-sky part of the band (frame 32, +-12 deg around the seam)
from the production blend and from the recommended correction (rim-aware
weights + 2-D log-gain field, 20 deg decay) and stacks them, plus the same
two images detrended (log luminance minus a per-column quadratic, x12) so
the residual seam is visible to the eye.

Usage: set OSV_BANDS to the 30-deg probe dir; python make_figure.py <out.png>
"""
import sys

import cv2
import numpy as np

import bandio
from blend_experiments import blend, decay_rows, detrended_view
from photo_grid_experiments import gridfield
from rim_experiments import occlusion_part, rim_limits, smoothstep


def main():
    out = sys.argv[1]
    f = 32
    m = bandio.meta()
    w = m["w"]
    lat = bandio.lat_deg()
    rpd = 1.0 / abs(lat[1] - lat[0])
    th = bandio.theta().astype(np.float64)
    tmax, feather = m["lensFovDeg"] / 2.0, m["featherDeg"]
    ov = tmax - 90.0
    A0, A1 = bandio.load(f, "lens0").astype(np.float64), bandio.load(f, "lens1").astype(np.float64)
    L0, L1, w0, w1 = A0[..., :3], A1[..., :3], A0[..., 3], A1[..., 3]
    occ0 = occlusion_part(w0, th[..., 0], tmax, feather)
    occ1 = occlusion_part(w1, th[..., 1], tmax, feather)
    cov = (w0 > 1e-4) & (w1 > 1e-4) & (occ0 > 0.99) & (occ1 > 0.99)
    lim0 = rim_limits(L0, L1, th[..., 0], cov)
    lim1 = rim_limits(L1, L0, th[..., 1], cov)
    fw0 = np.where(th[..., 0] > lim0[None, :], 0.0, smoothstep((lim0[None, :] - th[..., 0]) / 3.0))
    fw1 = np.where(th[..., 1] > lim1[None, :], 0.0, smoothstep((lim1[None, :] - th[..., 1]) / 3.0))
    wr0, wr1 = fw0 * occ0, fw1 * occ1
    dead = (wr0 + wr1) <= 1e-4
    wr0, wr1 = np.where(dead, w0, wr0), np.where(dead, w1, wr1)
    trust = cov & (th[..., 0] < lim0[None, :] - 0.5) & (th[..., 1] < lim1[None, :] - 0.5)
    D = gridfield(L0, L1, trust, lat, ov, rpd)
    k0 = decay_rows(lat, -ov, 90, 20.0)[:, None, None]
    k1 = decay_rows(lat, -90, ov, 20.0)[:, None, None]
    fixed = blend(L0 * np.exp(0.5 * D * k0), L1 * np.exp(-0.5 * D * k1), wr0, wr1)
    prod = bandio.load(f, "blend")[..., :3]
    rows = np.abs(lat) <= 12.0
    cols = slice(int(0.15 * w), int(0.47 * w))
    tiles = []
    for img in (prod, fixed):
        tiles.append(bandio.tonemap(img[rows][:, cols], 1.6))
        tiles.append(np.ones((6, tiles[-1].shape[1], 3)))
    for img in (prod, fixed):
        d = detrended_view(img, lat)[rows][:, cols]
        tiles.append(np.repeat(d[..., None], 3, -1))
        tiles.append(np.ones((6, d.shape[1], 3)))
    pic = np.concatenate(tiles[:-1], 0).astype(np.float32)
    pic = cv2.resize(pic, (pic.shape[1] * 3 // 4, pic.shape[0] * 3 // 4), interpolation=cv2.INTER_AREA)
    cv2.imwrite(out, (pic[..., ::-1] * 255 + 0.5).astype(np.uint8))
    print(out, pic.shape)


if __name__ == "__main__":
    main()
