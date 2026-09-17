// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HostSuites: RAII acquisition of SweetPea (PICA) suites through the
// SPBasicSuite the host hands every plug-in.
//
// Premiere returns a NULL suite pointer (and an error) for a (name, version)
// pair it does not implement; per docs/PREMIERE.md that is a graceful
// downgrade, never a crash.  SuiteHandle therefore never throws and simply
// reads as false when the suite is missing.  Every acquire is balanced by a
// release in the destructor, which keeps the host's reference counts exact
// even on early-return error paths.
//
// The Adobe headers are pack(push, 1); this header only holds pointers to
// their structs and defines its own types outside any packed region.
#pragma once

#include "PrSDKTypes.h"
#include "SPBasic.h"

#include "PrSDKAppInfoSuite.h"
#include "PrSDKColorManagementSuite.h"
#include "PrSDKErrorSuite.h"
#include "PrSDKImporterFileManagerSuite.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixCreator2Suite.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKTimeSuite.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace osv::premiere {

/// Owns one acquired suite of type SuiteT.  Move-only; releasing happens in
/// the destructor or through release().
template <class SuiteT>
class SuiteHandle {
public:
    SuiteHandle() = default;

    /// Acquire immediately; check operator bool afterwards.
    SuiteHandle(SPBasicSuite* basic, const char* name, int version) noexcept { acquire(basic, name, version); }

    ~SuiteHandle() { release(); }

    SuiteHandle(const SuiteHandle&) = delete;
    SuiteHandle& operator=(const SuiteHandle&) = delete;

    SuiteHandle(SuiteHandle&& other) noexcept
        : m_basic(other.m_basic), m_suite(other.m_suite), m_name(other.m_name), m_version(other.m_version) {
        other.m_basic = nullptr;
        other.m_suite = nullptr;
        other.m_name = nullptr;
        other.m_version = 0;
    }

    SuiteHandle& operator=(SuiteHandle&& other) noexcept {
        if (this != &other) {
            release();
            m_basic = other.m_basic;
            m_suite = other.m_suite;
            m_name = other.m_name;
            m_version = other.m_version;
            other.m_basic = nullptr;
            other.m_suite = nullptr;
            other.m_name = nullptr;
            other.m_version = 0;
        }
        return *this;
    }

    /// Acquire (name, version).  Any previously held suite is released
    /// first.  Returns true when the host handed back a non-null suite.
    /// `name` must outlive the handle (string literals from the SDK headers
    /// are the intended use).
    bool acquire(SPBasicSuite* basic, const char* name, int version) noexcept {
        release();
        if (!basic || !basic->AcquireSuite || !name || version <= 0) {
            return false;
        }
        const void* suite = nullptr;
        const SPErr err = basic->AcquireSuite(name, version, &suite);
        if (err != kSPNoError || !suite) {
            // Some hosts return an error but leave a pointer; never trust
            // that pointer.  Balance the acquire only when it succeeded.
            if (err == kSPNoError && suite) {
                basic->ReleaseSuite(name, version);
            }
            return false;
        }
        m_basic = basic;
        m_suite = static_cast<const SuiteT*>(suite);
        m_name = name;
        m_version = version;
        return true;
    }

    /// Try each version in order and keep the first the host provides.
    /// Used for suites whose newer versions are strict supersets (PPix Cache
    /// 8 -> 7, Video Segment 9 -> 6).
    bool acquireFirst(SPBasicSuite* basic, const char* name, std::initializer_list<int> versions) noexcept {
        for (const int v : versions) {
            if (acquire(basic, name, v)) {
                return true;
            }
        }
        return false;
    }

    /// Release the suite (no-op when nothing is held).
    void release() noexcept {
        if (m_basic && m_suite && m_name && m_basic->ReleaseSuite) {
            m_basic->ReleaseSuite(m_name, m_version);
        }
        m_basic = nullptr;
        m_suite = nullptr;
        m_name = nullptr;
        m_version = 0;
    }

    /// The suite, or nullptr.
    [[nodiscard]] const SuiteT* get() const noexcept { return m_suite; }
    [[nodiscard]] const SuiteT* operator->() const noexcept { return m_suite; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_suite != nullptr; }

    /// Version actually acquired (0 when none).
    [[nodiscard]] int version() const noexcept { return m_version; }
    /// Suite name (nullptr when none).
    [[nodiscard]] const char* name() const noexcept { return m_name; }

private:
    SPBasicSuite* m_basic = nullptr;
    const SuiteT* m_suite = nullptr;
    const char* m_name = nullptr;
    int m_version = 0;
};

/// Host application identity as reported by the App Info Suite.
struct HostVersion {
    unsigned major = 0;         ///< 0 when unavailable.
    unsigned minor = 0;
    unsigned patch = 0;
    csSDK_uint32 appFourcc = 0; ///< kAppPremierePro, kAppMediaEncoder, ... or 0.
    csSDK_uint32 build = 0;     ///< 0 when unavailable.
    bool valid = false;         ///< True when the suite answered.

    /// "26.2.2" or "unknown".
    [[nodiscard]] std::string toString() const;

    /// Four printable characters of appFourcc ("PPro"), "????" when zero.
    [[nodiscard]] std::string appName() const;
};

/// The set of suites the importer uses, acquired in one go.  Each pointer
/// may be null when the host does not provide that suite; callers test each
/// one before use.  The PPix Cache suite is tried at v8 then v7 (v7 added
/// the colour-space aware calls in Premiere 13.0; the entries the importer
/// uses have the same offsets in both layouts).
struct ImporterSuites {
    SuiteHandle<PrSDKPPixSuite> ppix;                          ///< "Premiere PPix Suite" v1
    SuiteHandle<PrSDKPPix2Suite> ppix2;                        ///< "Premiere PPix 2 Suite" v3
    SuiteHandle<PrSDKPPixCreatorSuite> ppixCreator;            ///< "Premiere PPix Creator Suite" v1
    SuiteHandle<PrSDKPPixCreator2Suite> ppixCreator2;          ///< "Premiere PPix Creator 2 Suite" v4
    SuiteHandle<PrSDKPPixCacheSuite> ppixCache;                ///< "Premiere PPix Cache Suite" v8, else v7
    SuiteHandle<PrSDKTimeSuite> time;                          ///< "Premiere Time Suite" v1
    SuiteHandle<PrSDKStringSuite> string;                      ///< "MediaCore StringSuite" v1
    SuiteHandle<PrSDKAppInfoSuite> appInfo;                    ///< "MediaCore App Info Suite" v3
    SuiteHandle<PrSDKErrorSuite3> error;                       ///< "Premiere Error Suite" v3
    SuiteHandle<PrSDKColorManagementSuite> colorManagement;    ///< "Color Management Suite" v1
    SuiteHandle<PrSDKMemoryManagerSuite> memory;               ///< "Premiere Memory Manager Suite" v4
    SuiteHandle<PrSDKImporterFileManagerSuite> fileManager;    ///< "Importer File Manager Suite" v4

    /// Acquire every suite.  Returns the number of suites obtained; the
    /// importer decides which ones are mandatory (PPix, PPix Creator, Time
    /// and String at minimum).
    int acquire(SPBasicSuite* basic) noexcept;

    /// Release everything (also done by the destructor of each member).
    void release() noexcept;

    /// True when the suites the importer cannot work without are present.
    [[nodiscard]] bool hasEssentials() const noexcept {
        return ppix && ppixCreator && time && string;
    }

    /// Host version from the App Info Suite; zeros when unavailable.
    [[nodiscard]] HostVersion hostVersion() const noexcept;

    /// One line listing which suites (and versions) were obtained, for the
    /// log.
    [[nodiscard]] std::string describe() const;
};

/// Query the App Info Suite directly (used by code paths that hold the
/// suite but not an ImporterSuites bundle).
[[nodiscard]] HostVersion queryHostVersion(const PrSDKAppInfoSuite* appInfo) noexcept;

}  // namespace osv::premiere
