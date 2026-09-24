// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxFileDialog.h - the "Choose .OSV File..." button's Windows file dialog.
//
// DaVinci Resolve shows an OpenFX file-path parameter as a plain text field
// with no Browse button, so pasting a path would be the only way in.  The
// OpenOSV Source generator therefore carries a push button whose
// kOfxActionInstanceChanged opens the standard Windows Open dialog and writes
// the chosen path into the file parameter - the pattern shipping Resolve
// plug-ins use for the same gap.
#pragma once

#include <optional>
#include <string>

namespace osv::ofx {

/// Show a modal Open dialog for .OSV / .LRF clips, starting in the folder of
/// `startPath` when it names one.  Returns the chosen path as UTF-8, or
/// std::nullopt when the user cancelled or the dialog could not be shown.
/// Must be called on the host's UI thread (kOfxActionInstanceChanged).
[[nodiscard]] std::optional<std::string> chooseOsvFile(const std::string& startPath) noexcept;

/// Trim what users paste into a path field: surrounding whitespace, and the
/// quotes Windows Explorer's "Copy as path" puts around every path.
[[nodiscard]] std::string cleanPath(std::string text);

}  // namespace osv::ofx
