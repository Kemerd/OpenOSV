// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The macOS bundle packaging (cmake/OsvMacBundle.cmake and
// OsvMacBundleDylibs.cmake), checked on the probe bundle built exactly like
// the Premiere plug-ins: what is inside it, what it exports, what it loads
// and whether it is signed.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kBinary = OSV_BUNDLE_PROBE_BINARY;
const fs::path kBundle = OSV_BUNDLE_PROBE_DIR;

/// Output of a shell command and its exit status.
struct Run {
    int status = -1;
    std::string output;
};

Run run(const std::string& command) {
    Run r;
    FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        return r;
    }
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        r.output.append(buffer, got);
    }
    const int status = ::pclose(pipe);
    r.status = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

std::string quoted(const fs::path& p) { return "'" + p.string() + "'"; }

}  // namespace

TEST_CASE("a bundle carries its FFmpeg under OpenOSV names", "[macos][bundle]") {
    REQUIRE(fs::is_regular_file(kBinary));
    REQUIRE(fs::is_regular_file(kBundle / "Contents" / "Info.plist"));
    const fs::path frameworks = kBundle / "Contents" / "Frameworks";
    REQUIRE(fs::is_directory(frameworks));

    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(frameworks)) {
        names.push_back(entry.path().filename().string());
    }
    INFO("Frameworks: " << names.size() << " file(s)");
    REQUIRE_FALSE(names.empty());
    bool haveAvutil = false;
    for (const std::string& n : names) {
        INFO(n);
        // Every embedded library is renamed; nothing is copied under the
        // name a host's own FFmpeg could also have.
        CHECK(n.rfind("OpenOSV_", 0) == 0);
        haveAvutil = haveAvutil || n.find("libavutil") != std::string::npos;
    }
    CHECK(haveAvutil);

    // The module references only the renamed copies, through its own RPATH.
    const Run deps = run("otool -L " + quoted(kBinary));
    REQUIRE(deps.status == 0);
    INFO(deps.output);
    CHECK(deps.output.find("@rpath/OpenOSV_libavutil") != std::string::npos);
    CHECK(deps.output.find("@rpath/libavutil") == std::string::npos);
    const Run rpaths = run("otool -l " + quoted(kBinary));
    REQUIRE(rpaths.status == 0);
    CHECK(rpaths.output.find("@loader_path/../Frameworks") != std::string::npos);
    CHECK(rpaths.output.find("vcpkg_installed") == std::string::npos);
}

TEST_CASE("a bundle exports its entry points and nothing else, and is signed", "[macos][bundle]") {
    // The export list: our two functions, not the hidden one, not the library.
    const Run exports = run("nm -gU " + quoted(kBinary));
    REQUIRE(exports.status == 0);
    INFO(exports.output);
    CHECK(exports.output.find("_osvBundleProbeAvutilVersion") != std::string::npos);
    CHECK(exports.output.find("_osvBundleProbeAvcodecImage") != std::string::npos);
    CHECK(exports.output.find("_osvBundleProbeHidden") == std::string::npos);

    // An ad-hoc signature that covers the whole bundle, nested code included.
    const Run verify = run("codesign --verify --deep --strict " + quoted(kBundle));
    INFO(verify.output);
    CHECK(verify.status == 0);
}

TEST_CASE("a loaded bundle resolves FFmpeg from inside itself", "[macos][bundle]") {
    void* handle = ::dlopen(kBinary.c_str(), RTLD_NOW | RTLD_LOCAL);
    INFO((handle ? "" : ::dlerror()));
    REQUIRE(handle != nullptr);

    using VersionFn = unsigned (*)();
    using ImageFn = const void* (*)();
    auto version = reinterpret_cast<VersionFn>(::dlsym(handle, "osvBundleProbeAvutilVersion"));
    auto image = reinterpret_cast<ImageFn>(::dlsym(handle, "osvBundleProbeAvcodecImage"));
    REQUIRE(version != nullptr);
    REQUIRE(image != nullptr);
    CHECK(::dlsym(handle, "osvBundleProbeHidden") == nullptr);

    // A plausible FFmpeg version (major in the top 16 bits).
    CHECK((version() >> 16) >= 50u);

    // The avcodec the bundle called is the copy in its own Frameworks folder.
    Dl_info info{};
    REQUIRE(::dladdr(image(), &info) != 0);
    REQUIRE(info.dli_fname != nullptr);
    const std::string provider = info.dli_fname;
    INFO("avcodec_version comes from " << provider);
    CHECK(provider.find("/Contents/Frameworks/OpenOSV_libavcodec") != std::string::npos);
    ::dlclose(handle);
}
