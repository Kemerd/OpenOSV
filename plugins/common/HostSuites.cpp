// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "HostSuites.h"

#include "PluginLog.h"

#include <cstring>
#include <format>

namespace osv::premiere {

// -----------------------------------------------------------------------------
//  HostVersion
// -----------------------------------------------------------------------------
std::string HostVersion::toString() const {
    if (!valid) {
        return "unknown";
    }
    return std::format("{}.{}.{}", major, minor, patch);
}

std::string HostVersion::appName() const {
    if (appFourcc == 0) {
        return "????";
    }
    // Fourcc literals such as 'PPro' are big-endian character constants:
    // the first character sits in the most significant byte.
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>((appFourcc >> (8 * (3 - i))) & 0xFFu);
        s[static_cast<std::size_t>(i)] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return s;
}

HostVersion queryHostVersion(const PrSDKAppInfoSuite* appInfo) noexcept {
    HostVersion v;
    if (!appInfo || !appInfo->GetAppInfo) {
        return v;
    }

    // Each selector fills a different struct; a failure on one leaves the
    // others usable, so every call is independent.
    VersionInfo info{};
    if (appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_Version, &info) == suiteError_NoError) {
        v.major = info.major;
        v.minor = info.minor;
        v.patch = info.patch;
        v.valid = true;
    }
    csSDK_uint32 fourcc = 0;
    if (appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, &fourcc) == suiteError_NoError) {
        v.appFourcc = fourcc;
    }
    csSDK_uint32 build = 0;
    if (appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_Build, &build) == suiteError_NoError) {
        v.build = build;
    }
    return v;
}

// -----------------------------------------------------------------------------
//  ImporterSuites
// -----------------------------------------------------------------------------
int ImporterSuites::acquire(SPBasicSuite* basic) noexcept {
    release();
    if (!basic) {
        PluginLog::warn("ImporterSuites::acquire: null SPBasicSuite");
        return 0;
    }

    int count = 0;
    auto tally = [&count](bool ok) {
        if (ok) {
            ++count;
        }
    };

    // Every name/version pair below is a macro from the corresponding SDK
    // header so a typo cannot slip through.
    // Where a suite has several versions whose older structs are PREFIXES of
    // the newer ones (Adobe only ever appends members), ask for the newest
    // and fall back, so the importer still works on an older host - the
    // project targets Premiere 2026 but declares 2022+ compatibility, and a
    // hard single-version acquire turns "one feature missing" into "no suite
    // at all".
    //
    // The rule that makes a fallback SAFE is that a caller may only touch a
    // member that exists in the version actually acquired.  A shorter struct
    // means reading a later member's function pointer reads past the end of
    // what the host allocated, so a null test on it proves nothing.  Every
    // call site that uses a member added after the lowest version in its list
    // must therefore gate on version(), not just on the pointer - see
    // ImporterVideo.cpp's CreateColorManagedPPix, which is a v4 addition.
    tally(ppix.acquire(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion));
    tally(ppix2.acquireFirst(basic, kPrSDKPPix2Suite,
                             {kPrSDKPPix2SuiteVersion3, kPrSDKPPix2SuiteVersion2, kPrSDKPPix2SuiteVersion1}));
    tally(ppixCreator.acquire(basic, kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion));
    tally(ppixCreator2.acquireFirst(basic, kPrSDKPPixCreator2Suite,
                                    {kPrSDKPPixCreator2SuiteVersion4, kPrSDKPPixCreator2SuiteVersion3,
                                     kPrSDKPPixCreator2SuiteVersion2, kPrSDKPPixCreator2SuiteVersion1}));
    tally(ppixCache.acquireFirst(basic, kPrSDKPPixCacheSuite,
                                 {kPrSDKPPixCacheSuiteVersion8, kPrSDKPPixCacheSuiteVersion7}));
    tally(time.acquire(basic, kPrSDKTimeSuite, kPrSDKTimeSuiteVersion));
    tally(string.acquire(basic, kPrSDKStringSuite, kPrSDKStringSuiteVersion));
    tally(appInfo.acquire(basic, kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion));
    tally(error.acquireFirst(basic, kPrSDKErrorSuite,
                             {kPrSDKErrorSuiteVersion3, kPrSDKErrorSuiteVersion2, kPrSDKErrorSuiteVersion1}));
    // Only one version of these two has ever existed, so a list would be
    // noise; if Adobe adds v2 the single version stays correct.
    tally(colorManagement.acquire(basic, kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion1));
    tally(memory.acquireFirst(basic, kPrSDKMemoryManagerSuite,
                              {kPrSDKMemoryManagerSuiteVersion4, kPrSDKMemoryManagerSuiteVersion3,
                               kPrSDKMemoryManagerSuiteVersion2, kPrSDKMemoryManagerSuiteVersion1}));
    tally(fileManager.acquire(basic, kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion));

    PluginLog::write(PluginLog::Level::Debug, describe());
    return count;
}

void ImporterSuites::release() noexcept {
    // Reverse order of acquisition; the host does not care but it keeps the
    // log symmetrical when tracing.
    fileManager.release();
    memory.release();
    colorManagement.release();
    error.release();
    appInfo.release();
    string.release();
    time.release();
    ppixCache.release();
    ppixCreator2.release();
    ppixCreator.release();
    ppix2.release();
    ppix.release();
}

HostVersion ImporterSuites::hostVersion() const noexcept { return queryHostVersion(appInfo.get()); }

std::string ImporterSuites::describe() const {
    std::string s = "suites:";
    auto add = [&s](const char* label, bool present, int version) {
        s += ' ';
        s += label;
        if (present) {
            s += '=';
            s += std::to_string(version);
        } else {
            s += "=none";
        }
    };
    add("PPix", static_cast<bool>(ppix), ppix.version());
    add("PPix2", static_cast<bool>(ppix2), ppix2.version());
    add("PPixCreator", static_cast<bool>(ppixCreator), ppixCreator.version());
    add("PPixCreator2", static_cast<bool>(ppixCreator2), ppixCreator2.version());
    add("PPixCache", static_cast<bool>(ppixCache), ppixCache.version());
    add("Time", static_cast<bool>(time), time.version());
    add("String", static_cast<bool>(string), string.version());
    add("AppInfo", static_cast<bool>(appInfo), appInfo.version());
    add("Error", static_cast<bool>(error), error.version());
    add("ColorManagement", static_cast<bool>(colorManagement), colorManagement.version());
    add("MemoryManager", static_cast<bool>(memory), memory.version());
    add("ImporterFileManager", static_cast<bool>(fileManager), fileManager.version());
    const HostVersion hv = hostVersion();
    s += " host=" + hv.appName() + " " + hv.toString();
    return s;
}

}  // namespace osv::premiere
