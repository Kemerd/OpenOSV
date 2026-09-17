// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Small double-precision linear algebra used on the host: 2/3-vectors, 3x3
// matrices and unit quaternions.  All calibration and camera math is done in
// double and only converted to float when packed for the render kernels.
//
// Conventions (see docs/GEOMETRY.md):
//   * Vectors are column vectors; Mat3d * Vec3d applies the matrix.
//   * Mat3d is row-major: m[row * 3 + col].
//   * Quatd is (w, x, y, z) internally; use the named factories when the
//     source data uses a different component order.
//   * Rotations are right-handed; angles in radians unless the name says Deg.
#pragma once

#include <cmath>
#include <cstddef>
#include <algorithm>

namespace osv {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr double kHalfPi = 0.5 * kPi;

/// Degrees to radians.
[[nodiscard]] constexpr double deg2rad(double deg) noexcept { return deg * (kPi / 180.0); }
/// Radians to degrees.
[[nodiscard]] constexpr double rad2deg(double rad) noexcept { return rad * (180.0 / kPi); }

/// Clamp helper that tolerates lo > hi (returns lo) instead of being UB.
[[nodiscard]] constexpr double clampd(double v, double lo, double hi) noexcept {
    if (hi < lo) {
        return lo;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

// -----------------------------------------------------------------------------
//  Vec2d
// -----------------------------------------------------------------------------
struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2d() noexcept = default;
    constexpr Vec2d(double px, double py) noexcept : x(px), y(py) {}

    constexpr Vec2d operator+(const Vec2d& o) const noexcept { return {x + o.x, y + o.y}; }
    constexpr Vec2d operator-(const Vec2d& o) const noexcept { return {x - o.x, y - o.y}; }
    constexpr Vec2d operator*(double s) const noexcept { return {x * s, y * s}; }
    constexpr Vec2d operator/(double s) const noexcept { return {x / s, y / s}; }

    [[nodiscard]] constexpr double dot(const Vec2d& o) const noexcept { return x * o.x + y * o.y; }
    [[nodiscard]] double norm() const noexcept { return std::sqrt(x * x + y * y); }
};

// -----------------------------------------------------------------------------
//  Vec3d / Vec3f
// -----------------------------------------------------------------------------
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3d() noexcept = default;
    constexpr Vec3d(double px, double py, double pz) noexcept : x(px), y(py), z(pz) {}

    constexpr Vec3d operator+(const Vec3d& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3d operator-(const Vec3d& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3d operator-() const noexcept { return {-x, -y, -z}; }
    constexpr Vec3d operator*(double s) const noexcept { return {x * s, y * s, z * s}; }
    constexpr Vec3d operator/(double s) const noexcept { return {x / s, y / s, z / s}; }
    Vec3d& operator+=(const Vec3d& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    [[nodiscard]] constexpr double dot(const Vec3d& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3d cross(const Vec3d& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] double norm() const noexcept { return std::sqrt(dot(*this)); }

    /// Unit vector; returns (0,0,0) for a zero input instead of NaN.
    [[nodiscard]] Vec3d normalized() const noexcept {
        const double n = norm();
        if (n <= 0.0 || !std::isfinite(n)) {
            return {};
        }
        return *this / n;
    }

    /// Angle between two vectors in radians, robust to rounding (clamped acos).
    [[nodiscard]] double angleTo(const Vec3d& o) const noexcept {
        const double denom = norm() * o.norm();
        if (denom <= 0.0) {
            return 0.0;
        }
        return std::acos(clampd(dot(o) / denom, -1.0, 1.0));
    }

    [[nodiscard]] bool isFinite() const noexcept { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3f() noexcept = default;
    constexpr Vec3f(float px, float py, float pz) noexcept : x(px), y(py), z(pz) {}
    constexpr explicit Vec3f(const Vec3d& v) noexcept
        : x(static_cast<float>(v.x)), y(static_cast<float>(v.y)), z(static_cast<float>(v.z)) {}

    [[nodiscard]] constexpr Vec3d toDouble() const noexcept { return {x, y, z}; }
};

// -----------------------------------------------------------------------------
//  Mat3d (row-major)
// -----------------------------------------------------------------------------
struct Mat3d {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    constexpr Mat3d() noexcept = default;
    constexpr Mat3d(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21,
                    double m22) noexcept
        : m{m00, m01, m02, m10, m11, m12, m20, m21, m22} {}

    static constexpr Mat3d identity() noexcept { return Mat3d{}; }

    /// Rotation about +X by `rad` (right-handed).
    static Mat3d rotX(double rad) noexcept {
        const double c = std::cos(rad), s = std::sin(rad);
        return {1, 0, 0, 0, c, -s, 0, s, c};
    }
    /// Rotation about +Y by `rad`.
    static Mat3d rotY(double rad) noexcept {
        const double c = std::cos(rad), s = std::sin(rad);
        return {c, 0, s, 0, 1, 0, -s, 0, c};
    }
    /// Rotation about +Z by `rad`.
    static Mat3d rotZ(double rad) noexcept {
        const double c = std::cos(rad), s = std::sin(rad);
        return {c, -s, 0, s, c, 0, 0, 0, 1};
    }

    /// Rotation about an arbitrary unit axis (Rodrigues).  A zero axis yields identity.
    static Mat3d axisAngle(const Vec3d& axisIn, double rad) noexcept {
        const Vec3d a = axisIn.normalized();
        if (a.norm() == 0.0) {
            return identity();
        }
        const double c = std::cos(rad), s = std::sin(rad), t = 1.0 - c;
        return {t * a.x * a.x + c,       t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y,
                t * a.x * a.y + s * a.z, t * a.y * a.y + c,       t * a.y * a.z - s * a.x,
                t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c};
    }

    /// Build from three column vectors (the images of the basis vectors).
    static constexpr Mat3d fromColumns(const Vec3d& c0, const Vec3d& c1, const Vec3d& c2) noexcept {
        return {c0.x, c1.x, c2.x, c0.y, c1.y, c2.y, c0.z, c1.z, c2.z};
    }

    [[nodiscard]] constexpr double at(int row, int col) const noexcept { return m[row * 3 + col]; }
    constexpr double& at(int row, int col) noexcept { return m[row * 3 + col]; }

    [[nodiscard]] constexpr Vec3d row(int r) const noexcept { return {m[r * 3], m[r * 3 + 1], m[r * 3 + 2]}; }
    [[nodiscard]] constexpr Vec3d col(int c) const noexcept { return {m[c], m[3 + c], m[6 + c]}; }

    constexpr Vec3d operator*(const Vec3d& v) const noexcept {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }

    constexpr Mat3d operator*(const Mat3d& o) const noexcept {
        Mat3d r;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.m[i * 3 + j] = m[i * 3] * o.m[j] + m[i * 3 + 1] * o.m[3 + j] + m[i * 3 + 2] * o.m[6 + j];
            }
        }
        return r;
    }

    [[nodiscard]] constexpr Mat3d transposed() const noexcept {
        return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
    }

    [[nodiscard]] constexpr double determinant() const noexcept {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    }

    /// Frobenius norm of (this - other), handy for orthonormality tests.
    [[nodiscard]] double distance(const Mat3d& o) const noexcept {
        double s = 0.0;
        for (int i = 0; i < 9; ++i) {
            const double d = m[i] - o.m[i];
            s += d * d;
        }
        return std::sqrt(s);
    }

    /// Copy into a float[9] (row-major) for the render kernels.
    void toFloat9(float out[9]) const noexcept {
        if (!out) {
            return;
        }
        for (int i = 0; i < 9; ++i) {
            out[i] = static_cast<float>(m[i]);
        }
    }
};

// -----------------------------------------------------------------------------
//  Quatd (w, x, y, z)
// -----------------------------------------------------------------------------
struct Quatd {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Quatd() noexcept = default;
    constexpr Quatd(double qw, double qx, double qy, double qz) noexcept : w(qw), x(qx), y(qy), z(qz) {}

    /// Identity rotation.
    static constexpr Quatd identity() noexcept { return Quatd{}; }

    /// Factories that make the component order explicit at the call site.
    static constexpr Quatd fromWXYZ(double qw, double qx, double qy, double qz) noexcept { return {qw, qx, qy, qz}; }
    static constexpr Quatd fromXYZW(double qx, double qy, double qz, double qw) noexcept { return {qw, qx, qy, qz}; }

    /// Unit quaternion from axis (normalised internally) and angle.
    static Quatd fromAxisAngle(const Vec3d& axisIn, double rad) noexcept {
        const Vec3d a = axisIn.normalized();
        const double h = 0.5 * rad;
        const double s = std::sin(h);
        return {std::cos(h), a.x * s, a.y * s, a.z * s};
    }

    /// Quaternion from a rotation matrix (Shepperd's method, numerically safe).
    static Quatd fromMatrix(const Mat3d& r) noexcept {
        const double tr = r.m[0] + r.m[4] + r.m[8];
        Quatd q;
        if (tr > 0.0) {
            const double s = std::sqrt(tr + 1.0) * 2.0;
            q.w = 0.25 * s;
            q.x = (r.m[7] - r.m[5]) / s;
            q.y = (r.m[2] - r.m[6]) / s;
            q.z = (r.m[3] - r.m[1]) / s;
        } else if (r.m[0] > r.m[4] && r.m[0] > r.m[8]) {
            const double s = std::sqrt(1.0 + r.m[0] - r.m[4] - r.m[8]) * 2.0;
            q.w = (r.m[7] - r.m[5]) / s;
            q.x = 0.25 * s;
            q.y = (r.m[1] + r.m[3]) / s;
            q.z = (r.m[2] + r.m[6]) / s;
        } else if (r.m[4] > r.m[8]) {
            const double s = std::sqrt(1.0 + r.m[4] - r.m[0] - r.m[8]) * 2.0;
            q.w = (r.m[2] - r.m[6]) / s;
            q.x = (r.m[1] + r.m[3]) / s;
            q.y = 0.25 * s;
            q.z = (r.m[5] + r.m[7]) / s;
        } else {
            const double s = std::sqrt(1.0 + r.m[8] - r.m[0] - r.m[4]) * 2.0;
            q.w = (r.m[3] - r.m[1]) / s;
            q.x = (r.m[2] + r.m[6]) / s;
            q.y = (r.m[5] + r.m[7]) / s;
            q.z = 0.25 * s;
        }
        return q.normalized();
    }

    [[nodiscard]] constexpr double dot(const Quatd& o) const noexcept { return w * o.w + x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] double norm() const noexcept { return std::sqrt(dot(*this)); }

    /// Unit quaternion; identity for a zero input.
    [[nodiscard]] Quatd normalized() const noexcept {
        const double n = norm();
        if (n <= 0.0 || !std::isfinite(n)) {
            return identity();
        }
        return {w / n, x / n, y / n, z / n};
    }

    /// Conjugate (= inverse for unit quaternions).
    [[nodiscard]] constexpr Quatd conj() const noexcept { return {w, -x, -y, -z}; }

    /// Hamilton product: (this * o) applies o first, then this.
    [[nodiscard]] constexpr Quatd operator*(const Quatd& o) const noexcept {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }

    /// Rotate a vector: v' = q v q*.
    [[nodiscard]] Vec3d rotate(const Vec3d& v) const noexcept {
        const Vec3d qv{x, y, z};
        const Vec3d t = qv.cross(v) * 2.0;
        return v + t * w + qv.cross(t);
    }

    /// Rotation matrix R such that R * v == rotate(v).
    [[nodiscard]] Mat3d toMatrix() const noexcept {
        const Quatd q = normalized();
        const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        return {1 - 2 * (yy + zz), 2 * (xy - wz),     2 * (xz + wy),
                2 * (xy + wz),     1 - 2 * (xx + zz), 2 * (yz - wx),
                2 * (xz - wy),     2 * (yz + wx),     1 - 2 * (xx + yy)};
    }

    /// Rotation angle in radians between two unit quaternions (q and -q are
    /// the same rotation, so the absolute dot product is used).
    [[nodiscard]] double angleTo(const Quatd& o) const noexcept {
        const double d = clampd(std::fabs(normalized().dot(o.normalized())), -1.0, 1.0);
        return 2.0 * std::acos(d);
    }

    /// Spherical linear interpolation with shortest-path sign handling and a
    /// normalised-lerp fallback when the inputs are nearly parallel.
    static Quatd slerp(const Quatd& a, const Quatd& bIn, double t) noexcept {
        Quatd b = bIn;
        double d = a.dot(b);
        if (d < 0.0) {
            b = {-b.w, -b.x, -b.y, -b.z};
            d = -d;
        }
        t = clampd(t, 0.0, 1.0);
        if (d > 0.9995) {
            Quatd r{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            return r.normalized();
        }
        const double theta0 = std::acos(clampd(d, -1.0, 1.0));
        const double theta = theta0 * t;
        const double sinTheta0 = std::sin(theta0);
        if (sinTheta0 <= 0.0) {
            return a.normalized();
        }
        const double s0 = std::cos(theta) - d * std::sin(theta) / sinTheta0;
        const double s1 = std::sin(theta) / sinTheta0;
        Quatd r{a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1};
        return r.normalized();
    }

    [[nodiscard]] bool isFinite() const noexcept {
        return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
};

}  // namespace osv
