"""Tabulate the production parallax region scores dumped by dump_flow_bands.ps1."""
import json
import os
import sys

from common import OUT, utf8_console

utf8_console()
d = os.path.join(OUT, "flowbands")
rows = []
for f in (0, 32, 64):
    for be in ("classical", "neural"):
        for reg in ("sky", "ground", "wing"):
            p = os.path.join(d, f"prod_f{f}_{be}_{reg}.json")
            if not os.path.exists(p):
                continue
            with open(p, "r", encoding="utf-8-sig") as fh:
                txt = fh.read()
            j = json.loads(txt[txt.index("{"):])
            px = j.get("parallax", {})
            r = px.get("region", {})
            rows.append((f, be, reg, r.get("none", {}).get("ncc"), r.get("parallax", {}).get("ncc"),
                         px.get("nccAfter"), px.get("flowMs"), px.get("meanDisparityDeg")))
print("frame backend   region  ncc_none  ncc_after  whole_after  flowMs  meanCorrDeg")
for r in rows:
    print(f"{r[0]:5d} {r[1]:9s} {r[2]:7s} {r[3]:8.4f} {r[4]:9.4f} {r[5]:11.4f} {r[6]:7.2f} {r[7]:10.4f}")
json.dump(rows, open(os.path.join(d, "prod_table.json"), "w"), indent=1)
