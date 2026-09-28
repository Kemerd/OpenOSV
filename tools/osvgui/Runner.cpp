// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Runner.cpp - child processes and the render worker (see Runner.h).
//
// Windows: CreateProcessW with an explicit list of the handles the child may
// inherit (the pipe's write end and the null device, nothing else of ours),
// CREATE_NO_WINDOW so no console flashes up, and a job object that kills
// the whole tree when it is terminated - or when this process dies, so a
// crashed GUI never leaves an osvtool / ffmpeg pair running.
//
// POSIX: posix_spawn into a process group of its own, so SIGTERM (then,
// after a grace period, SIGKILL) reaches osvtool's ffmpeg as well.

#include "Runner.h"

#include "CommandBuilder.h"  // quoteWindowsArg
#include "Queue.h"           // pathToUtf8

#include <algorithm>
#include <chrono>
#include <exception>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;  // the process environment, handed to posix_spawn
#endif

namespace osvgui {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ===========================================================================
//  ChildProcess - Windows
// ===========================================================================
#if defined(_WIN32)

namespace {

/// UTF-8 -> UTF-16.
[[nodiscard]] std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

/// A Win32 error code as text ("The system cannot find the file specified.").
[[nodiscard]] std::string win32Message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer) {
        text = pathToUtf8(fs::path(std::wstring(buffer, length)));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buffer) {
        ::LocalFree(buffer);
    }
    return text.empty() ? "Windows error " + std::to_string(code) : text;
}

/// Close a handle and null it.
void closeHandle(HANDLE& h) noexcept {
    if (h && h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
    }
    h = nullptr;
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE readPipe = nullptr;
    std::atomic<bool> killed{false};
};

ChildProcess::ChildProcess()
    : m(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    if (!m) {
        return;
    }
    // Still running: end the tree, and wait so no zombie handle outlives us.
    if (m->process) {
        int code = 0;
        if (!wait(0, code)) {
            kill();
            wait(5000, code);
        }
    }
    closeHandle(m->readPipe);
    closeHandle(m->process);
    // KILL_ON_JOB_CLOSE: closing the last job handle ends anything left in it.
    closeHandle(m->job);
}

bool ChildProcess::start(const fs::path& exe, const std::vector<std::string>& args, std::string* error) noexcept {
    const auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    try {
        if (!m || m->process) {
            return fail("a process was already started");
        }

        // ---- the command line: CreateProcess wants one string ----------------------
        std::string line = quoteWindowsArg(pathToUtf8(exe));
        for (const std::string& arg : args) {
            line.push_back(' ');
            line += quoteWindowsArg(arg);
        }
        std::wstring commandLine = widen(line);
        if (commandLine.size() >= 32767) {
            return fail("the command line is longer than Windows allows (32767 characters)");
        }

        // ---- the output pipe: our end stays ours --------------------------------------
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 64 * 1024)) {
            return fail("cannot create a pipe: " + win32Message(::GetLastError()));
        }
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        // stdin: the null device, so nothing ever waits on a keyboard.
        HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (nul == INVALID_HANDLE_VALUE) {
            nul = nullptr;
        }

        // ---- inherit exactly these handles, nothing else this process holds -----------
        HANDLE inherit[2] = {writeEnd, nul};
        const DWORD inheritCount = nul ? 2u : 1u;
        SIZE_T attrSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<unsigned char> attrStorage(attrSize == 0 ? 64 : attrSize);
        auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        bool haveAttr = ::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize) != FALSE;
        if (haveAttr && !::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                     inheritCount * sizeof(HANDLE), nullptr, nullptr)) {
            ::DeleteProcThreadAttributeList(attrList);
            haveAttr = false;
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = haveAttr ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = writeEnd;
        si.StartupInfo.hStdError = writeEnd;
        si.lpAttributeList = haveAttr ? attrList : nullptr;

        // ---- the job: terminate the tree as one, and with us -----------------------------
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                closeHandle(job);
            }
        }

        // Suspended, so it is in the job before it can start ffmpeg.
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        if (haveAttr) {
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
        mutableLine.push_back(L'\0');
        const std::wstring exeW = exe.wstring();
        const BOOL created = ::CreateProcessW(exeW.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                              nullptr, &si.StartupInfo, &pi);
        const DWORD createError = created ? 0 : ::GetLastError();
        if (haveAttr) {
            ::DeleteProcThreadAttributeList(attrList);
        }
        // The child holds its own copies now; ours would keep the pipe open
        // forever and the reader would never see the end of the stream.
        closeHandle(writeEnd);
        closeHandle(nul);
        if (!created) {
            closeHandle(readEnd);
            closeHandle(job);
            return fail("cannot start " + pathToUtf8(exe.filename()) + ": " + win32Message(createError));
        }

        // A GUI started inside a job that forbids nesting (rare since
        // Windows 8) cannot assign: the child then runs without one, and
        // kill() falls back to TerminateProcess.
        if (job && !::AssignProcessToJobObject(job, pi.hProcess)) {
            closeHandle(job);
        }
        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);

        m->process = pi.hProcess;
        m->job = job;
        m->readPipe = readEnd;
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    } catch (...) {
        return fail("unexpected error starting the process");
    }
}

long ChildProcess::read(char* buffer, std::size_t size) noexcept {
    if (!m || !m->readPipe || !buffer || size == 0) {
        return -1;
    }
    for (;;) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (!::ReadFile(m->readPipe, buffer, want, &got, nullptr)) {
            // Every writer closed its end: the normal end of the stream.
            return ::GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        }
        if (got > 0) {
            return static_cast<long>(got);
        }
        // A zero-byte write on the other side; not the end.
    }
}

void ChildProcess::kill() noexcept {
    if (!m || !m->process) {
        return;
    }
    m->killed = true;
    if (m->job) {
        ::TerminateJobObject(m->job, 1);
    } else {
        ::TerminateProcess(m->process, 1);
    }
}

bool ChildProcess::wait(int timeoutMs, int& exitCode) noexcept {
    if (!m || !m->process) {
        return false;
    }
    const DWORD result = ::WaitForSingleObject(m->process, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
    if (result != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(m->process, &code)) {
        exitCode = -1;
    } else {
        exitCode = static_cast<int>(code);
    }
    return true;
}

bool ChildProcess::started() const noexcept {
    return m && m->process;
}

// ===========================================================================
//  ChildProcess - POSIX
// ===========================================================================
#else

struct ChildProcess::Impl {
    pid_t pid = -1;
    int readFd = -1;
    std::mutex mutex;  // guards reaped / status against kill()
    bool reaped = false;
    int exitCode = -1;
    bool killRequested = false;
    bool escalated = false;
    Clock::time_point killTime{};
};

ChildProcess::ChildProcess()
    : m(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    if (!m) {
        return;
    }
    if (m->pid > 0) {
        int code = 0;
        if (!wait(0, code)) {
            // Straight to SIGKILL: nobody is left to wait for a grace period.
            ::kill(-m->pid, SIGKILL);
            wait(-1, code);
        }
    }
    if (m->readFd >= 0) {
        ::close(m->readFd);
        m->readFd = -1;
    }
}

bool ChildProcess::start(const fs::path& exe, const std::vector<std::string>& args, std::string* error) noexcept {
    const auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    try {
        if (!m || m->pid > 0) {
            return fail("a process was already started");
        }
        // ---- argv: the executable, then every argument verbatim (no shell) ------------
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(exe.string());
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& a : storage) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        // ---- the output pipe; neither end leaks into other children ---------------------
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            return fail(std::string("cannot create a pipe: ") + std::strerror(errno));
        }
        (void)::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        (void)::fcntl(fds[1], F_SETFD, FD_CLOEXEC);  // dup2 below clears it on 1 and 2

        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return fail("posix_spawn_file_actions_init failed");
        }
        (void)::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        (void)::posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        (void)::posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);

        // ---- its own process group, default signal handling ---------------------------------
        posix_spawnattr_t attr;
        if (::posix_spawnattr_init(&attr) != 0) {
            ::posix_spawn_file_actions_destroy(&actions);
            ::close(fds[0]);
            ::close(fds[1]);
            return fail("posix_spawnattr_init failed");
        }
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        sigaddset(&defaults, SIGTERM);
        sigaddset(&defaults, SIGINT);
        sigset_t noMask;
        sigemptyset(&noMask);
        (void)::posix_spawnattr_setsigdefault(&attr, &defaults);
        (void)::posix_spawnattr_setsigmask(&attr, &noMask);
        (void)::posix_spawnattr_setpgroup(&attr, 0);
        (void)::posix_spawnattr_setflags(
            &attr, static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));

        pid_t pid = -1;
        const int rc = ::posix_spawn(&pid, storage[0].c_str(), &actions, &attr, argv.data(), environ);
        ::posix_spawnattr_destroy(&attr);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);  // the child's copy is its stdout / stderr now
        if (rc != 0) {
            ::close(fds[0]);
            return fail("cannot start " + exe.filename().string() + ": " + std::strerror(rc));
        }
        m->pid = pid;
        m->readFd = fds[0];
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    } catch (...) {
        return fail("unexpected error starting the process");
    }
}

long ChildProcess::read(char* buffer, std::size_t size) noexcept {
    if (!m || m->readFd < 0 || !buffer || size == 0) {
        return -1;
    }
    for (;;) {
        const ssize_t got = ::read(m->readFd, buffer, size);
        if (got >= 0) {
            return static_cast<long>(got);
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

void ChildProcess::kill() noexcept {
    if (!m || m->pid <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m->mutex);
    if (m->reaped || m->killRequested) {
        return;
    }
    // The whole group: osvtool and the ffmpeg it started.  SIGTERM first so
    // ffmpeg can close its file; wait() escalates to SIGKILL if needed.
    ::kill(-m->pid, SIGTERM);
    m->killRequested = true;
    m->killTime = Clock::now();
}

bool ChildProcess::wait(int timeoutMs, int& exitCode) noexcept {
    if (!m || m->pid <= 0) {
        return false;
    }
    const auto start = Clock::now();
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            if (m->reaped) {
                exitCode = m->exitCode;
                return true;
            }
            int status = 0;
            const pid_t done = ::waitpid(m->pid, &status, WNOHANG);
            if (done == m->pid) {
                m->reaped = true;
                m->exitCode =
                    WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
                exitCode = m->exitCode;
                return true;
            }
            if (done < 0 && errno != EINTR) {
                m->reaped = true;  // ECHILD: somebody else reaped it
                m->exitCode = -1;
                exitCode = -1;
                return true;
            }
            // A child that ignores SIGTERM for 3 s gets SIGKILL.
            if (m->killRequested && !m->escalated && Clock::now() - m->killTime > std::chrono::seconds(3)) {
                ::kill(-m->pid, SIGKILL);
                m->escalated = true;
            }
        }
        if (timeoutMs >= 0 && Clock::now() - start >= std::chrono::milliseconds(timeoutMs)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool ChildProcess::started() const noexcept {
    return m && m->pid > 0;
}

#endif

// ===========================================================================
//  ProcessRunner
// ===========================================================================

ProcessRunner::ProcessRunner(std::function<void()> wake)
    : m_wake(std::move(wake)) {}

ProcessRunner::~ProcessRunner() {
    try {
        reset();
    } catch (...) {
        // Nothing sensible left to do in a destructor.
    }
}

bool ProcessRunner::start(const fs::path& exe, const std::vector<std::string>& args, std::string* error) {
    if (m_busy.load(std::memory_order_acquire)) {
        if (error) {
            *error = "a render is already running";
        }
        return false;
    }
    auto child = std::make_unique<ChildProcess>();
    if (!child->start(exe, args, error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = RunnerState{};
        m_state.running = true;
        m_lines.clear();
    }
    m_child = std::move(child);
    m_busy.store(true, std::memory_order_release);
    try {
        m_thread = std::thread([this] { worker(); });
    } catch (...) {
        // No thread: end the child right away and report it as a failure.
        m_child->kill();
        int code = 0;
        m_child->wait(5000, code);
        m_child.reset();
        m_busy.store(false, std::memory_order_release);
        if (error) {
            *error = "cannot start a worker thread";
        }
        return false;
    }
    return true;
}

void ProcessRunner::stop() noexcept {
    if (!m_busy.load(std::memory_order_acquire) || !m_child) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.stopRequested = true;
    }
    m_child->kill();
}

RunnerState ProcessRunner::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void ProcessRunner::takeLines(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lines.empty()) {
        return;
    }
    out.insert(out.end(), std::make_move_iterator(m_lines.begin()), std::make_move_iterator(m_lines.end()));
    m_lines.clear();
}

void ProcessRunner::reset() {
    if (m_thread.joinable()) {
        // Still running: ask it to end, then wait for the reader to see the
        // end of the stream.
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            finished = m_state.finished;
        }
        if (!finished && m_child) {
            m_child->kill();
        }
        m_thread.join();
    }
    m_child.reset();
    m_busy.store(false, std::memory_order_release);
}

void ProcessRunner::publish(std::vector<std::string>& lines) {
    if (lines.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::string& line : lines) {
            if (auto progress = parseProgressLine(line)) {
                m_state.progress = std::move(*progress);
                m_state.haveProgress = true;
            } else {
                if (auto message = errorMessage(line)) {
                    m_state.lastError = std::move(*message);
                }
                m_state.lastLine = line;
            }
            // The UI drains this every frame; the cap only matters if it
            // stops doing so (minimised for hours with a chatty tool).
            if (m_lines.size() < 200000) {
                m_lines.push_back(std::move(line));
            }
        }
    }
    lines.clear();
    if (m_wake) {
        m_wake();
    }
}

void ProcessRunner::worker() {
    int exitCode = -1;
    try {
        LineSplitter splitter;
        std::vector<std::string> lines;
        std::vector<char> buffer(16 * 1024);
        // ---- read until every writer (osvtool and its ffmpeg) is gone ---------------
        for (;;) {
            const long got = m_child->read(buffer.data(), buffer.size());
            if (got <= 0) {
                break;
            }
            splitter.feed(buffer.data(), static_cast<std::size_t>(got), lines);
            publish(lines);
        }
        splitter.finish(lines);
        publish(lines);
        // ---- then its exit code -----------------------------------------------------------
        if (!m_child->wait(-1, exitCode)) {
            exitCode = -1;
        }
    } catch (...) {
        // Out of memory while reading: end the child, report a failure.
        m_child->kill();
        int ignored = 0;
        m_child->wait(10000, ignored);
        exitCode = -1;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.running = false;
        m_state.finished = true;
        m_state.exitCode = exitCode;
    }
    if (m_wake) {
        m_wake();
    }
}

// ===========================================================================
//  runCapture
// ===========================================================================

CaptureResult runCapture(const fs::path& exe, const std::vector<std::string>& args, int timeoutMs,
                         std::size_t maxBytes) {
    CaptureResult result;
    ChildProcess child;
    if (!child.start(exe, args, &result.error)) {
        return result;
    }
    result.started = true;

    // The reader runs beside the wait, so a child writing more than the pipe
    // holds is never stuck waiting for us.
    std::thread reader([&child, &result, maxBytes] {
        std::vector<char> buffer(8192);
        for (;;) {
            const long got = child.read(buffer.data(), buffer.size());
            if (got <= 0) {
                break;
            }
            const std::size_t room = maxBytes > result.output.size() ? maxBytes - result.output.size() : 0;
            result.output.append(buffer.data(), std::min<std::size_t>(room, static_cast<std::size_t>(got)));
        }
    });

    int code = -1;
    if (!child.wait(timeoutMs, code)) {
        result.timedOut = true;
        child.kill();
        child.wait(10000, code);
    } else {
        // It exited; anything it left behind holding the pipe open would
        // keep the reader waiting forever, so the tree is ended too.
        child.kill();
    }
    reader.join();
    result.exitCode = code;
    return result;
}

}  // namespace osvgui
