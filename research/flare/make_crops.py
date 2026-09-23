# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
"""Before/after crops for docs/research/FLARE.md from osv_flare_bench output.

Usage (Windows console safe, ASCII output only):

    osv_flare_bench.exe --out <dir>
    python research/flare/make_crops.py <dir>

Reads the 16-bit Rec.709 TIFFs the bench writes (<view>_before.tif,
<view>_after.tif, <view>_ghostmask.tif, equirect_veil_after.tif) and writes
JPEG crops next to this script.  Needs numpy and opencv-python.
"""
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def read16(path):
    """16-bit TIFF -> float32 in [0, 1] (BGR, as OpenCV reads it)."""
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit('cannot read ' + path)
    return img.astype(np.float32) / 65535.0


def to8(img):
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def stretch(img, lo, hi):
    return np.clip((img - lo) / max(hi - lo, 1e-6), 0.0, 1.0)


def label(img, text):
    """Burn a small caption into the top-left corner."""
    out = img.copy()
    cv2.putText(out, text, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(out, text, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 1, cv2.LINE_AA)
    return out


def save(name, img):
    path = os.path.join(HERE, name)
    cv2.imwrite(path, img, [cv2.IMWRITE_JPEG_QUALITY, 90])
    print('wrote', path)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]

    # ---- 1. the sun view (100 deg), native pixels, as an editor sees it --------
    a = read16(os.path.join(d, 'sun100_before.tif'))
    b = read16(os.path.join(d, 'sun100_after.tif'))
    x0, y0, x1, y1 = 250, 163, 1150, 753
    ca, cb = a[y0:y1, x0:x1], b[y0:y1, x0:x1]
    save('sun_view_before_after.jpg', np.hstack([label(to8(ca), 'before'), label(to8(cb), 'ghosts removed')]))

    # ---- 2. the same, contrast stretched so the faint ghosts show ---------------
    lo, hi = np.percentile(ca, 2), np.percentile(ca, 90)
    save('sun_view_stretched.jpg',
         np.hstack([label(to8(stretch(ca, lo, hi)), 'before (stretched)'),
                    label(to8(stretch(cb, lo, hi)), 'after (stretched)')]))

    # ---- 3. exactly what was taken away, x8 -------------------------------------
    diff = np.abs(a - b) * 8.0
    save('sun_view_removed_x8.jpg', label(to8(diff[y0:y1, x0:x1]), 'removed light x8'))

    # ---- 4. the pill ghost close up (30 deg view) -------------------------------
    a = read16(os.path.join(d, 'ghost30_before.tif'))
    b = read16(os.path.join(d, 'ghost30_after.tif'))
    m = read16(os.path.join(d, 'ghost30_ghostmask.tif'))[..., 0]
    ys, xs = np.nonzero(m > 0.01)
    pad = 120
    x0, x1 = max(0, xs.min() - pad), min(a.shape[1], xs.max() + pad)
    y0, y1 = max(0, ys.min() - pad), min(a.shape[0], ys.max() + pad)
    save('pill_closeup_before_after.jpg',
         np.hstack([label(to8(a[y0:y1, x0:x1]), 'before'), label(to8(b[y0:y1, x0:x1]), 'after')]))

    # ---- 5. the equirect sky: ghosts, then ghosts + overlap veil (research) -----
    a = read16(os.path.join(d, 'equirect_before.tif'))
    b = read16(os.path.join(d, 'equirect_after.tif'))
    veil_path = os.path.join(d, 'equirect_veil_after.tif')
    tiles = [label(to8(a), 'before'), label(to8(b), 'ghosts removed')]
    if os.path.exists(veil_path):
        v = read16(veil_path)
        tiles.append(label(to8(v), '+ overlap veil (research only)'))
    # The sky hemisphere: longitude -180..+10 deg, the full height, half size.
    W = a.shape[1]
    xs0, xs1 = 0, int(W * 0.53)
    sky = [cv2.resize(t[:, xs0:xs1], None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA) for t in tiles]
    save('equirect_sky_before_after.jpg', np.vstack(sky))


if __name__ == '__main__':
    main()
