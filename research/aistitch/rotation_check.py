"""Question 3 / build-next item 1: is the per-clip lens rotation the same
whatever flow measures it, and on every frame? (research only)

Fits the 3-parameter relative rotation w (depth_eval.fit_rotation) to the
far-field columns from three independent flows - SEA-RAFT-S (from
depth_eval.py's saved field), classical DIS per pixel (OpenCV, medium, on the
same 4096 RGB band), and the production DIS grid - on frames 0 / 32 / 64,
and scores "rotation only" for each.

    python rotation_check.py
"""
import json
import os
import warnings

import numpy as np

warnings.filterwarnings("ignore")

from common import OUT, utf8_console  # noqa: E402
from depth_eval import fit_rotation  # noqa: E402
from flow_eval import Scoring, production_grid_flow, rgb4096_inputs, run_dis, to_scoring_grid  # noqa: E402
from common import region_cols  # noqa: E402

utf8_console()


def main():
    out = {}
    for fr in (0, 32, 64):
        sc = Scoring(fr)
        far = region_cols(1110, 1870, sc.w)
        z = np.load(os.path.join(OUT, "depth", f"parallax_f{fr}.npz"))
        imgs, lat, w_in = rgb4096_inputs(fr, sc)
        f_dis, _ = run_dis(imgs, "medium")
        f_dis = to_scoring_grid(f_dis, lat, w_in, sc)
        sources = {"SEA-RAFT-S": (z["f_fw"], z["conf"]),
                   "DIS per pixel": (f_dis, (sc.a0 > 0.5) & (sc.a1 > 0.5)),
                   "production DIS grid": (production_grid_flow(fr, sc), (sc.a0 > 0.5) & (sc.a1 > 0.5))}
        for name, (f, conf) in sources.items():
            w, r = fit_rotation(f, conf, sc, far)
            s = sc.score(r)
            out.setdefault(name, {})[str(fr)] = {"w": w.tolist(), "deg": float(np.degrees(np.linalg.norm(w))),
                                                 "ground": s["ground"], "sky": s["sky"], "band": s["band"]}
            print(f"f{fr} {name:22s} w = ({w[0]:+.5f}, {w[1]:+.5f}, {w[2]:+.5f}) |w| {np.degrees(np.linalg.norm(w)):.3f}"
                  f" deg -> ground {s['ground']:.4f} band {s['band']:.4f}", flush=True)
    with open(os.path.join(OUT, "depth", "rotation_check.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
