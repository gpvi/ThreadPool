#pragma once

#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#include <coroutine>
#define THREADPOOL_HAS_COROUTINE 1
#endif

#include "SafeQueue.h"
#include "TaskScheduler.h"
#include "ThreadPoolOptions.h"
#include "ThreadPoolRuntime.h"
#include "ThreadPoolStopToken.h"
#include "ThreadPoolTypeTraits.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace threadpool {

class ThreadPool {
public:
    using ShutdownMode = threadpool::ShutdownMode;
    using ExecutionMode = threadpool::ExecutionMode;
    using Options = threadpool::ThreadPoolOptions;
    using StopToken = threadpool::StopToken;
    using StopSource = threadpool::StopSource;

private:
    ThreadPoolRuntime runtime_;

#ifdef THREADPOOL_HAS_COROUTINE
    void enqueue_coroutine_resume(std::coroutine_handle<> handle, bool prefer_current_worker);
#endif

public:
#ifdef THREADPOOL_HAS_COROUTINE
    class ScheduleAwaiter {
    private:
        ThreadPool *pool_;

    public:
        explicit ScheduleAwaiter(ThreadPool *pool)
            : pool_(pool)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle)
        {
            pool_->enqueue_coroutine_resume(handle, false);
        }

        void await_resume() const noexcept
        {
        }
    };

    class YieldAwaiter {
    private:
        ThreadPool *pool_;

    public:
        explicit YieldAwaiter(ThreadPool *pool)
            : pool_(pool)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle)
        {
            pool_->enqueue_coroutine_resume(handle, true);
        }

        void await_resume() const noexcept
        {
        }
    };
#endif

    explicit ThreadPool(std::size_t worker_count = 4)
        : ThreadPool(worker_count, ExecutionMode::ThreadOnly)
    {
    }

    ThreadPool(std::size_t worker_count, ExecutionMode execution_mode)
        : ThreadPool(make_fixed_options(worker_count, execution_mode))
    {
    }

    explicit ThreadPool(const Options &options)
        : runtime_(options)
    {
        start(runtime_.min_workers());
    }

    ~ThreadPool() noexcept
    {
        try {
            shutdown();
        } catch (...) {
        }
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    static Options make_fixed_options(std::size_t worker_count, ExecutionMode execution_mode)
    {
        Options options;
        options.min_workers = worker_count;
        options.max_workers = worker_count;
        options.execution_mode = execution_mode;
        return options;
    }

    void start(std::size_t worker_count)
    {
        runtime_.start(worker_count);
    }

    void shutdown(ShutdownMode mode = ShutdownMode::Drain)
    {
        runtime_.shutdown(mode);
    }

    void wait_idle()
    {
        runtime_.wait_idle();
    }

    template <typename Rep, typename Period>
    bool wait_idle_for(const std::chrono::duration<Rep, Period> &timeout)
    {
        return runtime_.wait_idle_for(timeout);
    }

    template <typename Clock, typename Duration>
    bool wait_idle_until(const std::chrono::time_point<Clock, Duration> &deadline)
    {
        return runtime_.wait_idle_until(deadline);
    }

    bool is_shutdown() const
    {
        return runtime_.is_shutdown();
    }

    std::size_t worker_count() const noexcept
    {
        return runtime_.worker_count();
    }

    std::size_t min_workers() const noexcept
    {
        return runtime_.min_workers();
    }

    std::size_t max_workers() const noexcept
    {
        return runtime_.max_workers();
    }

    std::size_t queued_tasks() const
    {
        return runtime_.queued_tasks();
    }

    std::size_t queued_coroutines() const
    {
        return runtime_.queued_coroutines();
    }

    std::size_t active_tasks() const
    {
        return runtime_.active_tasks();
    }

    ExecutionMode execution_mode() const noexcept
    {
        return runtime_.execution_mode();
    }

    bool is_coroutine_enabled() const noexcept
    {
        return runtime_.coroutine_enabled();
    }

    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args)
        -> std::future<detail::invoke_result_t<F, Args...>>;

    template <typename F, typename... Args>
    auto submit_with_stop(StopToken token, F &&f, Args &&...args)
        -> std::future<detail::invoke_result_t<F, StopToken, Args...>>;

#ifdef THREADPOOL_HAS_COROUTINE
    ScheduleAwaiter schedule()
    {
        if (!runtime_.coroutine_enabled()) {
            throw std::runtime_error("coroutine scheduling requires ThreadAndCoroutine mode");
        }

        return ScheduleAwaiter(this);
    }

    YieldAwaiter yield()
    {
        if (!runtime_.coroutine_enabled()) {
            throw std::runtime_error("coroutine yield requires ThreadAndCoroutine mode");
        }

        return YieldAwaiter(this);
    }
#endif

private:
#if !((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
    template <typename ReturnType, typename F, typename Tuple, std::size_t... I>
    static ReturnType call_impl(F &&f, Tuple &&tuple, std::index_sequence<I...>);
#endif
};

} // namespace threadpool

#include "ThreadPoolWorker.h"
#include "WorkerGroupImpl.h"
#include "ThreadPoolRuntimeImpl.h"
#include "ThreadPoolImpl.h"
#include "ThreadPoolSubmit.h"

using ThreadPool = threadpool::ThreadPool;
