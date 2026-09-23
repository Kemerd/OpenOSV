"""Photometric seam experiments on the sample clip (research only).

Re-blends the two single-lens bands written by bandprobe with EXACTLY the
production weights (bandprobe stores each lens's feather x occlusion weight
in alpha) after applying one of several photometric corrections, and scores
how visible the seam is in the open-sky half of the band.

Variants
  prod       the shipping blend: feather in linear light, no gain
  reblend    prod recomputed from the lens bands (validates the harness)
  gain       one symmetric RGB gain per lens from the overlap means (= --gain)
  colgain    per-column (longitude) symmetric RGB gain, smoothed along
             longitude, decayed to 1 away from the overlap
  lowfreq    2-D low-frequency log-difference field D = blur(log L1 - log L0)
             over the co-visible band (normalised convolution), each lens
             corrected by -/+ D/2, decayed away from the overlap
  affine     local affine model L1 ~ a*L0 + b (box-filtered covariances,
             regularised towards a = 1, i.e. pure offset on flat sky) - a
             guided-filter style colour transfer, split half/half
  poisson    gradient-domain blend in log space: weighted gradients of both
             lenses, Dirichlet rows from the single-lens band edges,
             periodic in longitude, solved exactly by FFT + tridiagonal
  lowfreq+2band   lowfreq followed by a two-band blend (low band wide
             feather, high band the production feather)

Metrics over sky columns (log2 luminance, per column):
  bumpRMS  RMS residual of a quadratic fit in latitude across the band, in
           millistops.  The sky is smooth at this scale; a seam band is a bump.
  curvP99  99th percentile |d2/dlat2| of the column profile (block-averaged
           over 32 columns, smoothed), millistops/deg^2 - Mach-band proxy.
  lonP99   99th percentile |log L - blur_lon(log L)| in the overlap rows,
           millistops - vertical edges such as the occlusion-ramp step.
  chroma   RMS quadratic-fit residual of log2(R/G) and log2(B/G), millistops.

Usage: python blend_experiments.py [frames...]   (writes previews too)
"""
import os
import sys
import time

import cv2
import numpy as np

import bandio

LN2 = np.log(2.0)
EPS = 1e-5


# --------------------------------------------------------------------------
#  helpers
# --------------------------------------------------------------------------
def gblur(img, sx, sy):
    """Gaussian blur, periodic in longitude (x), clamped in latitude (y)."""
    pad = int(3 * sx) + 1
    wrap = np.concatenate([img[:, -pad:], img, img[:, :pad]], axis=1)
    out = cv2.GaussianBlur(wrap.astype(np.float32), (0, 0), sigmaX=sx, sigmaY=max(sy, 1e-3),
                           borderType=cv2.BORDER_REPLICATE)
    return out[:, pad:-pad]


def nconv(val, wgt, sx, sy):
    """Normalised convolution: blur(val*w)/blur(w); also returns blur(w)."""
    if val.ndim == 3:
        num = np.stack([gblur(val[..., c] * wgt, sx, sy) for c in range(val.shape[2])], -1)
        den = gblur(wgt, sx, sy)[..., None]
    else:
        num = gblur(val * wgt, sx, sy)
        den = gblur(wgt, sx, sy)
    return num / np.maximum(den, 1e-6), (den[..., 0] if val.ndim == 3 else den)


def decay_rows(lat, lat_lo, lat_hi, ramp_deg):
    """1 inside [lat_lo, lat_hi], smoothstep to 0 over ramp_deg outside."""
    d = np.maximum(lat_lo - lat, 0) + np.maximum(lat - lat_hi, 0)
    t = np.clip(1.0 - d / max(ramp_deg, 1e-6), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def blend(L0, L1, w0, w1):
    s = w0 + w1
    out = (L0 * w0[..., None] + L1 * w1[..., None]) / np.maximum(s, 1e-6)[..., None]
    out[s <= 1e-4] = 0
    return out


# --------------------------------------------------------------------------
#  corrections: each returns corrected (L0, L1)
# --------------------------------------------------------------------------
def corr_gain(L0, L1, cov):
    m0 = np.array([np.mean(L0[..., c][cov]) for c in range(3)])
    m1 = np.array([np.mean(L1[..., c][cov]) for c in range(3)])
    g0 = np.clip(np.sqrt(m1 / m0), 0.5, 2.0)
    return L0 * g0, L1 / g0


def corr_colgain(L0, L1, cov, lat, ov, ramp):
    covf = cov.astype(np.float32)
    n = covf.sum(0)
    lr = np.log(np.maximum(L1, EPS)) - np.log(np.maximum(L0, EPS))
    colmean = (lr * covf[..., None]).sum(0) / np.maximum(n, 1)[:, None]
    # smooth along longitude (sigma 48 cols = 4.2 deg), weight by coverage count
    num = np.stack([gblur((colmean[:, c] * n)[None, :], 48, 0)[0] for c in range(3)], -1)
    den = gblur(n[None, :].astype(np.float32), 48, 0)[0]
    d = num / np.maximum(den, 1e-6)[:, None]  # (w, 3) log ratio per column
    k1 = decay_rows(lat, -90, ov, ramp)[:, None, None]    # lens1 = top: applies below ov
    k0 = decay_rows(lat, -ov, 90, ramp)[:, None, None]
    return L0 * np.exp(0.5 * d[None] * k0), L1 * np.exp(-0.5 * d[None] * k1)


def lowfreq_field(L0, L1, cov, sx, sy, fill_sx, fill_sy):
    """Smooth log-difference field over the band, hole-filled beyond coverage."""
    lr = np.log(np.maximum(L1, EPS)) - np.log(np.maximum(L0, EPS))
    covf = cov.astype(np.float32)
    D, den = nconv(lr, covf, sx, sy)
    # fill: a much wider normalised convolution of D where den is weak
    Df, _ = nconv(D, np.clip(den, 0, 1), fill_sx, fill_sy)
    a = np.clip(den / 0.5, 0, 1)[..., None]
    return a * D + (1 - a) * Df


def corr_lowfreq(L0, L1, cov, lat, ov, ramp):
    D = lowfreq_field(L0, L1, cov, 24, 12, 128, 48)
    k1 = decay_rows(lat, -90, ov, ramp)[:, None, None]
    k0 = decay_rows(lat, -ov, 90, ramp)[:, None, None]
    return L0 * np.exp(0.5 * D * k0), L1 * np.exp(-0.5 * D * k1)


def corr_affine(L0, L1, cov, lat, ov, ramp, r=24, eps=4e-4):
    """Guided-filter style local affine transfer, regularised to a = 1."""
    covf = cov.astype(np.float32)
    out0, out1 = np.empty_like(L0), np.empty_like(L1)
    k1 = decay_rows(lat, -90, ov, ramp)[:, None]
    k0 = decay_rows(lat, -ov, 90, ramp)[:, None]
    for c in range(3):
        x, y = L0[..., c], L1[..., c]
        mx, den = nconv(x, covf, r, r)
        my, _ = nconv(y, covf, r, r)
        mxx, _ = nconv(x * x, covf, r, r)
        mxy, _ = nconv(x * y, covf, r, r)
        vx = np.maximum(mxx - mx * mx, 0)
        cxy = mxy - mx * my
        a = (cxy + eps) / (vx + eps)          # -> 1 on flat content
        a = np.clip(a, 0.5, 2.0)
        b = my - a * mx
        # second smoothing of the coefficients (as the guided filter does)
        a = gblur(a, r, r)
        b = gblur(b, r, r)
        # outside coverage: fill coefficients by wider normalised convolution
        w = np.clip(den / 0.5, 0, 1)
        af, _ = nconv(a, w, 128, 48)
        bf, _ = nconv(b, w, 128, 48)
        a = w * a + (1 - w) * af
        b = w * b + (1 - w) * bf
        # half-way: lens0 -> (x + a x + b)/2, lens1 -> (y + (y - b)/a)/2
        a0, b0 = (1 + a) / 2, b / 2
        a1, b1 = (1 + 1 / a) / 2, -b / (2 * a)
        a0 = 1 + (a0 - 1) * k0
        b0 = b0 * k0
        a1 = 1 + (a1 - 1) * k1
        b1 = b1 * k1
        out0[..., c] = np.maximum(a0 * x + b0, 0)
        out1[..., c] = np.maximum(a1 * y + b1, 0)
    return out0, out1


def poisson_blend(L0, L1, w0, w1):
    """Gradient-domain blend of log RGB; rows 0 and h-1 are Dirichlet."""
    h, w = w0.shape
    s = np.maximum(w0 + w1, 1e-6)
    t1 = w1 / s
    out = np.empty_like(L0)
    for c in range(3):
        l0 = np.log(np.maximum(L0[..., c], EPS))
        l1 = np.log(np.maximum(L1[..., c], EPS))
        v0 = w0 > 1e-4
        v1 = w1 > 1e-4
        l0 = np.where(v0, l0, l1)
        l1 = np.where(v1, l1, l0)
        # weighted forward-difference gradients
        gx0 = np.roll(l0, -1, 1) - l0
        gx1 = np.roll(l1, -1, 1) - l1
        gy0 = np.vstack([l0[1:] - l0[:-1], np.zeros((1, w))])
        gy1 = np.vstack([l1[1:] - l1[:-1], np.zeros((1, w))])
        tx = 0.5 * (t1 + np.roll(t1, -1, 1))
        ty = np.vstack([0.5 * (t1[1:] + t1[:-1]), t1[-1:]])
        gx = (1 - tx) * gx0 + tx * gx1
        gy = (1 - ty) * gy0 + ty * gy1
        # divergence (backward differences)
        div = gx - np.roll(gx, 1, 1)
        div[1:] += gy[1:] - gy[:-1]
        div[0] += gy[0]
        # boundary values: feather blend at first/last row
        ref = np.log(np.maximum(blend(L0, L1, w0, w1)[..., c], EPS))
        top, bot = ref[0], ref[-1]
        # unknowns rows 1..h-2; periodic in x -> FFT
        n = h - 2
        rhs = div[1:-1].copy()
        rhs[0] -= top
        rhs[-1] -= bot
        R = np.fft.rfft(rhs, axis=1)
        k = np.arange(R.shape[1])
        lam = 2 * np.cos(2 * np.pi * k / w) - 2  # x eigenvalues
        # tridiagonal in y for each k: u[j-1] + (lam-2) u[j] + u[j+1] = R[j]
        diag = (lam - 2)[None, :].repeat(n, 0).astype(np.complex128)
        U = np.empty_like(R)
        cp = np.empty_like(R)
        dp = np.empty_like(R)
        cp[0] = 1.0 / diag[0]
        dp[0] = R[0] / diag[0]
        for j in range(1, n):
            m = diag[j] - cp[j - 1]
            cp[j] = 1.0 / m
            dp[j] = (R[j] - dp[j - 1]) / m
        U[-1] = dp[-1]
        for j in range(n - 2, -1, -1):
            U[j] = dp[j] - cp[j] * U[j + 1]
        u = np.fft.irfft(U, n=w, axis=1)
        full = np.vstack([top[None], u, bot[None]])
        out[..., c] = np.exp(full)
    out[(w0 + w1) <= 1e-4] = 0
    return out


def two_band(L0, L1, w0, w1, lat, ov):
    """Low band (sigma 32 px) blended with a wide linear ramp over the whole
    overlap, high band with the production weights."""
    t_wide = np.clip((lat - (-ov)) / (2 * ov), 0, 1)[:, None]  # 0 at -ov -> 1 at +ov (lens1 top)
    t_wide = t_wide * t_wide * (3 - 2 * t_wide)
    v0, v1 = w0 > 1e-4, w1 > 1e-4
    t_wide = np.where(~v0, 1.0, np.where(~v1, 0.0, t_wide))
    lo0 = np.stack([gblur(L0[..., c], 32, 32) for c in range(3)], -1)
    lo1 = np.stack([gblur(L1[..., c], 32, 32) for c in range(3)], -1)
    lo = lo0 * (1 - t_wide[..., None]) + lo1 * t_wide[..., None]
    hi = blend(L0 - lo0, L1 - lo1, w0, w1)
    out = np.maximum(lo + hi, 0)
    out[(w0 + w1) <= 1e-4] = 0
    return out


# --------------------------------------------------------------------------
#  metrics
# --------------------------------------------------------------------------
def metrics(img, lat, cols, ov):
    Y = np.maximum(bandio.luma(img[..., :3]), EPS)
    ly = np.log2(Y)[:, cols]
    X = np.stack([np.ones_like(lat), lat, lat * lat], 1)
    P = np.linalg.pinv(X)
    res = ly - X @ (P @ ly)
    bump = np.sqrt(np.mean(res ** 2)) * 1000
    # curvature on 32-column block averages
    nb = ly.shape[1] // 32
    blk = ly[:, :nb * 32].reshape(ly.shape[0], nb, 32).mean(2)
    blk = cv2.GaussianBlur(blk.astype(np.float32), (0, 0), sigmaX=1e-3, sigmaY=2.0)
    dlat = abs(lat[1] - lat[0])
    d2 = (blk[2:] - 2 * blk[1:-1] + blk[:-2]) / (dlat * dlat)
    curv = np.percentile(np.abs(d2), 99) * 1000
    # vertical-edge proxy in the overlap rows
    full = np.log2(Y)
    hp = full - gblur(full, 40, 0.001)
    rows = np.abs(lat) <= ov
    lon = np.percentile(np.abs(hp[rows][:, cols]), 99) * 1000
    ch = []
    for c in (0, 2):
        r = np.log2(np.maximum(img[..., c], EPS) / np.maximum(img[..., 1], EPS))[:, cols]
        rr = r - X @ (P @ r)
        ch.append(np.sqrt(np.mean(rr ** 2)) * 1000)
    return bump, curv, lon, ch[0], ch[1]


def detrended_view(img, lat, gain=12.0):
    """Log luminance minus its per-column quadratic fit, amplified - makes a
    seam band visible to the eye; mid-grey = on the smooth trend."""
    ly = np.log2(np.maximum(bandio.luma(img[..., :3]), EPS))
    X = np.stack([np.ones_like(lat), lat, lat * lat], 1)
    res = ly - X @ (np.linalg.pinv(X) @ ly)
    return np.clip(0.5 + res * gain * 0.1, 0, 1)


# --------------------------------------------------------------------------
def main():
    frames = [int(a) for a in sys.argv[1:]] or [0, 32, 64]
    m = bandio.meta()
    w = m["w"]
    lat = bandio.lat_deg()
    ov = (m["lensFovDeg"] / 2.0) - 90.0      # overlap half-height, deg (7.59)
    ramp = 16.0 - ov                          # decay to the band edge
    cols = np.zeros(w, bool)
    cols[int(0.20 * w):int(0.44 * w)] = True  # open sky, both lenses unoccluded + the occlusion edge
    cols_all_sky = np.zeros(w, bool)
    cols_all_sky[int(0.04 * w):int(0.46 * w)] = True
    outdir = os.path.join(bandio.BANDS, "exp")
    os.makedirs(outdir, exist_ok=True)
    names = ["prod", "reblend", "gain", "colgain", "lowfreq", "affine", "poisson", "lowfreq+2band"]
    acc = {n: [] for n in names}
    for f in frames:
        P = bandio.load(f, "blend")
        A0 = bandio.load(f, "lens0")
        A1 = bandio.load(f, "lens1")
        L0, L1 = A0[..., :3].astype(np.float64), A1[..., :3].astype(np.float64)
        w0, w1 = A0[..., 3].astype(np.float64), A1[..., 3].astype(np.float64)
        cov = (w0 > 1e-4) & (w1 > 1e-4)
        res = {}
        tm = {}
        res["prod"] = P[..., :3]
        t = time.perf_counter(); res["reblend"] = blend(L0, L1, w0, w1); tm["reblend"] = time.perf_counter() - t
        t = time.perf_counter(); a, b = corr_gain(L0, L1, cov); res["gain"] = blend(a, b, w0, w1); tm["gain"] = time.perf_counter() - t
        t = time.perf_counter(); a, b = corr_colgain(L0, L1, cov, lat, ov, ramp); res["colgain"] = blend(a, b, w0, w1); tm["colgain"] = time.perf_counter() - t
        t = time.perf_counter(); a, b = corr_lowfreq(L0, L1, cov, lat, ov, ramp); res["lowfreq"] = blend(a, b, w0, w1); tm["lowfreq"] = time.perf_counter() - t
        res["lowfreq+2band"] = two_band(a, b, w0, w1, lat, ov)
        t = time.perf_counter(); a, b = corr_affine(L0, L1, cov, lat, ov, ramp); res["affine"] = blend(a, b, w0, w1); tm["affine"] = time.perf_counter() - t
        t = time.perf_counter(); res["poisson"] = poisson_blend(L0, L1, w0, w1); tm["poisson"] = time.perf_counter() - t
        if f == frames[0]:
            dif = np.abs(res["reblend"] - P[..., :3]).max()
            print(f"harness check: max |reblend - prod| = {dif:.2e}")
            print("numpy CPU time per variant (s, not representative of GPU):",
                  {k: round(v, 2) for k, v in tm.items()})
        for n in names:
            acc[n].append(metrics(res[n], lat, cols, ov))
        # previews for this frame: sky half only
        sky = slice(int(0.0 * w), int(0.5 * w))
        rows_img, rows_det = [], []
        for n in names:
            if n == "reblend":
                continue
            im = bandio.tonemap(res[n][:, sky], 1.6)
            rows_img.append(im)
            rows_img.append(np.ones((4, im.shape[1], 3)))
            d = detrended_view(res[n], lat)[:, sky]
            rows_det.append(np.repeat(d[..., None], 3, -1))
            rows_det.append(np.ones((4, d.shape[1], 3)))
        for tag, rows in (("img", rows_img), ("detrend", rows_det)):
            pic = np.concatenate(rows, 0)
            pic = cv2.resize(pic.astype(np.float32), (pic.shape[1] // 2, pic.shape[0] // 2),
                             interpolation=cv2.INTER_AREA)
            cv2.imwrite(os.path.join(outdir, f"f{f}_{tag}.png"),
                        (pic[..., ::-1] * 255 + 0.5).astype(np.uint8))

    print(f"\nframes {frames}; sky columns {cols.sum()}; overlap +-{ov:.2f} deg; decay {ramp:.1f} deg")
    print(f"{'variant':15s} {'bumpRMS':>8s} {'curvP99':>8s} {'lonP99':>8s} {'R/G rms':>8s} {'B/G rms':>8s}"
          "   (millistops; lower = less visible seam)")
    for n in names:
        v = np.mean(np.array(acc[n]), 0)
        print(f"{n:15s} {v[0]:8.1f} {v[1]:8.1f} {v[2]:8.1f} {v[3]:8.1f} {v[4]:8.1f}")
    print("previews:", outdir)


if __name__ == "__main__":
    main()
