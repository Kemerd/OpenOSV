// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HostContext on macOS (plugins/common/HostContext.cpp): the stored "GPU"
// render-device choice means Metal there, and Auto picks Metal first.

#include <catch2/catch_test_macros.hpp>

#include "HostContext.h"

#include "osv/render/MetalRenderer.h"

#include <string>

using namespace osv::premiere;

TEST_CASE("the GPU render device means Metal on a Mac", "[macos][plugins][hostcontext]") {
    CHECK(std::string(HostContext::preferenceName(RenderDevicePreference::Cuda)) == "metal");
    CHECK(std::string(HostContext::preferenceName(RenderDevicePreference::OpenCl)) == "opencl");
    CHECK(std::string(HostContext::preferenceName(RenderDevicePreference::Cpu)) == "cpu");
    CHECK(std::string(HostContext::preferenceName(RenderDevicePreference::Auto)) == "auto");
}

TEST_CASE("Auto renders on Metal when the Mac has a Metal device", "[macos][plugins][hostcontext][metal]") {
    auto lease = HostContext::instance().acquireRenderer(RenderDevicePreference::Auto);
    REQUIRE(lease.ok());
    REQUIRE(lease.value().renderer);
    if (osv::render::MetalRenderer::available(nullptr)) {
        CHECK(lease.value().backend == "metal");
        CHECK(std::string(lease.value().renderer->name()) == "metal");
    } else {
        CHECK(lease.value().backend != "metal");
    }

    // The explicit "GPU" choice resolves to the same backend.
    if (osv::render::MetalRenderer::available(nullptr)) {
        auto gpu = HostContext::instance().acquireRenderer(RenderDevicePreference::Cuda);
        REQUIRE(gpu.ok());
        CHECK(gpu.value().backend == "metal");
    }

    // CUDA by name does not exist here and is refused, not crashed on.
    auto cuda = HostContext::instance().acquireRenderer(std::string_view("cuda"));
    CHECK_FALSE(cuda.ok());

    HostContext::shutdown();
}
