# ThreadPool 架构文档

本文档描述线程池项目的代码架构、组件职责、数据流、锁层次和文件组织。

## 总体架构

项目采用**四层架构**，每层职责明确、单向依赖：

```
┌─────────────────────────────────────────────────┐
│  ThreadPool (Facade)                             │
│  对外接口层：submit / shutdown / schedule / yield │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│  ThreadPoolRuntime (Runtime Coordinator)         │
│  运行时协调层：启动/关闭流程、worker 唤醒、       │
│  跨层通知、当前 worker 上下文                     │
└──────┬────────────────────────┬─────────────────┘
       │                        │
┌──────▼──────────┐    ┌───────▼──────────────────┐
│  TaskScheduler   │    │  WorkerGroup             │
│  调度层：        │    │  线程管理层：             │
│  全局任务队列    │    │  worker 创建/回收         │
│  协程队列        │    │  动态扩缩容               │
│  work stealing   │    │  join 管理                │
│  active 计数     │    │                          │
│  idle 等待       │    │                          │
└──────┬──────────┘    └───────┬──────────────────┘
       │                        │
       │              ┌─────────▼──────────────────┐
       │              │  ThreadPoolWorker           │
       │              │  执行单元层：               │
       │              │  wait → pop → execute       │
       │              │  协程 burst 计数            │
       │              └────────────────────────────┘
       │
┌──────▼──────────┐
│  SafeQueue<T>   │
│  基础数据结构    │
│  线程安全队列    │
└─────────────────┘
```

## 组件职责

### ThreadPool — 对外 Facade

文件：[ThreadPool.h](../ThreadPool.h)

- 持有 `ThreadPoolRuntime runtime_` 实例（组合，非指针）
- 所有对外接口均通过 `runtime_` 转发，自身不含业务逻辑
- 定义 `ScheduleAwaiter` / `YieldAwaiter` 两个内部类，供 C++20 协程使用
- `submit()` 和 `submit_with_stop()` 为模板成员函数，实现在 [ThreadPoolSubmit.h](../ThreadPoolSubmit.h)
- 构造时自动调用 `start()`，析构时自动调用 `shutdown()`（noexcept 保护）
- 不可拷贝、不可移动

### ThreadPoolRuntime — 运行时协调层

文件：[ThreadPoolRuntime.h](../include/threadpool/ThreadPoolRuntime.h) + [ThreadPoolRuntimeImpl.h](../include/threadpool/ThreadPoolRuntimeImpl.h)

- 持有 `TaskScheduler` 和 `WorkerGroup` 实例，持有 `on_exception_` 回调
- 持有 `mutex_` 和 `work_cv_`（worker 唤醒条件变量）
- 管理 `shutdown_` 和 `started_` 两个状态标记
- 通过 `thread_local` 静态变量提供当前 worker 上下文（`current_runtime()` / `current_worker_id()`）
- 核心方法：
  - `start()`：幂等启动，创建初始 worker
  - `shutdown()`：幂等关闭，支持 Drain / CancelPending
  - `shutdown_for()` / `shutdown_until()`：带超时关闭，超时后 detach 剩余 worker
  - `submit_task()`：检查关闭状态 → 入队 → 可能扩容 → 唤醒一个 worker
  - `wait_for_work()`：worker 等待任务或超时。min worker 用 `wait()`（无限等待），excess worker 用 `wait_for(timeout)`
  - `finish_work()`：更新协程 burst 计数和 active 计数
  - `enqueue_coroutine_resume()`：协程恢复入队，支持亲和性选择，`is_current` 条件只计算一次

### TaskScheduler — 任务调度层

文件：[TaskScheduler.h](../include/threadpool/TaskScheduler.h)

- 持有全局 `SafeQueue<Task>` 普通任务队列
- 持有 `vector<unique_ptr<SafeQueue<Task>>>` 每 worker 协程队列
- 持有 `active_tasks_` 活跃任务计数、`next_coroutine_worker_` 轮询下标、`max_coroutine_queue_size_` 容量限制
- 持有 `mutex_`（保护 active 计数和轮询下标）和 `idle_cv_`（空闲等待条件变量）
- 核心方法：
  - `push_task()` / `cancel_pending_tasks()`：普通任务操作
  - `pop_for_worker()`：**乐观 active 递增**后按优先级取任务（burst 耗尽→全局，己协程→全局→steal）。失败则回滚递减。close 了 wait_idle 的 TOCTOU 竞态
  - `push_coroutine()`：协程恢复入队，支持 round-robin 或亲和性选择。队列满时抛异常
  - `steal_coroutine()`：从其他 worker 协程队列偷取任务
  - `mark_started()` / `mark_finished()`：维护 active 计数
  - `wait_idle()` / `wait_idle_for()` / `wait_idle_until()`：空闲等待
  - `has_any_work_for_worker()`：判断是否有可执行任务（含可 steal）
  - `max_coroutine_queue_size()`：查询容量限制（0=无限）

### WorkerGroup — 线程组管理层

文件：[WorkerGroup.h](../include/threadpool/WorkerGroup.h) + [WorkerGroupImpl.h](../include/threadpool/WorkerGroupImpl.h)

- 持有 `vector<thread>` 线程容器和 `vector<size_t>` 退休索引记录（从 `thread::id` 改为索引，避免 OS 复用 ID 引发误回收）
- `live_workers_` 和 `next_worker_id_` 使用 `atomic<size_t>` 无锁访问
- 持有 `mutex_` 保护线程容器操作
- 核心职责：
  - `spawn()`：先 reap retired → +1 live 计数 → 创建线程（失败则回滚计数）
  - `reap_retired_workers()`：降序排序退休索引，从后往前 join + erase
  - `maybe_grow()`：当排队任务数 > 存活 worker 数时触发扩容
  - `try_retire()`：CAS 循环原子检查 `live_workers > min_workers` 并递减，避免多 worker 同时退休突破下限
  - `retire_current_worker()`：记录当前线程在 workers_ 中的索引到退休列表
  - `join_all()` / `join_all_for()` / `join_all_until()`：join 所有 worker。带超时版本使用 helper 线程 + deadline 轮询，超时则 detach

### ThreadPoolWorker — 执行单元

文件：[ThreadPoolWorker.h](../include/threadpool/ThreadPoolWorker.h)

- 持有 `runtime_` 指针和 `worker_id_`
- `operator()()` 是线程入口：
  1. `set_current_worker()` — 设置 thread_local 上下文
  2. 循环调用 `wait_for_work()` → 执行任务 → `finish_work()`
  3. 收到 exit 信号后 `clear_current_worker()` 退出
- 任务执行包裹 try/catch。若配置了 `on_exception` 回调则先调用回调再丢弃异常

### 辅助组件

| 组件 | 文件 | 职责 |
|------|------|------|
| `ThreadPoolOptions` | [ThreadPoolOptions.h](../include/threadpool/ThreadPoolOptions.h) | 启动参数：min/max workers、execution mode、coroutine burst limit、max coroutine queue size、exception callback、idle timeout |
| `StopSource` / `StopToken` | [ThreadPoolStopToken.h](../include/threadpool/ThreadPoolStopToken.h) | 协作式取消：共享 `atomic<bool>` 实现发布-订阅 |
| `detail::invoke_result_t` | [ThreadPoolTypeTraits.h](../include/threadpool/ThreadPoolTypeTraits.h) | C++14/17 兼容类型萃取 |
| `detail::apply_impl` | [ThreadPoolSubmit.h](../include/threadpool/ThreadPoolSubmit.h) | C++14 下替代 `std::apply` 的 index_sequence 展开工具 |

### SafeQueue<T> — 线程安全队列

文件：[SafeQueue.h](../include/threadpool/SafeQueue.h)

- 模板类，包装 `std::queue<T>` + `mutable std::mutex`
- 每个操作（push/pop/empty/size/clear）独立加锁
- 不可拷贝、不可移动
- 不持有条件变量，专注数据结构安全

## 数据流

### 普通任务提交流

```
Client                    ThreadPool           ThreadPoolRuntime      TaskScheduler        WorkerGroup        Worker
  │                           │                      │                     │                   │                 │
  │──submit(f, args...)──────►│                      │                     │                   │                 │
  │                           │──submit_task(task)───►│                     │                   │                 │
  │                           │                      │──检查 shutdown       │                   │                 │
  │                           │                      │──push_task(task)────►│                   │                 │
  │                           │                      │──maybe_grow()────────│──────────────────►│                 │
  │                           │                      │──notify_one()        │                   │                 │
  │                           │                      │                     │                   │                 │
  │   ◄──future               │                      │                     │                   │  ◄──唤醒        │
  │                           │                      │                     │                   │                 │
  │                           │                      │◄──wait_for_work()────│                   │                 │
  │                           │                      │                     │                   │                 │
  │                           │                      │──pop_for_worker()───►│ (内含 optimistic ++active)     │
  │                           │                      │◄──WorkerWork         │                   │                 │
  │                           │                      │                     │                   │──task()────────►│
  │                           │                      │                     │                   │                 │──执行
  │                           │                      │──finish_work()──────►│                   │                 │
  │                           │                      │                     │──mark_finished()  │                 │
  │                           │                      │◄──notify_idle        │                   │                 │
  │   ◄──future.get()         │                      │                     │                   │                 │
```

### 协程调度的两个路径

**schedule() — 初始调度到线程池：**

```
Coroutine                  ScheduleAwaiter        ThreadPoolRuntime      TaskScheduler
  │                           │                      │                     │
  │──co_await schedule()─────►│                      │                     │
  │                           │──await_suspend(h)───►│                     │
  │                           │                      │──检查模式            │
  │                           │                      │──检查 shutdown       │
  │                           │                      │──push_coroutine()───►│
  │                           │                      │                     │──round-robin 选 worker
  │   ◄──suspend              │                      │                     │──入队目标协程队列
  │                           │                      │──notify_all()       │
  │                           │                      │                     │
  │                    (稍后 worker 执行 handle.resume())                   │
  │   ◄──resume on worker     │                      │                     │
```

**yield() — 协程主动让出后放回：**

```
Coroutine                  YieldAwaiter          ThreadPoolRuntime      TaskScheduler
  │                           │                      │                     │
  │──co_await yield()────────►│                      │                     │
  │                           │──await_suspend(h)───►│                     │
  │                           │                      │──检查模式+shutdown   │
  │                           │                      │──prefer_current=true │
  │                           │                      │──目标=当前worker_id  │
  │                           │                      │──push_coroutine()───►│
  │                           │                      │                     │──入队本worker协程队列
  │                           │                      │──notify_all()       │
```

### Worker 取任务的优先级（pop_for_worker）

```
                    ┌─────────────────────────────┐
                    │ coroutine_burst >= limit ?    │
                    │ 是 → 优先全局普通任务          │
                    └──────────┬──────────────────┘
                               │否
                    ┌──────────▼──────────────────┐
                    │ 本 worker 协程队列有任务？    │
                    │ 是 → pop coroutine (burst++) │
                    └──────────┬──────────────────┘
                               │否
                    ┌──────────▼──────────────────┐
                    │ 全局普通队列有任务？           │
                    │ 是 → pop task (burst=0)      │
                    └──────────┬──────────────────┘
                               │否
                    ┌──────────▼──────────────────┐
                    │ steal 其他 worker 协程任务？  │
                    │ 成功 → pop (burst 不重置)    │
                    └─────────────────────────────┘
```

## 锁层次

项目遵循**严格的分层锁策略**，每把锁只保护其持有者拥有的数据：

```
                    ┌──────────────────┐
                    │ runtime.mutex_    │  保护: shutdown_, started_
                    │ + work_cv_        │        worker 唤醒
                    └────────┬─────────┘
                             │ 调用下层方法（不持本层锁）
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼─────┐ ┌─────▼──────┐ ┌─────▼──────────┐
     │ scheduler    │ │ workers    │ │ SafeQueue      │
     │ .mutex_      │ │ .mutex_    │ │ .m_mutex       │
     │ 保护:        │ │ 保护:      │ │ 保护: queue    │
     │ active_tasks │ │ thread数组 │ │ push/pop/      │
     │ next_worker  │ │ retired    │ │ size/clear     │
     │ idle_cv_     │ │            │ │                │
     └──────────────┘ └────────────┘ └────────────────┘
```

**锁规则：**

1. `SafeQueue` 的锁完全内部化，外部调用者不需要感知
2. `TaskScheduler::mutex_` 不保护队列本身（队列有内部锁），只保护 active 计数和调度状态
3. `WorkerGroup::mutex_` 只在线程容器操作时使用（创建、回收、join）
4. `ThreadPoolRuntime::mutex_` 是最外层锁，保护启动/关闭流程，并作为 `work_cv_` 的关联锁
5. 不允许跨层持锁调用 — 调用下层方法前必须释放上层锁

**关键并发场景：**

- **提交任务**：`runtime.mutex_` 内检查 shutdown → 入队（SafeQueue 内部加锁）→ 可能扩容（WorkerGroup 内部加锁）→ 释放 runtime 锁 → notify
- **Worker 等待与取任务**：持有 `runtime.mutex_` 调用 `wait()`/`wait_for()` → 唤醒后在锁内 `pop_for_worker()`（内部先 `scheduler.mutex_` 锁内 `++active`，再 SafeQueue 锁内 pop）。乐观递增确保 `wait_idle` 不会在 pop 和 mark_started 之间看到不一致状态
- **Worker 退休**：Idle timeout 路径用 `try_retire()` CAS 原子检查并递减 `live_workers_`。Shutdown 路径无条件 `decrease_live_worker()`
- **Idle 判断**：`scheduler.mutex_` 保护 active_tasks_ 读写，配合 `idle_cv_` 做等待。`idle_cv_.wait()` 的 predicate 中 `task_queue_.empty()` 内部加 SafeQueue 锁——形成了 scheduler.mutex_ → SafeQueue 的锁序
- **锁顺序**：runtime.mutex_ → WorkerGroup.mutex_ → scheduler.mutex_ → SafeQueue.m_mutex。所有路径遵循此序，不存在反向获取

## 线程模型

```
主线程 / 调用方线程
  │
  ├── submit() ─────────────► 入队 + notify_one
  │
  ├── future.get() ─────────► 阻塞等待结果
  │
  ├── shutdown() ───────────► 设置关闭标志 + notify_all + join_all
  │
  └── wait_idle() ──────────► 等待队列空 + active 为 0

Worker 线程 × N
  │
  ├── wait_for_work() ──────► condition_variable wait（关联 runtime.mutex_）
  │
  ├── pop_for_worker() ─────► 按优先级取任务
  │
  ├── task() ───────────────► 执行 std::function<void()>
  │
  └── finish_work() ────────► 更新 burst 计数 + mark_finished
```

- Worker 数量由 `min_workers` / `max_workers` 控制
- 支持**动态扩容**：当 `queued_tasks > live_workers` 且 `live_workers < max_workers` 时自动创建新 worker
- 支持**空闲收缩**：当 `live_workers > min_workers` 且 worker 空闲超时（默认 5s）时退出。退休操作使用 CAS 循环保证原子性，避免并发退休突破 min 下限
- Worker 线程通过 `thread_local` 访问当前 runtime 和 worker_id，无需全局变量

## 文件组织与 Include 顺序

```
include/threadpool/
├── SafeQueue.h                     # 基础数据结构（无内部依赖）
├── ThreadPoolOptions.h             # 枚举和配置结构（无内部依赖）
├── ThreadPoolStopToken.h           # 取消令牌（无内部依赖）
├── ThreadPoolTypeTraits.h          # 类型兼容层（无内部依赖）
├── TaskScheduler.h                 # 调度层（依赖 SafeQueue + Options）
├── ThreadPoolRuntime.h             # 运行时声明（依赖 TaskScheduler + WorkerGroup + Options）
├── ThreadPoolWorker.h              # Worker 声明（依赖 ThreadPoolRuntime 声明）
├── WorkerGroup.h                   # 线程组声明（前向声明 ThreadPoolRuntime）
├── ThreadPool.h                    # Facade — 唯一对外入口
│
│  # 以下为 inline 实现文件，由 ThreadPool.h 统一 include
├── WorkerGroupImpl.h               # WorkerGroup::spawn() 实现
├── ThreadPoolRuntimeImpl.h         # ThreadPoolRuntime 全部方法实现
├── ThreadPoolImpl.h                # ThreadPool 协程转发实现
└── ThreadPoolSubmit.h              # submit() / submit_with_stop() 模板实现

examples/
└── main.cpp                        # 使用示例

tests/
├── test.cpp                        # 行为测试（13 cases）
├── test_coroutine.cpp              # 协程测试（7 cases）
├── stress_test.cpp                 # 压力测试
├── benchmark.cpp                   # 性能基准 + std::async 基线
├── mode_compare.cpp                # 线程 vs 协程模式对比
└── long_run_stress.cpp             # 长时运行压力测试
```
```

**设计要点：**
- 4 个底层头文件无内部依赖，被所有上层组件使用
- `ThreadPoolRuntime` 声明依赖 `TaskScheduler` 和 `WorkerGroup` 的完整定义
- `WorkerGroup` 仅前向声明 `ThreadPoolRuntime`，避免头文件循环
- 所有 inline 实现在 `ThreadPool.h` 末尾统一引入，保证模板和 inline 函数的可见性
- 外部用户只需 `#include "ThreadPool.h"`

## C++ 版本兼容策略

| 特性 | C++14 | C++17+ |
|------|-------|--------|
| 返回类型推导 | `std::result_of<F(Args...)>::type` | `std::invoke_result_t<F, Args...>` |
| 参数展开 | `index_sequence` 手动展开 | `std::apply` |
| 协程支持 | 不可用 | `#if __has_include(<coroutine>)` 条件启用 |
| 条件变量等待 | 均支持 | 均支持 |

关键宏：
- `__has_include(<coroutine>) && defined(__cpp_impl_coroutine)` — 检测协程可用性
- `(defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L` — 检测 C++17 可用性

## 生产化成熟度

| 能力 | 实现状态 |
|------|---------|
| 任务调度 | 全局 FIFO + per-worker 协程队列 + work stealing |
| 关闭方式 | Drain / CancelPending / 带超时关闭 |
| 协程调度 | `schedule()` + `yield()` + burst 控制 + 队列上限 |
| 异常处理 | packaged_task → future / on_exception 回调 |
| 动态伸缩 | min/max workers + idle 回收（CAS 原子退休） |
| 空闲等待 | wait_idle / wait_idle_for / wait_idle_until |
| 取消机制 | StopToken 协作式取消 |
| CI 门禁 | Release × 2 平台 + TSan + ASan/UBSan + Valgrind |
| 安装分发 | CMake install + vcpkg.json |
| 测试覆盖 | 行为 13 + 协程 7 + 压测 + 长时运行 + benchmark |
