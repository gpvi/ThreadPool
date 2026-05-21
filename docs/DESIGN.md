# ThreadPool 设计文档

本文档阐述线程池项目的设计思想、核心策略、关键决策的动机和取舍。架构层面（组件职责、数据流、锁层次、文件组织）见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 设计目标与边界

### 目标

- 用固定数量 worker 线程复用系统线程资源，避免频繁创建销毁
- 统一的任务提交流程：函数指针、lambda、成员函数、带返回值任务
- 通过 `std::future` 把异步结果和异常传回调用方
- 支持优雅关闭，可选的两种语义：排空（Drain）与取消（CancelPending）
- 支持 `wait_idle` 方便测试同步和批处理阶段衔接
- 在 C++20 环境下支持协程调度到线程池，以及协作式让出
- 保持 C++14 基础接口可用，C++20 特性条件启用

### 非目标

- 不是高性能任务调度引擎或事件循环替代品
- 不实现抢占式调度、不强制终止线程
- 不提供完整的 `Task<T>` 协程异步组合模型
- 不处理网络 I/O、定时器、信号等

## 任务模型：从任意可调用对象到 `void()`

### 类型擦除链路

外部提交的任务可以是任意签名，但内部队列只存储一种类型：

```cpp
std::function<void()>
```

这是设计中的第一个关键决策——**worker 不需要知道任务的真实类型**。

转换过程分五步：

```
submit(f, args...)                                    // f: int(int, int)
    │
    ▼
std::bind(f, args...)                                 // → 无参可调用对象
    │
    ▼
std::packaged_task<ReturnType()>                      // → 打包返回值通道
    │
    ▼
packaged_task::get_future()                           // → 返回给调用方
    │
    ▼
lambda [packaged] { (*packaged)(); }                  // → std::function<void()>
    │
    ▼
SafeQueue.push() → notify_one()
```

### 为什么选 `std::function<void()>` 而不是模板化队列

| 方案 | 优点 | 缺点 |
|------|------|------|
| 类型擦除（当前） | worker 实现简单、队列类型统一 | 虚函数调用开销、无法编译期优化 |
| 模板化队列 | 零虚函数开销 | 每种签名实例化一套队列，worker 需知道所有类型 |
| 函数指针 + void* | 零分配 | 不安全、不支持 lambda 捕获 |

对于本项目定位（学习、小型工具、后台批处理），类型擦除方案的代码简洁性收益远超其性能代价。

### 为什么用 `std::packaged_task` 而不是 `std::promise` 手写

`packaged_task` 一次性封装了三个职责：
1. 调用任务函数
2. 捕获返回值到 `promise`
3. 在异常时自动将 `std::current_exception()` 写入 promise

如果手写 `promise`，需要手动 try/catch + set_value/set_exception，容易遗漏。

### C++14 兼容：`std::apply` 的替代

[ThreadPoolSubmit.h](../ThreadPoolSubmit.h) 中，C++17 及以上用 `std::apply` 展开 tuple 参数，C++14 回退到 `index_sequence` 手动展开。返回类型萃取同样在 [ThreadPoolTypeTraits.h](../ThreadPoolTypeTraits.h) 中做条件编译：C++17+ 用 `std::invoke_result_t`，C++14 用 `std::result_of`。

`std::result_of` 在 C++20 被正式移除，MSVC 的 C++20 模式不再提供它。这是早期 CI 在 Windows 上失败的直接原因。

## 调度策略

### 双层队列模型

```
普通任务 ──► 全局 FIFO 队列 (SafeQueue, 所有 worker 共享)

协程恢复 ──► Worker 0 本地协程队列
协程恢复 ──► Worker 1 本地协程队列
              ...
协程恢复 ──► Worker N 本地协程队列
```

**为什么普通任务和协程恢复不混在同一队列？**

最初实现是把 `handle.resume()` 直接包装成普通任务入全局队列。这导致三个问题：
1. 无法表达协程与 worker 的亲和性
2. `yield()` 后不一定回到当前 worker
3. 后续无法扩展 work stealing

分离后，"一个 worker 承载多个协程"的模型变得更清晰。

### Worker 取任务的优先级

这是一个需要仔细平衡的设计点。单纯"协程优先"会让普通任务饥饿，单纯"公平轮询"会损害协程响应性。

当前策略（[TaskScheduler.h:126-150](../TaskScheduler.h#L126-L150)）：

```
1. 若 coroutine_burst >= burst_limit（默认 8）
   → 优先取全局普通任务，成功后 burst 重置为 0
2. 取本 worker 协程队列
   → 成功则 burst++
3. 取全局普通任务
   → 成功则 burst 重置为 0
4. steal 其他 worker 的协程任务
   → 成功则 burst 不变（不惩罚 steal）
```

**burst_limit 的设计考虑：**
- 太大会让普通任务长时间得不到执行
- 太小会限制协程吞吐
- 默认值 8 是一个经验值，可由 `ThreadPoolOptions::coroutine_burst_limit` 调整

### Round-Robin 初始分配

`schedule()` 对协程做首次调度时，使用轮询策略选择目标 worker（[TaskScheduler.h:250-252](../TaskScheduler.h#L250-L252)）。`next_coroutine_worker_` 在 mutex 保护下递增，保证多个调用方并发 `schedule` 时分配均匀。

### Work Stealing

当 worker 自己的协程队列为空、全局队列也为空时，会尝试从其他 worker 偷取协程恢复任务（[TaskScheduler.h:109-124](../TaskScheduler.h#L109-L124)）。

**为什么只 steal 协程任务，不 steal 普通任务？**

普通任务只有一个全局队列，所有 worker 都从同一个地方取，不需要 steal。只有协程任务分布在各 worker 的本地队列中，才存在负载不均的问题。

**Steal 策略：**
- 以 `(worker_id + offset) % N` 的顺序轮询目标 worker
- 找到第一个有任务的目标即拿走（不是偷一半）
- 偷取发生在 worker 即将空闲之时，不引入额外开销

### 协程亲和性

`yield()` 的语义是"我先让出，但最好还是回到当前 worker"（`prefer_current_worker=true`）。实现上：
- 通过 `thread_local` 检查调用者是否确实是本 pool 的 worker
- 如果是，放入该 worker 的本地协程队列
- 如果不是（或未运行在 worker 上），回退到 round-robin

亲和性的好处是减少线程间数据迁移，提高缓存局部性。

## 生命周期设计

### 状态机

```
                    ┌─────────┐
                    │ 构造     │
                    └────┬────┘
                         │ start() 创建 worker
                    ┌────▼────┐
              ┌─────│ Running │◄──────────────┐
              │     └────┬────┘               │
              │          │ 队列清空            │
              │     ┌────▼────┐               │
              │     │  Idle   │───────────────┘
              │     └────┬────┘  新任务到来
              │          │
              │     ┌────▼──────────┐
              │     │ ShuttingDown  │  shutdown() 触发
              │     └────┬──────────┘
              │          │
         ┌────┴────┐ ┌──┴────────────┐
         │  Drain  │ │ CancelPending │
         └────┬────┘ └──┬────────────┘
              │          │
         ┌────▼──────────▼────┐
         │     Stopped         │  所有 worker join
         └─────────────────────┘
```

### 自动启动与自动关闭

这两个决策来自实际使用中的教训：

**自动启动**：早期版本需手动调用 `init()`。用户创建 pool 后直接 submit，任务入队但没有 worker 执行，`future.get()` 永久阻塞。改为构造函数自动启动后，`init()` 保留为幂等接口。

**自动关闭**：如果析构时还有 joinable 线程未 join，`std::thread` 析构函数会调用 `std::terminate` 终止程序。析构函数中调用 `shutdown()` 并用 try/catch 保护，保证资源回收。析构函数标记为 `noexcept`。

### 两种关闭语义

| | Drain（默认）| CancelPending |
|---|---|---|
| 新任务提交 | 抛异常 | 抛异常 |
| 已入队普通任务 | 继续执行 | 清空丢弃 |
| 已开始执行的任务 | 执行完毕 | 执行完毕 |
| 协程队列中的任务 | 继续执行 | **不清空**（见下文）|
| 被丢弃任务的 future | N/A | `std::future_error` |
| 适用场景 | 不丢任务的正常退出 | 快速退出、用户取消、服务降级 |

**为什么 CancelPending 不清空协程队列？**

协程挂起后，恢复它的唯一途径就是队列中的 `resume()` 任务。直接丢弃会让协程永远无法恢复，promise 不会完成，等待方（`CoroutineTask::get()`）永久阻塞。正确做法是让协程恢复一次，后续它如果尝试再 `schedule()` 会因为线程池已关闭而收到异常。

### 空闲等待

空闲条件定义为：

```
queued_tasks == 0 && queued_coroutines == 0 && active_tasks == 0
```

注意 `active_tasks` 是指 worker 已经取出队列、正在执行但尚未完成的任务。如果只判断队列为空，可能漏掉正在执行中的任务（它们在队列里看不到，但实际上还在占用 worker）。

三个接口：
- `wait_idle()` — 无限等待
- `wait_idle_for(timeout)` — 有超时
- `wait_idle_until(deadline)` — 有截止时间

这些接口在 `TaskScheduler` 层实现，有自己的 `idle_cv_` 和 `mutex_`，与 worker 唤醒的 `work_cv_` 独立。两个条件变量的等待条件不同：worker 等待的是"有活干或要关闭"，idle 等待的是"所有活干完"。强行共用一把锁会导致语义混淆。

### 动态扩缩容

当前支持基础的动态调整：

- **扩容触发**：`submit_task()` 中检查 `queued_tasks > live_workers` 且 `live_workers < max_workers`，自动创建新 worker
- **缩容触发**：worker 空闲超时（默认 5s）后，通过 `try_retire()` 的 CAS 循环原子检查 `live_workers > min_workers` 并递减计数。CAS 保证了多个 worker 同时超时时不会突破 min 下限
- **min worker 不超时**：处于 min_workers 层的 worker 使用 `wait()` 无限阻塞，不会被 idle_timeout 反复唤醒
- **线程回收**：退出的 worker 记录自己在 `workers_` 中的索引（从 `thread::id` 改为索引追踪，避免 OS 复用 ID 引发误回收）。下一次 `spawn()` 时降序排序后从后往前 join + erase
- **关闭方式**：`shutdown()`（无限等待）→ `shutdown_for(timeout)`（超时 detach）→ `shutdown_until(deadline)`。超时机制使用 helper 线程 join，主线程 deadline 轮询

**异常安全**：`WorkerGroup::spawn()` 先递增 `live_workers_` 计数，再创建线程。如果 `emplace_back` 抛异常，try-catch 回滚 `fetch_sub(1)` 保持计数正确。先增量后创建的顺序确保新 worker 在进入 `wait_for_work` 时计数已正确。

## 协程调度设计

### C++20 协程的缺口

C++20 标准库提供了协程的语言机制（`co_await`、`co_return`、`promise_type`），但故意不提供调度器。协程在哪里恢复执行，完全由 awaiter 的 `await_suspend` 实现决定。

本项目的 `ScheduleAwaiter` 在 `await_suspend` 中拿到 `std::coroutine_handle<>`，把它包装成线程池任务并放入队列。这样协程就被"调度到了线程池线程"。

### schedule() vs yield()

| | schedule() | yield() |
|---|---|---|
| 语义 | "把我调度到线程池 worker 上" | "我先让出，让别人跑" |
| 初始位置 | 任意线程 | 已经在 worker 上 |
| 亲和性 | round-robin 分配 | 优先回当前 worker |
| await_ready | 永远 false | 永远 false |
| 典型用法 | 协程入口第一行 | 循环体中周期性让出 |

两个 awaiter 的 `await_ready()` 都返回 `false`，这意味着协程总会挂起并重新排队。这是刻意设计——即使当前线程恰好是 pool 的 worker，也要走一遍入队-取出的流程，保证公平性。

### 协程模式显式启用

不是所有线程池使用者都需要协程。如果默认开启协程队列，会为每个 worker 创建空的协程队列，浪费内存。更重要的是，`ThreadOnly` 模式下调用 `schedule()` 直接抛异常，能在开发阶段暴露配置错误，而不是让协程默默在错误的线程上执行。

## 协作式取消

### 为什么不用强制终止

C++ 不能安全地强制终止线程。暴力杀线程的后果：
- 持有的互斥锁不会释放
- 栈上对象的析构函数不会执行
- 共享状态可能写了一半
- 文件、socket 等资源泄漏

因此采用协作式取消：

```cpp
StopSource source;
auto future = pool.submit_with_stop(source.token(), [](StopToken token) {
    while (!token.stop_requested()) {
        do_work_chunk();       // 小粒度工作单元
    }
    // 安全点：锁已释放，资源已释放
});
source.request_stop();          // 通知所有持有此 token 的任务
```

### StopSource / StopToken 的实现

[ThreadPoolStopToken.h](../ThreadPoolStopToken.h) 的设计是一个经典的发布-订阅模式：

- `StopSource` 和从它创建的 `StopToken` 共享一个 `shared_ptr<atomic<bool>>`
- `request_stop()` 写入 `true`（memory_order_release）
- `stop_requested()` 读取（memory_order_acquire）
- acquire-release 配对保证：写入 stop 之前的所有修改，在读取到 stop 之后对这些线程可见
- `shared_ptr` 保证 token 不会悬空——即使 source 先析构

## 线程安全模型

### 锁策略核心原则

**"谁拥有数据，谁管理锁。"**

这个原则防止了常见的"大锁症"——用一把锁覆盖所有操作。大锁的缺点是锁竞争集中，且无法并行执行不相关的操作。

### 各层持有的共享状态和锁

| 层次 | 共享状态 | 锁 | 备注 |
|------|---------|-----|------|
| SafeQueue | `queue<T>` | `m_mutex` | 完全内部化 |
| TaskScheduler | `active_tasks_`, `next_coroutine_worker_` | `mutex_` | 不保护队列 |
| TaskScheduler | idle 等待 | `idle_cv_` | 独立的 cv |
| WorkerGroup | `workers_`, `retired_worker_ids_` | `mutex_` | 仅线程容器 |
| WorkerGroup | `live_workers_`, `next_worker_id_` | 无（atomic） | 无锁读写 |
| ThreadPoolRuntime | `shutdown_`, `started_` | `mutex_` | 也用于 work_cv_ |

### 关键并发时序

**提交任务时的状态检查：**

```cpp
// ThreadPoolRuntimeImpl.h:79-93
void submit_task(Task task) {
    {
        lock(mutex_);
        if (shutdown_) throw ...;           // 1. 持锁检查
        scheduler_.push_task(move(task));   // 2. 入队（SafeQueue 内部加锁）
        maybe_grow_unlocked();              // 3. 扩容（WorkerGroup 内部加锁）
    }  // 4. 释放 runtime 锁
    work_cv_.notify_one();                  // 5. 锁外通知
}
```

注意 notify 在锁外执行——如果 notify 在锁内，被唤醒的 worker 会立即尝试获取同一把锁而阻塞，白费一次唤醒。

**Worker 等待任务时的条件判断：**

```cpp
// ThreadPoolRuntimeImpl.h:103-108
work_cv_.wait_for(lock, idle_timeout, [this, worker_id] {
    return shutdown_ || scheduler_.has_any_work_for_worker(worker_id);
});
```

predicate 不是简单的 `!queue.empty()`，而是 `has_any_work_for_worker()`，后者的判断范围包括：全局普通队列、自己的协程队列、其他 worker 的协程队列（可 steal 的）。这保证 worker 在有 stealable 任务时也能被唤醒。

### `thread_local` 的使用

两个 `thread_local` 变量提供 worker 上下文：

```cpp
static thread_local ThreadPoolRuntime *current_runtime;
static thread_local std::size_t current_worker_id;
```

用于：
1. `enqueue_coroutine_resume()` 中判断 `prefer_current_worker` 时的运行时和 worker_id
2. 让协程 yield 时知道"我是哪个 runtime 的哪个 worker"

`thread_local` 保证了这些状态天然线程安全，不需要加锁。

## 可观测性设计

线程池提供多维度的状态观测：

| 指标 | 含义 | 实现 |
|------|------|------|
| `queued_tasks()` | 全局队列中等待执行的任务 | `SafeQueue::size()` |
| `queued_coroutines()` | 所有协程队列中等待恢复的协程 | 遍历 sum |
| `active_tasks()` | worker 已取出、正在执行的任务 | `TaskScheduler::active_tasks_` |
| `worker_count()` | 当前存活 worker 数 | `atomic<size_t>` relaxed load |
| `min_workers()` / `max_workers()` | 配置范围 | 构造时记录 |
| `max_coroutine_queue_size()` | 协程队列上限（0=无限） | 构造时记录 |
| `is_shutdown()` | 是否已关闭 | runtime_.mutex_ 保护 |
| `execution_mode()` / `is_coroutine_enabled()` | 运行模式 | 构造时记录 |
| `on_exception` 回调 | worker 中非 packaged_task 异常 | 构造时注入，worker catch 块调用 |

`active_tasks` 采用乐观递增策略：`pop_for_worker` 在尝试取任务前先 `++active`，取失败则回滚。这消除了 `wait_idle` predicate 在"pop 完成但 mark_started 未调用"窗口中的误判。

## 取舍与限制

### 有意为之的限制

| 限制 | 原因 |
|------|------|
| 无任务优先级 | 保持队列模型简单。优先级可通过多 pool 间接实现 |
| 全局 FIFO 而非 per-worker 队列 | 实现简单，适合当前规模。多 worker 竞争同一队列在高并发下是瓶颈 |
| 无抢占式调度 | C++ 无安全强制终止线程的机制。协作式是唯一安全选项 |
| 协程仅 schedule/yield | 完整 `Task<T>` 需要独立的异步组合框架，超出本项目的范围 |
| `CancelPending` 不取消已运行的协程 | 协程帧生命周期绑定到 handle，取消需安全析构帧。运行中的协程继续执行直到 yield/schedule 时检测 shutdown 并退出 |

### 性能边界

- 全局队列在 worker 数较多（>16）且任务极短时，锁竞争成为瓶颈
- 极短任务（<1μs）场景下，调度开销可能超过任务本身
- `std::function<void()>` 包装每任务有潜在的堆分配（小函数优化缓冲约 16-32 字节）

## 测试与验证

### 测试分层

| 测试 | 文件 | 目的 | CTest |
|------|------|------|-------|
| 行为测试 (13 cases) | `tests/test.cpp` | submit/future、shutdown 语义、StopToken、动态伸缩、wait_idle_until、shutdown_timeout、max_workers 上限 | 是 |
| 协程测试 (7 cases) | `tests/test_coroutine.cpp` | schedule/yield、work stealing、burst limit、queue limit、shutdown 行为 | 是 |
| 压力测试 | `tests/stress_test.cpp` | 并发提交 + CancelPending + StopToken | 是（5000/4/4） |
| 基准测试 | `tests/benchmark.cpp` | 吞吐 + 延迟分位数 + std::async 基线 | 否 |
| 模式对比 | `tests/mode_compare.cpp` | ThreadOnly vs ThreadAndCoroutine | 否 |
| 长时压测 | `tests/long_run_stress.cpp` | 持续 submit、create/destroy 循环、协程压力、shutdown 超时 | 否 |

### CI 覆盖

GitHub Actions 5 个 job 并行：

| Job | 平台 | 检测 |
|-----|------|------|
| Release | ubuntu + windows | 编译 + 功能测试 |
| TSan | ubuntu | 数据竞争（data race） |
| ASan + UBSan | ubuntu | 内存错误、未定义行为 |
| Valgrind | ubuntu | 内存泄漏 |

确保 GCC / MSVC 均通过，C++14 / C++20 路径均编译正确。

## 后续演进方向

1. **per-worker 普通任务队列 + work stealing**：降低全局队列锁竞争，提升多 worker 吞吐
2. **任务优先级**：多优先级队列，紧急任务插队
3. **完整 `Task<T>` 协程模型**：支持 co_return 值、continuation、exception propagation
4. **更丰富的可观测性**：任务等待时间分布、per-worker 统计、prometheus 风格 metrics
