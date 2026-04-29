#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace threadpool {

class ThreadPoolRuntime;
class ThreadPoolWorker;

class WorkerGroup {
private:
    std::size_t min_workers_;
    std::size_t max_workers_;
    std::chrono::milliseconds idle_timeout_;
    std::atomic<std::size_t> live_workers_{0};
    std::atomic<std::size_t> next_worker_id_{0};
    std::vector<std::thread> workers_;
    std::vector<std::thread::id> retired_worker_ids_;
    mutable std::mutex mutex_;

public:
    WorkerGroup(
        std::size_t min_workers,
        std::size_t max_workers,
        std::chrono::milliseconds idle_timeout
    )
        : min_workers_(min_workers),
          max_workers_(max_workers),
          idle_timeout_(idle_timeout)
    {
    }

    std::size_t min_workers() const noexcept
    {
        return min_workers_;
    }

    std::size_t max_workers() const noexcept
    {
        return max_workers_;
    }

    std::chrono::milliseconds idle_timeout() const noexcept
    {
        return idle_timeout_;
    }

    std::size_t live_workers() const noexcept
    {
        return live_workers_.load(std::memory_order_relaxed);
    }

    bool can_spawn() const noexcept
    {
        return live_workers() < max_workers_;
    }

    bool should_exit_on_idle() const noexcept
    {
        return live_workers() > min_workers_;
    }

    void decrease_live_worker()
    {
        live_workers_.fetch_sub(1, std::memory_order_relaxed);
    }

    void retire_current_worker()
    {
        decrease_live_worker();
        std::unique_lock<std::mutex> lock(mutex_);
        retired_worker_ids_.push_back(std::this_thread::get_id());
    }

    void reserve()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        workers_.reserve(max_workers_);
    }

    void reap_retired_workers()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (retired_worker_ids_.empty()) {
            return;
        }

        for (auto retired = retired_worker_ids_.begin(); retired != retired_worker_ids_.end();) {
            bool removed = false;

            for (auto worker = workers_.begin(); worker != workers_.end(); ++worker) {
                if (worker->get_id() == *retired) {
                    if (worker->joinable()) {
                        worker->join();
                    }
                    workers_.erase(worker);
                    removed = true;
                    break;
                }
            }

            if (removed) {
                retired = retired_worker_ids_.erase(retired);
            } else {
                ++retired;
            }
        }
    }

    void spawn(ThreadPoolRuntime *runtime);

    void maybe_grow(ThreadPoolRuntime *runtime, std::size_t queued_tasks)
    {
        if (can_spawn() && queued_tasks > live_workers()) {
            spawn(runtime);
        }
    }

    void join_all()
    {
        std::vector<std::thread> workers;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workers.swap(workers_);
            retired_worker_ids_.clear();
        }

        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

} // namespace threadpool
