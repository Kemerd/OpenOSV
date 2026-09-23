# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""Before/after sky seam crops for the photometric seam fix (WP-PHOTO).

The inputs are 16-bit Rec.709 TIFFs from osvtool, one per frame and variant,
all the same view: straight at the stitch seam where it crosses open sky on
the sample clip (stabilisation off, so the seam stays put in the body frame),
with the parallax grid (classical flow) and, for the "carved" set, WP-SEAM's
carved seam - the importer's default stitch, whose Rim cost is the field's
usable rim in stage 2:

    set VIEW=--mode reframe --yaw 90 --pitch 0 --fov 80 --size 1280x960 ^
             --color 709 --stab off --blend --occlusion --parallax --flow-backend classical
    rem "carved" set: add --seam-carve to VIEW; "feather" set: leave it out
    rem production before the fix: calibrated FOV, 4 deg feather, global gain
    osvtool render clip.OSV %VIEW% --gain --blend-fov 195.18 --blend-feather 4 --frame N --out sky_fNN_before.tif
    rem stage 1: the 2.6 deg seam edge inset, trusted-pixel gain
    osvtool render clip.OSV %VIEW% --gain --frame N --out sky_fNN_stage1.tif
    rem stage 2: per-longitude usable rim + 2-D gain field
    osvtool render clip.OSV %VIEW% --photo full --frame N --out sky_fNN_stage2.tif

for N = 0, 32, 64 (NN zero padded to two digits), then

    python research/photo/make_crops.py <dir with one set's TIFFs> <carved|feather>

Writes two lossless 8-bit TIFFs (deflate) next to this script -
sky_seam_<set>_f32.tif (frame 32 as delivered) and sky_seam_<set>_stretched.tif
(frames 0 / 32 / 64, contrast x4) - and prints, per frame and variant, the
seam's column profile roughness in 8-bit codes.  Windows console safe (ASCII
output only).  Needs numpy and opencv-python.
"""
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = (0, 32, 64)
NATURAL_FRAME = 32
VARIANTS = (("before", "production"), ("stage1", "stage 1: inset"), ("stage2", "stage 2: rim + gain field"))

# The crop: the middle of the 1280 x 960 view, where the seam runs top to
# bottom, 400 x 600 pixels (about 25 x 38 degrees of sky).
CROP_X0, CROP_X1 = 440, 840
CROP_Y0, CROP_Y1 = 180, 780

# Contrast stretch for the second image: every panel of a frame gets the SAME
# mapping (taken from the production panel), so the three stay comparable.
STRETCH = 4.0


def read16(path):
    """16-bit TIFF -> float32 in [0, 1] (BGR, as OpenCV reads it)."""
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None or img.dtype != np.uint16:
        raise SystemExit('cannot read a 16-bit TIFF from ' + path)
    return img.astype(np.float32) / 65535.0


def to8(img):
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def label(img, text):
    """Burn a small caption into the top-left corner."""
    out = img.copy()
    cv2.putText(out, text, (10, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(out, text, (10, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
    return out


def save(name, img):
    """Lossless TIFF: deflate with the horizontal predictor (smooth sky packs well)."""
    path = os.path.join(HERE, name)
    ok = cv2.imwrite(path, img, [cv2.IMWRITE_TIFF_COMPRESSION, cv2.IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE,
                                 cv2.IMWRITE_TIFF_PREDICTOR, cv2.IMWRITE_TIFF_PREDICTOR_HORIZONTAL])
    if not ok:
        raise SystemExit('cannot write ' + path)
    print('wrote %s (%d KB)' % (path, os.path.getsize(path) // 1024))


def profile_roughness(crop):
    """RMS of the column-mean luma minus its 60-pixel Gaussian, in 8-bit codes.

    The seam band runs along the columns, so averaging down the rows keeps it
    and averages the sky's noise away; the wide Gaussian removes the sky's
    own smooth gradient.  What is left is the band and line a viewer sees.
    """
    luma = 0.0722 * crop[:, :, 0] + 0.7152 * crop[:, :, 1] + 0.2126 * crop[:, :, 2]
    prof = luma.mean(axis=0).astype(np.float64) * 255.0
    smooth = cv2.GaussianBlur(prof.reshape(1, -1), (0, 0), sigmaX=60.0, borderType=cv2.BORDER_REFLECT).ravel()
    return float(np.sqrt(np.mean((prof - smooth) ** 2)))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    tag = sys.argv[2]
    if tag not in ('carved', 'feather'):
        raise SystemExit('the set must be carved or feather')
    natural = None
    stretched_rows = []
    for f in FRAMES:
        crops = []
        for key, _ in VARIANTS:
            img = read16(os.path.join(d, 'sky_f%02d_%s.tif' % (f, key)))
            crops.append(img[CROP_Y0:CROP_Y1, CROP_X0:CROP_X1, :])
        # ---- roughness, printed for the write-up ------------------------------
        base = profile_roughness(crops[0])
        line = '%s frame %2d seam profile roughness (8-bit codes):' % (tag, f)
        for (key, _), c in zip(VARIANTS, crops):
            r = profile_roughness(c)
            line += '  %s %.2f (x%.2f)' % (key, r, r / base if base > 0 else 0.0)
        print(line)
        # ---- natural (one frame) and stretched (all) panels ------------------
        centre = np.median(crops[0].reshape(-1, 3), axis=0)
        stre = []
        for (key, caption), c in zip(VARIANTS, crops):
            s = 0.5 + STRETCH * (c - centre)
            stre.append(label(to8(s), 'f%d %s, contrast x%d' % (f, caption, int(STRETCH))))
        stretched_rows.append(np.hstack(stre))
        if f == NATURAL_FRAME:
            natural = np.hstack([label(to8(c), 'f%d %s' % (f, caption)) for (key, caption), c in zip(VARIANTS, crops)])
    save('sky_seam_%s_f%02d.tif' % (tag, NATURAL_FRAME), natural)
    save('sky_seam_%s_stretched.tif' % tag, np.vstack(stretched_rows))


if __name__ == '__main__':
    main()
