// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxFileDialog.cpp - the Windows Open dialog behind "Choose .OSV File...",
// and the path clean-up both platforms share.  The macOS panel is in
// OfxFileDialogMac.mm.

#include "OfxFileDialog.h"

#include "PluginLog.h"

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>

#include <filesystem>
#include <vector>
#endif  // _WIN32

namespace osv::ofx {

#if defined(_WIN32)
using osv::premiere::PluginLog;

namespace {

/// UTF-8 -> UTF-16; empty on any conversion failure.
[[nodiscard]] std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), n);
    return wide;
}

/// UTF-16 -> UTF-8; empty on any conversion failure.
[[nodiscard]] std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), utf8.data(), n,
                          nullptr, nullptr);
    return utf8;
}

}  // namespace
#endif  // _WIN32

std::string cleanPath(std::string text) {
    // Whitespace first, then one level of matching quotes, then whitespace
    // that was inside them.
    const auto trim = [](std::string& s) {
        const char* ws = " \t\r\n";
        const std::size_t first = s.find_first_not_of(ws);
        if (first == std::string::npos) {
            s.clear();
            return;
        }
        const std::size_t last = s.find_last_not_of(ws);
        s = s.substr(first, last - first + 1);
    };
    trim(text);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
        trim(text);
    }
    return text;
}

#if defined(_WIN32)
std::optional<std::string> chooseOsvFile(const std::string& startPath) noexcept {
    try {
        // Room for any path Windows can hand back (long paths included).
        std::vector<wchar_t> buffer(32768, L'\0');

        // Start in the folder of the current clip, when there is one.
        std::wstring initialDir;
        const std::wstring current = widen(cleanPath(startPath));
        if (!current.empty()) {
            std::error_code ec;
            const std::filesystem::path p(current);
            const std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path{};
            if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
                initialDir = dir.wstring();
            }
        }

        // Pairs of (label, pattern), double-NUL terminated.
        static const wchar_t kFilter[] =
            L"DJI Osmo 360 clips (*.OSV, *.LRF)\0*.osv;*.lrf\0"
            L"All files (*.*)\0*.*\0";

        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        // The host's active window owns the dialog, so it stays on top of
        // the host and blocks it like any modal dialog.
        ofn.hwndOwner = ::GetActiveWindow() ? ::GetActiveWindow() : ::GetForegroundWindow();
        ofn.lpstrFilter = kFilter;
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle = L"Choose a DJI Osmo 360 clip";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

        if (!::GetOpenFileNameW(&ofn)) {
            const DWORD err = ::CommDlgExtendedError();
            if (err != 0) {
                PluginLog::warn("ofx source: the Open dialog failed (CommDlgExtendedError {})", err);
            }
            return std::nullopt;  // cancelled, or failed and logged
        }
        std::string chosen = narrow(std::wstring(buffer.data()));
        if (chosen.empty()) {
            return std::nullopt;
        }
        return chosen;
    } catch (...) {
        PluginLog::warn("ofx source: exception while showing the Open dialog");
        return std::nullopt;
    }
}
#endif  // _WIN32

}  // namespace osv::ofx
