// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/core/ThreadPool.h"

#include <algorithm>
#include <exception>

namespace osv {

// -----------------------------------------------------------------------------
//  Construction / destruction
// -----------------------------------------------------------------------------
ThreadPool::ThreadPool(unsigned threads) {
    unsigned count = threads;
    if (count == 0) {
        count = std::thread::hardware_concurrency();
    }
    // hardware_concurrency() may legitimately return 0 on exotic systems.
    if (count == 0) {
        count = 1;
    }
    // Cap to something sane so a mis-typed argument cannot spawn thousands.
    count = std::min(count, 256u);

    m_workers.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

ThreadPool& ThreadPool::global() {
    static ThreadPool instance(0);
    return instance;
}

// -----------------------------------------------------------------------------
//  Work distribution
// -----------------------------------------------------------------------------
void ThreadPool::runChunks(Job& job) {
    // Each worker (and the caller) grabs chunk indices from a shared counter
    // until the range is exhausted.  Chunks are contiguous slices of `grain`
    // indices, which keeps memory access patterns friendly for row-based work.
    const std::size_t total = job.end - job.begin;
    const std::size_t chunkCount = (total + job.grain - 1) / job.grain;

    for (;;) {
        const std::size_t chunk = job.nextChunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= chunkCount) {
            break;
        }
        const std::size_t b = job.begin + chunk * job.grain;
        const std::size_t e = std::min(job.end, b + job.grain);

        if (!job.failed.load(std::memory_order_relaxed)) {
            try {
                (*job.body)(b, e);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(job.failureMutex);
                job.failed.store(true, std::memory_order_relaxed);
                if (job.failureMessage.empty()) {
                    job.failureMessage = ex.what();
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(job.failureMutex);
                job.failed.store(true, std::memory_order_relaxed);
                if (job.failureMessage.empty()) {
                    job.failureMessage = "non-standard exception in parallelFor body";
                }
            }
        }

        // Signal completion of this chunk; the last one wakes the caller.
        if (job.remainingChunks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.notify_all();
        }
    }
}

void ThreadPool::workerLoop() {
    std::uint64_t seenGeneration = 0;
    for (;;) {
        Job* job = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || (m_currentJob != nullptr && m_generation != seenGeneration); });
            if (m_stop) {
                return;
            }
            job = m_currentJob;
            seenGeneration = m_generation;
        }
        if (job) {
            runChunks(*job);
        }
    }
}

Status ThreadPool::parallelFor(std::size_t begin, std::size_t end, std::size_t grain, const ChunkBody& body) {
    if (!body) {
        return failStatus(ErrorCode::InvalidArgument, "parallelFor: null body");
    }
    if (end <= begin) {
        return okStatus();  // empty range is fine
    }
    if (grain == 0) {
        grain = 1;
    }

    Job job;
    job.begin = begin;
    job.end = end;
    job.grain = grain;
    job.body = &body;
    const std::size_t total = end - begin;
    const std::size_t chunkCount = (total + grain - 1) / grain;
    job.remainingChunks.store(chunkCount, std::memory_order_relaxed);

    // Publish the job and wake the workers.  parallelFor is serialised by the
    // caller (one job at a time per pool); nested calls from a body would
    // deadlock, which the CPU renderer never does.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentJob = &job;
        ++m_generation;
    }
    m_wake.notify_all();

    // The calling thread helps so single-threaded pools still make progress.
    runChunks(job);

    // Wait for stragglers still inside their chunk.
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [&] { return job.remainingChunks.load(std::memory_order_acquire) == 0; });
        m_currentJob = nullptr;
    }

    if (job.failed.load(std::memory_order_relaxed)) {
        return failStatus(ErrorCode::Internal, "parallelFor body failed: " + job.failureMessage);
    }
    return okStatus();
}

Status ThreadPool::parallelRows(std::size_t rows, std::size_t grain,
                                const std::function<void(std::size_t row)>& body) {
    if (!body) {
        return failStatus(ErrorCode::InvalidArgument, "parallelRows: null body");
    }
    return parallelFor(0, rows, grain, [&body](std::size_t b, std::size_t e) {
        for (std::size_t r = b; r < e; ++r) {
            body(r);
        }
    });
}

}  // namespace osv
