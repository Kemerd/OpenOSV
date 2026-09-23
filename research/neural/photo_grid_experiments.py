"""Decay length, 1-D vs 2-D photometric field, gain vs offset, split vs anchor.

Runs on a +-30 deg band (bandprobe ... 4096 30 ...) so the correction can
decay over a realistic distance.  All variants use the rim-aware weights from
rim_experiments.py (the part that removes the thin rim lines); they differ
only in the photometric correction:

  prod                 shipping blend (reference)
  rim                  rim-aware weights, no photometric correction
  col-d8               per-column log gain, symmetric, decay 8.4 deg (last run)
  col-d20              same, decay 20 deg
  grid-d20             2-D log-gain field over (lon, lat) from TRUSTED pixels
                       (normalised convolution, 48 cols x 1.5 deg), decay 20
  offset-d20           per-column ADDITIVE offset in linear light (veil model)
  col-anchor0-d20      per-column log gain applied to lens 1 only (lens 0 kept)

Metrics (millistops, sky columns, 32-column block averages of log2 luma):
  line    RMS(profile - 1.5 deg blur), |lat| <= 10
  band    RMS(0.5 deg blur - 4 deg blur), |lat| <= 10
  broad   RMS(2 deg blur - 10 deg blur), |lat| <= 25 (soft bands, decay ramps)
  dE      RMS chroma difference of the two corrected lenses on trusted pixels

Usage: set OSV_BANDS to the 30-deg probe dir; python photo_grid_experiments.py [frames]
"""
import os
import sys

import cv2
import numpy as np

import bandio
from blend_experiments import EPS, blend, decay_rows, gblur, nconv, detrended_view
from rim_experiments import occlusion_part, rim_limits, smoothstep


def colfield(L0, L1, trust, sigma_cols=48):
    covf = trust.astype(np.float64)
    n = covf.sum(0)
    lr = np.log(np.maximum(L1, EPS)) - np.log(np.maximum(L0, EPS))
    colmean = (lr * covf[..., None]).sum(0) / np.maximum(n, 1)[:, None]
    num = np.stack([gblur((colmean[:, c] * n)[None, :], sigma_cols, 0.001)[0] for c in range(3)], -1)
    den = gblur(n[None, :].astype(np.float64), sigma_cols, 0.001)[0]
    return num / np.maximum(den, 1e-6)[:, None]  # (w, 3)


def coloffset(L0, L1, trust, sigma_cols=48):
    covf = trust.astype(np.float64)
    n = covf.sum(0)
    df = L1 - L0
    colmean = (df * covf[..., None]).sum(0) / np.maximum(n, 1)[:, None]
    num = np.stack([gblur((colmean[:, c] * n)[None, :], sigma_cols, 0.001)[0] for c in range(3)], -1)
    den = gblur(n[None, :].astype(np.float64), sigma_cols, 0.001)[0]
    return num / np.maximum(den, 1e-6)[:, None]


def gridfield(L0, L1, trust, lat, ov, rows_per_deg):
    lr = np.log(np.maximum(L1, EPS)) - np.log(np.maximum(L0, EPS))
    D, den = nconv(lr, trust.astype(np.float64), 48, 1.5 * rows_per_deg)
    # beyond the trusted rows, hold the nearest trusted row's value along
    # latitude (a clamp), then the decay ramp takes it to zero
    have = den > 0.05
    out = D.copy()
    h = D.shape[0]
    for x in range(D.shape[1]):
        idx = np.where(have[:, x])[0]
        if idx.size == 0:
            out[:, x] = 0
            continue
        lo, hi = idx.min(), idx.max()
        out[:lo, x] = D[lo, x]
        out[hi + 1:, x] = D[hi, x]
    # columns with no trusted rows at all: fill along longitude
    colok = have.any(0).astype(np.float64)
    fill = np.stack([gblur(out[..., c] * colok[None, :], 96, 0.001) for c in range(3)], -1)
    fden = gblur(np.repeat(colok[None, :], h, 0), 96, 0.001)[..., None]
    fill = fill / np.maximum(fden, 1e-6)
    return np.where(colok[None, :, None] > 0, out, fill)


def metrics(img, lat, cols, L0c, L1c, trust):
    Y = np.log2(np.maximum(bandio.luma(img[..., :3]), EPS))[:, cols]
    nb = Y.shape[1] // 32
    blk = Y[:, :nb * 32].reshape(Y.shape[0], nb, 32).mean(2)
    rpd = 1.0 / abs(lat[1] - lat[0])

    def bl(x, s):
        return cv2.GaussianBlur(x.astype(np.float32), (0, 0), sigmaX=1e-3, sigmaY=s * rpd,
                                borderType=cv2.BORDER_REPLICATE)

    r10 = np.abs(lat) <= 10
    r25 = np.abs(lat) <= 25
    line = np.sqrt(np.mean((blk - bl(blk, 1.5))[r10] ** 2)) * 1000
    band = np.sqrt(np.mean((bl(blk, 0.5) - bl(blk, 4.0))[r10] ** 2)) * 1000
    broad = np.sqrt(np.mean((bl(blk, 2.0) - bl(blk, 10.0))[r25] ** 2)) * 1000
    t = trust & cols[None, :]
    c0 = np.log2(np.maximum(L0c[..., [0, 2]], EPS) / np.maximum(L0c[..., 1:2], EPS))
    c1 = np.log2(np.maximum(L1c[..., [0, 2]], EPS) / np.maximum(L1c[..., 1:2], EPS))
    de = np.sqrt(np.mean((c0 - c1)[t] ** 2)) * 1000
    return line, band, broad, de


def main():
    frames = [int(a) for a in sys.argv[1:]] or [0, 32, 64]
    m = bandio.meta()
    w = m["w"]
    lat = bandio.lat_deg()
    rpd = 1.0 / abs(lat[1] - lat[0])
    th = bandio.theta().astype(np.float64)
    tmax = m["lensFovDeg"] / 2.0
    feather = m["featherDeg"]
    ov = tmax - 90.0
    cols = np.zeros(w, bool)
    cols[int(0.20 * w):int(0.44 * w)] = True
    names = ["prod", "rim", "col-d8", "col-d20", "grid-d20", "offset-d20", "col-anchor0-d20"]
    acc = {n: [] for n in names}
    outdir = os.path.join(bandio.BANDS, "exp")
    os.makedirs(outdir, exist_ok=True)
    for f in frames:
        A0, A1 = bandio.load(f, "lens0").astype(np.float64), bandio.load(f, "lens1").astype(np.float64)
        L0, L1 = A0[..., :3], A1[..., :3]
        w0, w1 = A0[..., 3], A1[..., 3]
        occ0 = occlusion_part(w0, th[..., 0], tmax, feather)
        occ1 = occlusion_part(w1, th[..., 1], tmax, feather)
        cov = (w0 > 1e-4) & (w1 > 1e-4) & (occ0 > 0.99) & (occ1 > 0.99)
        lim0 = rim_limits(L0, L1, th[..., 0], cov)
        lim1 = rim_limits(L1, L0, th[..., 1], cov)
        fw0 = np.where(th[..., 0] > lim0[None, :], 0.0, smoothstep((lim0[None, :] - th[..., 0]) / 3.0))
        fw1 = np.where(th[..., 1] > lim1[None, :], 0.0, smoothstep((lim1[None, :] - th[..., 1]) / 3.0))
        wr0, wr1 = fw0 * occ0, fw1 * occ1
        dead = (wr0 + wr1) <= 1e-4
        wr0 = np.where(dead, w0, wr0)
        wr1 = np.where(dead, w1, wr1)
        trust = cov & (th[..., 0] < lim0[None, :] - 0.5) & (th[..., 1] < lim1[None, :] - 0.5)

        def k(lens, ramp):
            # lens 1 looks at +lat: its correction is full up to +ov and
            # decays above it; lens 0 mirrors that
            return (decay_rows(lat, -90, ov, ramp) if lens == 1 else decay_rows(lat, -ov, 90, ramp))[:, None, None]

        res, pr = {}, {}
        res["prod"] = bandio.load(f, "blend")[..., :3]
        pr["prod"] = (L0, L1)
        res["rim"] = blend(L0, L1, wr0, wr1)
        pr["rim"] = (L0, L1)
        d = colfield(L0, L1, trust)[None]
        for name, ramp in (("col-d8", 8.4), ("col-d20", 20.0)):
            a, b = L0 * np.exp(0.5 * d * k(0, ramp)), L1 * np.exp(-0.5 * d * k(1, ramp))
            res[name] = blend(a, b, wr0, wr1)
            pr[name] = (a, b)
        D = gridfield(L0, L1, trust, lat, ov, rpd)
        a, b = L0 * np.exp(0.5 * D * k(0, 20)), L1 * np.exp(-0.5 * D * k(1, 20))
        res["grid-d20"] = blend(a, b, wr0, wr1)
        pr["grid-d20"] = (a, b)
        o = coloffset(L0, L1, trust)[None]
        a, b = np.maximum(L0 + 0.5 * o * k(0, 20), 0), np.maximum(L1 - 0.5 * o * k(1, 20), 0)
        res["offset-d20"] = blend(a, b, wr0, wr1)
        pr["offset-d20"] = (a, b)
        a, b = L0, L1 * np.exp(-d * k(1, 20))
        res["col-anchor0-d20"] = blend(a, b, wr0, wr1)
        pr["col-anchor0-d20"] = (a, b)
        for n in names:
            acc[n].append(metrics(res[n], lat, cols, pr[n][0], pr[n][1], trust))
        rows_det, rows_img = [], []
        for n in ("prod", "rim", "col-d20", "grid-d20", "offset-d20"):
            dd = detrended_view(res[n], lat)[:, : w // 2]
            rows_det += [np.repeat(dd[..., None], 3, -1), np.ones((6, dd.shape[1], 3))]
            im = bandio.tonemap(res[n][:, : w // 2], 1.6)
            rows_img += [im, np.ones((6, im.shape[1], 3))]
        for tag, rr in (("grid_detrend", rows_det), ("grid_img", rows_img)):
            pic = np.concatenate(rr, 0).astype(np.float32)
            pic = cv2.resize(pic, (pic.shape[1] // 2, pic.shape[0] // 2), interpolation=cv2.INTER_AREA)
            cv2.imwrite(os.path.join(outdir, f"f{f}_{tag}.png"), (pic[..., ::-1] * 255 + 0.5).astype(np.uint8))
    print(f"frames {frames}; +-{m['h'] / 2 / rpd:.0f} deg band; sky columns {cols.sum()}")
    print(f"{'variant':18s} {'line':>7s} {'band':>7s} {'broad':>7s} {'dE':>7s}   (millistops, lower is better)")
    for n in names:
        v = np.mean(np.array(acc[n]), 0)
        print(f"{n:18s} {v[0]:7.1f} {v[1]:7.1f} {v[2]:7.1f} {v[3]:7.1f}")


if __name__ == "__main__":
    main()
