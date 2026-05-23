#include "soundpool.hpp"
#include <iterator>
#include <algorithm>
#include "../samplerate/pcmtypes.hpp"
#include "../utility/log.hpp"

soundpool::soundpool(const data_t &pcm, int8_t channels, bool low_memory_mode)
        : m_last_id(0)
        , m_frames(pcm.size() / channels)
        , m_channels(channels)
        , m_low_memory_mode(low_memory_mode) {
    if (m_low_memory_mode) {
        m_pcm_int16 = pcm;
        debug("soundpool: using low memory mode (int16 storage) for {} frames", m_frames);
    } else {
        m_pcm_float = to_float(pcm);
    }
}

void soundpool::do_by_id(long id, const std::function<void(
        std::vector<soundpool::sound>::iterator)> &callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto iter = std::find_if(m_sounds.begin(), m_sounds.end(), [id](const soundpool::sound &sound) {
        return sound.m_id == id;
    });
    if (iter != m_sounds.end()) {
        callback(iter);
    }
}

soundpool::sound soundpool::gen_sound(float volume, float pan, float speed, bool loop) {
    return sound{
            .m_cur_frame = 0,
            .m_paused = false,
            .m_id = ++m_last_id,
            .m_volume = volume,
            .m_looping = loop,
            .m_pan = pan_effect(pan),
            .m_resampler = resampler(resampler::converter::zero_order_hold, m_channels,
                                     1.f / std::clamp(speed, 0.5f, 2.0f))
    };
}

long soundpool::play(float volume, float speed, float pan, bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sounds.emplace_back(gen_sound(volume, pan, speed, loop));
    long id = m_sounds.back().m_id;
    return id;
}

void soundpool::pause() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &sound : m_sounds) {
        sound.m_paused = true;
    }
}

void soundpool::pause(long id) {
    do_by_id(id, [](auto sound) { sound->m_paused = true; });
}

void soundpool::resume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &sound : m_sounds) {
        sound.m_paused = false;
    }
}

void soundpool::resume(long id) {
    do_by_id(id, [](auto sound) { sound->m_paused = false; });
}

void soundpool::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sounds.clear();
}

void soundpool::stop(long id) {
    do_by_id(id, [&](auto sound) {
        m_sounds.erase(sound);
    });
}

void soundpool::volume(long id, float value) {
    do_by_id(id, [value](auto sound) { sound->m_volume = value; });
}

void soundpool::looping(long id, bool loop) {
    do_by_id(id, [loop](auto sound) { sound->m_looping = loop; });
}

void soundpool::speed(long id, float value) {
    do_by_id(id, [value](auto sound) {
        float safe_value = std::clamp(value, 0.5f, 2.0f);
        sound->m_resampler.ratio(1.f / safe_value);
    });
}

void soundpool::pan(long id, float value) {
    do_by_id(id, [value](auto sound) { sound->m_pan.pan(value); });
}

void soundpool::render(int16_t *audio_data, uint32_t num_frames) {
    static int limit_down = std::numeric_limits<int16_t>::min(),
            limit_up = std::numeric_limits<int16_t>::max();

    std::lock_guard<std::mutex> lock(m_mutex);
    int prevaluated = 0;
    m_sample_buffer.reserve(num_frames * m_channels + 16);
    if (m_low_memory_mode) {
        m_convert_buffer.reserve(num_frames * m_channels * 2 + 16); // Reserve enough for resampler input
    }

    for (auto it = m_sounds.begin(); it != m_sounds.end();) {
        if (!it->m_paused) {
            const int size = std::min(num_frames, m_frames - it->m_cur_frame);
            int used_frames;

            if (m_low_memory_mode) {
                // Ensure convert buffer is large enough for the input chunk
                int samples_to_convert = size * m_channels;
                if (m_convert_buffer.size() < samples_to_convert) {
                    m_convert_buffer.resize(samples_to_convert);
                }
                src_short_to_float_array(m_pcm_int16.data() + it->m_cur_frame * m_channels,
                                         m_convert_buffer.data(), samples_to_convert);

                used_frames = it->m_resampler.process(m_convert_buffer.data(), size,
                                                      m_sample_buffer.data(), num_frames);
            } else {
                auto iter = std::next(m_pcm_float.cbegin(), it->m_cur_frame * m_channels);
                used_frames = it->m_resampler.process(&(*iter), size, m_sample_buffer.data(), num_frames);
            }

            auto buffer_iter = m_sample_buffer.begin();
            auto end = std::next(buffer_iter, size * m_channels);
            for (int i = 0; buffer_iter != end; ++buffer_iter, ++i) {
                prevaluated = static_cast<int>(audio_data[i]) +
                              static_cast<int>(*buffer_iter * limit_up * it->m_volume *
                                               it->m_pan.modulation(i % m_channels));
                audio_data[i] = static_cast<int16_t>(std::clamp(prevaluated, limit_down, limit_up));
            }

            it->m_cur_frame += used_frames;
        }

        if (it->m_cur_frame >= m_frames) {
            if (it->m_looping) {
                it->m_cur_frame = 0;
                it->m_resampler.reset();
            } else {
                it = m_sounds.erase(it);
            }
        } else {
            it++;
        }
    }
}
