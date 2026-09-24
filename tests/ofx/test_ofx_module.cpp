// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_module.cpp - what an OpenFX host sees when it loads OpenOSV.ofx:
// the exports, the two plug-ins, their descriptors and their parameters.

#include "OfxTestSupport.h"

#include "OfxCamera.h"
#include "OfxSource.h"
#include "OfxSourceParams.h"

#include "ReframeParams.h"
#include "SourceSettingsParams.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace osv::ofxtest;

namespace {

/// The names of a parameter set, in definition order.
std::vector<std::string> names(const ParamSet& set) {
    std::vector<std::string> out;
    for (const auto& p : set.params) {
        out.push_back(p->name);
    }
    return out;
}

/// `pipe` split at '|' (the shared popup strings).
std::vector<std::string> items(const std::string& pipe) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = pipe.find('|', start);
        out.push_back(pipe.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) {
            return out;
        }
        start = bar + 1;
    }
}

}  // namespace

TEST_CASE("the module exports the OpenFX entry points and two plug-ins", "[ofx][module]") {
    Fixture& f = Fixture::get();
    REQUIRE(f.module.ok());
    CHECK(f.module.hasSetHostExport());
    REQUIRE(f.module.pluginCount() == 2);

    OfxPlugin* reframe = f.module.plugin(0);
    OfxPlugin* source = f.module.plugin(1);
    REQUIRE(reframe);
    REQUIRE(source);
    CHECK(std::string(reframe->pluginApi) == kOfxImageEffectPluginApi);
    CHECK(reframe->apiVersion == 1);
    // The identifiers are stored in every Resolve project: pinned here so a
    // rename cannot slip through.
    CHECK(std::string(reframe->pluginIdentifier) == "org.openosv.Open360Reframe");
    CHECK(std::string(source->pluginIdentifier) == "org.openosv.OSVSource");
    CHECK(reframe->pluginVersionMajor == 1);
    CHECK(source->pluginVersionMajor == 1);
    CHECK(f.module.plugin(2) == nullptr);
    CHECK(f.module.plugin(-1) == nullptr);
}

TEST_CASE("both plug-ins load and describe themselves", "[ofx][module]") {
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);

    const PropertySet& r = f.reframe.descriptor.props;
    CHECK(r.getString(kOfxPropLabel) == "OpenOSV 360 Reframe");
    CHECK(r.getString(kOfxImageEffectPluginPropGrouping) == "OpenOSV");
    CHECK(r.getStrings(kOfxImageEffectPropSupportedContexts) ==
          std::vector<std::string>{kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
    CHECK(r.getStrings(kOfxImageEffectPropSupportedPixelDepths) == std::vector<std::string>{kOfxBitDepthFloat});
    CHECK(r.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderFullySafe);
    CHECK(r.getInt(kOfxImageEffectPropSupportsTiles) == 0);
#if defined(OSV_OFX_TEST_HAVE_CUDA)
    // OpenFX 1.5 CUDA render, exactly the strings DaVinci Resolve reads.
    CHECK(r.getString(kOfxImageEffectPropCudaRenderSupported) == "true");
    CHECK(r.getString(kOfxImageEffectPropCudaStreamSupported) == "true");
#endif

    const PropertySet& s = f.source.descriptor.props;
    CHECK(s.getString(kOfxPropLabel) == "OpenOSV Source");
    CHECK(s.getString(kOfxImageEffectPluginPropGrouping) == "OpenOSV");
    CHECK(s.getStrings(kOfxImageEffectPropSupportedContexts) ==
          std::vector<std::string>{kOfxImageEffectContextGenerator});
    CHECK(s.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderInstanceSafe);
    // The generator renders to host memory: no GPU render is claimed.
    CHECK_FALSE(s.has(kOfxImageEffectPropCudaRenderSupported));
}

TEST_CASE("the reframe filter's context: clips and the camera controls", "[ofx][module]") {
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);
    OfxStatus st = kOfxStatFailed;
    auto ctx = f.reframe.describeInContext(kOfxImageEffectContextFilter, &st);
    REQUIRE(st == kOfxStatOK);

    REQUIRE(ctx->clipOrder == std::vector<std::string>{kOfxImageEffectSimpleSourceClipName,
                                                       kOfxImageEffectOutputClipName});
    CHECK(ctx->clips[kOfxImageEffectSimpleSourceClipName]->props.getString(kOfxImageEffectPropSupportedComponents) ==
          kOfxImageComponentRGBA);

    // Exactly the camera controls, in their permanent order.
    std::vector<std::string> expected(std::begin(osv::ofx::camera::kAllParams), std::end(osv::ofx::camera::kAllParams));
    CHECK(names(ctx->params) == expected);

    // Popups: the shared item lists (Output Resolution's first entry renamed
    // for an OpenFX host) and the shared defaults, 0-based.
    const Param* resolution = ctx->params.find(osv::ofx::camera::kOutputResolution);
    REQUIRE(resolution);
    std::vector<std::string> res = items(OSV_REFRAME_RESOLUTION_ITEMS);
    res[0] = "Match Timeline";
    CHECK(resolution->props.getStrings(kOfxParamPropChoiceOption) == res);
    CHECK(resolution->props.getInt(kOfxParamPropDefault) == OSV_REFRAME_RESOLUTION_DEFAULT - 1);

    const Param* preset = ctx->params.find(osv::ofx::camera::kPreset);
    REQUIRE(preset);
    CHECK(preset->props.getStrings(kOfxParamPropChoiceOption) == items(OSV_REFRAME_PRESET_ITEMS));
    CHECK(preset->props.getInt(kOfxParamPropDefault) == OSV_REFRAME_PRESET_DEFAULT - 1);

    const Param* lens = ctx->params.find(osv::ofx::camera::kLens);
    REQUIRE(lens);
    CHECK(lens->props.getStrings(kOfxParamPropChoiceOption) == items(OSV_REFRAME_LENS_ITEMS));
    CHECK(lens->props.getInt(kOfxParamPropDefault) == OSV_REFRAME_LENS_DEFAULT - 1);

    const Param* easing = ctx->params.find(osv::ofx::camera::kKeyframeEasing);
    REQUIRE(easing);
    CHECK(easing->props.getStrings(kOfxParamPropChoiceOption) == items(OSV_REFRAME_EASING_ITEMS));
    CHECK(easing->props.getInt(kOfxParamPropAnimates) == 0);

    // Every double carries BOTH a hard range and a display range: DaVinci
    // Resolve clamps a double that lacks either to (-1, 1) or (0, 0).
    for (const auto& p : ctx->params.params) {
        if (p->type != kOfxParamTypeDouble) {
            continue;
        }
        INFO(p->name);
        CHECK(p->props.dimension(kOfxParamPropMin) == 1);
        CHECK(p->props.dimension(kOfxParamPropMax) == 1);
        CHECK(p->props.dimension(kOfxParamPropDisplayMin) == 1);
        CHECK(p->props.dimension(kOfxParamPropDisplayMax) == 1);
        CHECK(p->props.getDouble(kOfxParamPropMin) <= p->props.getDouble(kOfxParamPropDefault));
        CHECK(p->props.getDouble(kOfxParamPropDefault) <= p->props.getDouble(kOfxParamPropMax));
    }

    // The shared defaults, reachable by value.
    const Param* djiFov = ctx->params.find(osv::ofx::camera::kDjiFov);
    REQUIRE(djiFov);
    CHECK(djiFov->props.getDouble(kOfxParamPropDefault) == OSV_REFRAME_DJI_FOV_DEFAULT);
    CHECK(djiFov->props.getDouble(kOfxParamPropMax) == OSV_REFRAME_DJI_FOV_VALID_MAX);
    const Param* pan = ctx->params.find(osv::ofx::camera::kPan);
    REQUIRE(pan);
    CHECK(pan->props.getString(kOfxParamPropDoubleType) == kOfxParamDoubleTypeAngle);
    CHECK(pan->props.getInt(kOfxParamPropAnimates) == 1);
    // The Lens mirror is created hidden.
    const Param* mirror = ctx->params.find(osv::ofx::camera::kLensMirror);
    REQUIRE(mirror);
    CHECK(mirror->props.getInt(kOfxParamPropSecret) == 1);
}

TEST_CASE("the generator's context: output clip, clip controls, camera, stitch", "[ofx][module]") {
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);
    OfxStatus st = kOfxStatFailed;
    auto ctx = f.source.describeInContext(kOfxImageEffectContextGenerator, &st);
    REQUIRE(st == kOfxStatOK);
    CHECK(ctx->clipOrder == std::vector<std::string>{kOfxImageEffectOutputClipName});

    std::vector<std::string> expected = {osv::ofx::source::kFile, osv::ofx::source::kChooseFile,
                                         osv::ofx::source::kClipInfo, osv::ofx::source::kOutput,
                                         osv::ofx::source::kStartFrame};
    expected.insert(expected.end(), std::begin(osv::ofx::camera::kAllParams), std::end(osv::ofx::camera::kAllParams));
    expected.insert(expected.end(), std::begin(osv::ofx::source_params::kAllParams),
                    std::end(osv::ofx::source_params::kAllParams));
    CHECK(names(ctx->params) == expected);

    const Param* file = ctx->params.find(osv::ofx::source::kFile);
    REQUIRE(file);
    CHECK(file->type == kOfxParamTypeString);
    CHECK(file->props.getString(kOfxParamPropStringMode) == kOfxParamStringIsFilePath);
    const Param* button = ctx->params.find(osv::ofx::source::kChooseFile);
    REQUIRE(button);
    CHECK(button->type == kOfxParamTypePushButton);
    const Param* info = ctx->params.find(osv::ofx::source::kClipInfo);
    REQUIRE(info);
    CHECK(info->props.getString(kOfxParamPropStringMode) == kOfxParamStringIsLabel);
    const Param* output = ctx->params.find(osv::ofx::source::kOutput);
    REQUIRE(output);
    CHECK(output->props.getStrings(kOfxParamPropChoiceOption) ==
          std::vector<std::string>{"Reframed view", "360 equirect"});

    // Colour Output: the shared list, with Rec. 709 as the OpenFX default.
    const Param* colour = ctx->params.find(osv::ofx::source_params::kColorOutput);
    REQUIRE(colour);
    CHECK(colour->props.getStrings(kOfxParamPropChoiceOption) == items(OSV_SS_COLOR_ITEMS));
    CHECK(colour->props.getInt(kOfxParamPropDefault) == 2);
    CHECK(items(OSV_SS_COLOR_ITEMS)[2] == "Rec. 709");

    // Every stitch setting is static per clip: no keyframes.
    for (const char* name : osv::ofx::source_params::kAllParams) {
        const Param* p = ctx->params.find(name);
        REQUIRE(p);
        if (p->type == kOfxParamTypeGroup) {
            continue;
        }
        INFO(name);
        CHECK(p->props.getInt(kOfxParamPropAnimates) == 0);
    }
}
