#pragma once

#include "TaskScheduler.h"
#include "ThreadPoolOptions.h"
#include "WorkerGroup.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace threadpool {

class ThreadPoolRuntime {
public:
    using Task = TaskScheduler::Task;

    struct WorkerWork {
        bool exit = false;
        bool has_task = false;
        bool popped_coroutine = false;
        Task task;
    };

private:
    bool shutdown_ = false;
    bool started_ = false;

    ExecutionMode execution_mode_ = ExecutionMode::ThreadOnly;
    std::function<void(const std::exception_ptr &)> on_exception_;

    TaskScheduler scheduler_;
    WorkerGroup workers_;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;

public:
    explicit ThreadPoolRuntime(const ThreadPoolOptions &options);
    ~ThreadPoolRuntime() = default;

    ThreadPoolRuntime(const ThreadPoolRuntime &) = delete;
    ThreadPoolRuntime(ThreadPoolRuntime &&) = delete;
    ThreadPoolRuntime &operator=(const ThreadPoolRuntime &) = delete;
    ThreadPoolRuntime &operator=(ThreadPoolRuntime &&) = delete;

    static ThreadPoolRuntime *&current_runtime();
    static std::size_t &current_worker_id();

    void start(std::size_t worker_count);
    void shutdown(ShutdownMode mode = ShutdownMode::Drain);

    template <typename Rep, typename Period>
    bool shutdown_for(ShutdownMode mode, const std::chrono::duration<Rep, Period> &timeout);

    template <typename Clock, typename Duration>
    bool shutdown_until(ShutdownMode mode, const std::chrono::time_point<Clock, Duration> &deadline);

    void submit_task(Task task);

    WorkerWork wait_for_work(std::size_t worker_id, std::size_t coroutine_burst);
    void finish_work(bool popped_coroutine, std::size_t &coroutine_burst);

    void retire_current_worker();
    void set_current_worker(std::size_t worker_id);
    void clear_current_worker();

    void wait_idle();

    template <typename Rep, typename Period>
    bool wait_idle_for(const std::chrono::duration<Rep, Period> &timeout)
    {
        return scheduler_.wait_idle_for(timeout);
    }

    template <typename Clock, typename Duration>
    bool wait_idle_until(const std::chrono::time_point<Clock, Duration> &deadline)
    {
        return scheduler_.wait_idle_until(deadline);
    }

    bool is_shutdown() const;
    std::size_t worker_count() const noexcept;
    std::size_t min_workers() const noexcept;
    std::size_t max_workers() const noexcept;
    std::size_t queued_tasks() const;
    std::size_t queued_coroutines() const;
    std::size_t active_tasks() const;
    std::size_t max_coroutine_queue_size() const noexcept;
    ExecutionMode execution_mode() const noexcept;
    bool coroutine_enabled() const noexcept;

    const std::function<void(const std::exception_ptr &)> &on_exception() const noexcept
    {
        return on_exception_;
    }

#ifdef THREADPOOL_HAS_COROUTINE
    void enqueue_coroutine_resume(std::coroutine_handle<> handle, bool prefer_current_worker);
#endif

private:
    void spawn_worker_unlocked();
    void maybe_grow_unlocked();
};

} // namespace threadpool
