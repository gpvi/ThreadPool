# ThreadPool 实现原理

本文档从实现层面逐层解析线程池的内部机制：worker 循环怎么转、任务怎么封装、协程怎么挂到线程上、线程安全怎么保证。

---

## 1. 核心问题：为什么要线程池

创建线程的代价：系统调用 → 内核分配栈空间 → 调度器注册 → 创建完成。销毁同理。

线程池的做法：**提前创建 N 个线程，让它们反复从任务队列取任务执行，而不是每次来任务都创建新线程。**

```
普通做法:  创建线程 → 执行任务 → 销毁线程 → 创建线程 → 执行任务 → ...
线程池:    创建 N 个线程 → while(取任务→执行) → 关闭时销毁
```

核心数据结构只有两个：一个任务队列 + 一组线程。

---

## 2. Worker 主循环

每个 worker 线程的入口函数是 `ThreadPoolWorker::operator()()`：

```cpp
// ThreadPoolWorker.h
void operator()() {
    runtime_->set_current_worker(worker_id_);  // 写 thread_local

    std::size_t coroutine_burst = 0;

    while (true) {
        WorkerWork work = runtime_->wait_for_work(worker_id_, coroutine_burst);

        if (work.exit) break;         // 线程池关闭或空闲超时缩容
        if (!work.has_task) continue; // 虚假唤醒

        try { work.task(); }          // 执行 std::function<void()>
        catch (...) {}                 // 兜底，防止非 packaged_task 异常

        runtime_->finish_work(work.popped_coroutine, coroutine_burst);
    }

    runtime_->clear_current_worker();
}
```

### 2.1 wait_for_work — 带有超时的条件等待

```cpp
// ThreadPoolRuntimeImpl.h
WorkerWork wait_for_work(worker_id, coroutine_burst) {
    WorkerWork work;
    std::unique_lock<std::mutex> lock(mutex_);

    // 带超时的 predicate 等待
    const bool awakened = work_cv_.wait_for(lock, idle_timeout, [this, worker_id] {
        return shutdown_ || scheduler_.has_any_work_for_worker(worker_id);
    });

    // 情况1：超时（idle_timeout 内都没活干）
    if (!awakened) {
        if (workers_.should_exit_on_idle()) {  // live > min → 缩容退出
            retire_current_worker();
            work.exit = true;
        }
        return work;
    }

    // 情况2：关闭且没活干 → 退出
    if (shutdown_ && !scheduler_.has_any_work_for_worker(worker_id)) {
        retire_current_worker();
        work.exit = true;
        return work;
    }

    // 情况3：有活干
    work.has_task = scheduler_.pop_for_worker(worker_id, coroutine_burst, ...);
    if (work.has_task) scheduler_.mark_started();
    return work;
}
```

关键 predicate：`shutdown_ || has_any_work_for_worker(worker_id)`。注意 worker 并不只看"全局队列是否为空"，而是调用 `has_any_work_for_worker()` —— 它的搜索范围包括：全局普通队列、本 worker 协程队列、**其他 worker 的协程队列（可 steal）**。

### 2.2 pop_for_worker — 四级优先级取任务

```cpp
// TaskScheduler.h
bool pop_for_worker(worker_id, coroutine_burst, popped_coroutine, task) {
    popped_coroutine = false;

    // 第一级：burst 满了，强行看全局普通任务
    if (coroutine_burst >= burst_limit_ && task_queue_.pop(task))
        return true;   // popped_coroutine = false → 普通任务

    // 第二级：自己的协程队列
    if (worker_has_coroutine(worker_id) && coroutine_queues_[worker_id]->pop(task)) {
        popped_coroutine = true;
        return true;
    }

    // 第三级：全局普通任务
    if (task_queue_.pop(task))
        return true;   // popped_coroutine = false

    // 第四级：steal 别人的协程
    popped_coroutine = steal_coroutine(worker_id, task);
    return popped_coroutine;
}
```

### 2.3 finish_work — 更新 burst 计数

```cpp
// ThreadPoolRuntimeImpl.h
void finish_work(popped_coroutine, coroutine_burst) {
    if (popped_coroutine)
        ++coroutine_burst;    // 是协程 → 累加，满了下次就优先普通任务
    else
        coroutine_burst = 0;  // 是普通任务 → 清零

    scheduler_.mark_finished();
}
```

---

## 3. 任务提交：类型擦除全链路

外部调用：

```cpp
auto future = pool.submit([](int x) { return x * 2; }, 21);
```

### 3.1 submit 模板展开

```cpp
// ThreadPoolSubmit.h
template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args)
    -> std::future<detail::invoke_result_t<F, Args...>>
{
    using ReturnType = detail::invoke_result_t<F, Args...>;

    // 第一步：bind(f, 21) → 无参可调用对象
    // 第二步：packaged_task<ReturnType()> 包装
    auto packaged = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(f),
         tuple_args = std::make_tuple(std::forward<Args>(args)...)]() mutable -> ReturnType
        {
            // 第三步：展开参数并调用
            return std::apply(std::move(func), std::move(tuple_args));
        }
    );

    // 第四步：取出 future
    auto future = packaged->get_future();

    // 第五步：包装成 std::function<void()> 入队
    runtime_.submit_task([packaged] {
        (*packaged)();   // 执行 + 写入 promise + 异常传播
    });

    return future;
}
```

### 3.2 类型擦除链

```
int(int, int)                     用户函数签名
    │ std::bind
    ▼
int()                              绑定参数后的无参函数
    │ std::packaged_task
    ▼
packaged_task<int()>               返回值通道 + future
    │ lambda
    ▼
std::function<void()>              队列存储类型 — worker 只认这个
```

**为什么选 `std::function<void()>`？**

worker 只负责执行，它不需要知道任务有参数还是有返回值、返回 int 还是 string。类型擦除后队列类型统一，worker 实现极致简单。

代价：`std::function` 内部有虚函数调用开销，对极短任务有影响，但在绝大多数场景下可忽略。

### 3.3 为什么用 `packaged_task` 而不是手写 promise

`packaged_task` 一次性解决三个问题：

```cpp
// packaged_task 内部等价于：
try {
    auto result = func();        // 1. 调用任务
    promise.set_value(result);   // 2. 正常返回 → 写入 promise
} catch (...) {
    promise.set_exception(       // 3. 异常 → 把异常写入 promise
        std::current_exception()
    );
}
```

手写 promise 需要自己 try/catch + set_value/set_exception，容易遗漏异常路径。

### 3.4 submit_task 入队

```cpp
// ThreadPoolRuntimeImpl.h
void submit_task(Task task) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) throw std::runtime_error("submit on stopped ThreadPool");
        scheduler_.push_task(std::move(task));   // SafeQueue 内部加锁
        maybe_grow_unlocked();                    // 队列堆积 → 可能扩容
    }  // 释放锁
    work_cv_.notify_one();  // 锁外通知，避免惊群
}
```

注意 `notify_one()` 在锁外调用。如果在锁内 notify，被唤醒的 worker 会立即尝试获取同一把锁而阻塞——白费一次唤醒。

---

## 4. 线程安全机制

### 4.1 锁分层

```
ThreadPoolRuntime::mutex_     ← 最外层：shutdown_ / started_ / work_cv_
    │
    ├── SafeQueue::m_mutex    ← 队列自有锁，完全内部化
    ├── TaskScheduler::mutex_ ← 保护 active_tasks_ / next_coroutine_worker_
    └── WorkerGroup::mutex_   ← 保护线程容器 / retired 列表
```

原则：**谁拥有数据，谁管锁。** 不存在一把大锁覆盖全局。

### 4.2 SafeQueue — 最基础的线程安全单元

```cpp
// SafeQueue.h
template <typename T>
class SafeQueue {
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;

    void push(T t) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.push(std::move(t));
    }

    bool pop(T &t) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        t = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }
};
```

每个操作独立加锁，锁粒度是单次 push/pop。外部调用者不需要知道内部有锁。

### 4.3 条件变量的 predicate 模式

所有等待都使用带 predicate 的形式：

```cpp
work_cv_.wait(lock, [] { return shutdown || !queue.empty(); });
```

不用 predicate 有两个风险：
- **虚假唤醒**：OS 可能无故唤醒线程，没有 predicate 会导致空转
- **通知丢失**：notify 发生在 wait 之前，没有 predicate 补救会永久睡眠

predicate 把"条件是否满足"的判断从"是否收到 notify"变为"检查确实共享状态"，解决上述两个问题。

### 4.4 atomic 无锁计数

```cpp
// WorkerGroup.h
std::atomic<std::size_t> live_workers_{0};     // 存活 worker 数
std::atomic<std::size_t> next_worker_id_{0};   // worker id 分配器
```

这两个变量被多个线程频繁读取（判断扩容/缩容），用 `atomic` 避免每次读取都加锁。`live_workers_` 的关键约束：先创建线程并放入容器，成功了再 `fetch_add(1)`，异常安全。

### 4.5 thread_local 上下文

```cpp
// ThreadPoolRuntimeImpl.h
static thread_local ThreadPoolRuntime *current_runtime;
static thread_local std::size_t current_worker_id;
```

每个 worker 线程启动时由 `set_current_worker()` 写入自己的 runtime 指针和 worker_id。协程 yield 时通过这两个值判断"我是不是在这个 pool 的 worker 上运行的"——是就用亲和性路径，不是就回退到 round-robin。

`thread_local` 保证"天然线程安全"——每个线程有自己的副本，不存在竞争。

---

## 5. 生命周期

### 5.1 构造

```cpp
// ThreadPool.h
explicit ThreadPool(const Options &options)
    : runtime_(options)    // Runtime 构造：校验参数、初始化 Scheduler + WorkerGroup
{
    start(runtime_.min_workers());  // 自动创建 worker
}
```

`ThreadPoolRuntime` 构造函数做参数校验：`min_workers == 0` 抛 `std::invalid_argument`（0 个 worker = 任务永远无法执行），`max_workers >= min_workers`。

`start()` 是幂等的——二次调用检测 `started_` 标志后直接返回。

### 5.2 析构

```cpp
~ThreadPool() noexcept {
    try { shutdown(); } catch (...) {}
}
```

主动调用 `shutdown()` 然后吞掉所有异常——不管发生什么，析构函数不能抛异常（否则触发 `std::terminate`）。这也防止用户忘记关闭线程池导致 `std::thread` joinable 析构。

### 5.3 shutdown — 两种关闭路径

```cpp
// ThreadPoolRuntimeImpl.h
void shutdown(ShutdownMode mode) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) return;              // 幂等
        shutdown_ = true;

        if (mode == ShutdownMode::CancelPending) {
            scheduler_.cancel_pending_tasks();  // 清空全局普通队列
        }
    }                                        // 释放锁
    scheduler_.notify_idle_if_needed();      // 唤醒 idle 等待者
    work_cv_.notify_all();                   // 唤醒所有 worker
    workers_.join_all();                     // join 所有线程
}
```

| 步骤 | Drain | CancelPending |
|------|-------|---------------|
| 设置 shutdown 标志 | 是 | 是 |
| 清空全局普通队列 | 否 | 是 |
| notify_all 所有 worker | 是 | 是 |
| join 所有线程 | 是 | 是 |

**CancelPending 为什么不清空协程队列？** 协程挂起后，恢复它的唯一途径就是队列中的 `resume()` 任务。直接丢弃 → 协程永远无法恢复 → promise 不完成 → 等待方（`CoroutineTask::get()`）永久阻塞。正确做法：让协程恢复一次，后续它尝试再 `schedule()` 时因为线程池已关闭收到异常。

### 5.4 动态扩缩容

**扩容触发**（`submit_task` 中）：

```cpp
void maybe_grow_unlocked() {
    workers_.maybe_grow(this, scheduler_.queued_tasks());
}

// WorkerGroup::maybe_grow
void maybe_grow(runtime, queued_tasks) {
    if (can_spawn() && queued_tasks > live_workers())
        spawn(runtime);
}
```

当排队任务数 > 存活 worker 数且还没达到 `max_workers` 时，自动扩容。

**缩容触发**（`wait_for_work` 超时后）：

```cpp
if (!awakened) {
    if (workers_.should_exit_on_idle())  // live_workers > min_workers
        retire_current_worker();
}
```

Worker 空闲超过 `idle_timeout`（默认 30s）且当前 worker 数多于最小值时退出。

**线程回收**：worker 不能 join 自己（会死锁）。退出的 worker 记录自己的 `thread::id` 到 `retired_worker_ids_`。下一次 `spawn()` 调用时，由其他线程在 `reap_retired_workers()` 中执行 join 并 erase。`shutdown` 时 `join_all()` 做最终清理。

---

## 6. 协程调度

### 6.1 C++20 协程给了什么、没给什么

给了：`co_await` / `co_return` / `co_yield` 语言机制 + `promise_type` 生命周期控制 + `coroutine_handle` 手动 resume 能力。

没给：**调度器。** 协程在哪个线程上恢复、何时恢复，标准库故意不定义。

这个项目补上的就是调度层——在 `await_suspend` 中拿到 `coroutine_handle<>`，手动决定放进哪个 worker 的队列、由哪个线程 resume。

### 6.2 ScheduleAwaiter — 把协程"挂"到线程池上

```cpp
// ThreadPool.h
class ScheduleAwaiter {
    bool await_ready() const noexcept { return false; }  // 总是挂起

    void await_suspend(std::coroutine_handle<> handle) {
        pool_->enqueue_coroutine_resume(handle, false);  // false = round-robin
    }

    void await_resume() const noexcept {}  // 恢复后无操作
};
```

`await_ready()` 返回 `false` 意味着协程一定会挂起——即使调用方本身就在 pool 的 worker 上也不例外。这是刻意设计，保证公平：走一遍完整的"入队 → 被取 → 执行"流程。

### 6.3 YieldAwaiter — 协作式让出

```cpp
class YieldAwaiter {
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        pool_->enqueue_coroutine_resume(handle, true);   // true = 亲和性
    }
};
```

和 ScheduleAwaiter 的唯一区别：`prefer_current_worker` 传 `true`。

### 6.4 enqueue_coroutine_resume — 亲和性判断

```cpp
// ThreadPoolRuntimeImpl.h
void enqueue_coroutine_resume(std::coroutine_handle<> handle, bool prefer_current_worker) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) throw std::runtime_error("...");
        if (!coroutine_enabled()) throw std::runtime_error("...");

        std::size_t target = 0;
        if (prefer_current_worker && current_runtime() == this
            && current_worker_id() < workers_.max_workers()) {
            target = current_worker_id();  // 亲和路径
        }

        scheduler_.push_coroutine(handle,
            prefer_current_worker && current_runtime() == this, target);
        maybe_grow_unlocked();
    }
    work_cv_.notify_all();  // 用 notify_all，不单 notify_one
}
```

`push_coroutine` 内部：

```cpp
// TaskScheduler.h
void push_coroutine(handle, prefer_worker, preferred_worker) {
    size_t target = 0;
    if (prefer_worker && preferred_worker < coroutine_queues_.size()) {
        target = preferred_worker;       // 走亲和路径
    } else {
        std::unique_lock lock(mutex_);
        target = next_coroutine_worker_; // round-robin
        next_coroutine_worker_ = (target + 1) % coroutine_queues_.size();
    }

    coroutine_queues_[target]->push([handle] {
        handle.resume();  // 包装成 std::function<void()>
    });
}
```

### 6.5 协程帧的生命周期

调用一个协程函数时：

```
1. 编译器 new 协程帧（堆上）：
   ├─ promise_type 对象
   ├─ 函数参数
   ├─ 局部变量
   └─ 状态机（记录当前在哪个挂起点）

2. promise_type::get_return_object() → CoroutineTask

3. initial_suspend() → suspend_never → 直接执行函数体

4. 遇到 co_await → 挂起 → 协程帧保留在堆上

5. Worker 执行 handle.resume() → 从挂起点下一行恢复

6. 函数体结束 → return_void() → final_suspend() → 编译器 delete 协程帧
```

---

## 7. Work Stealing 和防饥饿

### 7.1 为什么需要 steal

普通任务只有一个全局队列，所有 worker 公平竞争，不存在"这个 worker 有活那个 worker 闲"的问题。但协程队列是 per-worker 的：

```
Worker 0: 协程队列空           ← 闲
Worker 1: 协程队列 [A, B, C]   ← A 在跑，B/C 排队等
```

Worker 0 闲下来了、全局队列也空——但 Worker 1 的队列里还有活。不让 Worker 0 去帮忙就是浪费。

### 7.2 steal_coroutine 实现

```cpp
// TaskScheduler.h
bool steal_coroutine(worker_id, task) {
    if (!coroutine_enabled() || coroutine_queues_.empty()) return false;

    for (size_t offset = 1; offset < coroutine_queues_.size(); ++offset) {
        size_t victim = (worker_id + offset) % coroutine_queues_.size();
        if (coroutine_queues_[victim]->pop(task))
            return true;
    }
    return false;
}
```

从自己 + 1 开始线性搜索第一个有活的受害者，取一个就走。不是分一半——保持实现简单。

### 7.3 burst_limit 防普通任务饥饿

没有 burst_limit 时：worker 永远优先取自己的协程队列。如果协程频繁 yield → 重新入队 → worker 立刻又取到协程 → 普通任务饿死。

burst_limit = 8 的含义：连续执行 8 个协程恢复后，**强制在下一次循环的第一步检查全局普通任务**。如果普通任务存在，优先执行并重置 burst。

---

## 8. 协作式取消：StopSource / StopToken

### 8.1 实现

```cpp
// ThreadPoolStopToken.h
class StopSource {
    std::shared_ptr<std::atomic<bool>> stop_requested_;  // 默认 false

    StopToken token() const { return StopToken(stop_requested_); }
    void request_stop() const noexcept {
        stop_requested_->store(true, std::memory_order_release);
    }
};

class StopToken {
    std::shared_ptr<std::atomic<bool>> stop_requested_;

    bool stop_requested() const noexcept {
        return stop_requested_->load(std::memory_order_acquire);
    }
};
```

Source 和 Token 共享一个 `shared_ptr<atomic<bool>>`：
- `shared_ptr` 保证 token 不悬空（source 先析构也能安全读取）
- `atomic<bool>` 保证读写安全
- `release` / `acquire` 配对保证可见性：request_stop 之前的修改在 token 读到 true 时必定可见

### 8.2 使用模式

```cpp
StopSource source;
auto future = pool.submit_with_stop(source.token(), [](StopToken token) {
    while (!token.stop_requested()) {
        do_work_chunk();  // 小粒度工作单元
    }
    // 安全点退出
});
source.request_stop();
```

不强制杀线程，因为 C++ 杀线程 = 锁不释放 + 析构不执行 + 共享状态损坏。

---

## 9. 两种运行模式

### 9.1 ThreadOnly（默认）

只创建全局普通任务队列。协程队列数组仍然分配（为了统一数据结构），但 `schedule()` / `yield()` 直接抛异常。

### 9.2 ThreadAndCoroutine

完整功能：全局任务队列 + per-worker 协程队列 + work stealing + burst_limit。

构造时显式指定：

```cpp
ThreadPool pool(4, ThreadPool::ExecutionMode::ThreadAndCoroutine);
// 或通过 Options
Options opt;
opt.execution_mode = ExecutionMode::ThreadAndCoroutine;
ThreadPool pool(opt);
```

---

## 10. C++ 版本兼容

| 能力 | C++14 | C++17+ |
|------|-------|--------|
| 返回类型推导 | `std::result_of<F(Args...)>::type` | `std::invoke_result_t<F, Args...>` |
| 参数展开 | `index_sequence` 手动 | `std::apply` |
| 协程 | 整个路径被 `#if` 剔除 | `co_await` 全部可用 |

关键宏检测：

```cpp
// 协程能力
#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#define THREADPOOL_HAS_COROUTINE 1
#endif

// C++17 能力
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
    std::invoke_result_t / std::apply
#endif
```

`std::result_of` 在 C++20 被移除，MSVC C++20 模式不再提供——这是早期 CI 在 Windows 上失败的直接原因。

---

## 11. 完整执行时序图

```
调用方线程                       Worker 线程                   协程
    │                               │                           │
    │ pool.submit(f)                │                           │
    │   ├─ bind / packaged_task    │                           │
    │   ├─ future ◄── 返回          │                           │
    │   ├─ push_task(task)          │                           │
    │   └─ notify_one() ──────────►│ 醒来                      │
    │                               ├─ lock(mutex_)            │
    │                               ├─ pop_for_worker()        │
    │                               ├─ unlock                  │
    │                               ├─ mark_started()          │
    │                               ├─ task()                  │
    │                               │   └─ (*packaged)()       │
    │                               │       └─ f(args) → 42   │
    │                               ├─ mark_finished()         │
    │                               └─ 继续下一轮循环           │
    │ future.get() → 42             │                           │
    │                               │                           │
    │                               │              co_await pool.schedule()
    │                               │                ├─ await_ready → false
    │                               │                ├─ await_suspend(h)
    │                               │                │    └─ push_coroutine(h)
    │                               │                │        入 Worker 0 协程队列
    │                               │                └─ 挂起 ◄──────────┘
    │                               │
    │                               ├─ pop 协程队列
    │                               ├─ h.resume() ────────────────► 恢复
    │                               │                                  │
    │                               │                    co_await pool.yield()
    │                               │                      ├─ await_suspend(h)
    │                               │                      │    └─ prefer=true
    │                               │                      │       入本 worker 队列
    │                               │                      └─ 再次挂起
    │                               │
    │                               ├─ pop 协程队列
    │                               ├─ h.resume() ────────────────► 恢复
    │                               │                                  │
    │                               │                          函数结束
    │                               │                          promise.set_value()
    │                               │                          delete 协程帧
```

---

## 12. 关键设计取舍总结

| 决定 | 选择 | 原因 |
|------|------|------|
| 队列存储类型 | `std::function<void()>` | 类型统一，worker 实现简单 |
| 任务包装 | `std::packaged_task` | 自动处理返回值 + 异常传播 |
| 协程队列分离 | per-worker 本地队列 | 支持亲和性 + work stealing |
| 协程防饥饿 | burst_limit = 8 | 简单有效，可配置 |
| 取消方式 | 协作式 | C++ 不能安全强制终止线程 |
| 锁策略 | 分层各自管理 | 避免大锁瓶颈 |
| 通知方式 | submit 用 notify_one，shutdown 用 notify_all | 精准唤醒 vs 全员通知 |
| 协程入队通知 | notify_all（不用 notify_one） | yield 有亲和目标，all 让 steal 有机会 |
| 构造启动 | 自动 | 防止忘记 init 导致永久阻塞 |

---

## 13. 数据结构全景

```
ThreadPool (栈上对象, 不可拷贝/移动)
  └─ ThreadPoolRuntime runtime_
       ├─ bool shutdown_ = false
       ├─ bool started_ = false
       ├─ ExecutionMode execution_mode_
       ├─ std::mutex mutex_
       ├─ std::condition_variable work_cv_
       │
       ├─ TaskScheduler scheduler_
       │    ├─ SafeQueue<Task> task_queue_          ← 全局普通任务
       │    ├─ vector<SafeQueue<Task>> coroutine_queues_ ← per-worker 协程
       │    ├─ size_t active_tasks_
       │    ├─ size_t next_coroutine_worker_
       │    ├─ size_t coroutine_burst_limit_
       │    ├─ std::mutex mutex_
       │    └─ std::condition_variable idle_cv_
       │
       └─ WorkerGroup workers_
            ├─ size_t min_workers_ / max_workers_
            ├─ chrono::milliseconds idle_timeout_
            ├─ atomic<size_t> live_workers_
            ├─ atomic<size_t> next_worker_id_
            ├─ vector<std::thread> workers_
            ├─ vector<thread::id> retired_worker_ids_
            └─ std::mutex mutex_
```
