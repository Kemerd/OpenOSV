"""Shared loaders for the bandprobe output (research only).

Every band file is RGBA float32, h x w, scene-linear light; theta.f32 holds
(theta0, theta1) in degrees per pixel.  Latitude of band row y is
90 - (row0 + y + 0.5) * 180 / mapH (polar-axis layout: +lat towards lens 0).
"""
import json
import os

import numpy as np

BANDS = os.environ.get(
    "OSV_BANDS",
    r"C:\Users\Donny\AppData\Local\Temp\claude\L--Dev-premiere-360-reframe"
    r"\68ebcb6f-1d24-4bec-9dbe-67a1d16f6c29\scratchpad\bands",
)


def meta():
    """Band geometry written by bandprobe."""
    with open(os.path.join(BANDS, "meta.json"), "r", encoding="utf-8") as f:
        return json.load(f)


def load(frame, tag):
    """One RGBA band as float32 (h, w, 4)."""
    m = meta()
    a = np.fromfile(os.path.join(BANDS, f"f{frame}_{tag}.f32"), dtype=np.float32)
    return a.reshape(m["h"], m["w"], 4)


def theta():
    """(h, w, 2) angles from lens 0 / lens 1 axes, degrees."""
    m = meta()
    a = np.fromfile(os.path.join(BANDS, "theta.f32"), dtype=np.float32)
    return a.reshape(m["h"], m["w"], 2)


def lat_deg():
    """Latitude of every band row, degrees (+ = towards lens 0)."""
    m = meta()
    rows = np.arange(m["h"], dtype=np.float64) + m["row0"] + 0.5
    return 90.0 - rows * 180.0 / m["mapH"]


def tonemap(rgb, exposure=1.0):
    """Simple display mapping for previews: Reinhard + sRGB-ish gamma."""
    x = np.clip(rgb * exposure, 0.0, None)
    x = x / (1.0 + x)
    return np.clip(x ** (1.0 / 2.2), 0.0, 1.0)


def luma(rgb):
    """Rec.2020 luma weights (working space of the linear output)."""
    return 0.2627 * rgb[..., 0] + 0.6780 * rgb[..., 1] + 0.0593 * rgb[..., 2]
