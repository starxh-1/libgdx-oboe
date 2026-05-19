#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>

class executor {
public:
    executor(std::function<void()> job)
            : m_run(true)
            , m_done(true) // Start as done
            , m_job(std::move(job))
            , m_worker(&executor::run, this) { }

    ~executor() {
        m_run.store(false);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cond.notify_all();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void queue() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.store(false);
        }
        m_cond.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_done.wait(lock, [this] { return m_done.load(); });
    }

private:
    void run() {
        while (m_run.load()) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cond.wait(lock, [this] { return !m_run.load() || !m_done.load(); });
                if (!m_run.load()) break;
            }

            m_job();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_done.store(true);
            }
            m_cond_done.notify_all();
        }
    }

    std::atomic<bool> m_run;
    std::atomic<bool> m_done;
    std::function<void()> m_job;
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::condition_variable m_cond_done;
};
