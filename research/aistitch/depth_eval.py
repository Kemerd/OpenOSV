"""Question 3: depth-aware stitching on the sample clip (research only).

Model (derived in docs/research/AI_STITCHING.md section 3.1).  If the two lens
centres sit a distance b apart along the lens axes (body Y, the polar axis of
the band), a point at distance D in band direction (lon, lat) is seen by the
two lenses with a latitude difference of (b / D) cos(lat) and NO longitude
difference.  So the lens0 -> lens1 flow over the overlap band is

    f_x = r_x(lon, lat)                         (calibration residual only)
    f_y = r_y(lon, lat) + K * cos(lat) / D      (K = +-b * mapH / pi, px*m)

where r is the flow of a small relative rotation w between the lenses
(r = (w x d) projected on the lon / lat axes: three numbers per clip).

Steps per frame:
  1. per-lens perspective crops (90 deg, 518 px, every 30 deg of longitude,
     centred on the seam) from the bandprobe per-lens bands, each lens's
     blind area filled from the other lens;
  2. Depth Anything V2 Small (relative inverse depth, Apache-2.0) and two
     metric models - MoGe-2 ViT-S (MIT) and DA-V2 Metric-Outdoor Small - on
     every crop, resampled to the 2048 x +-10 deg scoring band;
  3. a reference flow (SEA-RAFT-S Spring, forward + backward, and the
     production DIS grid) -> the rotation w from confident far-field pixels;
  4. parallax p = f_y - r_y on confident wing pixels, against depth:
     the relative model gives K per crop, the metric models give the
     baseline b = |p| pi / mapH * D / cos(lat);
  5. warps scored with flow_eval.Scoring: rotation only, the best single
     global offset, the best near/far offset pair, depth-aware (relative,
     fitted K; metric, fixed b), each lens reprojected with its own depth.

    python depth_eval.py [--frames 0,32,64]
"""
import argparse
import json
import os
import time
import warnings

import numpy as np

warnings.filterwarnings("ignore")

from common import MODELS, OUT, REGIONS_2048, ProbeBands, region_cols, srgb_encode, utf8_console  # noqa: E402
from flow_eval import (BANDS45, INPUT_HALF_DEG, Scoring, TorchFlow, fill_outside,  # noqa: E402
                       production_grid_flow, rgb4096_inputs, to_scoring_grid, warp)
from sphere import PerspectiveCrop, basis_lonlat, dir_from_lonlat  # noqa: E402

utf8_console()
CROP_LONS = [-165 + 30 * i for i in range(12)]
CROP_FOV = 90.0
CROP_N = 518
OUTD = os.path.join(OUT, "depth")


# --------------------------------------------------------------------------
#  Depth models
# --------------------------------------------------------------------------
class DepthModels:
    def __init__(self):
        import torch
        from transformers import AutoImageProcessor, AutoModelForDepthEstimation
        self.torch = torch
        self.rel_proc = AutoImageProcessor.from_pretrained("depth-anything/Depth-Anything-V2-Small-hf",
                                                           cache_dir=os.path.join(MODELS, "hf"))
        self.rel = AutoModelForDepthEstimation.from_pretrained(
            "depth-anything/Depth-Anything-V2-Small-hf", cache_dir=os.path.join(MODELS, "hf"),
            torch_dtype=torch.float16).cuda().eval()
        self.met_proc = AutoImageProcessor.from_pretrained("depth-anything/Depth-Anything-V2-Metric-Outdoor-Small-hf",
                                                           cache_dir=os.path.join(MODELS, "hf"))
        self.met = AutoModelForDepthEstimation.from_pretrained(
            "depth-anything/Depth-Anything-V2-Metric-Outdoor-Small-hf", cache_dir=os.path.join(MODELS, "hf"),
            torch_dtype=torch.float16).cuda().eval()
        self.moge = None
        try:
            from moge.model.v2 import MoGeModel
            self.moge = MoGeModel.from_pretrained("Ruicheng/moge-2-vits-normal",
                                                  cache_dir=os.path.join(MODELS, "hf")).cuda().eval()
        except Exception as e:                      # optional: the package may be missing
            print("MoGe-2 unavailable:", type(e).__name__, e)
        self.times = {"dav2s_rel": [], "dav2s_metric": [], "moge2s": []}

    def _hf(self, proc, model, imgs_u8):
        torch = self.torch
        inputs = proc(images=list(imgs_u8), return_tensors="pt", do_resize=False)
        pv = inputs["pixel_values"].cuda().half()
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        with torch.no_grad():
            out = model(pixel_values=pv).predicted_depth
        torch.cuda.synchronize()
        dt = (time.perf_counter() - t0) * 1000
        out = torch.nn.functional.interpolate(out[:, None].float(), size=imgs_u8[0].shape[:2], mode="bilinear",
                                              align_corners=False)[:, 0]
        return out.cpu().numpy(), dt

    def run(self, imgs_u8):
        """imgs_u8: list of (n, n, 3) RGB uint8 crops -> dict of (k, n, n) maps."""
        rel, t = self._hf(self.rel_proc, self.rel, imgs_u8)
        self.times["dav2s_rel"].append(t / len(imgs_u8))
        met, t = self._hf(self.met_proc, self.met, imgs_u8)
        self.times["dav2s_metric"].append(t / len(imgs_u8))
        out = {"rel": rel, "metric_dav2": met}
        if self.moge is not None:
            torch = self.torch
            dists = []
            t0 = time.perf_counter()
            for im in imgs_u8:
                x = torch.from_numpy(im).cuda().float().permute(2, 0, 1) / 255.0
                with torch.no_grad():
                    r = self.moge.infer(x, fov_x=CROP_FOV)
                pts = r["points"].float()
                d = torch.linalg.norm(pts, dim=-1)
                d[~r["mask"].bool()] = float("nan")
                dists.append(d.cpu().numpy())
            torch.cuda.synchronize()
            self.times["moge2s"].append((time.perf_counter() - t0) * 1000 / len(imgs_u8))
            out["metric_moge"] = np.stack(dists)
        return out


def ray_distance_from_z(depth_z, crop):
    """Metric models that return z-depth -> distance along the ray."""
    rays = crop.rays()
    cos_off = rays @ crop.F
    return depth_z / np.maximum(cos_off, 1e-6)


# --------------------------------------------------------------------------
#  Geometry fits
# --------------------------------------------------------------------------
def rotation_design(sc):
    """Per scoring pixel, the 2x3 matrix mapping a small rotation w to (fx, fy) px."""
    lon = -np.pi + (np.arange(sc.w) + 0.5) * 2 * np.pi / sc.w
    lat = np.radians(sc.lat)
    L, A = np.meshgrid(lon, lat)
    d = dir_from_lonlat(L, A)
    e_lon, e_lat = basis_lonlat(L, A)
    ax = np.cross(d, e_lon) / np.cos(A)[..., None] * sc.w / (2 * np.pi)   # fx = w . (d x e_lon) / cos(lat)
    ay = -np.cross(d, e_lat) * sc.map_h / np.pi                           # fy = -w . (d x e_lat)
    return ax, ay, A


def fit_rotation(flow, conf, sc, cols):
    ax, ay, _ = rotation_design(sc)
    m = np.zeros(conf.shape, bool)
    m[:, cols] = conf[:, cols]
    m &= sc.score_rows[:, None]
    A = np.concatenate([ax[m], ay[m]], 0)
    b = np.concatenate([flow[..., 0][m], flow[..., 1][m]], 0)
    w, *_ = np.linalg.lstsq(A, b, rcond=None)
    # one Huber-ish reweighting pass
    res = A @ w - b
    s = 1.4826 * np.median(np.abs(res)) + 1e-6
    wt = 1.0 / np.maximum(1.0, np.abs(res) / (2 * s))
    w, *_ = np.linalg.lstsq(A * wt[:, None], b * wt, rcond=None)
    r = np.stack([ax @ w, ay @ w], -1).astype(np.float32)
    return w, r


def cols_mask(sc, cols, ramp=8):
    """Column weight: 1 in `cols`, raised-cosine ramp over `ramp` columns outside."""
    m = np.zeros(sc.w, np.float32)
    m[cols] = 1.0
    out = m.copy()
    for k in range(1, ramp + 1):
        v = 0.5 * (1 + np.cos(np.pi * k / (ramp + 1)))
        out = np.maximum(out, np.roll(m, k) * v)
        out = np.maximum(out, np.roll(m, -k) * v)
    return out


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="0,32,64")
    args = ap.parse_args()
    frames = [int(x) for x in args.frames.split(",")]
    os.makedirs(OUTD, exist_ok=True)
    pb = ProbeBands(BANDS45)
    models = DepthModels()
    flow_ref = TorchFlow("sea_raft_s:spring")
    crops = [PerspectiveCrop(lc, 0.0, CROP_FOV, CROP_N) for lc in CROP_LONS]
    results = {}

    for fr in frames:
        print(f"== frame {fr}", flush=True)
        sc = Scoring(fr)
        wcols = region_cols(*REGIONS_2048["wing"], sc.w)
        gcols = region_cols(*REGIONS_2048["ground"], sc.w)
        farcols = region_cols(1110, 1870, sc.w)

        # ---- 1. per-lens crops ---------------------------------------------
        b0 = pb.load(fr, "lens0")
        b1 = pb.load(fr, "lens1")
        filled = fill_outside([b0[..., :3], b1[..., :3]], [b0[..., 3], b1[..., 3]])
        depth_band = {}
        crop_of_col = np.argmin(np.abs(((np.degrees(-np.pi + (np.arange(sc.w) + 0.5) * 2 * np.pi / sc.w))[:, None]
                                        - np.array(CROP_LONS)[None, :] + 180) % 360 - 180), axis=1)
        for lens in (0, 1):
            imgs = []
            for c in crops:
                rgb, ok = c.render_from_band(filled[lens], pb.w, pb.row0, pb.map_h)
                rgb = srgb_encode(rgb, 2.0) * ok[..., None]
                imgs.append((rgb * 255 + 0.5).astype(np.uint8))
            if fr == frames[0]:
                from PIL import Image
                Image.fromarray(np.concatenate([imgs[10], imgs[11]], 1)).save(
                    os.path.join(OUTD, f"crops_f{fr}_lens{lens}_wing.png"))
            maps = models.run(imgs)
            if fr == frames[0] and lens == 1:
                from PIL import Image
                rel = maps["rel"][11]
                v = (rel - rel.min()) / max(np.ptp(rel), 1e-6)
                Image.fromarray((v * 255).astype(np.uint8)).save(os.path.join(OUTD, f"dav2s_f{fr}_lens1_wing.png"))
            # to the scoring band, each column from its nearest crop
            for key, stack in maps.items():
                band = np.full((sc.h, sc.w), np.nan, np.float32)
                for ci, c in enumerate(crops):
                    img = stack[ci]
                    if key == "metric_dav2":
                        img = ray_distance_from_z(img, c)
                    val, ok = c.sample_to_band(img.astype(np.float32), sc.lat, sc.w)
                    sel = (crop_of_col == ci)[None, :] & ok
                    band[sel] = val[sel]
                depth_band[(lens, key)] = band

        # ---- 2. reference flow + confidence --------------------------------
        imgs, lat_in, w_in = rgb4096_inputs(fr, sc)
        hh = imgs[0].shape[0]
        f_fw, _ = flow_ref.tiles(imgs, 2 * hh, hh // 2)
        f_bw, _ = flow_ref.tiles([imgs[1], imgs[0]], 2 * hh, hh // 2)
        f_fw = to_scoring_grid(f_fw, lat_in, w_in, sc)
        f_bw = to_scoring_grid(f_bw, lat_in, w_in, sc)
        back, _ = warp(f_bw, f_fw)
        fb_err = np.hypot(f_fw[..., 0] + back[..., 0], f_fw[..., 1] + back[..., 1])
        conf = (fb_err < 0.5) & (sc.a0 > 0.5) & (sc.a1 > 0.5)
        f_prod = production_grid_flow(fr, sc)

        # ---- 3. rotation from the far field --------------------------------
        w_rot, r = fit_rotation(f_fw, conf, sc, farcols)
        res_far = f_fw - r
        m_far = np.zeros_like(conf)
        m_far[:, gcols] = conf[:, gcols]
        m_far &= sc.score_rows[:, None]
        rot_rms = float(np.sqrt(np.mean(res_far[m_far] ** 2)))

        # ---- 4. parallax vs depth on the wing ------------------------------
        _, _, A = rotation_design(sc)
        p = f_fw[..., 1] - r[..., 1]
        m_w = np.zeros_like(conf)
        m_w[:, wcols] = conf[:, wcols]
        m_w &= sc.score_rows[:, None]
        rel0, rel1 = depth_band[(0, "rel")], depth_band[(1, "rel")]
        rel_avg = 0.5 * (rel0 + rel1)
        fit = {}
        # far reference per crop: median relative disparity on ground pixels
        d0 = {}
        far_cols = np.zeros(sc.w, bool)
        far_cols[farcols] = True
        for ci in range(len(crops)):
            cm = np.zeros_like(conf)
            cm[:, crop_of_col == ci] = True
            cm &= sc.score_rows[:, None] & np.isfinite(rel_avg)
            g = cm & far_cols[None, :]
            d0[ci] = float(np.nanmedian(rel_avg[g])) if g.any() else float(np.nanmin(rel_avg[cm])) if cm.any() else 0.0
        d0_band = np.array([d0[c] for c in crop_of_col], np.float32)[None, :]
        x_rel = (rel_avg - d0_band) * np.cos(A)
        mm = m_w & np.isfinite(x_rel) & (x_rel > 0.25 * np.nanpercentile(x_rel[m_w], 90))
        if mm.sum() > 30:
            ratios = p[mm] / x_rel[mm]
            K_rel = float(np.median(ratios))
            fit["K_rel"] = K_rel
            fit["K_rel_iqr"] = [float(np.percentile(ratios, 25)), float(np.percentile(ratios, 75))]
            fit["n_wing_conf"] = int(mm.sum())
            fit["corr_p_vs_reldisp"] = float(np.corrcoef(p[mm], x_rel[mm])[0, 1])
        # Object pixels: the nearest 30 % (relative disparity) of the co-visible
        # wing columns.  Monocular metric models put the km-distant ground at
        # 5-20 m on these views, so a distance cut alone would mix ground in.
        cov = np.zeros_like(conf)
        cov[:, wcols] = True
        cov &= sc.score_rows[:, None] & (sc.a0 > 0.5) & (sc.a1 > 0.5) & np.isfinite(rel_avg)
        obj = cov & (rel_avg >= np.nanpercentile(rel_avg[cov], 70)) if cov.any() else cov
        fit["object_pixels"] = int(obj.sum())
        for key in ("metric_moge", "metric_dav2"):
            if (1, key) not in depth_band:
                continue
            Dm = 0.5 * (depth_band[(0, key)] + depth_band[(1, key)])
            fit[f"object_dist_m_{key}"] = float(np.nanmedian(Dm[obj])) if obj.any() else float("nan")
            gm = np.zeros_like(conf)
            gm[:, gcols] = True
            gm &= sc.score_rows[:, None] & np.isfinite(Dm)
            fit[f"ground_dist_m_{key}"] = float(np.nanmedian(Dm[gm])) if gm.any() else float("nan")
            mk = m_w & obj & np.isfinite(Dm) & (Dm > 0.2) & (Dm < 20)
            if mk.sum() > 30:
                p_rad = p[mk] * np.pi / sc.map_h
                b = np.abs(p_rad) / np.cos(A[mk]) * Dm[mk]
                fit[f"baseline_mm_{key}"] = float(np.median(b) * 1000)
                fit[f"baseline_mm_{key}_iqr"] = [float(np.percentile(b, 25) * 1000), float(np.percentile(b, 75) * 1000)]
                fit[f"wing_dist_m_{key}"] = float(np.median(Dm[mk]))
                fit[f"sign_{key}"] = float(np.sign(np.median(p[mk])))
                fit[f"corr_p_vs_invD_{key}"] = float(np.corrcoef(p[mk], np.cos(A[mk]) / Dm[mk])[0, 1])
        np.savez_compressed(os.path.join(OUTD, f"parallax_f{fr}.npz"), p=p, conf=conf, rel0=rel0, rel1=rel1,
                            moge0=depth_band.get((0, "metric_moge"), np.zeros(1)),
                            moge1=depth_band.get((1, "metric_moge"), np.zeros(1)),
                            dav2m0=depth_band[(0, "metric_dav2")], dav2m1=depth_band[(1, "metric_dav2")],
                            lat=sc.lat, r=r, f_fw=f_fw)

        # ---- 5. warps --------------------------------------------------------
        scores = {}
        zero = np.zeros((sc.h, sc.w, 2), np.float32)
        scores["none"] = sc.score(zero)
        scores["production DIS grid"] = sc.score(f_prod)
        scores["SEA-RAFT-S (reference flow)"] = sc.score(f_fw)
        scores["rotation only"] = sc.score(r)
        # single global offset along the meridian, on top of the rotation
        best = None
        for c in np.arange(-12.0, 12.01, 0.5):
            f = r.copy()
            f[..., 1] += c
            s = sc.score(f)
            if best is None or s["wing"] > best[1]["wing"]:
                best = (float(c), s)
        scores["global offset (best for wing)"] = dict(best[1], offsetPx=best[0])
        # near / far pair: near in the wing columns, far elsewhere
        mcol = cols_mask(sc, wcols)[None, :]
        best_nf = None
        for cn in np.arange(-12.0, 12.01, 0.5):
            f = r.copy()
            f[..., 1] += cn * mcol
            s = sc.score(f)
            if best_nf is None or s["wing"] > best_nf[1]["wing"]:
                best_nf = (float(cn), s)
        scores["near/far offsets (best near, far = 0)"] = dict(best_nf[1], nearPx=best_nf[0])
        # depth-aware, relative depth with the fitted K
        if "K_rel" in fit:
            dy0 = np.nan_to_num(fit["K_rel"] * (rel0 - d0_band) * np.cos(A))
            dy1 = np.nan_to_num(fit["K_rel"] * (rel1 - d0_band) * np.cos(A))
            dy_avg = 0.5 * (dy0 + dy1)
            f = r.copy()
            f[..., 1] += dy_avg
            scores["depth-aware (DA-V2-S relative, K fitted)"] = sc.score(f)
            h0 = -0.5 * r.copy()
            h1 = 0.5 * r.copy()
            h0[..., 1] -= 0.5 * dy0
            h1[..., 1] += 0.5 * dy1
            scores["depth-aware per lens (DA-V2-S)"] = sc.score_halves(h0, h1)

        def per_lens_halves(dy0, dy1):
            """Each lens moved half the rotation plus its OWN depth parallax."""
            h0 = -0.5 * r.copy()
            h1 = 0.5 * r.copy()
            h0[..., 1] -= 0.5 * np.nan_to_num(dy0)
            h1[..., 1] += 0.5 * np.nan_to_num(dy1)
            return h0, h1

        # oracle K for the relative model: the best any single scale can do
        x0 = (rel0 - d0_band) * np.cos(A)
        x1 = (rel1 - d0_band) * np.cos(A)
        best_k = None
        for K in np.arange(-3.0, 3.01, 0.1):
            s = sc.score_halves(*per_lens_halves(K * x0, K * x1))
            if best_k is None or s["wing"] > best_k[1]["wing"]:
                best_k = (float(K), s)
        scores["depth-aware per lens (DA-V2-S, oracle K)"] = dict(best_k[1], K=best_k[0])

        # metric: parallax = +-b * mapH / pi * cos(lat) / D, b swept (the
        # baseline the sample itself prefers), raw and with far pixels at infinity
        if (1, "metric_moge") in depth_band:
            far0 = rel0 <= d0_band + 0.5 * (np.nanpercentile(rel_avg[cov], 70) - d0_band) if cov.any() else rel0 < 0
            far1 = rel1 <= d0_band + 0.5 * (np.nanpercentile(rel_avg[cov], 70) - d0_band) if cov.any() else rel1 < 0
            for tag, clamp in (("raw", False), ("far = infinity", True)):
                D0 = depth_band[(0, "metric_moge")].copy()
                D1 = depth_band[(1, "metric_moge")].copy()
                if clamp:
                    D0[far0] = np.inf
                    D1[far1] = np.inf
                sweep = []
                for sign in (-1.0, 1.0):
                    for b_mm in np.arange(0.0, 60.01, 5.0):
                        k = sign * b_mm / 1000.0 * sc.map_h / np.pi
                        s = sc.score_halves(*per_lens_halves(k * np.cos(A) / D0, k * np.cos(A) / D1))
                        sweep.append((sign * b_mm, s))
                best_b = max(sweep, key=lambda t: t[1]["wing"])
                sgn = float(np.sign(best_b[0])) if best_b[0] != 0 else -1.0
                at30 = [s for b, s in sweep if b == sgn * 30.0]
                scores[f"depth-aware metric MoGe-2 ({tag}), best b"] = dict(best_b[1], b_mm=best_b[0])
                if at30:
                    scores[f"depth-aware metric MoGe-2 ({tag}), b = 30 mm"] = at30[0]
                fit[f"b_sweep_wing_ncc_{tag}"] = [[float(b), float(s["wing"]), float(s["ground"])] for b, s in sweep]
        results[str(fr)] = {"fit": fit, "rotation_w": w_rot.tolist(), "rotation_residual_rms_px": rot_rms,
                            "scores": scores}
        print(json.dumps(fit, indent=1))
        for k, s in scores.items():
            print(f"  {k:45s} ground {s['ground']:.4f} wing {s['wing']:.4f} band {s['band']:.4f} "
                  f"edgeNCC {s['wingEdgeNcc']:.3f} chamfer {s['wingChamferPx']:.2f}", flush=True)
        with open(os.path.join(OUTD, "depth_eval.json"), "w", encoding="utf-8") as f:
            json.dump({"results": results, "times_ms_per_crop": {k: float(np.median(v)) if v else None
                                                                  for k, v in models.times.items()}}, f, indent=1)
    print("ms per 518^2 crop:", {k: (round(float(np.median(v)), 2) if v else None) for k, v in models.times.items()})


if __name__ == "__main__":
    main()
