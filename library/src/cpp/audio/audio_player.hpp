#pragma once

#include "renderable_audio.hpp"
#include "oboe_engine.hpp"
#include "spectrum_analyzer.hpp"
#include <memory>
#include <vector>
#include <atomic>

#if defined(__LP64__) || defined(__aarch64__) || defined(__x86_64__) || defined(__amd64__)
    #define IS_LOW_POWER_DEVICE 0
#else
    #define IS_LOW_POWER_DEVICE 1
#endif

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

    /// Get the sample rate used by the audio engine.
    uint32_t sample_rate() const { return 44100; }

    /// Get audio session ID for Visualizer.
    int32_t get_audio_session_id() const { return 0; }

    /// Get spectrum data
    const std::vector<float>& get_spectrum() { return m_analyzer.get_bands(); }

private:
    const std::vector<int16_t>& generate_audio(uint32_t num_frames);

    oboe_engine m_engine;
    spectrum_analyzer m_analyzer;
    float m_volume;

    std::vector<int16_t> m_pcm;
    std::vector<int16_t> m_buffer;
    std::vector<std::weak_ptr<renderable_audio>> m_tracks;

    // Spinlock for thread safety
    std::atomic_flag m_rendering_flag;

#if IS_LOW_POWER_DEVICE
    // 32-bit: periodic cleanup to avoid O(n) erase in every audio callback
    std::atomic<uint32_t> m_cleanup_counter{0};
    static constexpr uint32_t CLEANUP_INTERVAL = 64;  // cleanup every N callbacks
#endif
};
