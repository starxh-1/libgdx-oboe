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
int64_t k_limit_down = std::numeric_limits<int16_t>::min();
int64_t k_limit_up = std::numeric_limits<int16_t>::max();
}

audio_player::audio_player()
    : audio_player(48000) {
}

audio_player::audio_player(uint32_t sample_rate)
    : m_engine(oboe_engine::mode::async_writing, 2, sample_rate)
    , m_volume(1.0f)
    , m_rendering_flag(false) {
    m_engine.set_on_async_write([this](uint32_t num_frames) -> const std::vector<int16_t>& {
        return generate_audio(num_frames);
    });
}

void audio_player::play_audio(const std::shared_ptr<renderable_audio> &audio) {
    while (m_rendering_flag.test_and_set(std::memory_order_acquire));
    m_tracks.emplace_back(audio);
    m_rendering_flag.clear(std::memory_order_release);
}

const std::vector<int16_t>& audio_player::generate_audio(uint32_t num_frames) {
    while (m_rendering_flag.test_and_set(std::memory_order_acquire));

    m_pcm.clear();
    m_pcm.resize(num_frames, 0);
    m_buffer.resize(m_pcm.size(), 0);

    bool is_dirty = false;

    for (const auto &weak_track : m_tracks) {
        is_dirty |= weak_track.expired();
        if (auto track = weak_track.lock()) {
            track->sync_timing(m_engine.sample_rate(), m_engine.frames_read());

            std::fill(m_buffer.begin(), m_buffer.end(), 0);
            track->render(m_buffer.data(), num_frames / m_engine.channels());

            // Mix with int32 on 32-bit devices, int64 on 64-bit devices
#if IS_LOW_POWER_DEVICE
            for (uint32_t i = 0; i < m_pcm.size(); ++i) {
                int32_t mixed = static_cast<int32_t>(m_pcm[i]) + static_cast<int32_t>(m_buffer[i]);
                if (mixed < -32768) mixed = -32768;
                else if (mixed > 32767) mixed = 32767;
                m_pcm[i] = static_cast<int16_t>(mixed);
            }
#else
            for (uint32_t i = 0; i < m_pcm.size(); ++i) {
                int64_t mixed = static_cast<int64_t>(m_pcm[i]) + static_cast<int64_t>(m_buffer[i]);
                if (mixed < -32768) mixed = -32768;
                else if (mixed > 32767) mixed = 32767;
                m_pcm[i] = static_cast<int16_t>(mixed);
            }
#endif
        }
    }

#if IS_LOW_POWER_DEVICE
    // 32-bit: only cleanup periodically to avoid O(n) erase in every callback
    if (is_dirty) {
        m_cleanup_counter++;
        if (m_cleanup_counter >= CLEANUP_INTERVAL) {
            m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                          [](const std::weak_ptr<renderable_audio>& track) {
                                              return track.expired();
                                          }), m_tracks.end());
            m_cleanup_counter.store(0);
        }
    }
#else
    if (is_dirty) {
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                      [](const std::weak_ptr<renderable_audio>& track) {
                                          return track.expired();
                                      }), m_tracks.end());
    }
#endif

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
            float scaled = static_cast<float>(pcm_bit) * m_volume;
            if (scaled < -32768.0f) pcm_bit = -32768;
            else if (scaled > 32767.0f) pcm_bit = 32767;
            else pcm_bit = static_cast<int16_t>(scaled);
        }
#endif
    }

    m_analyzer.feed(m_pcm.data(), m_pcm.size(), m_engine.channels());

    m_rendering_flag.clear(std::memory_order_release);

    return m_pcm;
}
