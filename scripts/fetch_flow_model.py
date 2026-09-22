#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""
fetch_flow_model.py - build the neural optical-flow model the Neural flow
backend loads (src/osv/render/FlowBackendOnnx.cpp).

WHAT IT PRODUCES
----------------
    models/searaft_s_gray.onnx      (~36 MB, fp32, opset 17)

SEA-RAFT, small variant, exported to ONNX with DYNAMIC height and width, a
single-channel [0, 1] input and a [N, 2, H, W] flow output in pixels.  The
file is NOT committed (models/ is gitignored): it is ~36 MB of third-party
weights, so it is rebuilt from pinned upstream sources instead.

WHERE THE PIECES COME FROM (all pinned, all verified)
------------------------------------------------------
  * Code:    github.com/princeton-vl/SEA-RAFT @ SEARAFT_COMMIT, BSD-3-Clause.
  * Weights: huggingface.co/MemorySlices/Tartan-C-T-TSKH432x960-S @ WEIGHTS_REV.
             MemorySlices is the SEA-RAFT authors' own organisation (the
             upstream README links it), and every model card there declares
             bsd-3-clause.  The unaffiliated mirror some tutorials use has no
             licence on its card and is deliberately NOT used.
             The SHA-256 below is checked before the weights are trusted.

WHY THE EXPORT IS NOT A PLAIN torch.onnx.export(RAFT(...))
---------------------------------------------------------
Three upstream constructs would make the exported graph silently wrong or
unusable, and each is replaced here WITHOUT editing the upstream files:

  1. InputPadder computes its pad amounts as Python ints from .shape, so a
     trace BAKES THEM IN: the "dynamic" model would pad every input as if it
     were the trace size.  The inference path is re-implemented below without
     it, and the contract becomes "H and W are multiples of 8" - which the
     C++ tiler guarantees.
  2. CorrBlock.corr scales by torch.sqrt(torch.tensor(dim)), which exports as
     a Shape->Gather->Cast->Sqrt chain.  dim is a constant (feature width),
     so it is folded to a Python float.
  3. ResNetFPN.__init__ downloads ImageNet weights from torchvision that are
     immediately overwritten.  Skipped, so the script needs no torchvision.

The upstream checkpoint also appears to be "missing" 16 keys
(`*.downsample.1.*`).  It is not: BasicBlock registers the same BatchNorm
module twice, as `bn3` and as `downsample[1]`, and the checkpoint stores it
once under `bn3`.  load() below verifies that every missing key is exactly
such an alias and refuses anything else.

MEASURED CONSTRAINTS THE C++ SIDE RELIES ON
-------------------------------------------
  * H and W must be multiples of 8 (no padder in the graph).
  * H and W must be >= 128: the 4-level correlation pyramid halves the 1/8
    feature map four times and fails below 16 feature rows.
  * Input is [N, 1, H, W] float32 in [0, 1]; the graph replicates it to RGB
    and applies SEA-RAFT's own 2*(x*255/255)-1 normalisation internally.
  * Output flow (u, v) at p means content at p in image_a is at p + (u, v)
    in image_b, in pixels - the same convention as osv::render::DisFlow.

USAGE
-----
    python scripts/fetch_flow_model.py              # export + verify
    python scripts/fetch_flow_model.py --fp16       # also write an fp16 file

Requires: torch, onnx, numpy.  onnxruntime is needed for the verification
step (strongly recommended; --skip-verify exists only for air-gapped boxes).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import shutil
import sys
import types
import urllib.request
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
#  Pins.  Changing any of these means re-running the verification and
#  updating the numbers quoted in FlowBackendOnnx.cpp.
# ---------------------------------------------------------------------------
SEARAFT_COMMIT = "9137517ba24e628442aec097d3afe71d03503b75"
SEARAFT_ZIP_URL = f"https://codeload.github.com/princeton-vl/SEA-RAFT/zip/{SEARAFT_COMMIT}"

WEIGHTS_REPO = "MemorySlices/Tartan-C-T-TSKH432x960-S"
WEIGHTS_REV = "de21657a1519e5b14d2a9c10885ec369a8654c05"
WEIGHTS_URL = f"https://huggingface.co/{WEIGHTS_REPO}/resolve/{WEIGHTS_REV}/model.safetensors"
WEIGHTS_SHA256 = "9752a49fca0a3f33551d51fec7bc5bd87f93a155b1e9fadd803e8176f4edb423"

# The S variant's eval configuration (config/eval/sintel-S.json upstream).
# Inlined so the export does not depend on which JSON file a future upstream
# commit keeps; only the fields the network constructor reads matter.
SEARAFT_S_CONFIG = {
    "pretrain": "resnet18",
    "initial_dim": 64,
    "block_dims": [64, 128, 256],
    "radius": 4,
    "dim": 128,
    "num_blocks": 2,
    "iters": 4,
    "use_var": True,
    "var_min": 0,
    "var_max": 10,
}

MODEL_NAME = "searaft_s_gray.onnx"
MODEL_NAME_FP16 = "searaft_s_gray_fp16.onnx"

# Size the graph is traced at.  Deliberately not one of the verification
# sizes, so a spatial constant baked in by the trace shows up as a failure.
TRACE_H, TRACE_W = 128, 256

REPO_ROOT = Path(__file__).resolve().parent.parent


def say(msg: str) -> None:
    """Print ASCII-safe text; a Windows console on a legacy code page must
    never be handed a byte it cannot show."""
    print(msg.encode("ascii", "replace").decode("ascii"), flush=True)


def fail(msg: str) -> "NoReturn":  # type: ignore[name-defined]
    say(f"error: {msg}")
    sys.exit(1)


# ---------------------------------------------------------------------------
#  Downloads
# ---------------------------------------------------------------------------
def download(url: str, dest: Path) -> None:
    """Fetch `url` to `dest` atomically (temp file + rename)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    say(f"  downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "openosv-fetch/1"})
    with urllib.request.urlopen(req, timeout=120) as resp, open(tmp, "wb") as out:
        shutil.copyfileobj(resp, out, length=1 << 20)
    tmp.replace(dest)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch_sources(cache: Path) -> Path:
    """Return the path of the pinned SEA-RAFT `core/` directory."""
    src = cache / f"SEA-RAFT-{SEARAFT_COMMIT}"
    if not (src / "core" / "raft.py").is_file():
        zpath = cache / f"SEA-RAFT-{SEARAFT_COMMIT}.zip"
        if not zpath.is_file():
            download(SEARAFT_ZIP_URL, zpath)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(cache)
    if not (src / "core" / "raft.py").is_file():
        fail(f"SEA-RAFT sources not found under {src}")
    lic = (src / "LICENSE").read_text(encoding="utf-8", errors="replace")
    if "BSD 3-Clause" not in lic:
        fail("upstream LICENSE is no longer BSD 3-Clause; re-check before shipping")
    return src / "core"


def fetch_weights(cache: Path) -> Path:
    w = cache / f"searaft-s-{WEIGHTS_REV[:12]}.safetensors"
    if not w.is_file() or sha256_of(w) != WEIGHTS_SHA256:
        download(WEIGHTS_URL, w)
    digest = sha256_of(w)
    if digest != WEIGHTS_SHA256:
        fail(f"weights SHA-256 mismatch: got {digest}, expected {WEIGHTS_SHA256}")
    return w


# ---------------------------------------------------------------------------
#  Model construction
# ---------------------------------------------------------------------------
def import_searaft(core_dir: Path):
    """Import upstream modules with the three export fixes applied."""
    # raft.py imports huggingface_hub only for a model-hub mixin, and
    # utils.py imports scipy only for a training helper; neither is used on
    # the inference path, so stand-ins keep the dependency list short.
    try:
        import huggingface_hub  # noqa: F401
    except ImportError:
        hub = types.ModuleType("huggingface_hub")

        class PyTorchModelHubMixin:  # noqa: D401 - stand-in
            def __init_subclass__(cls, **kwargs):
                super().__init_subclass__()

        hub.PyTorchModelHubMixin = PyTorchModelHubMixin
        sys.modules["huggingface_hub"] = hub
    try:
        import scipy  # noqa: F401
    except ImportError:
        scipy_stub = types.ModuleType("scipy")
        scipy_stub.interpolate = types.ModuleType("scipy.interpolate")
        sys.modules["scipy"] = scipy_stub
        sys.modules["scipy.interpolate"] = scipy_stub.interpolate

    sys.path.insert(0, str(core_dir))
    import corr as corr_mod  # type: ignore
    import extractor as extractor_mod  # type: ignore
    import raft as raft_mod  # type: ignore
    import torch

    # Fix 2: constant correlation scale.
    def corr_const(fmap1, fmap2, num_head):
        batch, dim, h1, w1 = fmap1.shape
        h2, w2 = fmap2.shape[2:]
        fmap1 = fmap1.view(batch, num_head, dim // num_head, h1 * w1)
        fmap2 = fmap2.view(batch, num_head, dim // num_head, h2 * w2)
        c = fmap1.transpose(2, 3) @ fmap2
        c = c.reshape(batch, num_head, h1, w1, h2, w2).permute(0, 2, 3, 1, 4, 5)
        return c / (float(dim) ** 0.5)

    corr_mod.CorrBlock.corr = staticmethod(corr_const)

    # Fix 3: no torchvision download.  Kaiming init is irrelevant because
    # every parameter is overwritten by the checkpoint.
    extractor_mod.ResNetFPN._init_weights = lambda self, args: None

    return raft_mod, corr_mod, torch


def load(raft_mod, torch, weights: Path):
    # Checked for in main(); the upstream weights are published only as
    # safetensors, which (unlike a pickle) cannot execute code on load.
    from safetensors.torch import load_file

    args = types.SimpleNamespace(**SEARAFT_S_CONFIG)
    model = raft_mod.RAFT(args)
    sd = load_file(str(weights))
    missing, unexpected = model.load_state_dict(sd, strict=False)
    if unexpected:
        fail(f"checkpoint has keys the network does not: {unexpected[:5]}")
    # Every missing key must be the downsample[1] alias of a loaded bn3.
    for k in missing:
        alias = k.replace("downsample.1.", "bn3.")
        if "downsample.1." not in k or alias not in sd:
            fail(f"checkpoint is genuinely missing {k}")
    # Prove the alias really is shared: same storage, not a copy.
    for name, mod in model.named_modules():
        if hasattr(mod, "bn3") and getattr(mod, "downsample", None) is not None:
            if mod.downsample[1] is not mod.bn3:
                fail(f"{name}.downsample[1] is not the bn3 module; alias assumption broken")
    model.eval()
    say(f"  weights loaded ({len(sd)} tensors, {len(missing)} alias keys resolved)")
    return model


def make_wrapper(model, corr_mod, raft_mod, torch):
    """Inference-only forward: gray [0,1] in, final flow out.  Mirrors
    RAFT.forward(test_mode=True) line for line minus the padder (fix 1)."""
    import torch.nn as nn

    coords_grid = sys.modules["utils.utils"].coords_grid

    class SeaRaftGray(nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, a, b):
            m = self.m
            # [0,1] gray -> SEA-RAFT's [-1,1] RGB.  The *255 then /255 pair
            # of the upstream normalisation cancels; written out so the
            # correspondence with raft.py stays obvious.
            image1 = (2.0 * a - 1.0).expand(-1, 3, -1, -1).contiguous()
            image2 = (2.0 * b - 1.0).expand(-1, 3, -1, -1).contiguous()
            N, _, H, W = image1.shape
            dilation = torch.ones(N, 1, H // 8, W // 8, device=image1.device)
            cnet = m.cnet(torch.cat([image1, image2], dim=1))
            cnet = m.init_conv(cnet)
            net, context = torch.split(cnet, [m.args.dim, m.args.dim], dim=1)
            flow_update = m.flow_head(net)
            flow_8x = flow_update[:, :2]
            fmap1_8x = m.fnet(image1)
            fmap2_8x = m.fnet(image2)
            corr_fn = corr_mod.CorrBlock(fmap1_8x, fmap2_8x, m.args)
            for _ in range(m.args.iters):
                n8, _, h8, w8 = flow_8x.shape
                coords2 = coords_grid(n8, h8, w8, device=image1.device) + flow_8x
                corr = corr_fn(coords2, dilation=dilation)
                net = m.update_block(net, context, corr, flow_8x)
                flow_update = m.flow_head(net)
                flow_8x = flow_8x + flow_update[:, :2]
            weight_update = 0.25 * m.upsample_weight(net)
            info_8x = flow_update[:, 2:]
            flow_up, _ = m.upsample_data(flow_8x, info_8x, weight_update)
            return flow_up

    return SeaRaftGray(model).eval()


# ---------------------------------------------------------------------------
#  Export and verification
# ---------------------------------------------------------------------------
def export(wrapper, torch, out: Path, opset: int) -> None:
    import inspect

    x1 = torch.rand(1, 1, TRACE_H, TRACE_W)
    x2 = torch.rand(1, 1, TRACE_H, TRACE_W)
    kwargs = dict(
        input_names=["image_a", "image_b"],
        output_names=["flow"],
        opset_version=opset,
        dynamic_axes={
            "image_a": {0: "N", 2: "H", 3: "W"},
            "image_b": {0: "N", 2: "H", 3: "W"},
            "flow": {0: "N", 2: "H", 3: "W"},
        },
        do_constant_folding=True,
    )
    # The TorchScript exporter is the one this graph was verified with; newer
    # torch defaults to the dynamo exporter, so ask for the old one by name.
    if "dynamo" in inspect.signature(torch.onnx.export).parameters:
        kwargs["dynamo"] = False
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp.onnx")
    with torch.no_grad():
        torch.onnx.export(wrapper, (x1, x2), str(tmp), **kwargs)
    tmp.replace(out)
    say(f"  wrote {out} ({out.stat().st_size / 1e6:.1f} MB)")


def textured(h, w, seed):
    """Band-limited random texture: constrains flow everywhere, and smooth
    enough that the shift is well defined."""
    import numpy as np

    rng = np.random.default_rng(seed)
    img = rng.random((h, w)).astype(np.float32)
    for _ in range(3):
        img = (img + np.roll(img, 1, 0) + np.roll(img, -1, 0)) / 3.0
        img = (img + np.roll(img, 1, 1) + np.roll(img, -1, 1)) / 3.0
    img -= img.min()
    img /= max(float(img.max()), 1e-6)
    return img.astype(np.float32)


def verify(path: Path) -> bool:
    """Run the exported file on known translations at three sizes other than
    the trace size.  A wrong normalisation, channel order or sign still RUNS
    and returns plausible garbage; only a ground-truth check catches it."""
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        say("  onnxruntime not installed: verification skipped (pip install onnxruntime)")
        return True
    sess = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    ok = True
    pad = 32
    for (h, w, dx, dy) in [(128, 512, 3, 1), (192, 1024, -5, 2), (256, 768, 9, -3)]:
        big = textured(h + 2 * pad, w + 2 * pad, seed=h + w)
        # Non-wrapping crops: content leaving the frame is really gone, as in
        # a real scene, rather than wrapping round the opposite edge.
        a = np.ascontiguousarray(big[pad:pad + h, pad:pad + w])
        b = np.ascontiguousarray(big[pad - dy:pad - dy + h, pad - dx:pad - dx + w])
        flow = sess.run(["flow"], {"image_a": a[None, None], "image_b": b[None, None]})[0][0]
        if flow.shape != (2, h, w) or not np.isfinite(flow).all():
            say(f"  FAIL {w}x{h}: output shape {flow.shape} or non-finite values")
            ok = False
            continue
        m = 16
        err = np.hypot(flow[0, m:h - m, m:w - m] - dx, flow[1, m:h - m, m:w - m] - dy)
        good = err.mean() < 0.5
        ok &= bool(good)
        say(f"  {'ok  ' if good else 'FAIL'} {w:4d}x{h:3d} truth=({dx:+d},{dy:+d}) "
            f"median=({np.median(flow[0]):+.3f},{np.median(flow[1]):+.3f}) "
            f"EPE mean={err.mean():.3f} p95={np.percentile(err, 95):.3f} px")
    return ok


def make_fp16(src: Path, dst: Path) -> None:
    """fp16 variant.  convert_float_to_float16 leaves shape-derived
    Cast(to=FLOAT) nodes claiming fp32 while their consumers were retyped to
    fp16, and ORT rejects the model; those Casts are retyped here.

    MEASURED ON AN RTX 5090: this file is ~20x SLOWER than fp32 and less
    accurate, because the casts it adds fragment the graph -
      ORT 1.23 (Python), one 256x2048 tile: 426 ms vs 18.5 ms, EPE 0.28 vs 0.17
      ORT 1.30 (C++ backend), 2048x68 band: 1645 ms vs 81 ms, EPE 0.08 vs 0.02
    It is written only for experiments; the backend loads the fp32 file."""
    import onnx
    from onnx import TensorProto

    try:
        from onnxconverter_common import float16
    except ImportError:
        fail("--fp16 needs onnxconverter-common (pip install onnxconverter-common)")
    m16 = float16.convert_float_to_float16(onnx.load(str(src)), keep_io_types=True)
    types_by_name = {vi.name: vi.type.tensor_type.elem_type
                     for vi in list(m16.graph.value_info) + list(m16.graph.output)
                     if vi.type.HasField("tensor_type")}
    fixed = 0
    for node in m16.graph.node:
        if node.op_type != "Cast":
            continue
        for attr in node.attribute:
            if attr.name == "to" and attr.i == TensorProto.FLOAT and \
                    types_by_name.get(node.output[0]) == TensorProto.FLOAT16:
                attr.i = TensorProto.FLOAT16
                fixed += 1
    onnx.checker.check_model(m16)
    onnx.save(m16, str(dst))
    say(f"  wrote {dst} ({dst.stat().st_size / 1e6:.1f} MB, {fixed} casts retyped)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out-dir", type=Path, default=REPO_ROOT / "models",
                    help="where the .onnx goes (default: <repo>/models)")
    ap.add_argument("--cache-dir", type=Path, default=REPO_ROOT / "third_party" / "sea-raft",
                    help="download cache for sources and weights (gitignored)")
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--fp16", action="store_true", help="also write the (slower) fp16 variant")
    ap.add_argument("--skip-verify", action="store_true")
    a = ap.parse_args()

    try:
        import torch  # noqa: F401
        import onnx  # noqa: F401
        import safetensors  # noqa: F401
    except ImportError as exc:
        fail(f"{exc.name} is required: pip install torch onnx safetensors numpy onnxruntime")

    say("[1/4] sources")
    core = fetch_sources(a.cache_dir)
    say("[2/4] weights")
    weights = fetch_weights(a.cache_dir)
    say("[3/4] export")
    raft_mod, corr_mod, torch = import_searaft(core)
    model = load(raft_mod, torch, weights)
    wrapper = make_wrapper(model, corr_mod, raft_mod, torch)
    out = a.out_dir / MODEL_NAME
    export(wrapper, torch, out, a.opset)
    say(f"  sha256 {sha256_of(out)}")
    if a.fp16:
        make_fp16(out, a.out_dir / MODEL_NAME_FP16)
    say("[4/4] verify (known translations, sizes other than the trace size)")
    if a.skip_verify:
        say("  skipped by request")
        return 0
    if not verify(out):
        out.unlink(missing_ok=True)
        fail("the exported model does not recover a known translation; it was deleted")
    say("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
