// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Runner.h - running osvtool (and ffmpeg, for the probes) as child processes
// without ever blocking the UI thread.
//
//   ChildProcess   one child, its stdout and stderr merged on one pipe, no
//                  console window, stdin on the null device.  Stopping it
//                  ends the whole tree it started (osvtool's ffmpeg too):
//                  a Windows job object, a POSIX process group.
//   ProcessRunner  one render at a time on a worker thread: reads the
//                  pipe, parses progress lines (Progress.h) and hands the
//                  UI thread a snapshot and the new log lines on demand.
//   runCapture     run a short command to completion and collect its
//                  output (the start-up probes; called off the UI thread).
#pragma once

#include "Progress.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace osvgui {

// ===========================================================================
//  ChildProcess
// ===========================================================================

/// One child process.  start() once; read() from one thread; kill() from
/// any thread; wait() from the reading thread (or after it is done).
class ChildProcess {
public:
    ChildProcess();
    /// Kills a child that is still running and releases everything.
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Start `exe` (a full path) with `args` (UTF-8, not including the
    /// executable).  False with `error` when it could not be started.
    bool start(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string* error) noexcept;

    /// Blocking read of the merged stdout / stderr.  Returns the byte count,
    /// 0 at the end of the stream (every writer gone), -1 on an error.
    long read(char* buffer, std::size_t size) noexcept;

    /// End the child and everything it started.  Returns immediately; the
    /// end of the stream and the exit follow shortly.  Safe to call twice.
    void kill() noexcept;

    /// Wait for the child to exit, at most `timeoutMs` (negative = no
    /// limit).  True with its exit code once it has exited.
    bool wait(int timeoutMs, int& exitCode) noexcept;

    /// start() succeeded.
    [[nodiscard]] bool started() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// ===========================================================================
//  ProcessRunner
// ===========================================================================

/// What the UI needs to know about the render in flight.
struct RunnerState {
    bool running = false;        ///< The child is alive (or its output is still being read).
    bool finished = false;       ///< It exited and everything it printed has been read.
    int exitCode = 0;            ///< Valid once finished.
    bool stopRequested = false;  ///< stop() was called for this run.
    bool haveProgress = false;   ///< At least one progress line arrived.
    ProgressLine progress;       ///< The latest progress line.
    std::string lastError;       ///< The last error message osvtool printed.
    std::string lastLine;        ///< The last line that was not a progress line.
};

/// Runs one child at a time on its own worker thread.
class ProcessRunner {
public:
    /// `wake` is called from the worker thread whenever there is something
    /// new for the UI (it should wake the event loop, e.g. glfwPostEmptyEvent).
    explicit ProcessRunner(std::function<void()> wake = {});
    /// Stops a running child and joins the worker.
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    /// Start a run.  False with `error` when one is still in flight (call
    /// reset() after it finished) or the child could not be started.
    bool start(const std::filesystem::path& exe, const std::vector<std::string>& args, std::string* error);

    /// Ask the running child (and its children) to end.
    void stop() noexcept;

    /// A run was started and has not been reset() yet.
    [[nodiscard]] bool busy() const noexcept { return m_busy.load(std::memory_order_acquire); }

    /// A copy of the current state.
    [[nodiscard]] RunnerState state() const;

    /// Move the lines printed since the last call into `out` (appended).
    void takeLines(std::vector<std::string>& out);

    /// After the run finished: join the worker and get ready for the next.
    /// Stops the child first when it is still running.
    void reset();

private:
    void worker();
    void publish(std::vector<std::string>& lines);

    std::function<void()> m_wake;
    mutable std::mutex m_mutex;  // guards m_state and m_lines
    RunnerState m_state;
    std::vector<std::string> m_lines;
    std::unique_ptr<ChildProcess> m_child;
    std::thread m_thread;
    std::atomic<bool> m_busy{false};
};

// ===========================================================================
//  runCapture
// ===========================================================================

/// The outcome of a short command.
struct CaptureResult {
    bool started = false;   ///< The program could be started at all.
    bool timedOut = false;  ///< It ran past the limit and was killed.
    int exitCode = -1;      ///< Its exit code (valid when started and not timed out).
    std::string output;     ///< stdout and stderr, merged (capped).
    std::string error;      ///< Why it could not be started.
};

/// Run `exe args...` to completion (at most `timeoutMs`), collecting up to
/// `maxBytes` of its output.  Blocks the calling thread: never call it on
/// the UI thread.
[[nodiscard]] CaptureResult runCapture(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                       int timeoutMs, std::size_t maxBytes = 4u << 20);

}  // namespace osvgui
