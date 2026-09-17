// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ByteReader: cursor-based, bounds-checked reads over a ByteSpan.
//
// Every read returns bool.  A failed read leaves the cursor untouched and the
// output parameter unmodified, so parsers can simply `if (!r.u32be(x)) return
// truncated();` without extra state.  Both big-endian (ISO BMFF) and little
// endian (protobuf fixed32/64) accessors are provided.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"

#include <cstring>

namespace osv {

class ByteReader {
public:
    /// Read from the beginning of `span`.
    explicit ByteReader(ByteSpan span) noexcept : m_span(span) {}

    // -------------------------------------------------------------------------
    //  Cursor
    // -------------------------------------------------------------------------

    [[nodiscard]] std::uint64_t pos() const noexcept { return m_pos; }
    [[nodiscard]] std::uint64_t size() const noexcept { return m_span.size(); }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return m_span.size() - m_pos; }
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_span.size(); }
    [[nodiscard]] ByteSpan span() const noexcept { return m_span; }

    /// Move the cursor to an absolute position (must be <= size()).
    bool seek(std::uint64_t position) noexcept {
        if (position > m_span.size()) {
            return false;
        }
        m_pos = position;
        return true;
    }

    /// Advance the cursor by `count` bytes (must not pass the end).
    bool skip(std::uint64_t count) noexcept {
        if (count > remaining()) {
            return false;
        }
        m_pos += count;
        return true;
    }

    // -------------------------------------------------------------------------
    //  Fixed width integers
    // -------------------------------------------------------------------------

    bool u8(std::uint8_t& out) noexcept {
        if (remaining() < 1) {
            return false;
        }
        out = m_span.data()[m_pos];
        m_pos += 1;
        return true;
    }

    bool u16be(std::uint16_t& out) noexcept {
        if (remaining() < 2) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
        m_pos += 2;
        return true;
    }

    bool u24be(std::uint32_t& out) noexcept {
        if (remaining() < 3) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        out = (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
        m_pos += 3;
        return true;
    }

    bool u32be(std::uint32_t& out) noexcept {
        if (remaining() < 4) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        out = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
              (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        m_pos += 4;
        return true;
    }

    bool u64be(std::uint64_t& out) noexcept {
        if (remaining() < 8) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | p[i];
        }
        out = v;
        m_pos += 8;
        return true;
    }

    bool i16be(std::int16_t& out) noexcept {
        std::uint16_t v = 0;
        if (!u16be(v)) {
            return false;
        }
        out = static_cast<std::int16_t>(v);
        return true;
    }

    bool i32be(std::int32_t& out) noexcept {
        std::uint32_t v = 0;
        if (!u32be(v)) {
            return false;
        }
        out = static_cast<std::int32_t>(v);
        return true;
    }

    bool i64be(std::int64_t& out) noexcept {
        std::uint64_t v = 0;
        if (!u64be(v)) {
            return false;
        }
        out = static_cast<std::int64_t>(v);
        return true;
    }

    bool u16le(std::uint16_t& out) noexcept {
        if (remaining() < 2) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        out = static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
        m_pos += 2;
        return true;
    }

    bool u32le(std::uint32_t& out) noexcept {
        if (remaining() < 4) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        out = p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
              (static_cast<std::uint32_t>(p[3]) << 24);
        m_pos += 4;
        return true;
    }

    bool u64le(std::uint64_t& out) noexcept {
        if (remaining() < 8) {
            return false;
        }
        const std::uint8_t* p = m_span.data() + m_pos;
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | p[i];
        }
        out = v;
        m_pos += 8;
        return true;
    }

    /// IEEE-754 single precision, little endian (protobuf fixed32 floats).
    bool f32le(float& out) noexcept {
        std::uint32_t bits = 0;
        if (!u32le(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    /// IEEE-754 double precision, little endian (protobuf fixed64 doubles).
    bool f64le(double& out) noexcept {
        std::uint64_t bits = 0;
        if (!u64le(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    /// 16.16 fixed point big endian (ISO BMFF tkhd width/height, matrices).
    bool fixed1616be(double& out) noexcept {
        std::int32_t raw = 0;
        if (!i32be(raw)) {
            return false;
        }
        out = static_cast<double>(raw) / 65536.0;
        return true;
    }

    /// Four character code (ISO BMFF box types, sample entry names).
    bool fourcc(Fourcc& out) noexcept {
        std::uint32_t v = 0;
        if (!u32be(v)) {
            return false;
        }
        out = Fourcc{v};
        return true;
    }

    // -------------------------------------------------------------------------
    //  Variable width / blobs
    // -------------------------------------------------------------------------

    /// Protobuf base-128 varint.  Rejects encodings longer than `maxBytes`
    /// (10 is the largest legal 64-bit varint) so a run of 0x80 bytes cannot
    /// be used to loop forever.
    bool varint(std::uint64_t& out, std::size_t maxBytes = 10) noexcept {
        std::uint64_t value = 0;
        std::uint64_t cursor = m_pos;
        for (std::size_t i = 0; i < maxBytes; ++i) {
            if (cursor >= m_span.size()) {
                return false;
            }
            const std::uint8_t b = m_span.data()[cursor++];
            // Bits beyond 64 are dropped for the 10th byte, matching protobuf.
            if (i < 10) {
                value |= static_cast<std::uint64_t>(b & 0x7Fu) << (7 * i);
            }
            if ((b & 0x80u) == 0) {
                out = value;
                m_pos = cursor;
                return true;
            }
        }
        return false;
    }

    /// `count` raw bytes as a sub-span (no copy).
    bool bytes(std::uint64_t count, ByteSpan& out) noexcept {
        if (count > remaining()) {
            return false;
        }
        out = m_span.sub(m_pos, count);
        m_pos += count;
        return true;
    }

    /// Copy `out.size()` bytes into a caller buffer.
    bool copyTo(std::uint8_t* dst, std::size_t count) noexcept {
        if (!dst || count > remaining()) {
            return false;
        }
        std::memcpy(dst, m_span.data() + m_pos, count);
        m_pos += count;
        return true;
    }

    /// NUL terminated string bounded by `maxLength` bytes (terminator not
    /// included in `out`).  Consumes the terminator when present.
    bool cString(std::string& out, std::size_t maxLength = 256) noexcept {
        const std::size_t limit = static_cast<std::size_t>(remaining() < maxLength ? remaining() : maxLength);
        const std::uint8_t* p = m_span.data() + m_pos;
        std::size_t len = 0;
        while (len < limit && p[len] != 0) {
            ++len;
        }
        out.assign(reinterpret_cast<const char*>(p), len);
        m_pos += len;
        if (len < remaining() && m_span.data()[m_pos] == 0) {
            m_pos += 1;
        }
        return true;
    }

private:
    ByteSpan m_span;
    std::uint64_t m_pos = 0;
};

}  // namespace osv
