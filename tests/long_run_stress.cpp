#include "threadpool/ThreadPool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Config {
    std::size_t duration_sec = 60;
    std::size_t workers = 4;
};

std::size_t parse_arg(const char *value, std::size_t fallback) {
    const auto parsed = std::strtoull(value, nullptr, 10);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

Config parse_config(int argc, char **argv) {
    Config config;
    if (argc > 1) config.duration_sec = parse_arg(argv[1], config.duration_sec);
    if (argc > 2) config.workers = parse_arg(argv[2], config.workers);
    return config;
}

void run_continuous_submit(const Config &config) {
    threadpool::ThreadPool pool(config.workers);
    std::atomic<std::size_t> counter{0};
    std::atomic<bool> stop{false};
    const auto deadline = Clock::now() + std::chrono::seconds(config.duration_sec);

    std::vector<std::thread> submitters;
    for (std::size_t i = 0; i < config.workers; ++i) {
        submitters.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                pool.submit([&] {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop.store(true, std::memory_order_relaxed);

    for (auto &t : submitters) t.join();
    pool.shutdown();

    std::cout << "continuous submit: " << counter.load() << " tasks in "
              << config.duration_sec << "s\n";
}

void run_create_destroy_cycle(const Config &config) {
    const auto deadline = Clock::now() + std::chrono::seconds(config.duration_sec);
    std::size_t cycles = 0;

    while (Clock::now() < deadline) {
        threadpool::ThreadPool pool(config.workers);
        std::vector<std::future<int>> futures;
        for (std::size_t i = 0; i < 100; ++i) {
            futures.push_back(pool.submit([i] { return static_cast<int>(i * i); }));
        }
        for (auto &f : futures) f.get();
        pool.shutdown();
        ++cycles;
    }

    std::cout << "create/destroy cycles: " << cycles << " in "
              << config.duration_sec << "s\n";
}

#ifdef THREADPOOL_HAS_COROUTINE
class CoroutineTask {
    std::future<void> m_done;
public:
    struct promise_type {
        std::promise<void> done;
        CoroutineTask get_return_object() { return CoroutineTask(done.get_future()); }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() { done.set_value(); }
        void unhandled_exception() { done.set_exception(std::current_exception()); }
    };
    explicit CoroutineTask(std::future<void> done) : m_done(std::move(done)) {}
    void get() { m_done.get(); }
};

CoroutineTask coro_work(threadpool::ThreadPool &pool, std::atomic<std::size_t> &counter,
                         std::size_t yields) {
    co_await pool.schedule();
    for (std::size_t i = 0; i < yields; ++i) {
        counter.fetch_add(1, std::memory_order_relaxed);
        co_await pool.yield();
    }
    counter.fetch_add(1, std::memory_order_relaxed);
}

void run_coroutine_stress(const Config &config) {
    threadpool::ThreadPool::Options options;
    options.min_workers = config.workers;
    options.max_workers = config.workers;
    options.execution_mode = threadpool::ThreadPool::ExecutionMode::ThreadAndCoroutine;
    threadpool::ThreadPool pool(options);

    std::atomic<std::size_t> counter{0};
    std::atomic<bool> stop{false};
    const auto deadline = Clock::now() + std::chrono::seconds(config.duration_sec);

    std::thread submitter([&] {
        std::mt19937 rng(42);
        std::uniform_int_distribution<std::size_t> yields(0, 10);
        while (!stop.load(std::memory_order_relaxed)) {
            auto task = coro_work(pool, counter, yields(rng));
            task.get();
        }
    });

    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop.store(true, std::memory_order_relaxed);
    submitter.join();
    pool.shutdown();

    std::cout << "coroutine stress: " << counter.load() << " steps in "
              << config.duration_sec << "s\n";
}
#endif

void run_shutdown_timeout_test() {
    threadpool::ThreadPool pool(1);

    std::promise<void> blocker;
    auto block_future = blocker.get_future();
    pool.submit([&] { block_future.get(); });

    std::atomic<bool> done{false};
    pool.submit([&done] { done.store(true); });

    auto ok = pool.shutdown_for(std::chrono::milliseconds(50));

    assert(!ok);
    blocker.set_value();
    assert(done.load());

    std::cout << "shutdown timeout: works\n";
}

int main(int argc, char **argv) {
    const auto config = parse_config(argc, argv);
    std::cout << "long-run stress: duration=" << config.duration_sec
              << "s workers=" << config.workers << "\n\n";

    run_continuous_submit(config);
    run_create_destroy_cycle(config);
#ifdef THREADPOOL_HAS_COROUTINE
    run_coroutine_stress(config);
#endif
    run_shutdown_timeout_test();

    std::cout << "\nlong-run stress passed\n";
    return 0;
}
