# Examples

常见使用模式和代码示例合集。

## 基础用法

### Hello World

```cpp
#include "ThreadPool.h"
#include <iostream>

int main() {
    ThreadPool pool(2);  // 2 个 worker

    auto f = pool.submit([] {
        return 42;
    });

    std::cout << f.get() << '\n';  // 42

    pool.shutdown();
}
```

### 多任务提交 + 收集结果

```cpp
#include "ThreadPool.h"
#include <vector>
#include <iostream>

int main() {
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;

    // 提交 100 个计算任务
    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([i] {
            return i * i;
        }));
    }

    // 收集结果
    int sum = 0;
    for (auto& f : futures) {
        sum += f.get();
    }

    std::cout << "Sum of squares: " << sum << '\n';
    pool.shutdown();
}
```

### 传递引用参数

```cpp
#include "ThreadPool.h"
#include <iostream>

struct Result {
    int value = 0;
};

int main() {
    ThreadPool pool(4);

    Result r1, r2;

    // 用 std::ref 传递引用
    auto f1 = pool.submit([](Result& r) {
        r.value = 100;
    }, std::ref(r1));

    // 或用 lambda 捕获
    auto f2 = pool.submit([&r2] {
        r2.value = 200;
    });

    f1.get();
    f2.get();

    std::cout << r1.value << ", " << r2.value << '\n';  // 100, 200
    pool.shutdown();
}
```

---

## 关闭模式

### Drain — 等待完成

```cpp
ThreadPool pool(2);

for (int i = 0; i < 10; ++i) {
    pool.submit([i] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << i << ' ';
    });
}

pool.shutdown(ThreadPool::ShutdownMode::Drain);
// 输出：所有 0-9 的数字都被打印
```

### CancelPending — 快速关闭

```cpp
ThreadPool pool(1);

// 提交长时间任务占用 worker
auto blocking = pool.submit([] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
});

// 提交大量任务排队
std::vector<std::future<int>> futures;
for (int i = 0; i < 100; ++i) {
    futures.push_back(pool.submit([i] { return i; }));
}

// 立即关闭，取消排队任务
pool.shutdown(ThreadPool::ShutdownMode::CancelPending);

// 被取消的 future 会抛出 future_error
int cancelled = 0;
for (auto& f : futures) {
    try {
        f.get();
    } catch (const std::future_error&) {
        ++cancelled;
    }
}
std::cout << "Cancelled: " << cancelled << '\n';  // 大部分被取消
```

---

## 协作取消

### 基本用法

```cpp
ThreadPool pool(4);

ThreadPool::StopSource stop_source;
auto token = stop_source.token();

auto future = pool.submit_with_stop(token, [](ThreadPool::StopToken st) {
    int count = 0;
    while (!st.stop_requested()) {
        // 做一些可中断的工作
        do_unit_of_work();
        ++count;
    }
    return count;
});

// 运行一段时间后请求停止
std::this_thread::sleep_for(std::chrono::milliseconds(100));
stop_source.request_stop();

int units_done = future.get();
std::cout << "Completed " << units_done << " units before stop\n";
```

### 多个任务共享一个 StopSource

```cpp
ThreadPool pool(4);
ThreadPool::StopSource stop_source;

std::vector<std::future<int>> futures;

for (int i = 0; i < 10; ++i) {
    futures.push_back(
        pool.submit_with_stop(stop_source.token(),
            [i](ThreadPool::StopToken st) -> int {
                int sum = 0;
                for (int j = 0; j < 1000000 && !st.stop_requested(); ++j) {
                    sum += j;
                }
                return sum;
            })
    );
}

// 请求所有任务停止
stop_source.request_stop();

// 收集部分结果
for (auto& f : futures) {
    std::cout << f.get() << '\n';
}
```

---

## 协程调度（C++20）

### 基础调度

```cpp
#include "ThreadPool.h"
#include <coroutine>
#include <iostream>

// 用户侧协程任务类型
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

Task demo(ThreadPool& pool) {
    std::cout << "Before: " << std::this_thread::get_id() << '\n';

    co_await pool.schedule();  // 切换到线程池

    std::cout << "After:  " << std::this_thread::get_id() << '\n';
}

int main() {
    ThreadPool::Options opts;
    opts.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
    ThreadPool pool(opts);

    demo(pool);

    pool.wait_idle();
    pool.shutdown();
}
```

### Yield 交错执行

```cpp
Task interleaved(ThreadPool& pool, int id) {
    co_await pool.schedule();

    for (int i = 0; i < 5; ++i) {
        std::cout << id << ":" << i << ' ';
        co_await pool.yield();  // 让出给其他协程
    }
}

// 两个协程交错输出：
// 0:0 1:0 0:1 1:1 0:2 1:2 ...
```

### 验证顺序（同 test_coroutine）

```cpp
std::vector<int> log;
std::mutex mtx;

auto record = [&](int v) {
    std::lock_guard lock(mtx);
    log.push_back(v);
};

Task ordered(ThreadPool& pool, int id) {
    record(id * 10);
    co_await pool.schedule();
    record(id * 10 + 1);
    co_await pool.yield();
    record(id * 10 + 2);
}

// 启动两个协程后 wait_idle()
// expected log: [10, 20, 11, 21, 12, 22]
```

---

## 动态扩缩容

```cpp
ThreadPool::Options opts;
opts.min_workers = 2;
opts.max_workers = 8;
opts.idle_timeout = std::chrono::milliseconds(500);  // 500ms 闲置缩容

ThreadPool pool(opts);

std::cout << "Initial: " << pool.worker_count() << '\n';  // 2

// 大量任务触发扩容
for (int i = 0; i < 1000; ++i) {
    pool.submit([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
}

// 等待扩容生效
std::this_thread::sleep_for(std::chrono::milliseconds(100));
std::cout << "Under load: " << pool.worker_count() << '\n';  // > 2

// 等待任务完成 + 闲置缩容
pool.wait_idle();
std::this_thread::sleep_for(std::chrono::seconds(1));
std::cout << "Idle: " << pool.worker_count() << '\n';  // 回到 2

pool.shutdown();
```

---

## 状态监控

```cpp
ThreadPool pool(4);

// 监控线程
std::atomic<bool> stop_monitor{false};

std::thread monitor([&] {
    while (!stop_monitor) {
        std::cout << "queued: " << pool.queued_tasks()
                  << " | active: " << pool.active_tasks()
                  << " | workers: " << pool.worker_count()
                  << " | coroutines: " << pool.queued_coroutines()
                  << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
});

// 提交任务...
for (int i = 0; i < 100; ++i) {
    pool.submit([i] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
}

pool.wait_idle();
stop_monitor = true;
monitor.join();
pool.shutdown();
```

---

## wait_idle 模式

### 等待完成 + 继续提交

```cpp
ThreadPool pool(4);

// 提交第一波任务
for (int i = 0; i < 50; ++i) {
    pool.submit([i] { process_batch1(i); });
}
pool.wait_idle();  // 等待第一波完成

// 提交第二波（依赖第一波的结果）
for (int i = 0; i < 50; ++i) {
    pool.submit([i] { process_batch2(i); });
}
pool.wait_idle();

pool.shutdown();
```

### 带超时的等待

```cpp
ThreadPool pool(4);
pool.submit([] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
});

// 最多等 2 秒
bool done = pool.wait_idle_for(std::chrono::seconds(2));
if (!done) {
    std::cout << "Still working... cancelling\n";
    pool.shutdown(ThreadPool::ShutdownMode::CancelPending);
} else {
    pool.shutdown();
}
```

---

## 错误处理

```cpp
ThreadPool pool(2);

// submit 抛出的异常通过 future 传播
auto f = pool.submit([] {
    throw std::runtime_error("task failed");
});

try {
    f.get();
} catch (const std::runtime_error& e) {
    std::cout << "Caught: " << e.what() << '\n';
}

// 关闭后提交抛出异常
pool.shutdown();

try {
    pool.submit([]{});  // throws
} catch (const std::runtime_error& e) {
    std::cout << "Expected: " << e.what() << '\n';
}
```
