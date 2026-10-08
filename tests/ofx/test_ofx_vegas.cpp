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
//     in the Colour group and Playback Proxy in the Advanced group; the
//     Choose button hidden; no depth in the generator's clip preferences;
//   * playback: a Preview-quality frame comes from the .LRF proxy, a
//     Good-quality one from the .OSV;
//   * pixels: the filter in all four formats equals the Premiere effect's
//     float render of the same (dequantised) source, within one 8-bit code;
//     the generator's output in every format and both levels is its float
//     output through the packing rule, exactly;
//   * behaviour: cloned filter instances rendering at the same time, the
//     InstanceChanged VEGAS sends for the clip "Output", frame-local
//     generator time with field renders at x.5, formats VEGAS cannot hand out
//     refused without a pixel written;
//   * [WP-PAR] geometry: a non-square project (HDV 4:3, 2:1, 1:2) framed as
//     it is displayed - its grid lines where the square-pixel render has
//     them - and a square one exactly as before;
//   * stabilisation: Smooth + Horizon Lock reaches the reframed view exactly
//     as the engine applies it to its own sphere, from the .OSV and from the
//     .LRF proxy.

#include "OfxTestSupport.h"

#include "OfxCamera.h"
#include "OfxHost.h"
#include "OfxHostImage.h"
#include "OfxSource.h"
#include "OfxSourceParams.h"

#include "ReframeCpu.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
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

    /// Render `time`; `quality` is VEGAS's OfxImageEffectPropRenderQuality
    /// value (empty: not set, as a VEGAS without it would leave it).
    OfxStatus render(double time = 0.0, const std::string& field = kOfxImageFieldNone,
                     const std::string& quality = {}) {
        output.fill(-7.0f);
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = frame;
        args.field = field;
        args.quality = quality;
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

    // The shared list with Output Levels right after Colour Output and
    // Playback Proxy right after Sphere Size.
    std::vector<std::string> expected = {src::kFile, src::kChooseFile, src::kClipInfo, src::kOutput, src::kStartFrame};
    expected.insert(expected.end(), std::begin(cam::kAllParams), std::end(cam::kAllParams));
    for (const char* name : sp::kAllParams) {
        expected.emplace_back(name);
        if (std::string(name) == sp::kColorOutput) {
            expected.emplace_back(sp::kOutputLevels);
        }
        if (std::string(name) == sp::kSphereSize) {
            expected.emplace_back(sp::kPlaybackProxy);
        }
    }
    CHECK(names(ctx->params) == expected);

    // Playback Proxy: a static checkbox in the Advanced group, on by default.
    const Param* proxy = ctx->params.find(sp::kPlaybackProxy);
    REQUIRE(proxy);
    CHECK(proxy->type == kOfxParamTypeBoolean);
    CHECK(proxy->props.getString(kOfxPropLabel) == "Playback Proxy");
    CHECK(proxy->props.getInt(kOfxParamPropDefault) == 1);
    CHECK(proxy->props.getInt(kOfxParamPropAnimates) == 0);
    CHECK(proxy->props.getString(kOfxParamPropParent) == sp::kAdvancedGroup);
    CHECK(proxy->props.getString(kOfxParamPropHint).find(".LRF") != std::string::npos);

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
//  VEGAS playback: the quality it names, and the .LRF proxy
// ===========================================================================
// VEGAS names a quality for every render (OfxHost.h, renderModeFor()): its
// Preview window plays at Draft or Preview, a file render uses Good or Best.
// With Playback Proxy on, the generator stitches a Draft / Preview frame from
// the .LRF the camera recorded beside the .OSV, at the same moment; Good and
// Best always stitch the .OSV.

namespace {

/// Mean luma and the luma correlation of two float renders over `area`,
/// counting only pixels both cover (alpha > 0.99).
struct LumaComparison {
    double meanA = 0.0;
    double meanB = 0.0;
    double correlation = 0.0;
    std::size_t pixels = 0;
};

LumaComparison compareLuma(const HostImage& a, const HostImage& b, const OfxRectI& area) {
    std::vector<double> la;
    std::vector<double> lb;
    for (int y = area.y1; y < area.y2; ++y) {
        for (int x = area.x1; x < area.x2; ++x) {
            const float* p = a.pixel(x, y);
            const float* q = b.pixel(x, y);
            if (!p || !q || !(p[3] > 0.99f) || !(q[3] > 0.99f)) {
                continue;
            }
            // Rec. 709 luma weights; the exact weights do not matter here.
            la.push_back(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]);
            lb.push_back(0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]);
        }
    }
    LumaComparison c;
    c.pixels = la.size();
    if (c.pixels < 2) {
        return c;
    }
    for (std::size_t i = 0; i < c.pixels; ++i) {
        c.meanA += la[i];
        c.meanB += lb[i];
    }
    c.meanA /= static_cast<double>(c.pixels);
    c.meanB /= static_cast<double>(c.pixels);
    double sab = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    for (std::size_t i = 0; i < c.pixels; ++i) {
        const double da = la[i] - c.meanA;
        const double db = lb[i] - c.meanB;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    c.correlation = (saa > 0.0 && sbb > 0.0) ? sab / std::sqrt(saa * sbb) : 0.0;
    return c;
}

/// True when the render wrote every pixel of `area` (no -7 sentinel left).
bool everyPixelWritten(const HostImage& image, const OfxRectI& area) {
    for (int y = area.y1; y < area.y2; ++y) {
        for (int x = area.x1; x < area.x2; ++x) {
            const float* p = image.pixel(x, y);
            if (!p || p[3] == -7.0f) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("VEGAS: Preview playback plays the .LRF proxy, Good and Playback Proxy off stitch the .OSV",
          "[ofx][.vegas][sample]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the engine's own render, no GPU framing
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // The camera's proxy beside the sample clip, in either spelling.
    std::filesystem::path lrf = clip;
    lrf.replace_extension(".LRF");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(lrf, ec)) {
        lrf.replace_extension(".lrf");
        if (!std::filesystem::is_regular_file(lrf, ec)) {
            SKIP("no .LRF beside the sample clip");
        }
    }

    /// One float render of the same moment and camera at `quality`, with
    /// Playback Proxy `proxyOn`, at full levels.
    const auto renderAt = [&](const char* quality, bool proxyOn) {
        VegasSourceRig rig(kFormats[2], 29.97, 192, 108);
        rig.param(src::kFile).s = clip;
        rig.param(cam::kPan).d = 30.0;
        rig.param(cam::kTilt).d = -5.0;
        rig.param(sp::kPlaybackProxy).i = proxyOn ? 1 : 0;
        rig.param(sp::kOutputLevels).i = static_cast<int>(ofx::OutputLevels::Full);
        REQUIRE(rig.render(10.0, kOfxImageFieldNone, quality) == kOfxStatOK);
        CHECK(everyPixelWritten(rig.output, rig.frame));
        return rig.output;
    };
    const OfxRectI frame{0, 0, 192, 108};

    // ---- Good: a file render's quality stitches the .OSV, proxy or not ----------
    const HostImage goodOn = renderAt(ofx::kVegasQualityGood, true);
    const HostImage goodOff = renderAt(ofx::kVegasQualityGood, false);
    CHECK(maxDifference(goodOn, goodOff, frame) == 0.0);

    // ---- Preview: the proxy serves it - the same moment, other pixels ------------
    const HostImage previewOff = renderAt(ofx::kVegasQualityPreview, false);
    const HostImage previewOn = renderAt(ofx::kVegasQualityPreview, true);
    CHECK(maxDifference(previewOn, previewOff, frame) > 0.0);
    // The same moment through the same camera: the picture agrees in
    // brightness and structure, up to the proxy's resolution and 8-bit H.264.
    const LumaComparison c = compareLuma(previewOn, previewOff, frame);
    INFO("covered " << c.pixels << ", luma " << c.meanA << " vs " << c.meanB << ", correlation " << c.correlation);
    CHECK(c.pixels > static_cast<std::size_t>(192 * 108 * 9 / 10));
    CHECK(std::fabs(c.meanA - c.meanB) < 0.08);
    CHECK(c.correlation > 0.85);
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

// ===========================================================================
//  [WP-PAR] Non-square project pixels
// ===========================================================================
// A VEGAS project can have non-square pixels: HDV 1440 x 1080 is displayed
// 1920 x 1080 (pixel aspect 4:3), anamorphic formats at 2:1, and so on.  The
// host hands the effect an image of the PIXEL size and shows every pixel
// `pixelAspect` times as wide as it is tall, so the camera must frame the
// DISPLAYED picture - before 0.5.1 it framed the pixel grid as if square, and
// the view came out stretched horizontally by the pixel aspect.
//
// Measured with a coordinate panorama: R is the longitude and G the latitude
// of the equirect column / row, linear ramps the bilinear sampler reproduces
// exactly, so a rendered pixel's R and G name the direction it sees and a
// "grid line" (every 5 degrees) is where a channel crosses a level.  The
// positions of those lines in DISPLAY units ((pixel index + 0.5) x pixel
// aspect) must be those of the square-pixel 1920 x 1080 render.

namespace {

/// The display size every case frames: 1920 x 1080 square display units.
constexpr int kDisplayW = 1920;
constexpr int kDisplayH = 1080;

/// The longest a grid line may sit from its square-pixel position, in
/// display pixels (the brief's acceptance threshold).
constexpr double kGridTolerancePx = 0.5;

/// R = (longitude + 180) / 360 and G = (latitude + 90) / 180 of each texel's
/// centre, B = 0.5, A = 1 - painted into an OpenFX-layout (bottom-up) image.
void paintCoordinates(HostImage& image) {
    const int w = image.width();
    const int h = image.height();
    for (int y = image.bounds.y1; y < image.bounds.y2; ++y) {
        // Row counted from the TOP: row 0 is latitude +90.
        const int rowFromTop = image.bounds.y2 - 1 - y;
        const float g = 1.0f - (static_cast<float>(rowFromTop) + 0.5f) / static_cast<float>(h);
        for (int x = image.bounds.x1; x < image.bounds.x2; ++x) {
            const float r = (static_cast<float>(x - image.bounds.x1) + 0.5f) / static_cast<float>(w);
            const float p[4] = {r, g, 0.5f, 1.0f};
            image.writeRgba(x, y, p);
        }
    }
}

/// The R level of longitude `deg` and the G level of latitude `deg`.
double longitudeLevel(double deg) { return (deg + 180.0) / 360.0; }
double latitudeLevel(double deg) { return (deg + 90.0) / 180.0; }

/// The grid every case measures: every 5 degrees from -`span` to +`span`.
std::vector<double> gridDegrees(double span) {
    std::vector<double> out;
    for (double d = -span; d <= span + 1e-9; d += 5.0) {
        out.push_back(d);
    }
    return out;
}

/// Channel `channel` of float image `image` at the fractional pixel-centre
/// column `xf` of row `y`, linear between the two nearest columns; NaN off
/// the image or where a pixel saw nothing (alpha 0).
double channelAt(const HostImage& image, double xf, int y, int channel) {
    if (!std::isfinite(xf) || channel < 0 || channel > 3) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const int x0 = static_cast<int>(std::floor(xf));
    const double t = xf - static_cast<double>(x0);
    const float* a = image.pixel(image.bounds.x1 + x0, y);
    const float* b = image.pixel(image.bounds.x1 + std::min(x0 + 1, image.width() - 1), y);
    if (!a || !b || !(a[3] > 0.5f) || !(b[3] > 0.5f)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(a[channel]) * (1.0 - t) + static_cast<double>(b[channel]) * t;
}

/// Where the increasing profile `values` (index = pixel-centre index) crosses
/// each of `levels`, as a fractional index; NaN for a level it never crosses
/// between two finite samples.
std::vector<double> crossingsOf(const std::vector<double>& values, const std::vector<double>& levels) {
    std::vector<double> out(levels.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t k = 0; k < levels.size(); ++k) {
        for (std::size_t i = 0; i + 1 < values.size(); ++i) {
            const double v0 = values[i];
            const double v1 = values[i + 1];
            if (std::isfinite(v0) && std::isfinite(v1) && v0 <= levels[k] && levels[k] < v1) {
                out[k] = static_cast<double>(i) + (levels[k] - v0) / (v1 - v0);
                break;
            }
        }
    }
    return out;
}

/// The longitude lines crossing output row `y`, in DISPLAY units from the
/// left edge: (fractional index + 0.5) x `pixelAspect`.
std::vector<double> longitudeLines(const HostImage& image, int y, double pixelAspect, const std::vector<double>& deg) {
    std::vector<double> profile(static_cast<std::size_t>(image.width()));
    for (int x = 0; x < image.width(); ++x) {
        profile[static_cast<std::size_t>(x)] = channelAt(image, static_cast<double>(x), y, 0);
    }
    std::vector<double> levels;
    for (double d : deg) {
        levels.push_back(longitudeLevel(d));
    }
    std::vector<double> lines = crossingsOf(profile, levels);
    for (double& v : lines) {
        v = (v + 0.5) * pixelAspect;
    }
    return lines;
}

/// The latitude lines crossing the output column at DISPLAY position
/// `displayX` (interpolated between pixel columns), as fractional row
/// indices counted from the image's first row (rows are square already).
std::vector<double> latitudeLines(const HostImage& image, double displayX, double pixelAspect,
                                  const std::vector<double>& deg) {
    const double xf = displayX / pixelAspect - 0.5;
    std::vector<double> profile(static_cast<std::size_t>(image.height()));
    for (int y = 0; y < image.height(); ++y) {
        profile[static_cast<std::size_t>(y)] = channelAt(image, xf, image.bounds.y1 + y, 1);
    }
    std::vector<double> levels;
    for (double d : deg) {
        levels.push_back(latitudeLevel(d));
    }
    return crossingsOf(profile, levels);
}

/// The largest distance between matching lines of `a` and `b` (both finite),
/// and how many pairs were compared.
struct LineError {
    double worst = 0.0;
    int pairs = 0;
};
LineError lineError(const std::vector<double>& a, const std::vector<double>& b) {
    LineError e;
    for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
        if (std::isfinite(a[k]) && std::isfinite(b[k])) {
            e.worst = std::max(e.worst, std::fabs(a[k] - b[k]));
            ++e.pairs;
        }
    }
    return e;
}

/// One VEGAS filter instance in a `pixelW` x kDisplayH project of pixel
/// aspect `pixelAspect`: float R G B A, the coordinate panorama on its Source.
struct ParFilter {
    std::unique_ptr<Effect> effect;
    HostImage source;
    HostImage output;
    OfxRectI frame;
    double pixelAspect;

    ParFilter(int pixelW, double aspect) : frame{0, 0, pixelW, kDisplayH}, pixelAspect(aspect) {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, pixelW, kDisplayH, 29.97, &st,
                                                       aspect);
        REQUIRE(st == kOfxStatOK);
        source = makeImage(OfxRectI{0, 0, 2048, 1024});
        paintCoordinates(source);
        output = makeImage(frame);
        output.fill(-7.0f);
        Clip* in = effect->clip(kOfxImageEffectSimpleSourceClipName);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(in);
        REQUIRE(out);
        in->rod = OfxRectD{0, 0, 2048, 1024};
        provideImage(*in, source);
        provideImage(*out, output);
    }

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    [[nodiscard]] OfxStatus render() const {
        PluginHarness::RenderArgs args;
        args.window = frame;
        return Fixture::get().reframe.render(*effect, args);
    }
};

/// One lens set-up, in the filter's controls and as the Premiere effect's
/// Settings for the reference renders.
struct LensCase {
    const char* name;
    bool classic;
};
constexpr LensCase kLensCases[] = {{"DJI", false}, {"Classic", true}};

/// Apply `lens` to a filter's controls.
void applyLens(ParFilter& filter, const LensCase& lens) {
    if (lens.classic) {
        filter.param(cam::kLens).i = 1;  // "DJI|Classic": Classic
        filter.param(cam::kFov).d = 100.0;
        filter.param(cam::kDistortion).d = 30.0;
    } else {
        filter.param(cam::kDjiFov).d = 70.0;
        filter.param(cam::kCorrection).d = 0.5;
    }
    filter.param(cam::kTilt).d = 6.0;
}

/// The same look as Premiere's Settings.
rf::Settings lensSettings(const LensCase& lens) {
    rf::Settings s = defaultSettings();
    if (lens.classic) {
        s.cameraModel = rf::CameraModel::Classic;
        s.fovDeg = 100.0;
        s.distortion = 30.0;
    } else {
        s.djiFovDeg = 70.0;
        s.correction = 0.5;
    }
    s.tiltDeg = 6.0;
    return s;
}

/// Longitude lines on three rows and latitude lines on three display columns
/// of `test` (pixel aspect `testAspect`) against `reference` (pixel aspect
/// `referenceAspect`, 1.0 for the square-pixel render), both measured in
/// display units: the worst distance between matching lines.
LineError gridError(const HostImage& test, double testAspect, const HostImage& reference,
                    double referenceAspect = 1.0) {
    const std::vector<double> lon = gridDegrees(40.0);
    const std::vector<double> lat = gridDegrees(25.0);
    LineError total;
    for (const int y : {kDisplayH / 5, kDisplayH / 2, (4 * kDisplayH) / 5}) {
        const LineError e = lineError(longitudeLines(test, y, testAspect, lon),
                                      longitudeLines(reference, y, referenceAspect, lon));
        total.worst = std::max(total.worst, e.worst);
        total.pairs += e.pairs;
    }
    for (const double x : {kDisplayW * 0.25, kDisplayW * 0.5, kDisplayW * 0.75}) {
        const LineError e = lineError(latitudeLines(test, x, testAspect, lat),
                                      latitudeLines(reference, x, referenceAspect, lat));
        total.worst = std::max(total.worst, e.worst);
        total.pairs += e.pairs;
    }
    return total;
}

}  // namespace

TEST_CASE("VEGAS: a non-square project is framed as it is displayed, not stretched", "[ofx][.vegas][par]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the CPU loop against the square-pixel render
    // The pixel aspects of real formats whose display width is exactly 1920:
    // HDV (1440 at 4:3), anamorphic 2:1 (960) and half-width pixels (3840).
    struct ParCase {
        int pixelW;
        double pixelAspect;
    };
    const ParCase kCases[] = {{1440, 4.0 / 3.0}, {960, 2.0}, {3840, 0.5}};

    for (const LensCase& lens : kLensCases) {
        // ---- the square-pixel picture every case must reproduce ------------------
        ParFilter square(kDisplayW, 1.0);
        applyLens(square, lens);
        REQUIRE(square.render() == kOfxStatOK);

        // Square pixels are bit for bit the Premiere effect's render.
        const HostImage squareRef =
            referenceRender(lensSettings(lens), square.source, square.frame, {kDisplayW, kDisplayH});
        CHECK(maxDifference(square.output, squareRef, square.frame) == 0.0);

        for (const ParCase& c : kCases) {
            INFO(lens.name << " lens, " << c.pixelW << " x " << kDisplayH << " at pixel aspect " << c.pixelAspect);
            ParFilter filter(c.pixelW, c.pixelAspect);
            applyLens(filter, lens);
            REQUIRE(filter.render() == kOfxStatOK);

            // ---- the grid in display units is the square-pixel grid --------------
            const LineError now = gridError(filter.output, c.pixelAspect, square.output);
            INFO("grid lines compared " << now.pairs << ", worst " << now.worst << " display px");
            CHECK(now.pairs >= 60);
            CHECK(now.worst < kGridTolerancePx);

            // ---- and the square-pixel assumption (the camera every build before
            // 0.5.1 used: the frame's pixels taken as square) misses it by far ----
            const HostImage stretched =
                referenceRender(lensSettings(lens), filter.source, filter.frame, {c.pixelW, kDisplayH});
            const LineError before = gridError(stretched, c.pixelAspect, square.output);
            INFO("square-pixel assumption: worst " << before.worst << " display px");
            CHECK(before.worst > 20.0);
        }

        // ---- a named Output Resolution is a display shape ----------------------------
        // "1920 x 1080" in an HDV project is the frame's own 16:9: nothing is
        // cropped, so it frames exactly like Match Timeline.
        ParFilter match(1440, 4.0 / 3.0);
        applyLens(match, lens);
        REQUIRE(match.render() == kOfxStatOK);
        ParFilter named(1440, 4.0 / 3.0);
        applyLens(named, lens);
        named.param(cam::kOutputResolution).i = static_cast<int>(rf::Resolution::Fhd1920x1080) - 1;
        REQUIRE(named.render() == kOfxStatOK);
        const LineError shape = gridError(named.output, 4.0 / 3.0, match.output, 4.0 / 3.0);
        INFO(lens.name << " lens: named 1920 x 1080 vs Match Timeline, worst " << shape.worst << " display px");
        CHECK(shape.pairs >= 60);
        CHECK(shape.worst < 0.01);
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: the Zoom read-out and the presets use the shape a non-square project displays",
          "[ofx][.vegas][par]") {
    REQUIRE_VEGAS_PROFILE();
    // An HDV project (1440 x 1080 pixels at 4:3) displays 16:9, so every DJI
    // conversion must give what it gives in a square 1920 x 1080 project.
    const auto edit = [](ParFilter& filter, const char* name) {
        return Fixture::get().reframe.instanceChanged(*filter.effect, name, kOfxChangeUserEdited, 0.0);
    };
    const auto same = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); };

    // ---- a typed Zoom moves FOV and Correction along the same path ----------------
    ParFilter square(kDisplayW, 1.0);
    ParFilter hdv(1440, 4.0 / 3.0);
    square.param(cam::kZoom).d = 100.0;
    hdv.param(cam::kZoom).d = 100.0;
    REQUIRE(edit(square, cam::kZoom) == kOfxStatOK);
    REQUIRE(edit(hdv, cam::kZoom) == kOfxStatOK);
    INFO("square: DJI FOV " << square.param(cam::kDjiFov).d << ", correction " << square.param(cam::kCorrection).d
                            << "; HDV: DJI FOV " << hdv.param(cam::kDjiFov).d << ", correction "
                            << hdv.param(cam::kCorrection).d);
    CHECK(same(hdv.param(cam::kDjiFov).d, square.param(cam::kDjiFov).d));
    CHECK(same(hdv.param(cam::kCorrection).d, square.param(cam::kCorrection).d));
    CHECK(same(hdv.param(cam::kZoom).d, square.param(cam::kZoom).d));
    // And not what the 4:3 PIXEL shape would have given.
    const rf::DjiLens pixelShape = rf::djiZoomTo(
        100.0, rf::DjiLens{OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_CORRECTION_DEFAULT}, 1440.0 / 1080.0);
    CHECK_FALSE(same(hdv.param(cam::kDjiFov).d, pixelShape.fovDeg));

    // ---- a ratio the camera does not honour is square here too --------------------
    // buildView() renders a pixel aspect outside [OSV_PIXEL_ASPECT_MIN,
    // OSV_PIXEL_ASPECT_MAX] as square pixels, so the Zoom read-out must be the
    // square project's - not numbers for a 20:1 shape the render never shows.
    REQUIRE(20.0 > static_cast<double>(OSV_PIXEL_ASPECT_MAX));
    ParFilter absurd(kDisplayW, 20.0);
    absurd.param(cam::kZoom).d = 100.0;
    REQUIRE(edit(absurd, cam::kZoom) == kOfxStatOK);
    INFO("pixel aspect 20: DJI FOV " << absurd.param(cam::kDjiFov).d << ", correction "
                                     << absurd.param(cam::kCorrection).d);
    CHECK(same(absurd.param(cam::kDjiFov).d, square.param(cam::kDjiFov).d));
    CHECK(same(absurd.param(cam::kCorrection).d, square.param(cam::kCorrection).d));
    CHECK(same(absurd.param(cam::kZoom).d, square.param(cam::kZoom).d));
    // And not what the unhonoured 20:1 widening would have given (the check
    // tells the two apart).
    const rf::DjiLens wideShape =
        rf::djiZoomTo(100.0, rf::DjiLens{OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_CORRECTION_DEFAULT},
                      20.0 * static_cast<double>(kDisplayW) / static_cast<double>(kDisplayH));
    CHECK_FALSE(same(absurd.param(cam::kDjiFov).d, wideShape.fovDeg));

    // ---- a preset writes the same DJI look --------------------------------------
    ParFilter squarePreset(kDisplayW, 1.0);
    ParFilter hdvPreset(1440, 4.0 / 3.0);
    squarePreset.param(cam::kPreset).i = static_cast<int>(rf::Preset::Asteroid) - 1;
    hdvPreset.param(cam::kPreset).i = static_cast<int>(rf::Preset::Asteroid) - 1;
    REQUIRE(edit(squarePreset, cam::kPreset) == kOfxStatOK);
    REQUIRE(edit(hdvPreset, cam::kPreset) == kOfxStatOK);
    CHECK(same(hdvPreset.param(cam::kDjiFov).d, squarePreset.param(cam::kDjiFov).d));
    CHECK(same(hdvPreset.param(cam::kZoom).d, squarePreset.param(cam::kZoom).d));
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: square pixels build exactly the camera they always did", "[ofx][.vegas][par]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;
    // ---- the camera block: an explicit 1.0, and every unusable aspect, build
    // the block the default builds, byte for byte -------------------------------------
    for (const LensCase& lens : kLensCases) {
        INFO(lens.name << " lens");
        const rf::Settings s = lensSettings(lens);
        for (const rf::SizePx project : {rf::SizePx{1920, 1080}, rf::SizePx{1080, 1920}, rf::SizePx{}}) {
            const rf::ViewSetup reference = rf::buildView(s, 1920, 1080, project);
            REQUIRE(reference.valid);
            CHECK(reference.params.pixelAspect == 1.0f);
            const double unusable[] = {1.0, 0.0, -1.0, 1000.0, std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::infinity()};
            for (const double pa : unusable) {
                INFO("pixel aspect " << pa);
                const rf::ViewSetup v = rf::buildView(s, 1920, 1080, project, pa);
                REQUIRE(v.valid);
                CHECK(std::memcmp(&v.params, &reference.params, sizeof(OsvReframeParams)) == 0);
            }
        }
    }

    // ---- the module: a square project renders the Premiere effect's pixels ----------
    ParFilter square(640, 1.0);
    square.param(cam::kPan).d = -35.0;
    REQUIRE(square.render() == kOfxStatOK);
    rf::Settings s = defaultSettings();
    s.panDeg = -35.0;
    const HostImage ref = referenceRender(s, square.source, square.frame, {640, kDisplayH});
    CHECK(maxDifference(square.output, ref, square.frame) == 0.0);
    CHECK(MockHost::instance().imagesOut == 0);
}

// ===========================================================================
//  Stabilisation through the VEGAS profile (the sample clip)
// ===========================================================================
// Smooth + Horizon Lock - the default, DJI's RockSteady with Horizon Leveling
// - must reach the reframed view exactly as the importer engine applies it to
// its sphere: the generator's view is Open 360 Reframe's CPU framing of the
// generator's own native sphere, pixel for pixel, at Good quality (the .OSV)
// and at Preview quality with Playback Proxy on (the .LRF proxy, which
// smooths over the .OSV's seconds).  NVDEC / GPU: not part of the CPU-only
// runs.
//
// "Native" is a different size for each engine, and the reference sphere
// must be rendered at exactly the size the view frames:
//
//   * Good stitches the .OSV: the 6K sample's native sphere is 6000 x 3000
//     (as in test_ofx_source.cpp's view-vs-sphere case).
//   * Preview with Playback Proxy plays the .LRF proxy, whose native sphere
//     is its ORIGINAL's divided by the whole number that brings it nearest
//     the proxy's own 2048 x 1024 detail (ImporterInstance::
//     geometryForLocked, the [PROXY] divisor rule): 6000 x 3000 / 3 =
//     2000 x 1000, the size the importer advertises for this proxy
//     (test_lrf_coverage.cpp).  The generator's Equirect output stitches at
//     the frame's size, so the proxy half compares against the proxy's own
//     2000 x 1000 native sphere - a 6000 x 3000 reference would be a
//     different stitch of the same moment and could never match.

namespace {

/// One quality the parity case runs at, and the native sphere the
/// generator's reframed view frames at that quality.
struct NativeSphereCase {
    const char* quality;  ///< VEGAS's OfxImageEffectPropRenderQuality value.
    bool fromProxy;       ///< True when the frame is served by the .LRF proxy.
    int width;            ///< Native sphere width, pixels.
    int height;           ///< Native sphere height, pixels.
};

/// The camera's .LRF beside `clip` (either spelling), or empty when there is
/// none - the proxy half of the parity case then has nothing to play.
std::filesystem::path proxyBeside(const std::string& clip) {
    if (clip.empty()) {
        return {};
    }
    std::filesystem::path lrf = clip;
    std::error_code ec;
    for (const char* extension : {".LRF", ".lrf"}) {
        lrf.replace_extension(extension);
        if (std::filesystem::is_regular_file(lrf, ec)) {
            return lrf;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("VEGAS: Smooth + Horizon Lock frames the view out of the engine's own stabilised sphere",
          "[ofx][.vegas][sample]") {
    REQUIRE_VEGAS_PROFILE();
    const CpuPathOnly cpuPath;  // the engine's sphere and the CPU framing of it
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // The 0-based popup index of "Smooth + Horizon Lock" (OSV_SS_STAB_ITEMS).
    constexpr int kSmoothHorizonLock = 4;
    // Good: the .OSV at its own native size.  Preview: the .LRF proxy at its
    // original's native size divided per the [PROXY] rule (see above).
    const NativeSphereCase cases[] = {
        {ofx::kVegasQualityGood, false, 6000, 3000},
        {ofx::kVegasQualityPreview, true, 2000, 1000},
    };
    // Without the camera's .LRF beside the sample, Preview would stitch the
    // .OSV and the proxy half would test nothing it claims to: it is left
    // out (and said so), the .OSV half still runs.
    const bool haveProxy = !proxyBeside(clip).empty();
    if (!haveProxy) {
        WARN("no .LRF beside the sample clip: the proxy half of the parity case is not run");
    }
    for (const NativeSphereCase& c : cases) {
        if (c.fromProxy && !haveProxy) {
            continue;
        }
        INFO("quality " << c.quality << ", native sphere " << c.width << "x" << c.height
                        << (c.fromProxy ? " (the .LRF proxy)" : " (the .OSV)"));
        const char* const quality = c.quality;
        // ---- the engine's native sphere, stabilised -------------------------------
        VegasSourceRig sphere({ofx::HostDepth::Float, ofx::HostOrder::Rgba}, 29.97, c.width, c.height);
        sphere.param(src::kFile).s = clip;
        sphere.param(src::kOutput).i = src::kOutputEquirect;
        sphere.param(sp::kStabilization).i = kSmoothHorizonLock;
        sphere.param(sp::kPlaybackProxy).i = 1;
        sphere.param(sp::kOutputLevels).i = static_cast<int>(ofx::OutputLevels::Full);
        REQUIRE(sphere.render(6.0, kOfxImageFieldNone, quality) == kOfxStatOK);

        // ---- the reframed view of the same moment ------------------------------
        VegasSourceRig view({ofx::HostDepth::Float, ofx::HostOrder::Rgba}, 29.97, 640, 360);
        view.param(src::kFile).s = clip;
        view.param(sp::kStabilization).i = kSmoothHorizonLock;
        view.param(sp::kPlaybackProxy).i = 1;
        view.param(sp::kOutputLevels).i = static_cast<int>(ofx::OutputLevels::Full);
        view.param(cam::kPan).d = 40.0;
        view.param(cam::kTilt).d = -10.0;
        view.param(cam::kDjiFov).d = 80.0;
        REQUIRE(view.render(6.0, kOfxImageFieldNone, quality) == kOfxStatOK);

        // ---- Premiere's two-step path on the generator's own sphere --------------
        rf::Settings s = defaultSettings();
        s.panDeg = 40.0;
        s.tiltDeg = -10.0;
        s.djiFovDeg = 80.0;
        const HostImage ref = referenceRender(s, sphere.output, view.frame, {640, 360});
        CHECK(maxDifference(view.output, ref, view.frame) == 0.0);
    }
    CHECK(MockHost::instance().imagesOut == 0);
}
