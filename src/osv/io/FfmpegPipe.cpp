// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/io/FfmpegPipe.h"
#include "osv/core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// POSIX: the child is started with posix_spawnp and fed through a pipe.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
#endif

namespace osv::io {

namespace {

/// Quote one argument for the Windows command line (CommandLineToArgvW rules).
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    unsigned backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring widen(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string narrow(const std::wstring& s) {
#if defined(_WIN32)
    if (s.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
    return out;
#else
    return std::string(s.begin(), s.end());
#endif
}

#if !defined(_WIN32)
/// Quote one argument the way a POSIX shell would read it back.  Only used
/// for the logged command line; the child itself receives argv verbatim.
std::string shellQuote(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n'\"\\$`*?[]{}()<>|&;#~") == std::string::npos) {
        return arg;
    }
    std::string out = "'";
    for (const char c : arg) {
        if (c == '\'') {
            // Close the quote, emit an escaped quote, reopen.
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

/// Wait up to `timeoutMs` for `pid` to exit.  Returns true (and the raw wait
/// status) when it did; false while it is still running.
bool waitForExit(pid_t pid, unsigned timeoutMs, int* status) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (status) {
                *status = st;
            }
            return true;
        }
        if (r < 0 && errno != EINTR) {
            // ECHILD: somebody else reaped it, so it is gone either way.
            if (status) {
                *status = 0;
            }
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/// Exit code of a wait status: the process's own code, 128 + the signal for a
/// process that was killed (the shell convention), -1 when neither applies.
int exitCodeOf(int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

/// Search PATH for an executable file called `name` - what SearchPathW does
/// on Windows.  Empty when there is none.
std::filesystem::path searchPath(const char* name) {
    if (!name || !*name) {
        return {};
    }
    const char* path = std::getenv("PATH");
    if (!path || !*path) {
        return {};
    }
    const std::string all(path);
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = all.find(':', start);
        const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::filesystem::path candidate = std::filesystem::path(dir) / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}
#endif

/// Build the ffmpeg argument vector for the given codec.
std::vector<std::string> buildArgs(const FfmpegPipeOptions& o, const std::string& codec,
                                   const std::filesystem::path& out) {
    std::vector<std::string> a;
    a.push_back("-hide_banner");
    a.push_back("-loglevel");
    a.push_back("error");
    a.push_back("-y");
    // Input 0: raw frames on stdin.
    a.push_back("-f");
    a.push_back("rawvideo");
    a.push_back("-pix_fmt");
    a.push_back("rgb48le");
    a.push_back("-s");
    a.push_back(std::to_string(o.width) + "x" + std::to_string(o.height));
    a.push_back("-r");
    {
        std::ostringstream fps;
        fps.precision(6);
        fps << std::fixed << o.fps;
        a.push_back(fps.str());
    }
    a.push_back("-i");
    a.push_back("-");
    // Input 1: audio source (optional).
    const bool audio = !o.audioSource.empty();
    if (audio) {
        a.push_back("-i");
        a.push_back(o.audioSource.string());
        a.push_back("-map");
        a.push_back("0:v:0");
        a.push_back("-map");
        a.push_back("1:a:0?");
        a.push_back("-c:a");
        a.push_back("copy");
        a.push_back("-shortest");
    }
    a.push_back("-c:v");
    a.push_back(codec);
    a.push_back("-pix_fmt");
    a.push_back(o.pixFmt);
    // Quality knobs differ per encoder family.
    if (codec.find("nvenc") != std::string::npos) {
        a.push_back("-preset");
        a.push_back("p5");
        a.push_back("-rc");
        a.push_back("vbr");
        a.push_back("-cq");
        a.push_back(std::to_string(o.crf));
        a.push_back("-b:v");
        a.push_back("0");
        a.push_back("-profile:v");
        a.push_back("main10");
    } else if (codec == "libx265") {
        a.push_back("-crf");
        a.push_back(std::to_string(o.crf));
        a.push_back("-preset");
        a.push_back("medium");
        a.push_back("-x265-params");
        std::string params = "profile=main10";
        if (o.transfer == PipeTransfer::HLG) {
            params += ":colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc";
        } else if (o.transfer == PipeTransfer::PQ) {
            params += ":colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc";
        }
        a.push_back(params);
    } else {
        a.push_back("-crf");
        a.push_back(std::to_string(o.crf));
    }
    // Colour signalling.  Some ffmpeg builds ignore -color_primaries /
    // -color_trc as plain output options, so the setparams filter stamps the
    // frames as well; both together give a correct VUI and colr box.
    if (o.transfer == PipeTransfer::Rec709) {
        a.push_back("-vf");
        a.push_back("setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709");
    } else {
        a.push_back("-vf");
        a.push_back(std::string("setparams=color_primaries=bt2020:color_trc=") +
                    (o.transfer == PipeTransfer::HLG ? "arib-std-b67" : "smpte2084") + ":colorspace=bt2020nc");
    }
    if (o.transfer == PipeTransfer::Rec709) {
        a.push_back("-color_primaries");
        a.push_back("bt709");
        a.push_back("-color_trc");
        a.push_back("bt709");
        a.push_back("-colorspace");
        a.push_back("bt709");
    } else {
        a.push_back("-color_primaries");
        a.push_back("bt2020");
        a.push_back("-color_trc");
        a.push_back(o.transfer == PipeTransfer::HLG ? "arib-std-b67" : "smpte2084");
        a.push_back("-colorspace");
        a.push_back("bt2020nc");
    }
    a.push_back("-color_range");
    a.push_back("tv");
    a.push_back("-movflags");
    a.push_back("+write_colr+faststart");
    a.push_back("-tag:v");
    a.push_back("hvc1");
    for (const std::string& extra : o.extraArgs) {
        a.push_back(extra);
    }
    a.push_back(out.string());
    return a;
}

}  // namespace

struct FfmpegPipeWriter::Impl {
    std::string commandLine;
    std::uint64_t frames = 0;
    std::vector<std::uint8_t> rowBuffer;
    bool closed = false;
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
#else
    pid_t process = -1;   ///< The ffmpeg child, -1 when there is none.
    int stdinWrite = -1;  ///< Our end of its stdin pipe, -1 when closed.
#endif

    /// True while the pipe into the child is open.
    [[nodiscard]] bool pipeOpen() const noexcept {
#if defined(_WIN32)
        return stdinWrite != nullptr;
#else
        return stdinWrite >= 0;
#endif
    }

    ~Impl() { terminate(); }

    void terminate() noexcept {
#if defined(_WIN32)
        if (stdinWrite) {
            CloseHandle(stdinWrite);
            stdinWrite = nullptr;
        }
        if (process) {
            // Give ffmpeg a moment to finish on its own before killing it.
            if (WaitForSingleObject(process, 2000) != WAIT_OBJECT_0) {
                TerminateProcess(process, 1);
            }
            CloseHandle(process);
            process = nullptr;
        }
#else
        if (stdinWrite >= 0) {
            ::close(stdinWrite);
            stdinWrite = -1;
        }
        if (process > 0) {
            // The same grace period as on Windows, then SIGKILL - and reap it
            // either way so no zombie is left behind.
            int status = 0;
            if (!waitForExit(process, 2000, &status)) {
                ::kill(process, SIGKILL);
                (void)waitForExit(process, 2000, &status);
            }
            process = -1;
        }
#endif
    }

    /// Start the process with `args`; returns false when it could not be
    /// created or died within `probeMs` milliseconds (bad codec etc.).
    bool start(const std::filesystem::path& exe, const std::vector<std::string>& args, unsigned probeMs,
               std::string* error) {
#if defined(_WIN32)
        std::wstring cmd = quoteArg(exe.wstring());
        for (const std::string& arg : args) {
            cmd.push_back(L' ');
            cmd.append(quoteArg(widen(arg)));
        }
        commandLine = narrow(cmd);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) {
            if (error) {
                *error = "CreatePipe failed";
            }
            return false;
        }
        // Our write end must not be inherited by the child.
        SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = readEnd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi);
        CloseHandle(readEnd);  // child holds its own copy now
        if (!ok) {
            CloseHandle(writeEnd);
            if (error) {
                *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
            }
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        stdinWrite = writeEnd;

        // Probe: a bad codec makes ffmpeg exit almost immediately.
        if (WaitForSingleObject(process, probeMs) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process, &code);
            CloseHandle(stdinWrite);
            stdinWrite = nullptr;
            CloseHandle(process);
            process = nullptr;
            if (error) {
                *error = "ffmpeg exited early with code " + std::to_string(code);
            }
            return false;
        }
        return true;
#else
        // argv: the executable, then every argument verbatim (no shell).
        commandLine = shellQuote(exe.string());
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(exe.string());
        for (const std::string& arg : args) {
            storage.push_back(arg);
            commandLine.push_back(' ');
            commandLine.append(shellQuote(arg));
        }
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& a : storage) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            if (error) {
                *error = std::string("pipe failed: ") + std::strerror(errno);
            }
            return false;
        }
        // Our write end must not leak into the child (or into any other child
        // the process starts later); the read end becomes the child's stdin.
        (void)::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#if defined(F_SETNOSIGPIPE)
        // macOS: a write into a pipe whose reader died fails with EPIPE
        // instead of raising SIGPIPE, which would kill the whole process.
        (void)::fcntl(fds[1], F_SETNOSIGPIPE, 1);
#endif

        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            if (error) {
                *error = "posix_spawn_file_actions_init failed";
            }
            return false;
        }
        (void)::posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
        (void)::posix_spawn_file_actions_addclose(&actions, fds[0]);
        (void)::posix_spawn_file_actions_addclose(&actions, fds[1]);

        // posix_spawnp searches PATH for a bare "ffmpeg", like CreateProcessW.
        pid_t pid = -1;
        const int rc = ::posix_spawnp(&pid, storage.front().c_str(), &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(fds[0]);  // the child holds its own copy now
        if (rc != 0) {
            ::close(fds[1]);
            if (error) {
                *error = std::string("posix_spawnp failed: ") + std::strerror(rc);
            }
            return false;
        }
        process = pid;
        stdinWrite = fds[1];

        // Probe: a bad codec makes ffmpeg exit almost immediately.
        int status = 0;
        if (waitForExit(process, probeMs, &status)) {
            ::close(stdinWrite);
            stdinWrite = -1;
            process = -1;
            if (error) {
                *error = "ffmpeg exited early with code " + std::to_string(exitCodeOf(status));
            }
            return false;
        }
        return true;
#endif
    }

    bool writeAll(const std::uint8_t* data, std::size_t bytes) noexcept {
#if defined(_WIN32)
        while (bytes > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 20));
            if (!WriteFile(stdinWrite, data, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            data += written;
            bytes -= written;
        }
        return true;
#else
        if (stdinWrite < 0 || (!data && bytes > 0)) {
            return false;
        }
#if !defined(F_SETNOSIGPIPE)
        // No per-descriptor opt-out on this system: block SIGPIPE on this
        // thread while writing and swallow the one a failed write raised, so
        // a dead encoder is a failed write instead of a killed process.
        sigset_t pipeSet;
        sigset_t previous;
        sigemptyset(&pipeSet);
        sigaddset(&pipeSet, SIGPIPE);
        const bool masked = ::pthread_sigmask(SIG_BLOCK, &pipeSet, &previous) == 0;
#endif
        bool ok = true;
        while (bytes > 0) {
            const std::size_t chunk = std::min<std::size_t>(bytes, 1u << 20);
            const ssize_t written = ::write(stdinWrite, data, chunk);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                ok = false;
                break;
            }
            data += written;
            bytes -= static_cast<std::size_t>(written);
        }
#if !defined(F_SETNOSIGPIPE)
        if (masked) {
            if (!ok && errno == EPIPE) {
                const timespec zero{0, 0};
                (void)::sigtimedwait(&pipeSet, nullptr, &zero);
            }
            (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
#endif
        return ok;
#endif
    }
};

FfmpegPipeWriter::FfmpegPipeWriter() : m_impl(std::make_unique<Impl>()) {}
FfmpegPipeWriter::~FfmpegPipeWriter() = default;
FfmpegPipeWriter::FfmpegPipeWriter(FfmpegPipeWriter&&) noexcept = default;
FfmpegPipeWriter& FfmpegPipeWriter::operator=(FfmpegPipeWriter&&) noexcept = default;

std::filesystem::path FfmpegPipeWriter::resolveExecutable(const std::filesystem::path& preferred) {
    if (!preferred.empty()) {
        return preferred;
    }
    if (const char* env = std::getenv("OSV_FFMPEG_EXE")) {
        if (*env) {
            return std::filesystem::path(env);
        }
    }
#if defined(_WIN32)
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"ffmpeg", L".exe", MAX_PATH, found, nullptr) > 0) {
        return std::filesystem::path(found);
    }
#else
    if (std::filesystem::path found = searchPath("ffmpeg"); !found.empty()) {
        return found;
    }
#endif
    return std::filesystem::path("ffmpeg");
}

Result<FfmpegPipeWriter> FfmpegPipeWriter::open(const FfmpegPipeOptions& options, const std::filesystem::path& out) {
    if (options.width == 0 || options.height == 0 || options.fps <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "FfmpegPipeWriter: invalid size or fps"};
    }
    const std::filesystem::path exe = resolveExecutable(options.ffmpegExe);
    FfmpegPipeWriter w;
    std::string error;
    const auto args = buildArgs(options, options.codec, out);
    if (w.m_impl->start(exe, args, 1500, &error)) {
        log::info("ffmpeg: {}", w.m_impl->commandLine);
        return w;
    }
    log::warn("ffmpeg with {} failed ({}), retrying with {}", options.codec, error, options.fallbackCodec);
    if (!options.fallbackCodec.empty() && options.fallbackCodec != options.codec) {
        const auto fallback = buildArgs(options, options.fallbackCodec, out);
        if (w.m_impl->start(exe, fallback, 1500, &error)) {
            log::info("ffmpeg: {}", w.m_impl->commandLine);
            return w;
        }
    }
    return Error{ErrorCode::Io, "cannot start ffmpeg (" + exe.string() + "): " + error};
}

Status FfmpegPipeWriter::writeFrame(const render::ImageRGBAf& image) {
    Impl& impl = *m_impl;
    if (!impl.pipeOpen()) {
        return failStatus(ErrorCode::Io, "ffmpeg pipe is not open");
    }
    if (!image.valid()) {
        return failStatus(ErrorCode::InvalidArgument, "writeFrame: invalid image");
    }
    // Convert one row at a time to rgb48le (3 x uint16 little endian).
    const std::size_t rowBytes = static_cast<std::size_t>(image.w) * 3 * 2;
    impl.rowBuffer.resize(rowBytes);
    for (std::uint32_t y = 0; y < image.h; ++y) {
        const float* src = image.row(y);
        std::uint8_t* dst = impl.rowBuffer.data();
        for (std::uint32_t x = 0; x < image.w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = src[x * 4 + c];
                std::uint32_t q = 0;
                if (v > 0.0f) {
                    q = v >= 1.0f ? 65535u : static_cast<std::uint32_t>(v * 65535.0f + 0.5f);
                }
                dst[(x * 3 + c) * 2] = static_cast<std::uint8_t>(q & 0xFF);
                dst[(x * 3 + c) * 2 + 1] = static_cast<std::uint8_t>(q >> 8);
            }
        }
        if (!impl.writeAll(impl.rowBuffer.data(), rowBytes)) {
            return failStatus(ErrorCode::Io, "ffmpeg pipe write failed (encoder exited?)");
        }
    }
    ++impl.frames;
    return okStatus();
}

Status FfmpegPipeWriter::close() {
    Impl& impl = *m_impl;
    if (impl.closed) {
        return okStatus();
    }
    impl.closed = true;
#if defined(_WIN32)
    if (impl.stdinWrite) {
        CloseHandle(impl.stdinWrite);
        impl.stdinWrite = nullptr;
    }
    if (impl.process) {
        WaitForSingleObject(impl.process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(impl.process, &code);
        CloseHandle(impl.process);
        impl.process = nullptr;
        if (code != 0) {
            return failStatus(ErrorCode::Io, "ffmpeg exited with code " + std::to_string(code));
        }
    }
#else
    if (impl.stdinWrite >= 0) {
        ::close(impl.stdinWrite);
        impl.stdinWrite = -1;
    }
    if (impl.process > 0) {
        // A blocking wait, as WaitForSingleObject(INFINITE) above: the encoder
        // is flushing its last frames and writing the moov atom.
        int status = 0;
        pid_t r = -1;
        do {
            r = ::waitpid(impl.process, &status, 0);
        } while (r < 0 && errno == EINTR);
        impl.process = -1;
        const int code = r < 0 ? 0 : exitCodeOf(status);
        if (code != 0) {
            return failStatus(ErrorCode::Io, "ffmpeg exited with code " + std::to_string(code));
        }
    }
#endif
    return okStatus();
}

const std::string& FfmpegPipeWriter::commandLine() const noexcept { return m_impl->commandLine; }

std::uint64_t FfmpegPipeWriter::framesWritten() const noexcept { return m_impl->frames; }

}  // namespace osv::io
