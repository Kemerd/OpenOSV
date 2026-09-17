// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ByteSpan: a non-owning, bounds-safe view of immutable bytes.
//
// Everything that reads file data in OpenOSV goes through ByteSpan / ByteReader
// so that a truncated or hostile file can never cause an out-of-bounds read.
// sub() clamps instead of asserting: a request past the end yields an empty
// (or shortened) span, which the caller detects through size().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osv {

class ByteSpan {
public:
    /// Empty span.
    constexpr ByteSpan() noexcept = default;

    /// View over `size` bytes starting at `data`.  A null `data` with a
    /// non-zero size is treated as empty (defensive against garbage input).
    constexpr ByteSpan(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(data ? size : 0) {}

    /// View over a byte vector (the vector must outlive the span).
    explicit ByteSpan(const std::vector<std::uint8_t>& bytes) noexcept : ByteSpan(bytes.data(), bytes.size()) {}

    [[nodiscard]] constexpr const std::uint8_t* data() const noexcept { return m_data; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }

    [[nodiscard]] constexpr const std::uint8_t* begin() const noexcept { return m_data; }
    [[nodiscard]] constexpr const std::uint8_t* end() const noexcept { return m_data + m_size; }

    /// Byte at `index`, or 0 when out of range (never UB).
    [[nodiscard]] constexpr std::uint8_t at(std::size_t index) const noexcept {
        return index < m_size ? m_data[index] : std::uint8_t{0};
    }
    [[nodiscard]] constexpr std::uint8_t operator[](std::size_t index) const noexcept { return at(index); }

    /// Sub-span [offset, offset + length).  Both bounds are clamped to the
    /// parent; `length` of SIZE_MAX means "to the end".
    [[nodiscard]] constexpr ByteSpan sub(std::uint64_t offset, std::uint64_t length = UINT64_MAX) const noexcept {
        if (offset >= m_size) {
            return ByteSpan{};
        }
        const std::uint64_t available = static_cast<std::uint64_t>(m_size) - offset;
        const std::uint64_t take = length < available ? length : available;
        return ByteSpan{m_data + offset, static_cast<std::size_t>(take)};
    }

    /// True when [offset, offset + length) lies entirely inside the span.
    [[nodiscard]] constexpr bool contains(std::uint64_t offset, std::uint64_t length) const noexcept {
        if (offset > m_size) {
            return false;
        }
        return length <= static_cast<std::uint64_t>(m_size) - offset;
    }

    /// Copy the bytes into a std::string (used for protobuf string fields).
    [[nodiscard]] std::string toString() const {
        return std::string(reinterpret_cast<const char*>(m_data), m_size);
    }

    /// Copy into a fresh vector.
    [[nodiscard]] std::vector<std::uint8_t> toVector() const {
        return std::vector<std::uint8_t>(m_data, m_data + m_size);
    }

    /// Byte-wise equality of two spans (same size and same content).
    [[nodiscard]] bool equals(ByteSpan other) const noexcept {
        if (m_size != other.m_size) {
            return false;
        }
        for (std::size_t i = 0; i < m_size; ++i) {
            if (m_data[i] != other.m_data[i]) {
                return false;
            }
        }
        return true;
    }

private:
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
};

}  // namespace osv
