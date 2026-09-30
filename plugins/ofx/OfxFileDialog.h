// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxFileDialog.h - the "Choose .OSV File..." button's file dialog.
//
// DaVinci Resolve shows an OpenFX file-path parameter as a plain text field
// with no Browse button, so pasting a path would be the only way in.  The
// OpenOSV Source generator therefore carries a push button whose
// kOfxActionInstanceChanged opens the standard Windows Open dialog (the
// macOS Open panel on a Mac) and writes the chosen path into the file
// parameter - the pattern shipping Resolve plug-ins use for the same gap.
//
// VEGAS Pro gives a file-path parameter its own Browse button, so there the
// generator hides ours (OfxSource.cpp).  Should the dialog open under VEGAS
// anyway, VEGAS's main window - its "OfxPropVegasHostHWnd" host property -
// owns it, so it stays in front of VEGAS and blocks it like VEGAS's own.
#pragma once

#include <optional>
#include <string>

namespace osv::ofx {

/// Show a modal Open dialog for .OSV / .LRF clips, starting in the folder of
/// `startPath` when it names one.  Returns the chosen path as UTF-8, or
/// std::nullopt when the user cancelled or the dialog could not be shown.
/// Must be called on the host's UI thread (kOfxActionInstanceChanged).
///
/// `ownerWindow` is the host's window to own the dialog (an HWND on
/// Windows), or null to use the active window.  It is checked before use -
/// a stale handle falls back to the active window - and ignored on macOS,
/// where the panel is application-modal.
[[nodiscard]] std::optional<std::string> chooseOsvFile(const std::string& startPath,
                                                       void* ownerWindow = nullptr) noexcept;

/// Trim what users paste into a path field: surrounding whitespace, and the
/// quotes Windows Explorer's "Copy as path" puts around every path.
[[nodiscard]] std::string cleanPath(std::string text);

}  // namespace osv::ofx
