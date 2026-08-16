#include "oboe_engine.hpp"
#include "../utility/ptrptr.hpp"
#include "../utility/log.hpp"
#include "../utility/exception.hpp"
#include <array>
#include <algorithm>
#include <iterator>
#include <limits>
#include <cassert>
#include <sys/system_properties.h>
#include <stdlib.h>

// Detect 64-bit architecture for buffer size adjustment
#if defined(__LP64__) || defined(__aarch64__) || defined(__amd64__) || defined(__x86_64__)
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
        , m_is_playing(false)
        , m_consecutive_errors(0)
        , m_use_opensl_fallback(false)
        , m_last_reconnect_time(std::chrono::steady_clock::now()) {
    connect_to_device();
}

oboe_engine::~oboe_engine() {
    std::shared_ptr<oboe::AudioStream> snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        snapshot = m_stream;
    }
    if (!snapshot)
        return;

    stop();
    check(snapshot->close(), "Error closing stream: {}");
}

void oboe_engine::connect_to_device() {
    std::lock_guard<std::mutex> reconnect_lock(m_reconnect_mutex);

    auto create_builder = [this](oboe::SharingMode sharing_mode, oboe::AudioApi api) {
        oboe::AudioStreamBuilder builder;
        builder.setChannelCount(m_channels);
        builder.setSampleRate(static_cast<int32_t>(m_sample_rate.load(std::memory_order_acquire)));
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
        // If we are in emergency fallback mode, only allow OpenSL ES
        if (m_use_opensl_fallback && api != oboe::AudioApi::OpenSLES) {
            return false;
        }

        auto builder = create_builder(sharing, api);
        std::shared_ptr<oboe::AudioStream> new_stream;
        oboe::Result result = builder.openStream(new_stream);
        if (result != oboe::Result::OK || !new_stream ||
            new_stream->getState() == oboe::StreamState::Disconnected) {
            if (new_stream) new_stream->close();
            return false;
        }

        // Atomically swap the new stream in. Any in-flight audio callback
        // that already snapshotted m_stream holds its own shared_ptr, so
        // the old stream stays alive until close() below returns. Order:
        // swap under m_stream_mutex, then close the old stream outside the
        // lock so close() can block on callback completion.
        std::shared_ptr<oboe::AudioStream> old;
        {
            std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
            old = std::move(m_stream);
            m_stream = std::move(new_stream);
        }
        if (old) {
            old->close();
        }
        return true;
    };

    // --- Loop detection and circuit breaker ---
    auto now = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_reconnect_time).count();

    if (duration_ms < 1500) { // If reconnecting faster than once every 1.5 seconds
        m_consecutive_errors++;
        if (m_consecutive_errors >= 3) {
            warn("Detected rapid reconnection loop ({} times in {}ms). Triggering OpenSL ES fallback.",
                 m_consecutive_errors, duration_ms);
            m_use_opensl_fallback = true;
        }
    } else {
        // Reset counter if enough time has passed without errors
        m_consecutive_errors = 0;
    }
    m_last_reconnect_time = now;

    bool success = false;

    // Strategy 1: AAudio Shared only. Exclusive bypasses the system mixer, so
    // screen recorders and MediaProjection cannot capture this app's audio.
    if (!m_use_opensl_fallback) {
        if (try_open(oboe::SharingMode::Shared, oboe::AudioApi::AAudio)) success = true;
    }

    // Strategy 2: OpenSL ES (Universal fallback)
    if (!success) {
        if (m_use_opensl_fallback) {
            info("Using mandatory OpenSL ES fallback to break the loop.");
        } else {
            warn("AAudio initialization failed, falling back to OpenSL ES.");
        }

        if (!try_open(oboe::SharingMode::Shared, oboe::AudioApi::OpenSLES)) {
            error("FATAL: All audio backend initialization failed!");
            return;
        }
    }

    // Snapshot the freshly installed stream to read its actual parameters.
    std::shared_ptr<oboe::AudioStream> snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        snapshot = m_stream;
    }
    if (!snapshot) return;

    info("Stream opened: API={}, Sharing={}, State={}, Reconnects={}",
         oboe::convertToText(snapshot->getAudioApi()),
         snapshot->getSharingMode() == oboe::SharingMode::Exclusive ? "Exclusive" : "Shared",
         oboe::convertToText(snapshot->getState()),
         m_consecutive_errors);

    // Read back the actual stream sample rate after opening. With
    // setFormatConversionAllowed(true), some Oboe/Android versions may silently
    // open at the hardware's native rate (e.g. 44100) instead of the requested
    // rate (e.g. 48000). Using the real rate ensures decoded PCM matches playback
    // speed. Fall back to requested rate if getSampleRate() is unavailable.
    auto actual_rate = static_cast<uint32_t>(snapshot->getSampleRate());
    if (actual_rate > 0) {
        auto requested = m_sample_rate.load(std::memory_order_acquire);
        if (actual_rate != requested) {
            info("Sample rate adjusted: requested={}, actual={}", requested, actual_rate);
        }
        m_sample_rate.store(actual_rate, std::memory_order_release);
    }

    // Calculate buffer multiplier - 32-bit devices use 4, 64-bit uses 2
    int32_t burst_multiplier = IS_LOW_POWER_DEVICE ? 4 : 2;
    auto frames_per_burst = static_cast<uint32_t>(snapshot->getFramesPerBurst());
    auto payload_frames = frames_per_burst * burst_multiplier;
    m_payload_size.store(payload_frames, std::memory_order_release);
    debug("oboe_engine buffer: burst={}, multiplier={}, total={} frames",
          frames_per_burst, burst_multiplier, payload_frames);
    snapshot->setBufferSizeInFrames(static_cast<int32_t>(payload_frames));
}

void oboe_engine::rebuild_stream() {
    debug("oboe_engine::rebuild_stream called.");
    stop();
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        if (m_stream) {
            m_stream->close();
            m_stream.reset();
        }
    }
    connect_to_device();
}

void oboe_engine::onErrorAfterClose(oboe::AudioStream *self, oboe::Result error) {
    if (error != oboe::Result::ErrorDisconnected) return;

    info("Audio device disconnected. Reconnecting...");
    connect_to_device();

    bool playing = m_is_playing.load(std::memory_order_acquire);
    std::shared_ptr<oboe::AudioStream> snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        snapshot = m_stream;
    }
    if (playing && snapshot) {
        resume();
    }
}

oboe::DataCallbackResult oboe_engine::onAudioReady(oboe::AudioStream *self, void *audio_data,
                                                   int32_t num_frames) {
    android_assert(m_mode == mode::async_writing,
                   "engine not in async_writing mode, something went wrong.");

    // Snapshot the stream pointer under the lock. The shared_ptr extends the
    // stream's lifetime for this entire callback, so a concurrent reconnect
    // can safely swap-and-close the old stream without affecting us.
    std::shared_ptr<oboe::AudioStream> stream_snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        stream_snapshot = m_stream;
    }

    // Cache hardware frame count once at callback start for sync_timing
#if IS_LOW_POWER_DEVICE
    // 32-bit: use manual frame counter (avoid getFramesRead overhead on OpenSL ES)
    m_frames_read.fetch_add(static_cast<uint64_t>(num_frames), std::memory_order_acq_rel);
#else
    if (stream_snapshot) {
        m_frames_read.store(static_cast<uint64_t>(stream_snapshot->getFramesRead()),
                            std::memory_order_release);
    }
#endif

    if (num_frames > 0 && m_on_async_write) {
        auto& pcm_queue = m_on_async_write(static_cast<uint32_t>(num_frames * m_channels));
        auto audio_out = static_cast<int16_t*>(audio_data);
        int32_t write_size = std::min(static_cast<int32_t>(pcm_queue.size()), num_frames * m_channels);

        if (write_size != 0) {
            std::copy(pcm_queue.begin(),
                      std::next(pcm_queue.begin(), write_size),
                      audio_out);
        }

        if (write_size < num_frames) {
            std::fill(std::next(audio_out, write_size),
                      std::next(audio_out, num_frames * m_channels),
                      0);
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void oboe_engine::resume() {
    std::shared_ptr<oboe::AudioStream> stream_snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        stream_snapshot = m_stream;
    }
    if (!stream_snapshot) {
        connect_to_device();
        {
            std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
            stream_snapshot = m_stream;
        }
        if (!stream_snapshot) return;
    }

    oboe::StreamState state = stream_snapshot->getState();
    debug("oboe_engine::resume. Current State: {}", oboe::convertToText(state));

    // 1. Skip if already starting or started
    if (state == oboe::StreamState::Starting || state == oboe::StreamState::Started) {
        return;
    }

    // 2. Handle intermediate/broken states
    if (state == oboe::StreamState::Disconnected || state == oboe::StreamState::Closed) {
        warn("Detected {} state in resume(), rebuilding...", oboe::convertToText(state));
        rebuild_stream();
        {
            std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
            stream_snapshot = m_stream;
        }
        if (!stream_snapshot) return;
        state = stream_snapshot->getState();
    }

    // 3. Handle Stopping state (Wait or Reset)
    if (state == oboe::StreamState::Stopping) {
        debug("Stream is Stopping, waiting for Stopped state...");
        auto result = stream_snapshot->waitForStateChange(state, &state, 100 * 1000000); // 100ms
        if (result != oboe::Result::OK || state == oboe::StreamState::Stopping) {
            warn("Stream stuck in Stopping. Hard resetting.");
            rebuild_stream();
            {
                std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
                stream_snapshot = m_stream;
            }
            if (!stream_snapshot) return;
        }
    }

    // 4. Final attempt to start
    oboe::Result result = stream_snapshot->requestStart();
    if (result != oboe::Result::OK) {
        warn("Error starting stream: {}. Attempting emergency rebuild.", oboe::convertToText(result));
        rebuild_stream();
        {
            std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
            stream_snapshot = m_stream;
        }
        if (stream_snapshot) stream_snapshot->requestStart();
    } else {
        m_is_playing.store(true, std::memory_order_release);
        // Successful start eventually clears error counter if it stays stable
    }
}

void oboe_engine::stop() {
    std::shared_ptr<oboe::AudioStream> stream_snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        stream_snapshot = m_stream;
    }
    if (!stream_snapshot) return;

    oboe::StreamState state = stream_snapshot->getState();
    debug("oboe_engine::stop. Current State: {}", oboe::convertToText(state));

    if (state == oboe::StreamState::Stopping || state == oboe::StreamState::Stopped) {
        m_is_playing.store(false, std::memory_order_release);
        return;
    }

    if (check(stream_snapshot->requestStop(), "Error stopping stream: {}")) {
        m_is_playing.store(false, std::memory_order_release);
    }
}

void oboe_engine::blocking_write(const int16_t* pcm, size_t len) {
    android_assert(m_mode == mode::writing, "engine not in writing mode.");
    std::shared_ptr<oboe::AudioStream> stream_snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        stream_snapshot = m_stream;
    }
    if (!stream_snapshot) return;
    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto result = stream_snapshot->write(pcm, len_in_frames, std::numeric_limits<int64_t>::max());
    check(result, "Error while writing stream: {}");
}

void oboe_engine::blocking_read(int16_t* buffer, size_t len) {
    android_assert(m_mode == mode::reading, "engine not in writing mode.");
    std::shared_ptr<oboe::AudioStream> stream_snapshot;
    {
        std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
        stream_snapshot = m_stream;
    }
    if (!stream_snapshot) return;
    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto result = stream_snapshot->read(buffer, len_in_frames, std::numeric_limits<int64_t>::max());
    check(result, "Error while reading from stream: {}");
    if (result && result.value() < len_in_frames) {
        std::fill(std::next(buffer, result.value() * m_channels),
                  std::next(buffer, static_cast<int32_t>(len)), 0);
    }
}

uint32_t oboe_engine::payload_size() const {
    return m_payload_size.load(std::memory_order_acquire) * m_channels;
}

int32_t oboe_engine::get_audio_session_id() const {
    std::lock_guard<std::mutex> stream_lock(m_stream_mutex);
    return m_stream ? m_stream->getSessionId() : 0;
}