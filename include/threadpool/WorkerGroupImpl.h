#pragma once

#include <cstddef>
#include <mutex>

namespace threadpool {

inline void WorkerGroup::spawn(ThreadPoolRuntime *runtime)
{
    reap_retired_workers();

    if (!can_spawn()) {
        return;
    }

    const std::size_t worker_id =
        next_worker_id_.fetch_add(1, std::memory_order_relaxed) % max_workers_;

    live_workers_.fetch_add(1, std::memory_order_relaxed);
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        workers_.emplace_back(ThreadPoolWorker(runtime, worker_id));
    } catch (...) {
        live_workers_.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
}

} // namespace threadpool
