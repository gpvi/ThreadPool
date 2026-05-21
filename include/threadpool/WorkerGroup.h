#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
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
    std::vector<std::size_t> retired_indices_;
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

    bool try_retire()
    {
        while (true) {
            auto current = live_workers_.load(std::memory_order_relaxed);
            if (current <= min_workers_) {
                return false;
            }
            if (live_workers_.compare_exchange_weak(
                    current, current - 1, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void decrease_live_worker()
    {
        live_workers_.fetch_sub(1, std::memory_order_relaxed);
    }

    void retire_current_worker()
    {
        const auto self_id = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].get_id() == self_id) {
                retired_indices_.push_back(i);
                break;
            }
        }
    }

    void reserve()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        workers_.reserve(max_workers_);
    }

    void reap_retired_workers()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (retired_indices_.empty()) {
            return;
        }

        std::sort(retired_indices_.begin(), retired_indices_.end(), std::greater<std::size_t>());

        for (auto idx : retired_indices_) {
            if (idx < workers_.size()) {
                if (workers_[idx].joinable()) {
                    workers_[idx].join();
                }
                workers_.erase(workers_.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }

        retired_indices_.clear();
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
            retired_indices_.clear();
        }

        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    template <typename Rep, typename Period>
    bool join_all_for(const std::chrono::duration<Rep, Period> &timeout)
    {
        std::vector<std::thread> workers;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workers.swap(workers_);
            retired_indices_.clear();
        }

        auto workers_ptr = std::make_shared<std::vector<std::thread>>(std::move(workers));
        std::atomic<bool> done{false};

        std::thread joiner([workers_ptr, &done] {
            for (auto &w : *workers_ptr) {
                if (w.joinable()) {
                    w.join();
                }
            }
            done.store(true, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                joiner.detach();
                for (auto &w : *workers_ptr) {
                    if (w.joinable()) {
                        w.detach();
                    }
                }
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        joiner.join();
        return true;
    }

    template <typename Clock, typename Duration>
    bool join_all_until(const std::chrono::time_point<Clock, Duration> &deadline)
    {
        return join_all_for(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()
            )
        );
    }
};

} // namespace threadpool
