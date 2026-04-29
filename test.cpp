#include "ThreadPool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

void test_submit_returns_future_value()
{
	ThreadPool pool(2);
	auto result = pool.submit([] {
		return 42;
	});

	assert(result.get() == 42);
}

void test_all_queued_tasks_run_before_shutdown()
{
	ThreadPool pool(3);
	std::atomic<int> counter{0};
	std::vector<std::future<void>> futures;

	for (int i = 0; i < 50; ++i) {
		futures.push_back(pool.submit([&counter] {
			counter.fetch_add(1);
		}));
	}

	pool.shutdown();

	for (auto &future : futures) {
		future.get();
	}
	assert(counter.load() == 50);
}

void test_submit_after_shutdown_throws()
{
	ThreadPool pool(1);
	pool.shutdown();

	bool threw = false;
	try {
		pool.submit([] {});
	} catch (const std::runtime_error &) {
		threw = true;
	}

	assert(threw);
}

void test_constructor_starts_workers()
{
	ThreadPool pool(1);
	auto result = pool.submit([] {
		return 7;
	});

	assert(result.get() == 7);
}

void test_zero_worker_count_throws()
{
	bool threw = false;
	try {
		ThreadPool pool(0);
	} catch (const std::invalid_argument &) {
		threw = true;
	}

	assert(threw);
}

void test_wait_idle_observes_completed_work()
{
	ThreadPool pool(2);
	std::atomic<int> counter{0};

	for (int i = 0; i < 20; ++i) {
		pool.submit([&counter] {
			counter.fetch_add(1);
		});
	}

	pool.wait_idle();

	assert(counter.load() == 20);
	assert(pool.queued_tasks() == 0);
	assert(pool.active_tasks() == 0);
	assert(pool.worker_count() == 2);
	assert(!pool.is_shutdown());
}

void test_cancel_pending_shutdown_breaks_unstarted_futures()
{
	ThreadPool pool(1);
	std::promise<void> started;
	auto started_future = started.get_future();
	auto blocker = pool.submit([&started] {
		started.set_value();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	});
	started_future.get();

	auto canceled = pool.submit([] {
		return 99;
	});

	pool.shutdown(ThreadPool::ShutdownMode::CancelPending);
	blocker.get();

	bool threw = false;
	try {
		canceled.get();
	} catch (const std::future_error &) {
		threw = true;
	}

	assert(threw);
	assert(pool.is_shutdown());
}

void test_namespace_type_is_available()
{
	threadpool::ThreadPool pool(1);
	auto result = pool.submit([] {
		return 11;
	});

	assert(result.get() == 11);
}

void test_wait_idle_for_times_out_and_then_succeeds()
{
	ThreadPool pool(1);
	auto running = pool.submit([] {
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	});

	assert(!pool.wait_idle_for(std::chrono::milliseconds(10)));
	running.get();
	assert(pool.wait_idle_for(std::chrono::seconds(1)));
}

void test_submit_with_stop_observes_requested_stop()
{
	ThreadPool pool(1);
	ThreadPool::StopSource stop_source;

	auto result = pool.submit_with_stop(stop_source.token(), [](ThreadPool::StopToken token) {
		for (int i = 0; i < 100; ++i) {
			if (token.stop_requested()) {
				return i;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return 100;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	stop_source.request_stop();

	assert(result.get() < 100);
}

int main()
{
	test_submit_returns_future_value();
	test_all_queued_tasks_run_before_shutdown();
	test_submit_after_shutdown_throws();
	test_constructor_starts_workers();
	test_zero_worker_count_throws();
	test_wait_idle_observes_completed_work();
	test_cancel_pending_shutdown_breaks_unstarted_futures();
	test_namespace_type_is_available();
	test_wait_idle_for_times_out_and_then_succeeds();
	test_submit_with_stop_observes_requested_stop();
	return 0;
}
