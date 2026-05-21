# Configuration

ThreadPool 的配置项、枚举类型和调优指南。

## ThreadPoolOptions 完整参考

```cpp
struct ThreadPoolOptions {
    size_t min_workers           = 4;
    size_t max_workers           = 4;
    ExecutionMode execution_mode = ExecutionMode::ThreadOnly;
    size_t coroutine_burst_limit = 8;
    std::chrono::milliseconds idle_timeout{30000};
};
```

---

## 配置项详解

### min_workers / max_workers

控制线程池的**常驻**和**上限** worker 数量。

| 配置 | 行为 |
|------|------|
| `min == max` | 固定大小池，不扩缩 |
| `min < max` | 动态池，按需扩容，闲置缩容到 min |
| `min == 0` | 非法，抛出 `std::invalid_argument` |
| `max < min` | 非法，抛出 `std::invalid_argument` |

**建议**：
- CPU 密集型任务：`min = max = std::thread::hardware_concurrency()`
- IO 密集型任务：可适当增大 `max`，例如 `max = 2 * hw`
- 突发流量：设置较大的 `max`，配合短 `idle_timeout` 快速缩容

### execution_mode

```cpp
enum class ExecutionMode {
    ThreadOnly,         // 仅线程（默认）
    ThreadAndCoroutine  // 线程 + 协程
};
```

| 模式 | submit | submit_with_stop | schedule | yield | C++ 版本 |
|------|--------|------------------|----------|-------|----------|
| `ThreadOnly` | ✓ | ✓ | ✗ (throw) | ✗ (throw) | C++14 |
| `ThreadAndCoroutine` | ✓ | ✓ | ✓ | ✓ | C++20 |

> 在 `ThreadOnly` 模式下调用 `schedule()` / `yield()` 会抛出 `std::runtime_error`。

### coroutine_burst_limit

**默认**：8

连续执行协程的最大数量。超过此限制后，worker 会优先处理普通任务，防止协程饥饿普通任务。

**调优**：
- **增大** → 协程获得更多连续执行时间，减少上下文切换
- **减小** → 普通任务获得更公平的调度机会
- **设为 0** → 行为未定义（应保持 ≥ 1）

### idle_timeout

**默认**：30 秒（30000ms）

Worker 空闲超时时间。仅在 `min_workers < max_workers` 时生效。

- Worker 在 `wait_for_work()` 中调用 `work_cv_.wait_for(idle_timeout)`
- 超时后，如果 `live_workers() > min_workers()`，worker 退休

---

## 构造函数选择

### 简单构造

```cpp
// 固定 4 个 worker，ThreadOnly 模式
ThreadPool pool(4);

// 固定 N 个 worker，指定模式
ThreadPool pool(8, ThreadPool::ExecutionMode::ThreadAndCoroutine);
```

### 完整配置

```cpp
ThreadPool::Options opts;
opts.min_workers = 4;
opts.max_workers = 8;
opts.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
opts.coroutine_burst_limit = 16;
opts.idle_timeout = std::chrono::seconds(10);

ThreadPool pool(opts);
```

### 使用工厂方法

```cpp
auto opts = ThreadPool::make_fixed_options(4, ThreadPool::ExecutionMode::ThreadOnly);
ThreadPool pool(opts);
```

---

## 常用配置场景

### 场景 1：固定大小 CPU 密集型线程池

```cpp
ThreadPool::Options opts;
opts.min_workers = std::thread::hardware_concurrency();
opts.max_workers = opts.min_workers;  // 固定
opts.execution_mode = ThreadPool::ExecutionMode::ThreadOnly;
ThreadPool pool(opts);
```

### 场景 2：IO 密集型弹性线程池

```cpp
ThreadPool::Options opts;
opts.min_workers = 2;                          // 低水位
opts.max_workers = 4 * std::thread::hardware_concurrency();  // 高水位
opts.idle_timeout = std::chrono::seconds(5);    // 快速缩容
ThreadPool pool(opts);
```

### 场景 3：协程密集型线程池

```cpp
ThreadPool::Options opts;
opts.min_workers = std::thread::hardware_concurrency();
opts.max_workers = std::thread::hardware_concurrency();  // 协程不增线程
opts.execution_mode = ThreadPool::ExecutionMode::ThreadAndCoroutine;
opts.coroutine_burst_limit = 4;                 // 更多任务切换
ThreadPool pool(opts);
```

### 场景 4：高吞吐批处理

```cpp
ThreadPool::Options opts;
opts.min_workers = std::thread::hardware_concurrency();
opts.max_workers = opts.min_workers;
opts.coroutine_burst_limit = 32;  // 减少协程/任务切换开销
ThreadPool pool(opts);
```

---

## 可观测性（Observability）

运行时状态查询接口：

```cpp
pool.queued_tasks();       // 全局队列等待数
pool.queued_coroutines();  // 协程队列等待数
pool.active_tasks();       // 正在执行的数量（任务 + 协程）
pool.worker_count();       // 当前活跃 worker 数
pool.is_shutdown();        // 是否已关闭
pool.min_workers();        // 配置的最小值
pool.max_workers();        // 配置的最大值
```

**典型监控模式**：

```cpp
// 周期性打印状态
std::thread monitor([&pool]() {
    while (!pool.is_shutdown()) {
        std::cout << "queued: " << pool.queued_tasks()
                  << " active: " << pool.active_tasks()
                  << " workers: " << pool.worker_count() << '\n';
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
});
```

---

## 性能调优 checklist

| 检查项 | 调优方向 |
|--------|---------|
| 任务执行时间短（微秒级） | 减少 worker 数，减少上下文切换 |
| 任务执行时间长（秒级） | 增加 worker 数，提高并行度 |
| 大量协程 yield | 增大 `coroutine_burst_limit`，减少切换 |
| 混合任务+协程，任务饥饿 | 减小 `coroutine_burst_limit` |
| worker 频繁扩缩 | 增大 `idle_timeout` |
| 内存敏感 | 减小 `max_workers`，设置 `min == max` |
| 延迟敏感 | 增大 `max_workers`，减小 `idle_timeout` |
