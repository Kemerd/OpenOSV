"""Would a depth-placed seam be steadier than the current carve? (research only)

Inputs (all from the release build):
  * seamprobe (research/aistitch/seamprobe.cpp) over frames 0-64:
      - bucket_<B>.f32: the importer's schedule, one carve per 8-frame bucket
        steered by the previous bucket (ImporterInstance::applyAnalyses);
      - frame_<F>.f32:  a carve on every frame steered by the previous frame;
      - f<F>_luma*/alpha*: the bands the carve sees (through the grid);
  * bandprobe 2048 x +-45 deg per-lens RGB for every frame -> Depth Anything V2
    Small (Apache-2.0) relative inverse depth on 12 seam-centred crops per lens.

Seams compared (latitude per 1024 longitude columns):
  carve, importer schedule   bucket seams glided per frame with
                             parallaxCrossfadeWeight, exactly as the importer
  carve, every frame         frame_<F>
  depth seam, every frame    DP over a cost that is only "how near is the
                             content here" (normalised relative inverse depth,
                             both lenses) + coverage + the same centre pull
                             and step rules as the carve; NO temporal term
  depth seam, importer schedule   the same, carved per bucket and glided

Steadiness: per consecutive frame pair, |d lat| per column (deg): mean, p99,
max, and the share of columns that move > 0.05 deg (about 1 px at 6K);
over the wing columns and elsewhere.
Quality: mean |lens0 - lens1| (code luma) inside +-2 band rows (0.35 deg,
the narrow feather) of the seam, per column - what a cut there shows.

    python seam_stability.py
"""
import json
import os
import warnings

import numpy as np

warnings.filterwarnings("ignore")

from common import OUT, ProbeBands, srgb_encode, utf8_console  # noqa: E402
from sphere import PerspectiveCrop  # noqa: E402

utf8_console()
SEAM = os.path.join(OUT, "seam")
BANDS2K = os.path.join(OUT, "bands2k")
FRAMES = list(range(0, 65))
BUCKET = 8
CROP_LONS = [-165 + 30 * i for i in range(12)]


def load_meta():
    with open(os.path.join(SEAM, "meta.json"), "r", encoding="utf-8") as f:
        return json.load(f)


def table(path, cols):
    a = np.fromfile(path, dtype=np.float32)
    return a.reshape(cols, 2)[:, 0]                      # latitude, radians


def glide(bucket_lat, frame):
    """The importer's per-frame seam: previous bucket -> own bucket."""
    k = frame // BUCKET
    own = bucket_lat[k]
    if k == 0 or (k - 1) not in bucket_lat:
        return own
    w = (frame % BUCKET + 1) / BUCKET
    return bucket_lat[k - 1] + (own - bucket_lat[k - 1]) * w


def movement(seams, wing_cols):
    """Frame-to-frame motion statistics of a {frame: lat[cols]} series (deg)."""
    d = np.array([np.degrees(seams[f + 1] - seams[f]) for f in FRAMES[:-1]])
    other = np.ones(d.shape[1], bool)
    other[wing_cols] = False
    out = {}
    for name, sel in (("wing", wing_cols), ("elsewhere", other)):
        a = np.abs(d[:, sel])
        out[name] = {"mean_deg": float(a.mean()), "p99_deg": float(np.percentile(a, 99)),
                     "max_deg": float(a.max()), "share_moving_gt_0p05": float((a > 0.05).mean())}
    # total wander over the clip: per-column range of the seam latitude
    lat = np.degrees(np.array([seams[f] for f in FRAMES]))
    out["wing"]["range_over_clip_deg"] = float(np.mean(np.ptp(lat[:, wing_cols], axis=0)))
    out["elsewhere"]["range_over_clip_deg"] = float(np.mean(np.ptp(lat[:, other], axis=0)))
    return out


def dp_seam(cost, max_step=2, step_penalty=0.03):
    """One row per column through cost (rows x cols), open ends (the
    production DP closes the ring; for steadiness that does not matter)."""
    rows, cols = cost.shape
    acc = cost[:, 0].copy()
    back = np.zeros((rows, cols), np.int32)
    idx = np.arange(rows)
    for c in range(1, cols):
        best = np.full(rows, np.inf)
        arg = np.zeros(rows, np.int32)
        for s in range(-max_step, max_step + 1):
            src = idx - s
            ok = (src >= 0) & (src < rows)
            v = np.full(rows, np.inf)
            v[ok] = acc[src[ok]] + step_penalty * abs(s)
            better = v < best
            best[better] = v[better]
            arg[better] = src[better]
        acc = best + cost[:, c]
        back[:, c] = arg
    path = np.zeros(cols, np.int32)
    path[-1] = int(np.argmin(acc))
    for c in range(cols - 1, 0, -1):
        path[c - 1] = back[path[c], c]
    return path


def gauss1d(x, sigma):
    k = np.arange(-int(3 * sigma) - 1, int(3 * sigma) + 2)
    w = np.exp(-0.5 * (k / sigma) ** 2)
    w /= w.sum()
    xp = np.concatenate([x[-len(k):], x, x[:len(k)]])            # wrap: longitude ring
    return np.convolve(xp, w, mode="same")[len(k):-len(k)]


class DepthNet:
    def __init__(self):
        import torch
        from transformers import AutoImageProcessor, AutoModelForDepthEstimation
        from common import MODELS
        self.torch = torch
        cd = os.path.join(MODELS, "hf")
        self.proc = AutoImageProcessor.from_pretrained("depth-anything/Depth-Anything-V2-Small-hf", cache_dir=cd)
        self.model = AutoModelForDepthEstimation.from_pretrained("depth-anything/Depth-Anything-V2-Small-hf",
                                                                 cache_dir=cd, dtype=torch.float16).cuda().eval()

    def __call__(self, imgs):
        torch = self.torch
        pv = self.proc(images=list(imgs), return_tensors="pt", do_resize=False)["pixel_values"].cuda().half()
        with torch.no_grad():
            out = self.model(pixel_values=pv).predicted_depth
        out = torch.nn.functional.interpolate(out[:, None].float(), size=imgs[0].shape[:2], mode="bilinear",
                                              align_corners=False)[:, 0]
        return out.cpu().numpy()


def nearness_band(frame, pb, crops, net, meta):
    """Per band pixel of the carve band: max over lenses of normalised nearness
    in [0, 1] (0 = the far field of that crop, 1 = the nearest content)."""
    w, h = meta["w"], meta["h"]
    rows = np.arange(h) + meta["rowOffset"] + 0.5
    lat = 90.0 - rows * 180.0 / meta["mapH"]
    lon_deg = np.degrees(-np.pi + (np.arange(w) + 0.5) * 2 * np.pi / w)
    crop_of_col = np.argmin(np.abs(((lon_deg[:, None] - np.array(CROP_LONS)[None, :]) + 180) % 360 - 180), axis=1)
    b0 = pb.load(frame, "lens0")
    b1 = pb.load(frame, "lens1")
    from flow_eval import fill_outside
    filled = fill_outside([b0[..., :3], b1[..., :3]], [b0[..., 3], b1[..., 3]])
    near = np.zeros((h, w), np.float32)
    for lens in (0, 1):
        imgs = []
        for c in crops:
            rgb, ok = c.render_from_band(filled[lens], pb.w, pb.row0, pb.map_h)
            imgs.append((srgb_encode(rgb, 2.0) * ok[..., None] * 255 + 0.5).astype(np.uint8))
        disp = net(imgs)
        band = np.zeros((h, w), np.float32)
        for ci, c in enumerate(crops):
            # far reference and scale per crop, from the crop itself
            d = disp[ci]
            lo, hi = np.percentile(d, 20), np.percentile(d, 99.5)
            nd = np.clip((d - lo) / max(hi - lo, 1e-6), 0, 1).astype(np.float32)
            val, ok = c.sample_to_band(nd, lat, w)
            sel = (crop_of_col == ci)[None, :] & ok
            band[sel] = val[sel]
        near = np.maximum(near, band)
    return near


def depth_seam(near, alpha0, alpha1, cols):
    """DP seam on nearness only (+ coverage + centre pull), 1024 columns."""
    h, w = near.shape
    f = w // cols
    n = near.reshape(h, cols, f).mean(2)
    a0 = alpha0.reshape(h, cols, f).min(2)
    a1 = alpha1.reshape(h, cols, f).min(2)
    cost = np.zeros((h, cols), np.float32)
    half = 2                                                      # the narrow feather, +-2 rows
    for r in range(h):
        r0, r1 = max(0, r - half), min(h, r + half + 1)
        cost[r] = n[r0:r1].mean(0)
        blocked = (a0[r0:r1].min(0) < 0.02) | (a1[r0:r1].min(0) < 0.02)
        cost[r] += np.where(blocked, 1e6, 0.0)
        # coverage shortfall of the lens shown on each side of the seam
        # (master above the seam: +lat rows come first), weight 6 per row as the carve
        cost[r] += 6.0 * (np.maximum(0, 0.5 - a1[:r]).sum(0) + np.maximum(0, 0.5 - a0[r:]).sum(0))
    centre = (h - 1) / 2.0
    cost += 0.15 * (np.abs(np.arange(h) - centre) / centre)[:, None]
    path = dp_seam(cost).astype(np.float64)
    path = gauss1d(path, 1.5)
    return path


def rows_to_lat(path_rows, meta):
    return np.radians(90.0 - (meta["rowOffset"] + path_rows + 0.5) * 180.0 / meta["mapH"])


def seam_disagreement(frame, seams_lat, meta, wing_cols):
    """Mean |luma0 - luma1| within +-2 rows of the seam, per region."""
    w, h, cols = meta["w"], meta["h"], meta["columns"]
    l0 = np.fromfile(os.path.join(SEAM, f"f{frame}_luma0.f32"), np.float32).reshape(h, w)
    l1 = np.fromfile(os.path.join(SEAM, f"f{frame}_luma1.f32"), np.float32).reshape(h, w)
    a0 = np.fromfile(os.path.join(SEAM, f"f{frame}_alpha0.f32"), np.float32).reshape(h, w)
    a1 = np.fromfile(os.path.join(SEAM, f"f{frame}_alpha1.f32"), np.float32).reshape(h, w)
    f = w // cols
    rows = 90.0 - np.degrees(seams_lat)                           # colatitude of the seam
    rowf = rows * meta["mapH"] / 180.0 - meta["rowOffset"] - 0.5
    d = np.abs(l0 - l1)
    ok = (a0 > 0.5) & (a1 > 0.5)
    per = np.full(cols, np.nan)
    for c in range(cols):
        r = int(round(rowf[c]))
        sl = slice(max(0, r - 2), min(h, r + 3))
        cs = slice(c * f, (c + 1) * f)
        m = ok[sl, cs]
        if m.any():
            per[c] = d[sl, cs][m].mean()
    other = np.ones(cols, bool)
    other[wing_cols] = False
    return float(np.nanmean(per[wing_cols])), float(np.nanmean(per[other]))


def main():
    meta = load_meta()
    cols = meta["columns"]
    wing_cols = np.arange(1880 // 2, 2041 // 2)
    # ---- the carve, as the importer schedules it and per frame ---------------
    bucket_lat = {b: table(os.path.join(SEAM, f"bucket_{b}.f32"), cols) for b in range(0, 9)}
    carve_sched = {f: glide(bucket_lat, f) for f in FRAMES}
    carve_frame = {f: table(os.path.join(SEAM, f"frame_{f}.f32"), cols) for f in FRAMES}
    # ---- the depth seam --------------------------------------------------------
    pb = ProbeBands(BANDS2K)
    crops = [PerspectiveCrop(lc, 0.0, 90.0, 518) for lc in CROP_LONS]
    net = DepthNet()
    depth_frame = {}
    h, w = meta["h"], meta["w"]
    for f in FRAMES:
        near = nearness_band(f, pb, crops, net, meta)
        a0 = np.fromfile(os.path.join(SEAM, f"f{f}_alpha0.f32"), np.float32).reshape(h, w)
        a1 = np.fromfile(os.path.join(SEAM, f"f{f}_alpha1.f32"), np.float32).reshape(h, w)
        depth_frame[f] = rows_to_lat(depth_seam(near, a0, a1, cols), meta)
        if f % 16 == 0:
            print(f"depth seam frame {f}", flush=True)
    depth_bucket = {b: depth_frame[b * BUCKET] for b in range(0, 9)}
    depth_sched = {f: glide(depth_bucket, f) for f in FRAMES}

    series = {"carve, importer schedule (bucket + glide)": carve_sched,
              "carve, every frame (steered by the previous frame)": carve_frame,
              "depth seam, every frame (no temporal term)": depth_frame,
              "depth seam, importer schedule (bucket + glide)": depth_sched}
    res = {}
    for name, s in series.items():
        mv = movement(s, wing_cols)
        dis = [seam_disagreement(f, s[f], meta, wing_cols) for f in (0, 16, 32, 48, 64)]
        mv["seam_disagreement_wing"] = float(np.mean([x[0] for x in dis]))
        mv["seam_disagreement_elsewhere"] = float(np.mean([x[1] for x in dis]))
        mv["mean_abs_lat_deg"] = float(np.mean([np.abs(np.degrees(s[f])).mean() for f in FRAMES]))
        res[name] = mv
        print(f"{name}\n   wing: mean {mv['wing']['mean_deg']:.4f} p99 {mv['wing']['p99_deg']:.3f} max "
              f"{mv['wing']['max_deg']:.3f} deg/frame, moving>0.05: {100 * mv['wing']['share_moving_gt_0p05']:.1f} %, "
              f"range {mv['wing']['range_over_clip_deg']:.2f} deg | disagreement {mv['seam_disagreement_wing']:.4f}\n"
              f"   else: mean {mv['elsewhere']['mean_deg']:.4f} p99 {mv['elsewhere']['p99_deg']:.3f} max "
              f"{mv['elsewhere']['max_deg']:.3f} deg/frame, moving>0.05: "
              f"{100 * mv['elsewhere']['share_moving_gt_0p05']:.1f} %, range "
              f"{mv['elsewhere']['range_over_clip_deg']:.2f} deg | disagreement "
              f"{mv['seam_disagreement_elsewhere']:.4f}", flush=True)
    with open(os.path.join(SEAM, "seam_stability.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    np.savez_compressed(os.path.join(SEAM, "seams.npz"),
                        **{f"{k}": np.array([v[f] for f in FRAMES]) for k, v in
                           zip(["carve_sched", "carve_frame", "depth_frame", "depth_sched"], series.values())})


if __name__ == "__main__":
    main()
