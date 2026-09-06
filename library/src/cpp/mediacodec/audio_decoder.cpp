#include <tuple>
#include <algorithm>
#include "audio_decoder.hpp"
#include "../utility/log.hpp"

extern "C" {
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

audio_decoder::audio_decoder(decoder_bundle &&bundle)
        : m_format_ctx(std::move(bundle.m_format_ctx))
        , m_codec_ctx(std::move(bundle.m_codec_ctx))
        , m_avio_ctx(std::move(bundle.m_avio_ctx))
        , m_swr_ctx(std::move(bundle.m_swr_ctx))
        , m_iframe(std::move(bundle.m_iframe))
        , m_oframe(std::move(bundle.m_oframe))
        , m_packet(std::move(bundle.m_packet)) { }

int audio_decoder::ensure_out_capacity(int64_t samples) {
    if (samples <= 0) {
        samples = 1;
    }

    // Capacity of the currently allocated buffer, in frames.
    if (m_oframe->linesize[0] > 0) {
        const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(m_oframe->format));
        const int ch = m_oframe->ch_layout.nb_channels;
        if (bps > 0 && ch > 0) {
            const int allocated = m_oframe->linesize[0] / (bps * ch);
            if (allocated >= samples) {
                return allocated;  // existing buffer is large enough, reuse it
            }
        }
    }

    // (Re)allocate. av_frame_get_buffer() needs an empty frame, and
    // av_frame_unref() wipes the format fields -- save and restore them.
    AVChannelLayout layout;
    if (int error = av_channel_layout_copy(&layout, &m_oframe->ch_layout)) {
        warn("audio_decoder: av_channel_layout_copy failed ({})", av_err_str(error));
        return 0;
    }
    const int format = m_oframe->format;
    const int rate = m_oframe->sample_rate;

    av_frame_unref(m_oframe.get());
    av_channel_layout_copy(&m_oframe->ch_layout, &layout);
    m_oframe->format = format;
    m_oframe->sample_rate = rate;
    m_oframe->nb_samples = static_cast<int>(samples);
    if (int error = av_frame_get_buffer(m_oframe.get(), 0)) {
        warn("audio_decoder: Failed to allocate the resampler output frame ({})",
             av_err_str(error));
        av_channel_layout_uninit(&layout);
        return 0;
    }
    av_channel_layout_uninit(&layout);

    const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(m_oframe->format));
    const int ch = m_oframe->ch_layout.nb_channels;
    return m_oframe->linesize[0] / (bps * ch);
}

audio_decoder::buffer audio_decoder::decode(int samples) {
    while (m_use_flag.test_and_set(std::memory_order_acquire));
    int64_t delay = 0;
    int processed_samples = 0, error = 0, data_size;
    bool read_eof = false, decode_eof = false, request_more = true;
    if (samples > 0) {
        m_buffer.reserve(samples);
    }
    m_buffer.clear();

    // use cache from last decode
    if (!m_cache.empty()) {
        processed_samples = std::min(m_cache.size(), static_cast<size_t>(samples));
        auto begin = m_cache.begin(), end = std::next(m_cache.begin(), processed_samples);
        std::move(begin, end, std::back_inserter(m_buffer));
        m_cache.erase(begin, end);
        request_more = processed_samples < samples;
    }

    while (request_more && !(read_eof && decode_eof)) {
        if (!read_eof) {
            error = av_read_frame(m_format_ctx.get(), m_packet.get());
            decode_eof = false;
            if (error == AVERROR_EOF) {
                read_eof = true;
            } else if (error < 0) {
                warn("audio_decoder: Read frame error ({})", av_err_str(error));
            }

            // we were seeking, but seek wasn't precise.
            // so we need to drop some frames. sanity checks first.
            if (m_packet->pts > 0 && m_target_ts > 0 && m_packet->pts < m_target_ts) {
                auto stream = m_format_ctx->streams[m_packet->stream_index];
                auto delta =
                        static_cast<float>(m_target_ts - m_packet->pts) / stream->time_base.den;
                int64_t skip_frames =
                        delta * m_codec_ctx->time_base.den / m_codec_ctx->time_base.num;
//                debug("{} of {} (diff {}, delta {}): skip {} frames", m_packet->pts, m_target_ts, m_target_ts - m_packet->pts,delta, skip_frames);

                // AV_PKT_DATA_SKIP_SAMPLES layout:
                // u32le number of samples to skip from start of this packet
                // u32le number of samples to skip from end of this packet
                // u8    reason for start skip
                // u8    reason for end   skip (0=padding silence, 1=convergence)
                uint8_t *data = av_packet_get_side_data(m_packet.get(), AV_PKT_DATA_SKIP_SAMPLES,
                                                        nullptr);
                if (!data) {
                    data = av_packet_new_side_data(m_packet.get(), AV_PKT_DATA_SKIP_SAMPLES, 10);
                }

                // fixme: force little endian skip_frames
                *reinterpret_cast<uint32_t *>(data) = skip_frames;
                data[8] = 0;
            }

            if ((error = avcodec_send_packet(m_codec_ctx.get(),
                                           read_eof ? nullptr : m_packet.get()))) {
                warn("audio_decoder: Error sending packets ({})", av_err_str(error));
                break;
            }
        }

        while (!decode_eof) {
            error = avcodec_receive_frame(m_codec_ctx.get(), m_iframe.get());
            if (error == 0) {
                // Only configure the resampler when it is not initialized yet.
                // swr_config_frame() unconditionally closes the context (swr_close),
                // wiping the resampler's filter history and buffered delay. Calling
                // it per decoded frame resets the conversion state at every frame
                // boundary: harmless for 1:1 conversion (44100 Hz sources take the
                // direct path with no resampler), but audible artifacts at each
                // frame boundary when actually resampling (non-44100 Hz sources
                // converted to the 44100 Hz standard).
                if (!swr_is_initialized(m_swr_ctx.get())) {
                    swr_config_frame(m_swr_ctx.get(), m_oframe.get(), m_iframe.get());
                }

                // swr_convert_frame() uses out->nb_samples as the output
                // capacity and overwrites it with the produced count on
                // return. If the capacity is left at the previous call's
                // output count, variable input frame sizes (OGG Vorbis:
                // 64..8192 samples/frame) can leave it stuck below the
                // steady-state need -- each call then only partially consumes
                // its input and the remainder piles up inside swr's internal
                // buffer indefinitely: the buffer keeps growing (repeated
                // realloc + O(backlog) copies on every call -> CPU spikes in
                // the decode thread, which the audio callback busy-waits on)
                // and the audio latency creeps up. Restore the capacity so
                // every call drains the buffered delay plus the whole input
                // frame in one go.
                int64_t needed = swr_get_delay(m_swr_ctx.get(), m_oframe->sample_rate)
                        + (static_cast<int64_t>(m_iframe->nb_samples) * m_oframe->sample_rate
                           + m_iframe->sample_rate - 1) / m_iframe->sample_rate
                        + 32;
                if (int capacity = ensure_out_capacity(needed)) {
                    m_oframe->nb_samples = capacity;
                }

                error = swr_convert_frame(m_swr_ctx.get(), m_oframe.get(), m_iframe.get());
                if (error == AVERROR_INPUT_CHANGED || error == AVERROR_OUTPUT_CHANGED) {
                    // Stream parameters changed mid-file: reconfigure once and retry.
                    swr_config_frame(m_swr_ctx.get(), m_oframe.get(), m_iframe.get());
                    error = swr_convert_frame(m_swr_ctx.get(), m_oframe.get(), m_iframe.get());
                }

                if (error < 0) {
                    warn("audio_decoder: Error converting demuxed data ({})", av_err_str(error));
                } else if ((data_size = m_oframe->nb_samples * m_oframe->ch_layout.nb_channels) > 0) {
                    auto begin = reinterpret_cast<int16_t *>(m_oframe->extended_data[0]),
                            end = begin + data_size;
                    std::move(begin, end, std::back_inserter(m_buffer));
                    processed_samples += data_size;
                }

                if (samples > 0 && processed_samples >= samples) {
                    request_more = false;
                }
            } else if (error == AVERROR(EAGAIN)) {
                break;
            } else if (error == AVERROR_EOF) {
                decode_eof = true;
            } else {
                warn("audio_decoder: Error while trying to receive a frame from the decoder ({})",
                      av_err_str(error));
                break;
            }
        }

        av_packet_unref(m_packet.get());
    }

    if ((m_eof = read_eof & decode_eof)) {
        // Flush the resampler's internal delay once at real EOF so the tail
        // of the sound is not cut off (a few filter taps worth of samples).
        // Done before the caching step below so the flushed tail also honors
        // the "samples" contract (excess goes to the cache).
        while ((delay = swr_get_delay(m_swr_ctx.get(), m_oframe->sample_rate)) > 0) {
            // Restore the output capacity (see the comment in the decode
            // loop): nb_samples currently holds the previous call's produced
            // count, which may be smaller than the delay still buffered.
            if (int capacity = ensure_out_capacity(delay + 32)) {
                m_oframe->nb_samples = capacity;
            }
            if (swr_convert_frame(m_swr_ctx.get(), m_oframe.get(), nullptr) < 0) {
                break;
            }
            if ((data_size = m_oframe->nb_samples * m_oframe->ch_layout.nb_channels) == 0) {
                break;
            }
            auto begin = reinterpret_cast<int16_t *>(m_oframe->extended_data[0]),
                    end = begin + data_size;
            std::move(begin, end, std::back_inserter(m_buffer));
            processed_samples += data_size;
        }
        avcodec_flush_buffers(m_codec_ctx.get());
    }

    if (samples > 0 && processed_samples > samples) {
        // cache anything past requested
        auto begin = std::next(m_buffer.begin(), samples), end = m_buffer.end();
        std::move(begin, end, std::back_inserter(m_cache));
        m_buffer.resize(samples);
    }
    m_use_flag.clear(std::memory_order_release);

    return std::move(m_buffer);
}

audio_decoder::buffer audio_decoder::decode() {
    return decode(-1);
}

void audio_decoder::seek(float seconds) {
    while (m_use_flag.test_and_set(std::memory_order_acquire));

    auto stream = m_format_ctx->streams[m_packet->stream_index];
    m_target_ts = av_rescale_q(seconds * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);

    m_cache.clear();
    m_eof = false;
    // Reset the resampler state (swr_init calls clear_context internally on an
    // already-configured context): drop the ~half-a-filter worth of samples
    // still buffered from before the seek, so playback resumes exactly at the
    // target position instead of leaking a sub-millisecond of the old audio.
    if (swr_is_initialized(m_swr_ctx.get())) {
        swr_init(m_swr_ctx.get());
    }
    avcodec_flush_buffers(m_codec_ctx.get());
    if (int error = av_seek_frame(m_format_ctx.get(), m_packet->stream_index, m_target_ts,
                                AVSEEK_FLAG_BACKWARD)) {
        warn("audio_decoder: Error while seeking ({})", av_err_str(error));
    }
    m_use_flag.clear(std::memory_order_release);
}

bool audio_decoder::is_eof() const {
    return m_eof;
}
