#include<bits/stdc++.h>
#include"SafeQueue.h"
using namespace std;


class ThreadPool {
private:
	bool m_shutdown; // 线程池是否关闭
	SafeQueue<std::function<void()>> m_queue; // 执行函数安全队列，即任务队列
	std::vector<std::thread> m_threads; // 工作线程队列​
	std::mutex m_conditional_mutex; // 线程休眠锁互斥变量
	std::condition_variable m_conditional_lock; // 线程环境锁，可以让线程处于休眠或者唤醒状态
	
	class Worker {
		// work_id
	private:
		int m_id;
		
		ThreadPool *m_pool;//所属线程池指针
	public:
		Worker(ThreadPool* pool, const int id): m_pool(pool),m_id(id) 
		{		
		}
		
		//重载()
		void operator()(){
			function<void()> func;
			bool dequeued;// 判断是否正在取出队列任务
			// 从任务队列获取任务，需要加锁
			while(!m_pool->m_shutdown){
			
				//加锁作用域
				{
					unique_lock<mutex> lock(m_pool->m_conditional_mutex);
					if(m_pool->m_queue.empty()){
						 m_pool->m_conditional_lock.wait(lock); // 等待条件变量通知，开启线程				
					}
					dequeued = m_pool->m_queue.pop(func);
				}
				if(dequeued){
					func();
				}
			}
		}
	};

public:
	ThreadPool(const int n_threads = 4): m_threads(std::vector<std::thread>(n_threads)), m_shutdown(false)
	{
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
		for (int i = 0;i<m_threads.size();i++){
			// 创建线程，传入线程池地址与workid
			m_threads.at(i) = thread(Worker(this,i));
		}
	}
	// 
	void shutdown(){
		m_shutdown = true;
		m_conditional_lock.notify_all(); // 通知，唤醒所有工作线程
		for (int i = 0; i < m_threads.size(); ++i)
		{
			if (m_threads.at(i).joinable()) // 判断线程是否在等待
			{
				m_threads.at(i).join(); // 将线程加入到等待队列
			}
		}
	}
	
	
	template <typename F,typename... Args>
	auto submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))> {
		// 调用打包
//		function<decltype(f(args...))()> func = bind(forward<F>(f),forward<Args>(args));
		std::function<decltype(f(args...))()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
		//std::packaged_task可以用来封装任何可以调用的目标，从而用于实现异步的调用。
		//使用std::make_shared<>()方法，声明了一个std::packaged_task<decltype(f(args...))()>类型的智能指针
		auto task_ptr = make_shared<packaged_task<decltype(f(args...))()>>(func);
		std::function<void()> warpper_func = [task_ptr]()
		{
			(*task_ptr)();
		};
	   // 队列通用安全封包函数，并压入安全队列
		m_queue.push(warpper_func);
		// 唤醒一个等待中的线程
		m_conditional_lock.notify_one();
		// 返回先前注册的任务指针
		return task_ptr->get_future();
	}
	
	
	

};	
