#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>

namespace threadpool {

enum class ShutdownMode {
    Drain,
    CancelPending
};

enum class ExecutionMode {
    ThreadOnly,
    ThreadAndCoroutine
};

struct ThreadPoolOptions {
    std::size_t min_workers = 4;
    std::size_t max_workers = 4;
    ExecutionMode execution_mode = ExecutionMode::ThreadOnly;
    std::size_t coroutine_burst_limit = 8;
    std::size_t max_coroutine_queue_size = 0;
    std::function<void(const std::exception_ptr &)> on_exception;
    std::chrono::milliseconds idle_timeout{5000};
};

using Options = ThreadPoolOptions;

} // namespace threadpool
