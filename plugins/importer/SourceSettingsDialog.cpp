// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// imGetPrefs8 / imGetInstancePrefs and the Win32 modal "OpenOSV Source
// Settings" dialog behind them (docs/PREMIERE.md, "Prefs and Source Settings
// dialog").
//
// Three things are deliberately separate here:
//
//   1. controlsFromPrefs / prefsFromControls - a pure mapping between the
//      128-byte PrefsBlob and the dialog's widget state.  No Win32, no
//      globals, no allocation: this is what the unit tests exercise, so the
//      round trip is proven without ever creating a window (a modal dialog
//      cannot run inside a test).
//   2. showSourceSettingsDialog - the Win32 part, which only reads and writes
//      a DialogControls through the mapping above.
//   3. the two selectors, which implement the SDK's two-step prefs protocol.
//
// The two-step protocol (PrSDKImport.h, doc 7.3.6): the first call has
// prefs == nullptr and only wants prefsLength; a later call has a buffer of
// that size and is where defaults are written and the dialog is shown.
// privateData is NOT available in imGetPrefs8 (the call may run in a
// different process), which is why imGetInstancePrefs exists and why the
// dialog needs no clip state - every control is a property of the blob.

#include "ImporterPlugin.h"

#include "ImporterInstance.h"

#include "PluginLog.h"
#include "resource.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

namespace osv::premiere {

namespace {

/// True when OPENOSV_IMPORTER_NO_DIALOG is set to something other than "0".
/// Documented in docs/BUILDING.md: it makes the importer accept the defaults
/// without showing a modal dialog, which is what an unattended render farm
/// and the test suite need.
[[nodiscard]] bool dialogSuppressed() noexcept {
    char value[16] = {};
    std::size_t length = 0;
    if (::getenv_s(&length, value, sizeof(value), kNoDialogEnvVar) != 0 || length == 0) {
        return false;
    }
    return value[0] != '0';
}

/// Fill a combo box from a NUL-separated list and select `index`.
void fillCombo(HWND dialog, int control, const wchar_t* const* items, int count, int index) noexcept {
    HWND combo = ::GetDlgItem(dialog, control);
    if (!combo) {
        return;
    }
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index >= 0 && index < count ? index : 0), 0);
}

/// Current selection of a combo box (0 when nothing is selected).
[[nodiscard]] int comboSelection(HWND dialog, int control) noexcept {
    HWND combo = ::GetDlgItem(dialog, control);
    if (!combo) {
        return 0;
    }
    const LRESULT sel = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return sel == CB_ERR ? 0 : static_cast<int>(sel);
}

/// Write a double into an edit control with two decimals.
void setEditDouble(HWND dialog, int control, double value) noexcept {
    wchar_t text[32] = {};
    ::swprintf_s(text, L"%.2f", value);
    ::SetDlgItemTextW(dialog, control, text);
}

/// Read a double from an edit control.  Unparseable text yields `fallback`
/// rather than 0, so a user who cleared the field does not silently get a
/// different exposure than the one they see when the dialog reopens.
[[nodiscard]] double getEditDouble(HWND dialog, int control, double fallback) noexcept {
    wchar_t text[64] = {};
    if (::GetDlgItemTextW(dialog, control, text, static_cast<int>(std::size(text))) == 0) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const double value = ::wcstod(text, &end);
    if (end == text) {
        return fallback;
    }
    return value;
}

/// The state the dialog procedure operates on, passed through
/// DialogBoxParamW's lParam and stored in GWLP_USERDATA.
struct DialogState {
    DialogControls controls;
    bool accepted = false;
};

/// Load the controls into the widgets.
void controlsToWidgets(HWND dialog, const DialogControls& c) noexcept {
    // Colour output is a radio group: exactly one of the three is checked.
    ::CheckRadioButton(dialog, IDC_COLOR_PQ, IDC_COLOR_709,
                       c.colorOutput == 1 ? IDC_COLOR_HLG : c.colorOutput == 2 ? IDC_COLOR_709 : IDC_COLOR_PQ);

    // Order must match PrefsOutputSize exactly: the combo box index IS the
    // enum value, so inserting an entry anywhere but the end would silently
    // re-map every previously saved project's setting.
    static const wchar_t* const kSizes[] = {L"Native", L"4K (3840 x 1920)", L"2560 x 1280 (default)",
                                            L"2K (1920 x 960)"};
    fillCombo(dialog, IDC_OUTPUT_SIZE, kSizes, 3, c.outputSize);

    static const wchar_t* const kStab[] = {L"Off", L"Horizon lock", L"Full", L"Smooth"};
    fillCombo(dialog, IDC_STABILIZATION, kStab, 4, c.stabilization);

    static const wchar_t* const kCalib[] = {L"Native", L"Lens guards", L"Underwater"};
    fillCombo(dialog, IDC_CALIBRATION, kCalib, 3, c.calibration);

    static const wchar_t* const kFit[] = {L"DJI refit", L"Pocket 3"};
    fillCombo(dialog, IDC_DLOGM_FIT, kFit, 2, c.dlogmFit);

    static const wchar_t* const kDevice[] = {L"Auto", L"CPU", L"CUDA", L"OpenCL"};
    fillCombo(dialog, IDC_RENDER_DEVICE, kDevice, 4, c.renderDevice);

    ::CheckDlgButton(dialog, IDC_SEAM_SEARCH, c.seamSearch ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dialog, IDC_GAIN_MATCH, c.gainMatch ? BST_CHECKED : BST_UNCHECKED);
    setEditDouble(dialog, IDC_EXPOSURE, c.exposureStops);
}

/// Read the widgets back into the controls.
void widgetsToControls(HWND dialog, DialogControls& c) noexcept {
    c.colorOutput = ::IsDlgButtonChecked(dialog, IDC_COLOR_HLG) == BST_CHECKED   ? 1
                    : ::IsDlgButtonChecked(dialog, IDC_COLOR_709) == BST_CHECKED ? 2
                                                                                 : 0;
    c.outputSize = comboSelection(dialog, IDC_OUTPUT_SIZE);
    c.stabilization = comboSelection(dialog, IDC_STABILIZATION);
    c.calibration = comboSelection(dialog, IDC_CALIBRATION);
    c.dlogmFit = comboSelection(dialog, IDC_DLOGM_FIT);
    c.renderDevice = comboSelection(dialog, IDC_RENDER_DEVICE);
    c.seamSearch = ::IsDlgButtonChecked(dialog, IDC_SEAM_SEARCH) == BST_CHECKED;
    c.gainMatch = ::IsDlgButtonChecked(dialog, IDC_GAIN_MATCH) == BST_CHECKED;
    c.exposureStops = getEditDouble(dialog, IDC_EXPOSURE, c.exposureStops);
}

/// The dialog procedure.  It never throws (a C callback crossing back into
/// USER32 with a live exception would take the host down), so the whole body
/// is exception free by construction.
INT_PTR CALLBACK sourceSettingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<DialogState*>(lParam);
        ::SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
        if (state) {
            controlsToWidgets(dialog, state->controls);
        }
        return TRUE;  // Let the dialog manager set the initial focus.
    }
    case WM_COMMAND: {
        auto* state = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, GWLP_USERDATA));
        const int id = LOWORD(wParam);
        if (id == IDOK) {
            if (state) {
                widgetsToControls(dialog, state->controls);
                state->accepted = true;
            }
            ::EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            ::EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        ::EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

}  // namespace

// The pure PrefsBlob <-> DialogControls mapping this file reads and writes
// lives in PrefsMapping.cpp, which has no Win32 / suite / instance
// dependencies and is therefore compiled into the unit tests as well, so the
// mapping the shipping dialog uses is exactly the one the tests prove.

// ---------------------------------------------------------------------------
//  The modal dialog
// ---------------------------------------------------------------------------

bool showSourceSettingsDialog(void* ownerWindow, PrefsBlob& prefs) noexcept {
    if (dialogSuppressed()) {
        PluginLog::oncef("dialog-suppressed", PluginLog::Level::Info,
                         "{}=1: accepting the current source settings without showing the dialog", kNoDialogEnvVar);
        prefs.sanitise();
        return true;
    }

    HINSTANCE module = static_cast<HINSTANCE>(importerModuleHandle());
    if (!module) {
        PluginLog::error("source settings: the module handle is unknown; the dialog cannot be created");
        return false;
    }

    DialogState state;
    state.controls = controlsFromPrefs(prefs);

    const INT_PTR result = ::DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_SOURCE_SETTINGS),
                                             static_cast<HWND>(ownerWindow), &sourceSettingsProc,
                                             reinterpret_cast<LPARAM>(&state));
    if (result == -1) {
        const DWORD err = ::GetLastError();
        PluginLog::error("source settings: DialogBoxParamW failed with {}", static_cast<unsigned>(err));
        return false;
    }
    if (result != IDOK || !state.accepted) {
        return false;  // The caller returns imCancel.
    }
    prefs = prefsFromControls(state.controls);
    return true;
}

// ---------------------------------------------------------------------------
//  imGetPrefs8 / imGetInstancePrefs
// ---------------------------------------------------------------------------

namespace {

/// The shared body of both prefs selectors.  `instance` may be null
/// (imGetPrefs8 has no privateData by design).
[[nodiscard]] csSDK_int32 handlePrefsCommon(imStdParms* stdParms, imGetPrefsRec* rec,
                                            ImporterInstance* instance) noexcept {
    if (!rec) {
        return imOtherErr;
    }

    // Step 1: no buffer yet - just say how many bytes we need.
    if (!rec->prefs) {
        rec->prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        return imNoErr;
    }
    if (rec->prefsLength < static_cast<csSDK_int32>(PrefsBlob::kSize)) {
        // The host gave us a smaller buffer than we asked for; ask again
        // rather than writing past the end of it.
        rec->prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        return imNoErr;
    }

    // Step 2: the buffer exists.  Anything that is not one of our blobs (a
    // first-time call, a blob from another importer, garbage) becomes the
    // documented defaults.
    PrefsBlob blob = PrefsBlob::fromBytes(rec->prefs, static_cast<std::size_t>(rec->prefsLength));

    // The host main window owns the modal so the dialog is not lost behind
    // the application and the host's message loop is properly blocked.
    void* owner = nullptr;
    if (stdParms && stdParms->piSuites && stdParms->piSuites->windFuncs &&
        stdParms->piSuites->windFuncs->getMainWnd) {
        owner = stdParms->piSuites->windFuncs->getMainWnd();
    }

    if (!showSourceSettingsDialog(owner, blob)) {
        return imCancel;
    }

    std::memcpy(rec->prefs, &blob, PrefsBlob::kSize);
    rec->prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);

    if (instance) {
        instance->applyPrefs(&blob, PrefsBlob::kSize);
    }

    // Tell the host the media changed so it re-asks for info and frames with
    // the new settings.  Without this the timeline keeps the old frames until
    // something else invalidates them.
    ImporterGlobals& g = globals();
    if (g.suites.fileManager && g.suites.fileManager->RefreshFileAsync && instance) {
        const std::wstring path = instance->path().wstring();
        g.suites.fileManager->RefreshFileAsync(reinterpret_cast<const prUTF16Char*>(path.c_str()));
    }

    PluginLog::info("source settings accepted: colour {}, size {}, stab {}, seam {}, gain {}, calib {}, fit {}, "
                    "exposure {:+.2f}, device {}",
                    blob.colorOutput, blob.outputSize, blob.stabilization, blob.seamSearch, blob.gainMatch,
                    blob.calibration, blob.dlogmFit, static_cast<double>(blob.exposureStops), blob.renderDevice);
    return imNoErr;
}

}  // namespace

csSDK_int32 handleGetPrefs8(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetPrefsRec* rec) {
    (void)fileAccess;  // The static prefs call has no instance to reach.
    return handlePrefsCommon(stdParms, rec, nullptr);
}

csSDK_int32 handleGetInstancePrefs(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetInstancePrefsRec* rec) {
    (void)fileAccess;
    if (!rec) {
        return imOtherErr;
    }
    // Unlike imGetPrefs8 this one does carry privateData, so the dialog's
    // result can be pushed straight into the live instance.
    ImporterInstance* instance =
        instanceFromHandle(rec->privateData, stdParms && stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    return handlePrefsCommon(stdParms, &rec->prefsRec, instance);
}

}  // namespace osv::premiere
