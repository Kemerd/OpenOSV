// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Result<T> / Status: the only error channel used inside OpenOSV.
//
// The library is loaded into host applications (Premiere Pro, Resolve, ...)
// where an escaping exception means a crash of somebody else's process, so
// nothing in the public API throws.  Every fallible call returns a Result and
// callers propagate with OSV_TRY.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <optional>
#include <type_traits>

namespace osv {

/// Coarse error categories.  Fine-grained detail goes into Error::message.
enum class ErrorCode : std::uint16_t {
    Ok = 0,           ///< Not an error (never stored in a failed Result).
    InvalidArgument,  ///< Caller passed something nonsensical (null, out of range).
    Truncated,        ///< Input ended before a structure was complete.
    Malformed,        ///< Input violates the format specification.
    NotFound,         ///< A requested item does not exist (track, field, file).
    Unsupported,      ///< Valid but not implemented (codec, mode, backend).
    Io,               ///< Operating system / file system failure.
    Decoder,          ///< Video decoder (FFmpeg) failure.
    Gpu,              ///< CUDA / OpenCL failure.
    Internal          ///< Invariant violated inside the library (bug).
};

/// Human readable, stable name of an ErrorCode (for logs and JSON output).
[[nodiscard]] constexpr const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::Truncated: return "Truncated";
    case ErrorCode::Malformed: return "Malformed";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Io: return "Io";
    case ErrorCode::Decoder: return "Decoder";
    case ErrorCode::Gpu: return "Gpu";
    case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

/// An error value: category plus a free-form message meant for humans.
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    /// Convenience formatting: "Malformed: box size exceeds parent".
    [[nodiscard]] std::string toString() const { return std::string(errorCodeName(code)) + ": " + message; }
};

/// Either a value of type T or an Error.  T may be any move-constructible type
/// including std::monostate (see Status).
template <class T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    /// Construct a successful result holding `value`.
    static Result ok(T value) { return Result(std::move(value)); }

    /// Construct a failed result.
    static Result fail(ErrorCode code, std::string message) { return Result(Error{code, std::move(message)}); }

    /// Construct a failed result from an existing Error (used by OSV_TRY).
    static Result fail(Error error) { return Result(std::move(error)); }

    /// Implicit conversion from a value keeps call sites short: `return T{...};`
    Result(T value) : m_data(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)

    /// Implicit conversion from an Error so OSV_TRY can forward failures
    /// across differently typed Results.
    Result(Error error) : m_data(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

    /// True when the Result carries a value.
    [[nodiscard]] bool ok() const noexcept { return m_data.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    /// Access the value.  Precondition: ok().  Violating it is a programming
    /// error; we deliberately do not throw and instead return a reference to
    /// a static default to keep the process alive (and log loudly elsewhere).
    [[nodiscard]] const T& value() const& noexcept {
        if (ok()) {
            return std::get<0>(m_data);
        }
        return fallbackValue();
    }
    [[nodiscard]] T& value() & noexcept {
        if (ok()) {
            return std::get<0>(m_data);
        }
        return fallbackValue();
    }
    [[nodiscard]] T&& value() && noexcept {
        if (ok()) {
            return std::get<0>(std::move(m_data));
        }
        return std::move(fallbackValue());
    }

    /// Access the error.  Precondition: !ok(); returns a static "not an error"
    /// Error otherwise so callers never dereference garbage.
    [[nodiscard]] const Error& error() const noexcept {
        if (!ok()) {
            return std::get<1>(m_data);
        }
        static const Error kNoError{ErrorCode::Ok, "no error"};
        return kNoError;
    }

    /// Value or a caller-supplied default when failed.
    [[nodiscard]] T valueOr(T fallback) const& {
        if (ok()) {
            return std::get<0>(m_data);
        }
        return fallback;
    }

    /// Error code, or ErrorCode::Ok when successful.
    [[nodiscard]] ErrorCode code() const noexcept { return ok() ? ErrorCode::Ok : error().code; }

private:
    /// Shared per-type fallback so value() on a failed Result is defined
    /// behaviour (returns a default constructed T, or a zeroed T).
    static T& fallbackValue() noexcept {
        static T instance{};
        return instance;
    }

    std::variant<T, Error> m_data;
};

/// A Result that carries no value: success or an Error.
using Status = Result<std::monostate>;

/// Successful Status literal.
[[nodiscard]] inline Status okStatus() { return Status::ok(std::monostate{}); }

/// Failed Status literal.
[[nodiscard]] inline Status failStatus(ErrorCode code, std::string message) {
    return Status::fail(code, std::move(message));
}

}  // namespace osv

/// Evaluate `expr` (a Result); on failure return its Error from the current
/// function (which must itself return some Result).  On success the macro is
/// a no-op statement.  Use OSV_TRY_ASSIGN to also capture the value.
#define OSV_TRY(expr)                                                                                                  \
    do {                                                                                                               \
        auto&& osvTryResult_ = (expr);                                                                                 \
        if (!osvTryResult_.ok()) {                                                                                     \
            return ::osv::Error(osvTryResult_.error());                                                                \
        }                                                                                                              \
    } while (0)

/// `OSV_TRY_ASSIGN(auto x, fallibleCall());` - declares `x` with the value or
/// returns the error.  The declaration must be a single declarator.
#define OSV_TRY_ASSIGN(decl, expr)                                                                                     \
    auto&& OSV_CONCAT_(osvTryTmp_, __LINE__) = (expr);                                                                 \
    if (!OSV_CONCAT_(osvTryTmp_, __LINE__).ok()) {                                                                     \
        return ::osv::Error(OSV_CONCAT_(osvTryTmp_, __LINE__).error());                                                \
    }                                                                                                                  \
    decl = std::move(OSV_CONCAT_(osvTryTmp_, __LINE__)).value()

#define OSV_CONCAT_IMPL_(a, b) a##b
#define OSV_CONCAT_(a, b) OSV_CONCAT_IMPL_(a, b)
