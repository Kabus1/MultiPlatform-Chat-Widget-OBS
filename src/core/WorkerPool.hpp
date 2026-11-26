#pragma once

// Small fixed-size thread pool for blocking work (network lookups). Jobs run
// concurrently, so one slow request never delays the jobs queued behind it.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace usc {

class WorkerPool {
public:
    explicit WorkerPool(size_t threads) : size_(threads ? threads : 1) {}
    ~WorkerPool() { stop(); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        running_ = true;
        for (size_t i = 0; i < size_; ++i) threads_.emplace_back([this] { run(); });
    }

    // Finishes the job each thread is running, drops the rest.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;
            jobs_.clear();
        }
        cv_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }

    // False if the pool is not running (the job is not queued).
    bool post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return false;
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
        return true;
    }

    // Jobs waiting for a thread (not counting the ones running).
    size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return !running_ || !jobs_.empty(); });
                if (!running_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (...) {
                // A failed job must never take the pool (or the app) down.
            }
        }
    }

    size_t size_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool running_ = false;
};

} // namespace usc
