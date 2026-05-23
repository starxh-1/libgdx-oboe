#include "resampler.hpp"
#include "../utility/log.hpp"
#include "../utility/exception.hpp"

resampler::resampler(resampler::converter converter, int8_t channels, float ratio)
        : m_data(SRC_DATA{ .src_ratio = ratio })
        , m_channels(channels)
        , m_len(0) {
    int error = 0;
    m_state = src_state_ptr{ src_new(static_cast<int>(converter), channels, &error) };
    if (error) {
        throw_exception("resampler::resampler error: {}", src_strerror(error));
        m_state = nullptr;
    }
}

resampler::resampler(resampler &&other) noexcept {
    *this = std::move(other);
}

resampler &resampler::operator=(resampler &&other) noexcept {
    m_data = std::exchange(other.m_data, SRC_DATA{});
    m_state = std::exchange(other.m_state, nullptr);
    m_channels = std::exchange(other.m_channels, 0);
    m_len = std::exchange(other.m_len, 0);
    return *this;
}

float resampler::ratio() const {
    return static_cast<float>(m_data.src_ratio);
}

void resampler::ratio(float ratio) {
    m_data.src_ratio = ratio;
}

void resampler::reset() {
    src_reset(m_state.get());
}

int resampler::process(const float* data_in, int input_frames, float* data_out, int requested_frames) {
    if (m_state == nullptr) {
        int frames = std::min(input_frames, requested_frames);
        std::copy(data_in, data_in + frames * m_channels, data_out);
        return frames;
    } else {
        m_data.data_in = data_in;
        m_data.data_out = data_out;
        m_data.input_frames = input_frames;
        m_data.output_frames = requested_frames;
        m_data.end_of_input = requested_frames <= input_frames;

        if (int error = src_process(m_state.get(), &m_data)) {
            throw_exception("resampler::process error: {}", src_strerror(error));
        }

        return m_data.input_frames_used;
    }
}

int resampler::process(std::vector<float>::const_iterator begin,
                       std::vector<float>::const_iterator end,
                       std::vector<float>::iterator output, int requested_frames) {
    return process(&(*begin), std::distance(begin, end) / m_channels, &(*output), requested_frames);
}
