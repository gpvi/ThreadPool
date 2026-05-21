#pragma once

#include "SafeQueue.h"
#include "ThreadPoolOptions.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#include <coroutine>
#endif

namespace threadpool {

class TaskScheduler {
public:
    using Task = std::function<void()>;

private:
    ExecutionMode execution_mode_;
    std::size_t coroutine_burst_limit_;
    std::size_t max_coroutine_queue_size_;
    std::size_t active_tasks_ = 0;
    std::size_t next_coroutine_worker_ = 0;
    SafeQueue<Task> task_queue_;
    std::vector<std::unique_ptr<SafeQueue<Task>>> coroutine_queues_;
    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;

public:
    TaskScheduler(
        ExecutionMode execution_mode,
        std::size_t worker_capacity,
        std::size_t coroutine_burst_limit,
        std::size_t max_coroutine_queue_size = 0
    )
        : execution_mode_(execution_mode),
          coroutine_burst_limit_(coroutine_burst_limit == 0 ? 1 : coroutine_burst_limit),
          max_coroutine_queue_size_(max_coroutine_queue_size)
    {
        coroutine_queues_.reserve(worker_capacity);
        for (std::size_t i = 0; i < worker_capacity; ++i) {
            coroutine_queues_.push_back(
                std::make_unique<SafeQueue<Task>>()
            );
        }
    }

    std::size_t queued_tasks() const
    {
        return task_queue_.size();
    }

    std::size_t queued_coroutines() const
    {
        std::size_t total = 0;
        for (const auto &queue : coroutine_queues_) {
            total += queue->size();
        }
        return total;
    }

    std::size_t active_tasks() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return active_tasks_;
    }

    std::size_t max_coroutine_queue_size() const noexcept
    {
        return max_coroutine_queue_size_;
    }

    bool coroutine_enabled() const noexcept
    {
        return execution_mode_ == ExecutionMode::ThreadAndCoroutine;
    }

    void push_task(Task task)
    {
        task_queue_.push(std::move(task));
    }

    void cancel_pending_tasks()
    {
        task_queue_.clear();
    }

    bool worker_has_coroutine(std::size_t worker_id) const
    {
        return coroutine_enabled()
            && worker_id < coroutine_queues_.size()
            && !coroutine_queues_[worker_id]->empty();
    }

    bool coroutine_queues_empty() const
    {
        if (!coroutine_enabled()) {
            return true;
        }

        for (const auto &queue : coroutine_queues_) {
            if (!queue->empty()) {
                return false;
            }
        }

        return true;
    }

    bool has_any_work_for_worker(std::size_t worker_id) const
    {
        if (!task_queue_.empty() || worker_has_coroutine(worker_id)) {
            return true;
        }

        if (!coroutine_enabled()) {
            return false;
        }

        for (std::size_t offset = 1; offset < coroutine_queues_.size(); ++offset) {
            const std::size_t victim = (worker_id + offset) % coroutine_queues_.size();
            if (!coroutine_queues_[victim]->empty()) {
                return true;
            }
        }

        return false;
    }

    bool steal_coroutine(std::size_t worker_id, Task &task)
    {
        if (!coroutine_enabled() || coroutine_queues_.empty()) {
            return false;
        }

        for (std::size_t offset = 1; offset < coroutine_queues_.size(); ++offset) {
            const std::size_t victim = (worker_id + offset) % coroutine_queues_.size();

            if (coroutine_queues_[victim]->pop(task)) {
                return true;
            }
        }

        return false;
    }

    bool pop_for_worker(
        std::size_t worker_id,
        std::size_t coroutine_burst,
        bool &popped_coroutine,
        Task &task
    )
    {
        popped_coroutine = false;

        if (coroutine_burst >= coroutine_burst_limit_ && task_queue_.pop(task)) {
            return true;
        }

        if (worker_has_coroutine(worker_id) && coroutine_queues_[worker_id]->pop(task)) {
            popped_coroutine = true;
            return true;
        }

        if (task_queue_.pop(task)) {
            return true;
        }

        popped_coroutine = steal_coroutine(worker_id, task);
        return popped_coroutine;
    }

    void mark_started()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++active_tasks_;
    }

    void mark_finished()
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            --active_tasks_;
        }

        notify_idle_if_needed();
    }

    void notify_idle_if_needed()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (task_queue_.empty() && coroutine_queues_empty() && active_tasks_ == 0) {
            idle_cv_.notify_all();
        }
    }

    void wait_idle()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        idle_cv_.wait(lock, [this] {
            return task_queue_.empty()
                && coroutine_queues_empty()
                && active_tasks_ == 0;
        });
    }

    template <typename Rep, typename Period>
    bool wait_idle_for(
        const std::chrono::duration<Rep, Period> &timeout
    )
    {
        std::unique_lock<std::mutex> lock(mutex_);

        return idle_cv_.wait_for(lock, timeout, [this] {
            return task_queue_.empty()
                && coroutine_queues_empty()
                && active_tasks_ == 0;
        });
    }

    template <typename Clock, typename Duration>
    bool wait_idle_until(
        const std::chrono::time_point<Clock, Duration> &deadline
    )
    {
        std::unique_lock<std::mutex> lock(mutex_);

        return idle_cv_.wait_until(lock, deadline, [this] {
            return task_queue_.empty()
                && coroutine_queues_empty()
                && active_tasks_ == 0;
        });
    }

#ifdef THREADPOOL_HAS_COROUTINE
    void push_coroutine(
        std::coroutine_handle<> handle,
        bool prefer_worker,
        std::size_t preferred_worker
    )
    {
        if (!coroutine_enabled()) {
            throw std::runtime_error("coroutine scheduling requires ThreadAndCoroutine mode");
        }

        std::size_t target = 0;
        if (prefer_worker && preferred_worker < coroutine_queues_.size()) {
            target = preferred_worker;
        } else {
            std::unique_lock<std::mutex> lock(mutex_);
            target = next_coroutine_worker_;
            next_coroutine_worker_ = (next_coroutine_worker_ + 1) % coroutine_queues_.size();
        }

        if (max_coroutine_queue_size_ > 0
            && coroutine_queues_[target]->size() >= max_coroutine_queue_size_) {
            throw std::runtime_error("coroutine queue full for worker");
        }

        coroutine_queues_[target]->push([handle] {
            handle.resume();
        });
    }
#endif
};

} // namespace threadpool
