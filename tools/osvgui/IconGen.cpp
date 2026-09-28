// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// IconGen.cpp - a build-time helper: writes osvgui.ico from the procedural
// icon (Icon.h), so the executable gets an Explorer / taskbar icon without
// an image file in the repository.
//
//     osvgui_icongen <out.ico>
//
// Every size is a 32-bit BMP entry (BGRA with alpha, plus the 1-bit AND
// mask older shells read), which every Windows since Vista shows, 256 x 256
// included.

#include "Icon.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// Append a little-endian integer of `bytes` bytes.
void put(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

/// One icon image: BITMAPINFOHEADER, bottom-up BGRA rows, the AND mask.
std::vector<std::uint8_t> iconImage(int size) {
    const std::vector<std::uint8_t> rgba = osvgui::renderAppIcon(size);
    const auto s = static_cast<std::uint32_t>(size);
    const std::uint32_t maskStride = ((s + 31u) / 32u) * 4u;  // 1 bpp rows, padded to 32 bits
    const std::uint32_t xorBytes = s * s * 4u;
    const std::uint32_t andBytes = maskStride * s;

    std::vector<std::uint8_t> out;
    out.reserve(40 + xorBytes + andBytes);
    // BITMAPINFOHEADER; the height counts the XOR image and the mask.
    put(out, 40, 4);
    put(out, s, 4);
    put(out, s * 2u, 4);
    put(out, 1, 2);   // planes
    put(out, 32, 2);  // bits per pixel
    put(out, 0, 4);   // BI_RGB
    put(out, xorBytes + andBytes, 4);
    put(out, 0, 4);
    put(out, 0, 4);
    put(out, 0, 4);
    put(out, 0, 4);

    // XOR image, bottom row first, BGRA.
    for (std::uint32_t y = 0; y < s; ++y) {
        const std::uint32_t row = s - 1u - y;
        for (std::uint32_t x = 0; x < s; ++x) {
            const std::size_t i = (static_cast<std::size_t>(row) * s + x) * 4u;
            out.push_back(rgba[i + 2]);
            out.push_back(rgba[i + 1]);
            out.push_back(rgba[i + 0]);
            out.push_back(rgba[i + 3]);
        }
    }
    // AND mask, bottom row first: 1 where the icon is fully transparent.
    for (std::uint32_t y = 0; y < s; ++y) {
        const std::uint32_t row = s - 1u - y;
        std::vector<std::uint8_t> bits(maskStride, 0);
        for (std::uint32_t x = 0; x < s; ++x) {
            const std::size_t i = (static_cast<std::size_t>(row) * s + x) * 4u;
            if (rgba[i + 3] == 0) {
                bits[x / 8u] = static_cast<std::uint8_t>(bits[x / 8u] | (0x80u >> (x % 8u)));
            }
        }
        out.insert(out.end(), bits.begin(), bits.end());
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1] || !*argv[1]) {
        std::fprintf(stderr, "usage: osvgui_icongen <out.ico>\n");
        return 1;
    }
    try {
        static constexpr int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
        std::vector<std::vector<std::uint8_t>> images;
        for (int size : kSizes) {
            images.push_back(iconImage(size));
        }

        // ICONDIR, then one ICONDIRENTRY per image, then the images.
        std::vector<std::uint8_t> file;
        put(file, 0, 2);
        put(file, 1, 2);  // type: icon
        put(file, static_cast<std::uint32_t>(images.size()), 2);
        std::uint32_t offset = 6u + 16u * static_cast<std::uint32_t>(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) {
            const int size = kSizes[i];
            file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));  // 0 means 256
            file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
            file.push_back(0);  // palette colours
            file.push_back(0);  // reserved
            put(file, 1, 2);    // planes
            put(file, 32, 2);   // bits per pixel
            put(file, static_cast<std::uint32_t>(images[i].size()), 4);
            put(file, offset, 4);
            offset += static_cast<std::uint32_t>(images[i].size());
        }
        for (const auto& image : images) {
            file.insert(file.end(), image.begin(), image.end());
        }

        std::FILE* f = std::fopen(argv[1], "wb");
        if (!f) {
            std::fprintf(stderr, "osvgui_icongen: cannot write %s\n", argv[1]);
            return 1;
        }
        const std::size_t wrote = std::fwrite(file.data(), 1, file.size(), f);
        const bool closed = std::fclose(f) == 0;
        if (wrote != file.size() || !closed) {
            std::fprintf(stderr, "osvgui_icongen: short write to %s\n", argv[1]);
            return 1;
        }
        return 0;
    } catch (...) {
        std::fprintf(stderr, "osvgui_icongen: out of memory\n");
        return 1;
    }
}
