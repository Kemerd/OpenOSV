// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ThreadPool: a fixed set of worker threads with a blocking parallelFor.
//
// Used by the CPU renderer (row bands), the seam search, the gain estimation
// and the dual-stream decoder.  It is intentionally simple: parallelFor blocks
// the caller until every chunk has run, exceptions thrown by the body are
// caught and turned into a Status, and the pool can be reused any number of
// times.
#pragma once

#include "osv/core/Result.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace osv {

class ThreadPool {
public:
    /// Body invoked for each chunk [begin, end) of the index range.
    using ChunkBody = std::function<void(std::size_t begin, std::size_t end)>;

    /// Create `threads` workers (0 = std::thread::hardware_concurrency()).
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Number of worker threads (>= 1).
    [[nodiscard]] unsigned size() const noexcept { return static_cast<unsigned>(m_workers.size()); }

    /// Split [begin, end) into chunks of at most `grain` indices, run `body`
    /// on the workers (the calling thread also participates) and wait.
    /// Returns Internal if the body threw or the arguments are invalid.
    Status parallelFor(std::size_t begin, std::size_t end, std::size_t grain, const ChunkBody& body);

    /// Convenience: parallelFor over rows with a per-row body.
    Status parallelRows(std::size_t rows, std::size_t grain, const std::function<void(std::size_t row)>& body);

    /// A process-wide default pool sized to the machine, for callers that do
    /// not want to manage their own.
    static ThreadPool& global();

private:
    /// One unit of parallel work shared between all workers.
    struct Job {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t grain = 1;
        const ChunkBody* body = nullptr;
        std::atomic<std::size_t> nextChunk{0};
        std::atomic<std::size_t> remainingChunks{0};
        std::atomic<bool> failed{false};
        std::string failureMessage;
        std::mutex failureMutex;
    };

    void workerLoop();
    void runChunks(Job& job);

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    Job* m_currentJob = nullptr;
    std::uint64_t m_generation = 0;
    bool m_stop = false;
};

}  // namespace osv
