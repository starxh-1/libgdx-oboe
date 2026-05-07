#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include "../external/kissfft/kiss_fft.h"

class spectrum_analyzer {
public:
    static constexpr int FFT_SIZE = 1024;
    static constexpr int BANDS = 32;

    spectrum_analyzer() : m_write_pos(0) {
        m_cfg = kiss_fft_alloc(FFT_SIZE, 0, nullptr, nullptr);
        m_in_l.resize(FFT_SIZE); m_out_l.resize(FFT_SIZE);
        m_in_r.resize(FFT_SIZE); m_out_r.resize(FFT_SIZE);
        m_smoothed_l.resize(BANDS, 0.0f);
        m_smoothed_r.resize(BANDS, 0.0f);
        m_raw_l.resize(FFT_SIZE, 0);
        m_raw_r.resize(FFT_SIZE, 0);
        m_combined_result.resize(BANDS * 2, 0.0f);
        m_window.resize(FFT_SIZE);
        const float PI = 3.14159265358979f;
        for (int i = 0; i < FFT_SIZE; ++i) {
            m_window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
        }
    }

    ~spectrum_analyzer() { if (m_cfg) free(m_cfg); }

    void feed(const int16_t* pcm, size_t count, int channels) {
        int pos = m_write_pos.load(std::memory_order_relaxed);
        if (channels == 2) {
            for (size_t i = 0; i < count; i += 2) {
                m_raw_l[pos] = pcm[i];
                m_raw_r[pos] = pcm[i + 1];
                pos = (pos + 1) % FFT_SIZE;
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                m_raw_l[pos] = m_raw_r[pos] = pcm[i];
                pos = (pos + 1) % FFT_SIZE;
            }
        }
        m_write_pos.store(pos, std::memory_order_release);
    }

    const std::vector<float>& get_bands() {
        int end_pos = m_write_pos.load(std::memory_order_acquire);
        for (int i = 0; i < FFT_SIZE; ++i) {
            int idx = (end_pos + i) % FFT_SIZE;
            m_in_l[i].r = (m_raw_l[idx] / 32768.0f) * m_window[i]; m_in_l[i].i = 0;
            m_in_r[i].r = (m_raw_r[idx] / 32768.0f) * m_window[i]; m_in_r[i].i = 0;
        }

        if (m_cfg) {
            kiss_fft(m_cfg, m_in_l.data(), m_out_l.data());
            kiss_fft(m_cfg, m_in_r.data(), m_out_r.data());
        }

        int samples_per_band = (FFT_SIZE / 2 - 2) / BANDS;
        for (int b = 0; b < BANDS; ++b) {
            float sum_l = 0, sum_r = 0;
            for (int i = 0; i < samples_per_band; ++i) {
                int idx = 2 + b * samples_per_band + i;
                sum_l += sqrtf(m_out_l[idx].r * m_out_l[idx].r + m_out_l[idx].i * m_out_l[idx].i);
                sum_r += sqrtf(m_out_r[idx].r * m_out_r[idx].r + m_out_r[idx].i * m_out_r[idx].i);
            }
            // 采用非线性缩放 (sqrt) 增加动态范围，并降低增益系数
            process_band(b, sum_l / (float)samples_per_band, m_smoothed_l, 0);
            process_band(b, sum_r / (float)samples_per_band, m_smoothed_r, BANDS);
        }
        return m_combined_result;
    }

private:
    void process_band(int b, float avg, std::vector<float>& smoothed, int offset) {
        // 使用 sqrt 让频谱起伏更自然，增益调为 6.0f (配合 sqrt 后的数值)
        float val = std::min(1.0f, sqrtf(avg) * 6.5f);
        if (val > smoothed[b]) smoothed[b] = val;
        else smoothed[b] = smoothed[b] * 0.88f + val * 0.12f; // 稍微加快下降速度
        m_combined_result[offset + b] = smoothed[b];
    }

    kiss_fft_cfg m_cfg;
    std::vector<kiss_fft_cpx> m_in_l, m_out_l, m_in_r, m_out_r;
    std::vector<float> m_window, m_smoothed_l, m_smoothed_r, m_combined_result;
    std::vector<int16_t> m_raw_l, m_raw_r;
    std::atomic<int> m_write_pos;
};
