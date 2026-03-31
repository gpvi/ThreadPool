#include<bits/stdc++.h>
#include<mutex>
#include<thread>
using namespace std;
template <typename T>
/**
 * 
 * 实现线程安全的队列，对队列的入队，出队操作加锁
 */
class SafeQueue {
private:
	queue<T> m_queue;
	mutex m_mutex;
public:
	SafeQueue(){}
	SafeQueue(SafeQueue && other){}
    ~SafeQueue(){}	
	
	
	bool empty(){
		unique_lock <mutex> lock(m_mutex);
		return m_queue.empty();
	}
	
	int size(){
		unique_lock <mutex> lock(m_mutex);
		return m_queue.size();
	}
	
	void push(T &t){
		unique_lock <mutex> lock(m_mutex);
		m_queue.push(t);
	}
	
	bool pop(T &t){
		unique_lock <mutex> lock(m_mutex);
		if (m_queue.empty()) return false;
		
		t = move(m_queue.front());
		m_queue.pop();
		return true;
	}
	
};


