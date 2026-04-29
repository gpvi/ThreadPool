#pragma once

#include <chrono>
#include <cstddef>

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
    std::chrono::milliseconds idle_timeout{30000};
};

using Options = ThreadPoolOptions;

} // namespace threadpool
