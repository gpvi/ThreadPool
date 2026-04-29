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

struct BenchmarkConfig {
	std::size_t tasks = 100000;
	std::size_t submitters = 4;
	std::vector<std::size_t> workers;
};

std::size_t hardware_workers()
{
	const auto count = std::thread::hardware_concurrency();
	return count == 0 ? 4 : count;
}

std::size_t parse_arg(const char *value, const std::size_t fallback)
{
	const auto parsed = std::strtoull(value, nullptr, 10);
	return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

BenchmarkConfig parse_config(int argc, char **argv)
{
	BenchmarkConfig config;
	const auto hw = hardware_workers();
	config.workers = {1, std::max<std::size_t>(2, hw / 2), hw};

	if (argc > 1) {
		config.tasks = parse_arg(argv[1], config.tasks);
	}
	if (argc > 2) {
		config.submitters = parse_arg(argv[2], config.submitters);
	}
	config.submitters = std::max<std::size_t>(1, config.submitters);
	return config;
}

template <typename F>
long long measure_ms(F &&f)
{
	const auto started_at = Clock::now();
	f();
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		Clock::now() - started_at
	).count();
}

double throughput_per_second(const std::size_t tasks, const long long elapsed_ms)
{
	if (elapsed_ms == 0) {
		return 0.0;
	}
	return static_cast<double>(tasks) * 1000.0 / static_cast<double>(elapsed_ms);
}

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
		pool.wait_idle();
	});

	if (counter.load() != tasks) {
		throw std::runtime_error("fire-and-wait counter mismatch");
	}
	print_row("fire_and_wait", workers, tasks, elapsed_ms);
}

void benchmark_future_values(const std::size_t workers, const std::size_t tasks)
{
	threadpool::ThreadPool pool(workers);
	std::vector<std::future<std::size_t>> futures;
	futures.reserve(tasks);

	const auto elapsed_ms = measure_ms([&] {
		for (std::size_t i = 0; i < tasks; ++i) {
			futures.push_back(pool.submit([i] {
				return i;
			}));
		}

		std::size_t checksum = 0;
		for (auto &future : futures) {
			checksum += future.get();
		}

		const auto expected = (tasks - 1) * tasks / 2;
		if (checksum != expected) {
			throw std::runtime_error("future checksum mismatch");
		}
	});

	print_row("future_values", workers, tasks, elapsed_ms);
}

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
		for (std::size_t s = 0; s < submitters; ++s) {
			submitter_threads.emplace_back([&] {
				for (std::size_t i = 0; i < per_submitter; ++i) {
					pool.submit([&counter] {
						counter.fetch_add(1, std::memory_order_relaxed);
					});
				}
			});
		}

		for (auto &thread : submitter_threads) {
			thread.join();
		}
		pool.wait_idle();
	});

	if (counter.load() != submitted_tasks) {
		throw std::runtime_error("parallel submitter counter mismatch");
	}
	print_row("parallel_submit", workers, submitted_tasks, elapsed_ms);
}

void benchmark_latency_sample(const std::size_t workers)
{
	const std::size_t samples = 2000;
	threadpool::ThreadPool pool(workers);
	std::vector<std::future<long long>> futures;
	futures.reserve(samples);

	for (std::size_t i = 0; i < samples; ++i) {
		const auto submitted_at = Clock::now();
		futures.push_back(pool.submit([submitted_at] {
			return std::chrono::duration_cast<Microseconds>(
				Clock::now() - submitted_at
			).count();
		}));
	}

	std::vector<long long> latencies;
	latencies.reserve(samples);
	for (auto &future : futures) {
		latencies.push_back(future.get());
	}

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

int main(int argc, char **argv)
{
	const auto config = parse_config(argc, argv);

	std::cout << "benchmark config: tasks=" << config.tasks
		<< ", submitters=" << config.submitters << '\n';
	std::cout << std::left << std::setw(22) << "case"
		<< std::right << std::setw(8) << "workers"
		<< std::setw(12) << "tasks"
		<< std::setw(12) << "ms"
		<< std::setw(16) << "tasks/sec"
		<< '\n';

	for (const auto workers : config.workers) {
		benchmark_fire_and_wait(workers, config.tasks);
		benchmark_future_values(workers, config.tasks);
		benchmark_parallel_submitters(workers, config.tasks, config.submitters);
		benchmark_latency_sample(workers);
	}

	return 0;
}
