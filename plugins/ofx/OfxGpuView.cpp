// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxGpuView.cpp - the effects' own GPU path (OfxGpuView.h).
//
// This translation unit is the path's entry point.  Until a GPU
// implementation serves a frame, every function answers "not mine" (false,
// empty error), which the contract in OfxGpuView.h defines as "the caller
// runs its CPU path": the behaviour of every build and host before the path
// existed.

#include "OfxGpuView.h"

namespace osv::ofx::gpu {

bool renderSourceViewGpu(premiere::ImporterInstance& /*clip*/, std::uint32_t /*index*/,
                         const premiere::OutputGeometry& /*sphere*/, bool /*draft*/,
                         premiere::RenderPurpose /*purpose*/, const reframe::Settings& /*settings*/,
                         reframe::SizePx /*projectSize*/, const HostTarget& /*target*/, std::string& error) noexcept {
    // No GPU path serves the generator's view in this build: the CPU frames it.
    error.clear();
    return false;
}

bool renderSourceEquirectGpu(premiere::ImporterInstance& /*clip*/, std::uint32_t /*index*/, bool /*draft*/,
                             premiere::RenderPurpose /*purpose*/, const HostTarget& /*target*/,
                             std::string& error) noexcept {
    // No GPU path serves the generator's sphere in this build: the CPU copies it.
    error.clear();
    return false;
}

bool renderReframeFromHostGpu(const reframe::KernelSetup& /*setup*/, const HostImageView& /*source*/,
                              const HostTarget& /*target*/, std::string& error) noexcept {
    // No GPU path serves the filter on CPU images in this build: the CPU frames it.
    error.clear();
    return false;
}

}  // namespace osv::ofx::gpu
