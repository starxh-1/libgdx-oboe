#include "oboe_engine.hpp"
#include "../utility/log.hpp"
#include "../utility/exception.hpp"
#include <array>
#include <algorithm>
#include <iterator>
#include <limits>
#include <cassert>

// Detect 32-bit ARM for buffer size adjustment
#if defined(__aarch64__) || defined(__amd64__) || defined(__x86_64__)
    #define IS_LOW_POWER_DEVICE 0
#else
    #define IS_LOW_POWER_DEVICE 1
#endif

namespace {
/// @note: message should contain {}
inline bool check(oboe::Result result, std::string_view msg) {
    if (result != oboe::Result::OK) {
        warn(msg, oboe::convertToText(result));
        return false;
    }
    return true;
}
}

oboe_engine::oboe_engine(mode mode, uint8_t channels, uint32_t sample_rate)
        : oboe::AudioStreamDataCallback()
        , oboe::AudioStreamErrorCallback()
        , m_mode(mode)
        , m_channels(channels)
        , m_sample_rate(sample_rate)
        , m_payload_size(0)
        , m_is_playing(false) {
    connect_to_device();
}

oboe_engine::~oboe_engine() {
    if (!m_stream)
        return;

    stop();
    check(m_stream->close(), "Error closing stream: {}");
}

void oboe_engine::connect_to_device() {
    auto create_builder = [this](oboe::SharingMode sharing_mode, oboe::AudioApi api) {
        oboe::AudioStreamBuilder builder;
        builder.setChannelCount(m_channels);
        builder.setSampleRate(static_cast<int32_t>(m_sample_rate));
        builder.setErrorCallback(this);
        builder.setFormat(oboe::AudioFormat::I16);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(sharing_mode);
        builder.setAudioApi(api);
        builder.setFormatConversionAllowed(true);
        builder.setUsage(oboe::Usage::Game);

        switch(m_mode) {
            case mode::async_writing:
            case mode::writing: {
                builder.setContentType(oboe::ContentType::Music);
                builder.setDirection(oboe::Direction::Output);
                if (m_mode == mode::async_writing)
                    builder.setDataCallback(this);
            }
            break;
            case mode::reading: {
                builder.setDirection(oboe::Direction::Input);
                builder.setInputPreset(oboe::InputPreset::Generic);
            }
            break;
        }
        return builder;
    };

    auto try_open = [&](oboe::SharingMode sharing, oboe::AudioApi api) -> bool {
        auto builder = create_builder(sharing, api);
        oboe::Result result = builder.openStream(m_stream);
        if (result == oboe::Result::OK && m_stream && m_stream->getState() != oboe::StreamState::Disconnected) {
            return true;
        }
        if (m_stream) m_stream->close();
        m_stream.reset();
        return false;
    };

    // Attempt 1: AAudio Exclusive (Best)
    if (!try_open(oboe::SharingMode::Exclusive, oboe::AudioApi::AAudio)) {
        warn("AAudio Exclusive mode failed, trying AAudio Shared...");
        // Attempt 2: AAudio Shared
        if (!try_open(oboe::SharingMode::Shared, oboe::AudioApi::AAudio)) {
            warn("AAudio Shared mode failed, falling back to OpenSL ES...");
            // Attempt 3: OpenSL ES (Most compatible)
            if (!try_open(oboe::SharingMode::Shared, oboe::AudioApi::OpenSLES)) {
                error("All audio backend initialization failed!");
                return;
            }
        }
    }

    info("Stream opened: API={}, Sharing={}, State={}",
         oboe::convertToText(m_stream->getAudioApi()),
         m_stream->getSharingMode() == oboe::SharingMode::Exclusive ? "Exclusive" : "Shared",
         oboe::convertToText(m_stream->getState()));

    // multiplier 2 as requested by user
    int32_t burst_multiplier = 2;
    m_payload_size = m_stream->getFramesPerBurst() * burst_multiplier;
    debug("oboe_engine buffer: burst={}, multiplier={}, total={} frames",
          m_stream->getFramesPerBurst(), burst_multiplier, m_payload_size);
    m_stream->setBufferSizeInFrames(static_cast<int32_t>(m_payload_size));
}

void oboe_engine::onErrorAfterClose(oboe::AudioStream *self, oboe::Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        info("Audio device disconnected. Reconnecting...");
        connect_to_device();
        if (m_is_playing && m_stream) {
            resume();
        }
    }
}

oboe::DataCallbackResult oboe_engine::onAudioReady(oboe::AudioStream *self, void *audio_data,
                                                   int32_t num_frames) {
    android_assert(m_mode == mode::async_writing,
                   "engine not in async_writing mode, something went wrong.");

    if (num_frames > 0 && m_on_async_write) {
        auto& pcm_queue = m_on_async_write(static_cast<uint32_t>(num_frames * m_channels));
        auto stream = static_cast<int16_t*>(audio_data);
        int32_t write_size = std::min(static_cast<int32_t>(pcm_queue.size()), num_frames * m_channels);

        if (write_size != 0) {
            std::copy(pcm_queue.begin(),
                      std::next(pcm_queue.begin(), write_size),
                      stream);
        }

        if (write_size < num_frames) {
            std::fill(std::next(stream, write_size),
                      std::next(stream, num_frames * m_channels),
                      0);
        }
    }

    m_frames_read += num_frames;
    return oboe::DataCallbackResult::Continue;
}

void oboe_engine::resume() {
    if (!m_stream) {
        connect_to_device();
        if (!m_stream) return;
    }

    oboe::StreamState state = m_stream->getState();
    debug("oboe_engine::resume. Current State: {}", oboe::convertToText(state));

    if (state == oboe::StreamState::Disconnected) {
        warn("Detected Disconnected state in resume(), reconnecting...");
        connect_to_device();
        if (!m_stream) return;
    }

    oboe::Result result = m_stream->requestStart();
    if (result != oboe::Result::OK) {
        warn("Error starting stream: {}", oboe::convertToText(result));
        if (result == oboe::Result::ErrorDisconnected) {
            connect_to_device();
            if (m_stream) m_stream->requestStart();
        }
    } else {
        m_is_playing = true;
    }
}

void oboe_engine::stop() {
    if (!m_stream)
        return;

    debug("oboe_engine::stop. State: {}", oboe::convertToText(m_stream->getState()));
    if (check(m_stream->requestStop(), "Error stopping stream: {}")) {
        m_is_playing = false;
    }
}

void oboe_engine::blocking_write(const int16_t* pcm, size_t len) {
    android_assert(m_mode == mode::writing, "engine not in writing mode.");
    if (!m_stream) return;
    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto result = m_stream->write(pcm, len_in_frames, std::numeric_limits<int64_t>::max());
    check(result, "Error while writing stream: {}");
}

void oboe_engine::blocking_read(int16_t* buffer, size_t len) {
    android_assert(m_mode == mode::reading, "engine not in reading mode.");
    if (!m_stream) return;
    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto result = m_stream->read(buffer, len_in_frames, std::numeric_limits<int64_t>::max());
    check(result, "Error while reading from stream: {}");
    if (result && result.value() < len_in_frames) {
        std::fill(std::next(buffer, result.value() * m_channels),
                  std::next(buffer, static_cast<int32_t>(len)), 0);
    }
}

uint32_t oboe_engine::payload_size() const {
    return m_payload_size * m_channels;
}

uint64_t oboe_engine::frames_read() const {
    return m_frames_read;
}

int32_t oboe_engine::get_audio_session_id() const {
    return m_stream ? m_stream->getSessionId() : 0;
}
