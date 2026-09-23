// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osv_gpu_decode_bench: measures video::GpuClipDecoder against the WP-A
// acceptance targets in docs/DIRECT_GPU.md.  Not a ctest test - timings on a
// shared machine are not pass/fail material - but the numbers it prints are
// the ones quoted in the commit messages.
//
//   osv_gpu_decode_bench [clip.OSV] [--threads N] [--ahead N] [--budget-mb N] [--loops N]
//
// The clip defaults to OSV_SAMPLE_FILE (environment), then the CMake default.
//
// What it measures (all frames stay on the GPU; nothing is downloaded):
//   open         GpuClipDecoder::open (both NVDEC decoders primed on frame 0)
//   park cold    12 scattered landings, each on a freshly opened decoder, so
//                every landing decodes from its GOP's sync sample
//   park session the same landings on ONE decoder with the cache dropped
//                before each: the decoder position carries over, as it does
//                when a user parks around in Premiere
//   park hit     re-acquiring a landing that is cached
//   sequential   every frame in order, the clip looped, with decode-ahead
//                (and without, to show what the worker buys)
//   VRAM         the cache's own slots, and the process-wide drop in free
//                VRAM while a decoder is open (cache + NVDEC surfaces)

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/GpuClipDecoder.h"

#include <cuda.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `start`.
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Median of a copy of `values` (0 when empty).
double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return (n % 2) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

/// Free VRAM of device 0's primary context in MiB (the bench retains it for
/// the whole run so the numbers are comparable).
double freeVramMiB(CUcontext primary) {
    if (!primary || cuCtxPushCurrent(primary) != CUDA_SUCCESS) {
        return -1.0;
    }
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
    cuMemGetInfo(&freeBytes, &totalBytes);
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
    return static_cast<double>(freeBytes) / (1024.0 * 1024.0);
}

/// Parse "--name value" style integer options; returns `fallback` when absent.
long optionValue(int argc, char** argv, const char* name, long fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) {
            return std::strtol(argv[i + 1], nullptr, 10);
        }
    }
    return fallback;
}

/// The positional clip argument (first argument not starting with "--" and
/// not the value of an option), else OSV_SAMPLE_FILE, else the CMake default.
std::filesystem::path clipPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            ++i;  // skip the option's value
            continue;
        }
        return std::filesystem::path(arg);
    }
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(OSV_SAMPLE_FILE);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace osv;
    log::setLevel(log::Level::Warn);

    // ---- options ----------------------------------------------------------------
    const std::filesystem::path path = clipPath(argc, argv);
    video::GpuDecoderOptions options;
    options.decoderThreads = static_cast<int>(optionValue(argc, argv, "--threads", options.decoderThreads));
    options.decodeAhead = static_cast<std::uint32_t>(optionValue(argc, argv, "--ahead", options.decodeAhead));
    const long budgetMiB = optionValue(argc, argv, "--budget-mb", 0);
    options.vramBudgetBytes = budgetMiB > 0 ? static_cast<std::size_t>(budgetMiB) << 20 : 0;
    const long loops = std::max(1L, optionValue(argc, argv, "--loops", 4));

    std::printf("osv_gpu_decode_bench\n");
    std::printf("  clip            : %s\n", path.string().c_str());
    std::printf("  decoder threads : %d\n", options.decoderThreads);
    std::printf("  decode-ahead    : %u\n", options.decodeAhead);

    // ---- format -----------------------------------------------------------------
    auto file = OsvFile::open(path);
    if (!file.ok()) {
        std::printf("cannot open the clip: %s\n", file.error().toString().c_str());
        return 2;
    }
    auto track = meta::MetadataTrack::load(file.value());
    auto format = meta::FormatDetector::detect(file.value(), track.ok() ? &track.value() : nullptr);
    if (!format.ok()) {
        std::printf("cannot detect the format: %s\n", format.error().toString().c_str());
        return 2;
    }

    // ---- driver + a retained primary context for VRAM readings -------------------
    std::string reason;
    if (!video::GpuClipDecoder::available(&reason)) {
        std::printf("no CUDA: %s\n", reason.c_str());
        return 3;
    }
    CUdevice device = 0;
    CUcontext primary = nullptr;
    if (cuDeviceGet(&device, 0) != CUDA_SUCCESS || cuDevicePrimaryCtxRetain(&primary, device) != CUDA_SUCCESS) {
        std::printf("cannot retain the primary context of device 0\n");
        return 3;
    }
    char deviceName[256] = {};
    cuDeviceGetName(deviceName, static_cast<int>(sizeof(deviceName)), device);
    std::printf("  device          : %s\n\n", deviceName);
    const double freeAtStart = freeVramMiB(primary);

    const std::array<std::uint32_t, 12> landings{27, 45, 10, 63, 32, 5, 50, 18, 40, 2, 58, 23};
    int status = 0;
    {
        // ---- open ------------------------------------------------------------------
        const double freeBefore = freeVramMiB(primary);
        auto t0 = Clock::now();
        auto opened = video::GpuClipDecoder::open(path, format.value(), options);
        const double openMs = msSince(t0);
        if (!opened.ok()) {
            std::printf("open failed: %s\n", opened.error().toString().c_str());
            cuDevicePrimaryCtxRelease(device);
            return 4;
        }
        std::unique_ptr<video::GpuClipDecoder> dec = std::move(opened).value();
        const double freeAfterOpen = freeVramMiB(primary);
        const video::GpuDecoderStats s0 = dec->stats();
        std::printf("open            : %8.1f ms   (%u frames, 2 x %ux%u, slot %.1f MiB, cache %u slots, window %u)\n",
                    openMs, dec->frameCount(), dec->lensWidth(), dec->lensHeight(),
                    static_cast<double>(s0.slotBytes) / 1048576.0, s0.capacitySlots, s0.decodeAheadWindow);
        std::printf("VRAM after open : %8.1f MiB  (NVDEC decoders + first cache slot)\n",
                    freeBefore - freeAfterOpen);

        // ---- park, session (one decoder, cache dropped before each landing) --------
        std::vector<double> sessionMs;
        std::vector<double> hitMs;
        std::printf("\npark (one decoder, cache dropped before each landing; then an immediate re-acquire):\n");
        std::printf("  frame   decode ms   hit ms   source\n");
        for (const std::uint32_t k : landings) {
            dec->dropCachedFrames();
            auto t = Clock::now();
            auto lease = dec->acquire(k);
            const double ms = msSince(t);
            if (!lease.ok()) {
                std::printf("  acquire(%u) failed: %s\n", k, lease.error().toString().c_str());
                status = 5;
                break;
            }
            const video::LeaseSource source = lease.value().source();
            lease.value().release();
            t = Clock::now();
            auto again = dec->acquire(k);
            const double hit = msSince(t);
            if (!again.ok() || again.value().source() != video::LeaseSource::CacheHit) {
                std::printf("  re-acquire(%u) was not a cache hit\n", k);
                status = 5;
            }
            sessionMs.push_back(ms);
            hitMs.push_back(hit);
            std::printf("  %5u   %9.2f   %6.3f   %s\n", k, ms, hit, video::leaseSourceName(source));
        }
        std::printf("  median          : %8.2f ms decode, %6.3f ms hit\n", median(sessionMs), median(hitMs));

        // ---- sequential with decode-ahead ----------------------------------------------
        dec->dropCachedFrames();
        const video::GpuDecoderStats before = dec->stats();
        const std::uint32_t total = dec->frameCount() * static_cast<std::uint32_t>(loops);
        auto ts = Clock::now();
        for (std::uint32_t n = 0; n < total && status == 0; ++n) {
            auto lease = dec->acquire(n % dec->frameCount());
            if (!lease.ok()) {
                std::printf("sequential acquire(%u) failed: %s\n", n % dec->frameCount(),
                            lease.error().toString().c_str());
                status = 6;
            }
        }
        const double seqMs = msSince(ts);
        const video::GpuDecoderStats after = dec->stats();
        std::printf("\nsequential, decode-ahead %u: %u pairs in %.1f ms = %.1f pairs/s  (hits %llu, waited %llu, "
                    "decoded %llu, ahead %llu)\n",
                    after.decodeAheadWindow, total, seqMs, 1000.0 * total / seqMs,
                    static_cast<unsigned long long>(after.cacheHits - before.cacheHits),
                    static_cast<unsigned long long>(after.waitedHits - before.waitedHits),
                    static_cast<unsigned long long>(after.foregroundDecodes - before.foregroundDecodes),
                    static_cast<unsigned long long>(after.framesDecodedAhead - before.framesDecodedAhead));
        const double freePeak = freeVramMiB(primary);
        std::printf("VRAM            : cache %.1f MiB of a %.1f MiB budget (%u/%u slots); process drop %.1f MiB\n",
                    static_cast<double>(after.vramBytes) / 1048576.0,
                    static_cast<double>(after.budgetBytes) / 1048576.0, after.allocatedSlots, after.capacitySlots,
                    freeBefore - freePeak);
        if (after.vramBytes > after.budgetBytes) {
            std::printf("  !! cache exceeds its budget\n");
            status = 7;
        }
    }

    // ---- sequential without decode-ahead (the engine alone) ---------------------------
    if (status == 0) {
        video::GpuDecoderOptions noAhead = options;
        noAhead.decodeAhead = 0;
        auto opened = video::GpuClipDecoder::open(path, format.value(), noAhead);
        if (opened.ok()) {
            std::unique_ptr<video::GpuClipDecoder> dec = std::move(opened).value();
            const std::uint32_t total = dec->frameCount() * static_cast<std::uint32_t>(loops);
            auto ts = Clock::now();
            for (std::uint32_t n = 0; n < total; ++n) {
                auto lease = dec->acquire(n % dec->frameCount());
                if (!lease.ok()) {
                    std::printf("acquire failed: %s\n", lease.error().toString().c_str());
                    status = 6;
                    break;
                }
            }
            const double seqMs = msSince(ts);
            std::printf("sequential, no decode-ahead: %u pairs in %.1f ms = %.1f pairs/s\n", total, seqMs,
                        1000.0 * total / seqMs);
        }
    }

    // ---- park cold: a fresh decoder per landing ------------------------------------------
    if (status == 0) {
        std::vector<double> coldMs;
        std::printf("\npark cold (fresh decoder per landing, decode from the sync sample):\n  ");
        for (const std::uint32_t k : landings) {
            auto opened = video::GpuClipDecoder::open(path, format.value(), options);
            if (!opened.ok()) {
                std::printf("open failed: %s\n", opened.error().toString().c_str());
                status = 4;
                break;
            }
            std::unique_ptr<video::GpuClipDecoder> dec = std::move(opened).value();
            auto t = Clock::now();
            auto lease = dec->acquire(k);
            const double ms = msSince(t);
            if (!lease.ok()) {
                std::printf("acquire(%u) failed: %s\n", k, lease.error().toString().c_str());
                status = 5;
                break;
            }
            coldMs.push_back(ms);
            std::printf("%u:%.1f  ", k, ms);
        }
        std::printf("\n  median          : %8.2f ms  (target < 60 ms)\n", median(coldMs));
    }

    // ---- teardown: everything released ------------------------------------------------------
    const double freeAtEnd = freeVramMiB(primary);
    std::printf("\nfree VRAM       : %.1f MiB at start, %.1f MiB after every decoder closed (%+.1f MiB)\n",
                freeAtStart, freeAtEnd, freeAtEnd - freeAtStart);
    cuDevicePrimaryCtxRelease(device);
    return status;
}
