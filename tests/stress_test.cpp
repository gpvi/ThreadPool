#include "threadpool/ThreadPool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct StressConfig {
	std::size_t workers = std::thread::hardware_concurrency() == 0 ? 4 : std::thread::hardware_concurrency();
	std::size_t tasks = 20000;
	std::size_t submitters = 4;
};

std::size_t parse_arg(const char *value, const std::size_t fallback)
{
	const auto parsed = std::strtoull(value, nullptr, 10);
	return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

StressConfig parse_config(int argc, char **argv)
{
	StressConfig config;
	if (argc > 1) {
		config.tasks = parse_arg(argv[1], config.tasks);
	}
	if (argc > 2) {
		config.workers = parse_arg(argv[2], config.workers);
	}
	if (argc > 3) {
		config.submitters = parse_arg(argv[3], config.submitters);
	}
	return config;
}

void run_many_future_tasks(const StressConfig &config)
{
	threadpool::ThreadPool pool(config.workers);
	std::vector<std::future<std::size_t>> futures;
	futures.reserve(config.tasks);

	const auto started_at = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < config.tasks; ++i) {
		futures.push_back(pool.submit([i] {
			return i * i;
		}));
	}

	std::size_t checksum = 0;
	for (auto &future : futures) {
		checksum += future.get();
	}
	pool.wait_idle();

	const auto expected = (config.tasks - 1) * config.tasks * (2 * config.tasks - 1) / 6;
	assert(checksum == expected);
	assert(pool.queued_tasks() == 0);
	assert(pool.active_tasks() == 0);

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started_at
	).count();
	std::cout << "future tasks: " << config.tasks << " completed in " << elapsed << " ms\n";
}

void run_parallel_submitters(const StressConfig &config)
{
	threadpool::ThreadPool pool(config.workers);
	std::atomic<std::size_t> counter{0};
	std::vector<std::thread> submitters;
	const std::size_t per_submitter = config.tasks / config.submitters;

	for (std::size_t s = 0; s < config.submitters; ++s) {
		submitters.emplace_back([&pool, &counter, per_submitter] {
			for (std::size_t i = 0; i < per_submitter; ++i) {
				pool.submit([&counter] {
					counter.fetch_add(1, std::memory_order_relaxed);
				});
			}
		});
	}

	for (auto &submitter : submitters) {
		submitter.join();
	}
	pool.wait_idle();

	assert(counter.load() == per_submitter * config.submitters);
	std::cout << "parallel submitters: " << counter.load() << " tasks executed\n";
}

void run_cancel_pending_stress(const StressConfig &config)
{
	threadpool::ThreadPool pool(1);
	std::atomic<bool> first_task_started{false};
	std::atomic<std::size_t> executed{0};

	auto blocker = pool.submit([&first_task_started, &executed] {
		first_task_started.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		executed.fetch_add(1);
	});

	while (!first_task_started.load()) {
		std::this_thread::yield();
	}

	std::vector<std::future<void>> futures;
	futures.reserve(config.tasks);
	for (std::size_t i = 0; i < config.tasks; ++i) {
		futures.push_back(pool.submit([&executed] {
			executed.fetch_add(1);
		}));
	}

	pool.shutdown(threadpool::ThreadPool::ShutdownMode::CancelPending);
	blocker.get();

	std::size_t broken = 0;
	for (auto &future : futures) {
		try {
			future.get();
		} catch (const std::future_error &) {
			++broken;
		}
	}

	assert(broken > 0);
	assert(executed.load() < config.tasks + 1);
	std::cout << "cancel pending: " << broken << " queued tasks canceled\n";
}

void run_stop_token_stress(const StressConfig &config)
{
	threadpool::ThreadPool pool(config.workers);
	threadpool::ThreadPool::StopSource stop_source;
	std::atomic<std::size_t> observed_stops{0};
	std::vector<std::future<void>> futures;
	futures.reserve(config.workers);

	for (std::size_t i = 0; i < config.workers; ++i) {
		futures.push_back(pool.submit_with_stop(
			stop_source.token(),
			[&observed_stops](threadpool::ThreadPool::StopToken token) {
				while (!token.stop_requested()) {
					std::this_thread::yield();
				}
				observed_stops.fetch_add(1);
			}
		));
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	stop_source.request_stop();

	for (auto &future : futures) {
		future.get();
	}

	assert(observed_stops.load() == config.workers);
	std::cout << "stop token: " << observed_stops.load() << " workers observed stop\n";
}

} // namespace

int main(int argc, char **argv)
{
	const StressConfig config = parse_config(argc, argv);
	if (config.tasks < config.submitters) {
		throw std::invalid_argument("tasks must be >= submitters");
	}

	std::cout << "stress config: tasks=" << config.tasks
		<< ", workers=" << config.workers
		<< ", submitters=" << config.submitters << '\n';

	run_many_future_tasks(config);
	run_parallel_submitters(config);
	run_cancel_pending_stress(config);
	run_stop_token_stress(config);

	std::cout << "stress test passed\n";
	return 0;
}
