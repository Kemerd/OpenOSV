// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AudioDecoder implementation: FFmpeg demux + AAC decode + swresample to
// planar float, with a decoded-sample ring so sequential reads never
// re-decode and random reads only seek when they have to.

#include "ImporterAudio.h"

#include "PluginLog.h"

#if defined(_MSC_VER)
#pragma warning(push)
// C4244 (int64 -> int in FFmpeg inline helpers) and C4819 (non-ASCII author
// names in the FFmpeg headers) are the same two the library's FfmpegCommon.h
// silences; the plug-in cannot include that internal header, so the same
// suppression is repeated here around the same includes.
#pragma warning(disable : 4244 4819)
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>

namespace osv::premiere {

namespace {

/// Human readable FFmpeg error text (never empty).
[[nodiscard]] std::string ffError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, buf, sizeof(buf)) < 0) {
        return "ffmpeg error " + std::to_string(code);
    }
    return std::string(buf);
}

/// How far ahead of a requested position a seek is still not worth doing:
/// decoding forward through a second of audio is cheaper than a seek plus a
/// re-prime, and avoids the discontinuity a seek can introduce.
constexpr std::int64_t kMaxForwardScanSamples = 48000 * 2;

/// How far BEFORE the requested sample the seek aims, so the decoder has
/// real packets to warm up on.
///
/// AAC is an MDCT codec with 50 % overlap: every output frame is the sum of
/// the second half of the previous frame's window and the first half of this
/// one.  After avcodec_flush_buffers() that history is gone, so the first
/// frame decoded at a seek point is NOT the same as the frame a continuous
/// decode would produce there - close, but not bit-identical, which is
/// exactly what made a random read disagree with the sequential walk over
/// the same range.  Decoding a couple of packets before the target rebuilds
/// the overlap state and makes the two paths agree sample for sample.
///
/// 4096 samples is four AAC-LC frames at 1024 samples each: three more than
/// the one frame the overlap strictly needs, which also covers the SBR /
/// PS delay lines a HE-AAC stream would have.  The cost is one extra packet
/// decode per seek.
constexpr std::int64_t kSeekPrerollSamples = 4096;

}  // namespace

// ---------------------------------------------------------------------------
//  Impl
// ---------------------------------------------------------------------------

struct AudioDecoder::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int streamIndex = -1;

    std::int32_t channels = 0;
    double sampleRate = 0.0;
    std::int64_t durationSamples = 0;

    /// Planar float ring of already-decoded samples: buffer[ch] holds
    /// `available` samples starting at absolute frame `bufferStart`.
    std::vector<std::vector<float>> buffer;
    std::int64_t bufferStart = 0;   ///< Absolute sample frame of buffer[ch][0].
    std::size_t bufferOffset = 0;   ///< Consumed samples at the front of the ring.

    /// Absolute sample frame the next decoded packet will produce.  -1 while
    /// the position after a seek is still unknown.
    std::int64_t decodePosition = 0;
    bool eof = false;

    /// Cursor for imGetSequentialAudio.
    std::int64_t sequentialPosition = 0;

    ~Impl() { close(); }

    void close() noexcept {
        if (resampler) {
            swr_free(&resampler);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (codec) {
            avcodec_free_context(&codec);
        }
        if (format) {
            avformat_close_input(&format);
        }
    }

    /// Samples currently held in the ring from bufferStart + bufferOffset on.
    [[nodiscard]] std::size_t ringAvailable() const noexcept {
        if (buffer.empty() || buffer[0].size() <= bufferOffset) {
            return 0;
        }
        return buffer[0].size() - bufferOffset;
    }

    /// Absolute frame of the first unconsumed sample in the ring.
    [[nodiscard]] std::int64_t ringPosition() const noexcept {
        return bufferStart + static_cast<std::int64_t>(bufferOffset);
    }

    /// Forget every buffered sample (after a seek).
    void dropRing() noexcept {
        for (auto& ch : buffer) {
            ch.clear();
        }
        bufferOffset = 0;
        bufferStart = 0;
    }

    /// Drop consumed samples from the front so the ring does not grow without
    /// bound during a long sequential conform.
    void compactRing() noexcept {
        if (bufferOffset == 0) {
            return;
        }
        for (auto& ch : buffer) {
            if (bufferOffset >= ch.size()) {
                ch.clear();
            } else {
                ch.erase(ch.begin(), ch.begin() + static_cast<std::ptrdiff_t>(bufferOffset));
            }
        }
        bufferStart += static_cast<std::int64_t>(bufferOffset);
        bufferOffset = 0;
    }

    /// True once decodePosition is known to match the next decoded sample.
    /// Only used to make the intent of seekTo() explicit; the ring's own
    /// bufferStart is what readAt() trusts.
    bool pendingPositionKnown = false;

    /// Append the converted samples of `frame` to the ring.
    [[nodiscard]] Status appendFrame();

    /// Pull every frame the decoder is holding into the ring.  Errors are
    /// logged, not propagated: a single bad frame must not abort a conform.
    void drainDecoder() noexcept;

    /// Build (or rebuild) the swresample context for the stream's own format
    /// and rate.  Returns false when the context could not be created.
    [[nodiscard]] bool resetResampler() noexcept;

    /// Tear the decoder down and open a fresh one from the stream's
    /// parameters.  Returns false when it could not be re-opened (the caller
    /// then reports an error rather than decoding through a dead context).
    [[nodiscard]] bool reopenDecoder() noexcept;

    /// Decode until the ring holds at least `want` samples from its current
    /// position, or the stream ends.  Returns ok even at EOF; the caller
    /// checks ringAvailable().
    [[nodiscard]] Status fill(std::size_t want);

    /// Seek so the next decoded sample is at or before `position`, then
    /// discard forward to exactly `position`.  This is where AAC priming is
    /// handled: the seek lands on a packet boundary before the target and the
    /// decoded samples in between are thrown away.
    [[nodiscard]] Status seekTo(std::int64_t position);

    /// Fill `buffers` with `count` frames starting at `position`, zero-filling
    /// past the end of stream.
    [[nodiscard]] Status readAt(std::int64_t position, std::uint32_t count, float* const* buffers);
};

bool AudioDecoder::Impl::reopenDecoder() noexcept {
    if (!format || streamIndex < 0) {
        return false;
    }
    const AVStream* stream = format->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        return false;
    }
    if (codec) {
        avcodec_free_context(&codec);
    }
    codec = avcodec_alloc_context3(decoder);
    if (!codec) {
        return false;
    }
    if (avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
        return false;
    }
    codec->thread_count = 1;
    return avcodec_open2(codec, decoder, nullptr) >= 0;
}

bool AudioDecoder::Impl::resetResampler() noexcept {
    if (resampler) {
        swr_free(&resampler);
    }
    // Planar float at the native rate: exactly the layout Premiere wants, so
    // the conversion only changes the sample format and the interleaving.
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, channels);
    const int err = swr_alloc_set_opts2(&resampler, &outLayout, AV_SAMPLE_FMT_FLTP, codec->sample_rate,
                                        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (err < 0 || !resampler) {
        return false;
    }
    return swr_init(resampler) >= 0;
}

Status AudioDecoder::Impl::appendFrame() {
    if (!frame || frame->nb_samples <= 0) {
        return okStatus();
    }
    const int samples = frame->nb_samples;

    // swr_convert can emit more than it is handed (it never does for a plain
    // rate-preserving format conversion, but the API allows it), so ask for
    // the delay-corrected maximum and shrink afterwards.
    const std::int64_t maxOut = swr_get_out_samples(resampler, samples);
    const std::size_t room = static_cast<std::size_t>(maxOut > samples ? maxOut : samples);

    // Grow each plane, hand swresample the writable tails, then trim.
    const std::size_t oldSize = buffer.empty() ? 0 : buffer[0].size();
    std::vector<std::uint8_t*> planes(static_cast<std::size_t>(channels), nullptr);
    for (std::int32_t ch = 0; ch < channels; ++ch) {
        buffer[static_cast<std::size_t>(ch)].resize(oldSize + room);
        planes[static_cast<std::size_t>(ch)] =
            reinterpret_cast<std::uint8_t*>(buffer[static_cast<std::size_t>(ch)].data() + oldSize);
    }

    const int produced = swr_convert(resampler, planes.data(), static_cast<int>(room),
                                     const_cast<const std::uint8_t**>(frame->extended_data), samples);
    if (produced < 0) {
        for (auto& ch : buffer) {
            ch.resize(oldSize);
        }
        return Error{ErrorCode::Decoder, "swr_convert failed: " + ffError(produced)};
    }
    for (auto& ch : buffer) {
        ch.resize(oldSize + static_cast<std::size_t>(produced));
    }

    // The first appended block after a seek defines where the ring starts.
    if (oldSize == 0) {
        bufferStart = decodePosition;
        bufferOffset = 0;
    }
    decodePosition += produced;
    return okStatus();
}

void AudioDecoder::Impl::drainDecoder() noexcept {
    for (;;) {
        const int r = avcodec_receive_frame(codec, frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return;
        }
        if (r < 0) {
            PluginLog::oncef("audio-receive", PluginLog::Level::Warn,
                             "audio: avcodec_receive_frame failed ({})", ffError(r));
            return;
        }
        const Status st = appendFrame();
        av_frame_unref(frame);
        if (!st.ok()) {
            PluginLog::oncef("audio-append", PluginLog::Level::Warn, "audio: {}", st.error().message);
            return;
        }
    }
}

Status AudioDecoder::Impl::fill(std::size_t want) {
    while (ringAvailable() < want && !eof) {
        const int packetErr = av_read_frame(format, packet);
        if (packetErr == AVERROR_EOF) {
            // Flush the decoder: the last packets may still hold samples.
            avcodec_send_packet(codec, nullptr);
            for (;;) {
                const int r = avcodec_receive_frame(codec, frame);
                if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
                    break;
                }
                if (r < 0) {
                    return Error{ErrorCode::Decoder, "avcodec_receive_frame (flush) failed: " + ffError(r)};
                }
                Status st = appendFrame();
                av_frame_unref(frame);
                if (!st.ok()) {
                    return st;
                }
            }
            eof = true;
            break;
        }
        if (packetErr < 0) {
            return Error{ErrorCode::Io, "av_read_frame failed: " + ffError(packetErr)};
        }

        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        const int sendErr = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (sendErr < 0 && sendErr != AVERROR(EAGAIN)) {
            // A single corrupt packet must not kill the whole conform; skip it.
            PluginLog::oncef("audio-send-packet", PluginLog::Level::Warn,
                             "audio: avcodec_send_packet failed ({}); skipping the packet", ffError(sendErr));
            continue;
        }
        for (;;) {
            const int r = avcodec_receive_frame(codec, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                break;
            }
            if (r < 0) {
                return Error{ErrorCode::Decoder, "avcodec_receive_frame failed: " + ffError(r)};
            }
            Status st = appendFrame();
            av_frame_unref(frame);
            if (!st.ok()) {
                return st;
            }
        }
    }
    return okStatus();
}

Status AudioDecoder::Impl::seekTo(std::int64_t position) {
    if (position < 0) {
        position = 0;
    }
    // Seek in the stream's own time base.  AVSEEK_FLAG_BACKWARD lands on the
    // packet at or before the target so no requested sample is missed; the
    // samples between the packet start and `position` are the AAC priming /
    // pre-roll and are discarded by the caller once they are decoded.
    const AVStream* stream = format->streams[streamIndex];
    const AVRational sampleTb{1, static_cast<int>(sampleRate > 0.0 ? sampleRate : 48000.0)};
    // Aim the seek before the target so the decoder has packets to rebuild
    // its MDCT overlap state on (see kSeekPrerollSamples).  The samples
    // decoded ahead of `position` are the pre-roll and are dropped by
    // readAt(), together with the AAC priming that a packet-aligned seek
    // always produces.
    const std::int64_t preroll = std::max<std::int64_t>(0, position - kSeekPrerollSamples);
    const std::int64_t ts = av_rescale_q(preroll, sampleTb, stream->time_base);

    const int err = av_seek_frame(format, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (err < 0) {
        return Error{ErrorCode::Io, "av_seek_frame failed: " + ffError(err)};
    }

    // Both the decoder and the resampler are REBUILT, not flushed.
    //
    // avcodec_flush_buffers() is not enough for this stream: measured on the
    // 6K sample clip, decoding forward to sample 8192 and then seeking back
    // to 0 with a flush yields 9.30792e-05 for the first sample, while both a
    // fresh decode and a decode on a re-opened context yield 0.000244020 -
    // the correct value.  The AAC decoder keeps state across a flush that a
    // freshly opened one does not have, so a flush-based seek cannot be
    // bit-exact with a continuous decode, and Premiere would then get
    // different audio for the same range depending on whether it arrived by
    // conforming (sequential) or by scrubbing (random).
    //
    // swr_convert likewise keeps internal delay, and swresample has no public
    // "flush that keeps the configuration", so that context is rebuilt too.
    //
    // Both rebuilds are cheap - an AAC-LC context is a few kilobytes and the
    // resampler designs no filters for a rate-preserving format conversion -
    // and they happen only on a real seek, never on the sequential path.
    if (!reopenDecoder()) {
        return Error{ErrorCode::Decoder, "the audio decoder could not be re-opened after a seek"};
    }
    if (!resetResampler()) {
        return Error{ErrorCode::Internal, "the audio resampler could not be reset after a seek"};
    }

    dropRing();
    eof = false;

    // Where the seek ACTUALLY landed.  The requested timestamp is not a safe
    // answer: for a target past the end of the stream FFmpeg clamps the seek
    // to the last packet, whose real time is far below `ts`.  Labelling those
    // samples with `ts` would claim the stream reaches a position it never
    // does, and readAt() would then copy the tail of the clip as if it were
    // the requested (nonexistent) range instead of zero-filling.
    //
    // So the position is taken from the first packet of OUR stream after the
    // seek, and that packet is remembered for the decoder rather than being
    // thrown away.
    decodePosition = 0;
    pendingPositionKnown = false;
    for (;;) {
        const int r = av_read_frame(format, packet);
        if (r == AVERROR_EOF) {
            // Nothing left at all: the ring stays empty and every read past
            // this point is silence, which is correct.
            eof = true;
            decodePosition = position;
            break;
        }
        if (r < 0) {
            return Error{ErrorCode::Io, "av_read_frame after seek failed: " + ffError(r)};
        }
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        const std::int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        decodePosition = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, stream->time_base, sampleTb) : 0;
        // Hand this packet to the decoder now; fill() would otherwise read
        // the NEXT one and lose it.
        const int sendErr = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (sendErr < 0 && sendErr != AVERROR(EAGAIN)) {
            PluginLog::oncef("audio-seek-send", PluginLog::Level::Warn,
                             "audio: the first packet after a seek was rejected ({})", ffError(sendErr));
        }
        drainDecoder();
        break;
    }
    return okStatus();
}

Status AudioDecoder::Impl::readAt(std::int64_t position, std::uint32_t count, float* const* buffers) {
    if (!buffers) {
        return Error{ErrorCode::InvalidArgument, "audio read with a null buffer array"};
    }
    if (count == 0) {
        return okStatus();
    }
    if (position < 0) {
        position = 0;
    }

    // Decide whether to seek.  Reading straight on from the cursor (the
    // sequential case, and repeated scrubs in the same area) needs no seek at
    // all; a small forward jump is cheaper to decode through than to seek,
    // and avoids the re-prime a seek costs.
    const bool ringEmpty = buffer.empty() || buffer[0].empty();
    const std::int64_t here = ringEmpty ? decodePosition : ringPosition();
    const bool needSeek = ringEmpty || position < here ||
                          position > here + static_cast<std::int64_t>(ringAvailable()) + kMaxForwardScanSamples;
    if (needSeek) {
        Status st = seekTo(position);
        if (!st.ok()) {
            return st;
        }
    }

    // Decode until the ring covers [position, position + count) or the stream
    // ends.  Everything in the ring has a known absolute position, so the
    // amount still missing at the front is computed rather than assumed.
    //
    // This loop is what makes both access patterns exact.  The earlier
    // version skipped forward with a clamped std::min, which silently served
    // whatever the ring happened to hold when the requested range lay past
    // the end of the stream (the seek lands on the LAST packet, decodes real
    // samples from well before `position`, and the clamp then copied them as
    // if they were the requested ones - the bug that made a past-EOS read
    // return audio instead of silence, and made a random read of a late block
    // disagree with the sequential walk).
    for (;;) {
        const std::int64_t ringStart = ringPosition();
        const std::int64_t ringEnd = ringStart + static_cast<std::int64_t>(ringAvailable());
        if (ringEnd >= position + static_cast<std::int64_t>(count) || eof) {
            break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<std::int64_t>(
            position + static_cast<std::int64_t>(count) - ringStart, 1 << 20));
        const std::size_t before = ringAvailable();
        Status st = fill(want);
        if (!st.ok()) {
            return st;
        }
        if (ringAvailable() == before) {
            break;  // No progress: the stream really has ended.
        }
    }

    // Drop everything before `position` (the priming samples after a seek,
    // or the gap after a forward scan).  Never more than the ring holds.
    const std::int64_t lead = position - ringPosition();
    if (lead > 0) {
        bufferOffset += static_cast<std::size_t>(std::min<std::int64_t>(lead, static_cast<std::int64_t>(ringAvailable())));
    }

    // How many of the requested samples are genuinely available AT
    // `position`.  If the ring does not actually start there (the stream
    // ended first), nothing is copied and the whole buffer is silence.
    std::size_t have = 0;
    if (ringPosition() == position) {
        have = std::min<std::size_t>(ringAvailable(), count);
    }

    for (std::int32_t ch = 0; ch < channels; ++ch) {
        float* dst = buffers[static_cast<std::size_t>(ch)];
        if (!dst) {
            continue;  // The host never does this; tolerate it anyway.
        }
        const auto& src = buffer[static_cast<std::size_t>(ch)];
        if (have > 0 && bufferOffset + have <= src.size()) {
            std::memcpy(dst, src.data() + bufferOffset, have * sizeof(float));
        }
        // Zero-fill past the end of stream: the host asks for whole buffers
        // even at the tail of a clip, and silence is the right answer.
        if (have < count) {
            std::memset(dst + have, 0, (static_cast<std::size_t>(count) - have) * sizeof(float));
        }
    }

    bufferOffset += have;
    compactRing();
    return okStatus();
}

// ---------------------------------------------------------------------------
//  AudioDecoder
// ---------------------------------------------------------------------------

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() = default;
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;

Result<AudioDecoder> AudioDecoder::open(const std::filesystem::path& path) {
    auto impl = std::make_unique<Impl>();

    // Its own AVFormatContext (and therefore its own OS handle) so video
    // seeks and audio conforming never disturb each other.
    const std::string utf8 = path.string();
    int err = avformat_open_input(&impl->format, utf8.c_str(), nullptr, nullptr);
    if (err < 0) {
        return Error{ErrorCode::Io, "avformat_open_input failed: " + ffError(err)};
    }
    err = avformat_find_stream_info(impl->format, nullptr);
    if (err < 0) {
        return Error{ErrorCode::Malformed, "avformat_find_stream_info failed: " + ffError(err)};
    }

    const AVCodec* decoder = nullptr;
    impl->streamIndex = av_find_best_stream(impl->format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (impl->streamIndex < 0 || !decoder) {
        return Error{ErrorCode::NotFound, "the clip carries no decodable audio stream"};
    }

    AVStream* stream = impl->format->streams[impl->streamIndex];
    impl->codec = avcodec_alloc_context3(decoder);
    if (!impl->codec) {
        return Error{ErrorCode::Internal, "avcodec_alloc_context3 failed"};
    }
    err = avcodec_parameters_to_context(impl->codec, stream->codecpar);
    if (err < 0) {
        return Error{ErrorCode::Malformed, "avcodec_parameters_to_context failed: " + ffError(err)};
    }
    // One thread: an AAC stereo stream decodes far faster than real time and
    // a worker pool inside a host that already runs many importer threads
    // only adds contention.
    impl->codec->thread_count = 1;
    err = avcodec_open2(impl->codec, decoder, nullptr);
    if (err < 0) {
        return Error{ErrorCode::Decoder, "avcodec_open2 failed: " + ffError(err)};
    }

    impl->channels = impl->codec->ch_layout.nb_channels;
    impl->sampleRate = static_cast<double>(impl->codec->sample_rate);
    if (impl->channels <= 0 || impl->sampleRate <= 0.0) {
        return Error{ErrorCode::Malformed, "the audio stream reports no channels or no sample rate"};
    }

    // Duration in sample frames from the stream's own duration.
    if (stream->duration > 0) {
        const AVRational sampleTb{1, impl->codec->sample_rate};
        impl->durationSamples = av_rescale_q(stream->duration, stream->time_base, sampleTb);
    } else if (impl->format->duration > 0) {
        impl->durationSamples =
            static_cast<std::int64_t>(static_cast<double>(impl->format->duration) / AV_TIME_BASE * impl->sampleRate);
    }

    // Planar float at the native rate (the same helper a seek uses to rebuild
    // the context, so the two paths cannot drift apart).
    if (!impl->resetResampler()) {
        return Error{ErrorCode::Internal, "the audio resampler could not be created"};
    }

    impl->packet = av_packet_alloc();
    impl->frame = av_frame_alloc();
    if (!impl->packet || !impl->frame) {
        return Error{ErrorCode::Internal, "av_packet_alloc / av_frame_alloc failed"};
    }
    impl->buffer.resize(static_cast<std::size_t>(impl->channels));

    AudioDecoder out;
    out.m_impl = std::move(impl);
    return out;
}

bool AudioDecoder::isOpen() const noexcept { return m_impl && m_impl->codec != nullptr; }

std::int32_t AudioDecoder::channels() const noexcept { return m_impl ? m_impl->channels : 0; }

double AudioDecoder::sampleRate() const noexcept { return m_impl ? m_impl->sampleRate : 0.0; }

std::int64_t AudioDecoder::durationSamples() const noexcept { return m_impl ? m_impl->durationSamples : 0; }

Status AudioDecoder::read(std::int64_t position, std::uint32_t count, float* const* buffers) {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "audio read on a closed decoder"};
    }
    return m_impl->readAt(position, count, buffers);
}

Status AudioDecoder::readSequential(std::uint32_t count, float* const* buffers) {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "sequential audio read on a closed decoder"};
    }
    Status st = m_impl->readAt(m_impl->sequentialPosition, count, buffers);
    if (st.ok()) {
        m_impl->sequentialPosition += count;
    }
    return st;
}

void AudioDecoder::resetSequential() {
    if (m_impl) {
        m_impl->sequentialPosition = 0;
    }
}

std::int64_t AudioDecoder::sequentialPosition() const noexcept { return m_impl ? m_impl->sequentialPosition : 0; }

}  // namespace osv::premiere
