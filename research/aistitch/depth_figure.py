"""Diagnostic figure for question 3: the wing crossing, per-lens depth, and the
measured meridian parallax (research only).

    python depth_figure.py <frame> <out.png>
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from common import OUT, utf8_console  # noqa: E402
from flow_eval import Scoring  # noqa: E402

utf8_console()


def main():
    fr = int(sys.argv[1])
    out = sys.argv[2]
    z = np.load(os.path.join(OUT, "depth", f"parallax_f{fr}.npz"))
    sc = Scoring(fr)
    c0, c1 = 1840, 2048
    sl = np.s_[:, c0:c1]
    panels = [
        ("lens 0 luma", sc.l0[sl], "gray", None),
        ("lens 1 luma", sc.l1[sl], "gray", None),
        ("SEA-RAFT flow f_y minus rotation (px @2048)", np.where(z["conf"][sl], z["p"][sl], np.nan), "coolwarm", (-4, 4)),
        ("DA-V2-S rel. disparity, lens 0", z["rel0"][sl], "magma", None),
        ("DA-V2-S rel. disparity, lens 1", z["rel1"][sl], "magma", None),
        ("MoGe-2-S distance lens 1 (m)", z["moge1"][sl] if z["moge1"].ndim == 2 else np.zeros((sc.h, c1 - c0)),
         "viridis_r", (0, 20)),
    ]
    fig, axs = plt.subplots(len(panels), 1, figsize=(10, 2.1 * len(panels)))
    ext = [c0, c1, sc.lat[-1], sc.lat[0]]
    for ax, (t, img, cm, lim) in zip(axs, panels):
        kw = {"vmin": lim[0], "vmax": lim[1]} if lim else {}
        im = ax.imshow(img, cmap=cm, aspect="auto", extent=ext, **kw)
        ax.set_title(t, fontsize=9)
        ax.axhline(0, color="w", lw=0.5, ls="--")
        ax.set_ylabel("lat (deg)")
        fig.colorbar(im, ax=ax, fraction=0.02)
    axs[-1].set_xlabel("band column (of 2048)")
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    print("wrote", out)


if __name__ == "__main__":
    main()
