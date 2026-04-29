#pragma once

namespace threadpool {

inline ThreadPoolRuntime::ThreadPoolRuntime(const ThreadPoolOptions &options)
    : execution_mode_(options.execution_mode),
      scheduler_(options.execution_mode, options.max_workers, options.coroutine_burst_limit),
      workers_(options.min_workers, options.max_workers, options.idle_timeout)
{
    if (workers_.min_workers() == 0) {
        throw std::invalid_argument("ThreadPool requires at least one min worker");
    }

    if (workers_.max_workers() < workers_.min_workers()) {
        throw std::invalid_argument("max_workers must be >= min_workers");
    }
}

inline ThreadPoolRuntime *& ThreadPoolRuntime::current_runtime()
{
    static thread_local ThreadPoolRuntime *runtime = nullptr;
    return runtime;
}

inline std::size_t & ThreadPoolRuntime::current_worker_id()
{
    static thread_local std::size_t id = 0;
    return id;
}

inline void ThreadPoolRuntime::start(std::size_t worker_count)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (started_) {
        return;
    }

    if (worker_count == 0 || worker_count > workers_.max_workers()) {
        throw std::invalid_argument("invalid worker_count");
    }

    started_ = true;
    workers_.reserve();

    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            spawn_worker_unlocked();
        }
    } catch (...) {
        shutdown_ = true;
        work_cv_.notify_all();
        throw;
    }
}

inline void ThreadPoolRuntime::shutdown(ShutdownMode mode)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            return;
        }

        shutdown_ = true;

        if (mode == ShutdownMode::CancelPending) {
            scheduler_.cancel_pending_tasks();
        }
    }

    scheduler_.notify_idle_if_needed();
    work_cv_.notify_all();

    workers_.join_all();
}

inline void ThreadPoolRuntime::submit_task(Task task)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }

        scheduler_.push_task(std::move(task));
        maybe_grow_unlocked();
    }

    work_cv_.notify_one();
}

inline ThreadPoolRuntime::WorkerWork ThreadPoolRuntime::wait_for_work(
    std::size_t worker_id,
    std::size_t coroutine_burst
)
{
    WorkerWork work;
    std::unique_lock<std::mutex> lock(mutex_);

    const bool awakened = work_cv_.wait_for(
        lock,
        workers_.idle_timeout(),
        [this, worker_id] {
            return shutdown_ || scheduler_.has_any_work_for_worker(worker_id);
        }
    );

    if (!awakened) {
        if (workers_.should_exit_on_idle()) {
            retire_current_worker();
            scheduler_.notify_idle_if_needed();
            work.exit = true;
        }

        return work;
    }

    if (shutdown_ && !scheduler_.has_any_work_for_worker(worker_id)) {
        retire_current_worker();
        scheduler_.notify_idle_if_needed();
        work.exit = true;
        return work;
    }

    work.has_task = scheduler_.pop_for_worker(
        worker_id,
        coroutine_burst,
        work.popped_coroutine,
        work.task
    );

    if (work.has_task) {
        scheduler_.mark_started();
    }

    return work;
}

inline void ThreadPoolRuntime::finish_work(
    bool popped_coroutine,
    std::size_t &coroutine_burst
)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (popped_coroutine) {
        ++coroutine_burst;
    } else {
        coroutine_burst = 0;
    }

    scheduler_.mark_finished();
}

inline void ThreadPoolRuntime::retire_current_worker()
{
    workers_.retire_current_worker();
}

inline void ThreadPoolRuntime::set_current_worker(std::size_t worker_id)
{
    current_runtime() = this;
    current_worker_id() = worker_id;
}

inline void ThreadPoolRuntime::clear_current_worker()
{
    current_runtime() = nullptr;
}

inline void ThreadPoolRuntime::wait_idle()
{
    scheduler_.wait_idle();
}

inline bool ThreadPoolRuntime::is_shutdown() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return shutdown_;
}

inline std::size_t ThreadPoolRuntime::worker_count() const noexcept
{
    return workers_.live_workers();
}

inline std::size_t ThreadPoolRuntime::min_workers() const noexcept
{
    return workers_.min_workers();
}

inline std::size_t ThreadPoolRuntime::max_workers() const noexcept
{
    return workers_.max_workers();
}

inline std::size_t ThreadPoolRuntime::queued_tasks() const
{
    return scheduler_.queued_tasks();
}

inline std::size_t ThreadPoolRuntime::queued_coroutines() const
{
    return scheduler_.queued_coroutines();
}

inline std::size_t ThreadPoolRuntime::active_tasks() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return scheduler_.active_tasks();
}

inline ExecutionMode ThreadPoolRuntime::execution_mode() const noexcept
{
    return execution_mode_;
}

inline bool ThreadPoolRuntime::coroutine_enabled() const noexcept
{
    return execution_mode_ == ExecutionMode::ThreadAndCoroutine;
}

inline void ThreadPoolRuntime::spawn_worker_unlocked()
{
    workers_.spawn(this);
}

inline void ThreadPoolRuntime::maybe_grow_unlocked()
{
    workers_.maybe_grow(this, scheduler_.queued_tasks());
}

#ifdef THREADPOOL_HAS_COROUTINE
inline void ThreadPoolRuntime::enqueue_coroutine_resume(
    std::coroutine_handle<> handle,
    bool prefer_current_worker
)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            throw std::runtime_error("schedule coroutine on stopped ThreadPool");
        }

        if (!coroutine_enabled()) {
            throw std::runtime_error("coroutine scheduling requires ThreadAndCoroutine mode");
        }

        std::size_t target = 0;

        if (
            prefer_current_worker
            && current_runtime() == this
            && current_worker_id() < workers_.max_workers()
        ) {
            target = current_worker_id();
        }

        scheduler_.push_coroutine(
            handle,
            prefer_current_worker && current_runtime() == this,
            target
        );

        maybe_grow_unlocked();
    }

    work_cv_.notify_all();
}
#endif

} // namespace threadpool
