// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The audio selectors of OpenOSVImporter.prm.  They are a thin layer over
// AudioDecoder: validate what the host handed us, take the instance lock
// (audio conforming and video decoding run at the same time on different
// threads) and forward.
//
// The three selectors serve two access patterns the host uses at once:
//
//   imImportAudio7         random access, position >= 0 (scrubbing, export)
//                          or position < 0 meaning "continue from the last
//                          call" - the legacy sequential form, which we
//                          answer from the same cursor as the dedicated
//                          selectors below;
//   imResetSequentialAudio rewind the conform cursor to sample 0;
//   imGetSequentialAudio   the conform's forward walk.
//
// Both share one AudioDecoder because they are the same stream; the cursor
// lives in the decoder so a conform is never disturbed by a scrub (the scrub
// seeks, the conform's next sequential read seeks back - correct, just not
// free, and conforming is a background task).

#include "ImporterPlugin.h"

#include "ImporterAudio.h"
#include "ImporterInstance.h"

#include "PluginLog.h"

#include "PrSDKAudioSuite.h"

#include <cstring>
#include <mutex>

namespace osv::premiere {

namespace {

/// Resolve the instance from the record and validate everything the host
/// handed us, WITHOUT touching the decoder (which needs the lock).
/// Returns nullptr and fills `outError` when the request cannot be served.
[[nodiscard]] ImporterInstance* resolveInstance(imStdParms* stdParms, imImportAudioRec7* rec,
                                                csSDK_int32& outError) noexcept {
    outError = imOtherErr;
    if (!stdParms || !rec) {
        return nullptr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->privateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        outError = imBadFile;
        return nullptr;
    }
    if (!instance->hasAudio()) {
        outError = imUnsupported;
        return nullptr;
    }
    if (!rec->buffer) {
        return nullptr;
    }
    // Every channel buffer must exist: the host allocates numChannels of
    // them, but a defensive check here is cheaper than a crash in the host.
    for (std::int32_t ch = 0; ch < instance->audioChannels(); ++ch) {
        if (!rec->buffer[ch]) {
            return nullptr;
        }
    }
    outError = imNoErr;
    return instance;
}

/// Fill every channel buffer with silence.  Used whenever the decode cannot
/// happen: a clip whose audio will not open must still play, and a failed
/// block in the middle of a conform must not abort the whole conform.
void fillSilence(const ImporterInstance& instance, imImportAudioRec7& rec) noexcept {
    for (std::int32_t ch = 0; ch < instance.audioChannels(); ++ch) {
        if (rec.buffer[ch]) {
            std::memset(rec.buffer[ch], 0, static_cast<std::size_t>(rec.size) * sizeof(float));
        }
    }
}

}  // namespace

csSDK_int32 handleImportAudio7(imStdParms* stdParms, imImportAudioRec7* rec) {
    csSDK_int32 error = imOtherErr;
    ImporterInstance* instance = resolveInstance(stdParms, rec, error);
    if (!instance) {
        return error;
    }
    if (rec->size == 0) {
        return imNoErr;
    }

    // One lock for the whole call.  Everything below therefore uses the
    // *Locked accessors; the plain ones would try to take the same
    // non-recursive mutex again and throw "resource deadlock would occur".
    std::lock_guard<std::mutex> guard(instance->lock());
    instance->applyPrefsLocked(rec->prefs, PrefsBlob::kSize);

    AudioDecoder* decoder = instance->audioLocked();
    if (!decoder || !decoder->isOpen()) {
        fillSilence(*instance, *rec);
        return imNoErr;
    }

    // A negative position is the legacy "continue sequentially" request.
    const Status st = rec->position < 0 ? decoder->readSequential(rec->size, rec->buffer)
                                        : decoder->read(rec->position, rec->size, rec->buffer);
    if (!st.ok()) {
        PluginLog::oncef("audio-read-failed", PluginLog::Level::Warn, "imImportAudio7 failed: {}",
                         st.error().message);
        fillSilence(*instance, *rec);
    }
    return imNoErr;
}

csSDK_int32 handleResetSequentialAudio(imStdParms* stdParms, imImportAudioRec7* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->privateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imBadFile;
    }
    if (!instance->hasAudio()) {
        return imUnsupported;
    }

    std::lock_guard<std::mutex> guard(instance->lock());
    AudioDecoder* decoder = instance->audioLocked();
    if (decoder) {
        decoder->resetSequential();
    }
    // imResetSequentialAudio carries no buffer; it only moves the cursor.
    return imNoErr;
}

csSDK_int32 handleGetSequentialAudio(imStdParms* stdParms, imImportAudioRec7* rec) {
    csSDK_int32 error = imOtherErr;
    ImporterInstance* instance = resolveInstance(stdParms, rec, error);
    if (!instance) {
        return error;
    }
    if (rec->size == 0) {
        return imNoErr;
    }

    std::lock_guard<std::mutex> guard(instance->lock());
    AudioDecoder* decoder = instance->audioLocked();
    if (!decoder || !decoder->isOpen()) {
        fillSilence(*instance, *rec);
        return imNoErr;
    }

    const Status st = decoder->readSequential(rec->size, rec->buffer);
    if (!st.ok()) {
        PluginLog::oncef("audio-seq-failed", PluginLog::Level::Warn, "imGetSequentialAudio failed: {}",
                         st.error().message);
        fillSilence(*instance, *rec);
    }
    return imNoErr;
}

csSDK_int32 handleGetAudioChannelLayout(imStdParms* stdParms, imGetAudioChannelLayoutRec* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed() || !instance->hasAudio()) {
        return imUnsupported;
    }
    const std::int32_t channels = instance->audioChannels();
    // 1, 2 and 6 channels are the layouts imAudioInfoRec7::numChannels can
    // describe on its own; only anything else needs explicit labels.
    if (channels == 1 || channels == 2 || channels == 6) {
        return imUnsupported;
    }
    if (channels <= 0 || channels > kMaxAudioChannelCount) {
        return imUnsupported;
    }

    // A discrete label per channel: the .OSV carries no channel-layout
    // metadata beyond the count, so claiming a specific speaker arrangement
    // would be a guess.  Discrete lets the user map them in the host.
    std::memset(rec->outChannelLabels, 0, sizeof(rec->outChannelLabels));
    for (std::int32_t ch = 0; ch < channels; ++ch) {
        rec->outChannelLabels[ch] = kPrAudioChannelLabel_Discrete;
    }
    PluginLog::oncef("audio-layout", PluginLog::Level::Info,
                     "imGetAudioChannelLayout: {} discrete channels", channels);
    return imNoErr;
}

}  // namespace osv::premiere
