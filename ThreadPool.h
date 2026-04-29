#pragma once

#include "SafeQueue.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#include <coroutine>
#define THREADPOOL_HAS_COROUTINE 1
#endif

namespace threadpool {

namespace detail {

#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
template <typename F, typename... Args>
using invoke_result_t = std::invoke_result_t<F, Args...>;
#else
template <typename F, typename... Args>
using invoke_result_t = typename std::result_of<F(Args...)>::type;
#endif

} // namespace detail

class ThreadPool {
public:
	enum class ShutdownMode {
		Drain,
		CancelPending
	};

	enum class ExecutionMode {
		ThreadOnly,
		ThreadAndCoroutine
	};

	struct Options {
		std::size_t worker_count;
		ExecutionMode execution_mode;

		Options(): worker_count(4), execution_mode(ExecutionMode::ThreadOnly)
		{
		}
	};

	class StopToken {
	private:
		std::shared_ptr<std::atomic<bool>> m_stop_requested;

	public:
		StopToken(): m_stop_requested(std::make_shared<std::atomic<bool>>(false))
		{
		}

		bool stop_requested() const noexcept
		{
			return m_stop_requested->load();
		}

	private:
		friend class ThreadPool;

		explicit StopToken(std::shared_ptr<std::atomic<bool>> stop_requested):
			m_stop_requested(std::move(stop_requested))
		{
		}
	};

	class StopSource {
	private:
		std::shared_ptr<std::atomic<bool>> m_stop_requested;

	public:
		StopSource(): m_stop_requested(std::make_shared<std::atomic<bool>>(false))
		{
		}

		StopToken token() const
		{
			return StopToken(m_stop_requested);
		}

		void request_stop() const noexcept
		{
			m_stop_requested->store(true);
		}
	};

private:
	bool m_shutdown; // 线程池是否关闭
	bool m_started; // 线程池是否已经启动
	ExecutionMode m_execution_mode; // 线程池运行模式
	std::size_t m_active_tasks; // 正在执行的任务数量
	std::size_t m_next_coroutine_worker; // 协程首次调度时轮询选择 worker
	std::size_t m_coroutine_burst_limit; // 连续执行协程恢复任务的上限，避免普通任务饥饿
	SafeQueue<std::function<void()>> m_queue; // 执行函数安全队列，即任务队列
	std::vector<std::unique_ptr<SafeQueue<std::function<void()>>>> m_coroutine_queues; // 每个 worker 的协程队列
	std::vector<std::thread> m_threads; // 工作线程队列​
	mutable std::mutex m_conditional_mutex; // 线程休眠锁互斥变量
	std::condition_variable m_conditional_lock; // 线程环境锁，可以让线程处于休眠或者唤醒状态
	std::condition_variable m_idle_lock; // 队列和执行中的任务全部清空时通知

	static ThreadPool *&current_pool()
	{
		static thread_local ThreadPool *pool = nullptr;
		return pool;
	}

	static std::size_t &current_worker_id()
	{
		static thread_local std::size_t worker_id = 0;
		return worker_id;
	}

	bool worker_has_coroutine(const std::size_t worker_id) const
	{
		return coroutine_enabled()
			&& worker_id < m_coroutine_queues.size()
			&& !m_coroutine_queues.at(worker_id)->empty();
	}

	bool coroutine_queues_empty() const
	{
		if (!coroutine_enabled()) {
			return true;
		}
		for (const auto &queue : m_coroutine_queues) {
			if (!queue->empty()) {
				return false;
			}
		}
		return true;
	}

	bool steal_coroutine(const std::size_t worker_id, std::function<void()> &func)
	{
		if (!coroutine_enabled() || m_coroutine_queues.empty()) {
			return false;
		}
		for (std::size_t offset = 1; offset < m_coroutine_queues.size(); ++offset) {
			const std::size_t victim = (worker_id + offset) % m_coroutine_queues.size();
			if (m_coroutine_queues.at(victim)->pop(func)) {
				return true;
			}
		}
		return false;
	}

	bool pop_worker_task(
		const std::size_t worker_id,
		const std::size_t coroutine_burst,
		bool &popped_coroutine,
		std::function<void()> &func
	)
	{
		popped_coroutine = false;
		if (coroutine_burst >= m_coroutine_burst_limit && m_queue.pop(func)) {
			return true;
		}
		if (worker_has_coroutine(worker_id) && m_coroutine_queues.at(worker_id)->pop(func)) {
			popped_coroutine = true;
			return true;
		}
		if (m_queue.pop(func)) {
			return true;
		}
		popped_coroutine = steal_coroutine(worker_id, func);
		return popped_coroutine;
	}

	bool has_any_work_for_worker(const std::size_t worker_id) const
	{
		if (!m_queue.empty() || worker_has_coroutine(worker_id)) {
			return true;
		}
		if (!coroutine_enabled()) {
			return false;
		}
		for (std::size_t offset = 1; offset < m_coroutine_queues.size(); ++offset) {
			const std::size_t victim = (worker_id + offset) % m_coroutine_queues.size();
			if (!m_coroutine_queues.at(victim)->empty()) {
				return true;
			}
		}
		return false;
	}

	void notify_idle_if_needed()
	{
		if (m_queue.empty() && coroutine_queues_empty() && m_active_tasks == 0) {
			m_idle_lock.notify_all();
		}
	}

#ifdef THREADPOOL_HAS_COROUTINE
	void enqueue_coroutine_resume(std::coroutine_handle<> handle, const bool prefer_current_worker)
	{
		std::size_t target_worker = 0;
		{
			std::unique_lock<std::mutex> lock(m_conditional_mutex);
			if (m_shutdown) {
				throw std::runtime_error("schedule coroutine on stopped ThreadPool");
			}
			if (!coroutine_enabled()) {
				throw std::runtime_error("coroutine scheduling requires ThreadAndCoroutine mode");
			}

			if (
				prefer_current_worker
				&& current_pool() == this
				&& current_worker_id() < m_coroutine_queues.size()
			) {
				target_worker = current_worker_id();
			} else {
				target_worker = m_next_coroutine_worker;
				m_next_coroutine_worker = (m_next_coroutine_worker + 1) % m_coroutine_queues.size();
			}

			m_coroutine_queues.at(target_worker)->push([handle] {
				handle.resume();
			});
		}
		m_conditional_lock.notify_all();
	}
#endif
	
	class Worker {
		// work_id
	private:
		int m_id;
		
		ThreadPool *m_pool;//所属线程池指针
	public:
		Worker(ThreadPool* pool, const int id): m_id(id), m_pool(pool) 
		{		
		}
		
		//重载()
		void operator()(){
			current_pool() = m_pool;
			current_worker_id() = static_cast<std::size_t>(m_id);
			std::function<void()> func;
			bool dequeued;// 判断是否正在取出队列任务
			bool popped_coroutine = false;
			std::size_t coroutine_burst = 0;
			// 从任务队列获取任务，需要加锁
			while(true){
				//加锁作用域
				{
					std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);
					m_pool->m_conditional_lock.wait(lock, [this] {
						return m_pool->m_shutdown
							|| m_pool->has_any_work_for_worker(static_cast<std::size_t>(m_id));
					}); // 等待任务入队或线程池关闭
					if(
						m_pool->m_shutdown
						&& m_pool->m_queue.empty()
						&& !m_pool->has_any_work_for_worker(static_cast<std::size_t>(m_id))
					){
						return;
					}
					dequeued = m_pool->pop_worker_task(
						static_cast<std::size_t>(m_id),
						coroutine_burst,
						popped_coroutine,
						func
					);
					if (dequeued) {
						++m_pool->m_active_tasks;
					}
				}
				if(dequeued){
					func();
					std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);
					if (popped_coroutine) {
						++coroutine_burst;
					} else {
						coroutine_burst = 0;
					}
					--m_pool->m_active_tasks;
					m_pool->notify_idle_if_needed();
				}
			}
		}
	};

public:
#ifdef THREADPOOL_HAS_COROUTINE
	class ScheduleAwaiter {
	private:
		ThreadPool *m_pool;

	public:
		explicit ScheduleAwaiter(ThreadPool *pool): m_pool(pool)
		{
		}

		bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> handle)
		{
			m_pool->enqueue_coroutine_resume(handle, false);
		}

		void await_resume() const noexcept
		{
		}
	};

	class YieldAwaiter {
	private:
		ThreadPool *m_pool;

	public:
		explicit YieldAwaiter(ThreadPool *pool): m_pool(pool)
		{
		}

		bool await_ready() const noexcept
		{
			return false;
		}

		void await_suspend(std::coroutine_handle<> handle)
		{
			m_pool->enqueue_coroutine_resume(handle, true);
		}

		void await_resume() const noexcept
		{
		}
	};
#endif

	ThreadPool(const std::size_t n_threads = 4):
		ThreadPool(n_threads, ExecutionMode::ThreadOnly)
	{
	}

	ThreadPool(const std::size_t n_threads, const ExecutionMode execution_mode):
		m_shutdown(false),
		m_started(false),
		m_execution_mode(execution_mode),
		m_active_tasks(0),
		m_next_coroutine_worker(0),
		m_coroutine_burst_limit(8),
		m_threads(n_threads)
	{
		if (n_threads == 0) {
			throw std::invalid_argument("ThreadPool requires at least one worker thread");
		}
		m_coroutine_queues.reserve(n_threads);
		for (std::size_t i = 0; i < n_threads; ++i) {
			m_coroutine_queues.push_back(std::unique_ptr<SafeQueue<std::function<void()>>>(
				new SafeQueue<std::function<void()>>
			));
		}
		init();
	}

	explicit ThreadPool(const Options &options):
		ThreadPool(options.worker_count, options.execution_mode)
	{
	}
	~ThreadPool() noexcept {
		try {
			shutdown();
		} catch (...) {
		}
	}
	// 禁用了拷贝构造函数，防止通过拷贝构造函数创建 ThreadPool 类的对象的副本。
	ThreadPool(const ThreadPool &) = delete;
	// 禁用了移动构造函数，防止通过移动构造函数创建 ThreadPool 类的对象。
	ThreadPool(ThreadPool &&) = delete;
	//禁用了拷贝赋值运算符，防止通过拷贝赋值运算符将一个 ThreadPool 对象赋值给另一个对象。
	ThreadPool &operator=(const ThreadPool &) = delete;
	//禁用了移动赋值运算符，防止通过移动赋值运算符将一个 ThreadPool 对象赋值给另一个对象。
	ThreadPool &operator=(ThreadPool &&) = delete;
	
	void init(){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		if (m_started) {
			return;
		}
		m_started = true;
		lock.unlock();
		try {
			for (std::size_t i = 0;i<m_threads.size();i++){
				// 创建线程，传入线程池地址与workid
				m_threads.at(i) = std::thread(Worker(this, static_cast<int>(i)));
			}
		} catch (...) {
			shutdown(ShutdownMode::CancelPending);
			throw;
		}
	}
	// 
	void shutdown(const ShutdownMode mode = ShutdownMode::Drain){
		{
			std::unique_lock<std::mutex> lock(m_conditional_mutex);
			if (m_shutdown) {
				return;
			}
			m_shutdown = true;
			if (mode == ShutdownMode::CancelPending) {
				m_queue.clear();
			}
		}
		m_idle_lock.notify_all();
		m_conditional_lock.notify_all(); // 通知，唤醒所有工作线程
		for (std::size_t i = 0; i < m_threads.size(); ++i)
		{
			if (m_threads.at(i).joinable()) // 判断线程是否在等待
			{
				m_threads.at(i).join(); // 将线程加入到等待队列
			}
		}
	}

	void wait_idle(){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		m_idle_lock.wait(lock, [this] {
			return m_queue.empty() && coroutine_queues_empty() && m_active_tasks == 0;
		});
	}

	template <typename Rep, typename Period>
	bool wait_idle_for(const std::chrono::duration<Rep, Period> &timeout){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_idle_lock.wait_for(lock, timeout, [this] {
			return m_queue.empty() && coroutine_queues_empty() && m_active_tasks == 0;
		});
	}

	template <typename Clock, typename Duration>
	bool wait_idle_until(const std::chrono::time_point<Clock, Duration> &deadline){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_idle_lock.wait_until(lock, deadline, [this] {
			return m_queue.empty() && coroutine_queues_empty() && m_active_tasks == 0;
		});
	}

	bool is_shutdown() const {
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_shutdown;
	}

	std::size_t worker_count() const {
		return m_threads.size();
	}

	std::size_t queued_tasks() const {
		return m_queue.size();
	}

	std::size_t queued_coroutines() const {
		std::size_t total = 0;
		for (const auto &queue : m_coroutine_queues) {
			total += queue->size();
		}
		return total;
	}

	std::size_t active_tasks() const {
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_active_tasks;
	}

	ExecutionMode execution_mode() const {
		return m_execution_mode;
	}

	bool coroutine_enabled() const {
		return m_execution_mode == ExecutionMode::ThreadAndCoroutine;
	}
	
	
	template <typename F,typename... Args>
	auto submit(F &&f, Args &&...args) -> std::future<detail::invoke_result_t<F, Args...>> {
		using return_type = detail::invoke_result_t<F, Args...>;
		// 调用打包
//		function<decltype(f(args...))()> func = bind(forward<F>(f),forward<Args>(args));
		std::function<return_type()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
		//std::packaged_task可以用来封装任何可以调用的目标，从而用于实现异步的调用。
		//使用std::make_shared<>()方法，声明了一个std::packaged_task<decltype(f(args...))()>类型的智能指针
		auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(func);
		std::function<void()> warpper_func = [task_ptr]()
		{
			(*task_ptr)();
		};
	   // 队列通用安全封包函数，并压入安全队列
		{
			std::unique_lock<std::mutex> lock(m_conditional_mutex);
			if (m_shutdown) {
				throw std::runtime_error("submit on stopped ThreadPool");
			}
			m_queue.push(std::move(warpper_func));
		}
		// 唤醒一个等待中的线程
		m_conditional_lock.notify_one();
		// 返回先前注册的任务指针
		return task_ptr->get_future();
	}

	template <typename F, typename... Args>
	auto submit_with_stop(StopToken token, F &&f, Args &&...args)
		-> std::future<detail::invoke_result_t<F, StopToken, Args...>> {
		return submit(
			std::forward<F>(f),
			std::move(token),
			std::forward<Args>(args)...
		);
	}

#ifdef THREADPOOL_HAS_COROUTINE
	ScheduleAwaiter schedule()
	{
		if (!coroutine_enabled()) {
			throw std::runtime_error("coroutine scheduling requires ThreadAndCoroutine mode");
		}
		return ScheduleAwaiter(this);
	}

	YieldAwaiter yield()
	{
		if (!coroutine_enabled()) {
			throw std::runtime_error("coroutine yield requires ThreadAndCoroutine mode");
		}
		return YieldAwaiter(this);
	}
#endif
	
	
	

};	

} // namespace threadpool

using ThreadPool = threadpool::ThreadPool;
