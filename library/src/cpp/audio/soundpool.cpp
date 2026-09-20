#include "soundpool.hpp"
#include <iterator>
#include <algorithm>
#include <thread>
#include "../samplerate/pcmtypes.hpp"
#include "../utility/log.hpp"

#if defined(__LP64__) || defined(__aarch64__) || defined(__x86_64__) || defined(__amd64__)
    #define IS_LOW_POWER_DEVICE 0
#else
    #define IS_LOW_POWER_DEVICE 1
#endif

soundpool::soundpool(const data_t &pcm, int8_t channels)
        : m_last_id(0)
        , m_frames(pcm.size() / channels)
        , m_channels(channels)
        , m_pcm(to_float(pcm))
        , m_rendering_flag(false)
        , m_pending_flag(false)
{
#if IS_LOW_POWER_DEVICE
    // 32-bit: pre-allocate to avoid realloc per frame in render()
    m_sample_buffer.reserve(4096);
#endif
}

void soundpool::do_by_id(long id, const std::function<void(
        std::vector<soundpool::sound>::iterator)> &callback) {
    // 1) m_pending —— play() 里刚排进去、音频回调还没提升成 voice 的声音。
    //    必须一起扫:play() 只往列表追加,真正的 voice 要等下一次回调才出现在
    //    m_sounds 里(一个回调 ≈ 10ms),这期间只扫 m_sounds 就是"看不见它"。
    //    两个 flag 分开拿、先释放再拿下一个,不与 render() 形成锁序反转
    //    (render() 的顺序是 rendering → pending,且它对 pending 是非阻塞获取)。
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    {
        auto iter = std::find_if(m_pending.begin(), m_pending.end(),
                                 [id](const soundpool::sound &sound) {
                                     return sound.m_id == id;
                                 });
        if (iter != m_pending.end()) {
            callback(iter);
        }
    }
    m_pending_flag.clear(std::memory_order_release);

    // 2) m_sounds —— 已经在混音的声音。
    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }
    {
        auto iter = std::find_if(m_sounds.begin(), m_sounds.end(),
                                 [id](const soundpool::sound &sound) {
                                     return sound.m_id == id;
                                 });
        if (iter != m_sounds.end()) {
            callback(iter);
        }
    }
    m_rendering_flag.clear(std::memory_order_release);
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
    // Lock-free pending list. UI thread never blocks on audio.
    sound s = gen_sound(volume, pan, speed, loop);
    long id = s.m_id;
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    m_pending.push_back(std::move(s));
    m_pending_flag.clear(std::memory_order_release);
    return id;
}

void soundpool::pause() {
    // pending 也要盖到:pending 里的 voice 之后会被 render() 提升并直接出声,
    // 只 pause m_sounds 的话它会"漏出来"。
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (auto &sound : m_pending) {
        sound.m_paused = true;
    }
    m_pending_flag.clear(std::memory_order_release);

    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }
    for (auto &sound : m_sounds) {
        sound.m_paused = true;
    }
    m_rendering_flag.clear(std::memory_order_release);
}

void soundpool::pause(long id) {
    do_by_id(id, [](auto sound) { sound->m_paused = true; });
}

void soundpool::resume() {
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (auto &sound : m_pending) {
        sound.m_paused = false;
    }
    m_pending_flag.clear(std::memory_order_release);

    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }
    for (auto &sound : m_sounds) {
        sound.m_paused = false;
    }
    m_rendering_flag.clear(std::memory_order_release);
}

void soundpool::resume(long id) {
    do_by_id(id, [](auto sound) { sound->m_paused = false; });
}

void soundpool::stop() {
    // 顺序不能反:pending 先清、m_sounds 后清。反过来的话,两步之间被 render()
    // 提升进 m_sounds 的 voice 会漏掉,stop 之后仍然出声。
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    m_pending.clear();
    m_pending_flag.clear(std::memory_order_release);

    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }
    m_sounds.clear();
    m_rendering_flag.clear(std::memory_order_release);
}

void soundpool::stop(long id) {
    // erase 需要知道迭代器属于哪个 vector,所以两个容器分开写,不能共用回调。
    //
    // pending 必须一起删 —— 这是 BMS「同一 #WAV 定义下标同时只响一个」语义的关键:
    // AbstractAudioDriver.play0() 对同 channel 走 stop(wav, channel) → play(...),
    // 而 channel = wavId*256 + pitch + 128 是按 #WAV 定义下标分的。play() 只是把
    // 新 voice 排进 m_pending,等下一次音频回调才提升进 m_sounds(≈10ms 窗口);
    // 两个"同一时刻"的 note 相隔只有微秒级,后一个的 stop 必然落在这个窗口里。
    // 只扫 m_sounds 就找不到前一个 voice → 两个一起出声(听感 = 200% 音量)。
    //
    // 注意:#WAV4D 与 #WAV4E 这种不同定义、同一音频文件的情况 id 不同、channel
    // 不同,本来就该同时出声 —— 所以这里只能按 id 判,不能按文件路径去重。
    while (m_pending_flag.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    {
        auto iter = std::find_if(m_pending.begin(), m_pending.end(),
                                 [id](const soundpool::sound &sound) {
                                     return sound.m_id == id;
                                 });
        if (iter != m_pending.end()) {
            m_pending.erase(iter);
        }
    }
    m_pending_flag.clear(std::memory_order_release);

    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }
    {
        auto iter = std::find_if(m_sounds.begin(), m_sounds.end(),
                                 [id](const soundpool::sound &sound) {
                                     return sound.m_id == id;
                                 });
        if (iter != m_sounds.end()) {
            m_sounds.erase(iter);
        }
    }
    m_rendering_flag.clear(std::memory_order_release);
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

bool soundpool::active() const {
    // Unsynchronised on purpose: this runs once per registered pool per callback
    // (thousands of times), so taking m_rendering_flag here would eat most of
    // what the skip saves. The read is safe because of how the writers are laid
    // out -- both failure modes are benign:
    //
    //   m_sounds  -- render() (this thread) is the only writer that grows it.
    //                The two cross-thread writers, stop() and stop(id), only
    //                ever shrink it. Racing them can at worst report "not empty"
    //                from a stale end pointer, which costs one extra render pass
    //                (that pass re-checks under m_rendering_flag and finds
    //                nothing). It can never report "empty" for a vector that
    //                still holds voices, so a sounding pool is never skipped.
    //   m_pending -- pushed by the UI thread. A read racing the push may say
    //                "empty" and skip this callback, but the entry is never
    //                dropped: the next callback sees it and consumes it.
    //                Worst case is one note starting one buffer (~4ms) late.
    //
    // So: never loses audio, never grows without bound. It is deliberately NOT
    // a substitute for the real check inside render().
    return !m_sounds.empty() || !m_pending.empty();
}

void soundpool::render(int16_t *audio_data, uint32_t num_frames) {
    static int limit_down = std::numeric_limits<int16_t>::min(),
            limit_up = std::numeric_limits<int16_t>::max();

    while (m_rendering_flag.test_and_set(std::memory_order_acquire)) {
        ;  // pure spin for lowest latency
    }

    // Consume the pending list handed over by the UI thread.
    //
    // play() pushes under m_pending_flag, so we must take that same flag before
    // touching m_pending. Without it, a push_back that happens to reallocate the
    // vector can free the storage we are iterating right now -- use-after-free
    // inside the audio callback. (It is rare because clear() keeps the capacity,
    // so it only bites when the list grows past its high-water mark; but a busy
    // section with a few hundred notes a second does reach it.)
    //
    // Taken non-blocking on purpose: this is the realtime thread, so if the UI
    // thread is mid-push we simply leave the list alone and pick it up on the
    // next callback (~4ms later). Never spin here.
    if (!m_pending.empty() &&
        !m_pending_flag.test_and_set(std::memory_order_acquire)) {
        for (auto& s : m_pending) {
            m_sounds.push_back(std::move(s));
        }
        m_pending.clear();
        m_pending_flag.clear(std::memory_order_release);
    }

    int prevaluated = 0;
    // resize 而不是 reserve:下面是从 begin() 起写 size * m_channels 个 float,
    // 而 reserve 只给容量、size 仍然是 0 —— 对空 vector 解引用 begin() 是未定义行为。
    // float 是 POD,reserve 出来的内存实践中能写,但没必要赌这个。
    // 只在不够时扩:扩展才会填 0,之后每帧只是一次比较,不产生额外开销。
    const size_t needed_samples = static_cast<size_t>(num_frames) * m_channels + 16;
    if (m_sample_buffer.size() < needed_samples) {
        m_sample_buffer.resize(needed_samples);
    }
    for (auto it = m_sounds.begin(); it != m_sounds.end();) {
        if (!it->m_paused) {
            auto iter = std::next(m_pcm.cbegin(), it->m_cur_frame * m_channels);
            const int size = std::min(num_frames, m_frames - it->m_cur_frame);

            int used_frames = it->m_resampler.process(iter, m_pcm.cend(), m_sample_buffer.begin(),
                                                      size);

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
                it->m_marked_for_delete = true;  // mark for batch delete instead of immediate erase
                ++it;
            }
        } else {
            ++it;
        }
    }
    // Batch delete marked sounds (single O(n) instead of per-sound O(n))
    m_sounds.erase(
        std::remove_if(m_sounds.begin(), m_sounds.end(),
                       [](const sound& s) { return s.m_marked_for_delete; }),
        m_sounds.end());

    m_rendering_flag.clear(std::memory_order_release);
}

