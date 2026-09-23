# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""Before/after figures and seam-crossing profiles for the lens shading
correction (WP-VIGNETTE; docs/research/NEURAL_STITCHING.md, section 9).

The inputs are osvtool renders of the sample clip with the importer's default
stitch (parallax grid, carved seam, photometric field) and the correction off
or on, frames 0 / 32 / 64:

    set STITCH=--parallax --seam-search --seam-carve --gain --photo full --flow-backend classical
    rem the polar map (lens axes at the poles, the seam on the equator), scene-linear
    osvtool render clip.OSV --frame N --mode equirect-polar --size 4096x2048 --color linear ^
        --stab off %STITCH% --shading off|auto --out polar_fNN_off|auto.exr
    rem the wide view toward the sun that crosses both seam lines (stereographic, 200 deg),
    rem horizon lock, the Rec.709 DJI look - the user's kind of view
    osvtool render clip.OSV --frame N --mode reframe --proj eye-offset --distortion 1 --fov 200 ^
        --yaw 0 --pitch 15 --size 1920x1080 --color 709 --stab horizon %STITCH% ^
        --shading off|auto --out wide_fNN_off|auto.tif

(NN zero padded to two digits), then

    python research/vignette/make_figures.py <dir with those files>

Writes, next to this script:

  * wide_sun_view_f32.tif      frame 32 as delivered, off above, on below;
  * seam_crossings_x4.tif      the two sky seam crossings of the wide view,
                               frames 0 / 32 / 64, off | on, each panel's
                               brightness around its own local mean x4;
  * seam_profiles.csv          the polar map's log2 luma profile across the
                               seam (median over the open-sky columns,
                               0.20w-0.44w) and its deviation from the sky's
                               own trend, per frame, off and on;

and prints, per frame, the seam's dip below the sky trend and the same
statistic in open sky away from the seam (millistops).  Lossless 8-bit TIFFs
(deflate), ASCII console output.  Needs numpy and opencv-python (OpenEXR
support is switched on below).
"""
import os
import sys

os.environ["OPENCV_IO_ENABLE_OPENEXR"] = "1"

import cv2  # noqa: E402  (after the environment switch)
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = (0, 32, 64)
LUMA = np.array([0.2627, 0.6780, 0.0593])

# The two sky seam crossings of the 1920 x 1080 wide view: the seam is the
# circle 90 deg from the master lens axis, at the left and right of the view.
CROSSINGS = (("left", 0, 460, 60, 560), ("right", 1460, 1920, 60, 560))
STRETCH = 4.0


def read_exr(path):
    """Linear EXR -> float64 RGB."""
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit("cannot read " + path)
    return img[..., 2::-1].astype(np.float64) if img.shape[2] >= 3 else img.astype(np.float64)


def read16(path):
    """16-bit TIFF -> float32 in [0, 1] (BGR, as OpenCV reads it)."""
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None or img.dtype != np.uint16:
        raise SystemExit("cannot read a 16-bit TIFF from " + path)
    return img[..., :3].astype(np.float32) / 65535.0


def save(name, img):
    """Lossless TIFF: deflate with the horizontal predictor (smooth sky packs well)."""
    path = os.path.join(HERE, name)
    ok = cv2.imwrite(path, img, [cv2.IMWRITE_TIFF_COMPRESSION, cv2.IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE,
                                 cv2.IMWRITE_TIFF_PREDICTOR, cv2.IMWRITE_TIFF_PREDICTOR_HORIZONTAL])
    if not ok:
        raise SystemExit("cannot write " + path)
    print("wrote %s (%d KB)" % (path, os.path.getsize(path) // 1024))


def label(img, text):
    """Burn a small caption into the top-left corner."""
    out = img.copy()
    cv2.putText(out, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(out, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    return out


def to8(img):
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def stretched(panel, reference):
    """Contrast x STRETCH around the reference panel's heavily blurred local
    mean, the same mapping for every panel of a crossing."""
    mean = cv2.GaussianBlur(reference, (0, 0), 40)
    return np.clip(0.5 + (panel - mean) * STRETCH + (mean - 0.5) * 0.25, 0.0, 1.0)


def profiles(path):
    """Median log2 luma over the open-sky columns per polar latitude, and the
    deviation from the sky trend (a quadratic fitted outside +-10 deg)."""
    img = read_exr(path)
    h, w = img.shape[:2]
    lat = 90.0 - (np.arange(h) + 0.5) * 180.0 / h
    y = np.log2(np.maximum(img @ LUMA, 1e-6))
    block = max(1, w // 128)
    cols = range(int(0.20 * w), int(0.44 * w) - block + 1, block)
    blocks = np.array([y[:, c:c + block].mean(axis=1) for c in cols])
    prof = np.median(blocks, axis=0)

    def depth(fit, ev):
        dips = []
        for b in blocks:
            p = np.polyfit(lat[fit], b[fit], 2)
            dips.append(np.max(np.polyval(p, lat[ev]) - b[ev]))
        return 1000.0 * float(np.median(dips))

    fit = ((lat >= -22) & (lat <= -10)) | ((lat >= 10) & (lat <= 22))
    seam = depth(fit, (lat >= -2) & (lat <= 8))
    sky = depth(((lat >= -29) & (lat <= -24)) | ((lat >= -11) & (lat <= -4)), (lat >= -23) & (lat <= -12))
    trend = np.polyval(np.polyfit(lat[fit], prof[fit], 2), lat)
    return lat, prof, prof - trend, seam, sky


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]

    # ---- the profiles and the numbers ---------------------------------------------
    rows = {}
    print("frame   seam dip off -> on   open-sky dip off -> on   (millistops below the sky trend)")
    for f in FRAMES:
        for v in ("off", "auto"):
            lat, prof, dev, seam, sky = profiles(os.path.join(src, "polar_f%02d_%s.exr" % (f, v)))
            rows[(f, v)] = (prof, dev, seam, sky)
        a, b = rows[(f, "off")], rows[(f, "auto")]
        print("%5d   %7.1f -> %6.1f        %7.1f -> %6.1f" % (f, a[2], b[2], a[3], b[3]))
    keep = np.abs(lat) <= 30.0
    with open(os.path.join(HERE, "seam_profiles.csv"), "w", newline="\n") as out:
        head = ["lat_deg"]
        for f in FRAMES:
            head += ["f%02d_off_log2" % f, "f%02d_on_log2" % f, "f%02d_off_dev" % f, "f%02d_on_dev" % f]
        out.write(",".join(head) + "\n")
        for i in np.nonzero(keep)[0]:
            vals = ["%.3f" % lat[i]]
            for f in FRAMES:
                vals += ["%.5f" % rows[(f, "off")][0][i], "%.5f" % rows[(f, "auto")][0][i],
                         "%.5f" % rows[(f, "off")][1][i], "%.5f" % rows[(f, "auto")][1][i]]
            out.write(",".join(vals) + "\n")
    print("wrote %s" % os.path.join(HERE, "seam_profiles.csv"))

    # ---- the wide view, frame 32, as delivered ----------------------------------------
    off32 = read16(os.path.join(src, "wide_f32_off.tif"))
    on32 = read16(os.path.join(src, "wide_f32_auto.tif"))
    half = lambda im: cv2.resize(im, (im.shape[1] // 2, im.shape[0] // 2), interpolation=cv2.INTER_AREA)
    save("wide_sun_view_f32.tif", np.concatenate([label(to8(half(off32)), "frame 32, lens shading off"),
                                                   label(to8(half(on32)), "frame 32, lens shading auto")], axis=0))

    # ---- the two crossings, stretched -----------------------------------------------------
    strips = []
    for f in FRAMES:
        off = read16(os.path.join(src, "wide_f%02d_off.tif" % f))
        on = read16(os.path.join(src, "wide_f%02d_auto.tif" % f))
        panels = []
        for name, x0, x1, y0, y1 in CROSSINGS:
            ref = off[y0:y1, x0:x1]
            panels.append(label(to8(stretched(ref, ref)), "f%d %s off" % (f, name)))
            panels.append(label(to8(stretched(on[y0:y1, x0:x1], ref)), "f%d %s on" % (f, name)))
        strips.append(np.concatenate(panels, axis=1))
    save("seam_crossings_x4.tif", np.concatenate(strips, axis=0))


if __name__ == "__main__":
    main()
