#include "ThreadPool.h"

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
		assert(!pool.coroutine_enabled());

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
	return 0;
}
