// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// .cube 3D LUT writer / reader.  The writer streams the table through a
// std::string buffer (one flush per file) so 65^3 = 274 625 lines take a few
// milliseconds; the reader is a tolerant line parser that ignores comments and
// unknown keywords.

#include "osv/color/Cube.h"

#include "osv/core/Log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

namespace osv::color {

namespace {

/// Largest grid the writer accepts (256^3 * 3 floats = 200 MB of text).
constexpr std::uint32_t kMaxCubeSize = 256;

/// Keep TITLE ASCII, single line and free of quotes so any parser copes.
std::string sanitiseTitle(std::string_view title) {
    std::string out;
    out.reserve(title.size());
    for (const char ch : title) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u >= 0x7F || ch == '"') {
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/// Trim ASCII whitespace from both ends.
std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Parse up to three floats from a line; returns how many were read.
int parseFloats(std::string_view text, float out[3]) noexcept {
    int count = 0;
    std::string buf(text);
    const char* p = buf.c_str();
    while (count < 3) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) {
            break;
        }
        if (!std::isfinite(v)) {
            return -1;
        }
        out[count++] = static_cast<float>(v);
        p = end;
    }
    // Anything left that is not whitespace means the line is not pure data.
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '\0') {
        return -1;
    }
    return count;
}

/// True when `line` starts with `keyword` followed by end or whitespace.
bool startsWithKeyword(std::string_view line, std::string_view keyword) noexcept {
    if (line.size() < keyword.size() || line.substr(0, keyword.size()) != keyword) {
        return false;
    }
    return line.size() == keyword.size() || line[keyword.size()] == ' ' || line[keyword.size()] == '\t';
}

}  // namespace

// -----------------------------------------------------------------------------
//  Lut3D
// -----------------------------------------------------------------------------
bool Lut3D::valid() const noexcept {
    if (size < 2 || size > kMaxCubeSize) {
        return false;
    }
    const std::size_t expect = static_cast<std::size_t>(size) * size * size * 3u;
    return data.size() == expect;
}

void Lut3D::at(std::uint32_t r, std::uint32_t g, std::uint32_t b, float out[3]) const noexcept {
    if (!out) {
        return;
    }
    if (!valid()) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    // Clamp the indices so a caller can never read outside the table.
    const std::uint32_t last = size - 1;
    r = r > last ? last : r;
    g = g > last ? last : g;
    b = b > last ? last : b;
    const std::size_t idx = (static_cast<std::size_t>(b) * size * size + static_cast<std::size_t>(g) * size + r) * 3u;
    out[0] = data[idx];
    out[1] = data[idx + 1];
    out[2] = data[idx + 2];
}

bool Lut3D::sample(const float in[3], float out[3]) const noexcept {
    if (!in || !out) {
        return false;
    }
    if (!valid()) {
        out[0] = out[1] = out[2] = 0.0f;
        return false;
    }
    // Map each input to a fractional grid coordinate inside the domain.
    float fpos[3];
    for (int i = 0; i < 3; ++i) {
        const float span = domainMax[i] - domainMin[i];
        float t = (std::fabs(span) > 1e-12f) ? (in[i] - domainMin[i]) / span : 0.0f;
        if (!std::isfinite(t)) {
            t = 0.0f;
        }
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        fpos[i] = t * static_cast<float>(size - 1);
    }
    // Integer cell and interpolation weights per axis.
    std::uint32_t i0[3];
    std::uint32_t i1[3];
    float w[3];
    for (int i = 0; i < 3; ++i) {
        const float fl = std::floor(fpos[i]);
        i0[i] = static_cast<std::uint32_t>(fl);
        if (i0[i] >= size - 1) {
            i0[i] = size - 1;
        }
        i1[i] = i0[i] + 1 < size ? i0[i] + 1 : i0[i];
        w[i] = fpos[i] - static_cast<float>(i0[i]);
        w[i] = w[i] < 0.0f ? 0.0f : (w[i] > 1.0f ? 1.0f : w[i]);
    }
    // Eight corners, blended red -> green -> blue.
    float c[8][3];
    at(i0[0], i0[1], i0[2], c[0]);
    at(i1[0], i0[1], i0[2], c[1]);
    at(i0[0], i1[1], i0[2], c[2]);
    at(i1[0], i1[1], i0[2], c[3]);
    at(i0[0], i0[1], i1[2], c[4]);
    at(i1[0], i0[1], i1[2], c[5]);
    at(i0[0], i1[1], i1[2], c[6]);
    at(i1[0], i1[1], i1[2], c[7]);
    for (int ch = 0; ch < 3; ++ch) {
        const float x00 = c[0][ch] + (c[1][ch] - c[0][ch]) * w[0];
        const float x10 = c[2][ch] + (c[3][ch] - c[2][ch]) * w[0];
        const float x01 = c[4][ch] + (c[5][ch] - c[4][ch]) * w[0];
        const float x11 = c[6][ch] + (c[7][ch] - c[6][ch]) * w[0];
        const float y0 = x00 + (x10 - x00) * w[1];
        const float y1 = x01 + (x11 - x01) * w[1];
        out[ch] = y0 + (y1 - y0) * w[2];
    }
    return true;
}

// -----------------------------------------------------------------------------
//  Evaluation shared by the writer and the tests
// -----------------------------------------------------------------------------
void evaluateCubeEntry(const OsvColorParams& params, const CubeOptions& options, const float in[3],
                       float out[3]) noexcept {
    if (!in || !out) {
        return;
    }
    float code[3] = {in[0], in[1], in[2]};
    if (options.inputIsNarrowCode) {
        // The host feeds limited-range video values on a 0..1 scale: undo the
        // narrow-range coding so the curve sees the real log code.
        int depth = params.bitDepth;
        if (depth < 8) {
            depth = 8;
        }
        if (depth > 16) {
            depth = 16;
        }
        const float maxCode = static_cast<float>((1u << depth) - 1u);
        const float shift = static_cast<float>(1u << (depth - 8));
        const float black = 16.0f * shift;
        const float range = 219.0f * shift;
        for (int i = 0; i < 3; ++i) {
            code[i] = (code[i] * maxCode - black) / range;
            code[i] = code[i] < 0.0f ? 0.0f : (code[i] > 1.0f ? 1.0f : code[i]);
        }
    }
    osvCodeToOutput(&params, code, out);
    // A LUT must never contain NaN/inf; substitute zero defensively.
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(out[i])) {
            out[i] = 0.0f;
        }
    }
}

// -----------------------------------------------------------------------------
//  Writer
// -----------------------------------------------------------------------------
Status writeCube(const std::filesystem::path& path, const OsvColorParams& params, const CubeOptions& options) {
    // Validate the request before touching the file system.
    if (options.size < 2 || options.size > kMaxCubeSize) {
        return failStatus(ErrorCode::InvalidArgument,
                          "cube size must be in [2, " + std::to_string(kMaxCubeSize) + "]");
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(options.domainMin[i]) || !std::isfinite(options.domainMax[i]) ||
            options.domainMax[i] <= options.domainMin[i]) {
            return failStatus(ErrorCode::InvalidArgument, "cube domain must be finite with max > min");
        }
    }
    if (path.empty()) {
        return failStatus(ErrorCode::InvalidArgument, "cube path is empty");
    }

    const std::uint32_t n = options.size;
    const std::string title = options.title.empty() ? std::string("OpenOSV D-Log M LUT") : sanitiseTitle(options.title);

    // Header.
    std::string text;
    text.reserve(static_cast<std::size_t>(n) * n * n * 30u + 256u);
    char line[128];
    std::snprintf(line, sizeof(line), "TITLE \"%s\"\n", title.c_str());
    text += line;
    std::snprintf(line, sizeof(line), "LUT_3D_SIZE %u\n", n);
    text += line;
    std::snprintf(line, sizeof(line), "DOMAIN_MIN %.6f %.6f %.6f\n", static_cast<double>(options.domainMin[0]),
                  static_cast<double>(options.domainMin[1]), static_cast<double>(options.domainMin[2]));
    text += line;
    std::snprintf(line, sizeof(line), "DOMAIN_MAX %.6f %.6f %.6f\n", static_cast<double>(options.domainMax[0]),
                  static_cast<double>(options.domainMax[1]), static_cast<double>(options.domainMax[2]));
    text += line;

    // Table: red varies fastest, then green, then blue.
    const float step = 1.0f / static_cast<float>(n - 1);
    for (std::uint32_t b = 0; b < n; ++b) {
        for (std::uint32_t g = 0; g < n; ++g) {
            for (std::uint32_t r = 0; r < n; ++r) {
                const float in[3] = {
                    options.domainMin[0] + (options.domainMax[0] - options.domainMin[0]) * (static_cast<float>(r) * step),
                    options.domainMin[1] + (options.domainMax[1] - options.domainMin[1]) * (static_cast<float>(g) * step),
                    options.domainMin[2] + (options.domainMax[2] - options.domainMin[2]) * (static_cast<float>(b) * step),
                };
                float out[3] = {0.0f, 0.0f, 0.0f};
                evaluateCubeEntry(params, options, in, out);
                std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", static_cast<double>(out[0]),
                              static_cast<double>(out[1]), static_cast<double>(out[2]));
                text += line;
            }
        }
    }

    // One binary write so line endings are exactly "\n" on every platform.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return failStatus(ErrorCode::Io, "cannot create cube file: " + log::safe(path.string()));
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        return failStatus(ErrorCode::Io, "write failed for cube file: " + log::safe(path.string()));
    }
    file.close();
    log::debug("wrote {} ({}^3 entries)", log::safe(path.string()), n);
    return okStatus();
}

// -----------------------------------------------------------------------------
//  Reader
// -----------------------------------------------------------------------------
Result<Lut3D> readCube(const std::filesystem::path& path) {
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "cube path is empty"};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return Error{ErrorCode::Io, "cannot open cube file: " + log::safe(path.string())};
    }

    Lut3D lut;
    std::size_t expected = 0;   // size^3 once LUT_3D_SIZE is known
    std::size_t lineNo = 0;
    std::string raw;
    while (std::getline(file, raw)) {
        ++lineNo;
        const std::string_view line = trim(raw);
        // Blank lines and comments are ignored.
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (startsWithKeyword(line, "TITLE")) {
            std::string_view t = trim(line.substr(5));
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
                t = t.substr(1, t.size() - 2);
            }
            lut.title.assign(t.begin(), t.end());
            continue;
        }
        if (startsWithKeyword(line, "LUT_3D_SIZE")) {
            const std::string_view v = trim(line.substr(11));
            const long parsed = std::strtol(std::string(v).c_str(), nullptr, 10);
            if (parsed < 2 || parsed > static_cast<long>(kMaxCubeSize)) {
                return Error{ErrorCode::Malformed, "LUT_3D_SIZE out of range at line " + std::to_string(lineNo)};
            }
            if (lut.size != 0) {
                return Error{ErrorCode::Malformed, "duplicate LUT_3D_SIZE at line " + std::to_string(lineNo)};
            }
            lut.size = static_cast<std::uint32_t>(parsed);
            expected = static_cast<std::size_t>(lut.size) * lut.size * lut.size;
            lut.data.reserve(expected * 3u);
            continue;
        }
        if (startsWithKeyword(line, "LUT_1D_SIZE")) {
            return Error{ErrorCode::Unsupported, "1D cube LUTs are not supported"};
        }
        if (startsWithKeyword(line, "DOMAIN_MIN") || startsWithKeyword(line, "DOMAIN_MAX")) {
            float v[3] = {0.0f, 0.0f, 0.0f};
            if (parseFloats(line.substr(10), v) != 3) {
                return Error{ErrorCode::Malformed, "bad DOMAIN line at line " + std::to_string(lineNo)};
            }
            float* dst = startsWithKeyword(line, "DOMAIN_MIN") ? lut.domainMin : lut.domainMax;
            dst[0] = v[0];
            dst[1] = v[1];
            dst[2] = v[2];
            continue;
        }
        // Any other keyword (LUT_3D_INPUT_RANGE etc.) starts with a letter.
        if ((line.front() >= 'A' && line.front() <= 'Z') || (line.front() >= 'a' && line.front() <= 'z')) {
            log::debug("cube: ignoring keyword line {}: {}", lineNo, log::safe(line.substr(0, 32)));
            continue;
        }
        // Data line.
        float v[3] = {0.0f, 0.0f, 0.0f};
        if (parseFloats(line, v) != 3) {
            return Error{ErrorCode::Malformed, "bad data line at line " + std::to_string(lineNo)};
        }
        if (lut.size == 0) {
            return Error{ErrorCode::Malformed, "data before LUT_3D_SIZE at line " + std::to_string(lineNo)};
        }
        if (lut.data.size() >= expected * 3u) {
            return Error{ErrorCode::Malformed, "more data lines than LUT_3D_SIZE^3 at line " + std::to_string(lineNo)};
        }
        lut.data.push_back(v[0]);
        lut.data.push_back(v[1]);
        lut.data.push_back(v[2]);
    }
    if (file.bad()) {
        return Error{ErrorCode::Io, "read failed for cube file: " + log::safe(path.string())};
    }
    if (lut.size == 0) {
        return Error{ErrorCode::Malformed, "missing LUT_3D_SIZE"};
    }
    if (lut.data.size() != expected * 3u) {
        return Error{ErrorCode::Truncated, "cube has " + std::to_string(lut.data.size() / 3u) + " entries, expected " +
                                               std::to_string(expected)};
    }
    for (int i = 0; i < 3; ++i) {
        if (lut.domainMax[i] <= lut.domainMin[i]) {
            return Error{ErrorCode::Malformed, "cube domain max <= min"};
        }
    }
    return lut;
}

}  // namespace osv::color
