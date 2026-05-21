# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
# Configure and build (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# Run all tests
ctest --test-dir build --build-config Release --output-on-failure

# Run individual test targets
./build/Release/threadpool_tests.exe            # C++14 behavioral tests
./build/Release/threadpool_coroutine_tests.exe   # C++20 coroutine tests
./build/Release/threadpool_stress.exe 50000 8 8  # stress test: <tasks> <workers> <submitters>
./build/Release/threadpool_benchmark.exe 100000 4 # benchmark: <tasks> <submitters>
./build/Release/threadpool_mode_compare.exe 20000 2000 10 4  # <tasks> <coroutines> <yields_per> <workers>

# Manual compilation (no CMake, g++)
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -pthread test_coroutine.cpp -o threadpool_coroutine_tests.exe
```

## Architecture

This is a **header-only** C++ thread pool library. Users include only `ThreadPool.h`. Four-layer architecture:

| Layer | File | Responsibility |
|---|---|---|
| Facade | `ThreadPool.h` | Public API: `submit()`, `shutdown()`, `wait_idle()`, coroutine awaiters |
| Runtime | `ThreadPoolRuntime.h` + `ThreadPoolRuntimeImpl.h` | Coordinates start/shutdown, worker wake/sleep, cross-layer dispatch |
| Scheduler | `TaskScheduler.h` | Global task FIFO, per-worker coroutine queues, work stealing, active/idle tracking |
| Execution | `WorkerGroup.h` + `ThreadPoolWorker.h` | Thread lifecycle, worker loop (pop coroutine first → global task → steal) |

**Lock strategy**: "who owns the data manages the lock." `SafeQueue` locks itself, `TaskScheduler` locks scheduling state, `WorkerGroup` locks thread container, `ThreadPoolRuntime` locks start/shutdown state only.

**Two execution modes**: `ThreadOnly` (default, C++14) and `ThreadAndCoroutine` (C++20 coroutine scheduling). Coroutine mode must be explicitly enabled at construction; calling `schedule()`/`yield()` in `ThreadOnly` mode throws.

**Key C++ version compat**: `ThreadPoolTypeTraits.h` uses `std::invoke_result_t` in C++17+ and `std::result_of` in C++14. Coroutine targets require C++20 (`cxx_std_20` in CMake). C++14 is the project baseline.

## Testing & CI

CTest runs three tests by default: `threadpool_tests` (behavior), `threadpool_coroutine_tests` (coroutines), `threadpool_stress 5000 4 4` (stress). Benchmark is not in CTest.

CI (`.github/workflows/ci.yml`): `ubuntu-latest` + `windows-latest`, runs cmake configure → build → ctest on every push/PR to main/master.

## Code Style Constraints

- C++14 baseline; C++20 features gated behind `#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)`
- No `bits/stdc++.h` — use standard headers only
- Use `thread_local` for per-thread mutable state, never share non-thread-safe objects across worker threads
- Worker count 0 is rejected with `std::invalid_argument`
- Submitting tasks after shutdown throws `std::runtime_error`
- Destructor calls `shutdown()` in a `try/catch` — must remain `noexcept`
