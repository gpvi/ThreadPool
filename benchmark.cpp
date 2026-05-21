#include "ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;

// 基准测试配置参数
struct BenchmarkConfig {
    std::size_t tasks = 100000;      // 总任务数
    std::size_t submitters = 4;      // 并发提交线程数
    std::vector<std::size_t> workers; // 待测试的 worker 数量序列
};

// 获取硬件线程数，若获取失败则回退为 4
std::size_t hardware_workers()
{
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 4 : count;
}

// 解析命令行参数，解析失败时使用默认值
std::size_t parse_arg(const char *value, const std::size_t fallback)
{
    const auto parsed = std::strtoull(value, nullptr, 10);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

// 解析命令行参数并构建配置
// 用法: benchmark.exe <tasks> <submitters>
BenchmarkConfig parse_config(int argc, char **argv)
{
    BenchmarkConfig config;
    const auto hw = hardware_workers();

    // 默认测试三种 worker 数量：1 线程、半数硬件线程、全部硬件线程
    config.workers = {1, std::max<std::size_t>(2, hw / 2), hw};

    if (argc > 1) {
        config.tasks = parse_arg(argv[1], config.tasks);
    }
    if (argc > 2) {
        config.submitters = parse_arg(argv[2], config.submitters);
    }
    // 提交线程数至少为 1
    config.submitters = std::max<std::size_t>(1, config.submitters);
    return config;
}

// 测量函数执行耗时（毫秒）
template <typename F>
long long measure_ms(F &&f)
{
    const auto started_at = Clock::now();
    f();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started_at
    ).count();
}

// 计算每秒吞吐量
double throughput_per_second(const std::size_t tasks, const long long elapsed_ms)
{
    if (elapsed_ms == 0) {
        return 0.0;
    }
    return static_cast<double>(tasks) * 1000.0 / static_cast<double>(elapsed_ms);
}

// 打印一行 benchmark 结果
void print_row(
    const char *name,
    const std::size_t workers,
    const std::size_t tasks,
    const long long elapsed_ms
)
{
    std::cout << std::left << std::setw(22) << name
        << std::right << std::setw(8) << workers
        << std::setw(12) << tasks
        << std::setw(12) << elapsed_ms
        << std::setw(16) << static_cast<std::size_t>(throughput_per_second(tasks, elapsed_ms))
        << '\n';
}

// 场景一：提交即忘（fire-and-wait）
// 提交大量空任务 + wait_idle，测量纯调度吞吐
void benchmark_fire_and_wait(const std::size_t workers, const std::size_t tasks)
{
    threadpool::ThreadPool pool(workers);
    std::atomic<std::size_t> counter{0};

    const auto elapsed_ms = measure_ms([&] {
        for (std::size_t i = 0; i < tasks; ++i) {
            pool.submit([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // 等待所有任务执行完成
        pool.wait_idle();
    });

    // 验证所有任务都已执行
    if (counter.load() != tasks) {
        throw std::runtime_error("fire-and-wait counter mismatch");
    }
    print_row("fire_and_wait", workers, tasks, elapsed_ms);
}

// 场景二：带返回值的 future 任务
// 测量 future 创建 + get 同步的吞吐
void benchmark_future_values(const std::size_t workers, const std::size_t tasks)
{
    threadpool::ThreadPool pool(workers);
    std::vector<std::future<std::size_t>> futures;
    futures.reserve(tasks);

    const auto elapsed_ms = measure_ms([&] {
        // 提交阶段：提交 tasks 个返回计算值的任务
        for (std::size_t i = 0; i < tasks; ++i) {
            futures.push_back(pool.submit([i] {
                return i;
            }));
        }

        // 收集阶段：汇总所有返回值并校验
        std::size_t checksum = 0;
        for (auto &future : futures) {
            checksum += future.get();
        }

        // 校验 checksum = 0 + 1 + ... + (tasks-1)
        const auto expected = (tasks - 1) * tasks / 2;
        if (checksum != expected) {
            throw std::runtime_error("future checksum mismatch");
        }
    });

    print_row("future_values", workers, tasks, elapsed_ms);
}

// 场景三：多线程并发提交
// 模拟多个线程同时 submit 的竞争压力
void benchmark_parallel_submitters(
    const std::size_t workers,
    const std::size_t tasks,
    const std::size_t submitters
)
{
    threadpool::ThreadPool pool(workers);
    std::atomic<std::size_t> counter{0};
    std::vector<std::thread> submitter_threads;
    const std::size_t per_submitter = tasks / submitters;
    const std::size_t submitted_tasks = per_submitter * submitters;

    const auto elapsed_ms = measure_ms([&] {
        // 启动多个提交线程
        for (std::size_t s = 0; s < submitters; ++s) {
            submitter_threads.emplace_back([&] {
                for (std::size_t i = 0; i < per_submitter; ++i) {
                    pool.submit([&counter] {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    });
                }
            });
        }

        // 等待所有提交线程结束
        for (auto &thread : submitter_threads) {
            thread.join();
        }
        // 等待所有任务执行完毕
        pool.wait_idle();
    });

    if (counter.load() != submitted_tasks) {
        throw std::runtime_error("parallel submitter counter mismatch");
    }
    print_row("parallel_submit", workers, submitted_tasks, elapsed_ms);
}

// 场景四：延迟采样
// 测量任务从提交到开始执行的排队延迟（p50 / p95 / p99）
void benchmark_latency_sample(const std::size_t workers)
{
    const std::size_t samples = 2000;
    threadpool::ThreadPool pool(workers);
    std::vector<std::future<Microseconds::rep>> futures;
    futures.reserve(samples);

    // 提交时记录时间戳，任务内计算差值
    for (std::size_t i = 0; i < samples; ++i) {
        const auto submitted_at = Clock::now();
        futures.push_back(pool.submit([submitted_at] {
            return std::chrono::duration_cast<Microseconds>(
                Clock::now() - submitted_at
            ).count();
        }));
    }

    // 收集所有延迟数据
    std::vector<Microseconds::rep> latencies;
    latencies.reserve(samples);
    for (auto &future : futures) {
        latencies.push_back(future.get());
    }

    // 排序后取分位数
    std::sort(latencies.begin(), latencies.end());
    const auto p50 = latencies[latencies.size() / 2];
    const auto p95 = latencies[latencies.size() * 95 / 100];
    const auto p99 = latencies[latencies.size() * 99 / 100];

    std::cout << "latency_sample"
        << " workers=" << workers
        << " samples=" << samples
        << " p50_us=" << p50
        << " p95_us=" << p95
        << " p99_us=" << p99
        << '\n';
}

} // namespace

// 用法: threadpool_benchmark.exe <tasks> <submitters>
// 示例: threadpool_benchmark.exe 100000 4
int main(int argc, char **argv)
{
    const auto config = parse_config(argc, argv);

    std::cout << "benchmark config: tasks=" << config.tasks
        << ", submitters=" << config.submitters << '\n';

    // 输出表头
    std::cout << std::left << std::setw(22) << "case"
        << std::right << std::setw(8) << "workers"
        << std::setw(12) << "tasks"
        << std::setw(12) << "ms"
        << std::setw(16) << "tasks/sec"
        << '\n';

    // 对每种 worker 数量运行全部四组 benchmark
    for (const auto workers : config.workers) {
        benchmark_fire_and_wait(workers, config.tasks);
        benchmark_future_values(workers, config.tasks);
        benchmark_parallel_submitters(workers, config.tasks, config.submitters);
        benchmark_latency_sample(workers);
    }

    return 0;
}
