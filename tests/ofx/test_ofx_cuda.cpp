// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_cuda.cpp - OpenOSV 360 Reframe on the host's CUDA images, the way
// DaVinci Resolve hands them over on an NVIDIA machine (OpenFX 1.5 CUDA
// render): device pointers in kOfxImagePropData, the host's context current,
// the host's stream in kOfxImageEffectPropCudaStream.
//
// Every test needs a device; on a machine without one they SKIP.
//
// [WP-V-GPU] The second half drives the effects' OWN GPU path - the one for
// hosts that hand CPU images (VEGAS Pro) - through the same mock host:
// OPENOSV_OFX_GPU=1 switches it on for this host (OfxGpuView.h), =0 keeps
// the CPU path, and every render is held to the CPU path's render of the
// same instance.  tests/ofx_gpu drives the pipeline under it directly, with
// every pixel format and level; here it is the built module end to end.

#include "OfxTestSupport.h"

#include "OfxCamera.h"
#include "OfxHostImage.h"    // [WP-V-GPU] VEGAS's formats and levels
#include "OfxSource.h"       // [WP-V-GPU] the generator's parameter names
#include "OfxSourceParams.h" // [WP-V-GPU] Output Levels

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// ===========================================================================
//  [WP-V-GPU] The own GPU path, for hosts that hand CPU images
// ===========================================================================

namespace {

/// OPENOSV_OFX_GPU for one scope, restored afterwards.  The module shares
/// this CRT's environment (both /MD) and reads the switch on every render.
class GpuSwitch {
public:
    explicit GpuSwitch(const char* value) {
        char old[32] = {};
        std::size_t length = 0;
        if (getenv_s(&length, old, sizeof(old), "OPENOSV_OFX_GPU") == 0 && length > 0) {
            m_had = true;
            m_old = old;
        }
        ::_putenv_s("OPENOSV_OFX_GPU", value);
    }
    ~GpuSwitch() { ::_putenv_s("OPENOSV_OFX_GPU", m_had ? m_old.c_str() : ""); }
    GpuSwitch(const GpuSwitch&) = delete;
    GpuSwitch& operator=(const GpuSwitch&) = delete;

private:
    bool m_had = false;
    std::string m_old;
};

/// OPENOSV_OFX_DIRECT for one scope, restored afterwards: "0" makes the
/// generator's own-GPU view frame out of the stitched sphere, as before the
/// direct view existed - the path whose pixels match the CPU framing
/// exactly.  Read by the module on every render, like OPENOSV_OFX_GPU.
class DirectSwitch {
public:
    explicit DirectSwitch(const char* value) {
        char old[32] = {};
        std::size_t length = 0;
        if (getenv_s(&length, old, sizeof(old), "OPENOSV_OFX_DIRECT") == 0 && length > 0) {
            m_had = true;
            m_old = old;
        }
        ::_putenv_s("OPENOSV_OFX_DIRECT", value);
    }
    ~DirectSwitch() { ::_putenv_s("OPENOSV_OFX_DIRECT", m_had ? m_old.c_str() : ""); }
    DirectSwitch(const DirectSwitch&) = delete;
    DirectSwitch& operator=(const DirectSwitch&) = delete;

private:
    bool m_had = false;
    std::string m_old;
};

/// True when the driver loads and reports a device; the reason otherwise.
bool cudaDeviceUsable(std::string& why) {
    if (!cudaDriverLoadable()) {
        why = "no NVIDIA driver (nvcuda.dll)";
        return false;
    }
    if (cuInit(0) != CUDA_SUCCESS) {
        why = "cuInit failed";
        return false;
    }
    int count = 0;
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) {
        why = "no CUDA device";
        return false;
    }
    return true;
}

/// The context current on this thread (null when none).
CUcontext currentContext() {
    CUcontext c = nullptr;
    REQUIRE(cuCtxGetCurrent(&c) == CUDA_SUCCESS);
    return c;
}

/// Pop every context off this thread's stack.
void popAllContexts() {
    CUcontext top = nullptr;
    while (cuCtxGetCurrent(&top) == CUDA_SUCCESS && top) {
        CUcontext popped = nullptr;
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    }
}

/// Whether device 0's primary context is active.  The filter's CPU path
/// never touches CUDA, and the own GPU path retains that context for its
/// slots - so in a process of its own (ctest runs every test case in one)
/// the state tells which path ran.
bool primaryContextActive() {
    CUdevice device = 0;
    unsigned flags = 0;
    int active = 0;
    if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
        return false;
    }
    if (cuDevicePrimaryCtxGetState(device, &flags, &active) != CUDA_SUCCESS) {
        return false;
    }
    return active != 0;
}

/// True when no float of `image` outside `area` differs from `value`.
bool untouchedOutside(const HostImage& image, const OfxRectI& area, float value) {
    for (int y = image.bounds.y1; y < image.bounds.y2; ++y) {
        for (int x = image.bounds.x1; x < image.bounds.x2; ++x) {
            if (x >= area.x1 && x < area.x2 && y >= area.y1 && y < area.y2) {
                continue;
            }
            const float* p = image.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                if (p[c] != value) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// The intersection of two rectangles (empty when disjoint).
OfxRectI clipRect(const OfxRectI& a, const OfxRectI& b) {
    OfxRectI r{std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    r.x2 = std::max(r.x2, r.x1);
    r.y2 = std::max(r.y2, r.y1);
    return r;
}

/// The sample clip, or empty when this machine has none.
std::string sampleClipPath() {
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

/// One OpenOSV Source instance rendering the sample clip into a `w` x `h`
/// output of the given layout.
struct GeneratorRig {
    std::unique_ptr<Effect> effect;
    HostImage output;
    OfxRectI frame;

    GeneratorRig(int w, int h, bool negativePitch = false, int padFloats = 0, double fps = 29.97)
        : frame{0, 0, w, h} {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().source.createInstance(kOfxImageEffectContextGenerator, w, h, fps, &st);
        REQUIRE(st == kOfxStatOK);
        output = makeImage(frame, negativePitch, padFloats);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(out);
        provideImage(*out, output);
    }
    ~GeneratorRig() {
        if (effect) {
            (void)Fixture::get().source.destroyInstance(*effect);
        }
    }
    GeneratorRig(const GeneratorRig&) = delete;
    GeneratorRig& operator=(const GeneratorRig&) = delete;

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    /// Render `time` over `window` (the whole frame when empty), the output
    /// pre-filled with -7 so an unwritten pixel shows.
    OfxStatus render(double time, bool draft, OfxRectI window = OfxRectI{0, 0, 0, 0}, bool interactive = false) {
        output.fill(-7.0f);
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = (window.x2 > window.x1 && window.y2 > window.y1) ? window : frame;
        args.draft = draft;
        args.interactive = interactive;
        return Fixture::get().source.render(*effect, args);
    }
};

/// Point a rig at the sample clip with a moved camera.
void aimAtSample(GeneratorRig& rig, const std::string& clip, int output) {
    rig.param(osv::ofx::source::kFile).s = clip;
    rig.param(osv::ofx::source::kOutput).i = output;
    rig.param(cam::kPan).d = 40.0;
    rig.param(cam::kTilt).d = -10.0;
    rig.param(cam::kDjiFov).d = 80.0;
}

}  // namespace

TEST_CASE("the own-GPU filter path on CPU images matches the CPU path", "[ofx][reframe][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    OfxStatus st = kOfxStatFailed;
    auto effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 25.0, &st);
    REQUIRE(st == kOfxStatOK);
    effect->params.find(cam::kPan)->d = 64.0;
    effect->params.find(cam::kTilt)->d = 18.0;
    effect->params.find(cam::kDjiFov)->d = 95.0;
    effect->clip(kOfxImageEffectSimpleSourceClipName)->rod = OfxRectD{0, 0, 512, 256};

    // Before any render: which paths touch the device.
    popAllContexts();
    const bool primaryWasActive = primaryContextActive();
    bool gpuRan = false;

    struct Layout {
        const char* name;
        bool negative;
        int padFloats;
    };
    const Layout layouts[] = {
        {"tight", false, 0}, {"padded", false, 3}, {"negative", true, 0}, {"negative padded", true, 2}};
    const OfxRectI bounds{0, 0, kW, kH};
    const OfxRectI windows[] = {bounds, OfxRectI{37, 21, kW - 53, kH - 17}, OfxRectI{-9, -5, kW + 11, kH + 7}};

    for (const Layout& srcLayout : layouts) {
        for (const Layout& outLayout : layouts) {
            for (const OfxRectI& window : windows) {
                INFO("source " << srcLayout.name << ", output " << outLayout.name << ", window " << window.x1 << ","
                               << window.y1 << " - " << window.x2 << "," << window.y2);
                HostImage source = makeImage(OfxRectI{0, 0, 512, 256}, srcLayout.negative, srcLayout.padFloats);
                paintPanorama(source);
                provideImage(*effect->clip(kOfxImageEffectSimpleSourceClipName), source);
                PluginHarness::RenderArgs args;
                args.window = window;

                // The CPU path, as every host but VEGAS gets by default.
                HostImage cpu = makeImage(bounds, outLayout.negative, outLayout.padFloats);
                cpu.fill(-7.0f);
                {
                    GpuSwitch off("0");
                    provideImage(*effect->clip(kOfxImageEffectOutputClipName), cpu);
                    REQUIRE(Fixture::get().reframe.render(*effect, args) == kOfxStatOK);
                }
                if (!primaryWasActive && !gpuRan) {
                    // The CPU path never touched the device.
                    CHECK_FALSE(primaryContextActive());
                }

                // The own GPU path, switched on for this (non-VEGAS) host.
                HostImage gpu = makeImage(bounds, outLayout.negative, outLayout.padFloats);
                gpu.fill(-7.0f);
                {
                    GpuSwitch on("1");
                    provideImage(*effect->clip(kOfxImageEffectOutputClipName), gpu);
                    REQUIRE(Fixture::get().reframe.render(*effect, args) == kOfxStatOK);
                    gpuRan = true;
                }
                // It ran on the GPU (its slots hold the primary context), and
                // left this thread with nothing current, as it found it.
                CHECK(primaryContextActive());
                CHECK(currentContext() == nullptr);

                const OfxRectI area = clipRect(window, bounds);
                const double db = psnr(gpu, cpu, area);
                INFO("own GPU vs CPU PSNR: " << db << " dB");
                CHECK(db >= 60.0);
                CHECK(untouchedOutside(gpu, area, -7.0f));
                CHECK(untouchedOutside(cpu, area, -7.0f));
            }
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("the own-GPU filter path leaves a host's CUDA context current", "[ofx][reframe][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    CudaHost cuda;
    if (!cuda.context) {
        SKIP(cuda.failure);
    }
    OfxStatus st = kOfxStatFailed;
    auto effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 25.0, &st);
    REQUIRE(st == kOfxStatOK);
    effect->params.find(cam::kRoll)->d = 12.0;
    effect->clip(kOfxImageEffectSimpleSourceClipName)->rod = OfxRectD{0, 0, 512, 256};
    HostImage source = makeImage(OfxRectI{0, 0, 512, 256});
    paintPanorama(source);
    const HostImage cpu = cpuRender(*effect, source);

    HostImage gpu = makeImage(OfxRectI{0, 0, kW, kH});
    gpu.fill(-7.0f);
    provideImage(*effect->clip(kOfxImageEffectSimpleSourceClipName), source);
    provideImage(*effect->clip(kOfxImageEffectOutputClipName), gpu);
    {
        // A CPU-image render (no CUDA inArgs) with the host's context current.
        GpuSwitch on("1");
        PluginHarness::RenderArgs args;
        args.window = OfxRectI{0, 0, kW, kH};
        REQUIRE(Fixture::get().reframe.render(*effect, args) == kOfxStatOK);
    }
    CHECK(currentContext() == cuda.context);
    CHECK(psnr(gpu, cpu, OfxRectI{0, 0, kW, kH}) >= 60.0);
}

TEST_CASE("the own-GPU generator view matches the CPU framing of the clip", "[ofx][source][sample][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    // Draft: no seam search, parallax or measured corrections on either path,
    // so the two stitches are the same pixels (the analyses shade their bands
    // from device frames on the GPU path and from host frames on the CPU one,
    // two shaders 108-111 dB apart - see test_importer_bitdepth.cpp).
    //
    // The GPU view framed out of the sphere: the path that resamples exactly
    // as the CPU framing does.  The direct view (one resampling, sharper) has
    // its own test below.
    const DirectSwitch spherePath("0");
    struct Case {
        const char* name;
        bool negative;
        int padFloats;
        OfxRectI window;
    };
    const Case cases[] = {
        {"whole frame", false, 0, OfxRectI{0, 0, 640, 360}},
        {"inner window, padded rows", false, 5, OfxRectI{50, 40, 600, 300}},
        {"window wider than the frame, negative pitch", true, 0, OfxRectI{-16, -8, 656, 372}},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        GeneratorRig cpu(640, 360, c.negative, c.padFloats);
        aimAtSample(cpu, clip, osv::ofx::source::kOutputReframed);
        {
            GpuSwitch off("0");
            REQUIRE(cpu.render(3.0, /*draft=*/true, c.window) == kOfxStatOK);
        }
        GeneratorRig gpu(640, 360, c.negative, c.padFloats);
        aimAtSample(gpu, clip, osv::ofx::source::kOutputReframed);
        popAllContexts();
        {
            GpuSwitch on("1");
            REQUIRE(gpu.render(3.0, /*draft=*/true, c.window) == kOfxStatOK);
        }
        CHECK(currentContext() == nullptr);
        const OfxRectI area = clipRect(c.window, gpu.frame);
        const double db = psnr(gpu.output, cpu.output, area);
        INFO("own GPU vs CPU PSNR: " << db << " dB");
        CHECK(db >= 60.0);
        CHECK(untouchedOutside(gpu.output, area, -7.0f));
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

namespace {

/// Rec. 709 luma of a float render, with its coverage (alpha > 0.99).
struct LumaPlane {
    int w = 0;
    int h = 0;
    std::vector<double> value;
    std::vector<unsigned char> covered;

    [[nodiscard]] double at(int x, int y) const { return value[static_cast<std::size_t>(y) * w + x]; }
    [[nodiscard]] bool in(int x, int y) const { return covered[static_cast<std::size_t>(y) * w + x] != 0; }
};

/// The luma of `image` over `frame`, top row first.
LumaPlane lumaOf(const HostImage& image, const OfxRectI& frame) {
    LumaPlane plane;
    plane.w = frame.x2 - frame.x1;
    plane.h = frame.y2 - frame.y1;
    plane.value.assign(static_cast<std::size_t>(plane.w) * plane.h, 0.0);
    plane.covered.assign(static_cast<std::size_t>(plane.w) * plane.h, 0);
    for (int row = 0; row < plane.h; ++row) {
        // OpenFX rows count up; the plane's count down.
        const int y = frame.y2 - 1 - row;
        for (int col = 0; col < plane.w; ++col) {
            const float* p = image.pixel(frame.x1 + col, y);
            if (!p) {
                continue;
            }
            const std::size_t i = static_cast<std::size_t>(row) * plane.w + col;
            plane.value[i] = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            plane.covered[i] = p[3] > 0.99f ? 1 : 0;
        }
    }
    return plane;
}

/// Normalised cross-correlation of `a` shifted by (dx, dy) against `b`, over
/// the pixels both cover, `margin` pixels in from every edge.
double nccShifted(const LumaPlane& a, const LumaPlane& b, int dx, int dy, int margin) {
    double sa = 0.0;
    double sb = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    double sab = 0.0;
    double n = 0.0;
    for (int y = margin; y < a.h - margin; ++y) {
        for (int x = margin; x < a.w - margin; ++x) {
            const int ax = x + dx;
            const int ay = y + dy;
            if (!a.in(ax, ay) || !b.in(x, y)) {
                continue;
            }
            const double va = a.at(ax, ay);
            const double vb = b.at(x, y);
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
            n += 1.0;
        }
    }
    if (n < 2.0) {
        return 0.0;
    }
    const double cov = sab - sa * sb / n;
    const double va = saa - sa * sa / n;
    const double vb = sbb - sb * sb / n;
    return (va > 0.0 && vb > 0.0) ? cov / std::sqrt(va * vb) : 0.0;
}

/// Mean absolute horizontal + vertical luma gradient over covered pixels:
/// the sharper of two renders of the same view has the larger.
double meanGradient(const LumaPlane& p) {
    double sum = 0.0;
    double n = 0.0;
    for (int y = 1; y < p.h; ++y) {
        for (int x = 1; x < p.w; ++x) {
            if (!p.in(x, y) || !p.in(x - 1, y) || !p.in(x, y - 1)) {
                continue;
            }
            sum += std::fabs(p.at(x, y) - p.at(x - 1, y)) + std::fabs(p.at(x, y) - p.at(x, y - 1));
            n += 1.0;
        }
    }
    return n > 0.0 ? sum / n : 0.0;
}

}  // namespace

TEST_CASE("the own-GPU generator's direct view frames like the CPU framing, and is no softer",
          "[ofx][source][sample][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    // 1280 x 720 at 80 degrees samples about as densely as the fisheyes do,
    // so neither path decimates: the comparison measures framing, not two
    // different aliasing patterns.  Draft on both sides, so the stitch
    // (blend, colour, stabilisation) is the same; what differs is only that
    // the direct view resamples the fisheyes once, the CPU framing twice.
    constexpr int kViewW = 1280;
    constexpr int kViewH = 720;
    GeneratorRig cpu(kViewW, kViewH);
    aimAtSample(cpu, clip, osv::ofx::source::kOutputReframed);
    {
        GpuSwitch off("0");
        REQUIRE(cpu.render(3.0, /*draft=*/true) == kOfxStatOK);
    }
    GeneratorRig gpu(kViewW, kViewH);
    aimAtSample(gpu, clip, osv::ofx::source::kOutputReframed);
    {
        GpuSwitch on("1");
        DirectSwitch direct("1");
        REQUIRE(gpu.render(3.0, /*draft=*/true) == kOfxStatOK);
    }
    // Every pixel of the frame written.
    for (int y = 0; y < kViewH; ++y) {
        for (int x = 0; x < kViewW; ++x) {
            REQUIRE(gpu.output.pixel(x, y)[3] != -7.0f);
        }
    }

    const LumaPlane direct = lumaOf(gpu.output, gpu.frame);
    const LumaPlane twoStep = lumaOf(cpu.output, cpu.frame);

    // ---- framing: the correlation peaks at zero offset, and is high there -----
    // A framing error of half a pixel or more would move the peak to a
    // neighbour; the composition is proved within 0.0016 px on the Premiere
    // side (test_direct.cpp), and this checks the OpenFX plumbing keeps it.
    constexpr int kMargin = 8;
    const double centre = nccShifted(direct, twoStep, 0, 0, kMargin);
    INFO("direct vs CPU framing NCC at zero offset: " << centre);
    CHECK(centre >= 0.99);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            const double shifted = nccShifted(direct, twoStep, dx, dy, kMargin);
            INFO("offset (" << dx << ", " << dy << "): " << shifted);
            CHECK(centre >= shifted);
        }
    }

    // ---- coverage: the same pixels are picture ----------------------------------
    std::size_t disagree = 0;
    for (std::size_t i = 0; i < direct.covered.size(); ++i) {
        disagree += direct.covered[i] != twoStep.covered[i] ? 1u : 0u;
    }
    CHECK(disagree <= direct.covered.size() / 100u);

    // ---- sharpness: one resampling instead of two ---------------------------------
    const double gDirect = meanGradient(direct);
    const double gTwoStep = meanGradient(twoStep);
    INFO("mean gradient: direct " << gDirect << ", CPU framing " << gTwoStep);
    CHECK(gDirect >= 0.98 * gTwoStep);
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("the own-GPU generator renders a full-quality view with its analyses on",
          "[ofx][source][sample][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    // Not a parity test: with the analyses on, the two paths measure from
    // differently shaded bands and may choose a different seam.  What must
    // hold is a whole, plausible picture, frame after frame, in both purposes.
    GeneratorRig rig(960, 540);
    aimAtSample(rig, clip, osv::ofx::source::kOutputReframed);
    GpuSwitch on("1");
    for (const bool interactive : {false, true}) {
        for (double t = 0.0; t < 20.0; t += 4.0) {
            INFO("time " << t << (interactive ? " (interactive)" : " (exact)"));
            REQUIRE(rig.render(t, /*draft=*/false, OfxRectI{0, 0, 0, 0}, interactive) == kOfxStatOK);
            double sum = 0.0;
            std::size_t covered = 0;
            for (int y = 0; y < 540; ++y) {
                for (int x = 0; x < 960; ++x) {
                    const float* p = rig.output.pixel(x, y);
                    REQUIRE(p[3] != -7.0f);  // every pixel written
                    covered += p[3] > 0.99f ? 1u : 0u;
                    sum += p[0] + p[1] + p[2];
                }
            }
            const double mean = sum / (3.0 * 960.0 * 540.0);
            CHECK(covered > 960u * 540u * 9u / 10u);
            CHECK(mean > 0.05);
            CHECK(mean < 0.95);
        }
    }
}

TEST_CASE("the own-GPU generator sphere matches the CPU copy of the stitch", "[ofx][source][sample][cuda][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    GeneratorRig cpu(1024, 512);
    aimAtSample(cpu, clip, osv::ofx::source::kOutputEquirect);
    {
        GpuSwitch off("0");
        REQUIRE(cpu.render(5.0, /*draft=*/true) == kOfxStatOK);
    }
    // A host CUDA context current on the render thread, as in Resolve.
    CudaHost host;
    if (!host.context) {
        SKIP(host.failure);
    }
    GeneratorRig gpu(1024, 512, /*negativePitch=*/true, 4);
    aimAtSample(gpu, clip, osv::ofx::source::kOutputEquirect);
    {
        GpuSwitch on("1");
        REQUIRE(gpu.render(5.0, /*draft=*/true) == kOfxStatOK);
    }
    CHECK(currentContext() == host.context);
    const double db = psnr(gpu.output, cpu.output, gpu.frame);
    INFO("own GPU vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);
    CHECK(MockHost::instance().imagesOut == 0);
}

// ---------------------------------------------------------------------------
//  Benchmark (hidden: run it by name or with the [.bench] tag)
// ---------------------------------------------------------------------------

TEST_CASE("benchmark: the generator's view, CPU framing vs the own GPU path", "[.bench][cuda][sample][ofxgpu]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    std::string why;
    if (!cudaDeviceUsable(why)) {
        SKIP(why);
    }
    using Clock = std::chrono::steady_clock;
    constexpr int kFrames = 24;
    struct Size {
        int w;
        int h;
    };
    for (const Size size : {Size{1920, 1080}, Size{3840, 2160}}) {
        GeneratorRig rig(size.w, size.h);
        aimAtSample(rig, clip, osv::ofx::source::kOutputReframed);
        // Warm both paths over the same frames first: decoder opens, analysis
        // buckets, the GPU slots and kernels - none of it is per-frame cost.
        for (int f = 0; f < kFrames; ++f) {
            {
                GpuSwitch off("0");
                REQUIRE(rig.render(static_cast<double>(f), false, OfxRectI{0, 0, 0, 0}, true) == kOfxStatOK);
            }
            {
                GpuSwitch on("1");
                REQUIRE(rig.render(static_cast<double>(f), false, OfxRectI{0, 0, 0, 0}, true) == kOfxStatOK);
            }
        }
        // Then time them frame by frame, interleaved (playback: Interactive).
        std::vector<double> cpuMs;
        std::vector<double> gpuMs;
        for (int f = 0; f < kFrames; ++f) {
            for (const bool onGpu : {false, true}) {
                GpuSwitch sw(onGpu ? "1" : "0");
                const auto t0 = Clock::now();
                REQUIRE(rig.render(static_cast<double>(f), false, OfxRectI{0, 0, 0, 0}, true) == kOfxStatOK);
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                (onGpu ? gpuMs : cpuMs).push_back(ms);
            }
        }
        const auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v.empty() ? 0.0 : v[v.size() / 2];
        };
        const double cpuMedian = median(cpuMs);
        const double gpuMedian = median(gpuMs);
        char line[256] = {};
        std::snprintf(line, sizeof(line),
                      "generator view %dx%d float: CPU framing %.2f ms, own GPU path %.2f ms (median of %d), %.2fx",
                      size.w, size.h, cpuMedian, gpuMedian, kFrames, gpuMedian > 0.0 ? cpuMedian / gpuMedian : 0.0);
        WARN(line);
    }
}

// ---------------------------------------------------------------------------
//  [WP-V-GPU] ... and as VEGAS Pro drives it: every format, both levels
//
//  These need a process whose mock host IS VEGAS (the host profile is fixed
//  per module load): ctest runs them as "vegas: ..." with
//  OSV_MOCK_OFX_PROFILE=vegas - and OPENOSV_OFX_GPU=0 for the CPU tests of
//  that registration, which each test here overrides for its GPU renders.
//  Started in another process they SKIP.
// ---------------------------------------------------------------------------

namespace {

#define REQUIRE_VEGAS_AND_CUDA()                                                          \
    do {                                                                                  \
        if (!MockHost::instance().isVegas()) {                                            \
            SKIP("needs OSV_MOCK_OFX_PROFILE=vegas (ctest runs these as 'vegas: ...')");  \
        }                                                                                 \
        REQUIRE(Fixture::get().ready);                                                    \
        std::string whyNoCuda_;                                                           \
        if (!cudaDeviceUsable(whyNoCuda_)) {                                              \
            SKIP(whyNoCuda_);                                                             \
        }                                                                                 \
    } while (false)

/// One of the four formats VEGAS hands an effect that lists its BGR tokens.
struct VegasFormat {
    osv::ofx::HostDepth depth;
    osv::ofx::HostOrder order;
};
constexpr VegasFormat kVegasFormats[] = {
    {osv::ofx::HostDepth::Byte, osv::ofx::HostOrder::Rgba},
    {osv::ofx::HostDepth::Byte, osv::ofx::HostOrder::Bgra},
    {osv::ofx::HostDepth::Float, osv::ofx::HostOrder::Rgba},
    {osv::ofx::HostDepth::Float, osv::ofx::HostOrder::Bgra},
};

/// "byte BGRA", for INFO lines.
std::string vegasFormatName(const VegasFormat& f) {
    return std::string(osv::ofx::hostDepthName(f.depth)) + " " + osv::ofx::hostOrderName(f.order);
}

/// The GPU render against the CPU render of the same instance, over `area`:
/// an 8-bit image within one code, a float one at 60 dB or better (the
/// transcendental functions differ between the two in the last ulp).
void checkGpuMatchesCpu(const HostImage& gpu, const HostImage& cpu, const OfxRectI& area) {
    if (gpu.depth == osv::ofx::HostDepth::Byte) {
        const double worst = maxDifference(gpu, cpu, area);
        INFO("largest difference " << worst * 255.0 << " codes");
        CHECK(worst <= 1.0 / 255.0 + 1.0e-6);
    } else {
        const double db = psnr(gpu, cpu, area);
        INFO("own GPU vs CPU PSNR: " << db << " dB");
        CHECK(db >= 60.0);
    }
}

/// True when every byte of every pixel outside `area` is the same in the two
/// images (both were filled with the same sentinel before rendering).
bool sameOutside(const HostImage& a, const HostImage& b, const OfxRectI& area) {
    for (int y = a.bounds.y1; y < a.bounds.y2; ++y) {
        for (int x = a.bounds.x1; x < a.bounds.x2; ++x) {
            if (x >= area.x1 && x < area.x2 && y >= area.y1 && y < area.y2) {
                continue;
            }
            const unsigned char* p = a.rawPixel(x, y);
            const unsigned char* q = b.rawPixel(x, y);
            if (!p || !q || std::memcmp(p, q, static_cast<std::size_t>(a.pixelBytes())) != 0) {
                return false;
            }
        }
    }
    return true;
}

/// One VEGAS filter instance: a panorama in `format` on its Source (order
/// labelled on the image, as VEGAS does), a `format` Output.  [WP-PAR]
/// `pixelAspect` is the project's pixel aspect (kW stays the pixel width).
struct VegasFilter {
    std::unique_ptr<Effect> effect;
    HostImage source;

    VegasFilter(const VegasFormat& format, bool negativeSource, int padSource, double pan, double pixelAspect = 1.0) {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().reframe.createInstance(kOfxImageEffectContextFilter, kW, kH, 29.97, &st,
                                                       pixelAspect);
        REQUIRE(st == kOfxStatOK);
        effect->params.find(cam::kPan)->d = pan;
        effect->params.find(cam::kTilt)->d = -14.0;
        effect->params.find(cam::kDjiFov)->d = 84.0;
        HostImage painted = makeImage(OfxRectI{0, 0, 512, 256});
        paintPanorama(painted);
        source = convertImage(painted, format.depth, format.order, negativeSource, padSource);
        source.labelOrder = true;
        Clip* in = effect->clip(kOfxImageEffectSimpleSourceClipName);
        REQUIRE(in);
        in->rod = OfxRectD{0, 0, 512, 256};
        provideImage(*in, source);
    }

    /// Render `window` into `output` (its storage kept by the caller).
    OfxStatus render(HostImage& output, const OfxRectI& window, double time = 0.0) {
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        if (!out) {
            return kOfxStatErrBadHandle;
        }
        provideImage(*out, output);
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = window;
        return Fixture::get().reframe.render(*effect, args);
    }
};

}  // namespace

TEST_CASE("VEGAS: the own GPU filter frames every format like the CPU path", "[ofx][.vegas][cuda][ofxgpu]") {
    REQUIRE_VEGAS_AND_CUDA();
    popAllContexts();
    const bool primaryWasActive = primaryContextActive();
    bool gpuRan = false;

    const OfxRectI bounds{0, 0, kW, kH};
    const OfxRectI windows[] = {bounds, OfxRectI{37, 21, kW - 53, kH - 17}, OfxRectI{-9, -5, kW + 11, kH + 7}};
    int variant = 0;
    for (const VegasFormat& format : kVegasFormats) {
        for (const OfxRectI& window : windows) {
            // Odd pitches and row orders along the way, source and output apart.
            const bool negSource = (variant % 2) == 1;
            const int padSource = (variant % 3) * 4;
            const bool negOutput = (variant % 4) >= 2;
            const int padOutput = (variant % 3 == 1) ? 3 : 0;
            ++variant;
            INFO(vegasFormatName(format) << ", window " << window.x1 << "," << window.y1 << " - " << window.x2 << ","
                                         << window.y2 << ", variant " << variant);
            VegasFilter filter(format, negSource, padSource, 52.0);

            HostImage cpu = makeImage(bounds, negOutput, padOutput, format.depth, format.order);
            cpu.labelOrder = true;
            cpu.fill(-7.0f);
            {
                GpuSwitch off("0");
                REQUIRE(filter.render(cpu, window) == kOfxStatOK);
            }
            if (!primaryWasActive && !gpuRan) {
                CHECK_FALSE(primaryContextActive());  // the CPU path never touched the device
            }
            HostImage gpu = makeImage(bounds, negOutput, padOutput, format.depth, format.order);
            gpu.labelOrder = true;
            gpu.fill(-7.0f);
            {
                GpuSwitch on("1");
                REQUIRE(filter.render(gpu, window) == kOfxStatOK);
                gpuRan = true;
            }
            CHECK(primaryContextActive());
            CHECK(currentContext() == nullptr);
            const OfxRectI area = clipRect(window, bounds);
            checkGpuMatchesCpu(gpu, cpu, area);
            CHECK(sameOutside(gpu, cpu, area));
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: the own GPU filter frames a non-square project like the CPU path",
          "[ofx][.vegas][cuda][ofxgpu][par]") {
    // [WP-PAR] Both GPU kernels - the float sampler (osvReframeEquirectPixel)
    // and the 8-bit one of OfxReframeKernel.cu - take a pixel's display
    // width from the camera block; the CPU loop is held to the square-pixel
    // picture by test_ofx_vegas.cpp, so GPU == CPU here closes the loop.
    REQUIRE_VEGAS_AND_CUDA();
    popAllContexts();
    const OfxRectI bounds{0, 0, kW, kH};
    const double kAspects[] = {4.0 / 3.0, 0.5};
    for (const double aspect : kAspects) {
        for (const VegasFormat& format : kVegasFormats) {
            INFO(vegasFormatName(format) << ", pixel aspect " << aspect);
            VegasFilter filter(format, false, 0, 37.0, aspect);

            HostImage cpu = makeImage(bounds, false, 0, format.depth, format.order);
            cpu.labelOrder = true;
            cpu.fill(-7.0f);
            {
                GpuSwitch off("0");
                REQUIRE(filter.render(cpu, bounds) == kOfxStatOK);
            }
            HostImage gpu = makeImage(bounds, false, 0, format.depth, format.order);
            gpu.labelOrder = true;
            gpu.fill(-7.0f);
            {
                GpuSwitch on("1");
                REQUIRE(filter.render(gpu, bounds) == kOfxStatOK);
            }
            checkGpuMatchesCpu(gpu, cpu, bounds);

            // And the pixel aspect really changed the picture: the same
            // instance in a square project frames differently.
            VegasFilter squareFilter(format, false, 0, 37.0, 1.0);
            HostImage square = makeImage(bounds, false, 0, format.depth, format.order);
            square.labelOrder = true;
            square.fill(-7.0f);
            {
                GpuSwitch on("1");
                REQUIRE(squareFilter.render(square, bounds) == kOfxStatOK);
            }
            CHECK(maxDifference(gpu, square, bounds) > 0.01);
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: cloned filter instances render on the GPU at the same time", "[ofx][.vegas][cuda][ofxgpu]") {
    REQUIRE_VEGAS_AND_CUDA();
    // VEGAS clones a fully-safe effect once per render thread and renders the
    // clones concurrently.  More clones than the pipeline has GPU slots per
    // device, each with its own camera and format, so a slot, stream or
    // staging band shared by mistake shows up as a wrong picture.
    constexpr int kClones = 6;
    constexpr int kRounds = 3;
    const OfxRectI bounds{0, 0, kW, kH};
    std::vector<std::unique_ptr<VegasFilter>> clones;
    std::vector<HostImage> references;
    std::vector<HostImage> outputs;
    for (int i = 0; i < kClones; ++i) {
        const VegasFormat& format = kVegasFormats[i % 4];
        clones.push_back(std::make_unique<VegasFilter>(format, i % 2 == 1, (i % 3) * 4, -90.0 + 37.0 * i));
        // The CPU render of each clone first, on this thread.
        HostImage ref = makeImage(bounds, false, 0, format.depth, format.order);
        ref.labelOrder = true;
        ref.fill(-7.0f);
        {
            GpuSwitch off("0");
            REQUIRE(clones.back()->render(ref, bounds) == kOfxStatOK);
        }
        references.push_back(std::move(ref));
        HostImage out = makeImage(bounds, false, 0, format.depth, format.order);
        out.labelOrder = true;
        outputs.push_back(std::move(out));
    }

    // Then every clone at once, on the GPU.  Catch2's assertions are not
    // thread-safe: the threads only record.
    std::vector<OfxStatus> statuses(static_cast<std::size_t>(kClones * kRounds), kOfxStatFailed);
    {
        GpuSwitch on("1");
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < kClones; ++i) {
            threads.emplace_back([&, i] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const std::size_t k = static_cast<std::size_t>(i);
                for (int r = 0; r < kRounds; ++r) {
                    outputs[k].fill(-7.0f);
                    statuses[k * kRounds + static_cast<std::size_t>(r)] = clones[k]->render(outputs[k], bounds);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (std::thread& t : threads) {
            t.join();
        }
    }
    for (std::size_t k = 0; k < statuses.size(); ++k) {
        INFO("render " << k);
        CHECK(statuses[k] == kOfxStatOK);
    }
    for (int i = 0; i < kClones; ++i) {
        INFO("clone " << i << ", " << vegasFormatName(kVegasFormats[i % 4]));
        const std::size_t k = static_cast<std::size_t>(i);
        checkGpuMatchesCpu(outputs[k], references[k], bounds);
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("VEGAS: the own GPU generator packs every format and level like the CPU path",
          "[ofx][.vegas][sample][cuda][ofxgpu]") {
    REQUIRE_VEGAS_AND_CUDA();
    const std::string clip = sampleClipPath();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // Draft on both sides: no seam search or measured corrections, so the
    // two stitches are the same pixels (see the view test above).  The view
    // through the sphere, for the same reason as there: this test holds the
    // packing to the CPU path's bytes, not the direct view's sharper pixels.
    const DirectSwitch spherePath("0");
    const int modes[] = {osv::ofx::source::kOutputEquirect, osv::ofx::source::kOutputReframed};
    const osv::ofx::OutputLevels allLevels[] = {osv::ofx::OutputLevels::Full, osv::ofx::OutputLevels::Studio};
    // The output image larger than the camera frame (the clip's region of
    // definition), and a window wider than both: the transparent black
    // outside the frame - studio black under studio levels - is compared too.
    const OfxRectI bounds{0, 0, 512, 256};
    const OfxRectD rod{16.0, 8.0, 496.0, 248.0};
    const OfxRectI window{-8, -4, 520, 260};
    for (const int mode : modes) {
        for (const VegasFormat& format : kVegasFormats) {
            for (const osv::ofx::OutputLevels levels : allLevels) {
                INFO("output " << mode << ", " << vegasFormatName(format) << " "
                               << osv::ofx::outputLevelsName(levels));
                HostImage outputs[2];
                for (int onGpu = 0; onGpu < 2; ++onGpu) {
                    OfxStatus st = kOfxStatFailed;
                    auto effect = Fixture::get().source.createInstance(kOfxImageEffectContextGenerator, bounds.x2,
                                                                       bounds.y2, 29.97, &st);
                    REQUIRE(st == kOfxStatOK);
                    const auto param = [&effect](const char* name) -> Param& {
                        Param* p = effect->params.find(name);
                        REQUIRE(p);
                        return *p;
                    };
                    param(osv::ofx::source::kFile).s = clip;
                    param(osv::ofx::source::kOutput).i = mode;
                    param(cam::kPan).d = 25.0;
                    param(osv::ofx::source_params::kOutputLevels).i = static_cast<int>(levels);
                    Clip* outClip = effect->clip(kOfxImageEffectOutputClipName);
                    REQUIRE(outClip);
                    outClip->rod = rod;
                    HostImage& out = outputs[onGpu];
                    out = makeImage(bounds, onGpu == 1, onGpu == 1 ? 2 : 0, format.depth, format.order);
                    out.labelOrder = true;
                    out.fill(-7.0f);
                    provideImage(*outClip, out);
                    PluginHarness::RenderArgs args;
                    args.time = 4.0;
                    args.window = window;
                    args.draft = true;
                    {
                        GpuSwitch sw(onGpu == 1 ? "1" : "0");
                        REQUIRE(Fixture::get().source.render(*effect, args) == kOfxStatOK);
                    }
                    (void)Fixture::get().source.destroyInstance(*effect);
                }
                checkGpuMatchesCpu(outputs[1], outputs[0], bounds);
            }
        }
    }
    CHECK(MockHost::instance().imagesOut == 0);
}
