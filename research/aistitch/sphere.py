"""Polar-axis sphere helpers shared by the depth and generative experiments.

Band convention (osv_kernel.h): a body direction d maps to
    lon = atan2(d.x, d.z),  lat = asin(d.y)
so d = (cos lat sin lon, sin lat, cos lat cos lon); +lat is the master lens's
axis (+Y body).  Column c of a W-wide map is lon = -pi + (c + 0.5) 2pi / W,
full-map row R is lat = pi/2 - (R + 0.5) pi / mapH.
"""
import numpy as np

from common import bilinear_sample


def dir_from_lonlat(lon, lat):
    cl = np.cos(lat)
    return np.stack([cl * np.sin(lon), np.sin(lat), cl * np.cos(lon)], axis=-1)


def lonlat_from_dir(d):
    lon = np.arctan2(d[..., 0], d[..., 2])
    lat = np.arcsin(np.clip(d[..., 1], -1.0, 1.0))
    return lon, lat


def basis_lonlat(lon, lat):
    """Unit vectors along +lon and +lat at each direction."""
    e_lon = np.stack([np.cos(lon), np.zeros_like(lon), -np.sin(lon)], axis=-1)
    e_lat = np.stack([-np.sin(lat) * np.sin(lon), np.cos(lat), -np.sin(lat) * np.cos(lon)], axis=-1)
    return e_lon, e_lat


class PerspectiveCrop:
    """A pinhole view centred on (lon_c, lat_c) with +lat up, square, N px."""

    def __init__(self, lon_c_deg, lat_c_deg=0.0, fov_deg=90.0, n=518):
        self.lon_c = np.radians(lon_c_deg)
        self.lat_c = np.radians(lat_c_deg)
        self.n = n
        self.fov = np.radians(fov_deg)
        self.f = (n / 2.0) / np.tan(self.fov / 2.0)
        fwd = dir_from_lonlat(np.array(self.lon_c), np.array(self.lat_c))
        e_lon, e_lat = basis_lonlat(np.array(self.lon_c), np.array(self.lat_c))
        self.F, self.R, self.U = fwd, e_lon, e_lat

    def rays(self):
        """(n, n, 3) unit rays for every pixel centre."""
        u = (np.arange(self.n) + 0.5 - self.n / 2.0) / self.f
        x, y = np.meshgrid(u, -u)                     # image y down -> +lat up
        d = self.F[None, None] + x[..., None] * self.R[None, None] + y[..., None] * self.U[None, None]
        return d / np.linalg.norm(d, axis=-1, keepdims=True)

    def project(self, d):
        """Directions -> (u, v) pixel coords and a validity mask."""
        z = d @ self.F
        x = (d @ self.R) / np.maximum(z, 1e-9)
        y = (d @ self.U) / np.maximum(z, 1e-9)
        u = x * self.f + self.n / 2.0 - 0.5
        v = -y * self.f + self.n / 2.0 - 0.5
        ok = (z > 1e-6) & (u >= 0) & (u <= self.n - 1) & (v >= 0) & (v <= self.n - 1)
        return u, v, ok

    def render_from_band(self, band_img, band_w, band_row0, map_h):
        """Sample a polar band image (rows band_row0.. of a map_h-high map)."""
        lon, lat = lonlat_from_dir(self.rays())
        col = (lon + np.pi) / (2 * np.pi) * band_w - 0.5
        row = (np.pi / 2 - lat) / np.pi * map_h - 0.5 - band_row0
        v, ok = bilinear_sample(band_img, col.astype(np.float32), row.astype(np.float32))
        return v, ok

    def sample_to_band(self, crop_img, lat_deg_rows, band_w):
        """Sample a crop-space image (n, n[, c]) at the directions of a band grid."""
        lon = -np.pi + (np.arange(band_w) + 0.5) * 2 * np.pi / band_w
        lat = np.radians(np.asarray(lat_deg_rows))
        L, A = np.meshgrid(lon, lat)
        u, v, ok = self.project(dir_from_lonlat(L, A))
        val, ok2 = bilinear_sample(crop_img, np.where(ok, u, 0).astype(np.float32),
                                   np.where(ok, v, 0).astype(np.float32))
        return val, ok & ok2
