"""Question 1: diffusion inpainting / refinement of the seam band (research only).

Input: the importer's default stitch (osvtool render --mode equirect-polar
--parallax --flow-backend classical --seam-carve --photo full --color linear,
6144 x 3072, frames 28-36), cut into 1024 x 512 tiles (60 x 30 deg) centred
on the seam.  The seam band |lat| <= MASK_DEG is masked and handed to a
commercially licensed generator:

  * FLUX.2 [klein] 4B (Apache-2.0), Flux2KleinInpaintPipeline, 4 steps:
      - "inpaint": strength 1.0, the band is regenerated;
      - "refine":  strength 0.35, the band is re-noised and denoised (SDEdit),
        which keeps its structure;
    one fixed seed for every frame (the kindest case for flicker);
  * Wan2.1-VACE 1.3B (Apache-2.0), WanVACEPipeline: the 9 frames as one
    masked video-to-video job (temporal attention across frames).

The generated band is composited back with a feathered mask, so pixels
outside the band are the input's.

Measurements, all inside the mask unless stated:
  fidelity   PSNR (8-bit display) against the input stitch; detail change =
             RMS(highpass(out) - highpass(in)) / RMS(highpass(in));
  invented   per 16 x 16 textured patch, the best NCC against either lens
             rendered ALONE (bandprobe, +-4 px search): fraction below 0.5
             ("supported by neither lens"), against the input's own fraction;
  seam       mean |d/dlat log2 L| over |lat| < 2 deg divided by the same
             over 4-10 deg (1.0 = the seam rows are as smooth as the rest);
  flicker    RMS over consecutive frames of the change in log2 luma the
             generator ADDED: (out[t+1]-out[t]) - (in[t+1]-in[t]), stops;
  runtime    per tile on the RTX 5090, and per 6K frame (the +-7 deg overlap
             strip of a 6144-column map = 6 tiles).

    python gen_seam.py flux   # FLUX.2 klein 4B variants
    python gen_seam.py vace   # Wan2.1-VACE 1.3B
    python gen_seam.py score  # metrics + figures from the saved outputs
"""
import json
import os
import sys
import time
import warnings

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
warnings.filterwarnings("ignore")

import cv2  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

from common import MODELS, OUT, ProbeBands, luma709, srgb_decode, srgb_encode, utf8_console  # noqa: E402

utf8_console()

FRAMES = list(range(28, 37))
MAP_W, MAP_H = 6144, 3072
PX_PER_DEG = MAP_H / 180.0
TILE_W, TILE_H = 1024, 512
MASK_DEG = 3.0
EXPOSURE = 2.0
TILES = {                       # name -> centre longitude (deg)
    "wing": 160.0,              # the nacelle crossing (lon ~150-180)
    "skyground": -2.0,          # open sky meeting the ground at the horizon haze
    "sky": -90.0,               # open sky
    "ground": 60.0,             # suburbs from altitude
}
PROMPT = ("aerial photograph from a small aircraft: suburban city far below, clear blue sky, "
          "part of the aircraft's metal wing and engine nacelle, sharp, natural colours")
NEUTRAL = "a photograph"
# Every variant: generator, its settings, the prompt and the half-height of
# the repainted band.  "narrow" is the most favourable case for inpainting:
# only +-1 deg (about the carved seam's mean feather, 0.87 deg) is repainted
# and everything around it is context.
VARIANTS = {
    "flux_inpaint": {"model": "flux", "strength": 1.0, "prompt": PROMPT, "mask_deg": MASK_DEG},
    "flux_inpaint_neutral": {"model": "flux", "strength": 1.0, "prompt": NEUTRAL, "mask_deg": MASK_DEG},
    "flux_inpaint_narrow": {"model": "flux", "strength": 1.0, "prompt": NEUTRAL, "mask_deg": 1.0},
    "flux_refine": {"model": "flux", "strength": 0.35, "prompt": PROMPT, "mask_deg": MASK_DEG},
    "vace_30": {"model": "vace", "steps": 30, "prompt": PROMPT, "mask_deg": MASK_DEG},
    "vace_30_narrow": {"model": "vace", "steps": 30, "prompt": NEUTRAL, "mask_deg": 1.0},
}
GEN = os.path.join(OUT, "gen")
STITCH = os.path.join(OUT, "stitch")
BANDS6K = os.path.join(OUT, "bands6k")


# --------------------------------------------------------------------------
#  Data
# --------------------------------------------------------------------------
def tile_cols(name):
    c = int(round((TILES[name] + 180.0) / 360.0 * MAP_W))
    return (np.arange(TILE_W) + c - TILE_W // 2) % MAP_W


def tile_rows():
    r0 = MAP_H // 2 - TILE_H // 2
    return np.arange(r0, r0 + TILE_H)


def tile_lat():
    return 90.0 - (tile_rows() + 0.5) * 180.0 / MAP_H


def load_stitch(frame):
    p = os.path.join(STITCH, f"f_{frame:05d}.exr")
    img = cv2.imread(p, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise FileNotFoundError(p)
    img = img[..., :3][..., ::-1].astype(np.float32)          # BGR(A) -> RGB
    if img.shape[:2] != (MAP_H, MAP_W):
        raise ValueError(f"{p}: {img.shape}")
    return img


def input_tiles():
    """{tile: [frames] of linear RGB (TILE_H, TILE_W, 3)} from the default stitch."""
    rows = tile_rows()
    out = {k: [] for k in TILES}
    for fr in FRAMES:
        img = load_stitch(fr)
        for k in TILES:
            out[k].append(img[rows][:, tile_cols(k)])
    return out


def seam_mask(feather_px=0, mask_deg=MASK_DEG):
    lat = tile_lat()
    m = (np.abs(lat) <= mask_deg).astype(np.float32)
    if feather_px > 0:
        m = cv2.GaussianBlur(m[:, None].repeat(8, 1), (0, 0), feather_px)[:, 0]
    return np.repeat(m[:, None], TILE_W, 1)


def to_u8(lin):
    return (srgb_encode(lin, EXPOSURE) * 255 + 0.5).astype(np.uint8)


def from_u8(u8):
    return srgb_decode(u8.astype(np.float32) / 255.0, EXPOSURE)


def save_out(variant, tile, frame, u8):
    d = os.path.join(GEN, variant, tile)
    os.makedirs(d, exist_ok=True)
    Image.fromarray(u8).save(os.path.join(d, f"f{frame}.png"))


def load_out(variant, tile, frame):
    p = os.path.join(GEN, variant, tile, f"f{frame}.png")
    return np.asarray(Image.open(p).convert("RGB")) if os.path.exists(p) else None


def composite(in_u8, gen_u8, mask_deg=MASK_DEG):
    m = seam_mask(feather_px=6, mask_deg=mask_deg)[..., None]
    return (in_u8.astype(np.float32) * (1 - m) + gen_u8.astype(np.float32) * m + 0.5).astype(np.uint8)


# --------------------------------------------------------------------------
#  Generators
# --------------------------------------------------------------------------
def run_flux():
    import torch
    from diffusers import Flux2KleinInpaintPipeline
    path = os.path.join(MODELS, "flux2_klein_4b")
    pipe = Flux2KleinInpaintPipeline.from_pretrained(path, torch_dtype=torch.bfloat16).to("cuda")
    pipe.set_progress_bar_config(disable=True)
    tiles = input_tiles()
    times_path = os.path.join(GEN, "times_flux.json")
    times = {}
    if os.path.exists(times_path):
        with open(times_path, "r", encoding="utf-8") as f:
            times = json.load(f)
    for variant, cfg in VARIANTS.items():
        if cfg["model"] != "flux" or variant in times:
            continue                                              # already generated
        mask = Image.fromarray((seam_mask(mask_deg=cfg["mask_deg"]) * 255).astype(np.uint8))
        # The prompt is encoded once per variant (it does not change per
        # frame), so the timed call is the image work alone.
        with torch.no_grad():
            pe = pipe.encode_prompt(prompt=cfg["prompt"], device="cuda")
        prompt_embeds = pe[0] if isinstance(pe, tuple) else pe
        dts = []
        for k, frames in tiles.items():
            for i, fr in enumerate(FRAMES):
                src = to_u8(frames[i])
                g = torch.Generator("cuda").manual_seed(1234)          # one seed for every frame
                torch.cuda.synchronize()
                t0 = time.perf_counter()
                res = pipe(prompt_embeds=prompt_embeds, image=Image.fromarray(src), mask_image=mask, height=TILE_H,
                           width=TILE_W, strength=cfg["strength"], num_inference_steps=4, guidance_scale=1.0,
                           generator=g).images[0]
                torch.cuda.synchronize()
                dt = time.perf_counter() - t0
                dts.append(dt)
                gen = np.asarray(res.convert("RGB").resize((TILE_W, TILE_H), Image.LANCZOS))
                save_out(variant, k, fr, composite(src, gen, cfg["mask_deg"]))
                print(f"{variant} {k} f{fr}: {dt * 1000:.0f} ms", flush=True)
        times[variant] = {"median_s_per_tile": float(np.median(dts[1:])), "n": len(dts),
                          "note": "prompt encoded once per variant, not timed"}
        with open(times_path, "w", encoding="utf-8") as f:
            json.dump(times, f, indent=1)


def run_vace():
    import torch
    from diffusers import AutoencoderKLWan, WanVACEPipeline
    path = os.path.join(MODELS, "wan21_vace_1_3b")
    vae = AutoencoderKLWan.from_pretrained(path, subfolder="vae", torch_dtype=torch.float32)
    pipe = WanVACEPipeline.from_pretrained(path, vae=vae, torch_dtype=torch.bfloat16).to("cuda")
    pipe.set_progress_bar_config(disable=True)
    W, H = 832, 416                                             # the model's 480p class, /16
    tiles = input_tiles()
    neg = "blurry, distorted, text, watermark, low quality, artifacts"
    times = {}
    for variant, cfg in VARIANTS.items():
        if cfg["model"] != "vace":
            continue
        m = seam_mask(mask_deg=cfg["mask_deg"])
        mask = Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.NEAREST)
        dts = []
        for k, frames in tiles.items():
            video = [Image.fromarray(to_u8(f)).resize((W, H), Image.LANCZOS) for f in frames]
            g = torch.Generator("cuda").manual_seed(1234)
            torch.cuda.synchronize()
            t0 = time.perf_counter()
            res = pipe(video=video, mask=[mask] * len(video), prompt=cfg["prompt"], negative_prompt=neg, height=H,
                       width=W, num_frames=len(video), num_inference_steps=cfg["steps"], guidance_scale=5.0,
                       generator=g, output_type="np").frames[0]
            torch.cuda.synchronize()
            dt = time.perf_counter() - t0
            dts.append(dt)
            for i, fr in enumerate(FRAMES):
                src = to_u8(frames[i])
                gen = (np.clip(res[i], 0, 1) * 255 + 0.5).astype(np.uint8)
                gen = np.asarray(Image.fromarray(gen).resize((TILE_W, TILE_H), Image.LANCZOS))
                save_out(variant, k, fr, composite(src, gen, cfg["mask_deg"]))
            print(f"{variant} {k}: {dt:.1f} s for {len(video)} frames", flush=True)
        times[variant] = {"median_s_per_clip_tile": float(np.median(dts)), "frames": len(FRAMES), "n": len(dts),
                          "size": [W, H], "steps": cfg["steps"]}
    with open(os.path.join(GEN, "times_vace.json"), "w", encoding="utf-8") as f:
        json.dump(times, f, indent=1)


# --------------------------------------------------------------------------
#  Metrics
# --------------------------------------------------------------------------
def highpass(y):
    return y - cv2.GaussianBlur(y, (0, 0), 1.5)


def lens_tiles(tile, frame):
    """Each lens alone (bandprobe, 6144 x +-16 deg) on the tile grid, u8 luma + alpha."""
    pb = ProbeBands(BANDS6K)
    rows = tile_rows() - pb.row0
    cols = tile_cols(tile)
    out = []
    for tag in ("lens0", "lens1"):
        b = pb.load(frame, tag)[rows][:, cols]
        out.append((luma709(to_u8(b[..., :3]).astype(np.float32)), b[..., 3]))
    return out


def invented_fraction(img_u8, lenses, mask, patch=16, search=4, min_std=2.0):
    """Share of textured patches in the mask that match neither lens (NCC < 0.5)."""
    y = luma709(img_u8.astype(np.float32))
    h, w = y.shape
    scores = []
    for r in range(search, h - patch - search, patch):
        for c in range(search, w - patch - search, patch):
            if mask[r:r + patch, c:c + patch].mean() < 0.99:
                continue
            p = y[r:r + patch, c:c + patch]
            best = -1.0
            textured = False
            for ly, la in lenses:
                if la[r:r + patch, c:c + patch].min() < 0.5:
                    continue
                win = ly[r - search:r + patch + search, c - search:c + patch + search]
                if win[search:search + patch, search:search + patch].std() > min_std:
                    textured = True
                res = cv2.matchTemplate(win.astype(np.float32), p.astype(np.float32), cv2.TM_CCOEFF_NORMED)
                best = max(best, float(res.max()))
            if textured and p.std() > min_std:
                scores.append(best)
    s = np.array(scores)
    return (float(np.mean(s < 0.5)) if s.size else float("nan")), (float(np.median(s)) if s.size else float("nan")), \
        int(s.size)


def seam_ratio(img_u8):
    lat = tile_lat()
    y = np.log2(np.maximum(luma709(from_u8(img_u8)), 1e-4))
    g = np.abs(np.diff(y, axis=0))
    latm = 0.5 * (lat[1:] + lat[:-1])
    a = g[np.abs(latm) < 2.0].mean()
    b = g[(np.abs(latm) > 4.0) & (np.abs(latm) < 10.0)].mean()
    return float(a / b)


def score():
    tiles_in = input_tiles()
    variants = [v for v in VARIANTS if os.path.isdir(os.path.join(GEN, v))]
    res = {}
    for k in TILES:
        ins = [to_u8(f) for f in tiles_in[k]]
        lenses = {fr: lens_tiles(k, fr) for fr in (FRAMES[0], FRAMES[4], FRAMES[-1])}
        for variant in ["input"] + variants:
            # each variant is scored inside the band IT repainted
            mask = seam_mask(mask_deg=VARIANTS.get(variant, {}).get("mask_deg", MASK_DEG)) > 0.5
            outs = ins if variant == "input" else [load_out(variant, k, fr) for fr in FRAMES]
            if any(o is None for o in outs):
                continue
            r = {}
            # fidelity to the input stitch
            psnr, det = [], []
            for o, i in zip(outs, ins):
                d = (o.astype(np.float32) - i.astype(np.float32))[mask]
                mse = float(np.mean(d * d))
                psnr.append(10 * np.log10(255.0 ** 2 / max(mse, 1e-6)))
                ho, hi = highpass(luma709(o.astype(np.float32))), highpass(luma709(i.astype(np.float32)))
                det.append(float(np.sqrt(np.mean((ho - hi)[mask] ** 2)) / max(np.sqrt(np.mean(hi[mask] ** 2)), 1e-6)))
            r["psnr_vs_input_db"] = float(np.median(psnr)) if variant != "input" else float("inf")
            r["detail_change"] = float(np.median(det))
            # invented content: support by either lens
            inv = [invented_fraction(outs[FRAMES.index(fr)], lenses[fr], mask) for fr in lenses]
            r["unsupported_patch_frac"] = float(np.mean([x[0] for x in inv]))
            r["median_best_lens_ncc"] = float(np.mean([x[1] for x in inv]))
            r["patches"] = int(np.mean([x[2] for x in inv]))
            # seam visibility
            r["seam_ratio"] = float(np.median([seam_ratio(o) for o in outs]))
            # temporal: change the generator ADDED between consecutive frames
            add, ratio = [], []
            for t in range(len(outs) - 1):
                lo0 = np.log2(np.maximum(luma709(from_u8(outs[t])), 1e-4))
                lo1 = np.log2(np.maximum(luma709(from_u8(outs[t + 1])), 1e-4))
                li0 = np.log2(np.maximum(luma709(from_u8(ins[t])), 1e-4))
                li1 = np.log2(np.maximum(luma709(from_u8(ins[t + 1])), 1e-4))
                dout, din = (lo1 - lo0)[mask], (li1 - li0)[mask]
                add.append(float(np.sqrt(np.mean((dout - din) ** 2))))
                ratio.append(float(np.sqrt(np.mean(dout ** 2)) / max(np.sqrt(np.mean(din ** 2)), 1e-6)))
            r["added_temporal_change_stops"] = float(np.mean(add))
            r["temporal_change_ratio"] = float(np.mean(ratio))
            res.setdefault(k, {})[variant] = r
            print(f"{k:10s} {variant:14s} " + " ".join(f"{kk}={vv:.3f}" for kk, vv in r.items()
                                                        if isinstance(vv, float)), flush=True)
    with open(os.path.join(GEN, "gen_metrics.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    figures(tiles_in, variants)


def figures(tiles_in, variants):
    """Frame 32 input vs every variant per tile, and a 3-frame strip of the wing."""
    rows = tile_rows()
    lat = tile_lat()
    band = np.abs(lat) <= 12.0
    i32 = FRAMES.index(32)
    for k in TILES:
        panels = [("input (default stitch)", to_u8(tiles_in[k][i32]))]
        for v in variants:
            o = load_out(v, k, 32)
            if o is not None:
                panels.append((v, o))
        strip = []
        for name, img in panels:
            crop = img[band]
            im = Image.fromarray(crop)
            from PIL import ImageDraw
            d = ImageDraw.Draw(im)
            d.rectangle([0, 0, 7 * len(name) + 8, 14], fill=(0, 0, 0))
            d.text((4, 1), name, fill=(255, 255, 255))
            strip.append(np.asarray(im))
            strip.append(np.full((4, crop.shape[1], 3), 255, np.uint8))
        Image.fromarray(np.concatenate(strip, 0)).save(os.path.join(GEN, f"fig_{k}_f32.png"))
    # temporal strip: the wing tile's seam band over three consecutive frames per variant
    for v in ["input"] + variants:
        seq = []
        for fr in (31, 32, 33):
            o = to_u8(tiles_in["wing"][FRAMES.index(fr)]) if v == "input" else load_out(v, "wing", fr)
            if o is None:
                break
            seq.append(o[np.abs(lat) <= 6.0][:, 300:800])
        if len(seq) == 3:
            Image.fromarray(np.concatenate(seq, 1)).save(os.path.join(GEN, f"fig_wing_t_{v}.png"))
    _ = rows


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "score"
    {"flux": run_flux, "vace": run_vace, "score": score}[what]()
