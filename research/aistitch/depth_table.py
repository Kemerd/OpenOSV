"""Markdown table of depth_eval.py results (per frame and mean)."""
import json
import os

import numpy as np

from common import OUT, utf8_console

utf8_console()
with open(os.path.join(OUT, "depth", "depth_eval.json"), "r", encoding="utf-8") as f:
    d = json.load(f)
res = d["results"]
frames = sorted(res, key=int)
names = list(res[frames[0]]["scores"].keys())
print("| correction | ground NCC | wing NCC | band NCC | wing edge NCC | wing chamfer px | parameter |")
print("|---|---|---|---|---|---|---|")
for n in names:
    rows = [res[f]["scores"][n] for f in frames if n in res[f]["scores"]]
    m = {k: np.mean([r[k] for r in rows]) for k in ("ground", "wing", "band", "wingEdgeNcc", "wingChamferPx")}
    par = ""
    for k in ("offsetPx", "nearPx", "K", "b_mm"):
        if k in rows[0]:
            par = f"{k} = " + ", ".join(f"{r[k]:g}" for r in rows)
    print(f"| {n} | {m['ground']:.3f} | {m['wing']:.3f} | {m['band']:.3f} | {m['wingEdgeNcc']:.3f} | "
          f"{m['wingChamferPx']:.2f} | {par} |")
print()
for f in frames:
    fit = res[f]["fit"]
    w = res[f]["rotation_w"]
    print(f"frame {f}: w = ({w[0]:+.5f}, {w[1]:+.5f}, {w[2]:+.5f}) rad = {np.degrees(np.linalg.norm(w)):.3f} deg, "
          f"residual {res[f]['rotation_residual_rms_px']:.3f} px; object MoGe {fit.get('object_dist_m_metric_moge', 0):.2f} m,"
          f" DA-metric {fit.get('object_dist_m_metric_dav2', 0):.2f} m; ground MoGe {fit.get('ground_dist_m_metric_moge', 0):.1f} m,"
          f" DA-metric {fit.get('ground_dist_m_metric_dav2', 0):.1f} m; K_rel {fit.get('K_rel', float('nan')):.3f}"
          f" (corr {fit.get('corr_p_vs_reldisp', float('nan')):.2f}, n {fit.get('n_wing_conf', 0)})")
print("ms per 518^2 crop:", d["times_ms_per_crop"])
