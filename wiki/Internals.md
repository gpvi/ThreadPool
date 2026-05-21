# Internals

内部实现细节、设计决策和扩展点。面向想要深入理解或贡献代码的读者。

> 建议先阅读 [Architecture](Architecture.md) 和 [Thread Safety](Thread-Safety.md)。

## 文件组织与 include 顺序

```
ThreadPool.h                          ← 公开头文件，用户只 include 这个
│
├── [声明区] — 仅类型声明，无实现
│   ├── ThreadPoolOptions.h           ← 枚举 + 配置结构体
│   ├── ThreadPoolStopToken.h         ← StopToken / StopSource
│   ├── ThreadPoolTypeTraits.h        ← invoke_result_t 兼容层
│   ├── SafeQueue.h                   ← 线程安全队列模板
│   ├── TaskScheduler.h               ← 调度器全部声明+实现
│   ├── ThreadPoolRuntime.h           ← 运行时声明 + WorkerWork
│   ├── ThreadPoolWorker.h            ← worker 线程主循环
│   ├── WorkerGroup.h                 ← 线程组声明
│   └── class ThreadPool { ... };     ← 公开 API 声明
│
└── [实现区] — inline 实现
    ├── WorkerGroupImpl.h             ← spawn 实现
    ├── ThreadPoolRuntimeImpl.h       ← 运行时 + enqueue_coroutine_resume
    ├── ThreadPoolSubmit.h            ← submit / submit_with_stop 模板
    └── ThreadPoolImpl.h              ← 协程转发
```

**分层原因**：
1. 声明和实现分离，方便读者快速了解 API
2. 模板实现在 header 内，但逻辑上独立
3. 每个 `*Impl.h` 对应一个声明头文件

---

## 关键数据结构全景

```
ThreadPool
└── ThreadPoolRuntime
    ├── std::mutex mutex_                   ← 保护 shutdown_ / started_
    ├── std::condition_variable work_cv_    ← worker 唤醒
    ├── ThreadPoolRuntime* current_runtime  ← thread_local
    ├── size_t current_worker_id             ← thread_local
    │
    ├── TaskScheduler
    │   ├── SafeQueue<Task> global_queue_   ← submit() 任务
    │   ├── vector<unique_ptr<SafeQueue<Task>>> coroutine_queues_
    │   │   └── per-worker: SafeQueue<Task> ← schedule/yield 协程
    │   ├── atomic<size_t> active_tasks_     ← 正在执行的任务
    │   ├── atomic<size_t> next_coroutine_worker_  ← 轮询索引
    │   ├── size_t coroutine_burst_limit_
    │   ├── std::mutex mutex_               ← active_tasks_ 等
    │   └── std::condition_variable idle_cv_ ← idle 等待
    │
    └── WorkerGroup
        ├── size_t min_workers_
        ├── size_t max_workers_
        ├── milliseconds idle_timeout_
        ├── atomic<size_t> live_workers_
        ├── atomic<size_t> next_worker_id_
        ├── vector<thread> workers_          ← 活跃线程
        ├── vector<thread::id> retired_worker_ids_  ← 待 join 线程
        └── std::mutex mutex_               ← workers_ / retired_
```

---

## Type Erasure 链（完整版）

从用户调用 `submit(f, args...)` 到 worker 执行，涉及的每一步：

```
Step 1: 用户层
  pool.submit(f, a, b, c)
  → 模板参数推导: F=decltype(f), Args={A, B, C}

Step 2: 类型推导
  invoke_result_t<F, Args...>
  → C++17: std::invoke_result_t<F, A, B, C>
  → C++14: typename std::result_of<F(A, B, C)>::type

Step 3: Lambda 捕获
  [f=forward<F>(f), ...args=forward<Args>(args)]() -> R {
      return invoke(f, forward<Args>(args)...);
  }
  → C++17: std::apply(f, tuple<Args...>(args...))
  → C++14: call_impl(f, tuple, index_sequence)

Step 4: packaged_task 包装
  make_shared<packaged_task<R()>>(lambda)
  → 用 shared_ptr 解决 packaged_task 不可拷贝问题

Step 5: future 提取
  task->get_future()
  → 返回 std::future<R>

Step 6: function<void()> 包装
  [task]() { (*task)(); }
  → 统一为 void() 签名，可存入 SafeQueue

Step 7: 入队
  runtime_.submit_task(function<void()>)
  → scheduler_.push_task(task)
  → SafeQueue::push → notify_one

Step 8: Worker 获取
  pop_for_worker() → SafeQueue::pop → 取出 function<void()>

Step 9: Worker 执行
  task() → (*packaged_task)() → lambda() → f(args...)
  → 返回值写入 promise → future 就绪
  → 异常写入 promise → future.get() 重新抛出
```

---

## wait_for_work() 详解

这是 worker 线程最核心的函数，控制 sleep/wake/exit 逻辑：

```cpp
WorkerWork ThreadPoolRuntime::wait_for_work(size_t worker_id) {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!shutdown_) {
        // 1. 带谓词的超时等待
        bool has_work = work_cv_.wait_for(
            lock,
            workers_.idle_timeout(),
            [this, worker_id] {
                return shutdown_ || scheduler_.has_any_work_for_worker(worker_id);
            });

        // 2. shutdown 检查
        if (shutdown_) {
            // 根据 shutdown mode 决定是否立即退出
            if (cancel_pending_) return {.exit = true};
            if (!scheduler_.has_any_work_for_worker(worker_id)) return {.exit = true};
            continue;  // 还有工作，继续执行
        }

        // 3. 超时 → 退休检查
        if (!has_work) {
            if (workers_.should_exit_on_idle(worker_id)) {
                return {.exit = true};
            }
            continue;  // 不能退休，继续等待
        }

        // 4. 有工作 → pop
        WorkerWork work;
        if (scheduler_.pop_for_worker(worker_id, work)) {
            return work;  // 成功取到
        }
        // 可能被其他 worker 抢先，重试
    }

    return {.exit = true};
}
```

**关键点**：
- `wait_for` 而非 `wait`：支持超时退休
- `while(!shutdown_)` 而非 `while(true)`：shutdown 后仍有工作则继续
- 虚假唤醒处理：pop 失败则重试
- 锁在 wait_for 期间释放，被唤醒后重新获取

---

## pop_for_worker() 4 级优先级（完整逻辑）

```cpp
bool TaskScheduler::pop_for_worker(size_t worker_id, WorkerWork& work) {
    // Level 1: Burst limit check
    // 如果该 worker 连续执行了太多协程，优先给普通任务
    if (coroutine_burst_count_[worker_id] >= coroutine_burst_limit_) {
        Task task;
        if (global_queue_.pop(task)) {
            work.task = std::move(task);
            work.popped_coroutine = false;
            mark_started();
            return true;
        }
    }

    // Level 2: 自己的协程队列
    // 最高效：利用 CPU 缓存热度
    if (worker_id < coroutine_queues_.size() && coroutine_queues_[worker_id]) {
        Task task;
        if (coroutine_queues_[worker_id]->pop(task)) {
            work.task = std::move(task);
            work.popped_coroutine = true;
            mark_started();
            return true;
        }
    }

    // Level 3: 全局任务队列
    // submit() 提交的普通任务
    {
        Task task;
        if (global_queue_.pop(task)) {
            work.task = std::move(task);
            work.popped_coroutine = false;
            mark_started();
            return true;
        }
    }

    // Level 4: 工作窃取（work stealing）
    // 从其他 worker 的协程队列偷任务
    return steal_coroutine(worker_id, work);
}
```

**steal_coroutine 实现**：

```cpp
bool TaskScheduler::steal_coroutine(size_t worker_id, WorkerWork& work) {
    for (size_t i = 0; i < coroutine_queues_.size(); ++i) {
        if (i == worker_id) continue;  // 跳过自己的
        if (!coroutine_queues_[i]) continue;

        Task task;
        if (coroutine_queues_[i]->pop(task)) {
            work.task = std::move(task);
            work.popped_coroutine = true;
            mark_started();
            return true;
        }
    }
    return false;  // 什么都没找到
}
```

**为什么 Level 4 放在最后？**
- 窃取涉及跨 worker 的并发访问（虽然 SafeQueue 是安全的）
- 尽量让每个 worker 处理自己的任务
- 只在真正空闲时才偷

---

## 动态扩缩（完整链路）

### 扩容

```
submit_task(task)
    │
    ├─ scheduler_.push_task(task)
    │
    └─ maybe_grow_unlocked()
         │
         └─ WorkerGroup::maybe_grow(queued_tasks, live_workers_)
              │
              ├─ if (queued <= live) return;    ← 不需要扩容
              ├─ if (live >= max) return;        ← 已达上限
              │
              └─ spawn(runtime_)
                   │
                   ├─ reap_retired_workers()     ← 先清理退休线程
                   │
                   └─ workers_.emplace_back(ThreadPoolWorker(runtime_, next_id))
                        │
                        └─ live_workers_++        ← 异常安全：线程创建成功后才 +1
```

**为什么每次只 spawn 一个？**
- 避免过度扩容（任务可能很快完成）
- 渐进式增长更平滑
- 如果有持续压力，下一次 submit 会再次触发

### 缩容

```
Worker::wait_for_work()
    │
    ├─ work_cv_.wait_for(idle_timeout)
    │   超时且没有工作
    │
    └─ workers_.should_exit_on_idle()
         │
         ├─ if (live_workers <= min_workers) return false;
         └─ return true;  → WorkerWork{exit=true}
              │
              ▼
         退出前调用 retire_current_worker()
              │
              ├─ live_workers_--        ← 原子递减
              ├─ 将 thread::id 加入 retired_worker_ids_
              └─ 线程返回（退出 operator()）
```

---

## 协程 burst 管理

每个 worker 维护自己的协程 burst 计数器：

```cpp
// ThreadPoolRuntime 内部
std::vector<size_t> worker_coroutine_burst_;  // 索引 = worker_id

void ThreadPoolRuntime::finish_work(bool popped_coroutine, size_t worker_id) {
    if (popped_coroutine) {
        worker_coroutine_burst_[worker_id]++;
    } else {
        worker_coroutine_burst_[worker_id] = 0;  // 重置
    }
}
```

**状态转换**：

```
连续执行协程:
  burst: 0→1→2→3→4→5→6→7→8
                          │
                    达到 burst_limit
                          │
                    下次 pop 优先全局队列
                          │
执行一个普通任务:
  burst: 8→0 (重置)
```

---

## C++ 版本兼容策略

| 特性 | C++14 | C++17 | C++20 |
|------|-------|-------|-------|
| `invoke_result_t` | `std::result_of` 回退 | `std::invoke_result_t` | ← 同 C++17 |
| tuple 展开 | `index_sequence` 手动 | `std::apply` | ← 同 C++17 |
| 协程 support | 不可用 | 不可用 | `#if __has_include(<coroutine>)` |
| 编译标准 | `cxx_std_14` | ← | `cxx_std_20`（仅协程目标） |

条件编译宏：
```cpp
#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
    // 启用协程相关代码
#endif
```

---

## 已知局限

| 局限 | 影响 | 可能的改进方向 |
|------|------|---------------|
| 无任务优先级 | 所有 submit 任务同优先级 | 支持优先级队列 |
| 无任务依赖图 | 不支持 DAG 调度 | 添加 `then()` / `when_all()` |
| 协程取消不完整 | CancelPending 不取消已调度的协程 | 协程感知 StopToken |
| 无内置定时器 | 无 `schedule_at()` / `schedule_after()` | 添加定时调度 |
| worker 固定 Function | 只能是 `function<void()>` | 支持任意任务类型 |
| 无 NUMA 感知 | 不考虑 CPU 拓扑 | 绑定 worker 到特定核心 |
| 单线程池 | 不支持嵌套/分层池 | 子线程池委托 |

---

## 贡献指南

### 修改代码时注意

1. **头文件包含顺序**：公开声明在前，inline 实现在后
2. **C++14 兼容**：新代码先用 C++14，C++20 用 `#if` 包裹
3. **锁层级**：不要从 SafeQueue 锁内获取 TaskScheduler 锁
4. **notify 在锁外**：`notify_one/all` 始终在 `unlock()` 后
5. **异常安全**：worker spawn 必须先创建线程再增加计数
6. **测试覆盖**：新功能要有对应的 test.cpp 或 test_coroutine.cpp 用例

### 添加新功能 check list

- [ ] 公开 API 在 `ThreadPool.h` 声明
- [ ] 实现放在对应的 `*Impl.h` 中
- [ ] 线程安全分析（哪些数据被哪些锁保护）
- [ ] 行为测试（test.cpp 或 test_coroutine.cpp）
- [ ] 压力测试（如果涉及并发路径）
- [ ] 文档更新（wiki 对应页面）
