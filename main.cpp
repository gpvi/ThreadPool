#include "ThreadPool.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <random>
#include <thread>

using namespace std;

// 设置线程睡眠时间
void simulate_hard_computation()
{
	thread_local mt19937 mt(random_device{}());
	uniform_int_distribution<int> dist(-1000, 1000); // 均匀分布的整数
	this_thread::sleep_for(chrono::milliseconds(2000 + dist(mt)));
}
// 添加两个数字的简单函数并打印结果
void multiply(const int a, const int b)
{
	simulate_hard_computation();
	const int res = a * b;
	std::cout << a << " * " << b << " = " << res << std::endl;
}

// 添加并输出结果
void multiply_output(int &out, const int a, const int b)
{
	simulate_hard_computation();
	out = a * b;
	std::cout << a << " * " << b << " = " << out << std::endl;
}

// 结果返回
int multiply_return(const int a, const int b)
{
	simulate_hard_computation();
	const int res = a * b;
	std::cout << a << " * " << b << " = " << res << std::endl;
	return res;
}

void example(){
	ThreadPool pool(3);
	for(int i=0;i<=2;i++){
		//TODO
		for(int j=0;j<=9;j++){
			//TODO
			pool.submit(multiply,i,j);
				
		}
	}
	// 使用ref传递的输出参数提交函数
	int output_ref;
	auto future1 = pool.submit(multiply_output, std::ref(output_ref), 5, 6);
	
	// 等待乘法输出完成
	future1.get();
	std::cout << "Last operation result is equals to " << output_ref << std::endl;
	
	// 使用return参数提交函数
	auto future2 = pool.submit(multiply_return, 5, 3);
	
	
	// 等待乘法输出完成
	int res = future2.get();
	std::cout << "Last operation result is equals to " << res << std::endl;
	
	
	// 关闭线程池
	pool.shutdown();
}
int main(){
	example();
	return  0;
}
