"""Question 3 picture: the wing crossing under each correction (research only).

Every panel is the 50/50 blend of the two lenses (4096-column RGB bands,
+-10 deg) after the named correction, so a residual misalignment shows as a
double edge.  Inputs come from depth_eval.py's parallax_f<F>.npz.

    python depth_visual.py <frame> <out.png>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

from common import OUT, ProbeBands, bilinear_sample, srgb_encode, utf8_console
from flow_eval import BANDS45, INPUT_HALF_DEG, Scoring, production_grid_flow

utf8_console()


def halves_to_4096(h, sc, lat4096, w4096):
    """Resample a 2048-grid half-flow onto the 4096 band rows/cols (x2 vectors)."""
    xs = (np.arange(w4096) + 0.5) / 2.0 - 0.5
    ys = (sc.lat[0] - lat4096) / (sc.lat[0] - sc.lat[1])
    X, Y = np.meshgrid(xs, ys)
    v, ok = bilinear_sample(h, X.astype(np.float32), Y.astype(np.float32))
    v[~ok] = 0.0
    return v * 2.0


def main():
    fr = int(sys.argv[1])
    out = sys.argv[2]
    z = np.load(os.path.join(OUT, "depth", f"parallax_f{fr}.npz"))
    sc = Scoring(fr)
    pb = ProbeBands(BANDS45)
    rows = pb.rows_for(INPUT_HALF_DEG)
    b0 = pb.load(fr, "lens0")[rows]
    b1 = pb.load(fr, "lens1")[rows]
    lat4 = pb.lat_deg()[rows]
    A = np.radians(sc.lat)[:, None] * np.ones((1, sc.w))
    r = z["r"]
    k30 = 0.030 * sc.map_h / np.pi

    def depth_halves(b_mm):
        D0 = np.where(np.isfinite(z["moge0"]), z["moge0"], np.inf)
        D1 = np.where(np.isfinite(z["moge1"]), z["moge1"], np.inf)
        rel = 0.5 * (z["rel0"] + z["rel1"])
        far = rel <= np.nanpercentile(rel, 60)             # ground / sky at infinity
        D0[far] = np.inf
        D1[far] = np.inf
        k = b_mm / 30.0 * k30
        h0 = -0.5 * r.copy()
        h1 = 0.5 * r.copy()
        h0[..., 1] -= 0.5 * np.nan_to_num(k * np.cos(A) / D0)
        h1[..., 1] += 0.5 * np.nan_to_num(k * np.cos(A) / D1)
        return h0, h1

    f_prod = production_grid_flow(fr, sc)
    variants = [
        ("uncorrected", np.zeros_like(r), np.zeros_like(r)),
        ("production DIS grid", -0.5 * f_prod, 0.5 * f_prod),
        ("rotation only (3 numbers per clip)", -0.5 * r, 0.5 * r),
        ("rotation + depth, b = +30 mm (MoGe-2)", *depth_halves(30.0)),
        ("rotation + depth, b = -30 mm (MoGe-2)", *depth_halves(-30.0)),
        ("SEA-RAFT-S flow", -0.5 * z["f_fw"], 0.5 * z["f_fw"]),
    ]
    c0, c1 = 3640, 4096
    tiles = []
    for name, h0, h1 in variants:
        H0 = halves_to_4096(h0, sc, lat4, pb.w)
        H1 = halves_to_4096(h1, sc, lat4, pb.w)
        xs, ys = np.meshgrid(np.arange(pb.w, dtype=np.float32), np.arange(len(lat4), dtype=np.float32))
        w0, _ = bilinear_sample(b0, xs + H0[..., 0], ys + H0[..., 1])
        w1, _ = bilinear_sample(b1, xs + H1[..., 0], ys + H1[..., 1])
        a0 = (w0[..., 3] > 0.5)[..., None]
        a1 = (w1[..., 3] > 0.5)[..., None]
        both = a0 & a1
        blend = np.where(both, 0.5 * (w0[..., :3] + w1[..., :3]), np.where(a0, w0[..., :3], w1[..., :3]))
        img = (srgb_encode(blend[:, c0:c1], 2.0) * 255 + 0.5).astype(np.uint8)
        im = Image.fromarray(img).resize(((c1 - c0) * 2, img.shape[0] * 2), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 8 * len(name) + 8, 16], fill=(0, 0, 0))
        d.text((4, 2), name, fill=(255, 255, 255))
        tiles.append(np.asarray(im))
    grid = np.concatenate([np.concatenate(tiles[i:i + 2], 1) for i in range(0, len(tiles), 2)], 0)
    Image.fromarray(grid).save(out)
    print("wrote", out, grid.shape)


if __name__ == "__main__":
    main()
