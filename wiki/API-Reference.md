# API Reference

完整的公开 API 参考。所有类型均位于 `threadpool` 命名空间。

## 目录

- [ThreadPool 类](#threadpool-类)
- [枚举类型](#枚举类型)
- [配置结构体](#配置结构体)
- [协作取消](#协作取消)
- [ScheduleAwaiter（C++20）](#scheduleawaiter-c20)
- [YieldAwaiter（C++20）](#yieldawaiter-c20)

---

## ThreadPool 类

**头文件**：`ThreadPool.h`
**命名空间别名**：`::ThreadPool` = `threadpool::ThreadPool`

### 构造函数

```cpp
// 固定大小池，ThreadOnly 模式
explicit ThreadPool(size_t worker_count = 4);

// 固定大小池，指定执行模式
ThreadPool(size_t worker_count, ExecutionMode execution_mode);

// 完整配置（支持动态扩缩）
explicit ThreadPool(const Options& options);
```

- `worker_count` 为 0 时抛出 `std::invalid_argument`
- `max_workers < min_workers` 时抛出 `std::invalid_argument`
- 构造函数**自动启动** worker 线程

### 析构函数

```cpp
~ThreadPool() noexcept;
```

- 内部调用 `shutdown()`，捕获所有异常
- 保证 `noexcept`

### 静态工具方法

```cpp
static Options make_fixed_options(size_t worker_count, ExecutionMode execution_mode);
```
快捷构造固定大小的 `Options`。

### 生命周期方法

```cpp
// 启动（构造时已启动，通常不需手动调用）
void start(size_t worker_count);
```

`start()` 是幂等的：如果已启动且 worker 数量和配置一致，则无操作。

```cpp
// 关闭线程池
void shutdown(ShutdownMode mode = ShutdownMode::Drain);
```

| 模式 | 行为 |
|------|------|
| `Drain` | 等待所有已入队任务执行完毕 |
| `CancelPending` | 丢弃队列中尚未开始的任务，对应的 future 抛出 `std::future_error` |

```cpp
// 等待所有任务完成（阻塞）
void wait_idle();

// 带超时的等待
template<typename Rep, typename Period>
bool wait_idle_for(const std::chrono::duration<Rep, Period>& timeout);

// 带截止时间的等待
template<typename Clock, typename Duration>
bool wait_idle_until(const std::chrono::time_point<Clock, Duration>& deadline);
```

> `wait_idle` 会阻塞直到 `queued_tasks() == 0 && queued_coroutines() == 0 && active_tasks() == 0`

### 查询方法

```cpp
bool is_shutdown() const;                 // 是否已关闭
size_t worker_count() const noexcept;     // 当前 worker 数量
size_t min_workers() const noexcept;      // 最小 worker 数
size_t max_workers() const noexcept;      // 最大 worker 数
size_t queued_tasks() const;              // 全局队列中等待的任务数
size_t queued_coroutines() const;         // 协程队列中等待的协程数
size_t active_tasks() const;              // 正在执行的任务/协程数
ExecutionMode execution_mode() const noexcept;  // 当前执行模式
bool is_coroutine_enabled() const noexcept;     // 是否启用协程
```

### 任务提交

```cpp
// 提交可调用对象，返回 std::future
template<typename F, typename... Args>
auto submit(F&& f, Args&&... args)
    -> std::future<detail::invoke_result_t<F, Args...>>;
```

- `F` 可以是函数指针、lambda、`std::function`、成员函数指针等
- 返回 `std::future<R>`，`R` 为调用 `F(args...)` 的返回类型
- 关闭后提交抛出 `std::runtime_error`

```cpp
// 带 StopToken 的任务提交
template<typename F, typename... Args>
auto submit_with_stop(StopToken token, F&& f, Args&&... args)
    -> std::future<detail::invoke_result_t<F, StopToken, Args...>>;
```

- `StopToken` 会作为**第一个参数**传入 `F`
- 函数签名等效于 `F(stop_token, args...)`

### 协程方法（仅 C++20）

```cpp
ScheduleAwaiter schedule();   // 调度协程到线程池
YieldAwaiter yield();         // 让出当前 worker
```

- 未启用 `ThreadAndCoroutine` 模式时调用会抛出 `std::runtime_error`

### 类型别名

```cpp
using ShutdownMode   = threadpool::ShutdownMode;
using ExecutionMode  = threadpool::ExecutionMode;
using Options        = threadpool::ThreadPoolOptions;
using StopToken      = threadpool::StopToken;
using StopSource     = threadpool::StopSource;
```

---

## 枚举类型

### ShutdownMode

```cpp
enum class ShutdownMode {
    Drain,          // 等待所有已入队任务执行完毕
    CancelPending   // 丢弃未开始的任务
};
```

### ExecutionMode

```cpp
enum class ExecutionMode {
    ThreadOnly,         // 仅线程执行（默认，C++14）
    ThreadAndCoroutine  // 线程 + 协程混合（需 C++20）
};
```

| 模式 | 可用方法 | C++ 版本 |
|------|---------|---------|
| `ThreadOnly` | `submit()`, `submit_with_stop()` | C++14+ |
| `ThreadAndCoroutine` | 上述 + `schedule()`, `yield()` | C++20+ |

---

## 配置结构体

### ThreadPoolOptions

```cpp
struct ThreadPoolOptions {
    size_t min_workers           = 4;
    size_t max_workers           = 4;
    ExecutionMode execution_mode = ExecutionMode::ThreadOnly;
    size_t coroutine_burst_limit = 8;
    std::chrono::milliseconds idle_timeout{30000};
};
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `min_workers` | 4 | 常驻 worker 下限（必须 > 0） |
| `max_workers` | 4 | worker 上限（必须 >= min_workers） |
| `execution_mode` | `ThreadOnly` | 执行模式 |
| `coroutine_burst_limit` | 8 | 协程连续执行上限（防饥饿） |
| `idle_timeout` | 30s | 闲置超时后缩容到 min_workers |

- `min_workers == max_workers` 时，池大小固定（不会扩缩）
- `min_workers < max_workers` 时，启用动态扩缩

---

## 协作取消

### StopToken

```cpp
class StopToken {
public:
    bool stop_requested() const noexcept;
};
```

- 轻量级、可拷贝
- 内部共享 `std::shared_ptr<std::atomic<bool>>`
- 使用 acquire-release 内存序保证跨线程可见性

### StopSource

```cpp
class StopSource {
public:
    StopToken token() const;
    void request_stop() const noexcept;
};
```

- `token()` 返回关联的 `StopToken`
- `request_stop()` 设置停止标志，所有持有 token 的线程可见

**典型用法**：

```cpp
StopSource stop_source;
auto future = pool.submit_with_stop(stop_source.token(), [](StopToken st) {
    while (!st.stop_requested()) {
        do_work();
    }
});
stop_source.request_stop();  // 通知任务停止
```

---

## ScheduleAwaiter（C++20）

通过 `pool.schedule()` 返回，将当前协程调度到线程池执行。

```cpp
class ScheduleAwaiter {
public:
    bool await_ready() const noexcept;   // 始终返回 false
    void await_suspend(std::coroutine_handle<> handle);  // 入队协程
    void await_resume() const noexcept;  // 无操作
};
```

**调度策略**：轮询（round-robin）分配到 worker 的协程队列。

**用法**：

```cpp
co_await pool.schedule();  // 协程被挂起并调度到线程池
// 在线程池 worker 上恢复执行
```

## YieldAwaiter（C++20）

通过 `pool.yield()` 返回，让出当前 worker 给其他协程/任务。

```cpp
class YieldAwaiter {
public:
    bool await_ready() const noexcept;   // 始终返回 false
    void await_suspend(std::coroutine_handle<> handle);  // 入队当前 worker 协程队列
    void await_resume() const noexcept;  // 无操作
};
```

**调度策略**：亲缘性（affinity）—— 优先放回当前 worker 的协程队列。

**与 `schedule()` 的区别**：

| | `schedule()` | `yield()` |
|---|---|---|
| 目标选择 | 轮询（round-robin） | 当前 worker（affinity） |
| 用途 | 首次将协程调度到线程池 | 协程内部让出执行权 |
| 通知方式 | `notify_all()` | `notify_all()` |

**用法**：

```cpp
co_await pool.schedule();  // 调度到线程池
// 做一些工作...
co_await pool.yield();     // 让出，稍后在同 worker 恢复
// 继续工作...
```
