#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Serializes blocking device-management calls without occupying the GUI thread.
class TaskQueue {
public:
    TaskQueue() : thread_([this] { run(); }) {}
    ~TaskQueue() { stop(); }
    void submit(std::function<void()> job) {
        { std::lock_guard lock(mutex_); if (stopping_) return; jobs_.push_back(std::move(job)); }
        wake_.notify_one();
    }
    void requestStop(bool discardPending = false) {
        { std::lock_guard lock(mutex_); stopping_ = true; if (discardPending) jobs_.clear(); }
        wake_.notify_one();
    }
    void stop() {
        requestStop();
        if (thread_.joinable()) thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_;
    void run() {
        for (;;) {
            std::function<void()> job;
            { std::unique_lock lock(mutex_); wake_.wait(lock,[this] { return stopping_ || !jobs_.empty(); });
              if (jobs_.empty() && stopping_) return;
              job = std::move(jobs_.front()); jobs_.pop_front(); }
            try { job(); } catch (...) { /* Each job reports its operation error. */ }
        }
    }
};
