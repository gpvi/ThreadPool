# Thread Safety

ThreadPool 的线程安全模型，包括锁层级、安全队列、条件变量使用和并发原语。

## 核心原则

**"谁拥有数据，谁管理锁"** — 每个组件只锁自己持有的数据。

- 不使用全局锁
- 不从内层锁向外层锁扩展（避免死锁）
- `notify` 操作始终在锁外（避免惊群效应）
- 优先用原子变量替代锁（减少 contention）

---

## 锁层级图

```
┌──────────────────────────────────────────────────┐
│              ThreadPoolRuntime::mutex_            │  ← 第 1 层
│   保护: shutdown_, started_, work_cv_             │
│                                                  │
│   ┌──────────────────────────────────────────┐   │
│   │          TaskScheduler::mutex_            │   │  ← 第 2a 层
│   │   保护: active_tasks_,                    │   │
│   │         next_coroutine_worker_,           │   │
│   │         idle_cv_                          │   │
│   │                                           │   │
│   │   ┌─────────────────────────────────┐     │   │
│   │   │     SafeQueue::m_mutex          │     │   │  ← 第 3 层
│   │   │   保护: std::queue<T>           │     │   │
│   │   └─────────────────────────────────┘     │   │
│   └──────────────────────────────────────────┘   │
│                                                  │
│   ┌──────────────────────────────────────────┐   │
│   │         WorkerGroup::mutex_              │   │  ← 第 2b 层
│   │   保护: workers_, retired_worker_ids_    │   │     (与 2a 并行)
│   └──────────────────────────────────────────┘   │
└──────────────────────────────────────────────────┘
```

**关键约束**：

| 规则 | 说明 |
|------|------|
| 不跨层反向加锁 | 持有 SafeQueue 锁时不许获取 TaskScheduler 锁 |
| 同层锁取序一致 | Runtime 锁 → Scheduler 锁（如果需要同时持有） |
| 锁外 notify | `notify_one/all` 永远在 `unlock()` 之后 |
| 无死锁路径 | 所有锁获取遵循固定顺序 |

---

## SafeQueue<T>

最底层的线程安全原语：

```cpp
template<typename T>
class SafeQueue {
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;

public:
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    void push(T item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(item));
    }

    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        item = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<T> empty;
        std::swap(m_queue, empty);
    }
};
```

**特点**：
- 每个操作独立加锁，不自带条件变量（由上层有条件变量）
- `pop` 返回 `bool` 表示是否取到（非阻塞）
- 不可拷贝、不可移动

---

## 条件变量模式

### worker 等待模式（predicate + wait_for）

```cpp
WorkerWork ThreadPoolRuntime::wait_for_work(size_t worker_id) {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        // 1. 检查是否需要退出
        if (shutdown_ && (mode == CancelPending || !has_any_work())) {
            return {.exit = true};
        }

        // 2. 带超时的等待
        bool has_work = work_cv_.wait_for(lock, workers_.idle_timeout(),
            [this, worker_id] {
                return shutdown_ || scheduler_.has_any_work_for_worker(worker_id);
            });

        // 3. 超时 → 退休（如果允许）
        if (!has_work && workers_.should_exit_on_idle()) {
            return {.exit = true};
        }

        // 4. 有工作 → pop
        if (scheduler_.pop_for_worker(worker_id, ...)) {
            return work;
        }
        // 否则重试（可能是虚假唤醒或工作被其他 worker 取走）
    }
}
```

**为什么用 `while(true)` 而非简单的 wait？**

1. 多个 worker 被 `notify_all()` 唤醒，但只有部分能取到工作
2. 取不到工作的 worker 需要重新等待
3. 这是标准的 condition variable 使用模式

### idle 等待模式（predicate + wait）

```cpp
void TaskScheduler::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] {
        return queued_tasks() == 0 &&
               queued_coroutines() == 0 &&
               active_tasks_ == 0;
    });
}
```

**is_idle 条件**：所有队列为空 **且** 没有正在执行的任务 — 三者缺一不可。

---

## 原子变量

### live_workers_

```cpp
std::atomic<size_t> live_workers_;
```

- **写入**：spawn 成功时 +1（WorkerGroup::spawn）
- **读取**：`worker_count()`、`can_spawn()`、`should_exit_on_idle()`
- **无锁**：所有读取路径无需获取 `WorkerGroup::mutex_`

### next_worker_id_

```cpp
std::atomic<size_t> next_worker_id_;
```

- **自增**：spawn 时 `fetch_add(1)` 分配唯一 ID
- **读取**：worker 获取自己的 ID

### StopToken 内部原子

```cpp
std::shared_ptr<std::atomic<bool>> stop_requested_;
```

- **写入**：`request_stop()` 用 `memory_order_release`
- **读取**：`stop_requested()` 用 `memory_order_acquire`
- acquire-release 保证 stop 标志的写入对其他线程可见

---

## thread_local 变量

```cpp
// ThreadPoolRuntime.h
thread_local ThreadPoolRuntime* current_runtime = nullptr;
thread_local size_t current_worker_id = static_cast<size_t>(-1);
```

**设置时机**：worker 线程入口

```cpp
// ThreadPoolWorker::operator()()
runtime_->set_current_worker(this);
// ... worker loop ...
runtime_->clear_current_worker();
```

**用途**：

1. `yield()` 需要知道当前 worker ID（affinity）
2. `current_runtime()` 提供运行时访问（协程 awaiter 需要）
3. 用户代码可以通过 `thread_local` 感知所在 worker

**线程安全保证**：`thread_local` 天然线程安全，每个线程有独立副本，无需任何同步。

---

## 关键并发序列

### submit 与 worker pop 的并发

```
Thread A (submit)                    Thread B (worker)
─────────────────                    ─────────────────
lock(not needed for SafeQueue)
queue.push(task)                     wait on work_cv_
unlock
                                     lock(mutex_)
notify_one() ──────────────────────► check has_work → true
                                     unlock
                                     lock(SafeQueue)
                                     queue.pop(task)
                                     unlock
```

**顺序保证**：task 入队 → unlock → notify，worker 收到通知 → lock → pop。任务一定在通知前入队，worker 不会看到空队列。

### shutdown 与 submit 的竞争

```
Thread A (shutdown)                  Thread B (submit)
─────────────────                    ─────────────────
lock(mutex_)                         lock(mutex_)
shutdown_ = true                     if (shutdown_) ← 可能看到 true 或 false
unlock                               unlock
notify_all()
```

- 时序 A 先 → B 看到 `shutdown_ == true`，抛出异常
- 时序 B 先 → 任务入队，A 的 shutdown 根据模式处理（Drain 等待 / CancelPending 清空）

### 多 worker 竞争同一任务

```
Worker A                             Worker B
────────                             ────────
pop_for_worker(0):                   pop_for_worker(1):
  check own coroutine queue → empty    check own coroutine queue → empty
  check global queue → 1 task          check global queue → 1 task ← 竞争！
  pop success ✓                        pop fail (已空)
  return task                           check steal → ...
```

**SafeQueue 保证**：只有一个 worker 能成功 pop，另一个得到 `false` 后会继续检查窃取路径。

---

## 防死锁设计

1. **锁仅在必要时持有** — 不在持有锁时调用外部代码（除了嵌套锁层级内的）
2. **不使用 `std::recursive_mutex`** — 需要重入说明设计有问题
3. **协程队列操作（SafeQueue）在调度器锁外** — pop_for_worker 在调用者持有 scheduler mutex 时操作 SafeQueue，但 SafeQueue 的 mutex 是独立的（第 3 层，只锁 queue）
4. **WorkerGroup 锁与 TaskScheduler 锁并行** — 两者不存在同时持有的情况
