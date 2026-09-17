// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "ImporterHarness.h"

#include <cstring>
#include <utility>

namespace osv::premiere::test {

namespace {

/// The paths the test CMakeLists bakes in.
#ifndef OSV_IMPORTER_MODULE_PATH
#error "OSV_IMPORTER_MODULE_PATH must be defined by the test target"
#endif

/// Turn a Win32 error code into readable text for the load failure message.
[[nodiscard]] std::string win32Message(DWORD code) {
    char* buffer = nullptr;
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = n && buffer ? std::string(buffer, n) : ("error " + std::to_string(code));
    if (buffer) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::filesystem::path importerModulePath() { return std::filesystem::path(OSV_IMPORTER_MODULE_PATH); }

std::filesystem::path sampleClipPath() {
#ifdef OSV_SAMPLE_CLIP_PATH
    return std::filesystem::path(OSV_SAMPLE_CLIP_PATH);
#else
    return {};
#endif
}

std::filesystem::path sampleProxyPath() {
#ifdef OSV_SAMPLE_PROXY_PATH
    return std::filesystem::path(OSV_SAMPLE_PROXY_PATH);
#else
    return {};
#endif
}

bool sampleClipAvailable() {
    const std::filesystem::path clip = sampleClipPath();
    std::error_code ec;
    return !clip.empty() && std::filesystem::exists(clip, ec) && std::filesystem::file_size(clip, ec) > 0;
}

// ---------------------------------------------------------------------------
//  ImporterHarness
// ---------------------------------------------------------------------------

ImporterHarness::ImporterHarness() {
    const std::filesystem::path module = importerModulePath();

    // LOAD_WITH_ALTERED_SEARCH_PATH so the module's own folder is searched
    // for its dependencies, exactly the way Premiere loads it from
    // MediaCore\OpenOSV.  (The delay-load hook handles the rest, but the
    // loader still has to find the module itself.)
    const std::wstring wide = module.wstring();
    m_module = ::LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_module) {
        m_loadError = "LoadLibraryExW(\"" + module.string() + "\") failed: " + win32Message(::GetLastError());
        return;
    }

    m_entry = reinterpret_cast<ImportEntryFunc>(
        reinterpret_cast<void*>(::GetProcAddress(m_module, "xImportEntry")));
    if (!m_entry) {
        m_loadError = "the module exports no xImportEntry: " + win32Message(::GetLastError());
        ::FreeLibrary(m_module);
        m_module = nullptr;
        return;
    }

    // Build the imStdParms the way Premiere 26 would.
    m_stdParms.imInterfaceVer = IMPORTMOD_VERSION;
    m_stdParms.funcs = &m_callbacks;
    m_stdParms.piSuites = m_host.piSuites();

    // The importer fills this during imInit; the host keeps it.
    std::memset(&m_importInfo, 0, sizeof(m_importInfo));
    m_initResult = m_entry(imInit, &m_stdParms, &m_importInfo, nullptr);
}

ImporterHarness::~ImporterHarness() {
    if (m_entry) {
        // imShutdown is what tears the renderer pool down; skipping it would
        // leak a CUDA context into the next test.
        m_entry(imShutdown, &m_stdParms, nullptr, nullptr);
    }
    if (m_module) {
        ::FreeLibrary(m_module);
    }
}

csSDK_int32 ImporterHarness::send(csSDK_int32 selector, void* param1, void* param2) {
    if (!m_entry) {
        return imOtherErr;
    }
    return m_entry(selector, &m_stdParms, param1, param2);
}

csSDK_int32 ImporterHarness::sendIndexed(csSDK_int32 selector, csSDK_int32 index, void* param2) {
    // param1 carries the index by value inside the pointer, which is how the
    // SDK passes it (doc 7.4).
    return send(selector, reinterpret_cast<void*>(static_cast<std::intptr_t>(index)), param2);
}

// ---------------------------------------------------------------------------
//  ClipHandle
// ---------------------------------------------------------------------------

ImporterHarness::ClipHandle::~ClipHandle() {
    if (m_privateData) {
        close();
    }
}

ImporterHarness::ClipHandle::ClipHandle(ClipHandle&& other) noexcept
    : m_harness(other.m_harness), m_privateData(other.m_privateData), m_fileRef(other.m_fileRef),
      m_importerId(other.m_importerId), m_openResult(other.m_openResult), m_path(std::move(other.m_path)) {
    other.m_harness = nullptr;
    other.m_privateData = nullptr;
    other.m_fileRef = imInvalidHandleValue;
}

ImporterHarness::ClipHandle& ImporterHarness::ClipHandle::operator=(ClipHandle&& other) noexcept {
    if (this != &other) {
        if (m_privateData) {
            close();
        }
        m_harness = other.m_harness;
        m_privateData = other.m_privateData;
        m_fileRef = other.m_fileRef;
        m_importerId = other.m_importerId;
        m_openResult = other.m_openResult;
        m_path = std::move(other.m_path);
        other.m_harness = nullptr;
        other.m_privateData = nullptr;
        other.m_fileRef = imInvalidHandleValue;
    }
    return *this;
}

csSDK_int32 ImporterHarness::ClipHandle::quiet() {
    if (!m_harness || !m_privateData) {
        return imOtherErr;
    }
    return m_harness->send(imQuietFile, &m_fileRef, m_privateData);
}

csSDK_int32 ImporterHarness::ClipHandle::close() {
    if (!m_harness || !m_privateData) {
        return imNoErr;
    }
    const csSDK_int32 result = m_harness->send(imCloseFile, &m_fileRef, m_privateData);
    m_privateData = nullptr;
    m_fileRef = imInvalidHandleValue;
    return result;
}

ImporterHarness::ClipHandle ImporterHarness::openClip(const std::filesystem::path& path, csSDK_int32 importerId) {
    ClipHandle clip;
    clip.m_harness = this;
    clip.m_path = path.wstring();
    clip.m_importerId = importerId;

    imFileOpenRec8 rec{};
    rec.fileinfo.filepath = reinterpret_cast<const prUTF16Char*>(clip.m_path.c_str());
    rec.fileinfo.importID = reinterpret_cast<void*>(static_cast<std::intptr_t>(importerId));
    rec.inReadWrite = kPrOpenFileAccess_ReadOnly;
    rec.inImporterID = importerId;
    rec.inStreamIdx = 0;
    rec.privatedata = nullptr;

    imFileRef fileRef = imInvalidHandleValue;
    clip.m_openResult = send(imOpenFile8, &fileRef, &rec);
    if (clip.m_openResult == imNoErr) {
        clip.m_privateData = rec.privatedata;
        clip.m_fileRef = fileRef;
    }
    return clip;
}

csSDK_int32 ImporterHarness::getInfo8(ClipHandle& clip, imFileInfoRec8& info, const PrefsBlob* prefs) {
    std::memset(&info, 0, sizeof(info));
    info.privatedata = clip.privateData();
    // The prefs pointer is non-const in the record; the importer only reads
    // it, so a const_cast of a local copy is safe and keeps the test's blob
    // immutable from the caller's point of view.
    static thread_local PrefsBlob blobCopy;
    if (prefs) {
        blobCopy = *prefs;
        info.prefs = &blobCopy;
    }
    info.streamIdx = 0;
    // The host stamps the importer id into the [in] field before the call.
    info.vidInfo.importerID = clip.importerId();

    imFileAccessRec8 access{};
    access.filetype = 'OSV_';
    access.fileref = clip.fileRef();
    return send(imGetInfo8, &access, &info);
}

csSDK_int32 ImporterHarness::getSourceVideo(ClipHandle& clip, const SourceVideoRequest& request,
                                            const PrefsBlob& prefs, PPixHand& outFrame) {
    outFrame = nullptr;

    imFrameFormat format{};
    format.inFrameWidth = request.width;
    format.inFrameHeight = request.height;
    format.inPixelFormat = request.format;

    PrefsBlob blob = prefs;

    imSourceVideoRec rec{};
    rec.inPrivateData = clip.privateData();
    rec.currentStreamIdx = 0;
    rec.inFrameTime = request.frameTime;
    rec.inFrameFormats = &format;
    rec.inNumFrameFormats = 1;
    rec.removePulldown = false;
    rec.outFrame = &outFrame;
    rec.prefs = &blob;
    rec.prefsSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
    rec.inQuality = request.quality;
    rec.inRenderContext.inIntent = request.intent;
    rec.inRenderContext.inPlaybackRatio = request.playbackRatio;
    rec.inRenderContext.inPlaybackRate = 1.0;
    rec.opaqueColorSpaceIdentifier = request.colorSpace;
    if (!request.selectedColorProfileName.empty()) {
        rec.selectedColorProfileName = m_host.makeString(request.selectedColorProfileName);
    }

    return send(imGetSourceVideo, nullptr, &rec);
}

namespace {

/// Allocate `channels` buffers of `frames` floats and the array of pointers
/// the host hands the importer.
struct AudioBuffers {
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;

    AudioBuffers(std::int32_t channels, std::uint32_t frames) {
        storage.resize(static_cast<std::size_t>(channels));
        pointers.resize(static_cast<std::size_t>(channels));
        for (std::int32_t ch = 0; ch < channels; ++ch) {
            // Poison so a channel the importer forgets to write shows up as
            // something other than silence.
            storage[static_cast<std::size_t>(ch)].assign(frames, -12345.0f);
            pointers[static_cast<std::size_t>(ch)] = storage[static_cast<std::size_t>(ch)].data();
        }
    }
};

}  // namespace

csSDK_int32 ImporterHarness::importAudio(ClipHandle& clip, std::int64_t position, std::uint32_t frames,
                                         std::int32_t channels, std::vector<std::vector<float>>& outBuffers) {
    AudioBuffers buffers(channels, frames);
    PrefsBlob blob = PrefsBlob::defaults();

    imImportAudioRec7 rec{};
    rec.position = position;
    rec.size = frames;
    rec.buffer = buffers.pointers.data();
    rec.privateData = clip.privateData();
    rec.prefs = &blob;

    const csSDK_int32 result = send(imImportAudio7, nullptr, &rec);
    outBuffers = std::move(buffers.storage);
    return result;
}

csSDK_int32 ImporterHarness::sequentialAudio(ClipHandle& clip, std::uint32_t frames, std::int32_t channels,
                                             std::vector<std::vector<float>>& outBuffers) {
    AudioBuffers buffers(channels, frames);
    PrefsBlob blob = PrefsBlob::defaults();

    imImportAudioRec7 rec{};
    rec.position = -1;  // Sequential requests carry no position.
    rec.size = frames;
    rec.buffer = buffers.pointers.data();
    rec.privateData = clip.privateData();
    rec.prefs = &blob;

    const csSDK_int32 result = send(imGetSequentialAudio, nullptr, &rec);
    outBuffers = std::move(buffers.storage);
    return result;
}

csSDK_int32 ImporterHarness::resetSequentialAudio(ClipHandle& clip) {
    imImportAudioRec7 rec{};
    rec.position = 0;
    rec.size = 0;
    rec.buffer = nullptr;
    rec.privateData = clip.privateData();
    rec.prefs = nullptr;
    return send(imResetSequentialAudio, nullptr, &rec);
}

}  // namespace osv::premiere::test
