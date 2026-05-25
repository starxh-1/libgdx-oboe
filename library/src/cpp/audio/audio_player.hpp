#pragma once

#include "renderable_audio.hpp"
#include "oboe_engine.hpp"
#include "spectrum_analyzer.hpp"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>

/// oboe_engine frontend for playing renderable_audio.
class audio_player {
public:
    /// Creates a default audio_player.
    audio_player();

    /// Creates an audio_player with specified sample rate.
    audio_player(uint32_t sample_rate);

    /// Plays a renderable_audio implementation.
    void play_audio(const std::shared_ptr<renderable_audio> &audio);

    /// Set volume of audio_player to value.
    void volume(float volume) { m_volume = std::clamp(volume, 0.0f, 1.0f); };
    /// Get volume of audio_player.
    float volume() const { return m_volume; }

    /// Stop any audio.
    void stop() { m_engine.stop(); }
    /// Resume playing audio.
    void resume() { m_engine.resume(); }

    /// Get audio session ID for Visualizer.
    int32_t get_audio_session_id() const { return m_engine.get_audio_session_id(); }

    /// Get spectrum data
    const std::vector<float>& get_spectrum() { return m_analyzer.get_bands(); }

    /// Check if currently in play mode (for spinlock vs mutex decision)
    bool is_play_mode() const { return m_play_mode.load(std::memory_order_acquire); }
    void set_play_mode(bool play) { m_play_mode.store(play, std::memory_order_release); }

private:
    const std::vector<int16_t>& generate_audio(uint32_t num_frames);

    oboe_engine m_engine;
    spectrum_analyzer m_analyzer;
    float m_volume;

    std::vector<int16_t> m_pcm;
    std::vector<int16_t> m_buffer;
    std::vector<std::weak_ptr<renderable_audio>> m_tracks;

    // Spinlock for play mode, mutex for non-play mode
    std::atomic_flag m_rendering_flag;
    std::mutex m_mutex;
    std::atomic<bool> m_play_mode{true};
};