// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_cuda.cpp - Open 360 Reframe on the host's CUDA images, the way
// DaVinci Resolve hands them over on an NVIDIA machine (OpenFX 1.5 CUDA
// render): device pointers in kOfxImagePropData, the host's context current,
// the host's stream in kOfxImageEffectPropCudaStream.
//
// Every test needs a device; on a machine without one they SKIP.

#include "OfxTestSupport.h"

#include "OfxCamera.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <cstring>
#include <string>

using namespace osv::ofxtest;
namespace cam = osv::ofx::camera;

namespace {

constexpr int kW = 480;
constexpr int kH = 270;

/// A private CUDA context, standing in for the host's.
struct CudaHost {
    CUcontext context = nullptr;
    std::string failure;

    CudaHost() {
        if (!cudaDriverLoadable()) {
            failure = "no NVIDIA driver (nvcuda.dll)";
            return;
        }
        if (cuInit(0) != CUDA_SUCCESS) {
            failure = "cuInit failed";
            return;
        }
        int count = 0;
        if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) {
            failure = "no CUDA device";
            return;
        }
        CUdevice device = 0;
        if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
            failure = "cuDeviceGet failed";
            return;
        }
        // cuCtxCreate makes the new context current on this thread - the
        // state a host's render thread is in.
        if (cuCtxCreate(&context, 0, device) != CUDA_SUCCESS) {
            failure = "cuCtxCreate failed";
            context = nullptr;
        }
    }
    ~CudaHost() {
        if (context) {
            cuCtxDestroy(context);
        }
    }
};

/// A device copy of a host image with the SAME layout (bottom-up rows,
/// the same pitch), as a GPU host allocates its images.
struct DeviceImage {
    CUdeviceptr base = 0;
    std::size_t bytes = 0;
    HostImage layout;  ///< Bounds and pitch; storage used for up/download.

    explicit DeviceImage(const HostImage& image) : layout(image) {
        bytes = image.storage.size() * sizeof(float);
        REQUIRE(cuMemAlloc(&base, bytes) == CUDA_SUCCESS);
        REQUIRE(cuMemcpyHtoD(base, image.storage.data(), bytes) == CUDA_SUCCESS);
    }
    ~DeviceImage() {
        if (base) {
            cuMemFree(base);
        }
    }
    /// Device address of the bottom-left pixel.
    [[nodiscard]] void* data() const noexcept {
        return reinterpret_cast<void*>(base + layout.bottomOffsetFloats * sizeof(float));
    }
    void describe(PropertySet& props) {
        layout.describe(props);
        props.setPointer(kOfxImagePropData, data());
    }
    HostImage download() {
        HostImage out = layout;
        REQUIRE(cuMemcpyDtoH(out.storage.data(), base, bytes) == CUDA_SUCCESS);
        return out;
    }
};

/// The CPU render of the same instance settings, for comparison.
HostImage cpuRender(Effect& effect, HostImage& source) {
    HostImage out = makeImage(OfxRectI{0, 0, kW, kH});
    provideImage(*effect.clip(kOfxImageEffectSimpleSourceClipName), source);
    provideImage(*effect.clip(kOfxImageEffectOutputClipName), out);
    PluginHarness::RenderArgs args;
    args.window = OfxRectI{0, 0, kW, kH};
    REQUIRE(Fixture::get().reframe.render(effect, args) == kOfxStatOK);
    return out;
}

}  // namespace

TEST_CASE("the CUDA render on the host's stream matches the CPU render", "[ofx][reframe][cuda]") {
    REQUIRE(Fixture::get().ready);
    CudaHost cuda;
    if (!cuda.context) {
        SKIP(cuda.failure);
    }
    OfxStatus st = kOfxStatFailed;
    auto effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 25.0, &st);
    REQUIRE(st == kOfxStatOK);
    effect->params.find(cam::kPan)->d = 64.0;
    effect->params.find(cam::kTilt)->d = 18.0;
    effect->params.find(cam::kDjiFov)->d = 95.0;
    effect->clip(kOfxImageEffectSimpleSourceClipName)->rod = OfxRectD{0, 0, 512, 256};

    HostImage source = makeImage(OfxRectI{0, 0, 512, 256});
    paintPanorama(source);
    const HostImage cpu = cpuRender(*effect, source);

    HostImage blank = makeImage(OfxRectI{0, 0, kW, kH});
    blank.fill(-7.0f);
    DeviceImage dSource(source);
    DeviceImage dOutput(blank);
    effect->clip(kOfxImageEffectSimpleSourceClipName)->provide = [&](double, PropertySet& p) {
        dSource.describe(p);
        return true;
    };
    effect->clip(kOfxImageEffectOutputClipName)->provide = [&](double, PropertySet& p) {
        dOutput.describe(p);
        return true;
    };
    CUstream stream = nullptr;
    REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);

    PluginHarness::RenderArgs args;
    args.window = OfxRectI{0, 0, kW, kH};
    args.setCudaProps = true;
    args.cuda = true;
    args.stream = stream;
    REQUIRE(Fixture::get().reframe.render(*effect, args) == kOfxStatOK);
    // The plug-in must not have synchronised for us: the result is ours to
    // wait for, on our stream.
    REQUIRE(cuStreamSynchronize(stream) == CUDA_SUCCESS);
    const HostImage gpu = dOutput.download();
    cuStreamDestroy(stream);

    // The same shared function on both sides, compiled without fused
    // multiply-add; only the transcendental libraries differ (the Premiere
    // effect's GPU parity bar).
    const double db = psnr(gpu, cpu, OfxRectI{0, 0, kW, kH});
    INFO("CUDA vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);
    // The host's context is still the current one.
    CUcontext current = nullptr;
    REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current == cuda.context);
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("a host that recreates its CUDA context still gets a picture", "[ofx][reframe][cuda]") {
    REQUIRE(Fixture::get().ready);
    // Three generations of the host's context.  The driver is free to hand
    // a new context the address of the one just destroyed, and a kernel
    // cached by that address alone belongs to the dead context.
    for (int generation = 0; generation < 3; ++generation) {
        INFO("context generation " << generation);
        CudaHost cuda;
        if (!cuda.context) {
            SKIP(cuda.failure);
        }
        OfxStatus st = kOfxStatFailed;
        auto effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 25.0, &st);
        REQUIRE(st == kOfxStatOK);
        effect->params.find(cam::kPan)->d = 10.0 * generation;
        effect->clip(kOfxImageEffectSimpleSourceClipName)->rod = OfxRectD{0, 0, 512, 256};

        HostImage source = makeImage(OfxRectI{0, 0, 512, 256});
        paintPanorama(source);
        const HostImage cpu = cpuRender(*effect, source);

        HostImage blank = makeImage(OfxRectI{0, 0, kW, kH});
        DeviceImage dSource(source);
        DeviceImage dOutput(blank);
        effect->clip(kOfxImageEffectSimpleSourceClipName)->provide = [&](double, PropertySet& p) {
            dSource.describe(p);
            return true;
        };
        effect->clip(kOfxImageEffectOutputClipName)->provide = [&](double, PropertySet& p) {
            dOutput.describe(p);
            return true;
        };
        PluginHarness::RenderArgs args;
        args.window = OfxRectI{0, 0, kW, kH};
        args.setCudaProps = true;
        args.cuda = true;
        REQUIRE(Fixture::get().reframe.render(*effect, args) == kOfxStatOK);
        const HostImage gpu = dOutput.download();
        CHECK(psnr(gpu, cpu, OfxRectI{0, 0, kW, kH}) >= 60.0);
    }
}

TEST_CASE("the CUDA render finds the images' context when none is current, and waits without a stream",
          "[ofx][reframe][cuda]") {
    REQUIRE(Fixture::get().ready);
    CudaHost cuda;
    if (!cuda.context) {
        SKIP(cuda.failure);
    }
    OfxStatus st = kOfxStatFailed;
    auto effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 25.0, &st);
    REQUIRE(st == kOfxStatOK);
    effect->params.find(cam::kRoll)->d = -20.0;
    effect->clip(kOfxImageEffectSimpleSourceClipName)->rod = OfxRectD{0, 0, 512, 256};

    HostImage source = makeImage(OfxRectI{0, 0, 512, 256});
    paintPanorama(source);
    const HostImage cpu = cpuRender(*effect, source);

    HostImage blank = makeImage(OfxRectI{0, 0, kW, kH});
    DeviceImage dSource(source);
    DeviceImage dOutput(blank);
    effect->clip(kOfxImageEffectSimpleSourceClipName)->provide = [&](double, PropertySet& p) {
        dSource.describe(p);
        return true;
    };
    effect->clip(kOfxImageEffectOutputClipName)->provide = [&](double, PropertySet& p) {
        dOutput.describe(p);
        return true;
    };

    // No context current on this thread: the plug-in has to ask the output
    // pointer which context owns it.  The WHOLE stack is popped - an earlier
    // test in this process may have left another context under ours.
    CUcontext top = nullptr;
    while (cuCtxGetCurrent(&top) == CUDA_SUCCESS && top) {
        CUcontext popped = nullptr;
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    }

    PluginHarness::RenderArgs args;
    args.window = OfxRectI{0, 0, kW, kH};
    args.setCudaProps = true;
    args.cuda = true;
    args.stream = nullptr;  // no stream: the render must be finished on return
    const OfxStatus rendered = Fixture::get().reframe.render(*effect, args);

    // The thread is left exactly as it was: nothing current.
    CUcontext current = reinterpret_cast<CUcontext>(1);
    REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current == nullptr);

    REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
    REQUIRE(rendered == kOfxStatOK);
    const HostImage gpu = dOutput.download();
    const double db = psnr(gpu, cpu, OfxRectI{0, 0, kW, kH});
    INFO("CUDA vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);
}
