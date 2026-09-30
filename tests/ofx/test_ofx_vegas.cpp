// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_vegas.cpp - both effects as VEGAS Pro sees them: the loaded
// module, driven by a mock host that says it is VEGAS.
//
// ===========================================================================
//  How these run
// ===========================================================================
// The module decides its host profile once per load, from the host's name,
// so these tests need a process whose mock host is VEGAS from the start:
//
//     set OSV_MOCK_OFX_PROFILE=vegas
//     osv_ofx_tests.exe "[vegas]"
//
// ctest does exactly that for every test here (the "vegas: ..." tests,
// tests/ofx/CMakeLists.txt).  They are hidden ([.vegas]) from a plain run,
// and SKIP when started in a process that plays another host.
//
// What they hold the module to (the facts come from VEGAS's OpenFX
// extension header, ofxSonyVegas.h, and from what is observed in VEGAS):
//
//   * descriptors: 8-bit and float, each in R G B A and B G R A; the filter
//     in the Filter context only; the generator render-unsafe; Output Levels
//     in the Colour group; the Choose button hidden; no depth in the
//     generator's clip preferences;
//   * pixels: the filter in all four formats equals the Premiere effect's
//     float render of the same (dequantised) source, within one 8-bit code;
//     the generator's output in every format and both levels is its float
//     output through the packing rule, exactly;
//   * behaviour: cloned filter instances rendering at the same time, the
//     InstanceChanged VEGAS sends for the clip "Output", frame-local
//     generator time with field renders at x.5, formats VEGAS cannot hand out
//     refused without a pixel written.

#include "OfxTestSupport.h"

#include "OfxCamera.h"
#include "OfxHost.h"
#include "OfxHostImage.h"
#include "OfxSource.h"
#include "OfxSourceParams.h"

#include "ReframeCpu.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace osv::ofxtest;
namespace cam = osv::ofx::camera;
namespace src = osv::ofx::source;
namespace sp = osv::ofx::source_params;
namespace ofx = osv::ofx;
namespace rf = osv::reframe;

namespace {

/// Every test here starts with this: the module in this process must have
/// been loaded by a VEGAS host.
#define REQUIRE_VEGAS_PROFILE()                                                                       \
    do {                                                                                              \
        if (!MockHost::instance().isVegas()) {                                                        \
            SKIP("needs OSV_MOCK_OFX_PROFILE=vegas (ctest runs these as 'vegas: ...')");              \
        }                                                                                             \
        REQUIRE(Fixture::get().ready);                                                                \
    } while (false)

/// Pins the CPU path for one test: OPENOSV_OFX_GPU=0 for its scope, the
/// previous value restored afterwards.  Under VEGAS the effects frame CPU
/// images on their own GPU by default (OfxGpuView.h); the tests here hold
/// the CPU loops to exact references, and the [cuda] tests of
/// test_ofx_cuda.cpp cover the GPU path.  ctest sets the same variable for
/// the "vegas: ..." entries; this makes a direct run deterministic too.  The
/// module shares this CRT's environment (both /MD) and reads the switch on
/// every render.
class CpuPathOnly {
public:
    CpuPathOnly() {
#if defined(_WIN32)
        // getenv_s: the same CRT environment the module reads, without
        // MSVC's deprecation of getenv.
        char old[32] = {};
        std::size_t length = 0;
        if (getenv_s(&length, old, sizeof(old), "OPENOSV_OFX_GPU") == 0 && length > 0) {
            m_had = true;
            m_old = old;
        }
#else
        if (const char* old = std::getenv("OPENOSV_OFX_GPU")) {
            m_had = true;
            m_old = old;
        }
#endif
        setSwitch("0");
    }
    ~CpuPathOnly() { setSwitch(m_had ? m_old.c_str() : ""); }
    CpuPathOnly(const CpuPathOnly&) = delete;
    CpuPathOnly& operator=(const CpuPathOnly&) = delete;

private:
    /// Write the switch; an empty value removes it.
    static void setSwitch(const char* value) {
#if defined(_WIN32)
        ::_putenv_s("OPENOSV_OFX_GPU", value);
#else
        if (value[0] == '\0') {
            ::unsetenv("OPENOSV_OFX_GPU");
        } else {
            ::setenv("OPENOSV_OFX_GPU", value, 1);
        }
#endif
    }

    bool m_had = false;
    std::string m_old;
};

constexpr int kW = 320;
constexpr int kH = 180;

/// The four formats VEGAS can hand an effect that lists its BGR tokens.
struct Format {
    ofx::HostDepth depth;
    ofx::HostOrder order;
};
constexpr Format kFormats[] = {
    {ofx::HostDepth::Byte, ofx::HostOrder::Rgba},
    {ofx::HostDepth::Byte, ofx::HostOrder::Bgra},
    {ofx::HostDepth::Float, ofx::HostOrder::Rgba},
    {ofx::HostDepth::Float, ofx::HostOrder::Bgra},
};

/// "byte BGRA", for INFO lines.
std::string formatName(const Format& format) {
    return std::string(ofx::hostDepthName(format.depth)) + " " + ofx::hostOrderName(format.order);
}

/// Where a test image says which channel order it is in.
enum class OrderLabel {
    Image,       ///< "OfxImageEffectPropPixelOrder" on the image itself.
    Clip,        ///< The same property on the clip only (where the header documents it).
    DepthToken,  ///< Only the depth label says it: OfxBitDepthByteBGR / OfxBitDepthFloatBGR.
};

/// Put `format`'s order label where `where` says, on `image` and `clip`.
void labelOrder(HostImage& image, Clip& clip, OrderLabel where) {
    const bool bgra = image.order == ofx::HostOrder::Bgra;
    switch (where) {
    case OrderLabel::Image:
        image.labelOrder = true;
        break;
    case OrderLabel::Clip:
        image.labelOrder = false;
        clip.props.setString(ofx::kPropPixelOrder, bgra ? ofx::kPixelOrderBgra : ofx::kPixelOrderRgba);
        break;
    case OrderLabel::DepthToken:
        image.labelOrder = false;
        if (bgra) {
            image.depthLabel = image.depth == ofx::HostDepth::Byte ? ofx::kBitDepthByteBgr : ofx::kBitDepthFloatBgr;
        }
        break;
    }
}

/// One filter instance with a panorama in `sourceFormat` on its Source and a
/// sentinel-filled Output in `outputFormat`.
struct VegasReframeRig {
    std::unique_ptr<Effect> effect;
    HostImage source;
    HostImage output;
    OfxRectI frame{0, 0, kW, kH};

    VegasReframeRig(const Format& sourceFormat, const Format& outputFormat, OrderLabel where = OrderLabel::Image,
                    bool negativeSource = false, int padSource = 0) {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 29.97, &st);
        REQUIRE(st == kOfxStatOK);
        // The panorama painted in float, then stored as VEGAS would hand it.
        HostImage painted = makeImage(OfxRectI{0, 0, 512, 256});
        paintPanorama(painted);
        source = convertImage(painted, sourceFormat.depth, sourceFormat.order, negativeSource, padSource);
        output = makeImage(frame, false, 0, outputFormat.depth, outputFormat.order);
        output.fill(-7.0f);
        Clip* in = effect->clip(kOfxImageEffectSimpleSourceClipName);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(in);
        REQUIRE(out);
        in->rod = OfxRectD{0, 0, 512, 256};
        labelOrder(source, *in, where);
        labelOrder(output, *out, where);
        provideImage(*in, source);
        provideImage(*out, output);
    }

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    [[nodiscard]] OfxStatus render(double time = 0.0) const {
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = frame;
        return Fixture::get().reframe.render(*effect, args);
    }
};

/// One generator instance rendering into a `format` output of kW x kH.
struct VegasSourceRig {
    std::unique_ptr<Effect> effect;
    HostImage output;
    OfxRectI frame{0, 0, kW, kH};

    VegasSourceRig(const Format& format, double fps = 29.97, int w = kW, int h = kH) : frame{0, 0, w, h} {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().source.createInstance(kOfxImageEffectContextGenerator, w, h, fps, &st);
        REQUIRE(st == kOfxStatOK);
        output = makeImage(frame, false, 0, format.depth, format.order);
        output.labelOrder = true;
        output.fill(-7.0f);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(out);
        provideImage(*out, output);
    }
    ~VegasSourceRig() {
        if (effect) {
            (void)Fixture::get().source.destroyInstance(*effect);
        }
    }
    VegasSourceRig(const VegasSourceRig&) = delete;
    VegasSourceRig& operator=(const VegasSourceRig&) = delete;

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    OfxStatus render(double time = 0.0, const std::string& field = kOfxImageFieldNone) {
        output.fill(-7.0f);
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = frame;
        args.field = field;
        return Fixture::get().source.render(*effect, args);
    }
};

/// The names of a parameter set, in definition order.
std::vector<std::string> names(const ParamSet& set) {
    std::vector<std::string> out;
    for (const auto& p : set.params) {
        out.push_back(p->name);
    }
    return out;
}

/// The sample clip, or empty when this machine has none.
std::string sampleClip() {
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        if (std::filesystem::exists(env)) {
            return env;
        }
    }
#if defined(OSV_OFX_SAMPLE_CLIP)
    if (std::filesystem::exists(OSV_OFX_SAMPLE_CLIP)) {
        return OSV_OFX_SAMPLE_CLIP;
    }
#endif
    return {};
}

}  // namespace

// ===========================================================================
//  Descriptors
// ===========================================================================

TEST_CASE("VEGAS: both effects list 8-bit and float in both orders", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    Fixture& f = Fixture::get();
    const std::vector<std::string> depths = {kOfxBitDepthByte, kOfxBitDepthFloat, ofx::kBitDepthByteBgr,
                                             ofx::kBitDepthFloatBgr};
    CHECK(f.reframe.descriptor.props.getStrings(kOfxImageEffectPropSupportedPixelDepths) == depths);
    CHECK(f.source.descriptor.props.getStrings(kOfxImageEffectPropSupportedPixelDepths) == depths);
}

TEST_CASE("VEGAS: the filter is a Filter only, fully safe", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const PropertySet& r = Fixture::get().reframe.descriptor.props;
    // Every declared context is its own entry in VEGAS's FX list.
    CHECK(r.getStrings(kOfxImageEffectPropSupportedContexts) == std::vector<std::string>{kOfxImageEffectContextFilter});
    // Clones render side by side; the filter shares nothing between renders.
    CHECK(r.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderFullySafe);
    CHECK(r.getInt(kOfxImageEffectPropSupportsMultipleClipDepths) == 0);

    OfxStatus st = kOfxStatFailed;
    auto ctx = Fixture::get().reframe.describeInContext(kOfxImageEffectContextFilter, &st);
    REQUIRE(st == kOfxStatOK);
    std::vector<std::string> expected(std::begin(cam::kAllParams), std::end(cam::kAllParams));
    CHECK(names(ctx->params) == expected);
}

TEST_CASE("VEGAS: the generator is unsafe, with Output Levels and no Choose button", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const PropertySet& s = Fixture::get().source.descriptor.props;
    CHECK(s.getStrings(kOfxImageEffectPropSupportedContexts) ==
          std::vector<std::string>{kOfxImageEffectContextGenerator});
    // A clone per render thread would open a decoder per thread.
    CHECK(s.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe);

    OfxStatus st = kOfxStatFailed;
    auto ctx = Fixture::get().source.describeInContext(kOfxImageEffectContextGenerator, &st);
    REQUIRE(st == kOfxStatOK);

    // The shared list with Output Levels right after Colour Output.
    std::vector<std::string> expected = {src::kFile, src::kChooseFile, src::kClipInfo, src::kOutput, src::kStartFrame};
    expected.insert(expected.end(), std::begin(cam::kAllParams), std::end(cam::kAllParams));
    for (const char* name : sp::kAllParams) {
        expected.emplace_back(name);
        if (std::string(name) == sp::kColorOutput) {
            expected.emplace_back(sp::kOutputLevels);
        }
    }
    CHECK(names(ctx->params) == expected);

    const Param* levels = ctx->params.find(sp::kOutputLevels);
    REQUIRE(levels);
    CHECK(levels->type == kOfxParamTypeChoice);
    CHECK(levels->props.getString(kOfxPropLabel) == "Output Levels");
    CHECK(levels->props.getStrings(kOfxParamPropChoiceOption) ==
          std::vector<std::string>{"Full range (0-255)", "Studio RGB (16-235)"});
    CHECK(levels->props.getInt(kOfxParamPropDefault) == 1);
    CHECK(levels->props.getInt(kOfxParamPropAnimates) == 0);
    CHECK(levels->props.getString(kOfxParamPropParent) == sp::kColourGroup);
    CHECK(levels->props.getString(kOfxParamPropHint).find("Studio RGB") != std::string::npos);

    // VEGAS has its own Browse button for a file path: ours is hidden, but
    // still defined, so projects and scripts that name it keep working.
    const Param* choose = ctx->params.find(src::kChooseFile);
    REQUIRE(choose);
    CHECK(choose->type == kOfxParamTypePushButton);
    CHECK(choose->props.getInt(kOfxParamPropSecret) == 1);
    const Param* file = ctx->params.find(src::kFile);
    REQUIRE(file);
    CHECK(file->props.getString(kOfxParamPropStringMode) == kOfxParamStringIsFilePath);

    // An instance starts with the button still hidden and Studio RGB chosen.
    VegasSourceRig rig(kFormats[0]);
    CHECK(rig.param(src::kChooseFile).props.getInt(kOfxParamPropSecret) == 1);
    CHECK(rig.param(sp::kOutputLevels).i == 1);
}

TEST_CASE("VEGAS: the generator leaves the output depth to the host", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    VegasSourceRig rig(kFormats[0]);
    PropertySet prefs;
    prefs.setString("OfxImageClipPropComponents_Output", kOfxImageComponentNone);
    prefs.setString("OfxImageClipPropDepth_Output", kOfxBitDepthByte);  // an 8-bit project
    prefs.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    prefs.setInt(kOfxImageEffectFrameVarying, 0);
    REQUIRE(Fixture::get().source.clipPreferences(*rig.effect, prefs) == kOfxStatOK);
    CHECK(prefs.getString("OfxImageClipPropDepth_Output") == kOfxBitDepthByte);  // untouched
    CHECK(prefs.getString("OfxImageClipPropComponents_Output") == kOfxImageComponentRGBA);
    CHECK(prefs.getInt(kOfxImageEffectFrameVarying) == 1);
    CHECK(prefs.getString(kOfxImageEffectPropPreMultiplication) == kOfxImageUnPreMultiplied);
}

// ===========================================================================
//  The filter's pixels
// ===========================================================================

TEST_CASE("VEGAS: the filter in every format equals the float reference within one code", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    // The order label in each of the places a host may put it.
    const OrderLabel places[] = {OrderLabel::Image, OrderLabel::Clip, OrderLabel::DepthToken};
    int variant = 0;
    for (const Format& format : kFormats) {
        for (const OrderLabel where : places) {
            INFO(formatName(format) << ", order label " << static_cast<int>(where));
            // Odd pitches and row orders along the way.
            VegasReframeRig rig(format, format, where, (variant % 2) == 1, (variant % 3) * 4);
            ++variant;
            rig.param(cam::kPan).d = 52.0;
            rig.param(cam::kTilt).d = -14.0;
            rig.param(cam::kDjiFov).d = 84.0;
            REQUIRE(rig.render(3.0) == kOfxStatOK);

            // The Premiere effect's float render of the same source as the
            // plug-in's sampler reads it (8-bit codes promoted by 1/255).
            rf::Settings s = defaultSettings();
            s.panDeg = 52.0;
            s.tiltDeg = -14.0;
            s.djiFovDeg = 84.0;
            const HostImage ref = referenceRender(s, rig.source, rig.frame, {kW, kH});
            const double worst = maxPackedDifference(rig.output, ref, rig.frame);
            if (format.depth == ofx::HostDepth::Byte) {
                CHECK(worst <= 1.0);  // codes
            } else {
                CHECK(worst == 0.0);  // the float path is exact
            }
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: an 8-bit source into a float output still frames", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    // VEGAS keeps one depth per effect (no multiple clip depths); the CPU
    // loop reads and writes each image in its own format regardless.
    VegasReframeRig rig({ofx::HostDepth::Byte, ofx::HostOrder::Bgra}, {ofx::HostDepth::Float, ofx::HostOrder::Rgba});
    REQUIRE(rig.render() == kOfxStatOK);
    const HostImage ref = referenceRender(defaultSettings(), rig.source, rig.frame, {kW, kH});
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
}

TEST_CASE("VEGAS: cloned filter instances render at the same time", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    // VEGAS clones a fully-safe effect once per render thread and renders
    // the clones concurrently.  Each clone here has its own camera and its
    // own format, so any state shared by mistake shows up as a wrong picture.
    constexpr int kClones = 4;
    constexpr int kRounds = 3;
    std::vector<std::unique_ptr<VegasReframeRig>> clones;
    std::vector<HostImage> references;
    for (int i = 0; i < kClones; ++i) {
        clones.push_back(std::make_unique<VegasReframeRig>(kFormats[i % 4], kFormats[(i + 1) % 4]));
        clones.back()->param(cam::kPan).d = -90.0 + 45.0 * i;
        rf::Settings s = defaultSettings();
        s.panDeg = -90.0 + 45.0 * i;
        references.push_back(referenceRender(s, clones.back()->source, clones.back()->frame, {kW, kH}));
    }

    // Catch2's assertions are not thread-safe: the threads only record.
    std::vector<OfxStatus> statuses(static_cast<std::size_t>(kClones * kRounds), kOfxStatFailed);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kClones; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int r = 0; r < kRounds; ++r) {
                statuses[static_cast<std::size_t>(i * kRounds + r)] = clones[static_cast<std::size_t>(i)]->render(r);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }

    for (std::size_t k = 0; k < statuses.size(); ++k) {
        INFO("render " << k);
        CHECK(statuses[k] == kOfxStatOK);
    }
    for (int i = 0; i < kClones; ++i) {
        INFO("clone " << i);
        const VegasReframeRig& rig = *clones[static_cast<std::size_t>(i)];
        const double worst = maxPackedDifference(rig.output, references[static_cast<std::size_t>(i)], rig.frame);
        CHECK(worst <= (rig.output.depth == ofx::HostDepth::Byte ? 1.0 : 0.0));
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: images VEGAS cannot hand out are refused without a pixel written", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const Format bytes{ofx::HostDepth::Byte, ofx::HostOrder::Bgra};

    // A 16-bit label: not a depth either effect declares.
    {
        VegasReframeRig rig(bytes, bytes);
        rig.output.depthLabel = kOfxBitDepthShort;
        CHECK(rig.render() == kOfxStatErrImageFormat);
        CHECK(rig.output.rawPixel(10, 10)[0] == HostImage::kByteSentinel);
    }
    // An order this plug-in cannot name.
    {
        VegasReframeRig rig(bytes, bytes);
        HostImage& image = rig.output;
        rig.effect->clip(kOfxImageEffectOutputClipName)->provide = [&image](double, PropertySet& props) {
            image.describe(props);
            props.setString(ofx::kPropPixelOrder, "OfxImagePixelOrderARGB");
            return true;
        };
        CHECK(rig.render() == kOfxStatErrImageFormat);
        CHECK(rig.output.rawPixel(10, 10)[0] == HostImage::kByteSentinel);
    }
    // A pitch too short for a row of 8-bit pixels.
    {
        VegasReframeRig rig(bytes, bytes);
        HostImage& image = rig.output;
        rig.effect->clip(kOfxImageEffectOutputClipName)->provide = [&image](double, PropertySet& props) {
            image.describe(props);
            props.setInt(kOfxImagePropRowBytes, kW * 4 - 4);
            return true;
        };
        CHECK(rig.render() == kOfxStatErrImageFormat);
        CHECK(rig.output.rawPixel(10, 10)[0] == HostImage::kByteSentinel);
    }
    // An unusable SOURCE is a gap, not an error: transparent black.
    {
        VegasReframeRig rig(bytes, bytes);
        rig.source.depthLabel = kOfxBitDepthShort;
        REQUIRE(rig.render() == kOfxStatOK);
        for (int c = 0; c < 4; ++c) {
            CHECK(rig.output.rawPixel(10, 10)[c] == 0);
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

// ===========================================================================
//  InstanceChanged for a clip
// ===========================================================================

TEST_CASE("VEGAS: an InstanceChanged for the clip Output is not a parameter edit", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const std::size_t messagesBefore = MockHost::instance().messages.size();
    const auto noWrites = [](const Effect& effect) {
        for (const auto& p : effect.params.params) {
            if (p->pluginWrites != 0) {
                return false;
            }
        }
        return true;
    };

    VegasReframeRig filter({ofx::HostDepth::Byte, ofx::HostOrder::Rgba}, {ofx::HostDepth::Byte, ofx::HostOrder::Rgba});
    // Named like a camera control on purpose: only the TYPE tells them apart.
    CHECK(Fixture::get().reframe.instanceChangedClip(*filter.effect, kOfxImageEffectOutputClipName,
                                                     kOfxChangeUserEdited, 0.0) == kOfxStatReplyDefault);
    CHECK(Fixture::get().reframe.instanceChangedClip(*filter.effect, cam::kPreset, kOfxChangeUserEdited, 0.0) ==
          kOfxStatReplyDefault);
    CHECK(noWrites(*filter.effect));

    VegasSourceRig generator(kFormats[1]);
    CHECK(Fixture::get().source.instanceChangedClip(*generator.effect, kOfxImageEffectOutputClipName,
                                                    kOfxChangeUserEdited, 0.0) == kOfxStatReplyDefault);
    CHECK(Fixture::get().source.instanceChangedClip(*generator.effect, src::kFile, kOfxChangePluginEdited, 0.0) ==
          kOfxStatReplyDefault);
    CHECK(noWrites(*generator.effect));
    CHECK(generator.param(src::kClipInfo).s == "No clip chosen.");
    CHECK(MockHost::instance().messages.size() == messagesBefore);
}

// ===========================================================================
//  The generator without a clip
// ===========================================================================

TEST_CASE("VEGAS: with no clip the generator clears in its own format and levels", "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    const ofx::OutputLevels allLevels[] = {ofx::OutputLevels::Full, ofx::OutputLevels::Studio};
    for (const Format& format : kFormats) {
        for (const ofx::OutputLevels levels : allLevels) {
            INFO(formatName(format) << " " << ofx::outputLevelsName(levels));
            VegasSourceRig rig(format);
            rig.param(sp::kOutputLevels).i = static_cast<int>(levels);
            // Frame-local time, the second field of a field render.
            REQUIRE(rig.render(12.5, kOfxImageFieldUpper) == kOfxStatOK);
            // Transparent black through the packing rule: RGB at studio black
            // in a Studio project, alpha 0 either way.
            HostImage black = makeImage(rig.frame);
            black.fill(0.0f);
            CHECK(maxPackedDifference(rig.output, black, rig.frame, levels) == 0.0);
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

// ===========================================================================
//  The sample clip (NVDEC / GPU: not part of the CPU-only runs)
// ===========================================================================

TEST_CASE("VEGAS: the generator's output in every format and levels is its float output packed",
          "[ofx][.vegas][sample]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    const int outputs[] = {src::kOutputEquirect, src::kOutputReframed};
    const ofx::OutputLevels allLevels[] = {ofx::OutputLevels::Full, ofx::OutputLevels::Studio};
    for (const int mode : outputs) {
        // The reference: float R G B A at full range - Resolve's output.
        VegasSourceRig reference({ofx::HostDepth::Float, ofx::HostOrder::Rgba}, 29.97, 512, 256);
        reference.param(src::kFile).s = clip;
        reference.param(src::kOutput).i = mode;
        reference.param(cam::kPan).d = 25.0;
        reference.param(sp::kOutputLevels).i = static_cast<int>(ofx::OutputLevels::Full);
        REQUIRE(reference.render(4.0) == kOfxStatOK);

        for (const Format& format : kFormats) {
            for (const ofx::OutputLevels levels : allLevels) {
                INFO("output " << mode << ", " << formatName(format) << " " << ofx::outputLevelsName(levels));
                VegasSourceRig rig(format, 29.97, 512, 256);
                rig.param(src::kFile).s = clip;
                rig.param(src::kOutput).i = mode;
                rig.param(cam::kPan).d = 25.0;
                rig.param(sp::kOutputLevels).i = static_cast<int>(levels);
                REQUIRE(rig.render(4.0) == kOfxStatOK);
                CHECK(maxPackedDifference(rig.output, reference.output, rig.frame, levels) == 0.0);
            }
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: a field render at x.5 shows the frame x shows", "[ofx][.vegas][sample]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loops against exact references
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // The timeline at the clip's own rate, read off the Clip read-out.
    double clipFps = 0.0;
    {
        VegasSourceRig probe(kFormats[2], 29.97, 64, 32);
        probe.param(src::kFile).s = clip;
        REQUIRE(Fixture::get().source.instanceChanged(*probe.effect, src::kFile, kOfxChangeUserEdited, 0.0) ==
                kOfxStatOK);
        const std::string info = probe.param(src::kClipInfo).s;
        const std::size_t at = info.find("frames at ");
        REQUIRE(at != std::string::npos);
        clipFps = std::stod(info.substr(at + 10));
    }
    REQUIRE(clipFps > 1.0);

    VegasSourceRig whole(kFormats[0], clipFps, 256, 128);
    whole.param(src::kFile).s = clip;
    whole.param(src::kOutput).i = src::kOutputEquirect;
    REQUIRE(whole.render(7.0) == kOfxStatOK);
    const HostImage first = whole.output;
    REQUIRE(whole.render(7.5, kOfxImageFieldUpper) == kOfxStatOK);
    CHECK(maxDifference(first, whole.output, whole.frame) == 0.0);
}

// ===========================================================================
//  VEGAS's parameter roll-call
// ===========================================================================
// Right after it creates an instance, VEGAS 17 sends one bracket announcing
// every parameter in definition order, each as kOfxChangeUserEdited, and
// sends it twice (observed live).  Taken as edits, it re-applied the preset,
// switched the lens to Classic and re-derived the DJI lens from Zoom; the
// undo group those edits opened is also what crashed VEGAS.  Only a bracket's
// first change may count as the user's.

namespace {

/// Begin, one user-edited change per parameter in definition order, End.
void vegasRollCall(PluginHarness& harness, Effect& effect) {
    PropertySet in;
    PropertySet out;
    REQUIRE(harness.action(kOfxActionBeginInstanceChanged, effect.handle(), &in, &out) == kOfxStatReplyDefault);
    for (const auto& p : effect.params.params) {
        (void)harness.instanceChanged(effect, p->name, kOfxChangeUserEdited, 0.0);
    }
    REQUIRE(harness.action(kOfxActionEndInstanceChanged, effect.handle(), &in, &out) == kOfxStatReplyDefault);
}

/// The plug-in's own writes to every parameter but `except`.
int writesExcept(const Effect& effect, const std::string& except) {
    int writes = 0;
    for (const auto& p : effect.params.params) {
        if (p->name != except) {
            writes += p->pluginWrites;
        }
    }
    return writes;
}

}  // namespace

TEST_CASE("VEGAS: the roll-call after CreateInstance edits nothing; one change per bracket still does",
          "[ofx][.vegas]") {
    REQUIRE_VEGAS_PROFILE();
    PluginHarness& reframe = Fixture::get().reframe;

    // ---- the filter: two roll-calls, not a single write ------------------------
    VegasReframeRig filter(kFormats[0], kFormats[0]);
    const int lensBefore = filter.param(cam::kLens).i;
    const int presetBefore = filter.param(cam::kPreset).i;
    vegasRollCall(reframe, *filter.effect);
    vegasRollCall(reframe, *filter.effect);
    CHECK(writesExcept(*filter.effect, std::string()) == 0);
    CHECK(filter.param(cam::kLens).i == lensBefore);
    CHECK(filter.param(cam::kPreset).i == presetBefore);

    // ---- a real edit, alone in its bracket, still drives the camera -------------
    filter.param(cam::kLens).i = 1;  // "DJI|Classic": Classic
    {
        PropertySet in;
        PropertySet out;
        REQUIRE(reframe.action(kOfxActionBeginInstanceChanged, filter.effect->handle(), &in, &out) ==
                kOfxStatReplyDefault);
        CHECK(reframe.instanceChanged(*filter.effect, cam::kLens, kOfxChangeUserEdited, 0.0) == kOfxStatOK);
        REQUIRE(reframe.action(kOfxActionEndInstanceChanged, filter.effect->handle(), &in, &out) ==
                kOfxStatReplyDefault);
    }
    CHECK(filter.param(cam::kLensMirror).pluginWrites > 0);  // the switch was carried out

    // ---- the generator: only the file's read-out may refresh ---------------------
    VegasSourceRig generator(kFormats[1]);
    vegasRollCall(Fixture::get().source, *generator.effect);
    vegasRollCall(Fixture::get().source, *generator.effect);
    CHECK(writesExcept(*generator.effect, src::kClipInfo) == 0);
}
