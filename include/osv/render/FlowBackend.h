// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlowBackend.h - one interface, several ways of measuring optical flow.
//
// WHY AN INTERFACE AT ALL
// -----------------------
// The parallax correction needs a dense correspondence field across the
// overlap band.  There is more than one defensible way to get it, they have
// genuinely different tradeoffs, and which one is right depends on the clip
// and on the machine:
//
//   Classical (DIS)  Always available, no model file, no GPU required, and
//                    entirely deterministic - the same input gives the same
//                    field on every machine and every run.  Weaker on large
//                    displacements and on repetitive texture, where patch
//                    matching has more than one plausible answer.
//
//   Neural           Better on exactly those hard cases, because a learned
//                    model brings a prior about what real motion looks like
//                    that a local patch match cannot have.  Costs a model
//                    file, a runtime dependency and a GPU, and its output is
//                    only as reproducible as the inference stack.
//
// DJI ships both kinds and picks one per product; here the user picks,
// which is strictly more useful.
//
// THE CONTRACT
// ------------
// Every backend takes two single-channel images of the same size and returns
// a bidirectional field plus a consistency mask, in the units DisFlow.h
// documents: (u, v) at p means the content at p in `a` sits at p + (u, v) in
// `b`, in pixels.  A backend that cannot run - no model, no GPU, a size it
// refuses - says so through isAvailable() BEFORE it is asked to compute, so
// the caller can fall back without an exception or a half-filled result.

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/DisFlow.h"

#include <memory>
#include <string>

namespace osv::render {

/// Which implementation to use.
///
/// The numeric values are persisted in the plug-in's preferences blob, so
/// they are permanent: insert new members before Count and never renumber.
enum class FlowBackendKind : int {
    /// Pick the best that is actually available, preferring Neural.  This is
    /// the default because it does the right thing on both a machine with
    /// the model installed and one without.
    Auto = 0,
    /// Dense Inverse Search on the CPU.  Always available.
    Classical = 1,
    /// A neural network through ONNX Runtime, on CUDA when present.
    Neural = 2,
    Count
};

/// Name for a kind, for logs and UI.  Never null.
[[nodiscard]] const char* flowBackendName(FlowBackendKind kind) noexcept;

/// Tuning shared by every backend, plus each backend's own.
struct FlowBackendParams {
    /// Parameters for the classical solver.  Ignored by the neural path.
    DisFlowParams dis;

    /// Path to the neural model file (ONNX).  Empty means "look in the
    /// plug-in's own models/ directory", which is where the installer puts
    /// it; an explicit path overrides that for testing.
    std::string modelPath;

    /// Largest edge the neural backend will accept in one inference.  A band
    /// wider than this is processed in horizontal tiles with overlap, so a
    /// 7680-wide band does not need a 7680-wide tensor.
    int maxTileEdge = 2048;

    /// Overlap between neural tiles, in pixels.  Flow near a tile edge is
    /// unreliable because the network cannot see past it, so tiles overlap
    /// and the seam between them is cross-faded.
    int tileOverlapPx = 64;

    /// Forward-backward consistency tolerance, in pixels.  Applies to every
    /// backend: the neural path produces a mask the same way the classical
    /// one does, so downstream code never has to ask which produced a field.
    double consistencyTolPx = 1.5;
};

/// What a backend can tell the caller about itself.
struct FlowBackendInfo {
    FlowBackendKind kind = FlowBackendKind::Classical;
    bool available = false;      ///< False when it cannot run here.
    std::string detail;          ///< Device name, model name, or the reason it is unavailable.
    bool deterministic = false;  ///< True when repeated runs are bit-identical.
};

/// A way of computing optical flow.
///
/// Implementations are obtained from makeFlowBackend(); the interface is
/// abstract so the neural one can be absent from a build entirely without
/// the callers noticing.
class FlowBackend {
public:
    virtual ~FlowBackend() = default;

    FlowBackend(const FlowBackend&) = delete;
    FlowBackend& operator=(const FlowBackend&) = delete;

    /// What this backend is and whether it can run.
    [[nodiscard]] virtual FlowBackendInfo info() const = 0;

    /// True when compute() can be called.  Checked before every use rather
    /// than once at construction, because a GPU can be lost at runtime.
    [[nodiscard]] virtual bool isAvailable() const = 0;

    /// Compute bidirectional flow between two same-sized single-channel
    /// images.
    ///
    /// Returns Unsupported when the backend is not available - that is a
    /// normal outcome the caller handles by falling back, not a failure.
    [[nodiscard]] virtual Result<BidirFlow> compute(const GrayImage& a, const GrayImage& b,
                                                    const FlowBackendParams& params, ThreadPool* pool) = 0;

protected:
    FlowBackend() = default;
};

/// Create a backend of the requested kind.
///
/// Never returns null and never fails: an unavailable kind yields a backend
/// whose isAvailable() is false and whose info().detail says why, so the
/// caller can report the reason instead of guessing.  `Auto` resolves to the
/// best available at the moment of the call.
[[nodiscard]] std::unique_ptr<FlowBackend> makeFlowBackend(FlowBackendKind kind, const FlowBackendParams& params);

/// Compute flow with `kind`, falling back to the classical backend when the
/// requested one is unavailable or fails.
///
/// This is what production code should call.  `usedKind` (optional) receives
/// what actually ran, so a caller can log "asked for Neural, used Classical"
/// rather than silently producing different output than the user selected.
[[nodiscard]] Result<BidirFlow> computeFlow(FlowBackendKind kind, const GrayImage& a, const GrayImage& b,
                                            const FlowBackendParams& params, ThreadPool* pool,
                                            FlowBackendKind* usedKind = nullptr);

/// True when this build contains the neural backend at all.
///
/// Compile-time capability, distinct from runtime availability: a build
/// without ONNX Runtime returns false here, while a build with it returns
/// true even on a machine with no model file installed.
[[nodiscard]] bool haveNeuralFlowBackend() noexcept;

}  // namespace osv::render
