# ThreadPool

header-only C++ 线程池库。C++14 基线，C++20 协程条件启用，跨 Windows / Linux（GCC + MSVC）。

## 能干什么

### 1. 提交任务，拿到 future 返回值

```cpp
#include "ThreadPool.h"
#include <iostream>

int main() {
    ThreadPool pool(4);                            // 4 个 worker 线程

    auto f = pool.submit([]{ return 42; });        // 提交任务
    std::cout << f.get() << std::endl;             // 拿到返回值 42

    // pool 析构自动 shutdown
}
```

### 2. 支持各种任务形式

```cpp
// 普通函数
auto f1 = pool.submit(add, 1, 2);

// lambda
auto f2 = pool.submit([](int x) { return x * 2; }, 21);

// 成员函数
Calculator calc;
auto f3 = pool.submit(&Calculator::multiply, &calc, 3, 4);

// 引用参数用 std::ref
int value = 0;
pool.submit([](int &out) { out = 100; }, std::ref(value)).get();
```

### 3. 关闭方式

```cpp
// Drain：等所有已提交任务跑完（默认）
pool.shutdown();

// 立即丢弃排队任务，正在跑的跑完
pool.shutdown(ThreadPool::ShutdownMode::CancelPending);

// 带超时：避免任务死锁导致进程永远退不出
if (!pool.shutdown_for(std::chrono::seconds(5))) {
    // false = 超时，剩余 worker 已 detach，可安全退出进程
    // 注意：detach 的 worker 中正在跑的任务会继续执行到完成
}
```

析构函数自动调用 `shutdown()`，不怕忘。

> **超时关闭的语义**：`shutdown_for(timeout)` 返回 `false` 表示有 worker 未能在超时内退出。该 worker 的线程会被 detach，其正在执行的任务将继续运行直到完成。已排队的任务如果模式是 `CancelPending` 会丢弃，`Drain` 则尽可能执行但可能在超时后被跳过。

### 4. 查看线程池状态

```cpp
pool.wait_idle();                    // 阻塞等到所有任务完成
pool.queued_tasks();                 // 还在排队的任务数
pool.active_tasks();                 // 正在执行的任务数
pool.queued_coroutines();            // 等待恢复的协程数
pool.worker_count();                 // 存活 worker 数
pool.max_coroutine_queue_size();     // 协程队列上限（0=无限制）
pool.is_shutdown();                  // 是否已关闭
```

### 5. 协作式取消

```cpp
ThreadPool::StopSource source;

auto future = pool.submit_with_stop(source.token(), [](ThreadPool::StopToken token) {
    while (!token.stop_requested()) {
        do_work_chunk();             // 小粒度工作单元，每次循环检查取消
    }
    return "stopped";
});

source.request_stop();               // 通知所有持有此 token 的任务
future.get();                        // → "stopped"
```

### 6. 协程队列容量限制

```cpp
ThreadPool::Options options;
options.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
options.max_coroutine_queue_size = 256;  // 每 worker 协程队列上限

ThreadPool pool(options);
// 队列满时，新的 co_await pool.schedule() 抛出 std::runtime_error
```

### 7. 任务异常回调

```cpp
ThreadPool::Options options;
options.on_exception = [](const std::exception_ptr &ep) {
    try { std::rethrow_exception(ep); }
    catch (const std::exception &e) {
        log_error("worker task failed: {}", e.what());
    }
};
ThreadPool pool(options);
// 非 packaged_task 任务的异常不再静默丢失
```

### 8. C++20 协程调度

```cpp
// 构造时显式启用
ThreadPool pool(4, ThreadPool::ExecutionMode::ThreadAndCoroutine);

// 把协程切换到线程池 worker 上执行
co_await pool.schedule();

// 主动让出 worker，稍后恢复
co_await pool.yield();
```

完整协程示例：

```cpp
CoroutineTask work(ThreadPool &pool, std::vector<int> &steps, int id) {
    co_await pool.schedule();        // 切换到 worker
    steps.push_back(id * 10);
    co_await pool.yield();           // 让出
    steps.push_back(id * 10 + 1);
    co_await pool.yield();           // 再让出
    steps.push_back(id * 10 + 2);
}
// 一个 worker 线程可以在多个协程之间协作式轮转
```

### 9. 动态扩缩容

```cpp
ThreadPool::Options options;
options.min_workers = 2;   // 最少保留 2 个 worker
options.max_workers = 8;   // 最多扩容到 8 个
options.idle_timeout = std::chrono::seconds(10);  // 空闲 10s 后缩容

ThreadPool pool(options);
// 排队任务 > 存活 worker → 自动扩容
// worker 空闲超时 → 自动缩容（不低于 min_workers）
```

---

## 不包括什么

- 任务优先级
- 抢占式任务终止
- 完整的 `Task<T>` 协程框架
- 定时器和 I/O 事件

---

## 安装

### CMake install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /usr/local
```

```cmake
# 项目中使用
find_package(threadpool REQUIRED)
target_link_libraries(my_app PRIVATE threadpool)
```

### vcpkg

```bash
vcpkg install threadpool
```

### 手动

复制 `include/threadpool/` 到项目中，`#include "threadpool/ThreadPool.h"`。

---

## 构建 & 运行

### CMake（推荐）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

CTest 默认跑 3 项：行为测试 + 协程测试 + 轻量压力测试。

### 手动编译

```bash
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -pthread test.cpp -o threadpool_tests
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -pthread test_coroutine.cpp -o threadpool_coroutine_tests
```

### 压力测试 & Benchmark

```bash
./threadpool_stress.exe 50000 8 8        # <tasks> <workers> <submitters>
./threadpool_benchmark.exe 100000 4      # <tasks> <submitters>
./threadpool_mode_compare.exe 20000 2000 10 4  # <tasks> <coroutines> <yields> <workers>
```

---

## 文档

| 文档 | 内容 |
|------|------|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 代码架构、组件职责、数据流、锁层次、文件组织 |
| [DESIGN.md](docs/DESIGN.md) | 设计决策、调度策略、trade-off、取舍分析 |
| [IMPLEMENTATION_PRINCIPLE.md](docs/IMPLEMENTATION_PRINCIPLE.md) | 逐层实现原理、源码追踪 |
| [INTERVIEW_QUESTIONS.md](docs/INTERVIEW_QUESTIONS.md) | 项目面试问题与回答要点 |
| [DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md) | 29 个开发问题的原因分析和解决策略 |
