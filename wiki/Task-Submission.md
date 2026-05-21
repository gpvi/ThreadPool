# Task Submission

本文深入解析 `submit()` 从调用到执行的完整流程。

## 类型擦除链路

用户提交的任务需要从"可调用对象 + 参数"转换为统一的 `std::function<void()>`：

```
f(args...)                          用户层：任意可调用对象
    │
    ▼
lambda: [f, args...]() {            Lambda 层：捕获所有参数
    return f(args...);
}
    │
    ▼
packaged_task<R()>(lambda)            packaged_task 层：管理返回值传播
    │                                    - 通过 shared_ptr 共享所有权
    │                                    - get_future() 返回 future
    ▼
std::function<void()>                 function 层：类型擦除
    = [task_ptr]() {                     - 统一的 void() 签名
          (*task_ptr)();                 - 入队到 SafeQueue<Task>
      }
```

### 为什么用 `shared_ptr<packaged_task>`？

`std::packaged_task` 不可拷贝，只能移动。而 `std::function` 要求可拷贝。解决方案：将 `packaged_task` 放入 `shared_ptr`，lambda 捕获 `shared_ptr`。

```cpp
// 简化的 submit 实现
template<typename F, typename... Args>
auto submit(F&& f, Args&&... args) {
    // 1. 创建 packaged_task
    auto task = std::make_shared<std::packaged_task<R()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() {
            return f(args...);  // C++17: std::apply
        }
    );

    // 2. 提取 future
    auto future = task->get_future();

    // 3. 包装成 function<void()>
    std::function<void()> wrapper = [task]() {
        (*task)();  // 执行 → 设置 promise → future 就绪
    };

    // 4. 入队
    runtime_.submit_task(std::move(wrapper));

    return future;
}
```

### C++14 兼容性

C++17 使用 `std::apply` 展开 tuple 参数：

```cpp
[f = std::forward<F>(f), t = std::make_tuple(std::forward<Args>(args)...)]() {
    return std::apply(f, t);
}
```

C++14 回退到 `index_sequence` 手动展开（`call_impl` 方法）。

### 返回值类型推导

使用 `detail::invoke_result_t`：

```cpp
// C++17+: std::invoke_result_t<F, Args...>
// C++14:  typename std::result_of<F(Args...)>::type
using ReturnType = detail::invoke_result_t<F, Args...>;
```

---

## submit() 完整流程

```
用户调用 pool.submit(f, args...)
    │
    ▼
ThreadPool::submit(F&& f, Args&&... args)
    │
    ├─ 1. using R = invoke_result_t<F, Args...>;
    │
    ├─ 2. auto task = make_shared<packaged_task<R()>>(lambda)
    │      lambda 捕获 f 和 args，调用 f(args...)
    │
    ├─ 3. auto future = task->get_future();
    │
    ├─ 4. function<void()> wrapper = [task] { (*task)(); }
    │
    └─ 5. runtime_.submit_task(std::move(wrapper))
              │
              ▼
        ThreadPoolRuntime::submit_task(Task task)
              │
              ├─ lock(mutex_)
              ├─ if (shutdown_) throw runtime_error("Pool is shut down")
              ├─ unlock(mutex_)
              │
              ├─ scheduler_.push_task(std::move(task))
              │        │
              │        ▼
              │   SafeQueue<Task>::push(task)
              │        lock → queue.push → unlock
              │
              ├─ maybe_grow_unlocked()
              │        │
              │        ▼
              │   WorkerGroup::maybe_grow(queued_tasks, live_workers)
              │        如果 queued > live 且 live < max
              │        → spawn() 创建新 worker 线程
              │
              └─ work_cv_.notify_one()
                      唤醒一个等待中的 worker
```

---

## submit_with_stop() 流程

与 `submit()` 类似，但在参数前插入 `StopToken`：

```cpp
template<typename F, typename... Args>
auto submit_with_stop(StopToken token, F&& f, Args&&... args) {
    // 等价于 submit(f, token, args...)
    // token 作为第一个参数传给 f
    auto callable = [f = std::forward<F>(f),
                     token,
                     ...args = std::forward<Args>(args)]() {
        return f(token, args...);
    };
    return submit_internal(std::move(callable));
}
```

用户任务函数签名：`R f(StopToken st, Args... args)`

---

## Worker 如何取任务

worker 主循环通过 `pop_for_worker()` 获取下一个任务：

```
Worker::operator()()
    │
    ▼
wait_for_work()                   ← 条件变量等待
    │
    ├─ 检查 shutdown → exit=true
    ├─ 检查超时 → 退休
    └─ 检查有工作 → 调用 pop
            │
            ▼
    TaskScheduler::pop_for_worker(worker_id)
            │
            ├─ 1. burst≥8? → 全局队列优先
            ├─ 2. 自己的协程队列
            ├─ 3. 全局任务队列
            └─ 4. 窃取其他 worker 的协程队列
            │
            ▼
        返回 WorkerWork{task, popped_coroutine, ...}
            │
            ▼
    execute(task)                 ← try/catch 包裹
            │
            ▼
    finish_work(popped_coroutine) ← 更新 burst 计数
```

---

## 异常传播

- `packaged_task` 内部捕获异常并存入 `std::promise`
- 用户调用 `future.get()` 时，异常被重新抛出
- Worker 的 `operator()` 额外包了一层 `try/catch`，防止非 `packaged_task` 异常导致线程死亡

```cpp
// ThreadPoolWorker.h
try {
    work.task();
} catch (...) {
    // 非 packaged_task 抛出的异常（理论上不应发生）
    // 吞掉异常，保护 worker 线程不退出
}
```

**注意**：`std::packaged_task` 会正确传播异常到 future。外层的 `try/catch` 是防御性的——但注释指出它可能导致 "exception silently swallowed" 的问题，因为 `packaged_task` 本身不应该在 `operator()` 之外抛出异常。

---

## 关键设计决策

| 决策 | 原因 |
|------|------|
| 使用 `packaged_task` 而非手动 `promise` | 更安全，自动异常传播 |
| `shared_ptr<packaged_task>` | `function` 要求可拷贝，`packaged_task` 不可拷贝 |
| task 先入队再 notify | 保证 worker 被唤醒时任务已在队列中 |
| notify 在锁外 | 避免被唤醒的 worker 立即阻塞在锁上 |
| 提交后检查 shutdown | 保证关闭后的提交被拒绝 |
