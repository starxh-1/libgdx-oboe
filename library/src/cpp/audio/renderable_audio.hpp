#pragma once

#include <cstdint>

/// Interface for writing audio into raw audio stream.
class renderable_audio {
public:
    /// Render this audio into the stream for num_frames of frames.
    virtual void render(int16_t* stream, uint32_t num_frames) = 0;

    /// Synchronize timing from the audio engine.
    /// @param sample_rate The actual sample rate of the audio engine
    /// @param engine_frames Total frames read by the engine since start
    virtual void sync_timing(uint32_t sample_rate, uint64_t engine_frames) {}

protected:
    virtual ~renderable_audio() = default;
};