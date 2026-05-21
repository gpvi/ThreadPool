# Coroutine Scheduling

C++20 协程调度支持，通过 `co_await pool.schedule()` 和 `co_await pool.yield()` 实现。

> **前提**：需要 C++20 编译器支持，且在构造时启用 `ExecutionMode::ThreadAndCoroutine`。

## C++20 协程基础

C++20 协程是**无栈协程**（stackless coroutine），编译器将协程体转换为状态机：

- `co_await` → 挂起点，编译器生成 `await_ready()` / `await_suspend()` / `await_resume()` 调用
- `co_return` → 返回，设置 promise 值
- `std::coroutine_handle<>` → 协程句柄，可调用 `handle.resume()` 恢复执行

ThreadPool 不管理协程帧的生命周期——由用户侧的 `CoroutineTask` 管理（参见 test_coroutine.cpp 中的示例）。

---

## 架构

```
用户协程                     ThreadPool              ThreadPoolRuntime        TaskScheduler
  │                            │                        │                       │
  ├─co_await pool.schedule()─►│                        │                       │
  │                            ├─ScheduleAwaiter        │                       │
  │                            │  ├─await_ready()=false │                       │
  │                            │  ├─await_suspend(h)───►│                       │
  │                            │  │                      ├─enqueue_coroutine_   │
  │                            │  │                      │  resume(h, false)    │
  │                            │  │                      │  ├─检查 shutdown     │
  │                            │  │                      │  ├─检查 mode        │
  │                            │  │                      │  ├─轮询选 worker    │
  │                            │  │                      │  ├─push_coroutine───►│
  │                            │  │                      │  │                   ├─worker[w].queue
  │                            │  │                      │  │                   │  .push(h.resume)
  │                            │  │                      │  └─notify_all()     │
  │  (挂起)                    │  │                      │                       │
  │                            │  │                      │                       │
  │  ◄── worker 上恢复         │  │                      │                       │
  │  await_resume()            │  │                      │                       │
```

---

## ScheduleAwaiter

```cpp
class ScheduleAwaiter {
public:
    bool await_ready() const noexcept { return false; }
    // 始终挂起，不尝试同步完成

    void await_suspend(std::coroutine_handle<> handle) {
        // 将 handle.resume() 包装为 Task，入队到目标 worker 的协程队列
        pool_.enqueue_coroutine_resume(handle, /*yield=*/false);
    }

    void await_resume() const noexcept {}
};
```

**调度策略**：**轮询（round-robin）** — 将协程分配到下一个 worker 的协程队列。

- 适合：首次将协程调度到线程池
- 效果：负载均衡，协程均匀分布到各 worker

---

## YieldAwaiter

```cpp
class YieldAwaiter {
public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        pool_.enqueue_coroutine_resume(handle, /*yield=*/true);
    }

    void await_resume() const noexcept {}
};
```

**调度策略**：**亲缘性（affinity）** — 优先放回当前 worker 的协程队列。

- 适合：协程中途让出执行权
- 效果：利用 CPU 缓存热度，减少跨线程迁移开销

---

## enqueue_coroutine_resume() 详解

```cpp
void ThreadPoolRuntime::enqueue_coroutine_resume(
    std::coroutine_handle<> handle, bool yield) {

    // 1. 安全检查
    if (shutdown_) return;                       // 关闭中不调度
    if (execution_mode_ != ExecutionMode::ThreadAndCoroutine)
        throw std::runtime_error("...");          // 模式检查

    // 2. 选择目标 worker
    size_t target_worker;
    if (yield && current_worker_id != no_worker) {
        target_worker = current_worker_id;        // yield → 亲和
    } else {
        target_worker = scheduler_.next_coroutine_worker();  // schedule → 轮询
    }

    // 3. 包装 handle.resume() 为 function<void()>
    auto task = [handle]() mutable {
        handle.resume();  // 恢复协程执行
    };

    // 4. 入队到目标 worker 的协程队列
    scheduler_.push_coroutine(target_worker, std::move(task));

    // 5. 唤醒 worker（notify_all，因为需要特定 worker 响应）
    work_cv_.notify_all();
}
```

**为什么 schedule 用 `notify_all()`？**

- 协程被推入特定 worker 的队列
- 如果只 `notify_one()`，可能唤醒的不是目标 worker
- `notify_all()` 保证目标 worker 被唤醒（其他 worker 检查无工作后继续睡眠）

---

## push_coroutine() 内部

```cpp
void TaskScheduler::push_coroutine(size_t worker_id, Task task) {
    // 确保协程队列存在
    if (worker_id >= coroutine_queues_.size()) {
        coroutine_queues_.resize(worker_id + 1);
    }
    if (!coroutine_queues_[worker_id]) {
        coroutine_queues_[worker_id] = std::make_unique<SafeQueue<Task>>();
    }

    coroutine_queues_[worker_id]->push(std::move(task));
}
```

每个 worker 有一个独立的 `SafeQueue<Task>`。协程任务只在这些队列中，不在全局任务队列。

---

## 协程执行流程

```
Worker 主循环（pop_for_worker 4 级优先级）
    │
    ├─ 2. 自己的协程队列   ← yield() 的任务在这里
    ├─ 3. 全局任务队列     ← submit() 的任务在这里
    └─ 4. 窃取其他协程队列  ← schedule() 到其他 worker 的任务
         │
         ▼
    执行 task() → handle.resume()
         │
         ▼
    协程恢复执行...
         ├─ 做了一些工作
         ├─ co_await pool.yield()  ← 再次挂起
         │      │
         │      └─ 推入协程队列，等待恢复
         │
         └─ co_return value        ← 完成
                │
                └─ promise.set_value(value)
```

---

## 协程防饥饿机制（Burst Limit）

问题：如果协程队列一直有任务，普通任务可能永远得不到执行。

解决：**协程 burst 计数器**。

```cpp
// finish_work() 内部
void ThreadPoolRuntime::finish_work(bool popped_coroutine) {
    if (popped_coroutine) {
        worker_coroutine_burst_[worker_id]++;  // 递增
    } else {
        worker_coroutine_burst_[worker_id] = 0; // 重置
    }
}
```

`pop_for_worker()` 第一级检查：

```cpp
if (worker_coroutine_burst_[worker_id] >= coroutine_burst_limit_) {
    // 先看全局任务队列，给普通任务机会
    if (global_queue_.pop(task)) {
        return {task, /*popped_coroutine=*/false};
    }
}
```

默认 `burst_limit = 8`：连续执行 8 个协程后，必须处理一个普通任务（如果有的话）。

---

## 协程帧生命周期

```
用户创建协程 → CoroutineTask 对象持有 promise_type
    │
    ▼
co_await pool.schedule() → 挂起，handle 交给 ThreadPool
    │
    │  handle 存储在协程队列的 Task 中
    │
    ▼
worker 执行 handle.resume() → 协程恢复
    │
    ├─ 如果协程再次 co_await/yield → handle 重新入队
    │
    └─ 如果协程 co_return → promise.set_value() → 帧销毁
```

**重要**：ThreadPool 不拥有协程帧。用户必须确保 `CoroutineTask` 对象在协程执行期间存活。

---

## 完整示例

```cpp
#include "ThreadPool.h"
#include <coroutine>
#include <iostream>

// 用户侧协程包装（简化版）
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

Task coroutine_work(ThreadPool& pool, int id) {
    std::cout << id << " starts on thread " << std::this_thread::get_id() << '\n';

    co_await pool.schedule();  // 调度到线程池

    std::cout << id << " runs on thread " << std::this_thread::get_id() << '\n';

    for (int i = 0; i < 3; ++i) {
        std::cout << id << ": step " << i << '\n';
        co_await pool.yield();  // 让出给其他协程
    }
}

int main() {
    ThreadPool::Options opts;
    opts.min_workers = 2;
    opts.max_workers = 2;
    opts.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;

    ThreadPool pool(opts);

    coroutine_work(pool, 1);
    coroutine_work(pool, 2);

    pool.wait_idle();  // 等待所有协程完成
    pool.shutdown();
}
```
