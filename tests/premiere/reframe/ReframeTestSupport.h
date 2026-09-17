// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Shared scaffolding for the Open360Reframe tests.
//
// The tests deliberately load the BUILT .aex with LoadLibraryW rather than
// linking its objects: that is the only way to prove what Premiere will
// actually see - the exported symbol names, the PiPL resource, the flag
// words at run time and the behaviour of the module as one linked unit,
// including the static initialisers and the delay-load hook.
//
// Everything below is about getting hold of that module and building the
// synthetic imagery the render tests compare against.
#pragma once

#include "MockHost.h"

#include "AEConfig.h"
#include "A.h"
#include "AE_Effect.h"
#include "PrSDKGPUFilter.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::reframe::test {

/// Signature of the AE entry point, exactly as the PiPL names it.
using EffectMainFn = PF_Err (*)(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                PF_LayerDef* output, void* extra);

/// Signature of the GPU entry point (PrSDKGPUFilter.h:221-228).
using GpuEntryFn = prSuiteError (*)(csSDK_uint32 inHostInterfaceVersion, csSDK_int32* ioIndex, prBool inStartup,
                                    piSuitesPtr piSuites, PrGPUFilter* outFilter, PrGPUFilterInfo* outFilterInfo);

/// The loaded plug-in.  One instance is shared by the whole test run: the
/// module keeps process-wide state (the CUDA module cache, the log file), and
/// loading and unloading it per test case would exercise a path Premiere
/// never takes.
class LoadedPlugin {
public:
    /// The singleton, loaded on first use.  Never null; check `ok()`.
    static LoadedPlugin& instance();

    [[nodiscard]] bool ok() const noexcept { return m_module != nullptr && m_effectMain && m_gpuEntry; }
    /// Why ok() is false ("" when it is true).
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

    [[nodiscard]] HMODULE module() const noexcept { return m_module; }
    [[nodiscard]] EffectMainFn effectMain() const noexcept { return m_effectMain; }
    [[nodiscard]] GpuEntryFn gpuEntry() const noexcept { return m_gpuEntry; }
    /// Full path the module was loaded from.
    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }

private:
    LoadedPlugin();
    ~LoadedPlugin() = default;
    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    HMODULE m_module = nullptr;
    EffectMainFn m_effectMain = nullptr;
    GpuEntryFn m_gpuEntry = nullptr;
    std::wstring m_path;
    std::string m_error;
};

// ---------------------------------------------------------------------------
//  The synthetic panorama
// ---------------------------------------------------------------------------

/// A labelled equirectangular test frame.
///
/// Colour is a known, CONTINUOUS, invertible function of direction:
///
///     R = 0.5 + 0.5 * cos(longitude)
///     B = 0.5 + 0.5 * sin(longitude)
///     G = (latitude + 90) / 180   in [0, 1], increasing upward
///     A = 1
///
/// Longitude is encoded as a cos/sin PAIR rather than as a ramp, and that
/// choice is the whole point.  A ramp - (lon + 180) / 360 - jumps by a full
/// 1.0 across the +/-180 degree seam, so a correctly wrapping bilinear
/// sampler averages 1.0 and 0.0 there and reports longitude 0 for a camera
/// looking straight at the seam.  The test would then fail on a renderer
/// that is doing exactly the right thing.  atan2(B', R') recovers the angle
/// continuously, so a seam-crossing sample is decoded correctly and a
/// genuine wrap-around bug still shows up as a wrong angle.
///
/// Latitude has no seam (it clamps at the poles), so a plain ramp is fine.
struct Panorama {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;  ///< width * height * 4, row 0 = north pole.

    /// The colour the formula gives for a direction in degrees.
    static void colourFor(double lonDeg, double latDeg, float out[4]) noexcept;

    /// Decode a sampled colour back to a longitude in degrees (-180, 180].
    [[nodiscard]] static double decodeLongitude(const float rgba[4]) noexcept;
    /// Decode a sampled colour back to a latitude in degrees [-90, 90].
    [[nodiscard]] static double decodeLatitude(const float rgba[4]) noexcept;
};

/// Build the labelled panorama.
[[nodiscard]] Panorama makePanorama(int width, int height);

/// The panorama packed as BGRA 32-bit float, top-left origin, with the given
/// row pitch (which must be at least width * 16).
[[nodiscard]] std::vector<std::uint8_t> packBgra32f(const Panorama& p, std::int32_t rowBytes);

/// The panorama packed as BGRA 16-bit float (IEEE binary16), top-left.
[[nodiscard]] std::vector<std::uint8_t> packBgra16f(const Panorama& p, std::int32_t rowBytes);

/// The code Premiere's 16-bit-integer formats use for white: 32768, NOT
/// 65535.  Premiere SDK guide section 5.4.2: "The 16-bit formats use channels
/// that go from black at 0 to white at 32768, like After Effects and
/// Photoshop 16-bit formats."
///
/// Declared here INDEPENDENTLY of the plug-in's own kBgra16uWhite, and that
/// duplication is deliberate: a test that imported the constant from the code
/// under test would pass just as happily if both were 65535.  Spelling the
/// documented number out on the test side is what makes the scale an
/// assertion rather than a tautology.
constexpr float kBgra16uWhiteRef = 32768.0f;

/// The panorama packed as BGRA 16-bit unsigned, top-left, on the 0..32768
/// scale above.
[[nodiscard]] std::vector<std::uint8_t> packBgra16u(const Panorama& p, std::int32_t rowBytes);

/// UTF-16 -> UTF-8.  Used only to put a module path into a Catch2 INFO
/// message; narrowing each wchar_t with a char cast would mangle any
/// non-ASCII character in the build directory's name.
[[nodiscard]] std::string toUtf8(const std::wstring& text);

/// IEEE binary16 -> binary32 (used to read 16f results back).
[[nodiscard]] float halfToFloat(std::uint16_t h) noexcept;
/// IEEE binary32 -> binary16, round to nearest even.
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept;

// ---------------------------------------------------------------------------
//  Comparison helpers
// ---------------------------------------------------------------------------

/// Peak signal-to-noise ratio in dB between two equally sized float buffers,
/// with 1.0 as the peak signal.  Identical buffers give +infinity.
[[nodiscard]] double psnr(const std::vector<float>& a, const std::vector<float>& b) noexcept;

/// Read one BGRA pixel out of a top-left buffer as RGBA floats.
void readPixelBgra32f(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept;
void readPixelBgra16f(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept;
void readPixelBgra8u(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept;
/// Read one BGRA 16-bit-unsigned pixel back as RGBA floats, dividing by the
/// documented white point (kBgra16uWhiteRef).
void readPixelBgra16u(const std::uint8_t* base, std::int32_t rowBytes, int x, int y, float out[4]) noexcept;

}  // namespace osv::reframe::test
