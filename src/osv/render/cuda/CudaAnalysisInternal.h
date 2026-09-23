// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaAnalysisInternal.h - declarations shared between the host files of the
// GPU analyses and nowhere else.  Not installed, not part of the public API:
// the only supported way to reach the CUDA flow backend is through
// makeFlowBackend() after installCudaAnalyses(), so the fallback logic in
// computeFlow() can never be bypassed by constructing it directly.

#pragma once

#include "osv/render/FlowBackend.h"

#include <memory>

namespace osv::render::gpu {

/// The factory installed for FlowBackendKind::ClassicalCuda.  Never null.
std::unique_ptr<FlowBackend> makeCudaDisFlowBackend(const FlowBackendParams& params);

/// Clear this thread's CUDA "last error" so the launch checks that follow
/// report only launches made by the analyses themselves.  A non-sticky error
/// left behind by unrelated code (another renderer, the host) would
/// otherwise surface through the first cudaGetLastError() after our own
/// launch and be blamed on it.
void clearStaleCudaError() noexcept;

}  // namespace osv::render::gpu
