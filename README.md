# ThreadPool

一个 C++ 轻量级线程池项目，用于演示固定线程池、任务调度、异步结果返回、协作式取消、C++20 协程调度，以及基础压力测试和 benchmark。

项目定位偏学习与工程实践：代码规模较小，但覆盖了线程池从“能运行”到“可测试、可观测、可扩展”的关键设计点。详细设计说明见 [docs/DESIGN.md](docs/DESIGN.md)。

## 核心能力

- 固定数量 worker 线程，复用线程资源，减少频繁创建线程的开销。
- 基于线程安全 FIFO 队列和条件变量完成任务调度、阻塞等待和唤醒。
- 支持普通函数、lambda、成员函数等任务形式。
- 使用 `std::packaged_task` 和 `std::future` 支持异步返回值和异常传播。
- 支持自动启动、析构自动关闭、优雅排空和取消排队任务。
- 支持等待线程池空闲、查询排队任务数和活跃任务数。
- 支持 `StopSource` / `StopToken` 协作式取消。
- 在 C++20 下支持 `co_await pool.schedule()`，让协程恢复到线程池 worker 执行。
- 提供单元测试、协程测试、压力测试和 benchmark。

## 项目结构

```text
.
├── CMakeLists.txt        # CMake 构建入口
├── SafeQueue.h           # 线程安全任务队列
├── ThreadPool.h          # 线程池主体实现
├── main.cpp              # 基础示例
├── test.cpp              # C++14 行为测试
├── test_coroutine.cpp    # C++20 协程调度测试
├── stress_test.cpp       # 压力测试
├── benchmark.cpp         # 基准测试
└── docs/
    └── DESIGN.md         # 详细设计说明
```

## 快速开始

推荐使用 CMake：

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest 默认运行：

- `threadpool_tests`
- `threadpool_coroutine_tests`
- `threadpool_stress`

benchmark 不加入默认 CTest，因为性能结果容易受机器配置、系统负载和编译器影响。

## 基本用法

```cpp
#include "ThreadPool.h"

#include <iostream>

int main()
{
	threadpool::ThreadPool pool(4);

	auto result = pool.submit([] {
		return 42;
	});

	std::cout << result.get() << std::endl;
	return 0;
}
```

`submit()` 返回 `std::future<T>`：

- 任务完成后，`future.get()` 返回结果。
- 任务抛异常时，异常会在 `future.get()` 中重新抛出。
- 任务没有返回值时，返回 `std::future<void>`。

## 任务提交

普通函数：

```cpp
int add(int a, int b)
{
	return a + b;
}

auto result = pool.submit(add, 1, 2);
```

lambda：

```cpp
auto result = pool.submit([](int a, int b) {
	return a * b;
}, 5, 6);
```

引用参数需要使用 `std::ref`：

```cpp
void write_result(int &out)
{
	out = 100;
}

int value = 0;
auto done = pool.submit(write_result, std::ref(value));
done.get();
```

成员函数：

```cpp
class Calculator {
public:
	int multiply(int a, int b)
	{
		return a * b;
	}
};

Calculator calc;
auto result = pool.submit(&Calculator::multiply, &calc, 3, 4);
```

## 关闭策略

默认关闭策略会等待已提交任务执行完成：

```cpp
pool.shutdown();
```

如果希望丢弃尚未开始执行的排队任务：

```cpp
pool.shutdown(threadpool::ThreadPool::ShutdownMode::CancelPending);
```

两种模式的区别：

- `Drain`：停止接收新任务，但执行完已提交任务。
- `CancelPending`：停止接收新任务，并丢弃尚未被 worker 取走的排队任务；这些任务对应的 `future.get()` 会抛出 `std::future_error`。

析构函数会自动调用关闭流程，避免忘记释放 worker 线程。

## 状态观测

```cpp
pool.wait_idle();
pool.wait_idle_for(std::chrono::seconds(1));
pool.wait_idle_until(deadline);

pool.worker_count();
pool.queued_tasks();
pool.active_tasks();
pool.is_shutdown();
```

这些接口主要用于测试、批处理阶段同步和退出前收尾。

## 协作式取消

线程池不能安全地强制终止正在运行的 C++ 线程。项目使用协作式取消：由任务主动检查取消信号并安全退出。

```cpp
threadpool::ThreadPool::StopSource stop_source;

auto result = pool.submit_with_stop(
	stop_source.token(),
	[](threadpool::ThreadPool::StopToken token) {
		while (!token.stop_requested()) {
			do_some_work_chunk();
		}
		return "stopped";
	}
);

stop_source.request_stop();
```

`CancelPending` 负责取消还没开始执行的任务；`StopToken` 负责通知已经开始执行的任务自行退出。

## 协程调度

在支持 C++20 coroutine 的编译器下，可以使用：

```cpp
co_await pool.schedule();
```

含义是：当前协程挂起，并把协程恢复动作提交到线程池；worker 线程执行恢复动作后，协程从 `co_await` 后继续运行。

```cpp
CoroutineTask work(threadpool::ThreadPool &pool)
{
	co_await pool.schedule();

	// 从这里开始运行在线程池 worker 线程中
	do_heavy_work();
}
```

当前协程支持是轻量调度入口，不是完整的 `Task<T>` 协程框架。

## 压力测试

CMake 会构建压力测试目标，也会通过 CTest 运行一组轻量压力测试。

手动运行：

```powershell
.\threadpool_stress.exe <tasks> <workers> <submitters>
```

示例：

```powershell
.\threadpool_stress.exe 50000 8 8
```

覆盖场景：

- 大量任务提交和结果校验。
- 多提交线程并发提交。
- 排队任务取消。
- 协作式取消。

## Benchmark

benchmark 用于观察不同 worker 数下的吞吐和简单调度延迟，不作为功能正确性测试。

运行：

```powershell
.\threadpool_benchmark.exe <tasks> <submitters>
```

示例：

```powershell
.\threadpool_benchmark.exe 100000 4
```

输出指标：

- `tasks/sec`：每秒完成任务数。
- `p50_us` / `p95_us` / `p99_us`：任务从提交到开始执行的延迟采样。

对极短任务来说，worker 数量增加不一定提升吞吐，因为共享队列锁和线程调度开销可能超过并行收益。

## 手动编译

不使用 CMake 时，也可以直接用 g++：

```powershell
g++ -std=gnu++14 -Wall -Wextra -Wpedantic -pthread main.cpp -o threadpool_check.exe
g++ -std=gnu++14 -Wall -Wextra -Wpedantic -pthread test.cpp -o threadpool_tests.exe
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -pthread test_coroutine.cpp -o threadpool_coroutine_tests.exe
g++ -std=gnu++14 -Wall -Wextra -Wpedantic -pthread stress_test.cpp -o threadpool_stress.exe
g++ -std=gnu++14 -Wall -Wextra -Wpedantic -pthread benchmark.cpp -o threadpool_benchmark.exe
```

## 适用场景

适合：

- 学习 C++ 线程池、条件变量、future 和 coroutine 调度。
- 小型工具中的后台任务执行。
- CPU 密集型批处理。
- 需要异步返回值的简单并行任务。
- 验证线程池调度、取消、关闭和压力测试策略。

不适合：

- 高实时性任务调度。
- 复杂优先级调度。
- 极高吞吐、低延迟服务的核心路径。
- 需要强制终止任务的场景。
- 完整异步 runtime 或事件循环替代品。

## 当前边界

项目仍保留一些明确边界：

- 没有任务优先级。
- 没有动态扩缩容。
- 没有 work stealing。
- 没有单个任务级超时取消。
- 协程支持还不是完整 `Task<T>` 模型。
- benchmark 仍是基础版本，缺少跨平台 CI、长期 soak test 和统计置信区间。
