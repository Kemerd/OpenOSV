// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the .cube 3D LUT writer / reader (osv/color/Cube.h): the file
// layout (header lines exact, red index fastest, six decimals), the round
// trip through readCube, trilinear sampling accuracy against the direct
// pipeline, the narrow-range input axis option and the reader's rejection of
// broken files.  None of these need the sample clip.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/color/ColorParams.h"
#include "osv/color/Cube.h"
#include "osv/color/DlogM.h"
#include "osv/color/Matrices.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::color;
using Catch::Matchers::WithinAbs;

namespace {

/// Read every line of a text file (used to check the exact header layout).
std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

/// Write a small text file for the reader error tests.
std::filesystem::path writeText(const std::string& name, const std::string& text) {
    const std::filesystem::path path = osvtest::tempDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << text;
    return path;
}

/// Largest |lut.sample - direct pipeline| over `count` random RGB inputs
/// drawn uniformly from the whole cube.  About a third of that volume is
/// colours outside the Rec.2020 gamut (a negative working channel that the
/// pipeline clamps to zero), and the HLG sqrt / PQ y^0.159 slope is infinite
/// at zero, so trilinear interpolation across the clip boundary is the worst
/// case any 3D LUT of this pipeline can have.  This bound therefore only
/// guards against gross layout bugs (a swapped axis order gives ~0.5).
float worstWholeCubeError(const Lut3D& lut, const OsvColorParams& params, const CubeOptions& options, int count,
                          unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    float worst = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float in[3] = {uni(rng), uni(rng), uni(rng)};
        float viaLut[3] = {0.0f, 0.0f, 0.0f};
        float direct[3] = {0.0f, 0.0f, 0.0f};
        REQUIRE(lut.sample(in, viaLut));
        evaluateCubeEntry(params, options, in, direct);
        for (int ch = 0; ch < 3; ++ch) {
            worst = std::max(worst, std::fabs(viaLut[ch] - direct[ch]));
        }
    }
    return worst;
}

/// Largest |lut.sample - direct pipeline| over `count` random colours the
/// camera can actually encode: Rec.709-gamut scene-linear colours spanning
/// `loStops` .. `hiStops` around 18 % grey, converted to native D-Log M code
/// through the inverse of the pipeline (2020 -> native matrix, then
/// linearToDlogm).  Rec.709 lies inside Rec.2020, so the forward pipeline
/// never clips.  `minMix` limits saturation: every Rec.709 channel is at
/// least that fraction of the largest one (1.0 = neutral axis only).
/// Candidates whose native linear value falls below `minNativeLinear` or
/// whose code leaves [0,1] are redrawn.
///
/// What limits the accuracy (measured, see the fit notes in DlogM.h): the
/// C0 cut of the refit curve at code 0.279 (slope ratio 3) and the HLG sqrt /
/// PQ y^0.159 slope next to black give ~0.008 at 65^3 for any exposure range
/// that reaches -7 stops, and at 17^3 the native -> Rec.2020 matrix's
/// negative coefficients make saturated blues cancel two exponentially
/// varying channels (0.09), so only the neutral axis holds 0.02 there.
float worstEncodableError(const Lut3D& lut, const OsvColorParams& params, const CubeOptions& options, int count,
                          unsigned seed, float minNativeLinear, float minMix, float loStops, float hiStops) {
    OsvMat3f toNative{};
    REQUIRE(mat3Inverse(kNativeToRec2020_Pocket3, toNative));
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    float worst = 0.0f;
    int accepted = 0;
    int guard = 0;
    while (accepted < count && guard < count * 20) {
        ++guard;
        // Random Rec.709 colour: log-uniform luminance, random chroma mix.
        const float lum = 0.18f * std::exp2(loStops + (hiStops - loStops) * uni(rng));
        float rgb709[3] = {minMix + (1.0f - minMix) * uni(rng), minMix + (1.0f - minMix) * uni(rng),
                           minMix + (1.0f - minMix) * uni(rng)};
        const float mx = std::max({rgb709[0], rgb709[1], rgb709[2], 1e-6f});
        for (float& v : rgb709) {
            v = v / mx * lum;
        }
        float lin2020[3];
        osvMat3Apply(&kRec709ToRec2020, rgb709[0], rgb709[1], rgb709[2], lin2020);
        float native[3];
        osvMat3Apply(&toNative, lin2020[0], lin2020[1], lin2020[2], native);
        bool usable = true;
        float in[3] = {0.0f, 0.0f, 0.0f};
        for (int ch = 0; ch < 3; ++ch) {
            if (native[ch] < minNativeLinear) {
                usable = false;
                break;
            }
            in[ch] = linearToDlogm(params.curve, native[ch]);
            if (in[ch] < 0.0f || in[ch] > 1.0f) {
                usable = false;
                break;
            }
        }
        if (!usable) {
            continue;
        }
        ++accepted;
        float viaLut[3] = {0.0f, 0.0f, 0.0f};
        float direct[3] = {0.0f, 0.0f, 0.0f};
        REQUIRE(lut.sample(in, viaLut));
        evaluateCubeEntry(params, options, in, direct);
        for (int ch = 0; ch < 3; ++ch) {
            worst = std::max(worst, std::fabs(viaLut[ch] - direct[ch]));
        }
    }
    REQUIRE(accepted == count);
    return worst;
}

}  // namespace

TEST_CASE("writeCube 65 writes the documented layout and round trips", "[color][cube]") {
    const OsvColorParams params = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f);
    CubeOptions options;
    options.size = 65;
    options.title = "OpenOSV test HLG";
    const std::filesystem::path path = osvtest::tempDir() / "color_hlg_65.cube";

    const Status st = writeCube(path, params, options);
    INFO((st.ok() ? std::string("ok") : st.error().toString()));
    REQUIRE(st.ok());

    // --- exact header and line count ---------------------------------------
    const std::vector<std::string> lines = readLines(path);
    REQUIRE(lines.size() == 4u + 65u * 65u * 65u);
    REQUIRE(lines[0] == "TITLE \"OpenOSV test HLG\"");
    REQUIRE(lines[1] == "LUT_3D_SIZE 65");
    REQUIRE(lines[2] == "DOMAIN_MIN 0.000000 0.000000 0.000000");
    REQUIRE(lines[3] == "DOMAIN_MAX 1.000000 1.000000 1.000000");
    // Every data line is three six-decimal ASCII numbers.
    for (std::size_t i = 4; i < lines.size(); i += 4097) {
        float v[3];
        REQUIRE(std::sscanf(lines[i].c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3);
        REQUIRE(lines[i].find('.') != std::string::npos);
        REQUIRE(lines[i].size() >= 3u * 8u + 2u);  // "0.000000 0.000000 0.000000"
        for (const char ch : lines[i]) {
            REQUIRE(static_cast<unsigned char>(ch) < 0x80);
        }
    }

    // --- read back ------------------------------------------------------------
    const Result<Lut3D> read = readCube(path);
    INFO((read.ok() ? std::string("ok") : read.error().toString()));
    REQUIRE(read.ok());
    const Lut3D& lut = read.value();
    REQUIRE(lut.valid());
    REQUIRE(lut.size == 65);
    REQUIRE(lut.title == "OpenOSV test HLG");
    REQUIRE(lut.data.size() == 65u * 65u * 65u * 3u);

    // --- red index varies fastest -------------------------------------------
    // Entry index 1 in the file is (r=1, g=0, b=0): the red input steps by
    // 1/64 while green and blue stay at zero.
    float second[3];
    REQUIRE(std::sscanf(lines[5].c_str(), "%f %f %f", &second[0], &second[1], &second[2]) == 3);
    float at100[3];
    lut.at(1, 0, 0, at100);
    REQUIRE(at100[0] == second[0]);
    REQUIRE(at100[1] == second[1]);
    REQUIRE(at100[2] == second[2]);
    const float redStep[3] = {1.0f / 64.0f, 0.0f, 0.0f};
    float direct[3];
    evaluateCubeEntry(params, options, redStep, direct);
    for (int ch = 0; ch < 3; ++ch) {
        REQUIRE_THAT(at100[ch], WithinAbs(direct[ch], 1e-6));
    }
    // The grid corner (0,0,0) is the first data line; (64,64,64) the last.
    float first[3];
    REQUIRE(std::sscanf(lines[4].c_str(), "%f %f %f", &first[0], &first[1], &first[2]) == 3);
    float at000[3];
    lut.at(0, 0, 0, at000);
    REQUIRE(at000[0] == first[0]);
    const float white[3] = {1.0f, 1.0f, 1.0f};
    float lutWhite[3];
    REQUIRE(lut.sample(white, lutWhite));
    evaluateCubeEntry(params, options, white, direct);
    REQUIRE_THAT(lutWhite[1], WithinAbs(direct[1], 1e-6));

    // --- trilinear sampling matches the direct pipeline -----------------------
    // Any Rec.709-gamut colour from -7 to +4 stops (native linear >= 2e-3):
    // measured 0.006-0.008, bounded by the curve's cut and the black corner.
    const float worst = worstEncodableError(lut, params, options, 2000, 4242u, 2e-3f, 0.0f, -7.0f, 4.0f);
    INFO("worst |lut.sample - direct| over encodable colours (65^3 HLG): " << worst);
    REQUIRE(worst < 0.012f);
    // Neutral axis from -4 to +4 stops: comfortably inside a 10-bit step.
    const float worstGrey = worstEncodableError(lut, params, options, 500, 4244u, 1e-4f, 1.0f, -4.0f, 4.0f);
    INFO("worst |lut.sample - direct| on the neutral axis (65^3 HLG): " << worstGrey);
    REQUIRE(worstGrey < 0.006f);
    // Whole cube including out-of-gamut colours: only guards the layout.
    const float worstAll = worstWholeCubeError(lut, params, options, 2000, 4243u);
    INFO("worst |lut.sample - direct| over the whole cube (65^3 HLG): " << worstAll);
    REQUIRE(worstAll < 0.10f);
    // Grid points reproduce exactly up to the six-decimal quantisation.
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    float lutGrey[3];
    REQUIRE(lut.sample(grey, lutGrey));
    evaluateCubeEntry(params, options, grey, direct);
    REQUIRE_THAT(lutGrey[0], WithinAbs(direct[0], 1e-6));
}

TEST_CASE("writeCube 17 stays within 0.02 of the direct pipeline on the neutral axis", "[color][cube]") {
    const OsvColorParams params = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::PQ, 0.0f);
    CubeOptions options;
    options.size = 17;
    const std::filesystem::path path = osvtest::tempDir() / "color_pq_17.cube";
    REQUIRE(writeCube(path, params, options).ok());
    const Result<Lut3D> read = readCube(path);
    REQUIRE(read.ok());
    REQUIRE(read.value().size == 17);
    // Default title is filled in by the writer.
    REQUIRE_FALSE(read.value().title.empty());
    // Neutral axis from -4 to +4 stops: measured 0.013 (the cut at code 0.279).
    const float worstGrey = worstEncodableError(read.value(), params, options, 2000, 99u, 1e-4f, 1.0f, -4.0f, 4.0f);
    INFO("worst |lut.sample - direct| on the neutral axis (17^3 PQ): " << worstGrey);
    REQUIRE(worstGrey < 0.02f);
    // Saturated colours at 17^3 are limited by the matrix cancellation (0.09).
    const float worst = worstEncodableError(read.value(), params, options, 2000, 101u, 2e-3f, 0.0f, -7.0f, 4.0f);
    INFO("worst |lut.sample - direct| over encodable colours (17^3 PQ): " << worst);
    REQUIRE(worst < 0.15f);
    const float worstAll = worstWholeCubeError(read.value(), params, options, 2000, 100u);
    INFO("worst |lut.sample - direct| over the whole cube (17^3 PQ): " << worstAll);
    REQUIRE(worstAll < 0.30f);
}

TEST_CASE("Cube narrow-range input axis decodes the video range first", "[color][cube]") {
    const OsvColorParams params = makeColorParams(DlogMFit::DjiRefit, OutputTransfer::HLG, 0.0f);
    CubeOptions narrow;
    narrow.inputIsNarrowCode = true;
    CubeOptions plain;
    // A narrow-range 10-bit value of (64 + 0.4 * 876) / 1023 is D-Log M code 0.40.
    const float v = (64.0f + 0.4f * 876.0f) / 1023.0f;
    const float inNarrow[3] = {v, v, v};
    const float inPlain[3] = {0.4f, 0.4f, 0.4f};
    float a[3], b[3];
    evaluateCubeEntry(params, narrow, inNarrow, a);
    evaluateCubeEntry(params, plain, inPlain, b);
    REQUIRE_THAT(a[0], WithinAbs(b[0], 1e-4));
    REQUIRE_THAT(a[0], WithinAbs(0.380, 0.006));
    // Below video black clamps to code 0, above video white to code 1.
    const float black[3] = {0.0f, 0.0f, 0.0f};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    evaluateCubeEntry(params, narrow, black, a);
    evaluateCubeEntry(params, plain, zero, b);
    REQUIRE(a[0] == b[0]);
    const float over[3] = {1.0f, 1.0f, 1.0f};
    evaluateCubeEntry(params, narrow, over, a);
    evaluateCubeEntry(params, plain, over, b);
    REQUIRE(a[0] == b[0]);
    // Writing with the option round trips through the file too.
    narrow.size = 9;
    const std::filesystem::path path = osvtest::tempDir() / "color_narrow_9.cube";
    REQUIRE(writeCube(path, params, narrow).ok());
    const Result<Lut3D> read = readCube(path);
    REQUIRE(read.ok());
    float viaLut[3];
    REQUIRE(read.value().sample(inNarrow, viaLut));
    REQUIRE_THAT(viaLut[0], WithinAbs(0.380, 0.03));
}

TEST_CASE("Lut3D sampling is defensive", "[color][cube]") {
    Lut3D empty;
    REQUIRE_FALSE(empty.valid());
    const float in[3] = {0.5f, 0.5f, 0.5f};
    float out[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE_FALSE(empty.sample(in, out));
    REQUIRE(out[0] == 0.0f);
    // Wrong data size is invalid as well.
    Lut3D bad;
    bad.size = 2;
    bad.data.assign(5, 0.0f);
    REQUIRE_FALSE(bad.valid());
    // A tiny 2^3 identity LUT interpolates linearly and clamps its input.
    Lut3D ident;
    ident.size = 2;
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                ident.data.push_back(static_cast<float>(r));
                ident.data.push_back(static_cast<float>(g));
                ident.data.push_back(static_cast<float>(b));
            }
        }
    }
    REQUIRE(ident.valid());
    const float mid[3] = {0.25f, 0.5f, 0.75f};
    REQUIRE(ident.sample(mid, out));
    REQUIRE_THAT(out[0], WithinAbs(0.25, 1e-6));
    REQUIRE_THAT(out[1], WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(out[2], WithinAbs(0.75, 1e-6));
    const float outside[3] = {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()};
    REQUIRE(ident.sample(outside, out));
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] == 1.0f);
    REQUIRE(out[2] == 0.0f);
    // Null pointers never crash.
    REQUIRE_FALSE(ident.sample(nullptr, out));
    REQUIRE_FALSE(ident.sample(in, nullptr));
    ident.at(0, 0, 0, nullptr);
    // Out-of-range grid indices clamp to the last entry.
    ident.at(99, 99, 99, out);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[2] == 1.0f);
}

TEST_CASE("writeCube rejects bad options and readCube rejects bad files", "[color][cube]") {
    const OsvColorParams params = makeColorParams(DlogMFit::Pocket3, OutputTransfer::HLG, 0.0f);
    CubeOptions options;
    options.size = 1;
    REQUIRE(writeCube(osvtest::tempDir() / "never.cube", params, options).error().code == ErrorCode::InvalidArgument);
    options.size = 300;
    REQUIRE(writeCube(osvtest::tempDir() / "never.cube", params, options).error().code == ErrorCode::InvalidArgument);
    options.size = 5;
    options.domainMax[1] = -1.0f;
    REQUIRE(writeCube(osvtest::tempDir() / "never.cube", params, options).error().code == ErrorCode::InvalidArgument);
    options.domainMax[1] = 1.0f;
    REQUIRE(writeCube(std::filesystem::path(), params, options).error().code == ErrorCode::InvalidArgument);
    // A directory that does not exist is an Io failure.
    REQUIRE(writeCube(osvtest::tempDir() / "no_such_dir_xyz" / "x.cube", params, options).error().code ==
            ErrorCode::Io);

    // Reader.
    REQUIRE(readCube(osvtest::tempDir() / "does_not_exist.cube").error().code == ErrorCode::Io);
    REQUIRE(readCube(std::filesystem::path()).error().code == ErrorCode::InvalidArgument);
    REQUIRE(readCube(writeText("lut1d.cube", "LUT_1D_SIZE 4\n0 0 0\n1 1 1\n")).error().code == ErrorCode::Unsupported);
    REQUIRE(readCube(writeText("nosize.cube", "TITLE \"x\"\n0 0 0\n")).error().code == ErrorCode::Malformed);
    REQUIRE(readCube(writeText("short.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n")).error().code == ErrorCode::Truncated);
    REQUIRE(readCube(writeText("baddata.cube", "LUT_3D_SIZE 2\n0 0 zebra\n")).error().code == ErrorCode::Malformed);
    REQUIRE(readCube(writeText("bigsize.cube", "LUT_3D_SIZE 999\n")).error().code == ErrorCode::Malformed);
    // Comments, blank lines, CRLF and unknown keywords are tolerated.
    std::string good = "# comment\r\nTITLE \"tiny\"\r\n\r\nLUT_3D_SIZE 2\r\nLUT_3D_INPUT_RANGE 0 1\r\n";
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                good += std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + "\r\n";
            }
        }
    }
    const Result<Lut3D> ok = readCube(writeText("tolerant.cube", good));
    INFO((ok.ok() ? std::string("ok") : ok.error().toString()));
    REQUIRE(ok.ok());
    REQUIRE(ok.value().title == "tiny");
    REQUIRE(ok.value().size == 2);
    float out[3];
    ok.value().at(1, 0, 1, out);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 0.0f);
    REQUIRE(out[2] == 1.0f);
}
