#include "ThreadPool.h"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <future>
#include <thread>

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

int main()
{
	ThreadPool pool(2);
	std::atomic<int> value{0};
	std::thread::id worker_id;

	auto task = switch_to_threadpool(pool, value, worker_id);
	task.get();

	assert(value.load() == 42);
	assert(worker_id != std::thread::id{});
	return 0;
}
