# Lifecycle

ThreadPool 的完整生命周期，从构造到销毁。

## 生命周期状态机

```
                 构造函数
                     │
                     ▼
              ┌─────────────┐
              │  Running     │  ← worker 线程运行中
              │             │  ← 可接受 submit()
              └──┬───┬─────┘
                 │   │
    shutdown(Drain)  │  shutdown(CancelPending)
                 │   │
        ┌────────▼───▼────────┐
        │   ShuttingDown      │
        │                     │
        │  Drain:             │  CancelPending:
        │  - 等待所有任务完成  │  - 清空队列
        │  - 不接受新提交     │  - 不接受新提交
        │                     │
        └─────────┬───────────┘
                  │
         所有 worker 退出
                  │
                  ▼
         ┌──────────────┐
         │   Stopped     │  ← 析构时可安全销毁
         └──────────────┘
```

---

## 构造函数

```cpp
// 三步初始化
ThreadPool::ThreadPool(const Options& options) {
    // 1. 验证参数
    if (options.min_workers == 0)
        throw std::invalid_argument("min_workers must be > 0");
    if (options.max_workers < options.min_workers)
        throw std::invalid_argument("max_workers must be >= min_workers");

    // 2. 初始化 runtime（传入 options）
    // runtime_ 持有 TaskScheduler + WorkerGroup

    // 3. 启动 worker
    start(options.min_workers);
}
```

**构造后状态**：`Running`，worker 线程已启动，可接受 `submit()`。

---

## start()

```cpp
void ThreadPool::start(size_t worker_count) {
    runtime_.start(worker_count);
}

void ThreadPoolRuntime::start(size_t worker_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;  // 幂等：已启动则忽略
    started_ = true;

    // spawn 初始 worker（在锁外？通常在锁内 reserve）
    // WorkerGroup::reserve(worker_count) → spawn n 个 thread
}
```

**幂等性**：重复调用 `start()` 无副作用。构造时已调用，用户通常不需要手动调用。

---

## shutdown() — 两种模式

### Drain（默认）

```cpp
pool.shutdown(ShutdownMode::Drain);
```

**行为**：

1. 设置 `shutdown_ = true`，不再接受新提交
2. 通知所有 worker 唤醒
3. 每个 worker：
   - 如果队列有任务 → 继续执行直到队列空
   - 如果没有任务且没有活跃任务 → 退出
4. 所有 worker 退出后 → 关闭完成

```
shutdown(Drain)
    │
    ├─ shutdown_ = true
    ├─ notify_all()
    │
    └─ 每个 worker:
         while (true):
           if shutdown_ && no_work():
             exit
           else:
             pop → execute → finish
```

**适用场景**：需要等待所有已提交任务完成。

### CancelPending

```cpp
pool.shutdown(ShutdownMode::CancelPending);
```

**行为**：

1. 设置 `shutdown_ = true`
2. 清空所有队列中的任务（未开始的任务）
3. 那些被清空的任务对应的 `future` 会抛出 `std::future_error`（broken promise）
4. 正在执行的任务不受影响，执行完毕
5. 已经调度（已入协程队列）的协程**不会被取消**

```
shutdown(CancelPending)
    │
    ├─ shutdown_ = true
    ├─ scheduler_.cancel_pending_tasks()
    │       │
    │       ├─ global_queue.clear()     ← 清空普通任务
    │       └─ 协程队列不清空            ← 已调度的协程保留
    │
    ├─ notify_all()
    │
    └─ 每个 worker:
         正在执行的任务 → 完成
         队列已空 → 检查 shutdown → exit
```

**适用场景**：需要快速关闭，不关心未开始任务的结果。

| | Drain | CancelPending |
|---|---|---|
| 等待已入队任务 | 是 | 否 |
| 已入队任务的 future | 正常返回值 | `future_error` (broken promise) |
| 正在执行的任务 | 执行完毕 | 执行完毕 |
| 已调度的协程 | 执行完毕 | 执行完毕（不取消） |
| 关闭速度 | 取决于队列长度 | 更快 |

---

## 空闲等待

```cpp
pool.wait_idle();                       // 阻塞直到全部完成
pool.wait_idle_for(5s);                 // 最多等 5 秒
pool.wait_idle_until(deadline);         // 等到截止时间
```

**idle 定义**：

```
queued_tasks() == 0 && queued_coroutines() == 0 && active_tasks() == 0
```

- `queued` 系列：队列中等待的任务/协程
- `active_tasks()`：正在 worker 上执行的任务+协程

**实现**：通过 `idle_cv_` 条件变量，`notify_idle_if_needed()` 在每个任务完成时通知。

---

## 析构函数

```cpp
ThreadPool::~ThreadPool() noexcept {
    try {
        shutdown();
    } catch (...) {
        // 吞掉所有异常，保证 noexcept
    }
}
```

**保证**：
- 总是尝试关闭线程池
- 即使 shutdown 抛异常也不会让析构函数抛异常（`noexcept`）
- 如果没有手动调用 `shutdown()`，析构时自动执行

> **最佳实践**：显式调用 `shutdown()` 而不是依赖析构，这样可以处理异常。

---

## 动态扩缩容

### 扩容（Grow）

触发条件：`queued_tasks() > live_workers() && live_workers() < max_workers()`

- 在 `submit_task()` 后检查
- 每次只 spawn 一个 worker（渐进式）
- 新 worker 立即开始工作

### 缩容（Shrink）

触发条件：worker 在 `work_cv_.wait_for()` 中超时且 `live_workers() > min_workers()`

- Worker 在 `wait_for_work()` 中检查超时
- 如果 `should_exit_on_idle()` 返回 true → worker 退休
- 退休的 worker 被 `reap_retired_workers()` join 并移除

```
Worker 空闲超时 → retire_current_worker()
    │
    ├─ live_workers_-- (原子)
    ├─ 把自己加入 retired_worker_ids_
    └─ 线程退出

下次 spawn/maybe_grow 时：
    └─ reap_retired_workers()
         ├─ lock(mutex_)
         ├─ join 退休线程
         └─ 从 workers_ 和 retired_worker_ids_ 移除
```

---

## 完整生命周期示例

```cpp
void lifecycle_demo() {
    // 1. 构造 → Running
    ThreadPool::Options opts;
    opts.min_workers = 2;
    opts.max_workers = 8;
    opts.idle_timeout = std::chrono::milliseconds(5000);
    ThreadPool pool(opts);

    // 2. 提交任务
    for (int i = 0; i < 100; ++i) {
        pool.submit([i] { process(i); });
    }

    // 3. 等待完成（可选）
    pool.wait_idle();
    std::cout << "All tasks done\n";

    // 4. 关闭 → ShuttingDown → Stopped
    pool.shutdown(ShutdownMode::Drain);

    // 5. 提交被拒绝
    try {
        pool.submit([]{});  // throws std::runtime_error
    } catch (const std::runtime_error& e) {
        std::cout << "Expected: " << e.what() << '\n';
    }
}  // 6. 析构 → 再次 shutdown（幂等，无操作）
```
