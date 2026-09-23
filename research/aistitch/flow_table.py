"""Markdown table of flow_eval.py results (mean over frames 0 / 32 / 64)."""
import json
import os
import sys

import numpy as np

from common import OUT, utf8_console

utf8_console()
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT, "flow_eval_rgb4096.json")
with open(path, "r", encoding="utf-8") as f:
    res = json.load(f)
cols = ["sky", "ground", "wing", "band", "wingEdgeNcc", "wingChamferPx", "flowDeg_sky", "ms"]
print("| model | sky | ground | wing | band | wing edge NCC | wing chamfer px | sky flow deg | ms / band |")
print("|---|---|---|---|---|---|---|---|---|")
for name, per in res.items():
    if "error" in per:
        print(f"| {name} | failed: {per['error'][:60]} |||||||")
        continue
    frames = [per[k] for k in ("0", "32", "64") if k in per]
    if not frames:
        continue
    vals = []
    for c in cols:
        xs = [fr[c] for fr in frames if c in fr and fr[c] is not None]
        if not xs:
            vals.append("-")
        elif c == "ms":
            vals.append(f"{np.median(xs):.0f}")
        elif c in ("wingChamferPx", "flowDeg_sky"):
            vals.append(f"{np.mean(xs):.2f}")
        else:
            vals.append(f"{np.mean(xs):.3f}")
    print(f"| {name} | " + " | ".join(vals) + " |")
