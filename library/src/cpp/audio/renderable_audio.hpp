#pragma once

#include <cstdint>

/// Interface for writing audio into raw audio stream.
class renderable_audio {
public:
    /// Render this audio into the stream for num_frames of frames.
    virtual void render(int16_t* stream, uint32_t num_frames) = 0;

    /// Whether this source could produce any sample in the current frame.
    ///
    /// audio_player::generate_audio() walks every registered source on each
    /// callback. In a BMS setup every wav file is its own soundpool, so m_tracks
    /// holds thousands of entries while only a few dozen actually sound at any
    /// moment. Without this check each callback clears the mix buffer and runs a
    /// full render pass for every idle pool -- hundreds of MB/s of pointless
    /// memory writes at ~250 Hz, all on the realtime audio thread while holding
    /// audio_player's spinlock.
    ///
    /// Defaults to true (conservative: rather over-render than miss audio).
    /// soundpool overrides it to "the pool still has live or pending voices".
    virtual bool active() const { return true; }

    /// Synchronize timing from the audio engine.
    /// @param sample_rate The actual sample rate of the audio engine
    /// @param engine_frames Total frames read by the engine since start
    virtual void sync_timing(uint32_t sample_rate, uint64_t engine_frames) {}

protected:
    virtual ~renderable_audio() = default;
};