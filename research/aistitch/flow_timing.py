"""Batched runtime of the flow models on one analysis band (research only).

The band is the 4096-column x +-10 deg overlap band cut into the same 36
overlapping 456 x 228 tiles flow_eval.py uses, sent as ONE batch - how a
product would run it.  Median of 7 runs after 3 warm-ups, fp32 and bf16
autocast, RTX 5090 shared with other jobs (treat as a range).

    python flow_timing.py [models...]
"""
import json
import os
import sys
import time
import warnings

import numpy as np

warnings.filterwarnings("ignore")

from common import OUT, utf8_console  # noqa: E402
from flow_eval import Scoring, TorchFlow, rgb4096_inputs  # noqa: E402

utf8_console()
MODELS = sys.argv[1:] or ["sea_raft_s:spring", "neuflow2:things", "flowseek_t:tar-c-t-tskh",
                          "flowseek_t:tar-c-t-tskh@iters=12", "gma:things", "raft:things"]


def main():
    import torch
    from ptlflow.utils.io_adapter import IOAdapter
    sc = Scoring(32)
    imgs, _, _ = rgb4096_inputs(32, sc)
    h, w = imgs[0].shape[:2]
    tw, step = 2 * h, 2 * h - h // 2
    tiles = [((np.arange(tw) + i * step) % w) for i in range(int(np.ceil(w / step)))]
    out = {}
    for spec in MODELS:
        tf = TorchFlow(spec)
        adapter = IOAdapter(tf.model, (h, tw), cuda=True)
        batch = torch.cat([adapter.prepare_inputs([imgs[0][:, c], imgs[1][:, c]])["images"] for c in tiles], 0)
        res = {}
        for mode in ("fp32", "bf16"):
            times = []
            for rep in range(10):
                torch.cuda.synchronize()
                t0 = time.perf_counter()
                with torch.no_grad(), torch.autocast("cuda", dtype=torch.bfloat16, enabled=(mode == "bf16")):
                    tf.model({"images": batch})
                torch.cuda.synchronize()
                if rep >= 3:
                    times.append((time.perf_counter() - t0) * 1000)
            res[mode] = float(np.median(times))
        res["tiles"] = len(tiles)
        res["tile"] = [h, tw]
        res["params_M"] = tf.params / 1e6
        out[spec] = res
        print(f"{spec:40s} fp32 {res['fp32']:7.1f} ms  bf16 {res['bf16']:7.1f} ms  ({len(tiles)} tiles {tw}x{h},"
              f" {res['params_M']:.1f} M params)", flush=True)
        del tf, batch
        torch.cuda.empty_cache()
    with open(os.path.join(OUT, "flow_timing.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
