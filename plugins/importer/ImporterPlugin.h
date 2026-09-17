// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The pieces of OpenOSVImporter.prm that more than one translation unit
// needs: the identity constants, the way the host handle wrapping the
// ImporterInstance is read and written, and the signatures of the selector
// handlers that ImporterEntry.cpp dispatches to.
//
// Include order matters here: the Adobe headers are all
// `#pragma pack(push, 1)` and every one of our own structs is defined either
// before the first Adobe include or (as PrefsBlob does) inside its own
// pack region.  Nothing in this header declares a struct inside an Adobe
// pack region.
#pragma once

// ---- Adobe SDK ------------------------------------------------------------
#include "PrSDKImport.h"
#include "PrSDKImporterShared.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKPlugSuites.h"
#include "PrSDKTypes.h"
#include "SPBasic.h"

// ---- our layers -----------------------------------------------------------
#include "HostSuites.h"
#include "PrefsBlob.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace osv::premiere {

class ImporterInstance;

// ---------------------------------------------------------------------------
//  Identity (docs/PREMIERE.md, "Registration and lifetime")
// ---------------------------------------------------------------------------

/// The importer's file type fourcc, matching the IMPT resource in
/// OpenOSVImporter.rc.  'OSV_' == 0x4F53565F.
constexpr csSDK_int32 kOsvFileType = 'OSV_';

/// The codec fourcc reported in imImageInfoRec::subType.  The media really is
/// HEVC in an ISO container, so 'hvc1' is honest and lets the host's UI show
/// something recognisable.
constexpr csSDK_int32 kOsvSubType = 'hvc1';

/// Names shown in the Import dialog's format popup.
constexpr const char* kFormatName = "DJI Osmo 360 (OpenOSV)";
constexpr const char* kFormatShortName = "OSV";

/// The description Premiere shows in the Properties panel.
constexpr const char* kCodecDescription = "DJI Osmo 360 dual fisheye HEVC, stitched by OpenOSV";

/// Log file base name (%LOCALAPPDATA%\OpenOSV\OpenOSVImporter.log).
constexpr const wchar_t* kLogName = L"OpenOSVImporter";

/// Environment variable that makes imGetPrefs8 accept the defaults instead of
/// showing the modal dialog.  Set OPENOSV_IMPORTER_NO_DIALOG=1 for automated
/// tests and unattended renders.
constexpr const char* kNoDialogEnvVar = "OPENOSV_IMPORTER_NO_DIALOG";

// ---------------------------------------------------------------------------
//  privateData
// ---------------------------------------------------------------------------
//
// Premiere's privateData is a host handle allocated with
// piSuites->memFuncs->newHandle.  Its payload is exactly one pointer: the
// ImporterInstance the plug-in new'd.  The indirection exists because the
// host may move the handle's storage, because the handle must be allocated
// with the host's allocator (the SDK is explicit about that), and because an
// ImporterInstance is not trivially copyable.
//
// Everything below tolerates a null handle and a handle whose payload was
// never written; a hostile or stale privateData yields nullptr rather than a
// crash.

/// Bytes the privateData handle holds.
constexpr csSDK_int32 kPrivateDataSize = static_cast<csSDK_int32>(sizeof(ImporterInstance*));

/// Read the instance out of a privateData handle (nullptr when there is
/// none).  `memFuncs` may be null, in which case the handle is dereferenced
/// directly (handles are plain pointers in every shipping host, and the mock
/// host does the same).
[[nodiscard]] ImporterInstance* instanceFromHandle(void* privateData, PlugMemoryFuncsPtr memFuncs) noexcept;

/// Store `instance` into an existing privateData handle.  Returns false when
/// the handle is unusable.
bool storeInstanceInHandle(void* privateData, PlugMemoryFuncsPtr memFuncs, ImporterInstance* instance) noexcept;

/// Allocate a privateData handle holding `instance`.  Returns nullptr on
/// failure (the caller returns imMemErr).
[[nodiscard]] void* allocateHandleFor(PlugMemoryFuncsPtr memFuncs, ImporterInstance* instance) noexcept;

// ---------------------------------------------------------------------------
//  Process-wide plug-in state
// ---------------------------------------------------------------------------

/// The suites acquired once per process (they are per-host, not per-clip) and
/// the host identity.  Lives in ImporterEntry.cpp; the accessor is here so
/// the other translation units can reach it.
/// The process-wide plug-in state.
///
/// Premiere calls an importer from many threads at once, and every selector
/// runs ensureSuites() first, so this structure is read concurrently and
/// written by whichever thread happens to arrive first.  Three things make
/// that safe:
///
///  * `mutex` serialises acquisition and release.  It is the ONLY thing that
///    may write `suites`, `basic` or `host`.
///  * `suitesAcquired` is a release/acquire atomic, so a thread that reads
///    true is guaranteed to see the fully written `suites` table published
///    by the thread that set it.  A plain bool would let a reader observe
///    the flag before the pointers it advertises.
///  * `interfaceVersion` is an atomic of its own because it is written on
///    every selector (it tracks the last imInterfaceVer) and read for
///    logging; it participates in no invariant with the rest.
///
/// Without the mutex two threads could both observe `suitesAcquired == false`
/// and both enter ImporterSuites::acquire(), whose first act is release() -
/// so the second thread would drop the host's refcounts on the very suite
/// pointers the first thread had already published and handed to a render.
struct ImporterGlobals {
    std::mutex mutex;               ///< Guards suites / basic / host.
    ImporterSuites suites;          ///< Acquired on the first selector that has piSuites.
    SPBasicSuite* basic = nullptr;  ///< The SPBasicSuite the host handed us.
    HostVersion host;               ///< App Info Suite answer (zeros when unavailable).
    std::atomic<csSDK_int32> interfaceVersion{0};  ///< imStdParms::imInterfaceVer of the last call.
    std::atomic<bool> suitesAcquired{false};      ///< Release-store after `suites` is complete.
};

/// The one ImporterGlobals of this module.
[[nodiscard]] ImporterGlobals& globals() noexcept;

/// The HINSTANCE of this .prm, captured in DllMain.  Returned as a void* so
/// this header does not have to pull <windows.h> into every translation unit
/// that includes it (the Adobe headers are pack(1) and windows.h is not).
/// Null before DllMain has run, which cannot happen for a loaded module.
[[nodiscard]] void* importerModuleHandle() noexcept;

/// Acquire the suites if that has not happened yet.  Safe to call on every
/// selector; does nothing after the first success.
void ensureSuites(imStdParms* stdParms) noexcept;

// ---------------------------------------------------------------------------
//  Colour
// ---------------------------------------------------------------------------

/// The predefined colour-space token (PrSDKColorSpaces.h) the importer
/// declares for a prefs blob: "BT.2100 PQ RGB Full", "BT.2100 HLG RGB Full"
/// or "BT.709 RGB Full".  Never null.
[[nodiscard]] const char* colorSpaceTokenFor(const PrefsBlob& prefs) noexcept;

/// The SEI code point triple for a prefs blob, used by the
/// OSV_IMPORTER_COLOR_SEI build of imGetIndColorSpace.  `primaries`,
/// `transfer` and `matrix` follow ITU-T H.273.
struct SeiCodes {
    std::int32_t primaries = 1;  ///< 1 BT.709, 9 BT.2020.
    std::int32_t transfer = 1;   ///< 1 BT.709, 16 PQ, 18 HLG.
    std::int32_t matrix = 0;     ///< 0 identity (we emit RGB).
};
[[nodiscard]] SeiCodes seiCodesFor(const PrefsBlob& prefs) noexcept;

// ---------------------------------------------------------------------------
//  Selector handlers (one translation unit each)
// ---------------------------------------------------------------------------

// ImporterVideo.cpp
csSDK_int32 handleGetInfo8(imStdParms* stdParms, imFileAccessRec8* fileAccess, imFileInfoRec8* info);
csSDK_int32 handleGetInfo9(imStdParms* stdParms, imFileAccessRec8* fileAccess, imFileInfoRec9* info);
csSDK_int32 handleGetIndPixelFormat(imStdParms* stdParms, csSDK_int32 index, imIndPixelFormatRec* rec);
csSDK_int32 handleGetPreferredFrameSize(imStdParms* stdParms, imPreferredFrameSizeRec* rec);
csSDK_int32 handleSelectClipFrameDescriptor(imStdParms* stdParms, imClipFrameDescriptorRec* rec, bool isVersion2);
csSDK_int32 handleGetIndColorSpace(imStdParms* stdParms, csSDK_int32 index, imIndColorSpaceRec* rec);
csSDK_int32 handleGetSourceVideo(imStdParms* stdParms, imSourceVideoRec* rec);
csSDK_int32 handleAnalysis(imStdParms* stdParms, imAnalysisRec* rec);
csSDK_int32 handleGetTimeInfo8(imStdParms* stdParms, imTimeInfoRec8* rec);
csSDK_int32 handleGetFileAttributes(imStdParms* stdParms, imFileAttributesRec* rec);

// ImporterAudioSelectors.cpp (in ImporterAudio.cpp's translation unit)
csSDK_int32 handleImportAudio7(imStdParms* stdParms, imImportAudioRec7* rec);
csSDK_int32 handleResetSequentialAudio(imStdParms* stdParms, imImportAudioRec7* rec);
csSDK_int32 handleGetSequentialAudio(imStdParms* stdParms, imImportAudioRec7* rec);
csSDK_int32 handleGetAudioChannelLayout(imStdParms* stdParms, imGetAudioChannelLayoutRec* rec);

// SourceSettingsDialog.cpp
csSDK_int32 handleGetPrefs8(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetPrefsRec* rec);
csSDK_int32 handleGetInstancePrefs(imStdParms* stdParms, imFileAccessRec8* fileAccess, imGetInstancePrefsRec* rec);

/// The pure mapping the dialog uses, exposed so it can be unit-tested without
/// ever creating a window.  `controls` is the state of the dialog's widgets.
struct DialogControls {
    int colorOutput = 0;    ///< Radio index: 0 PQ, 1 HLG, 2 Rec.709.
    int outputSize = 0;     ///< Combo index: 0 Native, 1 4K, 2 2K.
    int stabilization = 1;  ///< Combo index: 0 Off, 1 Horizon lock, 2 Full, 3 Smooth.
    bool seamSearch = true;
    bool gainMatch = true;
    int calibration = 0;    ///< Combo index: 0 Native, 1 Lens guards, 2 Underwater.
    int dlogmFit = 0;       ///< Combo index: 0 DJI refit, 1 Pocket 3.
    double exposureStops = 0.0;
    int renderDevice = 0;   ///< Combo index: 0 Auto, 1 CPU, 2 CUDA, 3 OpenCL.
};

/// PrefsBlob -> control state.  Every field is already in range because the
/// blob was sanitised.
[[nodiscard]] DialogControls controlsFromPrefs(const PrefsBlob& prefs) noexcept;

/// Control state -> PrefsBlob.  Out-of-range control values (a corrupted
/// dialog, a combo with no selection = -1) fall back to the defaults rather
/// than producing an invalid blob; the result is always sanitised.
[[nodiscard]] PrefsBlob prefsFromControls(const DialogControls& controls) noexcept;

/// Show the modal Source Settings dialog.  `owner` may be null.  Returns
/// true when the user pressed OK (and `prefs` was updated), false on Cancel
/// or when the dialog could not be created.  Honours
/// OPENOSV_IMPORTER_NO_DIALOG=1 by returning true without showing anything.
bool showSourceSettingsDialog(void* ownerWindow, PrefsBlob& prefs) noexcept;

}  // namespace osv::premiere
