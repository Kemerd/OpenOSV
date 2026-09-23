"""Question 2: newer optical-flow models on the lens-overlap band, scored exactly
like NEURAL_STITCHING.md section 1.6 (research only).

Scoring (identical to `osvtool seam --region`, CmdSeam.cpp scoreRegion):
  global NCC of the two lenses' code-space luma over co-visible pixels
  (both alphas > 0.5), rows |lat| <= 4 deg of the 2048-column polar band,
  in the sky (410-900), ground (1110-1700) and wing (1880-2040) columns,
  after warping both lenses half-way with the model's flow.  Lens bands come
  from `osvtool seam --dump-bands` (dump_flow_bands.ps1), so the pixels being
  scored are the ones the production DIS path scores.

Wing extras: the two lenses see different parts of the nacelle, so NCC is
bounded by occlusion.  "edge NCC" (NCC of Sobel gradient magnitude) and the
edge chamfer distance (mean px at 2048 scale, capped at 8, from lens 0's
strongest 5 % of gradients to lens 1's) say whether the VISIBLE edges line up.

Warps are symmetric (lens 0 at x - f/2, lens 1 at x + f/2), as the kernel
applies the parallax grid; see score().

Flow inputs:
  luma2048 : the same code-space luma production DIS sees, 2048 x +-10 deg
  rgb4096  : bandprobe per-lens scene-linear RGB at 4096 columns x +-10 deg,
             one global per-channel gain match, sRGB encoded (the models are
             trained on 8-bit display RGB); flows are resampled to the 2048 grid.
Every model runs on overlapping 2H x H tiles (H = band rows) with circular
longitude wrap: that is the aspect the depth branch of FlowSeek / WAFT is
trained with (it squashes each input to 518 x 518) and it keeps every model
inside its training resolution range.

    python flow_eval.py [--models raft:things,...] [--input rgb4096] [--frames 0,32,64]
"""
import argparse
import json
import os
import time
import warnings

import cv2
import numpy as np

from common import (OUT, REGIONS_2048, ProbeBands, bilinear_sample, load_dump, ncc,
                    region_cols, srgb_encode, utf8_console)

warnings.filterwarnings("ignore")
utf8_console()
os.environ.setdefault("TORCH_HOME", os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "torch"))

FLOWBANDS = os.path.join(OUT, "flowbands")
BANDS45 = os.path.join(OUT, "bands45")
SCORE_HALF_DEG = 4.0
INPUT_HALF_DEG = 10.0

DEFAULT_MODELS = [
    "dis_medium", "dis_fine",
    "raft:things", "gma:things", "flowformer:things", "flowformer_pp:things", "unimatch:things",
    "sea_raft_s:spring", "sea_raft_s:things", "sea_raft_m:spring",
    "flowseek_t:tar-c-t", "flowseek_t:tar-c-t-tskh", "flowseek_t:tar-c-t-tskh@iters=12",
    "flowseek_m:tar-c-t-tskh", "flowseek_m:tar-c-t-tskh@iters=12",
    "waft_dav2_a1:tar-c-t", "waft_twins_a2:zero_shot", "waft_dav2_a2:zero_shot",
    "dpflow:things", "neuflow2:things",
]


# --------------------------------------------------------------------------
#  Data
# --------------------------------------------------------------------------
class Scoring:
    """The 2048-column code-luma band production scores on (wide dump)."""

    def __init__(self, frame):
        prefix = os.path.join(FLOWBANDS, f"f{frame}_b10")
        l0, l1, a0, a1, info = load_dump(prefix, "none", os.path.join(FLOWBANDS, f"dump_f{frame}_b10.json"))
        self.l0, self.l1, self.a0, self.a1 = l0, l1, a0, a1
        self.w, self.h = l0.shape[1], l0.shape[0]
        self.row_offset = int(info["rowOffset"])
        self.map_h = self.w // 2
        rows = np.arange(self.h) + self.row_offset + 0.5
        self.lat = 90.0 - rows * 180.0 / self.map_h
        self.score_rows = np.abs(self.lat) <= SCORE_HALF_DEG

    def masks_from(self, base):
        out = {}
        for name, (c0, c1) in REGIONS_2048.items():
            m = np.zeros_like(base)
            cols = region_cols(c0, c1, self.w)
            m[:, cols] = base[:, cols]
            out[name] = m
        out["band"] = base
        return out

    def score(self, flow):
        """NCC per region after a symmetric warp: lens 0 sampled at x - flow/2,
        lens 1 at x + flow/2 (flow in 2048 px, lens 0 -> lens 1).  That is the
        production scheme (the grid moves the master by +g and the slave by
        -g) and it gives both lenses the same resampling blur, so a flow is
        not penalised merely for interpolating one lens and not the other."""
        half = 0.5 * flow
        return self.score_halves(-half, half, flow)

    def score_halves(self, h0, h1, flow=None):
        """Lens 0 sampled at x + h0, lens 1 at x + h1 (each lens its own shift,
        e.g. each reprojected with its own depth); flow = h1 - h0 by default."""
        if flow is None:
            flow = h1 - h0
        l0w, v0 = warp(self.l0, h0)
        a0w, _ = warp(self.a0, h0)
        l1w, v1 = warp(self.l1, h1)
        a1w, _ = warp(self.a1, h1)
        base = (a0w > 0.5) & (a1w > 0.5) & v0 & v1 & self.score_rows[:, None]
        ms = self.masks_from(base)
        res = {k: ncc(l0w, l1w, m) for k, m in ms.items()}
        res.update(edge_metrics(l0w, l1w, ms["wing"]))
        # Flow size per region, degrees.  In the sky the right answer is ~0
        # (far field; the calibration residual is ~0.09 deg): a large sky flow
        # is a warp that "fixes" photometry with geometry and distorts the sky.
        mag = np.hypot(flow[..., 0], flow[..., 1]) * 360.0 / self.w
        for k in ("sky", "ground", "wing"):
            res[f"flowDeg_{k}"] = float(np.median(mag[ms[k]])) if ms[k].any() else float("nan")
        return res


def warp(img, flow):
    h, w = flow.shape[:2]
    xs, ys = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return bilinear_sample(img, xs + flow[..., 0], ys + flow[..., 1])


def edge_metrics(l0, l1w, mask):
    """Visible-edge agreement in the wing columns (2048-scale pixels)."""
    def grad(x):
        gx = cv2.Sobel(x.astype(np.float32), cv2.CV_32F, 1, 0, ksize=3)
        gy = cv2.Sobel(x.astype(np.float32), cv2.CV_32F, 0, 1, ksize=3)
        return np.hypot(gx, gy)

    g0, g1 = grad(l0), grad(l1w)
    out = {"wingEdgeNcc": ncc(g0, g1, mask)}
    if mask.sum() < 50:
        out["wingChamferPx"] = float("nan")
        out["wingEdgeWithin1px"] = float("nan")
        return out
    # Strong edges only (top 5 % gradient in the wing columns of each lens):
    # the nacelle outline and the wing edge, not the ground texture.
    e0 = (g0 >= np.percentile(g0[mask], 95)) & mask
    e1 = (g1 >= np.percentile(g1[mask], 95)) & mask
    dt1 = cv2.distanceTransform((~e1).astype(np.uint8), cv2.DIST_L2, 3)
    d = np.minimum(dt1[e0], 8.0)
    out["wingChamferPx"] = float(np.mean(d))
    out["wingEdgeWithin1px"] = float(np.mean(d <= 1.0))
    return out


def rgb4096_inputs(frame, sc):
    """Per-lens display RGB at 4096 x +-10 deg, gain-matched, as uint8 BGR."""
    pb = ProbeBands(BANDS45)
    rows = pb.rows_for(INPUT_HALF_DEG)
    b0 = pb.load(frame, "lens0")[rows]
    b1 = pb.load(frame, "lens1")[rows]
    both = (b0[..., 3] > 0.5) & (b1[..., 3] > 0.5)
    gain = np.ones(3, np.float32)
    for c in range(3):
        r = np.log2(np.maximum(b0[..., c][both], 1e-6)) - np.log2(np.maximum(b1[..., c][both], 1e-6))
        gain[c] = 2.0 ** (0.5 * float(np.median(r)))
    enc = [srgb_encode(b0[..., :3] / gain[None, None, :], 2.0), srgb_encode(b1[..., :3] * gain[None, None, :], 2.0)]
    imgs = fill_outside([enc[0], enc[1]], [b0[..., 3], b1[..., 3]])
    imgs = [(x[..., ::-1] * 255 + 0.5).astype(np.uint8).copy() for x in imgs]
    lat = pb.lat_deg()[rows]
    return imgs, lat, pb.w


def fill_outside(imgs, alphas):
    """Where one lens sees nothing (past its FOV or inside its occlusion
    polygon) show the other lens there instead of black.  A black FOV edge is
    the strongest structure in the band and every model happily matches lens
    0's edge (+7.6 deg) to lens 1's (-7.6 deg); with the fill that region is
    identical in both inputs, i.e. carries no motion at all."""
    out = []
    for i in (0, 1):
        j = 1 - i
        own = alphas[i] > 0.5
        other = alphas[j] > 0.5
        x = imgs[i].copy()
        take = ~own & other
        x[take] = imgs[j][take]
        x[~own & ~other] = 0
        out.append(x)
    return out


def luma2048_inputs(sc):
    ls = fill_outside([np.clip(sc.l0, 0, 1), np.clip(sc.l1, 0, 1)], [sc.a0, sc.a1])
    imgs = []
    for l in ls:
        u8 = (l * 255 + 0.5).astype(np.uint8)
        imgs.append(np.repeat(u8[..., None], 3, axis=2).astype(np.uint8).copy())
    return imgs, sc.lat, sc.w


def to_scoring_grid(flow_in, lat_in, w_in, sc):
    """Resample a flow computed on (lat_in rows, w_in columns) to the scoring grid."""
    if w_in == sc.w and len(lat_in) == sc.h and np.allclose(lat_in, sc.lat):
        return flow_in
    s = w_in / sc.w                                   # input px per scoring px
    xs = (np.arange(sc.w) + 0.5) * s - 0.5
    dlat = lat_in[0] - lat_in[1]                      # deg per input row
    ys = (lat_in[0] - sc.lat) / dlat
    X, Y = np.meshgrid(xs, ys)
    f, _ = bilinear_sample(flow_in, X.astype(np.float32), Y.astype(np.float32))
    return (f / s).astype(np.float32)


def production_grid_flow(frame, sc, decay_rows=8):
    """The production parallax grid (osvtool seam --dump-bands *_grid.f32) as a
    lens0->lens1 flow on the scoring grid: the grid stores HALF the disparity
    as the master's (dLon, dLat) in radians, the slave takes the negation
    (ParallaxWarp.h, osv_kernel.h osvWarpSample)."""
    with open(os.path.join(FLOWBANDS, f"dump_f{frame}_b6.json"), "r", encoding="utf-8-sig") as f:
        txt = f.read()
    pj = json.loads(txt[txt.index("{"):])["parallax"]
    gw, gh = int(pj["gridW"]), int(pj["gridH"])
    info = pj["dump"]
    uv = np.fromfile(os.path.join(FLOWBANDS, f"f{frame}_b6_grid.f32"), dtype=np.float32).reshape(gh, gw, 2)
    map_h = int(info["w"]) // 2
    rad_per_row = np.pi / map_h
    lat_top = np.pi / 2 - (int(info["rowOffset"]) + 0.5) * rad_per_row
    lat_bot = np.pi / 2 - (int(info["rowOffset"]) + int(info["h"]) - 1 + 0.5) * rad_per_row
    rows_measured = gh - 2 * decay_rows
    per = (lat_top - lat_bot) / (rows_measured - 1)
    lat_min = lat_top + per * decay_rows          # grid row 0
    lat_max = lat_bot - per * decay_rows          # grid row gh-1
    lon = -np.pi + (np.arange(sc.w) + 0.5) * 2 * np.pi / sc.w
    lat = np.radians(sc.lat)
    fx = (lon + np.pi) / (2 * np.pi) * gw - 0.5 * 0   # kernel: plain fraction of the ring
    fy = (lat - lat_min) / (lat_max - lat_min) * (gh - 1)
    X, Y = np.meshgrid(fx, fy)
    g, valid = bilinear_sample(uv, X.astype(np.float32), Y.astype(np.float32))
    g[~valid] = 0.0
    flow = np.zeros((sc.h, sc.w, 2), np.float32)
    flow[..., 0] = 2.0 * g[..., 0] * sc.w / (2 * np.pi)
    flow[..., 1] = -2.0 * g[..., 1] * map_h / np.pi
    return flow


def score_production(frame):
    """The same metrics on the bands the kernel rendered WITH the production
    parallax grid (osvtool seam --dump-bands, tag 'parallax', +-6 deg band)."""
    prefix = os.path.join(FLOWBANDS, f"f{frame}_b6")
    l0, l1, a0, a1, info = load_dump(prefix, "parallax", os.path.join(FLOWBANDS, f"dump_f{frame}_b6.json"))
    w, h = l0.shape[1], l0.shape[0]
    rows = np.arange(h) + int(info["rowOffset"]) + 0.5
    lat = 90.0 - rows * 180.0 / (w // 2)
    base = (a0 > 0.5) & (a1 > 0.5) & (np.abs(lat) <= SCORE_HALF_DEG)[:, None]
    res = {}
    for name, (c0, c1) in REGIONS_2048.items():
        m = np.zeros_like(base)
        cols = region_cols(c0, c1, w)
        m[:, cols] = base[:, cols]
        res[name] = ncc(l0, l1, m)
        if name == "wing":
            res.update(edge_metrics(l0, l1, m))
    res["band"] = ncc(l0, l1, base)
    return res


# --------------------------------------------------------------------------
#  Flow runners
# --------------------------------------------------------------------------
def run_dis(imgs, preset):
    g0 = cv2.cvtColor(imgs[0], cv2.COLOR_BGR2GRAY)
    g1 = cv2.cvtColor(imgs[1], cv2.COLOR_BGR2GRAY)
    pad = 64
    g0p = np.concatenate([g0[:, -pad:], g0, g0[:, :pad]], 1)
    g1p = np.concatenate([g1[:, -pad:], g1, g1[:, :pad]], 1)
    if preset == "medium":
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    else:
        # patch 8, stride 5 (the DIS settings DJI's stitcher uses), finest scale
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_ULTRAFAST)
        dis.setPatchSize(8)
        dis.setPatchStride(5)
        dis.setFinestScale(0)
        dis.setGradientDescentIterations(25)
        dis.setVariationalRefinementIterations(5)
        dis.setUseMeanNormalization(True)
        dis.setUseSpatialPropagation(True)
    t0 = time.perf_counter()
    f = dis.calc(g0p, g1p, None)
    ms = (time.perf_counter() - t0) * 1000
    return f[:, pad:-pad].copy(), ms


def _patch_flowseek_bases():
    """ptlflow 0.4.2's FlowSeek.create_bases builds its u/v grid on the CPU
    (torch.linspace without a device) and fails on CUDA; rebuild it on the
    disparity's device.  Maths identical to the authors' create_bases."""
    import torch
    from ptlflow.models.flowseek import flowseek as fsmod

    def create_bases(self, disp):
        B, C, H, W = disp.shape
        dev, dt = disp.device, disp.dtype
        ys = torch.linspace(0.5 / H, 1.0 - 0.5 / H, H, dtype=dt, device=dev)
        xs = torch.linspace(0.5 / W, 1.0 - 0.5 / W, W, dtype=dt, device=dev)
        u, v = torch.meshgrid(xs, ys, indexing="xy")
        u = (u - 0.5)[None, None].repeat(B, 1, 1, 1)
        v = (v - 0.5)[None, None].repeat(B, 1, 1, 1)
        ar = W / H
        z, o = torch.zeros_like(disp), torch.ones_like(disp)

        def nrm(t):
            return t / torch.linalg.vector_norm(t, dim=(1, 2, 3), keepdim=True)

        Tx = 2 * disp * nrm(torch.cat([-o, z], 1))
        Ty = 2 * disp * nrm(torch.cat([z, -o], 1))
        Tz = 2 * disp * nrm(torch.cat([u, v], 1))
        R1x = nrm(torch.cat([z, o], 1))
        R2x = nrm(torch.cat([u * v, v * v], 1))
        R1y = nrm(torch.cat([-o, z], 1))
        R2y = nrm(torch.cat([-u * u, -u * v], 1))
        Rz = nrm(torch.cat([-v / ar, u * ar], 1))
        return torch.cat([Tx, Ty, Tz, R1x, R2x, R1y, R2y, Rz], dim=1)

    for name in dir(fsmod):
        cls = getattr(fsmod, name)
        if isinstance(cls, type) and hasattr(cls, "create_bases"):
            cls.create_bases = create_bases


class TorchFlow:
    def __init__(self, spec):
        import torch
        import ptlflow
        # "name:ckpt[@iters=N][@scale=S]" - FlowSeek (S) is the (T) weights run
        # with 12 refinement iterations, (L) is (M) with 12 (the authors'
        # eval.sh).  scale=S upsamples each tile by S before the model (for
        # models trained at 432 x 960 or larger that break on 228-row tiles);
        # the flow is resized back and its vectors divided by S.
        parts = spec.split("@")
        opts = dict(p.split("=") for p in parts[1:])
        iters = int(opts["iters"]) if "iters" in opts else None
        self.scale = float(opts.get("scale", 1.0))
        name, ckpt = parts[0].split(":")
        self.torch = torch
        if name.startswith("flowseek"):
            _patch_flowseek_bases()
        self.model = ptlflow.get_model(name, ckpt_path=ckpt).cuda().eval()
        if iters is not None:
            if not hasattr(self.model, "iters"):
                raise ValueError(f"{name} has no iters setting")
            self.model.iters = iters
        self.params = sum(p.numel() for p in self.model.parameters())

    def tiles(self, imgs, tile_w, overlap):
        """Run on overlapping tiles with circular wrap; returns flow (h, w, 2) and ms."""
        from ptlflow.utils.io_adapter import IOAdapter
        torch = self.torch
        h, w = imgs[0].shape[:2]
        step = tile_w - overlap
        n = int(np.ceil(w / step))
        acc = np.zeros((h, w, 2), np.float64)
        wsum = np.zeros((h, w), np.float64)
        ramp = np.minimum(np.arange(tile_w) + 0.5, tile_w - np.arange(tile_w) - 0.5)
        ramp = np.clip(ramp / max(overlap / 2, 1), 0.02, 1.0)
        target = None
        if self.scale != 1.0:
            target = (int(round(h * self.scale / 32)) * 32, int(round(tile_w * self.scale / 32)) * 32)
        adapter = IOAdapter(self.model, (h, tile_w), target_size=target, cuda=True)
        batch = []
        cols_list = []
        for i in range(n):
            cols = (np.arange(tile_w) + i * step) % w
            cols_list.append(cols)
            batch.append((imgs[0][:, cols], imgs[1][:, cols]))
        times = []
        flows = []
        for rep in range(3):                              # rep 0 = warm-up
            torch.cuda.synchronize()
            t0 = time.perf_counter()
            flows = []
            with torch.no_grad():
                for a, b in batch:
                    inputs = adapter.prepare_inputs([a, b])
                    pred = self.model(inputs)
                    pred = adapter.unscale(pred)
                    flows.append(pred["flows"][0, 0].permute(1, 2, 0))
            torch.cuda.synchronize()
            times.append((time.perf_counter() - t0) * 1000)
        for cols, f in zip(cols_list, flows):
            f = f.float().cpu().numpy()
            acc[:, cols] += f * ramp[None, :, None]
            wsum[:, cols] += ramp[None, :]
        return (acc / wsum[..., None]).astype(np.float32), float(np.median(times[1:]))


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default=",".join(DEFAULT_MODELS))
    ap.add_argument("--input", default="rgb4096", choices=["rgb4096", "luma2048"])
    ap.add_argument("--frames", default="0,32,64")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    frames = [int(x) for x in args.frames.split(",")]
    out_path = args.out or os.path.join(OUT, f"flow_eval_{args.input}.json")
    results = {}
    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as f:
            results = json.load(f)

    data = {}
    for fr in frames:
        sc = Scoring(fr)
        imgs, lat, w_in = rgb4096_inputs(fr, sc) if args.input == "rgb4096" else luma2048_inputs(sc)
        data[fr] = (sc, imgs, lat, w_in)
        results.setdefault("identity", {})[str(fr)] = sc.score(np.zeros((sc.h, sc.w, 2), np.float32))
        results.setdefault("production DIS grid (kernel render)", {})[str(fr)] = score_production(fr)
        results.setdefault("production DIS grid (this warp)", {})[str(fr)] = sc.score(production_grid_flow(fr, sc))
        for k in ("production DIS grid (kernel render)", "production DIS grid (this warp)"):
            r = results[k][str(fr)]
            print(f"  {k} f{fr}: sky {r['sky']:.4f} ground {r['ground']:.4f} wing {r['wing']:.4f}"
                  f" band {r['band']:.4f} | wing edgeNCC {r['wingEdgeNcc']:.3f} chamfer {r['wingChamferPx']:.2f}px")

    for spec in args.models.split(","):
        spec = spec.strip()
        if not spec:
            continue
        print(f"== {spec}", flush=True)
        runner = None
        try:
            if not spec.startswith("dis_"):
                runner = TorchFlow(spec)
            for fr in frames:
                sc, imgs, lat, w_in = data[fr]
                if spec.startswith("dis_"):
                    f_in, ms = run_dis(imgs, spec[4:])
                else:
                    hh = imgs[0].shape[0]
                    f_in, ms = runner.tiles(imgs, 2 * hh, hh // 2)
                flow = to_scoring_grid(f_in, lat, w_in, sc)
                r = sc.score(flow)
                r["ms"] = ms
                r["inputW"] = int(w_in)
                r["inputH"] = int(imgs[0].shape[0])
                if runner is not None:
                    r["params"] = int(runner.params)
                results.setdefault(spec, {})[str(fr)] = r
                print(f"  f{fr}: sky {r['sky']:.4f} ground {r['ground']:.4f} wing {r['wing']:.4f} band {r['band']:.4f}"
                      f" | wing edgeNCC {r['wingEdgeNcc']:.3f} chamfer {r['wingChamferPx']:.2f}px"
                      f" | flow sky {r['flowDeg_sky']:.3f} gnd {r['flowDeg_ground']:.3f} deg | {ms:.1f} ms",
                      flush=True)
        except Exception as e:                                      # keep going on a broken model
            print(f"  FAILED: {type(e).__name__}: {e}", flush=True)
            results.setdefault(spec, {})["error"] = f"{type(e).__name__}: {e}"
        finally:
            del runner
            try:
                import torch
                torch.cuda.empty_cache()
            except ImportError:
                pass
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
    print("identity:", {k: {kk: round(vv, 4) for kk, vv in v.items() if isinstance(vv, float)}
                        for k, v in results["identity"].items()})


if __name__ == "__main__":
    main()
