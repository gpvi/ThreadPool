#define THREADPOOL_USE_GLOBAL_NAMESPACE
#include "threadpool/ThreadPool.h"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

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

CoroutineTask switch_to_threadpool(ThreadPool &pool, std::atomic<int> &value, std::thread::id &worker_id)
{
	const auto caller_id = std::this_thread::get_id();
	co_await pool.schedule();
	worker_id = std::this_thread::get_id();
	assert(worker_id != caller_id);
	value.store(42);
}

CoroutineTask yielding_work(ThreadPool &pool, std::vector<int> &steps, const int id)
{
	co_await pool.schedule();
	steps.push_back(id * 10);
	co_await pool.yield();
	steps.push_back(id * 10 + 1);
	co_await pool.yield();
	steps.push_back(id * 10 + 2);
}

CoroutineTask rejected_coroutine_schedule(ThreadPool &pool)
{
	co_await pool.schedule();
}

CoroutineTask scheduled_before_shutdown(ThreadPool &pool, std::atomic<int> &value)
{
	co_await pool.schedule();
	value.store(1);
}

// Work stealing: coroutines assigned to one worker get stolen by idle workers
CoroutineTask stealable_work(ThreadPool &pool, std::atomic<int> &counter)
{
	co_await pool.schedule();
	counter.fetch_add(1, std::memory_order_relaxed);
}

// Burst limit: after exceeding coroutine_burst_limit, worker picks up global task
CoroutineTask bursty_work(ThreadPool &pool, std::atomic<int> &counter)
{
	co_await pool.schedule();
	for (int i = 0; i < 3; ++i) {
		counter.fetch_add(1, std::memory_order_relaxed);
		co_await pool.yield();
	}
}

int main()
{
	{
		ThreadPool pool(2, ThreadPool::ExecutionMode::ThreadAndCoroutine);
		std::atomic<int> value{0};
		std::thread::id worker_id;

		auto task = switch_to_threadpool(pool, value, worker_id);
		task.get();

		assert(value.load() == 42);
		assert(worker_id != std::thread::id{});
	}

	{
		ThreadPool pool(1, ThreadPool::ExecutionMode::ThreadAndCoroutine);
		std::vector<int> steps;
		assert(pool.queued_coroutines() == 0);

		auto first = yielding_work(pool, steps, 1);
		auto second = yielding_work(pool, steps, 2);

		first.get();
		second.get();

		const std::vector<int> expected{10, 20, 11, 21, 12, 22};
		assert(steps == expected);
		assert(pool.queued_coroutines() == 0);
	}

	{
		ThreadPool pool(1);
		assert(pool.execution_mode() == ThreadPool::ExecutionMode::ThreadOnly);
		assert(!pool.is_coroutine_enabled());

		auto task = rejected_coroutine_schedule(pool);
		bool threw = false;
		try {
			task.get();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		assert(threw);
	}

	{
		ThreadPool pool(1, ThreadPool::ExecutionMode::ThreadAndCoroutine);
		std::atomic<int> value{0};

		auto task = scheduled_before_shutdown(pool, value);
		pool.shutdown(ThreadPool::ShutdownMode::CancelPending);
		task.get();

		assert(value.load() == 1);
	}

	// Work stealing: multiple workers pull coroutines from each other's queues
	{
		ThreadPool pool(2, ThreadPool::ExecutionMode::ThreadAndCoroutine);
		std::atomic<int> counter{0};
		std::vector<CoroutineTask> tasks;
		for (int i = 0; i < 20; ++i) {
			tasks.push_back(stealable_work(pool, counter));
		}
		for (auto &task : tasks) {
			task.get();
		}
		assert(counter.load() == 20);
	}

	// Coroutine burst limit: global tasks interleave with coroutine bursts
	{
		ThreadPool::Options options;
		options.min_workers = 1;
		options.max_workers = 1;
		options.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
		options.coroutine_burst_limit = 2;
		ThreadPool pool(options);
		std::atomic<int> counter{0};

		auto coro = bursty_work(pool, counter);
		pool.submit([&counter] {
			counter.fetch_add(100, std::memory_order_relaxed);
		});
		coro.get();
		pool.wait_idle();

		assert(counter.load() >= 103); // 3 from coroutine + at least 100 from task
	}

	// Coroutine queue size limit rejects excess coroutines
	{
		ThreadPool::Options options;
		options.min_workers = 1;
		options.max_workers = 1;
		options.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
		options.max_coroutine_queue_size = 1;
		ThreadPool pool(options);

		// Block the worker so the queue backs up
		std::promise<void> blocker;
		auto block_future = blocker.get_future();
		pool.submit([&] {
			block_future.get();
		});

		std::atomic<int> value{0};
		auto first = scheduled_before_shutdown(pool, value);
		// Queue should have 1 coroutine (first), pushing another should fail
		bool threw = false;
		try {
			auto second = scheduled_before_shutdown(pool, value);
			second.get();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		blocker.set_value();
		first.get();
		assert(threw);
	}
	return 0;
}
