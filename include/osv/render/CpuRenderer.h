// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CpuRenderer: the reference implementation.  Runs osvShadePixel over row
// bands on a ThreadPool.  Compiled with strict floating point (/fp:precise),
// so its output is the truth the GPU backends are measured against.
#pragma once

#include "osv/render/Renderer.h"

namespace osv::render {

class CpuRenderer final : public IRenderer {
public:
    /// `pool` must outlive the renderer.
    explicit CpuRenderer(ThreadPool& pool) noexcept : m_pool(pool) {}

    Result<ImageRGBAf> render(const RenderJob& job) override;
    [[nodiscard]] const char* name() const noexcept override { return "cpu"; }

    /// Rows per work item handed to the pool (tunable for tests).
    void setRowGrain(std::size_t rows) noexcept { m_rowGrain = rows == 0 ? 1 : rows; }

private:
    ThreadPool& m_pool;
    std::size_t m_rowGrain = 16;
};

}  // namespace osv::render
