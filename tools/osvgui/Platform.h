// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Platform.h - the few things OpenOSV Studio asks of the operating system:
// native open dialogs, finding executables, opening a link or a folder,
// keeping the machine awake during a batch, taskbar progress and a title
// bar that matches the theme.
//
// Implemented in Platform.cpp (Windows, and the generic POSIX fallbacks)
// and Platform_mac.mm (AppKit).  Every function is noexcept and degrades to
// "nothing happened" when the system refuses.  `nativeWindow` is the HWND
// on Windows and the NSWindow* on macOS (glfwGetWin32Window /
// glfwGetCocoaWindow), or null.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osvgui::platform {

/// Once at start-up / shut-down, on the UI thread (COM on Windows).
void initialise() noexcept;
void shutdown() noexcept;

// ---- executables -------------------------------------------------------------

/// This program's own executable (empty if the system will not say).
[[nodiscard]] std::filesystem::path executablePath() noexcept;

/// `name` (".exe" added on Windows) found through PATH, or empty.
[[nodiscard]] std::filesystem::path findOnPath(std::string_view name) noexcept;

/// `name` next to this program first, then through PATH; empty if neither.
[[nodiscard]] std::filesystem::path findTool(std::string_view name) noexcept;

/// Places ffmpeg is commonly installed that a GUI app's PATH may not
/// include (Homebrew on macOS: a Finder-launched app does not get the
/// shell's PATH).  Only existing files are returned.
[[nodiscard]] std::vector<std::filesystem::path> extraFfmpegLocations() noexcept;

/// True for an existing regular file (and, on POSIX, executable).
[[nodiscard]] bool isExecutableFile(const std::filesystem::path& path) noexcept;

// ---- dialogs (modal; call from the UI thread) ----------------------------------

/// Pick one or more .OSV clips.  Empty when cancelled.
[[nodiscard]] std::vector<std::filesystem::path> chooseClips(void* nativeWindow) noexcept;

/// Pick folders.  `multiple` allows several.  Empty when cancelled.
[[nodiscard]] std::vector<std::filesystem::path> chooseFolders(void* nativeWindow, const char* title,
                                                               bool multiple) noexcept;

/// Pick a program (ffmpeg).  nullopt when cancelled.
[[nodiscard]] std::optional<std::filesystem::path> chooseProgram(void* nativeWindow, const char* title) noexcept;

/// True where chooseClips / chooseFolders exist (Windows, macOS).
[[nodiscard]] bool haveDialogs() noexcept;

// ---- the shell ---------------------------------------------------------------------

/// Open a web link in the default browser.
void openUrl(const std::string& url) noexcept;

/// Show `path` selected in Explorer / Finder (its folder when it is gone).
void revealInFileManager(const std::filesystem::path& path) noexcept;

/// Open `path` with its default application.
void openWithDefaultApp(const std::filesystem::path& path) noexcept;

// ---- the session -------------------------------------------------------------------

/// Keep the machine from sleeping while `awake` (a batch is rendering);
/// the display may still turn off.
void keepAwake(bool awake) noexcept;

/// Taskbar button progress: `fraction` in 0..1, negative clears it.
/// `paused` / `error` tint it.  Windows only; a no-op elsewhere.
void setTaskbarProgress(void* nativeWindow, double fraction, bool paused, bool error) noexcept;

/// Title bar in dark or light mode, its caption in `captionRgb`
/// (0xRRGGBB) where the system allows.  Windows only; a no-op elsewhere.
void styleTitleBar(void* nativeWindow, bool dark, unsigned captionRgb) noexcept;

}  // namespace osvgui::platform
