// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_core: Result, ByteSpan/ByteReader bounds safety, Fourcc,
// MappedFile, math primitives and the thread pool.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/core/ByteReader.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/MappedFile.h"
#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/core/Version.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <random>
#include <vector>

using namespace osv;

// -----------------------------------------------------------------------------
//  Result / Status
// -----------------------------------------------------------------------------
namespace {

Result<int> parsePositive(int v) {
    if (v <= 0) {
        return Error{ErrorCode::InvalidArgument, "not positive"};
    }
    return v;
}

Result<double> halfOfPositive(int v) {
    OSV_TRY_ASSIGN(int p, parsePositive(v));
    return p * 0.5;
}

Status checkPositive(int v) {
    OSV_TRY(parsePositive(v));
    return okStatus();
}

}  // namespace

TEST_CASE("Result carries values and errors", "[core]") {
    auto good = parsePositive(5);
    REQUIRE(good.ok());
    REQUIRE(good.value() == 5);
    REQUIRE(good.code() == ErrorCode::Ok);

    auto bad = parsePositive(-1);
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error().code == ErrorCode::InvalidArgument);
    REQUIRE(bad.error().message == "not positive");
    REQUIRE(bad.valueOr(42) == 42);
    // value() on a failed result is defined behaviour (a default T).
    REQUIRE(bad.value() == 0);

    REQUIRE(halfOfPositive(8).value() == 4.0);
    REQUIRE(halfOfPositive(0).error().code == ErrorCode::InvalidArgument);
    REQUIRE(checkPositive(3).ok());
    REQUIRE_FALSE(checkPositive(0).ok());
    REQUIRE(std::string(errorCodeName(ErrorCode::Truncated)) == "Truncated");
}

TEST_CASE("Version header is generated", "[core]") {
    REQUIRE(std::string(Version::string()).size() >= 5);
    REQUIRE(Version::major + Version::minor + Version::patch >= 1);
}

// -----------------------------------------------------------------------------
//  ByteSpan / ByteReader
// -----------------------------------------------------------------------------
TEST_CASE("ByteSpan sub() clamps and never reads out of bounds", "[core]") {
    const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    ByteSpan span(bytes);
    REQUIRE(span.size() == 8);
    REQUIRE(span.sub(2, 3).size() == 3);
    REQUIRE(span.sub(2, 3)[0] == 3);
    REQUIRE(span.sub(6, 100).size() == 2);
    REQUIRE(span.sub(8).empty());
    REQUIRE(span.sub(1000, 5).empty());
    REQUIRE(span.at(100) == 0);
    REQUIRE(span.contains(0, 8));
    REQUIRE_FALSE(span.contains(1, 8));
    REQUIRE(ByteSpan(nullptr, 100).empty());
}

TEST_CASE("ByteReader every width fails cleanly at the end", "[core]") {
    const std::vector<std::uint8_t> bytes = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
    ByteReader r{ByteSpan(bytes)};
    std::uint64_t u64 = 0;
    REQUIRE_FALSE(r.u64be(u64));  // only 7 bytes available
    REQUIRE(r.pos() == 0);        // cursor untouched on failure
    std::uint32_t u32 = 0;
    REQUIRE(r.u32be(u32));
    REQUIRE(u32 == 0x12345678u);
    std::uint16_t u16 = 0;
    REQUIRE(r.u16le(u16));
    REQUIRE(u16 == 0xBC9Au);
    std::uint8_t u8 = 0;
    REQUIRE(r.u8(u8));
    REQUIRE(u8 == 0xDE);
    REQUIRE(r.atEnd());
    REQUIRE_FALSE(r.u8(u8));
    REQUIRE_FALSE(r.skip(1));
    REQUIRE(r.seek(0));
    REQUIRE_FALSE(r.seek(8));
    std::uint32_t u24 = 0;
    REQUIRE(r.u24be(u24));
    REQUIRE(u24 == 0x123456u);
}

TEST_CASE("ByteReader varint decoding and limits", "[core]") {
    // 300 = 0xAC 0x02
    const std::vector<std::uint8_t> ok = {0xAC, 0x02};
    ByteReader r{ByteSpan(ok)};
    std::uint64_t v = 0;
    REQUIRE(r.varint(v));
    REQUIRE(v == 300);
    REQUIRE(r.atEnd());

    // Eleven continuation bytes: must be rejected, cursor untouched.
    const std::vector<std::uint8_t> tooLong(11, 0x80);
    ByteReader r2{ByteSpan(tooLong)};
    REQUIRE_FALSE(r2.varint(v));
    REQUIRE(r2.pos() == 0);

    // Truncated in the middle of a varint.
    const std::vector<std::uint8_t> trunc = {0x80, 0x80};
    ByteReader r3{ByteSpan(trunc)};
    REQUIRE_FALSE(r3.varint(v));

    // Maximum 64-bit value encodes in 10 bytes.
    const std::vector<std::uint8_t> maxv = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    ByteReader r4{ByteSpan(maxv)};
    REQUIRE(r4.varint(v));
    REQUIRE(v == UINT64_MAX);
}

TEST_CASE("ByteReader floats and fixed point", "[core]") {
    const float f = 829.3612f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const std::vector<std::uint8_t> bytes = {static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8),
                                             static_cast<std::uint8_t>(bits >> 16),
                                             static_cast<std::uint8_t>(bits >> 24), 0x00, 0x01, 0x80, 0x00};
    ByteReader r{ByteSpan(bytes)};
    float out = 0;
    REQUIRE(r.f32le(out));
    REQUIRE(out == f);
    double fixed = 0;
    REQUIRE(r.fixed1616be(fixed));
    REQUIRE(fixed == 1.5);
}

TEST_CASE("Fourcc literal and printing", "[core]") {
    constexpr Fourcc moov{"moov"};
    REQUIRE(moov.v == 0x6D6F6F76u);
    REQUIRE(moov.str() == "moov");
    REQUIRE(Fourcc::fromBytes(0xA9, 't', 'o', 'o').str() == "?too");
    REQUIRE(Fourcc{0x0000ABCDu}.hex() == "0x0000abcd");
    const std::vector<std::uint8_t> bytes = {'h', 'v', 'c', '1'};
    ByteReader r{ByteSpan(bytes)};
    Fourcc fc;
    REQUIRE(r.fourcc(fc));
    REQUIRE(fc == Fourcc{"hvc1"});
}

// -----------------------------------------------------------------------------
//  MappedFile
// -----------------------------------------------------------------------------
TEST_CASE("MappedFile reports missing files as Io errors", "[core]") {
    auto r = MappedFile::open(osvtest::tempDir() / "does-not-exist.bin");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == ErrorCode::Io);
}

TEST_CASE("MappedFile maps a real file and wraps buffers", "[core]") {
    const auto path = osvtest::tempDir() / "mapped.bin";
    {
        std::ofstream out(path, std::ios::binary);
        for (int i = 0; i < 4096; ++i) {
            out.put(static_cast<char>(i & 0xFF));
        }
    }
    auto r = MappedFile::open(path);
    REQUIRE(r.ok());
    const MappedFile& f = r.value();
    REQUIRE(f.size() == 4096);
    REQUIRE(f.span()[0] == 0);
    REQUIRE(f.span()[255] == 255);
    REQUIRE(f.span()[4095] == 255);

    MappedFile mem = MappedFile::fromBuffer({9, 8, 7});
    REQUIRE(mem.size() == 3);
    REQUIRE(mem.span()[1] == 8);
    REQUIRE_FALSE(mem.isMapped());

    MappedFile moved = std::move(mem);
    REQUIRE(moved.size() == 3);
    REQUIRE(mem.size() == 0);  // NOLINT(bugprone-use-after-move) - intentional check
}

// -----------------------------------------------------------------------------
//  Math
// -----------------------------------------------------------------------------
TEST_CASE("Mat3d rotations are orthonormal and compose", "[core]") {
    const Mat3d r = Mat3d::rotZ(deg2rad(30)) * Mat3d::rotX(deg2rad(-70)) * Mat3d::rotY(deg2rad(12));
    const Mat3d shouldBeIdentity = r * r.transposed();
    REQUIRE(shouldBeIdentity.distance(Mat3d::identity()) < 1e-12);
    REQUIRE_THAT(r.determinant(), Catch::Matchers::WithinAbs(1.0, 1e-12));

    // rotZ(90) maps +X to +Y (right-handed).
    const Vec3d v = Mat3d::rotZ(kHalfPi) * Vec3d{1, 0, 0};
    REQUIRE_THAT(v.y, Catch::Matchers::WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(v.x, Catch::Matchers::WithinAbs(0.0, 1e-12));

    // axisAngle agrees with the named rotations.
    REQUIRE(Mat3d::axisAngle({0, 0, 1}, 0.7).distance(Mat3d::rotZ(0.7)) < 1e-12);
    REQUIRE(Mat3d::axisAngle({0, 0, 0}, 0.7).distance(Mat3d::identity()) < 1e-12);
}

TEST_CASE("Quatd converts to and from matrices", "[core]") {
    const Quatd q = Quatd::fromAxisAngle({0.3, -0.5, 0.8}, 1.234);
    REQUIRE_THAT(q.norm(), Catch::Matchers::WithinAbs(1.0, 1e-12));
    const Mat3d m = q.toMatrix();
    REQUIRE(m.distance(Mat3d::axisAngle({0.3, -0.5, 0.8}, 1.234)) < 1e-12);
    const Quatd back = Quatd::fromMatrix(m);
    REQUIRE(back.angleTo(q) < 1e-9);

    // rotate() and toMatrix() agree.
    const Vec3d v{0.2, 0.9, -0.4};
    const Vec3d a = q.rotate(v);
    const Vec3d b = m * v;
    REQUIRE((a - b).norm() < 1e-12);

    // Component-order factories.
    const Quatd w = Quatd::fromWXYZ(1, 2, 3, 4);
    const Quatd x = Quatd::fromXYZW(2, 3, 4, 1);
    REQUIRE(w.w == x.w);
    REQUIRE(w.z == x.z);

    // Hamilton product order: (a*b) applies b first.
    const Quatd rz = Quatd::fromAxisAngle({0, 0, 1}, kHalfPi);
    const Quatd rx = Quatd::fromAxisAngle({1, 0, 0}, kHalfPi);
    const Vec3d viaQ = (rz * rx).rotate({0, 1, 0});
    const Vec3d viaM = (Mat3d::rotZ(kHalfPi) * Mat3d::rotX(kHalfPi)) * Vec3d{0, 1, 0};
    REQUIRE((viaQ - viaM).norm() < 1e-12);
}

TEST_CASE("Quatd slerp endpoints, shortest path and midpoint", "[core]") {
    const Quatd a = Quatd::fromAxisAngle({0, 0, 1}, 0.0);
    const Quatd b = Quatd::fromAxisAngle({0, 0, 1}, deg2rad(90));
    REQUIRE(Quatd::slerp(a, b, 0.0).angleTo(a) < 1e-12);
    REQUIRE(Quatd::slerp(a, b, 1.0).angleTo(b) < 1e-12);
    const Quatd mid = Quatd::slerp(a, b, 0.5);
    REQUIRE_THAT(rad2deg(mid.angleTo(a)), Catch::Matchers::WithinAbs(45.0, 1e-9));

    // -b is the same rotation; slerp must take the short way round.
    const Quatd negB{-b.w, -b.x, -b.y, -b.z};
    const Quatd mid2 = Quatd::slerp(a, negB, 0.5);
    REQUIRE_THAT(rad2deg(mid2.angleTo(a)), Catch::Matchers::WithinAbs(45.0, 1e-9));

    // Nearly identical inputs use the nlerp path and stay normalised.
    const Quatd c = Quatd::fromAxisAngle({0, 0, 1}, 1e-5);
    REQUIRE_THAT(Quatd::slerp(a, c, 0.3).norm(), Catch::Matchers::WithinAbs(1.0, 1e-12));
}

TEST_CASE("Vec3d helpers", "[core]") {
    REQUIRE(Vec3d{0, 0, 0}.normalized().norm() == 0.0);
    const double rightAngle = Vec3d{1, 0, 0}.angleTo(Vec3d{0, 1, 0});
    REQUIRE_THAT(rightAngle, Catch::Matchers::WithinAbs(kHalfPi, 1e-12));
    const Vec3d c = Vec3d{1, 0, 0}.cross(Vec3d{0, 1, 0});
    REQUIRE(c.z == 1.0);
    REQUIRE(clampd(5.0, 1.0, 2.0) == 2.0);
    REQUIRE(clampd(5.0, 2.0, 1.0) == 2.0);  // inverted bounds are tolerated
}

// -----------------------------------------------------------------------------
//  ThreadPool
// -----------------------------------------------------------------------------
TEST_CASE("ThreadPool parallelFor covers every index exactly once", "[core]") {
    ThreadPool pool(4);
    REQUIRE(pool.size() == 4);
    const std::size_t n = 1'000'000;
    std::vector<std::atomic<std::uint8_t>> hits(n);
    for (auto& h : hits) {
        h.store(0);
    }
    for (const std::size_t grain : {std::size_t{1}, std::size_t{7}, std::size_t{4096}, std::size_t{10'000'000}}) {
        for (auto& h : hits) {
            h.store(0);
        }
        auto st = pool.parallelFor(0, n, grain, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                hits[i].fetch_add(1);
            }
        });
        REQUIRE(st.ok());
        std::size_t total = 0;
        for (auto& h : hits) {
            REQUIRE(h.load() == 1);
            ++total;
        }
        REQUIRE(total == n);
    }
}

TEST_CASE("ThreadPool handles empty ranges, null bodies and exceptions", "[core]") {
    ThreadPool pool(2);
    REQUIRE(pool.parallelFor(10, 10, 1, [](std::size_t, std::size_t) {}).ok());
    REQUIRE(pool.parallelFor(0, 10, 1, nullptr).error().code == ErrorCode::InvalidArgument);
    auto st = pool.parallelFor(0, 100, 10, [](std::size_t b, std::size_t) {
        if (b == 50) {
            throw std::runtime_error("boom");
        }
    });
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.error().code == ErrorCode::Internal);
    REQUIRE(st.error().message.find("boom") != std::string::npos);
    // The pool is still usable afterwards.
    std::atomic<int> rows{0};
    REQUIRE(pool.parallelRows(64, 8, [&](std::size_t) { rows.fetch_add(1); }).ok());
    REQUIRE(rows.load() == 64);
    REQUIRE(ThreadPool::global().size() >= 1);
}
