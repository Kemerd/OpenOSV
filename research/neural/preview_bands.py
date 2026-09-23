"""Write PNG previews of the probe bands (blend / lens0 / lens1) for eyeballing.

Usage: python preview_bands.py <frame> <outdir> [exposure]
The 4096-wide band is split into two 2048-wide halves stacked vertically.
"""
import os
import sys

import cv2
import numpy as np

import bandio


def main():
    frame = int(sys.argv[1])
    out = sys.argv[2]
    exposure = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    os.makedirs(out, exist_ok=True)
    tiles = []
    for tag in ("blend", "lens0", "lens1"):
        b = bandio.load(frame, tag)
        img = bandio.tonemap(b[..., :3], exposure)
        w = img.shape[1] // 2
        tiles.append(np.concatenate([img[:, :w], img[:, w:]], axis=0))
        # thin separator
        tiles.append(np.ones((6, w, 3), np.float32))
    pic = np.concatenate(tiles, axis=0)
    bgr = (pic[..., ::-1] * 255.0 + 0.5).astype(np.uint8)
    path = os.path.join(out, f"preview_f{frame}.png")
    cv2.imwrite(path, bgr)
    print(path, pic.shape)


if __name__ == "__main__":
    main()
