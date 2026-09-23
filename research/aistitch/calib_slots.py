"""Dump every lens calibration slot of an .OSV clip and derive the lens baseline
DJI's own stitching-distance presets imply (research only).

The calibration schema records rotations (cam_extri_q) and intrinsics, never a
translation.  But the clip carries six "far_XX" presets (stitching distance
0.7 .. 1.6 m) that differ from native_refine only in focal length and a tiny
centre shift.  For back-to-back lenses whose centres sit a distance a off the
rig centre along their own axes, a point at distance d near the seam
(theta ~ 90 deg) is seen a/d radians further off-axis than from the rig
centre.  A focal change of df reproduces that shift where

    a / d = (df / f) * g(theta) / g'(theta),   r = f * g(theta)  (KB5 model)

so the presets encode a per-distance parallax, and a straight-line fit of
a/d against 1/d gives a (the half baseline, if both lenses are symmetric).

Usage:
    osvtool probe CLIP --json probe.json --raw
    python calib_slots.py probe.json [out.json]
"""
import json
import math
import sys

# Slot numbers of PanoDewarpParams (proto/dvtm_osmo360.proto)
SLOT_NAMES = {
    1: "native_refine_slave", 2: "native_refine_master",
    3: "native_refine_far_slave", 4: "native_refine_far_master",
    5: "lens_guards_slave", 6: "lens_guards_master",
    11: "native_slave", 12: "native_master",
    13: "far_07_slave", 14: "far_07_master",
    15: "far_09_slave", 16: "far_09_master",
    17: "far_11_slave", 18: "far_11_master",
    19: "far_12_5_slave", 20: "far_12_5_master",
    21: "far_14_slave", 22: "far_14_master",
    23: "far_16_slave", 24: "far_16_master",
}
# Stitching distance (m) of each far preset
FAR_DIST = {13: 0.7, 15: 0.9, 17: 1.1, 19: 1.25, 21: 1.4, 23: 1.6}


def find_pano_dewarp(raw):
    """Locate the PanoDewarpParams node: a message holding fields 13, 23 and 24."""
    hits = []

    def walk(node):
        kids = node.get("children") or []
        fields = {k.get("field") for k in kids}
        if {13, 23, 24} <= fields:
            hits.append(node)
        for k in kids:
            walk(k)

    for top in raw.get("fields", []):
        walk(top)
    return hits


def slot_values(node):
    """Field number -> float / list of floats for one DewarpParams record."""
    vals = {}
    for c in node.get("children") or []:
        f = c.get("field")
        if "float" in c:
            vals[f] = c["float"]
        elif "asFloats" in c:
            vals[f] = c["asFloats"]
        elif c.get("children"):
            vals[f] = [x.get("float") for x in c["children"] if "float" in x]
    return vals


def kb5(theta, k):
    """Kannala-Brandt 5-term radius over focal, and its derivative."""
    t2 = theta * theta
    poly = 1.0 + k[0] * t2 + k[1] * t2**2 + k[2] * t2**3 + k[3] * t2**4 + k[4] * t2**5
    dpoly = 1.0 + 3 * k[0] * t2 + 5 * k[1] * t2**2 + 7 * k[2] * t2**3 + 9 * k[3] * t2**4 + 11 * k[4] * t2**5
    return theta * poly, dpoly


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        probe = json.load(f)
    hits = find_pano_dewarp(probe.get("rawSample0", {}))
    if not hits:
        print("no PanoDewarpParams node found (was the probe run with --raw?)")
        return 1
    slots = {}
    for c in hits[0].get("children", []):
        v = slot_values(c)
        if 1 in v and v[1]:
            slots[c["field"]] = v
    print("slot  name                      fx          fy          cx          cy")
    for s in sorted(slots):
        v = slots[s]
        print(f"{s:4d}  {SLOT_NAMES.get(s, '?'):24s} {v[1]:11.4f} {v[2]:11.4f} {v[3]:11.4f} {v[4]:11.4f}")

    # ---- parallax implied by each far preset, relative to native_refine ----
    out = {"slots": {str(k): v for k, v in slots.items()}, "presets": []}
    theta = math.radians(90.0)
    rows = []
    for far, dist in sorted(FAR_DIST.items()):
        for lens, ref_slot, slot in ((0, 1, far), (1, 2, far + 1)):
            if slot not in slots or ref_slot not in slots:
                continue
            ref, cur = slots[ref_slot], slots[slot]
            f0 = 0.5 * (ref[1] + ref[2])
            f1 = 0.5 * (cur[1] + cur[2])
            k = [ref.get(i, 0.0) for i in (5, 6, 7, 8, 15)]
            g, dpoly = kb5(theta, k)
            gprime = dpoly
            shift = (f1 - f0) / f0 * g / gprime  # radians at theta = 90 deg
            rows.append((dist, lens, f1 - f0, shift))
            out["presets"].append({"distM": dist, "lens": lens, "dfPx": f1 - f0, "shiftRad": shift})
    print("\ndist(m) lens  df(px)   shift at 90deg (deg)   shift*d (mm)")
    for dist, lens, df, sh in rows:
        print(f"{dist:6.2f}  {lens:3d}  {df:+8.4f}   {math.degrees(sh):+10.5f}          {sh * dist * 1000:+8.2f}")

    # ---- least-squares fit shift = a / d + c per lens -----------------------
    for lens in (0, 1):
        pts = [(1.0 / d, s) for d, l, _, s in rows if l == lens]
        if len(pts) < 2:
            continue
        n = len(pts)
        sx = sum(p[0] for p in pts)
        sy = sum(p[1] for p in pts)
        sxx = sum(p[0] ** 2 for p in pts)
        sxy = sum(p[0] * p[1] for p in pts)
        a = (n * sxy - sx * sy) / (n * sxx - sx * sx)
        c = (sy - a * sx) / n
        resid = max(abs(p[1] - (a * p[0] + c)) for p in pts)
        print(f"lens {lens}: shift = {a * 1000:+.3f} mm / d  {math.degrees(c):+.5f} deg   (max resid {math.degrees(resid):.2e} deg)")
        out[f"lens{lens}_a_mm"] = a * 1000.0
        out[f"lens{lens}_c_deg"] = math.degrees(c)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
