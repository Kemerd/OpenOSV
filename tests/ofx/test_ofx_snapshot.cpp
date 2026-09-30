// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_snapshot.cpp - everything a host learns about OpenOSV.ofx before
// it renders a single frame, frozen in a golden file.
//
// ===========================================================================
//  Why a snapshot
// ===========================================================================
// DaVinci Resolve stores the effects' identifiers and parameter names in its
// projects, and it clamps, hides or refuses whatever a descriptor gets wrong
// (a double without a display range, a choice default off the list...).  The
// other tests pin the properties that matter today; this one pins ALL of
// them, so a change made for another host (VEGAS Pro, whose differences key
// on osv::ofx::hostProfile()) cannot leak into what Resolve sees:
//
//   * both plug-ins' kOfxActionDescribe properties;
//   * every context each plug-in declares: the context descriptor's
//     properties, every clip with its properties, every parameter with its
//     properties, in definition order;
//   * each plug-in's answer to kOfxImageEffectActionGetClipPreferences on a
//     fresh instance (the generator states its output format there).
//
// The CUDA render claims are the one exception: they depend on the build,
// not the host, and test_ofx_module.cpp pins them (buildDependent() below).
//
// The golden file (tests/ofx/golden/ofx_descriptors_generic.txt) was
// generated from the code as it stood BEFORE the VEGAS profile existed.  A
// deliberate change to what Resolve sees regenerates it:
//
//     set OSV_OFX_UPDATE_GOLDEN=1
//     osv_ofx_tests.exe "[snapshot]"
//
// and the diff of the golden file IS the review of that change.  VEGAS's
// descriptors have a golden file of their own (ofx_descriptors_vegas.txt),
// regenerated the same way in a VEGAS process:
//
//     set OSV_MOCK_OFX_PROFILE=vegas
//     osv_ofx_tests.exe "VEGAS: the descriptors are exactly the VEGAS golden snapshot"

#include "OfxTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef OSV_OFX_GOLDEN_DIR
#error "OSV_OFX_GOLDEN_DIR must name tests/ofx/golden (tests/ofx/CMakeLists.txt)"
#endif

using namespace osv::ofxtest;

namespace {

// ===========================================================================
//  Serialisation - deterministic, line based, diff friendly
// ===========================================================================

/// `text` as a quoted string literal: backslash, quote and control bytes
/// escaped, so every value is exactly one line of the golden file.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (byte < 0x20 || byte == 0x7f) {
            // Control characters would break the one-value-per-line rule.
            out += std::format("\\x{:02x}", static_cast<unsigned>(byte));
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

/// A double in its shortest round-trip spelling ("0.1", "-36000", "nan").
/// std::format never consults the locale, so a German Windows writes the
/// same file as an English one.
std::string number(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "inf" : "-inf";
    }
    return std::format("{}", value);
}

/// One property as "name : type = [v0, v1, ...]".  Pointers are recorded
/// only as set / null: their values are addresses, which differ per run.
std::string propertyLine(const std::string& name, const PropertySet::Prop& prop) {
    std::string values;
    const auto append = [&values](const std::string& v) {
        if (!values.empty()) {
            values += ", ";
        }
        values += v;
    };
    std::string type;
    switch (prop.type) {
    case PropertySet::Type::String:
        type = "string";
        for (const std::string& s : prop.s) append(quoted(s));
        break;
    case PropertySet::Type::Int:
        type = "int";
        for (const int i : prop.i) append(std::to_string(i));
        break;
    case PropertySet::Type::Double:
        type = "double";
        for (const double d : prop.d) append(number(d));
        break;
    case PropertySet::Type::Pointer:
        type = "pointer";
        for (void* p : prop.p) append(p ? "set" : "null");
        break;
    }
    return name + " : " + type + " = [" + values + "]";
}

/// True for the properties that depend on how the module was BUILT rather
/// than on the host: the OpenFX 1.5 CUDA render claims exist only in a
/// Windows build with the CUDA kernel.  test_ofx_module.cpp pins them where
/// they exist; leaving them out here keeps one golden file valid for every
/// build (macOS CI and CUDA-less Windows builds included).
bool buildDependent(const std::string& name) {
    return name == kOfxImageEffectPropCudaRenderSupported || name == kOfxImageEffectPropCudaStreamSupported;
}

/// Every property of `set`, sorted by name (the set is a std::map), one
/// line each, prefixed with `indent`.
void dumpProperties(std::ostringstream& out, const PropertySet& set, const std::string& indent) {
    for (const auto& [name, prop] : set.props) {
        if (buildDependent(name)) {
            continue;
        }
        out << indent << propertyLine(name, prop) << '\n';
    }
}

/// An OfxStatus by name, so a changed answer reads as a word in the diff.
std::string statusName(OfxStatus status) {
    switch (status) {
    case kOfxStatOK: return "kOfxStatOK";
    case kOfxStatReplyDefault: return "kOfxStatReplyDefault";
    case kOfxStatFailed: return "kOfxStatFailed";
    case kOfxStatErrMissingHostFeature: return "kOfxStatErrMissingHostFeature";
    case kOfxStatErrBadHandle: return "kOfxStatErrBadHandle";
    case kOfxStatErrImageFormat: return "kOfxStatErrImageFormat";
    case kOfxStatErrMemory: return "kOfxStatErrMemory";
    default: return "status " + std::to_string(status);
    }
}

/// One context descriptor: its properties, its clips and its parameters.
void dumpContext(std::ostringstream& out, PluginHarness& plugin, const std::string& context) {
    OfxStatus status = kOfxStatFailed;
    std::unique_ptr<Effect> ctx = plugin.describeInContext(context, &status);
    out << "  context " << context << " -> " << statusName(status) << '\n';
    if (!ctx) {
        out << "    (no descriptor)\n";
        return;
    }
    out << "    properties\n";
    dumpProperties(out, ctx->props, "      ");
    // Clips in the order the plug-in defined them (hosts show them so).
    for (const std::string& name : ctx->clipOrder) {
        out << "    clip " << name << '\n';
        const Clip* clip = ctx->clip(name);
        if (clip) {
            dumpProperties(out, clip->props, "      ");
        }
    }
    // Parameters in definition order: the order is part of the UI.
    for (const auto& param : ctx->params.params) {
        out << "    param " << param->name << " (" << param->type << ")\n";
        dumpProperties(out, param->props, "      ");
    }
}

/// The plug-in's answer to kOfxImageEffectActionGetClipPreferences on a
/// fresh instance, with the out args pre-filled the way a host fills them
/// (so what the plug-in leaves alone is visible as the host's value).
void dumpClipPreferences(std::ostringstream& out, PluginHarness& plugin, const std::string& context) {
    OfxStatus created = kOfxStatFailed;
    std::unique_ptr<Effect> instance = plugin.createInstance(context, 1920, 1080, 25.0, &created);
    out << "  clip preferences (" << context << ", create " << statusName(created) << ")";
    if (!instance || created != kOfxStatOK) {
        out << " -> no instance\n";
        return;
    }
    PropertySet prefs;
    for (const std::string& clip : instance->clipOrder) {
        prefs.setString("OfxImageClipPropComponents_" + clip, kOfxImageComponentNone);
        prefs.setString("OfxImageClipPropDepth_" + clip, kOfxBitDepthNone);
        prefs.setDouble("OfxImageClipPropPAR_" + clip, 1.0);
    }
    prefs.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    prefs.setInt(kOfxImageEffectFrameVarying, 0);
    prefs.setInt(kOfxImageClipPropContinuousSamples, 0);
    const OfxStatus status =
        plugin.action(kOfxImageEffectActionGetClipPreferences, instance->handle(), nullptr, &prefs);
    out << " -> " << statusName(status) << '\n';
    dumpProperties(out, prefs, "    ");
    (void)plugin.destroyInstance(*instance);
}

/// The whole snapshot of one plug-in.
void dumpPlugin(std::ostringstream& out, PluginHarness& plugin, const std::string& identifier) {
    out << "plugin " << identifier << '\n';
    out << "  describe\n";
    dumpProperties(out, plugin.descriptor.props, "    ");
    // Every context the plug-in says it supports, in its own order.
    const std::vector<std::string> contexts = plugin.descriptor.props.getStrings(kOfxImageEffectPropSupportedContexts);
    for (const std::string& context : contexts) {
        dumpContext(out, plugin, context);
    }
    if (!contexts.empty()) {
        dumpClipPreferences(out, plugin, contexts.front());
    }
}

/// The snapshot of the loaded module, as text.
std::string snapshot() {
    Fixture& f = Fixture::get();
    std::ostringstream out;
    out << "# OpenOSV.ofx descriptor snapshot (tests/ofx/test_ofx_snapshot.cpp)\n";
    dumpPlugin(out, f.reframe, "org.openosv.Open360Reframe");
    dumpPlugin(out, f.source, "org.openosv.OSVSource");
    return out.str();
}

/// `text` split into lines, carriage returns dropped (a checkout with CRLF
/// line endings compares equal to the LF file it came from).
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    for (const char c : text) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out.push_back(line);
            line.clear();
            continue;
        }
        line += c;
    }
    if (!line.empty()) {
        out.push_back(line);
    }
    return out;
}

/// The golden file's contents, or empty when it cannot be read.
std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Compare `actual` with the golden file `name`, or rewrite the golden file
/// when OSV_OFX_UPDATE_GOLDEN is set.
void checkGolden(const std::string& actual, const std::string& name) {
    const std::filesystem::path golden = std::filesystem::path(OSV_OFX_GOLDEN_DIR) / name;
    const char* update = std::getenv("OSV_OFX_UPDATE_GOLDEN");
    if (update && *update && std::string(update) != "0") {
        std::error_code ec;
        std::filesystem::create_directories(golden.parent_path(), ec);
        std::ofstream out(golden, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << actual;
        WARN("golden file rewritten: " << golden.string());
        return;
    }

    const std::string expectedText = readFile(golden);
    INFO("golden file: " << golden.string());
    REQUIRE_FALSE(expectedText.empty());
    const std::vector<std::string> expected = lines(expectedText);
    const std::vector<std::string> got = lines(actual);

    // The first differing line says what changed; the whole text is left
    // beside the test executable for a real diff.
    std::size_t first = 0;
    while (first < expected.size() && first < got.size() && expected[first] == got[first]) {
        ++first;
    }
    if (first < expected.size() || first < got.size()) {
        const std::filesystem::path dump = std::filesystem::current_path() / ("actual_" + name);
        std::ofstream(dump, std::ios::binary | std::ios::trunc) << actual;
        INFO("first difference at line " << (first + 1) << " (actual text written to " << dump.string() << ")");
        INFO("expected: " << (first < expected.size() ? expected[first] : std::string("<end of file>")));
        INFO("actual:   " << (first < got.size() ? got[first] : std::string("<end of file>")));
        CHECK(expected.size() == got.size());
        FAIL("the descriptors differ from the golden file");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Generic and Resolve: one golden file.  The module treats an unknown host
//  exactly like Resolve, and ctest runs this test a second time as Resolve
//  itself ("DaVinciResolveLite", OSV_MOCK_OFX_PROFILE=resolve) to prove it.
// ---------------------------------------------------------------------------
TEST_CASE("the descriptors are exactly the golden snapshot", "[ofx][module][snapshot]") {
    if (MockHost::instance().isVegas()) {
        SKIP("VEGAS has a snapshot of its own (the [vegas] tests)");
    }
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);
    checkGolden(snapshot(), "ofx_descriptors_generic.txt");
    CHECK(MockHost::instance().imagesOut == 0);
}

// ---------------------------------------------------------------------------
//  VEGAS: its own golden file, so every VEGAS-only descriptor - depths,
//  contexts, thread safety, Output Levels, the hidden Choose button, the
//  clip preferences without a depth - is pinned as completely as Resolve's.
// ---------------------------------------------------------------------------
TEST_CASE("VEGAS: the descriptors are exactly the VEGAS golden snapshot", "[ofx][module][.vegas]") {
    if (!MockHost::instance().isVegas()) {
        SKIP("needs OSV_MOCK_OFX_PROFILE=vegas (ctest runs it as 'vegas: ...')");
    }
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);
    checkGolden(snapshot(), "ofx_descriptors_vegas.txt");
    CHECK(MockHost::instance().imagesOut == 0);
}
