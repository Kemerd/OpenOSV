// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterHarness: loads the built OpenOSVImporter.prm with LoadLibraryW,
// resolves xImportEntry and plays host through the mock suites, so the tests
// exercise the SHIPPING BINARY rather than a re-link of its sources.
//
// That distinction matters: the module is built with /MD, a specific export
// set, a delay-load table and an IMPT resource, and a test that only linked
// the object files would not prove any of them.  It also means a test failure
// here is a failure of the thing that goes into MediaCore\OpenOSV.
//
// The harness owns the whole lifecycle: imInit on construction, imShutdown on
// destruction (which is what frees the renderer pool), and a ClipHandle RAII
// type for imOpenFile8 / imCloseFile.
#pragma once

#include "MockHost.h"

#include "PrSDKImport.h"
#include "PrSDKImporterShared.h"
#include "PrSDKTypes.h"

#include "PrefsBlob.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::premiere::test {

/// The importer's entry point signature (PrSDKImport.h:896).
using ImportEntryFunc = csSDK_int32(__cdecl*)(csSDK_int32 selector, imStdParms* stdParms, void* param1, void* param2);

/// Full path of the built OpenOSVImporter.prm, taken from the compile
/// definition the test CMakeLists supplies.  Present even when the file is
/// not, so a test can report a useful message.
[[nodiscard]] std::filesystem::path importerModulePath();

/// Full path of the sample clip (compile definition), or an empty path when
/// the build was configured without one.
[[nodiscard]] std::filesystem::path sampleClipPath();
[[nodiscard]] std::filesystem::path sampleProxyPath();

/// True when the sample clip exists on disk; the [sample] tests SKIP when it
/// does not.
[[nodiscard]] bool sampleClipAvailable();

class ImporterHarness {
public:
    /// Load the module and send imInit.  Check loaded() afterwards.
    ImporterHarness();
    /// Sends imShutdown and unloads the module.
    ~ImporterHarness();

    ImporterHarness(const ImporterHarness&) = delete;
    ImporterHarness& operator=(const ImporterHarness&) = delete;

    [[nodiscard]] bool loaded() const noexcept { return m_entry != nullptr; }
    /// Why loaded() is false ("" when it is true).
    [[nodiscard]] const std::string& loadError() const noexcept { return m_loadError; }

    /// The result imInit returned (imIsCacheable when all is well).
    [[nodiscard]] csSDK_int32 initResult() const noexcept { return m_initResult; }
    /// The imImportInfoRec the importer filled during imInit.
    [[nodiscard]] const imImportInfoRec& importInfo() const noexcept { return m_importInfo; }

    /// The mock host serving the suites.
    [[nodiscard]] mock::MockHost& host() noexcept { return m_host; }

    /// Send one selector.  `stdParms` is filled from the mock host.
    csSDK_int32 send(csSDK_int32 selector, void* param1, void* param2);

    /// Send a selector with an integer param1 (imGetIndFormat,
    /// imGetIndPixelFormat, imGetIndColorSpace pass the index that way).
    csSDK_int32 sendIndexed(csSDK_int32 selector, csSDK_int32 index, void* param2);

    /// The imStdParms the harness builds (exposed so a test can tweak
    /// imInterfaceVer to emulate an older host).
    [[nodiscard]] imStdParms& stdParms() noexcept { return m_stdParms; }

    /// One open clip: the privateData handle, the OS handle and the id the
    /// "host" assigned, closed on destruction.
    class ClipHandle {
    public:
        ClipHandle() = default;
        ~ClipHandle();
        ClipHandle(ClipHandle&& other) noexcept;
        ClipHandle& operator=(ClipHandle&& other) noexcept;
        ClipHandle(const ClipHandle&) = delete;
        ClipHandle& operator=(const ClipHandle&) = delete;

        [[nodiscard]] bool open() const noexcept { return m_privateData != nullptr; }
        [[nodiscard]] void* privateData() const noexcept { return m_privateData; }
        [[nodiscard]] imFileRef fileRef() const noexcept { return m_fileRef; }
        [[nodiscard]] csSDK_int32 importerId() const noexcept { return m_importerId; }
        [[nodiscard]] csSDK_int32 openResult() const noexcept { return m_openResult; }

        /// Send imQuietFile (the OS handle closes, privateData survives).
        csSDK_int32 quiet();
        /// Send imCloseFile and forget everything.
        csSDK_int32 close();

    private:
        friend class ImporterHarness;
        ImporterHarness* m_harness = nullptr;
        void* m_privateData = nullptr;
        imFileRef m_fileRef = imInvalidHandleValue;
        csSDK_int32 m_importerId = 0;
        csSDK_int32 m_openResult = imOtherErr;
        std::wstring m_path;
    };

    /// imOpenFile8 on `path`.  The returned handle reports openResult(); a
    /// failed open yields a handle whose open() is false.
    [[nodiscard]] ClipHandle openClip(const std::filesystem::path& path, csSDK_int32 importerId = 7);

    /// imGetInfo8 on an open clip.  `prefs` may be null for the defaults.
    csSDK_int32 getInfo8(ClipHandle& clip, imFileInfoRec8& info, const PrefsBlob* prefs = nullptr);

    /// imGetSourceVideo.  Returns the selector result; `outFrame` receives the
    /// PPix (which the caller disposes through the mock PPix suite).
    struct SourceVideoRequest {
        PrTime frameTime = 0;
        PrPixelFormat format = PrPixelFormat_BGRA_4444_32f;
        csSDK_int32 width = 0;   ///< 0 = any.
        csSDK_int32 height = 0;
        PrRenderQuality quality = kPrRenderQuality_High;
        imRenderIntent intent = imRenderIntent_Export;
        double playbackRatio = 1.0;
        PrSDKColorSpaceID colorSpace{};
        std::string selectedColorProfileName;  ///< Empty = do not set the field.
    };
    csSDK_int32 getSourceVideo(ClipHandle& clip, const SourceVideoRequest& request, const PrefsBlob& prefs,
                               PPixHand& outFrame);

    /// imImportAudio7 (random access when `position` >= 0).
    csSDK_int32 importAudio(ClipHandle& clip, std::int64_t position, std::uint32_t frames, std::int32_t channels,
                            std::vector<std::vector<float>>& outBuffers);
    /// imGetSequentialAudio.
    csSDK_int32 sequentialAudio(ClipHandle& clip, std::uint32_t frames, std::int32_t channels,
                                std::vector<std::vector<float>>& outBuffers);
    /// imResetSequentialAudio.
    csSDK_int32 resetSequentialAudio(ClipHandle& clip);

private:
    mock::MockHost m_host;
    HMODULE m_module = nullptr;
    ImportEntryFunc m_entry = nullptr;
    std::string m_loadError;
    imStdParms m_stdParms{};
    imCallbackFuncs m_callbacks{};
    imImportInfoRec m_importInfo{};
    csSDK_int32 m_initResult = imOtherErr;
};

}  // namespace osv::premiere::test
