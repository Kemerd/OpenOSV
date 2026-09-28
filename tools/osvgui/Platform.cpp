// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Platform.cpp - Windows, plus the POSIX pieces every other system shares
// (see Platform.h).  The AppKit parts of macOS live in Platform_mac.mm.

#include "Platform.h"

#include "Queue.h"  // pathToUtf8 / pathFromUtf8

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <climits>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace osvgui::platform {

namespace fs = std::filesystem;

// ===========================================================================
//  Windows
// ===========================================================================
#if defined(_WIN32)

namespace {

/// COM was initialised by initialise() and must be released by shutdown().
bool g_comInitialised = false;

/// The taskbar interface, created on first use (after the window exists).
ITaskbarList3* g_taskbar = nullptr;
bool g_taskbarTried = false;

/// UTF-8 -> UTF-16.
[[nodiscard]] std::wstring widen(std::string_view text) {
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

/// An environment variable as a path, empty when unset.
[[nodiscard]] fs::path environmentPath(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (got == 0 || got >= needed) {
        return {};
    }
    value.resize(got);
    return fs::path(value);
}

/// Releases a COM interface when it goes out of scope.
template <typename T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) {
            p->Release();
        }
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

/// The common IFileOpenDialog: files or folders, one or many.
[[nodiscard]] std::vector<fs::path> openDialog(void* owner, bool folders, bool multiple, const wchar_t* title,
                                               const COMDLG_FILTERSPEC* filters, UINT filterCount) noexcept {
    std::vector<fs::path> out;
    try {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(
                ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) ||
            !dialog) {
            return out;
        }
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
        options |= folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
        if (multiple) {
            options |= FOS_ALLOWMULTISELECT;
        }
        dialog->SetOptions(options);
        if (title) {
            dialog->SetTitle(title);
        }
        if (!folders && filters && filterCount > 0) {
            dialog->SetFileTypes(filterCount, filters);
            dialog->SetFileTypeIndex(1);
        }
        // Modal to our window; cancelling returns an error HRESULT.
        if (FAILED(dialog->Show(static_cast<HWND>(owner)))) {
            return out;
        }
        ComPtr<IShellItemArray> items;
        if (FAILED(dialog->GetResults(items.put())) || !items) {
            return out;
        }
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, item.put())) || !item) {
                continue;
            }
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                out.emplace_back(std::wstring(name));
                ::CoTaskMemFree(name);
            }
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

}  // namespace

void initialise() noexcept {
    // Apartment-threaded, as the shell's dialogs require on this thread.
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_comInitialised = SUCCEEDED(hr);
}

void shutdown() noexcept {
    if (g_taskbar) {
        g_taskbar->Release();
        g_taskbar = nullptr;
    }
    keepAwake(false);
    if (g_comInitialised) {
        ::CoUninitialize();
        g_comInitialised = false;
    }
}

fs::path executablePath() noexcept {
    try {
        std::wstring buffer(512, L'\0');
        for (int attempt = 0; attempt < 8; ++attempt) {
            const DWORD got = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (got == 0) {
                return {};
            }
            if (got < buffer.size()) {
                buffer.resize(got);
                return fs::path(buffer);
            }
            buffer.resize(buffer.size() * 2);  // truncated: a longer path
        }
    } catch (...) {
    }
    return {};
}

fs::path findOnPath(std::string_view name) noexcept {
    try {
        // SearchPathW, as osvtool itself looks for ffmpeg: what is found here
        // is what osvtool will find.
        const std::wstring wide = widen(name);
        if (wide.empty()) {
            return {};
        }
        std::wstring buffer(MAX_PATH, L'\0');
        for (int attempt = 0; attempt < 4; ++attempt) {
            const DWORD got = ::SearchPathW(nullptr, wide.c_str(), L".exe", static_cast<DWORD>(buffer.size()),
                                            buffer.data(), nullptr);
            if (got == 0) {
                return {};
            }
            if (got < buffer.size()) {
                buffer.resize(got);
                return fs::path(buffer);
            }
            buffer.resize(static_cast<std::size_t>(got) + 1);
        }
    } catch (...) {
    }
    return {};
}

std::vector<fs::path> extraFfmpegLocations() noexcept {
    std::vector<fs::path> out;
    try {
        // Package managers' shim folders: on PATH for a fresh shell, but not
        // for a program Explorer started before the install changed PATH.
        std::vector<fs::path> candidates;
        if (const fs::path local = environmentPath(L"LOCALAPPDATA"); !local.empty()) {
            candidates.push_back(local / L"Microsoft" / L"WinGet" / L"Links" / L"ffmpeg.exe");
        }
        if (const fs::path profile = environmentPath(L"USERPROFILE"); !profile.empty()) {
            candidates.push_back(profile / L"scoop" / L"shims" / L"ffmpeg.exe");
        }
        if (const fs::path programData = environmentPath(L"ProgramData"); !programData.empty()) {
            candidates.push_back(programData / L"chocolatey" / L"bin" / L"ffmpeg.exe");
        }
        candidates.emplace_back(L"C:\\ffmpeg\\bin\\ffmpeg.exe");
        for (const fs::path& candidate : candidates) {
            if (isExecutableFile(candidate)) {
                out.push_back(candidate);
            }
        }
    } catch (...) {
    }
    return out;
}

bool isExecutableFile(const fs::path& path) noexcept {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec) && !ec;
}

std::vector<fs::path> chooseClips(void* nativeWindow) noexcept {
    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"DJI Osmo 360 clips (*.OSV)", L"*.osv"},
        {L"All files", L"*.*"},
    };
    return openDialog(nativeWindow, false, true, L"Add clips", kFilters, 2);
}

std::vector<fs::path> chooseFolders(void* nativeWindow, const char* title, bool multiple) noexcept {
    const std::wstring wideTitle = title ? widen(title) : std::wstring();
    return openDialog(nativeWindow, true, multiple, wideTitle.empty() ? nullptr : wideTitle.c_str(), nullptr, 0);
}

std::optional<fs::path> chooseProgram(void* nativeWindow, const char* title) noexcept {
    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"Programs (*.exe)", L"*.exe"},
        {L"All files", L"*.*"},
    };
    const std::wstring wideTitle = title ? widen(title) : std::wstring();
    auto picked = openDialog(nativeWindow, false, false, wideTitle.empty() ? nullptr : wideTitle.c_str(), kFilters, 2);
    if (picked.empty()) {
        return std::nullopt;
    }
    return picked.front();
}

bool haveDialogs() noexcept {
    return true;
}

void openUrl(const std::string& url) noexcept {
    try {
        const std::wstring wide = widen(url);
        if (!wide.empty()) {
            ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } catch (...) {
    }
}

void revealInFileManager(const fs::path& path) noexcept {
    try {
        std::error_code ec;
        if (fs::exists(path, ec)) {
            // Explorer with the file selected, reusing an open window of that
            // folder when there is one.
            PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(path.c_str());
            if (pidl) {
                ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ::ILFree(pidl);
                return;
            }
        }
        const fs::path folder = path.parent_path();
        if (!folder.empty() && fs::is_directory(folder, ec)) {
            ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } catch (...) {
    }
}

void openWithDefaultApp(const fs::path& path) noexcept {
    try {
        ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } catch (...) {
    }
}

void keepAwake(bool awake) noexcept {
    // Per thread: always called from the UI thread.  The display may sleep;
    // the system may not, or a long batch would stop overnight.
    ::SetThreadExecutionState(awake ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

void setTaskbarProgress(void* nativeWindow, double fraction, bool paused, bool error) noexcept {
    if (!nativeWindow) {
        return;
    }
    if (!g_taskbar && !g_taskbarTried) {
        g_taskbarTried = true;
        ITaskbarList3* list = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))) &&
            list) {
            if (SUCCEEDED(list->HrInit())) {
                g_taskbar = list;
            } else {
                list->Release();
            }
        }
    }
    if (!g_taskbar) {
        return;
    }
    const HWND hwnd = static_cast<HWND>(nativeWindow);
    if (fraction < 0.0) {
        g_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
        return;
    }
    const double clamped = fraction > 1.0 ? 1.0 : fraction;
    g_taskbar->SetProgressState(hwnd, error ? TBPF_ERROR : (paused ? TBPF_PAUSED : TBPF_NORMAL));
    g_taskbar->SetProgressValue(hwnd, static_cast<ULONGLONG>(clamped * 1000.0), 1000);
}

void styleTitleBar(void* nativeWindow, bool dark, unsigned captionRgb) noexcept {
    if (!nativeWindow) {
        return;
    }
    const HWND hwnd = static_cast<HWND>(nativeWindow);
    // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 on Windows 10 20H1 and later, 19
    // before; an unknown attribute simply fails.
    const BOOL useDark = dark ? TRUE : FALSE;
    if (FAILED(::DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
        ::DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
    }
    // DWMWA_CAPTION_COLOR (35): Windows 11 paints the title bar in the
    // window's own background colour, so it reads as one surface.
    const COLORREF caption = RGB((captionRgb >> 16) & 0xFF, (captionRgb >> 8) & 0xFF, captionRgb & 0xFF);
    ::DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
}

// ===========================================================================
//  POSIX (macOS and the rest)
// ===========================================================================
#else

void initialise() noexcept {}

void shutdown() noexcept {
    keepAwake(false);
}

fs::path executablePath() noexcept {
    try {
#if defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
        buffer.resize(std::char_traits<char>::length(buffer.c_str()));
        char resolved[PATH_MAX] = {};
        if (::realpath(buffer.c_str(), resolved)) {
            return fs::path(resolved);
        }
        return fs::path(buffer);
#else
        std::error_code ec;
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::path() : self;
#endif
    } catch (...) {
        return {};
    }
}

bool isExecutableFile(const fs::path& path) noexcept {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec) && !ec && ::access(path.c_str(), X_OK) == 0;
}

fs::path findOnPath(std::string_view name) noexcept {
    try {
        const char* path = std::getenv("PATH");
        if (!path || !*path || name.empty()) {
            return {};
        }
        const std::string all(path);
        std::size_t start = 0;
        for (;;) {
            const std::size_t end = all.find(':', start);
            const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty()) {
                const fs::path candidate = fs::path(dir) / std::string(name);
                if (isExecutableFile(candidate)) {
                    return candidate;
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    } catch (...) {
    }
    return {};
}

std::vector<fs::path> extraFfmpegLocations() noexcept {
    std::vector<fs::path> out;
    try {
        // Homebrew (Apple Silicon, then Intel), MacPorts, the system.
        for (const char* candidate :
             {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/opt/local/bin/ffmpeg", "/usr/bin/ffmpeg"}) {
            if (isExecutableFile(candidate)) {
                out.emplace_back(candidate);
            }
        }
    } catch (...) {
    }
    return out;
}

void setTaskbarProgress(void*, double, bool, bool) noexcept {}

void styleTitleBar(void*, bool, unsigned) noexcept {}

#if !defined(__APPLE__)
// ---- no native dialogs outside Windows and macOS: drag and drop instead -------

std::vector<fs::path> chooseClips(void*) noexcept {
    return {};
}

std::vector<fs::path> chooseFolders(void*, const char*, bool) noexcept {
    return {};
}

std::optional<fs::path> chooseProgram(void*, const char*) noexcept {
    return std::nullopt;
}

bool haveDialogs() noexcept {
    return false;
}

namespace {
/// Start `xdg-open <target>` and forget it (reaped right away by a double
/// fork would be tidier; xdg-open exits quickly and is reaped here).
void xdgOpen(const std::string& target) noexcept {
    try {
        std::string program = "xdg-open";
        std::string argument = target;
        char* argv[] = {program.data(), argument.data(), nullptr};
        pid_t pid = -1;
        if (::posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0 && pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    } catch (...) {
    }
}
}  // namespace

void openUrl(const std::string& url) noexcept {
    xdgOpen(url);
}

void revealInFileManager(const fs::path& path) noexcept {
    xdgOpen(path.parent_path().string());
}

void openWithDefaultApp(const fs::path& path) noexcept {
    xdgOpen(path.string());
}

void keepAwake(bool) noexcept {}
#endif  // !__APPLE__

#endif  // _WIN32

// ===========================================================================
//  Everywhere
// ===========================================================================

fs::path findTool(std::string_view name) noexcept {
    try {
        // Beside this program first: the release zip puts osvgui and
        // osvtool in the same folder, and a build tree does too.
        const fs::path self = executablePath();
        if (!self.empty()) {
#if defined(_WIN32)
            const fs::path candidate = self.parent_path() / pathFromUtf8(std::string(name) + ".exe");
#else
            const fs::path candidate = self.parent_path() / pathFromUtf8(name);
#endif
            if (isExecutableFile(candidate)) {
                return candidate;
            }
        }
        return findOnPath(name);
    } catch (...) {
        return {};
    }
}

}  // namespace osvgui::platform
