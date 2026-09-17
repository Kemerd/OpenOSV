// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Double precision reference implementations of the transfer functions.
// These mirror ColorMath.h line by line but in double so the tests can
// measure the float error of the kernel math instead of comparing it against
// itself.

#include "osv/color/Transfer.h"

#include <algorithm>
#include <cmath>

namespace osv::color::ref {

namespace {

// BT.2100 / ST 2084 constants in double.
constexpr double kA = 0.17883277;
constexpr double kB = 0.28466892;
constexpr double kC = 0.55991073;
constexpr double kM1 = 0.1593017578125;
constexpr double kM2 = 78.84375;
constexpr double kC1 = 0.8359375;
constexpr double kC2 = 18.8515625;
constexpr double kC3 = 18.6875;
constexpr double kPqPeak = 10000.0;

/// Clamp to [0,1] in double.
double sat(double v) noexcept { return std::clamp(v, 0.0, 1.0); }

}  // namespace

double hlgOetf(double e) noexcept {
    // Negative light is clamped; the curve has two branches at 1/12.
    e = std::max(e, 0.0);
    if (e <= 1.0 / 12.0) {
        return std::sqrt(3.0 * e);
    }
    return kA * std::log(std::max(12.0 * e - kB, 1e-300)) + kC;
}

double hlgInverseOetf(double ep) noexcept {
    ep = std::max(ep, 0.0);
    if (ep <= 0.5) {
        return ep * ep / 3.0;
    }
    return (std::exp((ep - kC) / kA) + kB) / 12.0;
}

double hlgOotfScale(double ys, double peakNits, double gamma) noexcept {
    if (ys <= 0.0) {
        return 0.0;
    }
    return peakNits * std::pow(ys, gamma - 1.0);
}

double pqInverseEotf(double nits) noexcept {
    const double y = sat(nits / kPqPeak);
    const double ym1 = std::pow(y, kM1);
    return std::pow((kC1 + kC2 * ym1) / (1.0 + kC3 * ym1), kM2);
}

double pqEotf(double code) noexcept {
    const double ep = std::pow(sat(code), 1.0 / kM2);
    const double num = std::max(ep - kC1, 0.0);
    const double den = kC2 - kC3 * ep;
    if (den <= 1e-300) {
        return kPqPeak;
    }
    return std::pow(num / den, 1.0 / kM1) * kPqPeak;
}

double rec709Oetf(double e) noexcept {
    e = sat(e);
    if (e < 0.018) {
        return 4.5 * e;
    }
    return 1.099 * std::pow(e, 0.45) - 0.099;
}

double rec709InverseOetf(double v) noexcept {
    v = sat(v);
    if (v < 0.081) {
        return v / 4.5;
    }
    return std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

double bt2390Eetf(double pqCode, double srcPeakNits, double dstPeakNits) noexcept {
    // No compression needed when the display can reproduce the source peak.
    if (dstPeakNits >= srcPeakNits || srcPeakNits <= 0.0 || dstPeakNits <= 0.0) {
        return pqCode;
    }
    const double srcMax = pqInverseEotf(srcPeakNits);
    if (srcMax <= 1e-12) {
        return pqCode;
    }
    // Normalise to the source range (black assumed 0 nit -> PQ 0).
    const double e1 = sat(pqCode / srcMax);
    const double maxLum = pqInverseEotf(dstPeakNits) / srcMax;
    const double ks = 1.5 * maxLum - 0.5;
    double e2 = e1;
    // Hermite spline knee above KS.
    if (e1 > ks && ks < 1.0) {
        const double t = (e1 - ks) / (1.0 - ks);
        const double t2 = t * t;
        const double t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    return std::clamp(e2, 0.0, maxLum) * srcMax;
}

}  // namespace osv::color::ref
