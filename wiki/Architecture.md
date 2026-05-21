# Architecture

ThreadPool 采用**四层架构**，从公开 API 到底层线程执行逐层分工。

## 架构总览

```
┌─────────────────────────────────────────────┐
│              ThreadPool (Facade)             │  ← 公开 API
│         submit() / schedule() / yield()      │
│               shutdown() / start()           │
└──────────────────┬──────────────────────────┘
                   │ 委托
┌──────────────────▼──────────────────────────┐
│        ThreadPoolRuntime (Coordinator)       │  ← 运行时协调
│    启动/关闭、worker 唤醒/休眠、跨层调度       │
│          thread_local 上下文管理              │
└──────┬──────────────────────┬───────────────┘
       │                      │
┌──────▼─────────┐   ┌───────▼──────────────┐
│ TaskScheduler  │   │    WorkerGroup        │  ← 调度 + 线程管理
│ (调度器)       │   │    (线程组)           │
│                │   │                       │
│ 全局任务 FIFO  │   │  std::vector<thread>  │
│ 协程队列(每wkr)│   │  spawn/retire/reap    │
│ 工作窃取       │   │  live_workers 计数    │
│ 活跃计数       │   │  动态扩缩             │
└──────┬─────────┘   └───────┬──────────────┘
       │                      │
┌──────▼──────────────────────▼──────────────┐
│  SafeQueue<T>       ThreadPoolWorker        │  ← 数据结构 + 执行单元
│  (线程安全队列)      (worker 主循环)         │
│                       wait → pop → execute  │
└─────────────────────────────────────────────┘
```

## 各层职责

### Layer 1: ThreadPool（Facade）

**文件**：`ThreadPool.h`、`ThreadPoolSubmit.h`、`ThreadPoolImpl.h`

唯一的用户入口。职责：

- 包裹参数到 `packaged_task<R()>` → 提取 future → 包装到 `std::function<void()>`
- 调用 `runtime_.submit_task()` 入队
- 提供 `schedule()` / `yield()` 返回 awaiter 对象
- 协程 awaiter 的 `await_suspend()` 调用 `runtime_.enqueue_coroutine_resume()`

**不持有锁**，所有状态委托给 Runtime。

### Layer 2: ThreadPoolRuntime（Coordinator）

**文件**：`ThreadPoolRuntime.h`、`ThreadPoolRuntimeImpl.h`

协调整体生命周期和跨层交互。职责：

- **启动/关闭**：`start()` 创建 worker、`shutdown()` 通知停止
- **worker 唤醒/休眠**：通过 `std::condition_variable work_cv_` 和谓词 `has_any_work_for_worker()`
- **thread_local 上下文**：`current_runtime` / `current_worker_id`（协程亲和性需要）
- **协程入队**：`enqueue_coroutine_resume()` 决定目标 worker 并调用 Scheduler
- **动态扩缩触发**：`submit_task()` 后调用 `maybe_grow_unlocked()`

**持有锁**：`std::mutex mutex_` 保护 `shutdown_`、`started_` 和 `work_cv_`。

### Layer 3.1: TaskScheduler（Scheduler）

**文件**：`TaskScheduler.h`

所有调度逻辑。职责：

- **双队列模型**：
  - 全局 `SafeQueue<Task>` — 普通任务（`submit()` 提交的）
  - 每个 worker 一个 `SafeQueue<Task>` — 协程任务
- **4 级优先级 pop**：见下方数据流
- **工作窃取**：`steal_coroutine()` 从其他 worker 的协程队列取任务
- **活跃计数**：`mark_started()` / `mark_finished()` 管理 `active_tasks_`
- **空闲等待**：`wait_idle()` / `notify_idle_if_needed()` 通过 `idle_cv_`
- **协程入队**：`push_coroutine()` 支持指定目标 worker（轮询 / 亲和）

**持有锁**：`std::mutex mutex_` 保护 `active_tasks_`、`next_coroutine_worker_`、`idle_cv_`。

### Layer 3.2: WorkerGroup（Thread Manager）

**文件**：`WorkerGroup.h`、`WorkerGroupImpl.h`

管理 `std::vector<std::thread>` 容器。职责：

- **spawn**：创建线程（先 reap 已退休线程，异常安全地增加 `live_workers_`）
- **retire**：标记线程 ID 到退休列表（由 worker 自己调用）
- **reap**：join 退休线程并从容器移除
- **grow**：根据队列压力决定是否扩容（`queued_tasks() > live_workers()`）
- **join_all**：关闭时 join 所有线程
- **动态计数**：`std::atomic<size_t> live_workers_` 和 `next_worker_id_`（无锁读）

**持有锁**：`std::mutex mutex_` 保护 `workers_` 容器和 `retired_worker_ids_`。

### Layer 4: ThreadPoolWorker + SafeQueue

**ThreadPoolWorker**（`ThreadPoolWorker.h`）— worker 线程主循环：

```
set_current_worker(this)
    │
    ▼
┌─────────────────────┐
│  wait_for_work()    │  ← 条件变量等待，返回 WorkerWork
│  (ThreadPoolRuntime)│
└───────┬─────────────┘
        │
   ◄────┼──────────────────── 如果 exit == true
        │
        ▼
┌─────────────────────┐
│  execute(task)      │  ← try/catch 包裹，防止未捕获异常
└───────┬─────────────┘
        │
        ▼
┌─────────────────────┐
│  finish_work()      │  ← 管理协程 burst 计数
│  (ThreadPoolRuntime)│
└─────────────────────┘
        │
        └──────→ 循环回到 wait_for_work()

clear_current_worker()
```

**SafeQueue<T>**（`SafeQueue.h`）— 线程安全队列：

```cpp
template<typename T>
class SafeQueue {
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    // push / pop / empty / size / clear
};
```

每个操作独立加锁，不自带条件变量。

---

## 核心数据流

### 任务提交流（submit）

```
用户代码                ThreadPool            ThreadPoolRuntime       TaskScheduler       Worker
  │                       │                      │                      │                 │
  ├─submit(f, args)──────►│                      │                      │                 │
  │                       ├─packaged_task 包装    │                      │                 │
  │                       ├─提取 future          │                      │                 │
  │                       ├─→ std::function<void()>                     │                 │
  │                       ├─submit_task(task)───►│                      │                 │
  │                       │                      ├─检查 shutdown        │                 │
  │                       │                      ├─push_task(task)─────►│                 │
  │                       │                      │                      ├─global_queue     │
  │                       │                      ├─maybe_grow()         │   .push(task)    │
  │                       │                      ├─notify_one()─────────┼─────────────────►│
  │  ◄── future           │                      │                      │                 │
  │                       │                      │                      │                 ├─pop_for_worker()
  │                       │                      │                      │                 ├─execute(task)
  │                       │                      │                      │                 ├─finish_work()
```

### 协程 schedule() 流

```
协程                    ScheduleAwaiter        ThreadPoolRuntime       TaskScheduler
  │                       │                      │                      │
  ├─co_await schedule()──►│                      │                      │
  │                       ├─await_ready()=false  │                      │
  │                       ├─await_suspend(h)────►│                      │
  │                       │                      ├─检查 mode/shutdown   │
  │                       │                      ├─选目标 worker(轮询)   │
  │                       │                      ├─push_coroutine(wid,h)►│
  │                       │                      │                      ├─worker[wid].queue
  │                       │                      │                      │  .push(h.resume)
  │                       │                      ├─notify_all()─────────┼──► 唤醒 worker
  │  (挂起)               │                      │                      │
  │                       │                      │                      │
  │  ◄── 在 worker 上恢复 │                      │                      │
```

### pop_for_worker() 4 级优先级

```
pop_for_worker(worker_id):
    │
    ├─1. 如果 burst_count >= burst_limit (8)
    │     → 优先检查全局任务队列
    │     → 目的是防止协程饥饿普通任务
    │
    ├─2. 检查自己的协程队列
    │     → 有：pop + 设为 popped_coroutine=true
    │
    ├─3. 检查全局任务队列
    │     → 有：pop + 设为 popped_coroutine=false
    │
    └─4. 窃取其他 worker 的协程队列
          → 遍历所有 worker j ≠ worker_id
          → 找到第一个有任务的：pop + 设为 popped_coroutine=true
```

---

## 锁层级

**原则**："谁拥有数据，谁管理锁"

```
Lock Hierarchy（从外到内）:
┌──────────────────────────────────────┐
│ ThreadPoolRuntime::mutex_            │  ← 最外层
│   保护: shutdown_, started_, work_cv_│
│                                      │
│  ┌────────────────────────────────┐  │
│  │ TaskScheduler::mutex_          │  │  ← 中层
│  │   保护: active_tasks_,         │  │
│  │         next_coroutine_worker_,│  │
│  │         idle_cv_               │  │
│  │                                │  │
│  │  ┌──────────────────────────┐  │  │
│  │  │ SafeQueue::m_mutex       │  │  │  ← 最内层
│  │  │   保护: std::queue<T>    │  │  │
│  │  └──────────────────────────┘  │  │
│  └────────────────────────────────┘  │
│                                      │
│  ┌────────────────────────────────┐  │
│  │ WorkerGroup::mutex_            │  │  ← 并行于 TaskScheduler
│  │   保护: workers_, retired_     │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

**关键规则**：

1. **不从内层锁向外层锁扩展** — 避免死锁
2. **`notify_one()` / `notify_all()` 始终在锁外调用** — 避免"惊群"（thundering herd）导致不必要的唤醒
3. **`live_workers_` 和 `next_worker_id_` 使用 `std::atomic`** — 无锁读取
4. **`SafeQueue::m_mutex` 只在 queue 操作时持有** — 不会和上层锁形成嵌套

---

## 线程模型

```
┌──────────────────────────────────────────────────┐
│                    用户线程                       │
│              (调用 submit / shutdown)             │
└───────────────────┬──────────────────────────────┘
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
   ┌─────────┐ ┌─────────┐ ┌─────────┐
   │Worker 0 │ │Worker 1 │ │Worker N │
   │         │ │         │ │         │
   │ wait()  │ │ wait()  │ │ wait()  │  ← 条件变量等待
   │ pop()   │ │ pop()   │ │ pop()   │  ← 从调度器取任务
   │ exec()  │ │ exec()  │ │ exec()   │  ← 执行任务
   └─────────┘ └─────────┘ └─────────┘
```

- 每个 worker 是独立的 `std::thread`，运行 `ThreadPoolWorker::operator()`
- 用户线程和 worker 通过 `SafeQueue` + 条件变量通信
- `thread_local` 变量（`current_runtime`、`current_worker_id`）由 worker 在入口设置，出口清除
