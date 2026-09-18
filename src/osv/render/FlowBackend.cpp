// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlowBackend.cpp - backend selection, and the classical implementation.
//
// The neural backend lives in FlowBackendOnnx.cpp and is compiled only when
// OSV_HAVE_ONNXRUNTIME is defined.  This file therefore has exactly one
// #if on that macro - around the factory call - so that a build without
// ONNX Runtime differs from one with it in a single place, and every other
// line here is compiled identically either way.

#include "osv/render/FlowBackend.h"

#include "osv/core/Log.h"

#include <utility>

namespace osv::render {

#if defined(OSV_HAVE_ONNXRUNTIME)
/// Defined in FlowBackendOnnx.cpp.  Declared here rather than in the public
/// header because nothing outside this file may construct it directly: the
/// factory is the only supported entry point, so that the fallback logic
/// cannot be bypassed by accident.
std::unique_ptr<FlowBackend> makeOnnxFlowBackend(const FlowBackendParams& params);
#endif

namespace {

/// Dense Inverse Search, the always-available backend.
class ClassicalFlowBackend final : public FlowBackend {
public:
    ClassicalFlowBackend() = default;

    [[nodiscard]] FlowBackendInfo info() const override {
        FlowBackendInfo out;
        out.kind = FlowBackendKind::Classical;
        out.available = true;
        out.detail = "Dense Inverse Search (CPU)";
        // Every operation is float arithmetic in a fixed order with no
        // threading inside the solve, so two runs on the same machine agree
        // bit for bit.  That is worth advertising: it is what makes a
        // golden-image test possible at all.
        out.deterministic = true;
        return out;
    }

    [[nodiscard]] bool isAvailable() const override { return true; }

    [[nodiscard]] Result<BidirFlow> compute(const GrayImage& a, const GrayImage& b, const FlowBackendParams& params,
                                            ThreadPool* pool) override {
        // The consistency tolerance lives on FlowBackendParams so it applies
        // uniformly to every backend; copy it into the solver's own params
        // rather than duplicating the field.
        DisFlowParams dis = params.dis;
        dis.consistencyTolPx = params.consistencyTolPx;
        return disFlowBidirectional(a, b, dis, pool);
    }
};

/// A backend that cannot run, carrying the reason.
///
/// Returned instead of null so every caller can treat the result uniformly -
/// ask info() for the reason, or call compute() and get a clean Unsupported.
/// A null return would push that branch onto every call site.
class UnavailableFlowBackend final : public FlowBackend {
public:
    UnavailableFlowBackend(FlowBackendKind kind, std::string reason)
        : m_kind(kind), m_reason(std::move(reason)) {}

    [[nodiscard]] FlowBackendInfo info() const override {
        FlowBackendInfo out;
        out.kind = m_kind;
        out.available = false;
        out.detail = m_reason;
        out.deterministic = false;
        return out;
    }

    [[nodiscard]] bool isAvailable() const override { return false; }

    [[nodiscard]] Result<BidirFlow> compute(const GrayImage&, const GrayImage&, const FlowBackendParams&,
                                            ThreadPool*) override {
        return Error{ErrorCode::Unsupported, m_reason};
    }

private:
    FlowBackendKind m_kind;
    std::string m_reason;
};

}  // namespace

const char* flowBackendName(FlowBackendKind kind) noexcept {
    switch (kind) {
        case FlowBackendKind::Auto:      return "auto";
        case FlowBackendKind::Classical: return "classical";
        case FlowBackendKind::Neural:    return "neural";
        case FlowBackendKind::Count:     break;
    }
    // Unreachable for any enumerator; a value from a corrupt preferences
    // byte lands here rather than off the end of a table.
    return "unknown";
}

bool haveNeuralFlowBackend() noexcept {
#if defined(OSV_HAVE_ONNXRUNTIME)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<FlowBackend> makeFlowBackend(FlowBackendKind kind, const FlowBackendParams& params) {
    switch (kind) {
        case FlowBackendKind::Classical:
            return std::make_unique<ClassicalFlowBackend>();

        case FlowBackendKind::Neural: {
#if defined(OSV_HAVE_ONNXRUNTIME)
            std::unique_ptr<FlowBackend> neural = makeOnnxFlowBackend(params);
            if (neural) {
                return neural;
            }
            return std::make_unique<UnavailableFlowBackend>(FlowBackendKind::Neural,
                                                            "the neural backend could not be constructed");
#else
            (void)params;
            return std::make_unique<UnavailableFlowBackend>(
                FlowBackendKind::Neural, "this build has no neural backend (built without ONNX Runtime)");
#endif
        }

        case FlowBackendKind::Auto: {
            // Prefer the neural path when it can actually run, but only then:
            // "Auto" must never fail on a machine where "Classical" would
            // have worked, which is the whole point of offering it.
#if defined(OSV_HAVE_ONNXRUNTIME)
            std::unique_ptr<FlowBackend> neural = makeOnnxFlowBackend(params);
            if (neural && neural->isAvailable()) {
                return neural;
            }
#endif
            return std::make_unique<ClassicalFlowBackend>();
        }

        case FlowBackendKind::Count:
            break;
    }
    // A corrupt preference byte: the classical backend is the safe answer
    // because it always works, and saying so beats refusing to render.
    log::warn("flow backend: unknown kind {}; using the classical solver", static_cast<int>(kind));
    return std::make_unique<ClassicalFlowBackend>();
}

Result<BidirFlow> computeFlow(FlowBackendKind kind, const GrayImage& a, const GrayImage& b,
                              const FlowBackendParams& params, ThreadPool* pool, FlowBackendKind* usedKind) {
    std::unique_ptr<FlowBackend> backend = makeFlowBackend(kind, params);
    // makeFlowBackend never returns null; the check is a guard against a
    // future edit breaking that promise, not against today's code.
    if (backend && backend->isAvailable()) {
        Result<BidirFlow> result = backend->compute(a, b, params, pool);
        if (result.ok()) {
            if (usedKind) {
                *usedKind = backend->info().kind;
            }
            return result;
        }
        // A backend that was available and still failed is worth a line in
        // the log: silently producing a different result than the user asked
        // for is exactly the behaviour that makes a plug-in untrustworthy.
        log::warn("flow backend '{}' failed ({}); falling back to the classical solver",
                  flowBackendName(backend->info().kind), result.error().message);
    } else if (kind != FlowBackendKind::Classical) {
        log::info("flow backend '{}' is unavailable ({}); using the classical solver", flowBackendName(kind),
                  backend ? backend->info().detail : "no backend");
    }

    // The classical solver is the floor: it needs no model, no GPU and no
    // runtime, so if it cannot run then nothing can and the error is real.
    ClassicalFlowBackend fallback;
    Result<BidirFlow> result = fallback.compute(a, b, params, pool);
    if (result.ok() && usedKind) {
        *usedKind = FlowBackendKind::Classical;
    }
    return result;
}

}  // namespace osv::render
