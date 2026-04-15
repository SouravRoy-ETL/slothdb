#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace slothdb {

// A simple thread pool for parallel query execution.
// Uses a task queue with worker threads that pull and execute tasks.
class ThreadPool {
public:
    // Create a thread pool. num_threads=0 means auto-detect (hardware concurrency).
    explicit ThreadPool(unsigned int num_threads = 0);
    ~ThreadPool();

    // No copy/move.
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Submit a task and get a future for the result.
    template <class F, class... Args>
    auto Submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    // Wait for all submitted tasks to complete.
    void WaitAll();

    // Number of worker threads.
    unsigned int NumThreads() const { return static_cast<unsigned int>(workers_.size()); }

    // Get the global thread pool singleton.
    static ThreadPool &GetGlobal();
    static void SetGlobalThreadCount(unsigned int count);

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<int> active_tasks_{0};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};

} // namespace slothdb
