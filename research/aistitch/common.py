"""Shared helpers for the WP-AISTITCH research scripts (research only, not product code).

Data sources (all rendered by the release build, nothing synthetic):
  * bandprobe (research/neural/bandprobe.cpp, built by build_probe.cmd):
    per-lens scene-linear RGBA bands of the polar-axis map, alpha = that
    lens's production blend weight; f<F>_lens0.f32 / f<F>_lens1.f32 /
    f<F>_blend.f32 + meta.json + theta.f32.
  * osvtool seam --dump-bands: per-lens code-space luma + coverage of the
    parallax analysis band (the input production DIS sees) - <prefix>_none_luma0.f32 ...

Polar-axis layout: the lens axes are the poles, the seam is the equator.
+latitude is towards lens 1 (the master) on the sample clip.
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
MODELS = os.path.join(HERE, "models")
CLIP = os.environ.get("OSV_SAMPLE_FILE", "L:/Dev/premiere_360_reframe/example_footage_dlogm.OSV")

# Regions of the 2048-column analysis band (NEURAL_STITCHING.md section 1.6)
REGIONS_2048 = {"sky": (410, 900), "ground": (1110, 1700), "wing": (1880, 2040)}


def utf8_console():
    """Keep the Windows console from choking on non-ASCII output."""
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


# --------------------------------------------------------------------------
#  bandprobe output
# --------------------------------------------------------------------------
class ProbeBands:
    """Loader for one bandprobe directory."""

    def __init__(self, directory):
        self.dir = directory
        with open(os.path.join(directory, "meta.json"), "r", encoding="utf-8") as f:
            self.meta = json.load(f)
        self.w = int(self.meta["w"])
        self.h = int(self.meta["h"])
        self.map_h = int(self.meta["mapH"])
        self.row0 = int(self.meta["row0"])

    def load(self, frame, tag):
        """RGBA float32 (h, w, 4) for tag in lens0 / lens1 / blend."""
        a = np.fromfile(os.path.join(self.dir, f"f{frame}_{tag}.f32"), dtype=np.float32)
        if a.size != self.w * self.h * 4:
            raise ValueError(f"{tag} f{frame}: {a.size} floats, expected {self.w * self.h * 4}")
        return a.reshape(self.h, self.w, 4)

    def theta(self):
        a = np.fromfile(os.path.join(self.dir, "theta.f32"), dtype=np.float32)
        return a.reshape(self.h, self.w, 2)

    def lat_deg(self):
        rows = np.arange(self.h, dtype=np.float64) + self.row0 + 0.5
        return 90.0 - rows * 180.0 / self.map_h

    def rows_for(self, half_deg):
        """Slice of band rows covering |lat| <= half_deg."""
        lat = self.lat_deg()
        idx = np.nonzero(np.abs(lat) <= half_deg + 1e-9)[0]
        return slice(int(idx[0]), int(idx[-1]) + 1)


# --------------------------------------------------------------------------
#  osvtool seam --dump-bands output
# --------------------------------------------------------------------------
def load_dump(prefix, tag, dump_json):
    """(luma0, luma1, alpha0, alpha1, info) for tag none / parallax."""
    with open(dump_json, "r", encoding="utf-8-sig") as f:
        info = json.load(f)["parallax"]["dump"]
    w, h = int(info["w"]), int(info["h"])
    planes = []
    for name in ("luma0", "luma1", "alpha0", "alpha1"):
        a = np.fromfile(f"{prefix}_{tag}_{name}.f32", dtype=np.float32)
        if a.size != w * h:
            raise ValueError(f"{prefix}_{tag}_{name}: {a.size} floats, expected {w * h}")
        planes.append(a.reshape(h, w))
    return planes[0], planes[1], planes[2], planes[3], info


# --------------------------------------------------------------------------
#  Colour helpers
# --------------------------------------------------------------------------
def luma709(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def srgb_encode(lin, exposure=1.0):
    """Scene-linear -> display sRGB [0,1] with a soft shoulder (Reinhard on
    luma-preserving max) so the sky and sun do not clip hard."""
    x = np.clip(lin * exposure, 0.0, None)
    x = x / (1.0 + 0.25 * x)                     # gentle shoulder, ~linear below 0.5
    a = 0.055
    y = np.where(x <= 0.0031308, 12.92 * x, (1 + a) * np.power(np.maximum(x, 1e-12), 1 / 2.4) - a)
    return np.clip(y, 0.0, 1.0)


def srgb_decode(y, exposure=1.0):
    """Inverse of srgb_encode (display -> scene-linear)."""
    y = np.clip(y, 0.0, 1.0)
    a = 0.055
    x = np.where(y <= 0.04045, y / 12.92, np.power((y + a) / (1 + a), 2.4))
    x = np.clip(x, 0.0, 3.99)
    lin = x / (1.0 - 0.25 * x)
    return lin / exposure


# --------------------------------------------------------------------------
#  Metrics
# --------------------------------------------------------------------------
def ncc(a, b, mask):
    """Global normalised cross-correlation over mask (osvtool scoreRegion)."""
    x = a[mask].astype(np.float64)
    y = b[mask].astype(np.float64)
    if x.size < 16:
        return float("nan")
    x -= x.mean()
    y -= y.mean()
    d = np.sqrt((x * x).sum() * (y * y).sum())
    return float((x * y).sum() / d) if d > 0 else 0.0


def region_cols(c0, c1, w, scale=1.0):
    """Column indices of region [c0, c1] of a 2048 band mapped to width w (wraps)."""
    a = int(round(c0 * scale))
    b = int(round((c1 + 1) * scale)) - 1
    if b >= a:
        return np.arange(a, b + 1) % w
    return np.concatenate([np.arange(a, w), np.arange(0, b + 1)])


def bilinear_sample(img, x, y):
    """Sample img (h, w[, c]) at float coords, longitude wraps, rows clamp.
    Returns (values, valid) where valid is False outside the rows."""
    h, w = img.shape[:2]
    valid = (y >= 0) & (y <= h - 1)
    yc = np.clip(y, 0, h - 1.000001)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(yc).astype(np.int64)
    fx = (x - x0).astype(np.float32)
    fy = (yc - y0).astype(np.float32)
    x0w = x0 % w
    x1w = (x0 + 1) % w
    y1 = np.minimum(y0 + 1, h - 1)
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]
    v = (img[y0, x0w] * (1 - fx) * (1 - fy) + img[y0, x1w] * fx * (1 - fy)
         + img[y1, x0w] * (1 - fx) * fy + img[y1, x1w] * fx * fy)
    return v, valid


def warp_by_flow(img, flow):
    """img sampled at (x + u, y + v): brings the second image onto the first's grid."""
    h, w = flow.shape[:2]
    xs, ys = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return bilinear_sample(img, xs + flow[..., 0], ys + flow[..., 1])


def resize_flow(flow, w_new, h_new, row_scale=None):
    """Resample a flow field (h, w, 2) to (h_new, w_new) by pixel-centre mapping,
    scaling the vectors.  row_scale defaults to the column scale (square px)."""
    import cv2
    h, w = flow.shape[:2]
    sx = w_new / w
    sy = h_new / h if row_scale is None else row_scale
    f = cv2.resize(flow, (w_new, h_new), interpolation=cv2.INTER_LINEAR)
    f[..., 0] *= sx
    f[..., 1] *= sy
    return f


def tonemap_preview(lin, exposure=1.0):
    return (srgb_encode(lin, exposure) * 255.0 + 0.5).astype(np.uint8)
