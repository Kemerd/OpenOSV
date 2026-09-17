// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Implementation of the shared test scaffolding.

#include "ReframeTestSupport.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace osv::reframe::test {

namespace {

/// The plug-in is staged next to the build's plugins/OpenOSV directory,
/// which the build passes in as OSV_REFRAME_AEX_PATH.  Falling back to the
/// test executable's own directory covers the case where a developer copied
/// the module next to the tests by hand.
std::wstring aexPath() {
#ifdef OSV_REFRAME_AEX_PATH
    return std::wstring(OSV_REFRAME_AEX_PATH);
#else
    return L"Open360Reframe.aex";
#endif
}

/// Human-readable GetLastError().
std::string lastErrorText(DWORD code) {
    char* buffer = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = (len && buffer) ? std::string(buffer, len) : std::string("unknown error");
    if (buffer) {
        LocalFree(buffer);
    }
    // Trim the trailing CRLF FormatMessage appends.
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
//  LoadedPlugin
// ---------------------------------------------------------------------------
LoadedPlugin::LoadedPlugin() {
    m_path = aexPath();

    // LOAD_WITH_ALTERED_SEARCH_PATH makes the module's own folder the first
    // place the loader looks for ITS dependencies, which is exactly how
    // Premiere loads a plug-in out of MediaCore - and it is what lets the
    // staged fmt.dll / spdlog.dll beside the .aex be found.
    m_module = LoadLibraryExW(m_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_module) {
        const DWORD err = GetLastError();
        m_error = "LoadLibraryExW failed (" + std::to_string(err) + "): " + lastErrorText(err);
        return;
    }

    // GetProcAddress by the exact names the PiPL and PrSDKGPUFilter.h use.
    // A mismatch here is the single most common way a plug-in silently does
    // nothing in a real host.
    m_effectMain = reinterpret_cast<EffectMainFn>(
        reinterpret_cast<void*>(GetProcAddress(m_module, "EffectMain")));
    if (!m_effectMain) {
        m_error = "the module exports no 'EffectMain' (the name the PiPL's CodeWin64X86 property declares)";
        return;
    }
    m_gpuEntry = reinterpret_cast<GpuEntryFn>(
        reinterpret_cast<void*>(GetProcAddress(m_module, PrGPUFilterEntryPointName)));
    if (!m_gpuEntry) {
        m_error = std::string("the module exports no '") + PrGPUFilterEntryPointName + "'";
        return;
    }
}

LoadedPlugin& LoadedPlugin::instance() {
    // Function-local static: constructed on first use, never destroyed, so
    // the module stays loaded for the whole run (FreeLibrary during static
    // teardown would race the CRT).
    static LoadedPlugin* plugin = new LoadedPlugin();
    return *plugin;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    // The fill character is overwritten immediately by the conversion; it
    // only has to be a valid char, so a space is used rather than a NUL.
    std::string out(static_cast<std::size_t>(needed), ' ');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

// ---------------------------------------------------------------------------
//  The synthetic panorama
// ---------------------------------------------------------------------------
void Panorama::colourFor(double lonDeg, double latDeg, float out[4]) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const double lonRad = lonDeg * kPi / 180.0;
    const double lat = std::clamp(latDeg, -90.0, 90.0);

    // cos / sin of the longitude, mapped into [0, 1].  Both are periodic, so
    // the label has no discontinuity anywhere on the sphere - see the header
    // for why that matters at the +/-180 degree seam.
    out[0] = static_cast<float>(0.5 + 0.5 * std::cos(lonRad));
    out[1] = static_cast<float>((lat + 90.0) / 180.0);
    out[2] = static_cast<float>(0.5 + 0.5 * std::sin(lonRad));
    out[3] = 1.0f;
}

double Panorama::decodeLongitude(const float rgba[4]) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    // Undo the [0, 1] mapping, then atan2.  Bilinear interpolation shortens
    // the (cos, sin) vector slightly between samples but does not rotate it,
    // so the recovered ANGLE stays accurate even where the magnitude does
    // not - which is exactly the property a ramp label lacks.
    const double c = 2.0 * static_cast<double>(rgba[0]) - 1.0;
    const double s = 2.0 * static_cast<double>(rgba[2]) - 1.0;
    return std::atan2(s, c) * 180.0 / kPi;
}

double Panorama::decodeLatitude(const float rgba[4]) noexcept {
    return static_cast<double>(rgba[1]) * 180.0 - 90.0;
}

Panorama makePanorama(int width, int height) {
    Panorama p;
    if (width <= 0 || height <= 0) {
        return p;
    }
    p.width = width;
    p.height = height;
    p.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);

    // Standard layout, matching osvSampleEquirectRgba():
    //   lon = ((x + 0.5) / W - 0.5) * 360
    //   lat = (0.5 - (y + 0.5) / H) * 180
    for (int y = 0; y < height; ++y) {
        const double lat = (0.5 - (static_cast<double>(y) + 0.5) / static_cast<double>(height)) * 180.0;
        for (int x = 0; x < width; ++x) {
            const double lon = ((static_cast<double>(x) + 0.5) / static_cast<double>(width) - 0.5) * 360.0;
            float rgba[4];
            Panorama::colourFor(lon, lat, rgba);
            const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(x)) * 4u;
            p.rgba[base + 0] = rgba[0];
            p.rgba[base + 1] = rgba[1];
            p.rgba[base + 2] = rgba[2];
            p.rgba[base + 3] = rgba[3];
        }
    }
    return p;
}

std::vector<std::uint8_t> packBgra32f(const Panorama& p, std::int32_t rowBytes) {
    std::vector<std::uint8_t> bytes;
    if (p.width <= 0 || p.height <= 0 || rowBytes < p.width * 16) {
        return bytes;
    }
    bytes.assign(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(p.height), 0u);
    for (int y = 0; y < p.height; ++y) {
        float* row = reinterpret_cast<float*>(bytes.data() + static_cast<std::size_t>(rowBytes) *
                                                                 static_cast<std::size_t>(y));
        for (int x = 0; x < p.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width) +
                                     static_cast<std::size_t>(x)) * 4u;
            row[x * 4 + 0] = p.rgba[src + 2];  // B
            row[x * 4 + 1] = p.rgba[src + 1];  // G
            row[x * 4 + 2] = p.rgba[src + 0];  // R
            row[x * 4 + 3] = p.rgba[src + 3];  // A
        }
    }
    return bytes;
}

std::vector<std::uint8_t> packBgra16f(const Panorama& p, std::int32_t rowBytes) {
    std::vector<std::uint8_t> bytes;
    if (p.width <= 0 || p.height <= 0 || rowBytes < p.width * 8) {
        return bytes;
    }
    bytes.assign(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(p.height), 0u);
    for (int y = 0; y < p.height; ++y) {
        std::uint16_t* row = reinterpret_cast<std::uint16_t*>(
            bytes.data() + static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(y));
        for (int x = 0; x < p.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width) +
                                     static_cast<std::size_t>(x)) * 4u;
            row[x * 4 + 0] = floatToHalf(p.rgba[src + 2]);
            row[x * 4 + 1] = floatToHalf(p.rgba[src + 1]);
            row[x * 4 + 2] = floatToHalf(p.rgba[src + 0]);
            row[x * 4 + 3] = floatToHalf(p.rgba[src + 3]);
        }
    }
    return bytes;
}

// ---------------------------------------------------------------------------
//  Half conversions (independent of the plug-in's own, on purpose: a test
//  that used the code under test to check the code under test would pass
//  even if both were wrong)
// ---------------------------------------------------------------------------
float halfToFloat(std::uint16_t h) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) & 1u;
    const std::uint32_t exponent = static_cast<std::uint32_t>(h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = static_cast<std::uint32_t>(h) & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign << 31;
        } else {
            std::uint32_t m = mantissa;
            std::uint32_t e = 113u;
            while ((m & 0x400u) == 0u) {
                m <<= 1;
                e -= 1u;
            }
            bits = (sign << 31) | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
    } else {
        bits = (sign << 31) | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::uint16_t floatToHalf(float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t rawExp = (bits >> 23) & 0xFFu;
    const std::uint32_t mantissa = bits & 0x7FFFFFu;

    if (rawExp == 0xFFu) {
        return static_cast<std::uint16_t>(sign | (mantissa != 0u ? 0x7E00u : 0x7C00u));
    }
    int exponent = static_cast<int>(rawExp) - 127 + 15;
    if (exponent >= 0x1F) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        const std::uint32_t m = mantissa | 0x800000u;
        const int shift = 14 - exponent;
        const std::uint32_t half = m >> shift;
        const std::uint32_t rem = m & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        std::uint32_t rounded = half;
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            rounded += 1u;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rem = mantissa & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
        half += 1u;
    }
    return static_cast<std::uint16_t>(sign | half);
}

// ---------------------------------------------------------------------------
//  Comparison helpers
// ---------------------------------------------------------------------------
double psnr(const std::vector<float>& a, const std::vector<float>& b) noexcept {
    if (a.empty() || a.size() != b.size()) {
        return -1.0;  // caller treats a negative result as "cannot compare"
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        // A non-finite sample would poison the whole metric; count it as a
        // full-scale error instead of producing NaN dB.
        const double d = (std::isfinite(a[i]) && std::isfinite(b[i])) ? (static_cast<double>(a[i]) -
                                                                        static_cast<double>(b[i]))
                                                                     : 1.0;
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(a.size());
    if (mse <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(1.0 / mse);
}

void readPixelBgra32f(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept {
    const float* row = reinterpret_cast<const float*>(base + static_cast<std::ptrdiff_t>(rowBytes) *
                                                                 static_cast<std::ptrdiff_t>(y));
    out[0] = row[x * 4 + 2];  // R
    out[1] = row[x * 4 + 1];  // G
    out[2] = row[x * 4 + 0];  // B
    out[3] = row[x * 4 + 3];  // A
}

void readPixelBgra16f(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept {
    const std::uint16_t* row = reinterpret_cast<const std::uint16_t*>(
        base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(y));
    out[0] = halfToFloat(row[x * 4 + 2]);
    out[1] = halfToFloat(row[x * 4 + 1]);
    out[2] = halfToFloat(row[x * 4 + 0]);
    out[3] = halfToFloat(row[x * 4 + 3]);
}

void readPixelBgra8u(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept {
    const std::uint8_t* row = base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(y);
    out[0] = static_cast<float>(row[x * 4 + 2]) / 255.0f;
    out[1] = static_cast<float>(row[x * 4 + 1]) / 255.0f;
    out[2] = static_cast<float>(row[x * 4 + 0]) / 255.0f;
    out[3] = static_cast<float>(row[x * 4 + 3]) / 255.0f;
}

}  // namespace osv::reframe::test
