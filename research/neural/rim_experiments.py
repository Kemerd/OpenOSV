"""Rim-aware weights + photometric matching (research only).

Finding that motivated this (rim_profile.py): in the open-sky columns lens 0
is fine to theta ~93.5 deg and then falls off a cliff (-1 stop at 95 deg,
-4 stops past 97 deg) while the production feather still gives it 0.67 weight
at 95 deg - a dark line along the overlap edge.  So the usable rim is not the
calibrated 97.59 deg, and it differs per lens and per longitude.

Variants (all reuse bandprobe's exact production weights for the occlusion
part; only the FOV part of the weight is replaced where stated):
  prod          shipping blend
  gain          + global symmetric RGB gain (= --gain)
  inset         static inset: FOV weight = smoothstep((95 - theta) / 3)
  rimaware      per-lens, per-column usable limit theta_lim(c) measured from
                the data (where the lens falls 0.25 stop below its own core
                ratio vs the other lens), FOV weight = smoothstep((theta_lim - theta) / 3)
  rimaware+colgain   + per-column symmetric gain from the TRUSTED co-visible
                pixels only, decayed away from the overlap (DJI-style
                g_seam_up / g_seam_dn)
  rimaware+colgain+wide   same, occlusion ramp widened 3x (in band pixels)

Metrics, sky columns, log2 luminance block-averaged over 32 columns:
  line   RMS of profile minus its 1.5 deg Gaussian (thin lines), millistops
  band   RMS of 0.5 deg minus 4 deg Gaussians (band-scale bumps), millistops
  edge   P99 |log L - blur_lon| in the overlap rows (vertical steps), millistops
  dE     RMS log2 chroma difference R/G and B/G between the two corrected
         lenses over trusted co-visible pixels, millistops (colour match)

Usage: python rim_experiments.py [frames...]
"""
import os
import sys

import cv2
import numpy as np

import bandio
from blend_experiments import EPS, blend, decay_rows, gblur, detrended_view


def smoothstep(x):
    t = np.clip(x, 0.0, 1.0)
    return t * t * (3 - 2 * t)


def prod_fov_weight(theta, theta_max, feather):
    return np.where(theta > theta_max, 0.0, smoothstep((theta_max - theta) / feather))


def occlusion_part(w, theta, theta_max, feather):
    """Recover the occlusion factor from a production weight."""
    f = prod_fov_weight(theta, theta_max, feather)
    occ = np.where(f > 0.02, w / np.maximum(f, 1e-6), np.nan)
    # where the fov part vanished, take the nearest defined value along latitude
    # by a normalised blur (the occlusion ramp is spatially smooth)
    good = np.isfinite(occ)
    num = gblur(np.where(good, occ, 0.0), 4, 4)
    den = gblur(good.astype(np.float64), 4, 4)
    fill = num / np.maximum(den, 1e-6)
    occ = np.where(good, occ, fill)
    return np.clip(occ, 0.0, 1.0)


def rim_limits(Lme, Lother, theta_me, valid, core=(88.5, 91.0), drop=0.25, smooth_cols=48):
    """Per-column usable-rim angle for one lens (degrees)."""
    h, w = theta_me.shape
    r = np.log2(np.maximum(Lme[..., 1], EPS) / np.maximum(Lother[..., 1], EPS))
    r = np.where(valid, r, np.nan)
    # block-average in 16-column blocks for robustness
    lim = np.full(w, np.nan)
    edges = np.arange(core[0], 97.6, 0.25)
    for c0 in range(0, w, 16):
        sl = slice(c0, c0 + 16)
        t = theta_me[:, sl]
        rr = r[:, sl]
        coremask = (t >= core[0]) & (t < core[1]) & np.isfinite(rr)
        if coremask.sum() < 20:
            continue
        ref = np.median(rr[coremask])
        found = 97.59
        for lo in edges:
            if lo < core[1]:
                continue
            s = (t >= lo) & (t < lo + 0.25) & np.isfinite(rr)
            if s.sum() < 8:
                continue
            if abs(np.median(rr[s]) - ref) > drop:
                found = lo
                break
        lim[sl] = found
    good = np.isfinite(lim)
    if not good.any():
        return np.full(w, 97.59)
    # fill and smooth along longitude (periodic); take a conservative minimum
    lim = np.where(good, lim, np.nanmedian(lim))
    k = smooth_cols
    padded = np.concatenate([lim[-k:], lim, lim[:k]])
    mins = np.array([padded[i:i + 2 * k + 1].min() for i in range(w)])
    return gblur(mins[None, :], k / 2, 0.001)[0]


def colgain_trusted(L0, L1, trust, lat, ov, ramp, sigma_cols=48):
    covf = trust.astype(np.float64)
    n = covf.sum(0)
    lr = np.log(np.maximum(L1, EPS)) - np.log(np.maximum(L0, EPS))
    colmean = (lr * covf[..., None]).sum(0) / np.maximum(n, 1)[:, None]
    num = np.stack([gblur((colmean[:, c] * n)[None, :], sigma_cols, 0.001)[0] for c in range(3)], -1)
    den = gblur(n[None, :].astype(np.float64), sigma_cols, 0.001)[0]
    d = num / np.maximum(den, 1e-6)[:, None]
    k1 = decay_rows(lat, -90, ov, ramp)[:, None, None]
    k0 = decay_rows(lat, -ov, 90, ramp)[:, None, None]
    return L0 * np.exp(0.5 * d[None] * k0), L1 * np.exp(-0.5 * d[None] * k1), d


def metrics(img, lat, cols, ov, L0c=None, L1c=None, trust=None):
    Y = np.log2(np.maximum(bandio.luma(img[..., :3]), EPS))
    ly = Y[:, cols]
    nb = ly.shape[1] // 32
    blk = ly[:, :nb * 32].reshape(ly.shape[0], nb, 32).mean(2)
    rows_per_deg = 1.0 / abs(lat[1] - lat[0])
    rows = np.abs(lat) <= 10.0

    def blat(x, sdeg):
        return cv2.GaussianBlur(x.astype(np.float32), (0, 0), sigmaX=1e-3, sigmaY=sdeg * rows_per_deg,
                                borderType=cv2.BORDER_REPLICATE)

    line = np.sqrt(np.mean((blk - blat(blk, 1.5))[rows] ** 2)) * 1000
    band = np.sqrt(np.mean((blat(blk, 0.5) - blat(blk, 4.0))[rows] ** 2)) * 1000
    hp = Y - gblur(Y, 40, 0.001)
    edge = np.percentile(np.abs(hp[np.abs(lat) <= ov][:, cols]), 99) * 1000
    de = np.nan
    if L0c is not None:
        t = trust & cols[None, :]
        c0 = np.log2(np.maximum(L0c[..., [0, 2]], EPS) / np.maximum(L0c[..., 1:2], EPS))
        c1 = np.log2(np.maximum(L1c[..., [0, 2]], EPS) / np.maximum(L1c[..., 1:2], EPS))
        de = np.sqrt(np.mean((c0 - c1)[t] ** 2)) * 1000
    return line, band, edge, de


def main():
    frames = [int(a) for a in sys.argv[1:]] or [0, 32, 64]
    m = bandio.meta()
    w = m["w"]
    lat = bandio.lat_deg()
    th = bandio.theta().astype(np.float64)
    tmax = m["lensFovDeg"] / 2.0
    feather = m["featherDeg"]
    ov = tmax - 90.0
    ramp = 16.0 - ov
    cols = np.zeros(w, bool)
    cols[int(0.20 * w):int(0.44 * w)] = True
    names = ["prod", "gain", "inset", "rimaware", "rimaware+colgain", "rimaware+colgain+wide"]
    acc = {n: [] for n in names}
    outdir = os.path.join(bandio.BANDS, "exp")
    os.makedirs(outdir, exist_ok=True)
    for f in frames:
        A0, A1 = bandio.load(f, "lens0").astype(np.float64), bandio.load(f, "lens1").astype(np.float64)
        L0, L1 = A0[..., :3], A1[..., :3]
        w0, w1 = A0[..., 3], A1[..., 3]
        v0, v1 = w0 > 1e-4, w1 > 1e-4
        occ0 = occlusion_part(w0, th[..., 0], tmax, feather)
        occ1 = occlusion_part(w1, th[..., 1], tmax, feather)
        cov = v0 & v1 & (occ0 > 0.99) & (occ1 > 0.99)
        res, pair = {}, {}
        res["prod"] = bandio.load(f, "blend")[..., :3]
        pair["prod"] = (L0, L1, cov)
        m0 = np.array([L0[..., c][cov].mean() for c in range(3)])
        m1 = np.array([L1[..., c][cov].mean() for c in range(3)])
        g0 = np.sqrt(m1 / m0)
        res["gain"] = blend(L0 * g0, L1 / g0, w0, w1)
        pair["gain"] = (L0 * g0, L1 / g0, cov)
        # static inset
        wi0 = prod_fov_weight(th[..., 0], 95.0, 3.0) * occ0
        wi1 = prod_fov_weight(th[..., 1], 95.0, 3.0) * occ1
        res["inset"] = blend(L0, L1, wi0, wi1)
        pair["inset"] = (L0, L1, cov)
        # data-driven rim limits
        lim0 = rim_limits(L0, L1, th[..., 0], cov)
        lim1 = rim_limits(L1, L0, th[..., 1], cov)
        if f == frames[0]:
            skyc = cols
            print(f"frame {f}: usable rim (deg), sky cols: lens0 median {np.median(lim0[skyc]):.2f} "
                  f"min {lim0[skyc].min():.2f}; lens1 median {np.median(lim1[skyc]):.2f} min {lim1[skyc].min():.2f}")
            gnd = np.zeros(w, bool)
            gnd[int(0.54 * w):int(0.83 * w)] = True
            print(f"          ground cols: lens0 median {np.median(lim0[gnd]):.2f}; lens1 median {np.median(lim1[gnd]):.2f}")
        fw0 = np.where(th[..., 0] > lim0[None, :], 0.0, smoothstep((lim0[None, :] - th[..., 0]) / 3.0))
        fw1 = np.where(th[..., 1] > lim1[None, :], 0.0, smoothstep((lim1[None, :] - th[..., 1]) / 3.0))
        wr0, wr1 = fw0 * occ0, fw1 * occ1
        # keep a pixel covered where the rim-aware weights both vanish
        dead = (wr0 + wr1) <= 1e-4
        wr0 = np.where(dead, w0, wr0)
        wr1 = np.where(dead, w1, wr1)
        res["rimaware"] = blend(L0, L1, wr0, wr1)
        pair["rimaware"] = (L0, L1, cov)
        trust = cov & (th[..., 0] < lim0[None, :] - 0.5) & (th[..., 1] < lim1[None, :] - 0.5)
        c0, c1, d = colgain_trusted(L0, L1, trust, lat, ov, ramp)
        res["rimaware+colgain"] = blend(c0, c1, wr0, wr1)
        pair["rimaware+colgain"] = (c0, c1, trust)
        # widen the occlusion ramp 3x: blur the occlusion factor in band space
        o0w = np.clip(gblur(occ0, 18, 18) * 1.0, 0, 1) * (occ0 > 0) + 0
        o1w = np.clip(gblur(occ1, 18, 18) * 1.0, 0, 1) * (occ1 > 0) + 0
        ww0, ww1 = fw0 * np.minimum(occ0, o0w), fw1 * np.minimum(occ1, o1w)
        dead = (ww0 + ww1) <= 1e-4
        ww0 = np.where(dead, w0, ww0)
        ww1 = np.where(dead, w1, ww1)
        res["rimaware+colgain+wide"] = blend(c0, c1, ww0, ww1)
        pair["rimaware+colgain+wide"] = (c0, c1, trust)
        for n in names:
            a, b, t = pair[n]
            acc[n].append(metrics(res[n], lat, cols, ov, a, b, trust))
        if f == frames[0]:
            print("per-column log gain lens1/lens0 (G, stops), sky median "
                  f"{np.median(d[cols, 1]) / np.log(2):+.3f}, ground median "
                  f"{np.median(d[int(0.54 * w):int(0.83 * w), 1]) / np.log(2):+.3f}")
        sky = slice(0, w // 2)
        rows_img, rows_det = [], []
        for n in names:
            im = bandio.tonemap(res[n][:, sky], 1.6)
            rows_img += [im, np.ones((4, im.shape[1], 3))]
            dd = detrended_view(res[n], lat)[:, sky]
            rows_det += [np.repeat(dd[..., None], 3, -1), np.ones((4, dd.shape[1], 3))]
        for tag, rows_ in (("rim_img", rows_img), ("rim_detrend", rows_det)):
            pic = np.concatenate(rows_, 0).astype(np.float32)
            pic = cv2.resize(pic, (pic.shape[1] // 2, pic.shape[0] // 2), interpolation=cv2.INTER_AREA)
            cv2.imwrite(os.path.join(outdir, f"f{f}_{tag}.png"), (pic[..., ::-1] * 255 + 0.5).astype(np.uint8))

    print(f"\nframes {frames}; sky columns {cols.sum()}")
    print(f"{'variant':24s} {'line':>7s} {'band':>7s} {'edge':>7s} {'dE':>7s}   (millistops, lower is better)")
    for n in names:
        v = np.nanmean(np.array(acc[n], dtype=np.float64), 0)
        print(f"{n:24s} {v[0]:7.1f} {v[1]:7.1f} {v[2]:7.1f} {v[3]:7.1f}")


if __name__ == "__main__":
    main()
