#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct CompareConfig {
	std::size_t workers = std::thread::hardware_concurrency() == 0 ? 4 : std::thread::hardware_concurrency();
	std::size_t tasks = 20000;
	std::size_t coroutine_count = 2000;
	std::size_t yields_per_coroutine = 10;
};

std::size_t parse_arg(const char *value, const std::size_t fallback)
{
	const auto parsed = std::strtoull(value, nullptr, 10);
	return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

CompareConfig parse_config(int argc, char **argv)
{
	CompareConfig config;
	if (argc > 1) {
		config.tasks = parse_arg(argv[1], config.tasks);
	}
	if (argc > 2) {
		config.coroutine_count = parse_arg(argv[2], config.coroutine_count);
	}
	if (argc > 3) {
		config.yields_per_coroutine = parse_arg(argv[3], config.yields_per_coroutine);
	}
	if (argc > 4) {
		config.workers = parse_arg(argv[4], config.workers);
	}
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

class CoroutineTask {
private:
	std::future<void> m_done;

public:
	struct promise_type {
		std::promise<void> done;

		CoroutineTask get_return_object()
		{
			return CoroutineTask(done.get_future());
		}

		std::suspend_never initial_suspend() const noexcept
		{
			return {};
		}

		std::suspend_never final_suspend() const noexcept
		{
			return {};
		}

		void return_void()
		{
			done.set_value();
		}

		void unhandled_exception()
		{
			done.set_exception(std::current_exception());
		}
	};

	explicit CoroutineTask(std::future<void> done): m_done(std::move(done))
	{
	}

	void get()
	{
		m_done.get();
	}
};

CoroutineTask coroutine_work(
	threadpool::ThreadPool &pool,
	std::atomic<std::size_t> &counter,
	const std::size_t yields_per_coroutine
)
{
	co_await pool.schedule();
	for (std::size_t i = 0; i < yields_per_coroutine; ++i) {
		counter.fetch_add(1, std::memory_order_relaxed);
		co_await pool.yield();
	}
	counter.fetch_add(1, std::memory_order_relaxed);
}

long long run_thread_only(const CompareConfig &config)
{
	threadpool::ThreadPool pool(
		config.workers,
		threadpool::ThreadPool::ExecutionMode::ThreadOnly
	);
	std::atomic<std::size_t> counter{0};

	const auto elapsed_ms = measure_ms([&] {
		for (std::size_t i = 0; i < config.tasks; ++i) {
			pool.submit([&counter] {
				counter.fetch_add(1, std::memory_order_relaxed);
			});
		}
		pool.wait_idle();
	});

	if (counter.load() != config.tasks) {
		throw std::runtime_error("thread-only counter mismatch");
	}
	return elapsed_ms;
}

long long run_thread_and_coroutine(const CompareConfig &config)
{
	threadpool::ThreadPool pool(
		config.workers,
		threadpool::ThreadPool::ExecutionMode::ThreadAndCoroutine
	);
	std::atomic<std::size_t> counter{0};
	std::vector<CoroutineTask> coroutines;
	coroutines.reserve(config.coroutine_count);

	const auto elapsed_ms = measure_ms([&] {
		for (std::size_t i = 0; i < config.coroutine_count; ++i) {
			coroutines.push_back(coroutine_work(
				pool,
				counter,
				config.yields_per_coroutine
			));
		}

		for (auto &coroutine : coroutines) {
			coroutine.get();
		}
	});

	const auto expected = config.coroutine_count * (config.yields_per_coroutine + 1);
	if (counter.load() != expected) {
		throw std::runtime_error("thread-and-coroutine counter mismatch");
	}
	return elapsed_ms;
}

double ops_per_second(const std::size_t operations, const long long elapsed_ms)
{
	if (elapsed_ms == 0) {
		return 0.0;
	}
	return static_cast<double>(operations) * 1000.0 / static_cast<double>(elapsed_ms);
}

void print_result(
	const char *name,
	const std::size_t workers,
	const std::size_t operations,
	const long long elapsed_ms
)
{
	std::cout << std::left << std::setw(24) << name
		<< std::right << std::setw(8) << workers
		<< std::setw(14) << operations
		<< std::setw(12) << elapsed_ms
		<< std::setw(16) << static_cast<std::size_t>(ops_per_second(operations, elapsed_ms))
		<< '\n';
}

} // namespace

int main(int argc, char **argv)
{
	const auto config = parse_config(argc, argv);
	const auto coroutine_operations = config.coroutine_count * (config.yields_per_coroutine + 1);

	std::cout << "mode compare config: tasks=" << config.tasks
		<< ", coroutines=" << config.coroutine_count
		<< ", yields_per_coroutine=" << config.yields_per_coroutine
		<< ", workers=" << config.workers << '\n';

	std::cout << std::left << std::setw(24) << "mode"
		<< std::right << std::setw(8) << "workers"
		<< std::setw(14) << "operations"
		<< std::setw(12) << "ms"
		<< std::setw(16) << "ops/sec"
		<< '\n';

	const auto thread_only_ms = run_thread_only(config);
	print_result("thread_only", config.workers, config.tasks, thread_only_ms);

	const auto coroutine_ms = run_thread_and_coroutine(config);
	print_result("thread_and_coroutine", config.workers, coroutine_operations, coroutine_ms);

	std::cout << "note: coroutine operations include each yield step plus final completion step\n";
	return 0;
}
