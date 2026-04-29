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
	std::size_t m_active_tasks; // 正在执行的任务数量
	SafeQueue<std::function<void()>> m_queue; // 执行函数安全队列，即任务队列
	std::vector<std::thread> m_threads; // 工作线程队列​
	mutable std::mutex m_conditional_mutex; // 线程休眠锁互斥变量
	std::condition_variable m_conditional_lock; // 线程环境锁，可以让线程处于休眠或者唤醒状态
	std::condition_variable m_idle_lock; // 队列和执行中的任务全部清空时通知
	
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
			std::function<void()> func;
			bool dequeued;// 判断是否正在取出队列任务
			// 从任务队列获取任务，需要加锁
			while(true){
				//加锁作用域
				{
					std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);
					m_pool->m_conditional_lock.wait(lock, [this] {
						return m_pool->m_shutdown || !m_pool->m_queue.empty();
					}); // 等待任务入队或线程池关闭
					if(m_pool->m_shutdown && m_pool->m_queue.empty()){
						return;
					}
					dequeued = m_pool->m_queue.pop(func);
					if (dequeued) {
						++m_pool->m_active_tasks;
					}
				}
				if(dequeued){
					func();
					std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);
					--m_pool->m_active_tasks;
					if (m_pool->m_queue.empty() && m_pool->m_active_tasks == 0) {
						m_pool->m_idle_lock.notify_all();
					}
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
			m_pool->submit([handle] {
				handle.resume();
			});
		}

		void await_resume() const noexcept
		{
		}
	};
#endif

	ThreadPool(const std::size_t n_threads = 4): m_shutdown(false), m_started(false), m_active_tasks(0), m_threads(n_threads)
	{
		if (n_threads == 0) {
			throw std::invalid_argument("ThreadPool requires at least one worker thread");
		}
		init();
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
			return m_queue.empty() && m_active_tasks == 0;
		});
	}

	template <typename Rep, typename Period>
	bool wait_idle_for(const std::chrono::duration<Rep, Period> &timeout){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_idle_lock.wait_for(lock, timeout, [this] {
			return m_queue.empty() && m_active_tasks == 0;
		});
	}

	template <typename Clock, typename Duration>
	bool wait_idle_until(const std::chrono::time_point<Clock, Duration> &deadline){
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_idle_lock.wait_until(lock, deadline, [this] {
			return m_queue.empty() && m_active_tasks == 0;
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

	std::size_t active_tasks() const {
		std::unique_lock<std::mutex> lock(m_conditional_mutex);
		return m_active_tasks;
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
		return ScheduleAwaiter(this);
	}
#endif
	
	
	

};	

} // namespace threadpool

using ThreadPool = threadpool::ThreadPool;
