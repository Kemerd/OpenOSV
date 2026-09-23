"""Question 4: how much multi-view signal does the sample clip hold for a
Gaussian splat / NeRF? (research only)

A radiance field learns geometry only from parallax: the same surface seen
from different camera positions.  Here the camera is bolted to the wing, so
across the clip the camera moves with the aircraft.  This measures, in the
master lens alone (bandprobe 4096-column bands, +40..-5 deg latitude):

  * the motion of every pixel between frame 0 and frame 64 (1.07 s),
    SEA-RAFT-S Spring flow;
  * the part a pure camera rotation explains (3 numbers, fitted robustly on
    the ground columns), and the residual = translation parallax, which is
    the only thing a splat could learn depth from;
  * the same statistics on the wing / nacelle columns, which ride with the
    camera.

    python splat_probe.py
"""
import json
import os
import warnings

import numpy as np

warnings.filterwarnings("ignore")

from common import OUT, REGIONS_2048, ProbeBands, srgb_encode, utf8_console  # noqa: E402
from flow_eval import BANDS45, TorchFlow  # noqa: E402
from sphere import basis_lonlat, dir_from_lonlat  # noqa: E402

utf8_console()


def main():
    pb = ProbeBands(BANDS45)
    lat_all = pb.lat_deg()
    rows = np.nonzero((lat_all <= 40.0) & (lat_all >= -5.0))[0]
    lat = lat_all[rows]
    imgs = []
    alphas = []
    for fr in (0, 64):
        b = pb.load(fr, "lens1")[rows]
        rgb = srgb_encode(b[..., :3], 2.0) * (b[..., 3:4] > 0.5)
        imgs.append((rgb[..., ::-1] * 255 + 0.5).astype(np.uint8).copy())
        alphas.append(b[..., 3])
    model = TorchFlow("sea_raft_s:spring")
    hh = imgs[0].shape[0]
    flow, ms = model.tiles(imgs, 2 * hh, hh // 2)                  # px of the 4096-column map

    # rotation model: flow = w x d projected on lon / lat, in px
    w_px = pb.w / (2 * np.pi)
    h_px = pb.map_h / np.pi
    lon = -np.pi + (np.arange(pb.w) + 0.5) * 2 * np.pi / pb.w
    L, A = np.meshgrid(lon, np.radians(lat))
    d = dir_from_lonlat(L, A)
    e_lon, e_lat = basis_lonlat(L, A)
    ax = np.cross(d, e_lon) / np.maximum(np.cos(A), 0.05)[..., None] * w_px
    ay = -np.cross(d, e_lat) * h_px

    def cols(name):
        c0, c1 = REGIONS_2048[name]
        return np.arange(c0 * 2, (c1 + 1) * 2)

    valid = (alphas[0] > 0.5) & (alphas[1] > 0.5)
    g = np.zeros_like(valid)
    g[:, cols("ground")] = True
    g &= valid
    Am = np.concatenate([ax[g], ay[g]], 0)
    bm = np.concatenate([flow[..., 0][g], flow[..., 1][g]], 0)
    wv, *_ = np.linalg.lstsq(Am, bm, rcond=None)
    for _ in range(3):                                              # IRLS, Huber
        res = Am @ wv - bm
        s = 1.4826 * np.median(np.abs(res)) + 1e-6
        wt = 1.0 / np.maximum(1.0, np.abs(res) / (1.5 * s))
        wv, *_ = np.linalg.lstsq(Am * wt[:, None], bm * wt, rcond=None)
    rot = np.stack([ax @ wv, ay @ wv], -1)
    resid = flow - rot
    deg_per_px = 360.0 / pb.w

    def stats(mask, f):
        m = np.hypot(f[..., 0], f[..., 1])[mask] * deg_per_px
        return {"median_deg": float(np.median(m)), "p90_deg": float(np.percentile(m, 90)), "n": int(mask.sum())}

    wing = np.zeros_like(valid)
    wing[:, cols("wing")] = True
    wing &= valid
    near = wing & (np.hypot(flow[..., 0], flow[..., 1]) * deg_per_px < 0.5)   # moves with the camera
    out = {
        "frames": [0, 64], "seconds": 64 / 59.94,
        "rotation_rad": wv.tolist(), "rotation_deg": float(np.degrees(np.linalg.norm(wv))),
        "ground_total_motion": stats(g, flow),
        "ground_residual_after_rotation": stats(g, resid),
        "wing_columns_total_motion": stats(wing, flow),
        "wing_columns_static_fraction": float(near.sum() / max(wing.sum(), 1)),
        "flow_ms": ms,
    }
    os.makedirs(os.path.join(OUT, "splat"), exist_ok=True)
    with open(os.path.join(OUT, "splat", "splat_probe.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
