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
#include "OfxSource.h"  // [WP-V-GPU] the generator's parameter names

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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
