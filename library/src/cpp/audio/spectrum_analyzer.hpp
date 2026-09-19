#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "../external/kissfft/kiss_fft.h"

class spectrum_analyzer {
public:
    static constexpr int FFT_SIZE = 512;
    static constexpr int BANDS = 32;

    spectrum_analyzer() : m_write_pos(0), m_last_feed_seq(0xFFFFFFFFu) {
        m_cfg = kiss_fft_alloc(FFT_SIZE, 0, nullptr, nullptr);
        m_in_l.resize(FFT_SIZE); m_out_l.resize(FFT_SIZE);
        m_in_r.resize(FFT_SIZE); m_out_r.resize(FFT_SIZE);
        m_smoothed_l.resize(BANDS, 0.0f);
        m_smoothed_r.resize(BANDS, 0.0f);
        m_raw_l.resize(FFT_SIZE, 0);
        m_raw_r.resize(FFT_SIZE, 0);
        m_combined_result.resize(BANDS * 2, 0.0f);
        m_band_avg_l.resize(BANDS, 0.0f);
        m_band_avg_r.resize(BANDS, 0.0f);
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
        // 递增"数据版本号"，供 get_bands() 判断自上次查询后是否有新 PCM。
        // 不能用 m_write_pos 当版本号：它是 FFT_SIZE 取模的位置，
        // 当每次回调的帧数恰好是 512 的整数倍时（framesPerBurst*2 == 512，
        // 例如 burst=256 的设备）会绕回原值，缓存就永远命中 → 频谱冻结。
        // 这里只是一次 relaxed 计数（无阻塞、无等待、不建立新的同步关系），
        // 音频回调线程的同步协议未变；release 保证与上一行 store 同样的发布语义。
        m_feed_seq.fetch_add(1, std::memory_order_release);
    }

    /**
     * 取频谱（32 左 + 32 右 = BANDS*2 个值）。
     *
     * 加窗 + 2×kiss_fft(512) + 32 段合成，只在「自上次查询以来有新 PCM」时执行一次；
     * 同一音频块内的重复查询直接复用上次的 band 值。
     *
     * 为什么这是等价优化（输出与逐次重算逐位相同）：
     * - 输入数据不变时，加窗/FFT/分段求和的结果本来就不会变（kiss_fft 是确定性的）；
     * - 平滑（process_band：抬升立即、下落 0.65:0.3）**仍然逐调用执行**，
     *   所以频谱的响应速度与下落速度与改动前完全一致 —— 省掉的只是重复的 FFT。
     *
     * 本函数只在查询线程（渲染线程，经 JNI 调用）执行，未给音频回调线程增加任何等待/同步。
     */
    const std::vector<float>& get_bands() {
        uint32_t seq = m_feed_seq.load(std::memory_order_acquire);
        if (seq != m_last_feed_seq) {
            m_last_feed_seq = seq;

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
                m_band_avg_l[b] = sum_l / (float)samples_per_band;
                m_band_avg_r[b] = sum_r / (float)samples_per_band;
            }
        }

        // 采用非线性缩放 (sqrt) 增加动态范围，并降低增益系数
        for (int b = 0; b < BANDS; ++b) {
            process_band(b, m_band_avg_l[b], m_smoothed_l, 0);
            process_band(b, m_band_avg_r[b], m_smoothed_r, BANDS);
        }
        return m_combined_result;
    }

private:
    void process_band(int b, float avg, std::vector<float>& smoothed, int offset) {
        // 使用 sqrt 让频谱起伏更自然，增益调为 0.25f
        float val = std::min(1.0f, sqrtf(avg) * 0.25f);
        if (val > smoothed[b]) smoothed[b] = val;
        else smoothed[b] = smoothed[b] * 0.65f + val * 0.3f; // 稍微加快下降速度
        m_combined_result[offset + b] = smoothed[b];
    }

    kiss_fft_cfg m_cfg;
    std::vector<kiss_fft_cpx> m_in_l, m_out_l, m_in_r, m_out_r;
    std::vector<float> m_window, m_smoothed_l, m_smoothed_r, m_combined_result;
    std::vector<float> m_band_avg_l, m_band_avg_r; // 最近一次 FFT 的 32 段均值（左右各一份）
    std::vector<int16_t> m_raw_l, m_raw_r;
    std::atomic<int> m_write_pos;
    uint32_t m_last_feed_seq;             // 最近一次参与计算的 m_feed_seq（仅查询线程读写）
    std::atomic<uint32_t> m_feed_seq{0};  // 每收到一块 PCM 自增，作为"数据版本号"
};
