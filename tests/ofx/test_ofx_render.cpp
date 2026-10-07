// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_render.cpp - the host-independent pieces of OpenOSV.ofx, tested
// directly: the host classification (OfxHost.cpp) and the CPU pixel loops
// (OfxRender.cpp), compiled into this executable from the module's own
// sources.
//
// ===========================================================================
//  Why not through the loaded module
// ===========================================================================
// classifyHost() decides a module's profile ONCE per load from one host name,
// so every name worth checking would need a process of its own.  And the
// generator's pixel loops only see pixels once a real clip has been stitched
// - on the sample clip, through the GPU.  Compiled in here, both run on the
// CPU in the default test process: every known host name in one test, and
// every output format x levels combination of both generator paths held to
// the packing rule (OfxHostImage.h) with synthetic frames.  The [vegas]
// [sample] tests then check the same outputs end to end through the module.

#include "OfxTestSupport.h"

#include "OfxHost.h"
#include "OfxHostImage.h"
#include "OfxRender.h"

#include "DirectRender.h"  // [WP-PAR] the direct view's builder
#include "ReframeCpu.h"

#include "osv/color/ColorParams.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace osv::ofxtest;
namespace ofx = osv::ofx;
namespace rf = osv::reframe;

namespace {

/// One output format a host may hand the generator.
struct Format {
    ofx::HostDepth depth;
    ofx::HostOrder order;
};

/// Every format VEGAS can hand out (Resolve's is the first).
constexpr Format kFormats[] = {
    {ofx::HostDepth::Float, ofx::HostOrder::Rgba},
    {ofx::HostDepth::Float, ofx::HostOrder::Bgra},
    {ofx::HostDepth::Byte, ofx::HostOrder::Rgba},
    {ofx::HostDepth::Byte, ofx::HostOrder::Bgra},
};

/// Both output levels.
constexpr ofx::OutputLevels kLevels[] = {ofx::OutputLevels::Full, ofx::OutputLevels::Studio};

/// A HostImage as the render loops see it.
ofx::HostImageView viewOf(HostImage& image) {
    return ofx::HostImageView{image.data(), image.rowBytes, image.bounds, image.depth, image.order};
}

/// A label for INFO lines: "byte BGRA studio".
std::string formatName(const Format& format, ofx::OutputLevels levels) {
    return std::string(ofx::hostDepthName(format.depth)) + " " + ofx::hostOrderName(format.order) + " " +
           ofx::outputLevelsName(levels);
}

/// A stitched frame of `w` x `h` with values a real stitch can produce and a
/// few it should not but might: super-white, below-black, partial alpha.
osv::render::ImageRGBAf stitchedFrame(std::uint32_t w, std::uint32_t h) {
    auto created = osv::render::ImageRGBAf::create(w, h);
    REQUIRE(created.ok());
    osv::render::ImageRGBAf image = std::move(created).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        float* row = image.row(y);
        REQUIRE(row);
        for (std::uint32_t x = 0; x < w; ++x) {
            float* p = row + static_cast<std::size_t>(x) * 4;
            // A ramp across [-0.25, 1.25] in red, rows in green, a pattern in
            // blue and alpha in four steps: every quantisation corner is hit.
            p[0] = -0.25f + 1.5f * static_cast<float>(x) / static_cast<float>(w - 1);
            p[1] = static_cast<float>(y) / static_cast<float>(h - 1);
            p[2] = static_cast<float>((x * 7 + y * 13) % 256) / 255.0f;
            p[3] = static_cast<float>((x + y) % 4) / 3.0f;
        }
    }
    return image;
}

/// The packed transparent black of `format` in `levels`, as raw bytes.
std::vector<unsigned char> packedTransparent(const Format& format, ofx::OutputLevels levels) {
    std::vector<unsigned char> bytes(16, 0);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ofx::storeHostPixel(bytes.data(), format.depth, format.order, levels, zero);
    bytes.resize(static_cast<std::size_t>(ofx::bytesPerPixel(format.depth)));
    return bytes;
}

}  // namespace

// ===========================================================================
//  The host classification
// ===========================================================================

TEST_CASE("classifyHost knows every VEGAS and Resolve name, and nothing else", "[ofx][host]") {
    using ofx::HostProfile;
    // VEGAS, MAGIX era and Sony era, Pro and Movie Studio (VEGAS's OpenFX
    // extension header, ofxSonyVegas.h, names the Sony ones), in any case.
    CHECK(ofx::classifyHost("com.vegascreativesoftware.vegas") == HostProfile::Vegas);
    CHECK(ofx::classifyHost("com.sonycreativesoftware.vegas") == HostProfile::Vegas);
    CHECK(ofx::classifyHost("com.sonycreativesoftware.vegas.moviestudio.hd") == HostProfile::Vegas);
    CHECK(ofx::classifyHost("com.sonycreativesoftware.vegas.moviestudio.pe") == HostProfile::Vegas);
    CHECK(ofx::classifyHost("COM.VEGASCREATIVESOFTWARE.VEGAS") == HostProfile::Vegas);
    CHECK(ofx::classifyHost("com.sonycreativesoftware.catalyst") == HostProfile::Vegas);
    // Resolve: Studio and the free version.
    CHECK(ofx::classifyHost("DaVinciResolve") == HostProfile::Resolve);
    CHECK(ofx::classifyHost("DaVinciResolveLite") == HostProfile::Resolve);
    CHECK(ofx::classifyHost("davinciresolvelite") == HostProfile::Resolve);
    // Nothing, and everything else: the generic profile (Resolve's rules).
    CHECK(ofx::classifyHost("") == HostProfile::Generic);
    CHECK(ofx::classifyHost("OpenOSV.MockOfxHost") == HostProfile::Generic);
    CHECK(ofx::classifyHost("uk.co.thefoundry.nuke") == HostProfile::Generic);
    CHECK(ofx::classifyHost("fr.inria.Natron") == HostProfile::Generic);
    CHECK(ofx::classifyHost("vega") == HostProfile::Generic);
    CHECK(ofx::classifyHost("resolv") == HostProfile::Generic);
    CHECK(ofx::classifyHost("\xff\xfe\x01 junk \x7f") == HostProfile::Generic);
    // An embedded NUL splits the word: no match across it.
    CHECK(ofx::classifyHost(std::string_view("ve\0gas", 6)) == HostProfile::Generic);
    // The log names.
    CHECK(std::string(ofx::hostProfileName(HostProfile::Generic)) == "generic");
    CHECK(std::string(ofx::hostProfileName(HostProfile::Resolve)) == "resolve");
    CHECK(std::string(ofx::hostProfileName(HostProfile::Vegas)) == "vegas");
}

TEST_CASE("describeProperty reads what a host set, in the documented type, and says absent otherwise",
          "[ofx][host]") {
    // This executable's own copy of OfxHost.cpp, pointed at the mock's
    // suites (the loaded module has its own copy and is not affected).
    ofx::setHost(MockHost::instance().ofxHost());
    REQUIRE(ofx::fetchSuites());

    PropertySet set;
    set.setString("name", "com.vegascreativesoftware.vegas");
    set.setInts("version", {22, 0, 1});
    set.setString("depths", kOfxBitDepthByte, 0);
    set.setString("depths", kOfxBitDepthFloat, 1);
    set.setPointer("window", nullptr);
    set.setInt("flagAsInt", 1);          // a "true"-style flag some host stores as an int
    set.setDouble("intAsDouble", 2.5);   // an int some host stores as a double
    set.setString("stringNotPointer", "x");
    const OfxPropertySetHandle h = set.handle();

    CHECK(ofx::describeProperty(h, "name", ofx::PropKind::String) == "\"com.vegascreativesoftware.vegas\"");
    CHECK(ofx::describeProperty(h, "version", ofx::PropKind::Int) == "22, 0, 1");
    CHECK(ofx::describeProperty(h, "depths", ofx::PropKind::String) == "\"OfxBitDepthByte\", \"OfxBitDepthFloat\"");
    CHECK(ofx::describeProperty(h, "window", ofx::PropKind::Pointer) == "null");
    CHECK(ofx::describeProperty(h, "flagAsInt", ofx::PropKind::String) == "1");
    CHECK(ofx::describeProperty(h, "intAsDouble", ofx::PropKind::Int) == "2.5");
    // A string is never read through a pointer read, and vice versa: the
    // value is reported unreadable rather than guessed at.
    CHECK(ofx::describeProperty(h, "stringNotPointer", ofx::PropKind::Pointer) == "?");
    CHECK(ofx::describeProperty(h, "missing", ofx::PropKind::String) == "absent");
    CHECK(ofx::describeProperty(nullptr, "name", ofx::PropKind::String) == "absent");
    CHECK(ofx::describeProperty(h, nullptr, ofx::PropKind::String) == "absent");

    // Twenty values are cut at sixteen, and say how many more there were.
    std::vector<int> many(20, 7);
    set.setInts("many", many);
    const std::string text = ofx::describeProperty(h, "many", ofx::PropKind::Int);
    CHECK(text.find("(+4 more)") != std::string::npos);

    // The same copy logs the mock host's whole description without a fault.
    ofx::logHostDescription();
    ofx::clearSuites();
}

TEST_CASE("VEGAS's named quality makes Draft and Preview playback, and nothing else does", "[ofx][host]") {
    using ofx::HostQuality;
    // ---- the four documented values, exactly, and nothing else ---------------
    CHECK(ofx::hostQualityFromName("OfxImageEffectPropRenderQualityDraft") == HostQuality::Draft);
    CHECK(ofx::hostQualityFromName("OfxImageEffectPropRenderQualityPreview") == HostQuality::Preview);
    CHECK(ofx::hostQualityFromName("OfxImageEffectPropRenderQualityGood") == HostQuality::Good);
    CHECK(ofx::hostQualityFromName("OfxImageEffectPropRenderQualityBest") == HostQuality::Best);
    CHECK(ofx::hostQualityFromName("") == HostQuality::Unknown);
    CHECK(ofx::hostQualityFromName("Preview") == HostQuality::Unknown);
    CHECK(ofx::hostQualityFromName("ofximageeffectproprenderqualitypreview") == HostQuality::Unknown);
    CHECK(ofx::hostQualityFromName("OfxImageEffectPropRenderQualityPreview ") == HostQuality::Unknown);
    CHECK(std::string(ofx::hostQualityName(HostQuality::Draft)) == "draft");
    CHECK(std::string(ofx::hostQualityName(HostQuality::Preview)) == "preview");
    CHECK(std::string(ofx::hostQualityName(HostQuality::Good)) == "good");
    CHECK(std::string(ofx::hostQualityName(HostQuality::Best)) == "best");
    CHECK(std::string(ofx::hostQualityName(HostQuality::Unknown)) == "unnamed");

    // ---- render modes, through this executable's own copy of OfxHost.cpp -----
    ofx::setHost(MockHost::instance().ofxHost());
    REQUIRE(ofx::fetchSuites());

    /// The inArgs of one render: the two OpenFX 1.4 flags, and VEGAS's
    /// quality string when `quality` is not empty.
    const auto argsWith = [](int interactive, int draft, const char* quality) {
        auto set = std::make_unique<PropertySet>();
        set->setInt(kOfxImageEffectPropInteractiveRenderStatus, interactive);
        set->setInt(kOfxImageEffectPropRenderQualityDraft, draft);
        if (quality && quality[0] != '\0') {
            set->setString(ofx::kPropVegasRenderQuality, quality);
        }
        return set;
    };
    struct Case {
        const char* name;
        ofx::HostProfile profile;
        int interactive;
        int draft;
        const char* quality;
        bool wantInteractive;
        bool wantDraft;
        bool wantPlayback;
        HostQuality wantQuality;
    };
    const Case cases[] = {
        // VEGAS: its Preview window's qualities are playback...
        {"VEGAS Draft", ofx::HostProfile::Vegas, 0, 0, ofx::kVegasQualityDraft, true, true, true, HostQuality::Draft},
        {"VEGAS Preview", ofx::HostProfile::Vegas, 0, 0, ofx::kVegasQualityPreview, true, true, true,
         HostQuality::Preview},
        // ...a file render's are final, unless the 1.4 flags say otherwise.
        {"VEGAS Good", ofx::HostProfile::Vegas, 0, 0, ofx::kVegasQualityGood, false, false, false, HostQuality::Good},
        {"VEGAS Best", ofx::HostProfile::Vegas, 0, 0, ofx::kVegasQualityBest, false, false, false, HostQuality::Best},
        {"VEGAS Best, 1.4 flags set", ofx::HostProfile::Vegas, 1, 1, ofx::kVegasQualityBest, true, true, false,
         HostQuality::Best},
        // An absent or unknown quality changes nothing.
        {"VEGAS, no quality", ofx::HostProfile::Vegas, 0, 0, "", false, false, false, HostQuality::Unknown},
        {"VEGAS, unknown quality", ofx::HostProfile::Vegas, 0, 0, "Fastest", false, false, false,
         HostQuality::Unknown},
        // Resolve and every other host: the 1.4 flags, as always - a quality
        // string some host might set is not read.
        {"Resolve, flags off", ofx::HostProfile::Resolve, 0, 0, ofx::kVegasQualityPreview, false, false, false,
         HostQuality::Unknown},
        {"Resolve, flags on", ofx::HostProfile::Resolve, 1, 1, "", true, true, false, HostQuality::Unknown},
        {"Generic, interactive only", ofx::HostProfile::Generic, 1, 0, ofx::kVegasQualityDraft, true, false, false,
         HostQuality::Unknown},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const auto set = argsWith(c.interactive, c.draft, c.quality);
        const ofx::RenderMode mode = ofx::renderModeFor(set->handle(), c.profile);
        CHECK(mode.interactive == c.wantInteractive);
        CHECK(mode.draft == c.wantDraft);
        CHECK(mode.playback == c.wantPlayback);
        CHECK(mode.quality == c.wantQuality);
    }
    // No inArgs at all: a final render, whatever the host.
    const ofx::RenderMode none = ofx::renderModeFor(nullptr, ofx::HostProfile::Vegas);
    CHECK_FALSE(none.interactive);
    CHECK_FALSE(none.draft);
    CHECK_FALSE(none.playback);
    CHECK(none.quality == HostQuality::Unknown);
    ofx::clearSuites();
}

TEST_CASE("the effects accept one format outside VEGAS and four inside", "[ofx][host]") {
    CHECK(std::string(ofx::acceptedFormats(ofx::HostProfile::Generic)) == "32-bit float RGBA");
    CHECK(std::string(ofx::acceptedFormats(ofx::HostProfile::Resolve)) == "32-bit float RGBA");
    CHECK(std::string(ofx::acceptedFormats(ofx::HostProfile::Vegas)) == "8-bit or 32-bit float RGBA / BGRA");
}

// ===========================================================================
//  The packing rule
// ===========================================================================

TEST_CASE("the packing rule: levels on colour only, then the depth, then the order", "[ofx][render]") {
    const float rgba[4] = {1.0f, 0.0f, 0.5f, 0.25f};

    // 8-bit B G R A at full range: codes rounded to nearest, B first.
    unsigned char bytes[4] = {};
    ofx::storeHostPixel(bytes, ofx::HostDepth::Byte, ofx::HostOrder::Bgra, ofx::OutputLevels::Full, rgba);
    CHECK(bytes[0] == 128);  // blue 0.5
    CHECK(bytes[1] == 0);    // green
    CHECK(bytes[2] == 255);  // red
    CHECK(bytes[3] == 64);   // alpha 0.25

    // Studio levels: black 16, white 235, alpha untouched.
    ofx::storeHostPixel(bytes, ofx::HostDepth::Byte, ofx::HostOrder::Rgba, ofx::OutputLevels::Studio, rgba);
    CHECK(bytes[0] == 235);
    CHECK(bytes[1] == 16);
    CHECK(bytes[2] == 126);  // 16 + 0.5 * 219 = 125.5, rounded up
    CHECK(bytes[3] == 64);

    // Float keeps the levelled values as they are, beyond [0, 1] included.
    const float hot[4] = {1.5f, -0.5f, 0.0f, 1.0f};
    float floats[4] = {};
    ofx::storeHostPixel(floats, ofx::HostDepth::Float, ofx::HostOrder::Bgra, ofx::OutputLevels::Studio, hot);
    CHECK(floats[0] == ofx::studioFromFull(0.0f));
    CHECK(floats[1] == ofx::studioFromFull(-0.5f));
    CHECK(floats[2] == ofx::studioFromFull(1.5f));
    CHECK(floats[3] == 1.0f);

    // Every code survives a read and a full-range store unchanged.
    for (int code = 0; code < 256; ++code) {
        const unsigned char in[4] = {static_cast<unsigned char>(code), 0, 255, static_cast<unsigned char>(255 - code)};
        float read[4] = {};
        ofx::loadHostPixel(in, ofx::HostDepth::Byte, ofx::HostOrder::Bgra, read);
        unsigned char out[4] = {};
        ofx::storeHostPixel(out, ofx::HostDepth::Byte, ofx::HostOrder::Bgra, ofx::OutputLevels::Full, read);
        INFO(code);
        CHECK(std::memcmp(in, out, 4) == 0);
    }
}

// ===========================================================================
//  The generator's two CPU paths, in every format and both levels
// ===========================================================================

TEST_CASE("the stitched sphere is packed exactly in every format and both levels", "[ofx][render]") {
    // A frame smaller than the image, and a window over the whole image, so
    // the pixels outside the frame must come out transparent black too.
    const OfxRectI frame{0, 0, 96, 48};
    const OfxRectI bounds{-6, -3, 100, 50};
    const osv::render::ImageRGBAf stitched = stitchedFrame(96, 48);

    // The reference: Resolve's format, which is the stitch copied as it is.
    HostImage reference = makeImage(bounds);
    reference.fill(-7.0f);
    ofx::HostImageView referenceView = viewOf(reference);
    REQUIRE(ofx::copyStitchedFrame(stitched, referenceView, bounds, frame, ofx::OutputLevels::Full, nullptr));
    for (int y = frame.y1; y < frame.y2; ++y) {
        const float* want = stitched.row(static_cast<std::uint32_t>(frame.y2 - 1 - y));
        REQUIRE(std::memcmp(reference.pixel(0, y), want, 96 * 16) == 0);
    }
    const float* outside = reference.pixel(-6, -3);
    REQUIRE(outside);
    CHECK((outside[0] == 0.0f && outside[1] == 0.0f && outside[2] == 0.0f && outside[3] == 0.0f));

    // Every other format and level: the reference through the packing rule,
    // exactly - codes for 8-bit, values for float.
    int layout = 0;
    for (const Format& format : kFormats) {
        for (const ofx::OutputLevels levels : kLevels) {
            INFO(formatName(format, levels));
            // Alternate the row order and padding as a host may.
            HostImage image = makeImage(bounds, (layout % 2) == 1, (layout % 3) * 4, format.depth, format.order);
            ++layout;
            image.fill(-7.0f);
            ofx::HostImageView view = viewOf(image);
            REQUIRE(ofx::copyStitchedFrame(stitched, view, bounds, frame, levels, nullptr));
            CHECK(maxPackedDifference(image, reference, bounds, levels) == 0.0);
        }
    }
}

TEST_CASE("the reframed view is packed exactly in every format and both levels", "[ofx][render]") {
    // A float panorama read in place, with alpha in vertical stripes (fully
    // transparent, half and opaque), so the view holds transparent pixels -
    // which Studio levels must lift to studio black like any other - next to
    // opaque ones.
    HostImage panorama = makeImage(OfxRectI{0, 0, 256, 128});
    paintPanorama(panorama);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 256; ++x) {
            float* p = panorama.pixel(x, y);
            p[3] = (x % 24) < 8 ? 0.0f : ((x % 24) < 16 ? 0.5f : 1.0f);
        }
    }
    rf::Settings settings = defaultSettings();
    settings.panDeg = 33.0;
    settings.tiltDeg = -8.0;
    const OfxRectI frame{0, 0, 64, 48};
    ofx::HostImageView panoramaView = viewOf(panorama);
    rf::KernelSetup setup = rf::buildParams(settings, ofx::sourceView(panoramaView), 64, 48, rf::SizePx{64, 48});
    REQUIRE(setup.valid);
    setup.source.isBgra = 0;

    HostImage reference = makeImage(frame);
    reference.fill(-7.0f);
    ofx::HostImageView referenceView = viewOf(reference);
    REQUIRE(ofx::renderReframeCpu(setup, referenceView, frame, frame, ofx::OutputLevels::Full, nullptr));
    // The view really holds transparent and opaque pixels.
    int transparent = 0;
    int opaque = 0;
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const float a = reference.pixel(x, y)[3];
            transparent += a == 0.0f ? 1 : 0;
            opaque += a == 1.0f ? 1 : 0;
        }
    }
    CHECK(transparent > 0);
    CHECK(opaque > 0);

    int layout = 0;
    for (const Format& format : kFormats) {
        for (const ofx::OutputLevels levels : kLevels) {
            INFO(formatName(format, levels));
            HostImage image = makeImage(frame, (layout % 2) == 0, (layout % 3) * 4, format.depth, format.order);
            ++layout;
            image.fill(-7.0f);
            ofx::HostImageView view = viewOf(image);
            REQUIRE(ofx::renderReframeCpu(setup, view, frame, frame, levels, nullptr));
            CHECK(maxPackedDifference(image, reference, frame, levels) == 0.0);
        }
    }
}

TEST_CASE("an 8-bit source in either order frames like its float twin", "[ofx][render]") {
    // The filter's CPU read path: an 8-bit source is promoted, and the
    // sampler is told its order.  The float twin holds the promoted values.
    HostImage panorama = makeImage(OfxRectI{0, 0, 256, 128});
    paintPanorama(panorama);
    const HostImage bytesRgba = convertImage(panorama, ofx::HostDepth::Byte, ofx::HostOrder::Rgba);
    const HostImage twin = convertImage(bytesRgba, ofx::HostDepth::Float, ofx::HostOrder::Rgba);
    rf::Settings settings = defaultSettings();
    settings.panDeg = -70.0;
    const OfxRectI frame{0, 0, 80, 45};

    const auto frameFrom = [&](HostImage source) {
        ofx::HostImageView sourceImage = viewOf(source);
        std::vector<float> promoted;
        rf::ConstFrameView sampler = ofx::sourceView(sourceImage);
        if (rf::layoutNeedsPromotion(sampler.layout)) {
            sampler = rf::promoteIntegerToFloat(sampler, promoted, nullptr);
        }
        REQUIRE(sampler.valid());
        rf::KernelSetup setup = rf::buildParams(settings, sampler, 80, 45, rf::SizePx{80, 45});
        REQUIRE(setup.valid);
        setup.source.isBgra = sourceImage.order == ofx::HostOrder::Bgra ? 1 : 0;
        HostImage out = makeImage(frame);
        ofx::HostImageView outView = viewOf(out);
        REQUIRE(ofx::renderReframeCpu(setup, outView, frame, frame, ofx::OutputLevels::Full, nullptr));
        return out;
    };
    const HostImage fromTwin = frameFrom(twin);
    CHECK(maxDifference(frameFrom(bytesRgba), fromTwin, frame) == 0.0);
    CHECK(maxDifference(frameFrom(convertImage(bytesRgba, ofx::HostDepth::Byte, ofx::HostOrder::Bgra, true, 8)),
                        fromTwin, frame) == 0.0);
    CHECK(maxDifference(frameFrom(convertImage(twin, ofx::HostDepth::Float, ofx::HostOrder::Bgra)), fromTwin,
                        frame) == 0.0);
}

TEST_CASE("clearCpu writes transparent black in every format and both levels, and nothing else",
          "[ofx][render]") {
    const OfxRectI bounds{0, 0, 40, 20};
    const OfxRectI window{5, 3, 60, 12};  // runs off the image on the right
    for (const Format& format : kFormats) {
        for (const ofx::OutputLevels levels : kLevels) {
            INFO(formatName(format, levels));
            HostImage image = makeImage(bounds, false, 0, format.depth, format.order);
            image.fill(-7.0f);
            std::vector<unsigned char> before(image.rawPixel(0, 0),
                                              image.rawPixel(0, 0) + static_cast<std::size_t>(image.pixelBytes()));
            ofx::clearCpu(viewOf(image), window, levels);
            const std::vector<unsigned char> black = packedTransparent(format, levels);
            bool insideOk = true;
            bool outsideOk = true;
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                for (int x = bounds.x1; x < bounds.x2; ++x) {
                    const unsigned char* p = image.rawPixel(x, y);
                    const bool inside = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
                    const std::vector<unsigned char>& want = inside ? black : before;
                    if (std::memcmp(p, want.data(), want.size()) != 0) {
                        (inside ? insideOk : outsideOk) = false;
                    }
                }
            }
            CHECK(insideOk);
            CHECK(outsideOk);
        }
    }
}

TEST_CASE("an unusable target or a stitch of the wrong size writes nothing", "[ofx][render]") {
    const OfxRectI frame{0, 0, 32, 16};
    const osv::render::ImageRGBAf stitched = stitchedFrame(32, 16);
    HostImage image = makeImage(frame, false, 0, ofx::HostDepth::Byte, ofx::HostOrder::Bgra);
    image.fill(-7.0f);
    const std::vector<float> untouched = image.storage;

    // A pitch too small for a row of the stated depth.
    ofx::HostImageView narrow = viewOf(image);
    narrow.rowBytes = 32 * 4 - 1;
    CHECK_FALSE(ofx::copyStitchedFrame(stitched, narrow, frame, frame, ofx::OutputLevels::Studio, nullptr));
    ofx::clearCpu(narrow, frame, ofx::OutputLevels::Studio);
    // No data at all.
    ofx::HostImageView none = viewOf(image);
    none.data = nullptr;
    CHECK_FALSE(ofx::copyStitchedFrame(stitched, none, frame, frame, ofx::OutputLevels::Full, nullptr));
    // A stitch that is not the frame's size.
    ofx::HostImageView ok = viewOf(image);
    const osv::render::ImageRGBAf wrong = stitchedFrame(31, 16);
    CHECK_FALSE(ofx::copyStitchedFrame(wrong, ok, frame, frame, ofx::OutputLevels::Full, nullptr));
    // An invalid setup.
    rf::KernelSetup invalid;
    CHECK_FALSE(ofx::renderReframeCpu(invalid, ok, frame, frame, ofx::OutputLevels::Full, nullptr));

    CHECK(std::memcmp(image.storage.data(), untouched.data(), untouched.size() * sizeof(float)) == 0);
}

// ===========================================================================
//  [WP-PAR] The direct view takes the pixel aspect too
// ===========================================================================
// The generator's GPU path traces the camera straight from the fisheyes
// (reframe::buildDirectParams() and the kernel's osvRayForPixel) instead of
// framing the stitched sphere (osvReframeEquirectPixel).  In a non-square
// project both must see the same direction through every pixel.  Held here on
// the CPU, with the shared kernel functions themselves: the sphere path's ray
// read back from a coordinate panorama (R = longitude, G = latitude, which the
// bilinear sampler reproduces exactly), the direct path's ray from the block
// buildDirectParams() composes.

namespace {

/// A stitch block the direct builder accepts: a Standard-layout equirect, no
/// stabilisation, one plain enabled lens and the default colour.  Nothing in
/// it is ever sampled here - only the composed camera is.
OsvRenderParams plainStitchBlock() {
    OsvRenderParams eq{};
    eq.outW = 2048;
    eq.outH = 1024;
    eq.mode = OSV_MODE_EQUIRECT;
    eq.layout = OSV_LAYOUT_STANDARD;
    for (int i = 0; i < 9; ++i) {
        eq.Rout[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    }
    OsvLens& lens = eq.lens[1];
    lens.fx = 1000.0f;
    lens.fy = 1000.0f;
    lens.cx = 1920.0f;
    lens.cy = 1920.0f;
    for (int i = 0; i < 9; ++i) {
        lens.R[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    }
    lens.thetaMax = 1.7f;
    lens.featherRad = 0.05f;
    lens.gain[0] = lens.gain[1] = lens.gain[2] = 1.0f;
    lens.width = 3840;
    lens.height = 3840;
    lens.enabled = 1;
    eq.color = osv::color::makeColorParams(osv::color::kDefaultDlogMFit, osv::color::OutputTransfer::PQ, 0.0f);
    return eq;
}

/// The body direction the sphere path sees through output pixel (x, y) of
/// `view` (top-down pixel indices), read back from a coordinate panorama
/// `w` x `h` (R G B A floats, top row first).  False when no picture.
bool spherePathDirection(const OsvReframeParams& view, const std::vector<float>& pano, int w, int h, int x, int y,
                         double dir[3]) {
    OsvRgbaSource src{};
    src.w = w;
    src.h = h;
    src.pitchBytes = w * 16;
    src.isHalf = 0;
    src.isBgra = 0;
    src.flipY = 0;
    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    osvReframeEquirectPixel(&view, &src, pano.data(), x, y, rgba);
    if (!(rgba[3] > 0.5f)) {
        return false;
    }
    // The coordinate panorama's two ramps back to angles, then the Standard
    // layout's direction: X right, Y forward (the centre column), Z up.
    const double lon = (static_cast<double>(rgba[0]) * 2.0 - 1.0) * 3.14159265358979323846;
    const double lat = (static_cast<double>(rgba[1]) - 0.5) * 3.14159265358979323846;
    dir[0] = std::sin(lon) * std::cos(lat);
    dir[1] = std::cos(lon) * std::cos(lat);
    dir[2] = std::sin(lat);
    return true;
}

/// The body direction the direct path traces through output pixel (x, y).
bool directPathDirection(const OsvRenderParams& block, int x, int y, double dir[3]) {
    float view[3] = {0.0f, 0.0f, 0.0f};
    if (!osvRayForPixel(&block, static_cast<float>(x), static_cast<float>(y), view)) {
        return false;
    }
    float body[3] = {0.0f, 0.0f, 0.0f};
    osvMat3MulVec(block.Rout, view, body);
    const double n = std::sqrt(static_cast<double>(body[0]) * body[0] + static_cast<double>(body[1]) * body[1] +
                               static_cast<double>(body[2]) * body[2]);
    if (!(n > 0.0)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        dir[i] = static_cast<double>(body[i]) / n;
    }
    return true;
}

}  // namespace

TEST_CASE("the direct view and the sphere path see the same ray through every pixel at any pixel aspect",
          "[ofx][render][par]") {
    // The coordinate panorama: texel centres' longitude and latitude.
    constexpr int kPanoW = 2048;
    constexpr int kPanoH = 1024;
    std::vector<float> pano(static_cast<std::size_t>(kPanoW) * kPanoH * 4);
    for (int r = 0; r < kPanoH; ++r) {
        for (int c = 0; c < kPanoW; ++c) {
            float* p = pano.data() + (static_cast<std::size_t>(r) * kPanoW + c) * 4;
            p[0] = (static_cast<float>(c) + 0.5f) / kPanoW;         // (lon + 180) / 360
            p[1] = 1.0f - (static_cast<float>(r) + 0.5f) / kPanoH;  // (lat + 90) / 180
            p[2] = 0.5f;
            p[3] = 1.0f;
        }
    }

    rf::StitchState stitch;
    stitch.equirect = plainStitchBlock();

    struct Case {
        const char* lens;
        int w;
        double pixelAspect;
    };
    const Case kCases[] = {
        {"DJI", 1920, 1.0}, {"DJI", 1440, 4.0 / 3.0}, {"DJI", 3840, 0.5},
        {"Classic", 1920, 1.0}, {"Classic", 1440, 4.0 / 3.0}, {"Classic", 960, 2.0},
    };
    for (const Case& c : kCases) {
        INFO(c.lens << " lens, " << c.w << " x 1080 at pixel aspect " << c.pixelAspect);
        rf::Settings s = defaultSettings();
        if (std::string_view(c.lens) == "Classic") {
            s.cameraModel = rf::CameraModel::Classic;
            s.fovDeg = 100.0;
            s.distortion = 30.0;
        } else {
            s.djiFovDeg = 70.0;
            s.correction = 0.5;
        }
        s.panDeg = 20.0;
        s.tiltDeg = 6.0;
        const rf::SizePx project{c.w, 1080};

        // ---- both cameras carry the pixel aspect ------------------------------------
        const rf::ViewSetup view = rf::buildView(s, c.w, 1080, project, c.pixelAspect);
        REQUIRE(view.valid);
        const rf::DirectSetup direct = rf::buildDirectParams(s, stitch, c.w, 1080, project, c.pixelAspect);
        INFO("direct refusal: " << rf::directRejectName(direct.reject));
        REQUIRE(direct.valid);
        CHECK(view.params.pixelAspect == static_cast<float>(c.pixelAspect));
        CHECK(direct.params.pixelAspect == view.params.pixelAspect);

        // ---- a square project builds exactly the block the default builds -------------
        if (c.pixelAspect == 1.0) {
            const rf::DirectSetup byDefault = rf::buildDirectParams(s, stitch, c.w, 1080, project);
            REQUIRE(byDefault.valid);
            CHECK(std::memcmp(&byDefault.params, &direct.params, sizeof(OsvRenderParams)) == 0);
        }

        // ---- ray for ray, over a grid of pixels -------------------------------------
        double worstRad = 0.0;
        int compared = 0;
        for (int y = 0; y < 1080; y += 53) {
            for (int x = 0; x < c.w; x += 61) {
                double a[3] = {0.0, 0.0, 0.0};
                double b[3] = {0.0, 0.0, 0.0};
                if (!spherePathDirection(view.params, pano, kPanoW, kPanoH, x, y, a) ||
                    !directPathDirection(direct.params, x, y, b)) {
                    continue;
                }
                const double dot = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
                worstRad = std::max(worstRad, std::acos(dot));
                ++compared;
            }
        }
        INFO("compared " << compared << " pixels, worst " << worstRad * 180.0 / 3.14159265358979323846 << " deg");
        CHECK(compared > 300);
        // A tenth of a texel of the 2048-wide panorama: float sampling noise,
        // where a missing pixel aspect is tens of degrees off at the edges.
        CHECK(worstRad < 0.1 * 2.0 * 3.14159265358979323846 / kPanoW);
    }
}
