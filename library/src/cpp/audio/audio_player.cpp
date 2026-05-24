#include "audio_player.hpp"
#include "../utility/log.hpp"
#include <algorithm>
#include <limits>

#if defined(__LP64__) || defined(__aarch64__) || defined(__x86_64__) || defined(__amd64__)
    #define IS_LOW_POWER_DEVICE 0
#else
    #define IS_LOW_POWER_DEVICE 1
#endif

namespace {
// Constants for audio processing
}

audio_player::audio_player()
    : audio_player(48000) {
}

audio_player::audio_player(uint32_t sample_rate)
    : m_engine(oboe_engine::mode::async_writing, 2, sample_rate)
    , m_volume(1.0f) {
    m_engine.set_on_async_write([this](uint32_t num_samples) -> const std::vector<int16_t>& {
        return generate_audio(num_samples);
    });
}

void audio_player::play_audio(const std::shared_ptr<renderable_audio> &audio) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_tracks.emplace_back(audio);
}

const std::vector<int16_t>& audio_player::generate_audio(uint32_t num_samples) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // The parameter num_samples is already total samples (frames * channels)
    const uint32_t total_samples = num_samples;
    const uint32_t num_frames = total_samples / m_engine.channels();

    m_pcm.clear();
    m_pcm.resize(total_samples, 0);
    m_buffer.resize(total_samples, 0);

    bool is_dirty = false;

    for (const auto &weak_track : m_tracks) {
        if (auto track = weak_track.lock()) {
            // Use the most accurate frame count from the engine for synchronization
            track->sync_timing(m_engine.sample_rate(), m_engine.frames_read());

            std::fill(m_buffer.begin(), m_buffer.begin() + total_samples, 0);
            track->render(m_buffer.data(), num_frames);

#if IS_LOW_POWER_DEVICE
            // 32-bit device path: optimized mixing
            for (uint32_t i = 0; i < total_samples; ++i) {
                int32_t mixed = static_cast<int32_t>(m_pcm[i]) + static_cast<int32_t>(m_buffer[i]);
                if (mixed < -32768) mixed = -32768;
                else if (mixed > 32767) mixed = 32767;
                m_pcm[i] = static_cast<int16_t>(mixed);
            }
#else
            // 64-bit device path: standard int64_t mixing
            for (uint32_t i = 0; i < total_samples; ++i) {
                int64_t mixed = static_cast<int64_t>(m_pcm[i]) + static_cast<int64_t>(m_buffer[i]);
                if (mixed < -32768) mixed = -32768;
                else if (mixed > 32767) mixed = 32767;
                m_pcm[i] = static_cast<int16_t>(mixed);
            }
#endif
        } else {
            is_dirty = true;
        }
    }

    if (is_dirty) {
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                      [](const std::weak_ptr<renderable_audio>& track) {
                                          return track.expired();
                                      }), m_tracks.end());
    }

    if (m_volume != 1.0f) {
#if IS_LOW_POWER_DEVICE
        constexpr int32_t VOL_SHIFT = 12;
        int32_t vol_fixed = static_cast<int32_t>(m_volume * (1 << VOL_SHIFT));
        for (auto& pcm_bit : m_pcm) {
            int32_t scaled = (static_cast<int32_t>(pcm_bit) * vol_fixed) >> VOL_SHIFT;
            if (scaled < -32768) pcm_bit = -32768;
            else if (scaled > 32767) pcm_bit = 32767;
            else pcm_bit = static_cast<int16_t>(scaled);
        }
#else
        for (auto& pcm_bit : m_pcm) {
            pcm_bit = static_cast<int16_t>(static_cast<float>(pcm_bit) * m_volume);
        }
#endif
    }

    m_analyzer.feed(m_pcm.data(), total_samples, m_engine.channels());

    return m_pcm;
}
