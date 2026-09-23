"""Download the third-party research weights WP-AISTITCH measured (never committed).

Everything lands in research/aistitch/models/ (git-ignored) and is logged in
models/DOWNLOADED.txt with its source and licence as read on 2026-09-23.
Research only: whether a weight may ship is decided in docs/research/AI_STITCHING.md.

    python fetch_models.py flow      # Depth Anything V2 S/B (the flow models come via ptlflow)
    python fetch_models.py depth     # logs the depth models depth_eval.py fetches itself
    python fetch_models.py gen       # generative models (large: tens of GB)
"""
import os
import sys
import time

from common import MODELS, utf8_console

utf8_console()
os.makedirs(MODELS, exist_ok=True)
LOG = os.path.join(MODELS, "DOWNLOADED.txt")


def log(line):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M ") + line + "\n")
    print(line)


def hf_file(repo, filename, licence, subdir=None):
    from huggingface_hub import hf_hub_download
    dst_dir = os.path.join(MODELS, subdir) if subdir else MODELS
    p = hf_hub_download(repo, filename, local_dir=dst_dir)
    log(f"{repo}/{filename}  [{licence}]  {os.path.getsize(p)} bytes")
    return p


def hf_repo(repo, licence, allow=None, subdir=None):
    from huggingface_hub import snapshot_download
    dst = os.path.join(MODELS, subdir or repo.replace("/", "__"))
    p = snapshot_download(repo, local_dir=dst, allow_patterns=allow)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs)
    log(f"{repo} -> {os.path.basename(dst)}  [{licence}]  {size / 1e9:.2f} GB")
    return p


def flow():
    # Depth Anything V2 (the FlowSeek / WAFT depth backbone)
    hf_file("depth-anything/Depth-Anything-V2-Small", "depth_anything_v2_vits.pth", "Apache-2.0")
    hf_file("depth-anything/Depth-Anything-V2-Base", "depth_anything_v2_vitb.pth", "CC-BY-NC-4.0 (research only)")
    # Every flow model itself (FlowSeek T/M, WAFT, RAFT, GMA, FlowFormer(++),
    # UniMatch, SEA-RAFT, DPFlow, NeuFlow v2) is fetched by ptlflow 0.4.2 on
    # first use from github.com/hmorimitsu/ptlflow/releases/download/weights1/
    # into models/torch/hub/checkpoints (flow_eval.py sets TORCH_HOME).  Those
    # are ptlflow's conversions of the authors' checkpoints: research use only;
    # a shipping build must take weights from the authors' own release (the
    # licence table in docs/research/AI_STITCHING.md says which ones may ship).
    log("ptlflow checkpoints: fetched on demand by flow_eval.py (see models/torch/hub/checkpoints)")


def depth():
    # depth_eval.py pulls these itself into models/hf through transformers /
    # MoGe's from_pretrained; listed here so the licence log is complete.
    log("depth-anything/Depth-Anything-V2-Small-hf  [Apache-2.0]  via transformers (models/hf)")
    log("depth-anything/Depth-Anything-V2-Metric-Outdoor-Small-hf  [card: see licence table; Virtual KITTI 2 "
        "training data is non-commercial -> research only]  via transformers (models/hf)")
    log("Ruicheng/moge-2-vits-normal  [MIT]  via moge (models/hf)")


def gen():
    # FLUX.2 [klein] 4B: Apache-2.0 (card + LICENSE.md), not gated; diffusers layout only
    hf_repo("black-forest-labs/FLUX.2-klein-4B", "Apache-2.0", subdir="flux2_klein_4b",
            allow=["model_index.json", "scheduler/*", "text_encoder/*", "tokenizer/*", "transformer/*", "vae/*",
                   "*.md"])
    # Wan2.1-VACE 1.3B: Apache-2.0 (card + LICENSE.txt), umT5-XXL text encoder Apache-2.0
    hf_repo("Wan-AI/Wan2.1-VACE-1.3B-diffusers", "Apache-2.0", subdir="wan21_vace_1_3b")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "flow"
    {"flow": flow, "depth": depth, "gen": gen}[what]()
