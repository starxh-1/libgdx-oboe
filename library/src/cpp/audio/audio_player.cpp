#include "audio_player.hpp"
#include "../utility/log.hpp"
#include <algorithm>
#include <limits>

#if defined(__arm__) || defined(__i386__)
    #define IS_LOW_POWER_DEVICE 1
#else
    #define IS_LOW_POWER_DEVICE 0
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
    , m_volume(1.0f) {
    m_engine.set_on_async_write([this](uint32_t num_frames) -> const std::vector<int16_t>& {
        return generate_audio(num_frames);
    });
}

void audio_player::play_audio(const std::shared_ptr<renderable_audio> &audio) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_tracks.emplace_back(audio);
}

const std::vector<int16_t>& audio_player::generate_audio(uint32_t num_frames) {
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint32_t total_samples = num_frames * m_engine.channels();

    m_pcm.clear();
    m_pcm.resize(total_samples, 0);
    m_buffer.resize(total_samples, 0);

    bool is_dirty = false;

    for (const auto &weak_track : m_tracks) {
        is_dirty |= weak_track.expired();
        if (auto track = weak_track.lock()) {
            // Sync timing from engine for accurate position tracking
            track->sync_timing(m_engine.sample_rate(), m_engine.frames_read());

            std::fill(m_buffer.begin(), m_buffer.begin() + total_samples, 0);
            track->render(m_buffer.data(), num_frames / m_engine.channels());

#if IS_LOW_POWER_DEVICE
            // 32-bit optimization: use int32_t instead of int64_t for mixing
            constexpr int32_t limit_down = -32768;
            constexpr int32_t limit_up = 32767;
            for (uint32_t i = 0; i < total_samples; ++i) {
                int32_t mixed = static_cast<int32_t>(m_pcm[i]) + static_cast<int32_t>(m_buffer[i]);
                if (mixed < limit_down) mixed = limit_down;
                else if (mixed > limit_up) mixed = limit_up;
                m_pcm[i] = static_cast<int16_t>(mixed);
            }
#else
            int64_t prevaluated = 0;
            for (uint32_t i = 0; i < total_samples; ++i) {
                prevaluated = static_cast<int64_t>(m_pcm[i]) + static_cast<int64_t>(m_buffer[i]);
                m_pcm[i] = static_cast<int16_t>(std::clamp(prevaluated, k_limit_down, k_limit_up));
            }
#endif
        }
    }

    if (is_dirty) {
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                      [](const std::weak_ptr<renderable_audio>& track) {
                                          return track.expired();
                                      }), m_tracks.end());
    }

#if IS_LOW_POWER_DEVICE
    // 32-bit optimization: use fixed-point volume (fast integer multiply)
    if (m_volume != 1.0f) {
        constexpr int32_t VOL_SHIFT = 12;
        int32_t vol_fixed = static_cast<int32_t>(m_volume * (1 << VOL_SHIFT));
        for (uint32_t i = 0; i < total_samples; ++i) {
            int32_t scaled = (static_cast<int32_t>(m_pcm[i]) * vol_fixed) >> VOL_SHIFT;
            if (scaled < -32768) scaled = -32768;
            else if (scaled > 32767) scaled = 32767;
            m_pcm[i] = static_cast<int16_t>(scaled);
        }
    }
#else
    for (auto& pcm_bit : m_pcm) {
        pcm_bit = static_cast<int16_t>(static_cast<float>(pcm_bit) * m_volume);
    }
#endif

    m_analyzer.feed(m_pcm.data(), total_samples, m_engine.channels());

    return m_pcm;
}
