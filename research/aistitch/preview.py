"""Write tone-mapped PNG previews of bandprobe bands (research only).

    python preview.py <bandprobe dir> <frame> <out prefix> [col0 col1] [halfDeg] [scale]
"""
import sys

import numpy as np
from PIL import Image

from common import ProbeBands, tonemap_preview, utf8_console

utf8_console()


def main():
    d, frame, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    pb = ProbeBands(d)
    c0 = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    c1 = int(sys.argv[5]) if len(sys.argv) > 5 else pb.w
    half = float(sys.argv[6]) if len(sys.argv) > 6 else 90.0
    scale = float(sys.argv[7]) if len(sys.argv) > 7 else 1.0
    rows = pb.rows_for(half)
    tiles = []
    for tag in ("lens0", "lens1", "blend"):
        a = pb.load(frame, tag)[rows, c0:c1]
        rgb = tonemap_preview(a[..., :3], 2.0)
        if tag != "blend":
            rgb = (rgb * (a[..., 3:4] > 0.01)).astype(np.uint8)
        tiles.append(rgb)
        tiles.append(np.full((4, rgb.shape[1], 3), 255, np.uint8))
    img = Image.fromarray(np.concatenate(tiles, 0))
    if scale != 1.0:
        img = img.resize((int(img.width * scale), int(img.height * scale)), Image.LANCZOS)
    img.save(out + ".png")
    print("wrote", out + ".png", img.size)


if __name__ == "__main__":
    main()
