"""Where does the "slight movement" at the seam come from? (research only)

The carved seam line itself barely moves (seam_stability.py).  What else
changes from frame to frame at the seam is the parallax WARP: the importer
measures a grid once per 8-frame bucket and glides between neighbouring
buckets (parallaxCrossfadeWeight), and each lens is sampled at d +- g, so a
change of g between frames moves that lens's picture by the same angle.

This replays the importer's schedule on the grids seamprobe dumped for every
frame (production DIS, classical) and reports the frame-to-frame change of g
in 6K pixels (6144-column equirect, 17.07 px/deg), over the measured grid
rows, in the sky / ground / wing columns; also the same for a grid measured
on every frame, and the seam table's feather width change.

    python grid_motion.py
"""
import json
import os

import numpy as np

from common import OUT, utf8_console

utf8_console()
SEAM = os.path.join(OUT, "seam")
FRAMES = list(range(0, 65))
BUCKET = 8
PX_PER_DEG_6K = 6144 / 360.0
DECAY_ROWS = 8


def main():
    with open(os.path.join(SEAM, "grid_meta.json"), "r", encoding="utf-8") as f:
        gm = json.load(f)
    gw, gh = gm["w"], gm["h"]
    grids = {f: np.fromfile(os.path.join(SEAM, f"grid_{f}.f32"), np.float32).reshape(gh, gw, 2) for f in FRAMES}
    buckets = {b: grids[b * BUCKET] for b in range(0, 9)}

    def sched(f):
        k = f // BUCKET
        if k == 0:
            return buckets[0]
        w = (f % BUCKET + 1) / BUCKET
        return buckets[k - 1] + (buckets[k] - buckets[k - 1]) * w

    def cols(c0, c1):                                  # of the 2048-column band -> grid columns
        return np.arange(int(c0 * gw / 2048), int((c1 + 1) * gw / 2048))

    regions = {"sky": cols(410, 900), "ground": cols(1110, 1700), "wing": cols(1880, 2040)}
    rows = np.arange(DECAY_ROWS, gh - DECAY_ROWS)       # measured rows only
    out = {}
    for name, series in (("importer schedule (bucket + glide)", sched), ("every frame", lambda f: grids[f])):
        d = []
        for f in FRAMES[:-1]:
            dg = series(f + 1) - series(f)
            d.append(np.degrees(np.hypot(dg[..., 0], dg[..., 1])) * PX_PER_DEG_6K)   # per lens, 6K px
        d = np.array(d)
        res = {}
        for rname, c in regions.items():
            a = d[:, rows][:, :, c]
            res[rname] = {"mean_px": float(a.mean()), "p99_px": float(np.percentile(a, 99)),
                          "max_px": float(a.max()), "frames_with_any_cell_gt_1px": int((a.reshape(len(d), -1).max(1)
                                                                                         > 1.0).sum())}
        out[name] = res
        print(name)
        for rname, r in res.items():
            print(f"   {rname:7s} per-lens picture motion per frame: mean {r['mean_px']:.3f} p99 {r['p99_px']:.2f}"
                  f" max {r['max_px']:.2f} px (6K); frames with a cell moving > 1 px: "
                  f"{r['frames_with_any_cell_gt_1px']}/64")
    # the bucket-to-bucket jumps that the glide spreads over 8 frames
    jumps = []
    for b in range(1, 9):
        dg = buckets[b] - buckets[b - 1]
        jumps.append(np.degrees(np.hypot(dg[..., 0], dg[..., 1]))[rows][:, regions["wing"]] * PX_PER_DEG_6K)
    out["wing bucket-to-bucket change px (max per bucket)"] = [float(j.max()) for j in jumps]
    print("wing, largest bucket-to-bucket change of g per bucket (6K px):",
          [round(float(j.max()), 1) for j in jumps])
    # ---- candidate: one grid per clip (the rig and the wing are rigid) ------------
    # The per-cell median of the nine bucket grids, applied to every frame:
    # zero motion by construction.  What does it cost in alignment?  Scored
    # with flow_eval's harness on frames 0 / 32 / 64 against each frame's own
    # grid (both through the same symmetric warp).
    from common import bilinear_sample
    from flow_eval import Scoring
    median = np.median(np.stack([buckets[b] for b in range(9)]), axis=0)
    ema = buckets[0].copy()
    for b in range(1, 9):
        ema = ema + 0.35 * (buckets[b] - ema)          # WP-PHOTO's temporalAlpha, for reference

    def grid_flow(uv, sc):
        lon = -np.pi + (np.arange(sc.w) + 0.5) * 2 * np.pi / sc.w
        lat = np.radians(sc.lat)
        fx = (lon + np.pi) / (2 * np.pi) * gw
        fy = (lat - gm["latMinRad"]) / (gm["latMaxRad"] - gm["latMinRad"]) * (gh - 1)
        X, Y = np.meshgrid(fx, fy)
        g, ok = bilinear_sample(uv, X.astype(np.float32), Y.astype(np.float32))
        g[~ok] = 0.0
        flow = np.zeros((sc.h, sc.w, 2), np.float32)
        flow[..., 0] = 2.0 * g[..., 0] * sc.w / (2 * np.pi)
        flow[..., 1] = -2.0 * g[..., 1] * sc.map_h / np.pi
        return flow

    align = {}
    for fr in (0, 32, 64):
        sc = Scoring(fr)
        for name, uv in (("own grid", grids[fr]), ("clip median grid", median), ("EMA 0.35 at bucket 8", ema)):
            s = sc.score(grid_flow(uv, sc))
            align.setdefault(name, []).append({k: s[k] for k in ("ground", "wing", "sky", "band")})
    out["alignment_with_clip_median_grid"] = align
    for name, rows_ in align.items():
        m = {k: np.mean([r[k] for r in rows_]) for k in ("ground", "wing", "sky", "band")}
        print(f"{name:22s} ground {m['ground']:.4f} wing {m['wing']:.4f} sky {m['sky']:.4f} band {m['band']:.4f}")
    with open(os.path.join(SEAM, "grid_motion.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
