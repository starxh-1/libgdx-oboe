#include "oboe_engine.hpp"
#include "../utility/ptrptr.hpp"
#include "../utility/log.hpp"
#include "../utility/exception.hpp"
#include <array>
#include <algorithm>
#include <iterator>
#include <limits>
#include <cassert>

namespace {
/// @note: message should contain {}
inline bool check(oboe::Result result, std::string_view msg) {
    if (result != oboe::Result::OK) {
        warn(msg, oboe::convertToText(result));
        return false;
    }
    return true;
}
/// Clears a bool flag when leaving the scope (reentrancy guard helper).
struct flag_reset {
    bool& flag;
    ~flag_reset() { flag = false; }
};
}

// Process-wide latch: once AAudio has been proven broken on this device/ROM it
// stays broken for the lifetime of the process, across every engine instance.
std::atomic<bool> oboe_engine::s_opensl_fallback{false};

oboe_engine::oboe_engine(mode mode, uint8_t channels, uint32_t sample_rate)
        : oboe::AudioStreamDataCallback()
        , oboe::AudioStreamErrorCallback()
        , m_mode(mode)
        , m_channels(channels)
        , m_sample_rate(sample_rate)
        , m_payload_size(0)
        , m_is_playing(false)
        , m_consecutive_errors(0)
        , m_last_reconnect_time(std::chrono::steady_clock::now()) {
    connect_to_device();
}

oboe_engine::~oboe_engine() {
    std::unique_ptr<oboe::AudioStream> stream;
    {
        std::lock_guard<std::mutex> lock(m_stream_mutex);
        stream = std::move(m_stream);
    }
    if (!stream)
        return;

    stream->requestStop();
    check(stream->close(), "Error closing stream: {}");
}

void oboe_engine::connect_to_device() {
    // Serialise against resume()/stop() so we never free m_stream underneath
    // another thread. Recursive: Oboe can deliver onErrorAfterClose() on this
    // same thread from inside close(), which would re-enter this method -- the
    // m_reconnecting guard below stops that from recursing.
    std::lock_guard<std::recursive_mutex> lifecycle(m_lifecycle_mutex);
    if (m_reconnecting)
        return;
    m_reconnecting = true;
    flag_reset reset{m_reconnecting};

    // --- Loop detection and circuit breaker ---
    // If we reconnect faster than once every 1.5s, count it; after 3 rapid
    // reconnects force OpenSL ES to break an AAudio disconnect loop.
    auto now = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_last_reconnect_time).count();

    if (duration_ms < 1500) {
        m_consecutive_errors++;
        if (m_consecutive_errors >= 3 && !s_opensl_fallback.exchange(true)) {
            warn("Detected rapid reconnection loop ({} times in {}ms). "
                 "Triggering OpenSL ES fallback.", m_consecutive_errors, duration_ms);
        }
    } else {
        m_consecutive_errors = 0;
    }
    m_last_reconnect_time = now;

    // Close any existing (disconnected) stream before opening a new one so we
    // don't leak the previous handle.
    {
        std::lock_guard<std::mutex> lock(m_stream_mutex);
        if (m_stream) {
            m_stream->close();
            m_stream.reset();
        }
    }

    // initialize Oboe audio stream
    oboe::AudioStreamBuilder builder;
    builder.setChannelCount(m_channels);
    builder.setSampleRate(static_cast<int32_t>(m_sample_rate));
    builder.setErrorCallback(this);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setFormatConversionAllowed(true);
    // Prefer AAudio; fall back to OpenSL ES either on open failure or once the
    // circuit breaker (see resume()/connect_to_device()) has latched it.
    builder.setAudioApi(s_opensl_fallback.load()
                        ? oboe::AudioApi::OpenSLES
                        : oboe::AudioApi::AAudio);

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

    oboe::Result result = builder.openStream(ptrptr(m_stream));
    if (result != oboe::Result::OK || !m_stream ||
        m_stream->getState() == oboe::StreamState::Disconnected) {
        if (m_stream) { m_stream->close(); m_stream.reset(); }
        if (!s_opensl_fallback.exchange(true)) {
            warn("AAudio initialization failed, falling back to OpenSL ES.");
            builder.setAudioApi(oboe::AudioApi::OpenSLES);
            result = builder.openStream(ptrptr(m_stream));
        }
        if (result != oboe::Result::OK || !m_stream) {
            error("FATAL: All audio backend initialization failed!");
            return;
        }
    }

    // Read back the actual stream sample rate. With setFormatConversionAllowed(true),
    // Oboe may open at the hardware native rate instead of the requested one.
    auto actual_rate = static_cast<uint32_t>(m_stream->getSampleRate());
    if (actual_rate > 0) {
        m_sample_rate = actual_rate;
    }

    m_payload_size = m_stream->getFramesPerBurst() * 2;
    m_stream->setBufferSizeInFrames(static_cast<int32_t>(m_payload_size));

    info("Stream opened: API={}, State={}, OpenSL_fallback={}",
         oboe::convertToText(m_stream->getAudioApi()),
         oboe::convertToText(m_stream->getState()),
         s_opensl_fallback.load());
}

void oboe_engine::onErrorAfterClose(oboe::AudioStream *self, oboe::Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        info("Previous device disconnected. Trying to connect to a new one...");
        connect_to_device();
        if (m_is_playing) {
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

    return oboe::DataCallbackResult::Continue;
}

void oboe_engine::resume() {
    // At most two attempts: one on the current backend, and one after rebuilding
    // the stream on OpenSL ES.
    //
    // This is the primary fallback trigger point. On ROMs with a broken AAudio
    // service (e.g. API 29 Mokee) openStream() succeeds and the stream reports
    // State::Open, and the failure only shows up here: requestStart() returns
    // ErrorDisconnected / ErrorInvalidState / ErrorNull. Oboe hands that error
    // straight back to us and never invokes onErrorAfterClose(), so a fallback
    // that only lives in connect_to_device()/onErrorAfterClose() can never fire.
    //
    // Holding the lifecycle mutex across both attempts also guarantees the
    // rebuild cannot free m_stream while another thread is inside requestStart().
    std::lock_guard<std::recursive_mutex> lifecycle(m_lifecycle_mutex);
    for (int attempt = 0; attempt < 2; ++attempt) {
        oboe::AudioStream* stream = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_stream_mutex);
            stream = m_stream.get();
        }
        if (!stream)
            return;

        debug("oboe_engine::resume. State: {}", oboe::convertToText(stream->getState()));

        if (check(stream->requestStart(), "Error starting stream: {}")) {
            m_is_playing = true;
            return;
        }

        // requestStart() failed. Latch the fallback; exchange() returns the
        // previous value, so if it was already true we are on OpenSL ES already
        // (or another thread just switched) and retrying would loop forever.
        if (s_opensl_fallback.exchange(true)) {
            warn("requestStart() failed on OpenSL ES as well; giving up.");
            return;
        }

        warn("requestStart() failed on AAudio; rebuilding the stream on OpenSL ES.");
        connect_to_device();
    }
}

void oboe_engine::stop() {
    std::lock_guard<std::recursive_mutex> lifecycle(m_lifecycle_mutex);
    oboe::AudioStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stream_mutex);
        stream = m_stream.get();
    }
    if (!stream)
        return;

    debug("stop::resume. State: {}", oboe::convertToText(stream->getState()));

    if (check(stream->requestStop(), "Error stopping stream: {}")) {
        m_is_playing = false;
    }
}

void oboe_engine::blocking_write(const int16_t* pcm, size_t len) {
    android_assert(m_mode == mode::writing,
                   "engine not in writing mode, something went wrong.");

    oboe::AudioStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stream_mutex);
        stream = m_stream.get();
    }
    if (!stream)
        return;

    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto frames = stream->write(pcm, len_in_frames, std::numeric_limits<int64_t>::max());
    check(frames, "Error while reading stream: {}");
}

void oboe_engine::blocking_read(int16_t* buffer, size_t len) {
    android_assert(m_mode == mode::reading,
                   "engine not in writing mode, something went wrong.");

    oboe::AudioStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stream_mutex);
        stream = m_stream.get();
    }
    if (!stream)
        return;

    int32_t len_in_frames = static_cast<int32_t>(len) / m_channels;
    auto frames = stream->read(buffer, len_in_frames, std::numeric_limits<int64_t>::max());

    check(frames, "Error while writing into stream: {}");
    if (frames && frames.value() < len_in_frames) {
        std::fill(std::next(buffer, frames.value() * m_channels),
                  std::next(buffer, static_cast<int32_t>(len)),
                  0);
    }
}

uint32_t oboe_engine::payload_size() const {
    return m_payload_size * m_channels;
}