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

#include "CalibrationUi.h"
#include "ImporterInstance.h"

#include "PluginLog.h"
#include "resource.h"

#include "osv/container/OsvFile.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

/// Log token of a calibration choice (the `osvtool --calib` spelling).
[[nodiscard]] const char* calibrationChoiceToken(PrefsCalibrationChoice choice) noexcept {
    switch (choice) {
    case PrefsCalibrationChoice::Auto:       return "auto";
    case PrefsCalibrationChoice::Native:     return "native";
    case PrefsCalibrationChoice::LensGuards: return "lens-guards";
    case PrefsCalibrationChoice::Underwater: return "underwater";
    case PrefsCalibrationChoice::Count:
    default:                                 break;
    }
    return "unknown";
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
    /// What the clip holds, so the calibration entries can say which sets
    /// exist.  known == false (plain labels) when no clip could be read.
    CalibrationUiFacts calibrationFacts;
    bool accepted = false;
};

/// Fill a combo box from owned wide strings and select `index`.
void fillComboStrings(HWND dialog, int control, const std::wstring* items, int count, int index) noexcept {
    HWND combo = ::GetDlgItem(dialog, control);
    if (!combo || !items || count <= 0) {
        return;
    }
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i].c_str()));
    }
    ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index >= 0 && index < count ? index : 0), 0);
}

/// Reduce a clip's calibration inventory to the facts the labels need.
[[nodiscard]] CalibrationUiFacts factsFrom(const meta::CalibrationInventory& inv) noexcept {
    CalibrationUiFacts facts;
    facts.known = true;
    facts.recordedAccessory = inv.recordedModePresent ? static_cast<int>(inv.recordedMode) : -1;
    // Usable-and-different, usable-but-a-copy, or not there at all.
    auto availability = [&inv](meta::CalibrationChoice choice) noexcept {
        if (inv.choiceSetState(choice) != meta::CalibrationSetState::Usable) {
            return CalibrationAvailability::Missing;
        }
        return inv.choiceChangesStitch(choice) ? CalibrationAvailability::Usable : CalibrationAvailability::SameAsNative;
    };
    facts.underwater = availability(meta::CalibrationChoice::Underwater);
    return facts;
}

/// The calibration facts of the clip the dialog is about.
///
/// A live instance already holds the parsed metadata (imGetInstancePrefs).
/// imGetPrefs8 has no instance, but its imFileAccessRec8 names the file, and
/// reading the container index plus djmd sample 0 is a few milliseconds - a
/// price worth paying once per dialog so the menu can tell the truth.  Any
/// failure simply yields "unknown" (plain labels); the dialog never fails
/// because of this.
[[nodiscard]] CalibrationUiFacts calibrationFactsFor(const imFileAccessRec8* fileAccess,
                                                     ImporterInstance* instance) noexcept {
    try {
        // ---- the instance's own parse, when there is one -------------------
        if (instance && instance->parsed() && instance->metadata().hasStream()) {
            return factsFrom(meta::CalibrationSelector::inventory(instance->metadata().stream()));
        }
        // ---- otherwise read the file the host named ------------------------
        if (!fileAccess || !fileAccess->filepath || fileAccess->filepath[0] == 0) {
            return {};
        }
        const std::filesystem::path path(reinterpret_cast<const wchar_t*>(fileAccess->filepath));
        Result<OsvFile> file = OsvFile::open(path);
        if (!file.ok()) {
            return {};
        }
        Result<meta::MetadataTrack> track = meta::MetadataTrack::load(file.value());
        if (!track.ok() || !track.value().hasStream()) {
            return {};
        }
        return factsFrom(meta::CalibrationSelector::inventory(track.value().stream()));
    } catch (...) {
        // Allocation failure or anything else: plain labels, never a crash
        // inside the host's prefs call.
        return {};
    }
}

/// Load the controls into the widgets.
void controlsToWidgets(HWND dialog, const DialogControls& c, const CalibrationUiFacts& calibrationFacts) noexcept {
    // EVERY list below is ordered to match its PrefsBlob enum exactly: the
    // combo box index IS the enum value, so inserting an entry anywhere but
    // the end would silently re-map every previously saved project's setting.
    //
    // And every count is std::size(), never a literal.  That is not style:
    // the size list gained a fourth entry (2560 x 1280) while its call still
    // passed a hard-coded 3, so the new DEFAULT was unreachable in the one
    // place a user goes to change it.  A literal count is a bug waiting for
    // the next appended entry, and the colour list below has just become the
    // second list to gain one.

    // Colour output.  A combo box, not the three radio buttons this used to
    // be - see plugins/importer/resource.h for why it changed.  The fourth
    // entry says "no transform" rather than just "D-Log M" because the other
    // three name what the output IS, and this one has to say that nothing was
    // done to it; a user who read it as "convert to D-Log M" would apply a
    // LUT on top and double-convert.
    static const wchar_t* const kColors[] = {L"Rec.2100 PQ", L"Rec.2100 HLG", L"Rec.709",
                                             L"D-Log M (no transform, grade downstream)"};
    fillCombo(dialog, IDC_COLOR_OUTPUT, kColors, static_cast<int>(std::size(kColors)), c.colorOutput);

    static const wchar_t* const kSizes[] = {L"Native", L"4K (3840 x 1920)", L"2560 x 1280 (default)",
                                            L"2K (1920 x 960)"};
    fillCombo(dialog, IDC_OUTPUT_SIZE, kSizes, static_cast<int>(std::size(kSizes)), c.outputSize);

    static const wchar_t* const kStab[] = {L"Off", L"Horizon lock", L"Full", L"Smooth"};
    fillCombo(dialog, IDC_STABILIZATION, kStab, static_cast<int>(std::size(kStab)), c.stabilization);

    // Calibration: indexed by PrefsCalibrationChoice (Auto first, the
    // default), labelled for THIS clip - "not in clip: Native" on a set the
    // file does not carry, so an unchanged picture is never a surprise.
    // The label array is sized by PrefsCalibrationChoice::Count, so a new
    // choice cannot be left out of the menu.
    try {
        const auto kCalib = calibrationChoiceLabels(calibrationFacts);
        fillComboStrings(dialog, IDC_CALIBRATION, kCalib.data(), static_cast<int>(kCalib.size()), c.calibration);
    } catch (...) {
        // Building four short strings can only fail on allocation; leave the
        // combo empty rather than let an exception cross USER32.
        PluginLog::error("source settings: could not build the calibration labels");
    }

    // Order must match PrefsDlogmFit exactly (the static_assert below pins the
    // count, not the order).  Osmo 360 is last because the enum is append-only.
    static const wchar_t* const kFit[] = {L"DJI refit", L"Pocket 3", L"Osmo 360"};
    fillCombo(dialog, IDC_DLOGM_FIT, kFit, static_cast<int>(std::size(kFit)), c.dlogmFit);

    static const wchar_t* const kDevice[] = {L"Auto", L"CPU", L"CUDA", L"OpenCL"};
    fillCombo(dialog, IDC_RENDER_DEVICE, kDevice, static_cast<int>(std::size(kDevice)), c.renderDevice);

    // Each list must be able to express every value of its enum, or a setting
    // becomes unreachable in the UI.  static_assert rather than a test,
    // because an enum that grows must break the BUILD, not a test run.
    static_assert(std::size(kColors) == static_cast<std::size_t>(PrefsColorOutput::Count),
                  "the colour combo does not list every PrefsColorOutput value");
    static_assert(std::size(kSizes) == static_cast<std::size_t>(PrefsOutputSize::Count),
                  "the size combo does not list every PrefsOutputSize value");
    static_assert(std::size(kStab) == static_cast<std::size_t>(PrefsStabilization::Count),
                  "the stabilisation combo does not list every PrefsStabilization value");
    static_assert(std::size(kFit) == static_cast<std::size_t>(PrefsDlogmFit::Count),
                  "the D-Log M combo does not list every PrefsDlogmFit value");
    static_assert(std::size(kDevice) == static_cast<std::size_t>(PrefsRenderDevice::Count),
                  "the device combo does not list every PrefsRenderDevice value");

    ::CheckDlgButton(dialog, IDC_SEAM_SEARCH, c.seamSearch ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dialog, IDC_GAIN_MATCH, c.gainMatch ? BST_CHECKED : BST_UNCHECKED);
    setEditDouble(dialog, IDC_EXPOSURE, c.exposureStops);
}

/// Read the widgets back into the controls.
void widgetsToControls(HWND dialog, DialogControls& c) noexcept {
    c.colorOutput = comboSelection(dialog, IDC_COLOR_OUTPUT);
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
            controlsToWidgets(dialog, state->controls, state->calibrationFacts);
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

namespace {

/// The dialog, with what is known about the clip's calibration sets.
bool showDialogWithFacts(void* ownerWindow, PrefsBlob& prefs, const CalibrationUiFacts& calibrationFacts) noexcept {
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
    state.calibrationFacts = calibrationFacts;

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
    // From the incoming blob, so the fields this dialog does not show
    // (parallax, flow backend, ...) survive an OK untouched.
    prefs = prefsFromControls(state.controls, prefs);
    return true;
}

}  // namespace

bool showSourceSettingsDialog(void* ownerWindow, PrefsBlob& prefs) noexcept {
    // No clip facts on this entry point: plain calibration labels.
    return showDialogWithFacts(ownerWindow, prefs, CalibrationUiFacts{});
}

// ---------------------------------------------------------------------------
//  imGetPrefs8 / imGetInstancePrefs
// ---------------------------------------------------------------------------

namespace {

/// The shared body of both prefs selectors.  `instance` may be null
/// (imGetPrefs8 has no privateData by design); `fileAccess` may be null too,
/// and is only read to label the calibration choices for the clip.
[[nodiscard]] csSDK_int32 handlePrefsCommon(imStdParms* stdParms, imGetPrefsRec* rec, ImporterInstance* instance,
                                            const imFileAccessRec8* fileAccess) noexcept {
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
    // What the clip was using before the dialog, to tell whether OK changed
    // anything the frames depend on (see the refresh below).
    const PrefsBlob before = blob;

    // The host main window owns the modal so the dialog is not lost behind
    // the application and the host's message loop is properly blocked.
    void* owner = nullptr;
    if (stdParms && stdParms->piSuites && stdParms->piSuites->windFuncs &&
        stdParms->piSuites->windFuncs->getMainWnd) {
        owner = stdParms->piSuites->windFuncs->getMainWnd();
    }

    // What this clip holds, so the calibration menu can say which sets exist.
    // Skipped when the dialog is suppressed: nothing would show it, and a
    // render farm should not pay for a second parse of every clip.
    const CalibrationUiFacts calibrationFacts =
        dialogSuppressed() ? CalibrationUiFacts{} : calibrationFactsFor(fileAccess, instance);

    if (!showDialogWithFacts(owner, blob, calibrationFacts)) {
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
    //
    // The SDK guide asks for exactly this whenever the settings changed in a
    // way that needs the frames reimported - through imGetPrefs8 too, which
    // has no instance and used to skip the refresh entirely.  There the clip
    // is named by the imFileAccessRec8 the host passed in.
    try {
        ImporterGlobals& g = globals();
        const std::wstring instancePath = instance ? instance->path().wstring() : std::wstring();
        const wchar_t* accessPath =
            (fileAccess && fileAccess->filepath) ? reinterpret_cast<const wchar_t*>(fileAccess->filepath) : nullptr;
        const std::wstring target = prefsRefreshTarget(before, blob, instancePath.c_str(), accessPath);
        if (!target.empty() && g.suites.fileManager && g.suites.fileManager->RefreshFileAsync) {
            const prSuiteError err =
                g.suites.fileManager->RefreshFileAsync(reinterpret_cast<const prUTF16Char*>(target.c_str()));
            PluginLog::info("source settings: asked the host to refresh '{}' ({})",
                            std::filesystem::path(target).filename().string(),
                            err == suiteError_NoError ? "ok" : "refused");
        }
    } catch (...) {
        // Path conversion only; the settings themselves are already stored.
        PluginLog::warn("source settings: could not request a refresh of the clip");
    }

    PluginLog::info("source settings accepted: colour {}, size {}, stab {}, seam {}, gain {}, calib {} ({}), fit {}, "
                    "exposure {:+.2f}, device {}",
                    blob.colorOutput, blob.outputSize, blob.stabilization, blob.seamSearch, blob.gainMatch,
                    blob.calibration, calibrationChoiceToken(blob.calibrationChoice()), blob.dlogmFit,
                    static_cast<double>(blob.exposureStops), blob.renderDevice);
    return imNoErr;
}

}  // namespace

csSDK_int32 handleGetPrefs8(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetPrefsRec* rec) {
    // The static prefs call has no instance to reach; the file it names is
    // read to label the calibration choices and is the one refreshed after
    // a changed OK.
    return handlePrefsCommon(stdParms, rec, nullptr, fileAccess);
}

csSDK_int32 handleGetInstancePrefs(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetInstancePrefsRec* rec) {
    if (!rec) {
        return imOtherErr;
    }
    // Unlike imGetPrefs8 this one does carry privateData, so the dialog's
    // result can be pushed straight into the live instance.
    ImporterInstance* instance =
        instanceFromHandle(rec->privateData, stdParms && stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    return handlePrefsCommon(stdParms, &rec->prefsRec, instance, fileAccess);
}

// ---------------------------------------------------------------------------
//  imPerformSourceSettingsCommand (selector 66)
// ---------------------------------------------------------------------------
//
// The private exchange with OpenOSVSourceSettings.aex.  See the declaration
// in ImporterPlugin.h for the protocol; the rules this implementation obeys
// are all about not trusting the buffer:
//
//   * a null record, a null ioData or a buffer shorter than a PrefsBlob is
//     answered with imOtherErr and NOT written to.  The effect treats any
//     non-success as "keep the stored control values", which is the correct
//     degradation, whereas a partial write would hand it a blob whose magic
//     is half-formed.
//   * a buffer LARGER than a PrefsBlob is fine; only the first kSize bytes
//     are ours and the tail is left alone, because it is not our memory to
//     define.
//   * whatever the effect sent is validated before it is used.  A blob that
//     is not ours becomes the defaults rather than being trusted, so a stale
//     payload from an older build cannot poison a live clip's settings.
//
// This selector never shows a dialog and never blocks: it may arrive on any
// thread during project load, and a modal window there would deadlock the
// load.
csSDK_int32 handlePerformSourceSettingsCommand(imStdParms* stdParms, imFileAccessRec8* fileAccess,
                                               imSourceSettingsCommandRec* rec) {
    if (!rec || !rec->ioData) {
        PluginLog::oncef("ss-cmd-null", PluginLog::Level::Warn,
                         "imPerformSourceSettingsCommand: no data buffer");
        return imOtherErr;
    }
    if (rec->inDataSize < static_cast<csSDK_int32>(PrefsBlob::kSize)) {
        PluginLog::oncef("ss-cmd-small", PluginLog::Level::Error,
                         "imPerformSourceSettingsCommand: the buffer is {} bytes, {} are needed",
                         rec->inDataSize, static_cast<int>(PrefsBlob::kSize));
        return imOtherErr;
    }

    // What the effect's controls currently say.  fromBytes() sanitises and
    // falls back to the defaults for anything that is not one of our blobs.
    PrefsBlob incoming = PrefsBlob::fromBytes(rec->ioData, static_cast<std::size_t>(rec->inDataSize));

    // The instance, when the host has one for this clip.  It is reached
    // through the record's own privateData rather than through fileAccess:
    // the SDK documents param1 as an imFileAccessRec8* for this selector, but
    // the record carries the instance pointer directly and that is the field
    // every other prefs path uses, so using it keeps one convention.
    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms && stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    (void)fileAccess;

    if (!instance) {
        // No live clip to consult - which is normal during project load and
        // before imOpenFile8.  The effect's own values are then the best
        // available truth, so they are echoed back sanitised.  Echoing rather
        // than writing defaults matters: overwriting with defaults here would
        // reset every control of every clip on every project open.
        std::memcpy(rec->ioData, &incoming, PrefsBlob::kSize);
        PluginLog::oncef("ss-cmd-noinstance", PluginLog::Level::Debug,
                         "imPerformSourceSettingsCommand: no live instance; echoing the effect's own settings");
        return imNoErr;
    }

    // There IS a live clip, and it is the only party that knows what the
    // media is actually being decoded with, so its blob wins.  This is what
    // makes the panel show "as shot" after a project reopen.
    const PrefsBlob current = instance->prefs();
    std::memcpy(rec->ioData, &current, PrefsBlob::kSize);

    PluginLog::debug("imPerformSourceSettingsCommand: '{}' reports colour {}, size {}, stab {}, seam {}, "
                     "gain {}, calib {}, fit {}, exposure {:+.2f}, device {}",
                     instance->path().filename().string(), current.colorOutput, current.outputSize,
                     current.stabilization, current.seamSearch, current.gainMatch, current.calibration,
                     current.dlogmFit, static_cast<double>(current.exposureStops), current.renderDevice);
    return imNoErr;
}

}  // namespace osv::premiere
