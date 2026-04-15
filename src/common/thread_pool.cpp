#include "slothdb/common/thread_pool.hpp"

namespace slothdb {

ThreadPool::ThreadPool(unsigned int num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4; // fallback
    }

    for (unsigned int i = 0; i < num_threads; i++) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    stop_.store(true);
    cv_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::WorkerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_.load() || !tasks_.empty(); });
            if (stop_.load() && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        active_tasks_.fetch_add(1);
        task();
        active_tasks_.fetch_sub(1);
        wait_cv_.notify_all();
    }
}

void ThreadPool::WaitAll() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait(lock, [this]() {
        std::lock_guard<std::mutex> task_lock(mutex_);
        return tasks_.empty() && active_tasks_.load() == 0;
    });
}

static std::unique_ptr<ThreadPool> global_pool;
static unsigned int global_thread_count = 0;

ThreadPool &ThreadPool::GetGlobal() {
    if (!global_pool) {
        global_pool = std::make_unique<ThreadPool>(global_thread_count);
    }
    return *global_pool;
}

void ThreadPool::SetGlobalThreadCount(unsigned int count) {
    global_thread_count = count;
    global_pool.reset(); // Force re-creation on next access.
}

} // namespace slothdb
