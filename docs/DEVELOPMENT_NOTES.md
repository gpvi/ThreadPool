# 开发问题集合与解决策略

本文档记录项目开发和工程化过程中遇到的问题、原因分析与解决策略。内容偏实践复盘，适合后续维护、排查 CI 问题，以及准备项目面试讲解。

## 1. 线程池关闭时可能丢失任务

### 问题

早期实现中，`shutdown()` 直接设置关闭标志，worker 在线程循环中发现关闭后退出。这样可能导致队列中仍有未执行任务，但 worker 已经停止。

### 原因

关闭状态和任务队列状态没有统一判断。worker 只关心线程池是否关闭，没有区分：

- 线程池已经关闭但队列仍有任务。
- 线程池已经关闭且队列为空。

### 解决策略

将关闭语义拆成两种模式：

- `Drain`：停止接收新任务，但继续执行完已提交任务。
- `CancelPending`：停止接收新任务，并清空尚未开始执行的排队任务。

worker 退出条件改为：

```text
线程池已关闭 && 任务队列为空
```

这样默认关闭流程不会丢失已提交任务。

## 2. 条件变量等待存在虚假唤醒和通知丢失风险

### 问题

早期 worker 等待任务时使用普通 `wait()`，没有 predicate。线程可能被虚假唤醒，也可能在任务入队和线程进入等待之间出现通知时序问题。

### 原因

条件变量只负责阻塞和唤醒，不保存“条件是否满足”的状态。真正可靠的条件必须由共享状态表达。

### 解决策略

改为使用带 predicate 的等待：

```cpp
condition.wait(lock, [] {
	return shutdown || !queue.empty();
});
```

这样 worker 被唤醒后会重新检查条件，避免虚假唤醒导致空转，也避免单纯依赖通知时序。

## 3. 忘记启动线程池会导致 future 永久等待

### 问题

早期版本需要手动调用 `init()`。如果调用方创建线程池后直接提交任务，任务会进入队列，但没有 worker 执行，`future.get()` 会一直等待。

### 原因

线程池对象构造完成后并不处于可运行状态，接口容易误用。

### 解决策略

构造函数自动启动 worker。保留 `init()` 作为兼容接口，并让它幂等，多次调用不会重复创建线程。

## 4. 析构时未关闭线程导致程序异常终止

### 问题

如果调用方忘记调用 `shutdown()`，对象析构时仍存在 joinable 线程，可能触发 `std::terminate`。

### 原因

`std::thread` 析构时如果仍处于 joinable 状态，标准库会终止程序。

### 解决策略

在线程池析构函数中自动调用 `shutdown()`，并将析构函数设计为 `noexcept`。这样可以降低调用方误用成本，保证资源能够被回收。

## 5. 示例代码中的随机数存在数据竞争

### 问题

示例程序中多个 worker 线程共享同一个随机数生成器，存在数据竞争。

### 原因

标准随机数引擎不是线程安全对象。多个线程并发调用同一个引擎会导致未定义行为。

### 解决策略

将随机数引擎改为 `thread_local`，让每个 worker 线程拥有自己的随机数状态。

## 6. 零 worker 线程会导致任务永远无法执行

### 问题

如果构造线程池时传入 worker 数量为 0，任务可以提交到队列，但永远没有线程取出执行。

### 原因

构造参数缺少合法性检查。

### 解决策略

构造函数中检查 worker 数量。如果为 0，直接抛出 `std::invalid_argument`，把错误暴露在创建阶段。

## 7. 关闭后继续提交任务的行为不明确

### 问题

线程池关闭后如果继续提交任务，任务可能进入队列但永远不会执行。

### 原因

任务提交时没有检查线程池是否已经进入关闭状态。

### 解决策略

在任务入队前加锁检查关闭状态。如果已经关闭，直接抛出 `std::runtime_error`。

## 8. 缺少空闲等待能力，测试难以稳定

### 问题

只依赖 `future.get()` 或 sleep 很难判断线程池是否真的处理完所有排队任务。

### 原因

线程池内部没有暴露队列状态和正在执行任务数量。

### 解决策略

增加状态计数：

- 排队任务数。
- 正在执行任务数。

并增加：

- `wait_idle()`
- `wait_idle_for()`
- `wait_idle_until()`

空闲条件定义为：

```text
队列为空 && 正在执行任务数为 0
```

## 9. 强制终止线程不安全

### 问题

长耗时任务需要取消，但 C++ 中强制杀死线程会带来资源一致性问题。

### 原因

强制终止线程可能导致：

- 锁没有释放。
- 对象析构没有执行。
- 共享状态只更新了一半。
- 文件、网络连接等资源未正确收尾。

### 解决策略

实现协作式取消：

- `StopSource` 发出取消请求。
- `StopToken` 传入任务。
- 任务定期检查取消状态，并在安全点主动退出。

这不能强制停止任务，但符合 C++ 资源管理习惯。

## 10. C++20 协程没有内置线程池调度器

### 问题

引入协程后，不能只依赖 C++ 语言本身完成线程切换。

### 原因

C++20 协程只提供挂起和恢复机制，不提供事件循环、线程池或运行时调度器。

### 解决策略

实现 `schedule()` awaiter：

1. 协程在 `co_await` 处挂起。
2. awaiter 获取 `std::coroutine_handle<>`。
3. 将 `handle.resume()` 封装成线程池任务。
4. worker 执行该任务，协程在线程池线程中恢复。

该方案是轻量调度入口，不是完整 `Task<T>` 协程框架。

## 11. 单个 worker 上如何承载多个协程

### 问题

如果协程切换到线程池后一直执行，也会像普通任务一样占用 worker。这样并不能体现“一个线程承载多个协程”的优势。

### 原因

C++ 协程不是抢占式调度。协程只有遇到 `co_await` 并主动挂起时，调度器才有机会恢复其他协程。

### 解决策略

增加 `yield()` awaiter：

```cpp
co_await pool.yield();
```

语义：

1. 当前协程主动挂起。
2. 将当前协程的恢复动作重新提交到线程池队列。
3. 当前 worker 可以继续执行队列中的其他任务或协程。
4. 之后某个 worker 再恢复该协程。

这使线程池具备基础的协作式协程轮转能力，但仍不是完整协程运行时。

## 12. 协程恢复任务不应全部混在全局队列里

### 问题

最初 `schedule()` 和 `yield()` 都是把协程恢复动作封装成普通任务，直接提交到全局任务队列。这样虽然实现简单，但不符合“每个 worker 维护自己的协程队列”的模型。

### 原因

普通任务和协程恢复任务混在一个队列中，会导致：

- 无法表达协程与 worker 的亲和性。
- `yield()` 后不一定回到当前 worker。
- 后续很难扩展本地调度或 work stealing。

### 解决策略

为每个 worker 增加本地协程队列：

```text
Worker 0 -> coroutine queue 0
Worker 1 -> coroutine queue 1
```

调度策略：

- 普通任务仍进入全局任务队列。
- `schedule()` 轮询选择 worker，并把协程恢复动作放入该 worker 的本地协程队列。
- `yield()` 优先把当前协程放回当前 worker 的本地协程队列。
- worker 优先执行自己的协程队列，再执行全局普通任务队列。

这样可以更清楚地表达“一个 worker 承载多个协程”的运行模型。

## 13. 快速关闭时不能直接丢弃已挂起协程

### 问题

`CancelPending` 如果直接清空 worker 本地协程队列，已经挂起的协程可能永远不会恢复，对应的等待方可能一直阻塞。

### 原因

协程挂起后，恢复动作保存在队列中的 `resume()` 任务里。直接丢弃该任务会让协程没有机会继续执行、传播异常或完成 promise。

### 解决策略

`CancelPending` 只清空普通任务队列，不直接清空 worker 本地协程队列。已经挂起的协程会被恢复一次，后续如果继续调度，由于线程池已经关闭，会通过异常路径结束。

## 14. 本地协程队列可能导致普通任务饥饿

### 问题

如果 worker 永远优先执行本地协程队列，而协程又频繁 `yield()` 重新入队，普通任务可能长时间得不到执行。

### 原因

本地协程队列优先级过高，缺少公平性控制。

### 解决策略

增加连续协程执行配额。worker 连续执行一定数量的协程恢复任务后，会优先检查一次全局普通任务队列。如果普通任务存在，就先执行普通任务，并重置配额。

## 15. 本地协程队列可能造成 worker 负载不均

### 问题

某些 worker 的协程队列可能很满，而其他 worker 空闲。

### 原因

`schedule()` 只负责初始分配，`yield()` 会优先回到当前 worker，本地队列天然可能产生负载偏斜。

### 解决策略

增加基础 work stealing：当 worker 自己的协程队列为空、全局普通任务队列也为空时，会尝试从其他 worker 的协程队列偷取恢复任务执行。

## 16. 协程能力需要显式启用

### 问题

线程池同时支持普通任务和协程调度。如果所有实例默认都开放协程接口，调用方容易忽略运行模式差异，也不利于表达“纯线程池”和“线程 + 协程调度器”两种不同用途。

### 原因

普通线程池只需要执行 `submit()` 提交的任务；协程模式还需要处理 `schedule()` 和 `yield()` 这类协程恢复动作。两者可以共用底层 worker 和任务队列，但语义不同。

### 解决策略

增加运行模式：

- `ThreadOnly`：默认模式，只运行普通任务。
- `ThreadAndCoroutine`：启用协程调度和协作式让出。

构造时显式选择：

```cpp
threadpool::ThreadPool pool(
	4,
	threadpool::ThreadPool::ExecutionMode::ThreadAndCoroutine
);
```

也可以使用 `Options` 统一传递启动参数：

```cpp
threadpool::ThreadPool::Options options;
options.worker_count = 4;
options.execution_mode = threadpool::ThreadPool::ExecutionMode::ThreadAndCoroutine;

threadpool::ThreadPool pool(options);
```

如果在纯线程模式下调用协程调度接口，直接抛出异常，尽早暴露配置错误。

## 17. `std::result_of` 在 MSVC/C++20 下不可用

### 问题

GitHub Actions Windows 构建协程测试时失败，错误提示 `result_of` 不是 `std` 成员。

### 原因

`std::result_of` 在 C++17 被弃用，在 C++20 中被移除。MSVC 在 C++20 模式下不再提供该类型。

### 解决策略

增加兼容类型萃取：

- C++17 及以上使用 `std::invoke_result_t`。
- C++14 使用 `std::result_of`。

这样既能保持 C++14 基础接口，又能让 C++20 协程目标通过 MSVC 编译。

## 18. `std::chrono::duration::rep` 在不同平台类型不同

### 问题

GitHub Actions Linux 构建 benchmark 时失败，`std::future<long>` 无法放入 `std::vector<std::future<long long>>`。

### 原因

`std::chrono::microseconds::count()` 的返回类型由标准库实现决定。Windows 和 Linux 上可能分别是 `long long` 和 `long`。

### 解决策略

不要手写 `long long`，改用标准库暴露的实际表示类型：

```cpp
Microseconds::rep
```

这样 benchmark 在不同平台上都能匹配对应的 future 类型。

## 19. benchmark 不应作为默认功能测试

### 问题

benchmark 输出受硬件、系统负载、编译器、后台进程影响。如果把性能结果作为 CI 通过条件，容易产生不稳定失败。

### 原因

性能测试和功能正确性测试目标不同：

- 功能测试验证行为是否正确。
- benchmark 观察性能趋势。

### 解决策略

CMake 构建 benchmark 目标，但不把 benchmark 加入默认 CTest。默认 CTest 只运行：

- 行为测试。
- 协程测试。
- 轻量压力测试。

benchmark 保留为人工运行工具。

## 20. 极短任务下 worker 越多不一定越快

### 问题

benchmark 中发现，极短任务场景下，增加 worker 数量不一定提升吞吐。

### 原因

任务本身太短时，开销主要来自：

- 任务封装。
- 共享队列加锁。
- 条件变量唤醒。
- 线程调度。
- future 同步。

并行收益可能被调度成本抵消。

### 解决策略

在文档中明确 benchmark 结果解释：线程池适合任务粒度较合理的并发任务，不适合把大量极短空任务作为唯一性能指标。

后续如果继续优化，可以考虑：

- 批量提交。
- 每 worker 本地队列。
- work stealing。
- 减少任务封装开销。

## 21. CMake 与手动编译需要同时维护

### 问题

项目既支持 CMake，又在文档中保留 g++ 手动编译命令。如果新增目标后只更新一处，文档容易过期。

### 原因

构建入口多时，维护成本增加。

### 解决策略

README 中优先推荐 CMake，手动 g++ 命令作为补充。新增 `stress_test.cpp`、`benchmark.cpp`、`test_coroutine.cpp` 后同步更新 CMake 和 README。

## 22. 构建产物容易误加入 Git

### 问题

本地编译后会生成 `build/` 和多个 `.exe` 文件，容易被误提交。

### 原因

早期 `.gitignore` 规则过少，只忽略了少量文件。

### 解决策略

扩展 `.gitignore`，覆盖：

- CMake 构建目录。
- 编译目标文件。
- 可执行文件和库文件。
- 调试文件。
- IDE 缓存。
- 日志和用户本地配置。

## 23. GitHub Actions 需要覆盖不同平台

### 问题

只在本地 Windows 验证无法发现 Linux 和 MSVC/C++20 的差异。

### 原因

不同平台和编译器在类型定义、标准库实现和语言特性支持上存在差异。

### 解决策略

增加 GitHub Actions：

- `ubuntu-latest`
- `windows-latest`

CI 执行：

```text
cmake configure
cmake build
ctest
```

实际 CI 暴露了两个本地未发现的问题：

- Linux 上 `microseconds::rep` 类型差异。
- MSVC/C++20 下 `std::result_of` 不可用。
- MSVC 下不支持 GCC 专属头文件 `bits/stdc++.h`。

## 24. `bits/stdc++.h` 不具备跨平台可移植性

### 问题

Windows GitHub Actions 使用 MSVC 构建 demo 时失败，提示找不到 `bits/stdc++.h`。

### 原因

`bits/stdc++.h` 是 GCC/MinGW 常见的非标准聚合头文件，不属于 C++ 标准库。MSVC 不提供该头文件。

### 解决策略

将 `main.cpp` 中的非标准头替换为实际使用到的标准头：

```cpp
#include <chrono>
#include <functional>
#include <iostream>
#include <random>
#include <thread>
```

生产代码和示例代码都应优先使用标准头文件，避免只在单一编译器环境下可用。

## 25. 文档需要分层

### 问题

README 内容不断追加后变得像教程笔记，项目首页信息过重。

### 原因

所有说明都放在 README，会让快速上手、设计细节、问题复盘混在一起。

### 解决策略

文档分层：

- `README.md`：项目定位、核心能力、快速开始、基本用法。
- `docs/DESIGN.md`：设计思想、调度策略、生命周期、优缺点。
- `docs/DEVELOPMENT_NOTES.md`：开发问题集合和解决策略。

这样读者可以根据目的选择阅读路径。

## 26. 头文件需要按职责拆分

### 问题

线程池功能不断增加后，`ThreadPool.h` 同时包含配置、取消令牌、类型兼容层、线程池声明、模板提交和 worker 实现，阅读成本变高。

### 原因

项目从单文件教学示例演进到小型库组件后，功能边界变多。如果所有内容继续堆在主入口头文件里，后续维护会变困难。

### 解决策略

保持 `#include "ThreadPool.h"` 作为唯一对外入口，同时把内部职责拆到多个头文件：

```text
ThreadPoolOptions.h    运行模式和启动参数
ThreadPoolStopToken.h  协作式取消
ThreadPoolTypeTraits.h C++ 标准兼容类型萃取
ThreadPoolImpl.h       pool 级调度、扩缩容、协程队列实现
ThreadPoolSubmit.h     submit / submit_with_stop 模板实现
ThreadPoolWorker.h     ThreadPoolWorker 类型、worker 循环和任务执行逻辑
TaskScheduler.h        普通任务、协程队列、work stealing、空闲等待
WorkerGroup.h          worker 生命周期、扩缩容、join 管理
WorkerGroupImpl.h      WorkerGroup 创建 ThreadPoolWorker 的 inline 实现
SafeQueue.h            线程安全队列
```

这样既保持用户使用简单，又让内部实现边界更清晰。

进一步拆分后，`ThreadPoolWorker` 不再只是一个函数实现文件，而是一个独立执行单元类型。`ThreadPool` 创建 worker，worker 自己负责等待、取任务、执行任务和空闲退出。

后续又引入 `TaskScheduler`，把全局任务队列、worker 本地协程队列、active task 计数、空闲等待、协程恢复入队和 work stealing 从 `ThreadPool` 中拿出来。这样 `ThreadPool` 更接近对外 facade，`TaskScheduler` 负责调度策略，`ThreadPoolWorker` 负责执行循环。

再进一步引入 `WorkerGroup`，把 worker 线程数组、live worker 计数、worker id 分配、动态扩容、空闲回收和 join 从 `ThreadPool` 中拿出来。这样线程池架构从“一个大类”变成：

```text
ThreadPool      对外 facade 和生命周期入口
TaskScheduler   任务/协程调度策略
WorkerGroup     worker 生命周期管理
ThreadPoolWorker worker 执行循环
```

## 27. WorkerGroup 创建线程需要异常安全

### 问题

如果先增加 live worker 计数，再创建 `std::thread`，一旦线程创建或 vector 扩容失败，计数会显示有 worker 存活，但实际线程并没有创建成功。

### 原因

线程创建和容器扩容都可能抛异常。资源计数必须和真实资源创建保持一致。

### 解决策略

调整顺序：

```text
先创建并保存 std::thread
创建成功后再增加 live worker 计数
```

这样异常发生时不会留下错误的 worker 计数。

## 28. 空闲 worker 退出后 thread 对象不能长期堆积

### 问题

动态扩缩容后，worker 线程可能因为空闲超时退出。如果只减少 live worker 数量，但不回收对应的 `std::thread` 对象，长期运行后 `workers_` 容器会持续增长。

### 原因

退出的线程对象仍然保存在 `WorkerGroup` 中，需要由其他线程 join 后才能安全移除。worker 线程不能 join 自己。

### 解决策略

worker 退出时记录自己的 `thread::id`，由 `WorkerGroup` 在下次扩容前执行回收：

```text
worker 退出 -> 记录 retired id
下一次 spawn 前 -> join 并 erase retired thread
shutdown -> join_all 清理所有 thread
```

这样既避免 worker 自己 join 自己，也避免历史 thread 对象长期堆积。

## 29. 简历表达需要突出难点而不是堆接口名

### 问题

项目能力很多，如果简历中直接堆函数名和模块名，会显得碎片化。

### 原因

简历更关注问题、方案和工程价值，而不是接口清单。

### 解决策略

表达时突出：

- 并发调度。
- 任务封装。
- 生命周期管理。
- 协作式取消。
- 协程调度。
- 压力测试和 CI 验证。

弱化简单接口名和过多实现细节。

## 30. `reap_retired_workers` 用 thread::id 匹配线程可能导致误回收

### 问题

代码审查发现 `WorkerGroup` 中 `retire_current_worker()` 通过 `std::this_thread::get_id()` 存储当前线程 ID，`reap_retired_workers()` 通过 `worker.get_id()` 匹配。一旦操作系统复用线程 ID，新创建的 worker 可能被误匹配，导致 join 正在运行的线程。

### 原因

`std::thread::id` 在 POSIX 上是 `pthread_t`，理论上线程退出后 ID 可以被操作系统复用。虽然实际概率极低，但作为线程池的基础设施不能依赖概率。

### 解决策略

将 ID 匹配改为索引追踪：

1. `retire_current_worker()` 在 `mutex_` 保护下遍历 `workers_` 找到自己的索引位置，存入 `retired_indices_`。
2. `reap_retired_workers()` 对索引降序排序后从 `workers_` 中 `join` + `erase`，避免索引偏移问题。
3. 用 `std::greater<std::size_t>()` 降序确保从后往前删除，前面索引保持稳定。

## 31. `spawn()` 中 live_workers 递增时序晚于线程启动

### 问题

`spawn()` 流程是 `emplace_back(std::thread(...))` 之后再 `live_workers_.fetch_add(1)`。但 `emplace_back` 在构造 `std::thread` 那一刻新线程就启动了，而 `live_workers_` 此时还是旧值。

### 场景推演

```
ThreadPool 构造 → start(1) → spawn() 创建 Worker-0
emplace_back 完成 → Worker-0 线程开始运行
Worker-0 进入 wait_for_work → should_exit_on_idle() 读到 live_workers_==0
  → 0 > min_workers(1) → false → 不退出，但逻辑建立在不一致的计数上
fetch_add(1) 终于执行 → live_workers_==1
```

更严重场景：shutdown 链路中，`join_all()` swap 走 workers 并 join，但此时 `live_workers_` 可能因尚未执行的 `fetch_add` 而泄漏一个永远不回收的计数。

### 解决策略

将 `live_workers_.fetch_add(1)` 移动到 `emplace_back` 之前：

```cpp
live_workers_.fetch_add(1, relaxed);
try {
    lock(mutex_);
    workers_.emplace_back(ThreadPoolWorker(runtime, worker_id));
} catch (...) {
    live_workers_.fetch_sub(1, relaxed);  // 回滚
    throw;
}
```

关键点：`emplace_back` 可能抛异常（线程创建失败），必须用 try-catch 回滚计数。

## 32. `retire_current_worker()` 中计数更新在锁外

### 问题

`decrease_live_worker()`（原子 fetch_sub）在 `WorkerGroup::mutex_` 之外执行。并发 `spawn()` → `reap_retired_workers()` 在 `decrease` 之后、索引记录之前执行时，会错过该 retired worker 的 join 时机。

### 解决策略

将 `decrease_live_worker()` 移入 `mutex_` 内。这样计数减少与索引记录在同一临界区，`reap_retired_workers()` 不会漏掉。

## 33. 多个 worker 可同时退休突破 min_workers 下限

### 问题

`should_exit_on_idle()` 检查 `live_workers() > min_workers_`，然后单独调用 `decrease_live_worker()`。两者不是原子操作。三个 worker 同时超时，可能全部读到 `3 > 1 = true`，全部退休，导致 `live_workers_` 归零。

### 根本原因

检查条件（read）和状态变更（write）之间存在 TOCTOU 窗口。标准条件变量 wait_for 超时后不保证只有一个线程看到超时——所有 worker 可以同时被唤醒并同时检查条件。

### 解决策略

引入 CAS 循环 `try_retire()`：

```cpp
bool try_retire() {
    while (true) {
        auto current = live_workers_.load(relaxed);
        if (current <= min_workers_) return false;   // 不能退
        if (live_workers_.compare_exchange_weak(current, current - 1, relaxed))
            return true;  // 原子减 1
    }
}
```

Idle timeout 路径改用 `try_retire()`，shutdown 路径保持无条件 `decrease_live_worker()`（shutdown 时所有 worker 必须退出）。

## 34. `wait_idle` 与 pop/mark_started 之间的 TOCTOU 竞态

### 问题

`wait_idle` 的 predicate（`task_queue_.empty() && active_tasks_ == 0`）和 worker 的 pop+mark_started 操作之间存在竞态：

```
Worker: pop_for_worker → task_queue.pop() → 队列变空
Main:   wait_idle 检查 predicate → empty=true, active=0 → 误判空闲
Worker: mark_started → active++（太晚了）
```

ASan 构建下 worker 线程变慢，这个窗口被放大，`test_wait_idle_for_times_out_and_then_succeeds` 在 CI 上稳定失败。

### 尝试 1：把 mark_started 移入 pop_for_worker

第一次修复把 `mark_started()` 移入 `pop_for_worker` 的每个成功返回路径。但这没解决问题——`pop` 释放 SafeQueue 锁后、`mark_started` 获取 `scheduler_.mutex_` 前，`wait_idle` 仍能在这个缝隙中观察到不一致状态。

### 最终方案：乐观 active_tasks 递增

在 `pop_for_worker` 的最开始，**先**在 `scheduler_.mutex_` 锁内 `++active_tasks_`，然后再尝试 pop。如果 pop 失败（无任务可取），回滚递减。

```
pop_for_worker:
  lock(scheduler_.mutex_)
  ++active_tasks_              ← 先记账，wait_idle 已经看不到 0
  unlock(scheduler_.mutex_)

  尝试 pop ... （无论成败，active_tasks 已经 > 0）

  if 失败:
    lock(scheduler_.mutex_)
    --active_tasks_            ← 回滚
    return false
```

关键洞察：`wait_idle` 的 predicate 同锁（`scheduler_.mutex_`）读取 `active_tasks_`，所以乐观递增后的值对 predicate 立即可见。pop 尝试过程中的队列变空不会导致误判，因为 active 计数已经为正。

## 35. `active_tasks()` 存在双重加锁

### 问题

`ThreadPoolRuntimeImpl::active_tasks()` 外层获取 `runtime_.mutex_`，然后调用 `scheduler_.active_tasks()` 内层获取 `scheduler_.mutex_`。两把锁没有冲突，但有一把是多余的——每次获取/释放锁都有开销。

### 解决策略

移除 `ThreadPoolRuntime` 中的外层锁，直接委托给 `TaskScheduler::active_tasks()`（它自带锁）。

## 36. `call_impl` 冗余声明 / `#include` 膨胀

### 问题

`call_impl` 是 C++14 下替代 `std::apply` 的静态成员函数模板。它在 `ThreadPool.h` 的 class 内声明，在 `ThreadPoolSubmit.h` 中定义。对于 header-only 库，这种声明-定义分离没有意义——声明唯一的目的是让定义在 class 外的模板能通过编译。

### 解决策略

将 `call_impl` 从静态成员函数改为 `detail::apply_impl` 自由函数。不再需要在 class 内声明。

连带优化：`ThreadPool.h` 原先为子头文件预导入 12 个标准库头文件（`<atomic>`、`<condition_variable>`、`<functional>` 等不在 facade 层直接使用的）。每个子头文件补齐各自的 `#include` 后，主头文件从 12 个缩减到 4 个（`<chrono>`、`<cstddef>`、`<future>`、`<stdexcept>`）。

## 37. `prefer_current_worker && current_runtime() == this` 被重复求值

### 问题

`enqueue_coroutine_resume` 中判断"是否偏好当前 worker"的复合条件 `prefer_current_worker && current_runtime() == this` 被计算了两次：一次选 target，一次传给 `push_coroutine`。`current_runtime()` 读 thread_local 不是纯函数——如果有协程在不同 worker 间迁移，第二次求值可能不同。

### 解决策略

计算一次存入 `const bool is_current`，后续两处引用同一值。

## 38. 协程队列容量不可查询

### 问题

`max_coroutine_queue_size` 的验证在 `TaskScheduler` 内部，异常直接抛给 `co_await` 处。调用方无法提前知道限制，也无法查询当前配置。

### 解决策略

三层贯通：`TaskScheduler::max_coroutine_queue_size()` → `ThreadPoolRuntime` → `ThreadPool` facade。调用方可以在提交协程前检查。

## 39. shutdown 可能永久阻塞

### 问题

`shutdown()` 调 `join_all()` 逐个 join worker 线程。如果某个 worker 正执行死循环或阻塞 IO，join 永不返回 → 进程无法退出。

### 解决策略

新增 `shutdown_for(timeout)` / `shutdown_until(deadline)`：

实现方式：单独启动一个 helper 线程 join 所有 worker，主线程以 deadline 轮询。超时则 `detach` 剩余 worker 和 helper，返回 `false`。

```
shutdown_for(5s):
  1. 设置 shutdown_ = true，通知所有 worker
  2. helper 线程 join 所有 worker
  3. 主线程以 1ms 粒度检查 helper 是否完成
  4. 超时 → detach helper + 剩余 worker → return false
  5. 完成 → join helper → return true
```

> **语义约定**：`shutdown_for` 返回 false 后，detach 的 worker 会继续执行到任务完成。已排队的全局任务在 CancelPending 模式下被丢弃。池对象仍可安全析构。

## 40. 非 packaged_task 任务的异常静默丢失

### 问题

worker 循环中 `catch(...)` 吞掉所有异常。`submit()` 创建的 `packaged_task` 会把异常保存到 future，但协程 recover 的 `schedule()`/`yield()` lambda 抛异常只能静默丢失。

### 解决策略

在 `ThreadPoolOptions` 中增加 `on_exception` 回调：

```cpp
std::function<void(const std::exception_ptr &)> on_exception;
```

worker 捕获异常后，先调用此回调再丢弃。用户可以在回调中记录日志、发告警。默认 `nullptr`（保持原有行为）。

## 41. min_worker 空闲时每 idle_timeout 伪唤醒

### 问题

所有 worker 统一用 `wait_for(timeout)` 等待工作。处于 min_workers 层的 worker 即便永远不会退出（`should_exit_on_idle() == false`），也会每 5 秒被 timeout 唤醒做无用检查。

### 解决策略

`wait_for_work` 中根据 `should_exit_on_idle()` 选择等待方式：

- 可退出的 excess worker → `wait_for(timeout)`，超时后尝试退休
- 保底的 min worker → `wait()`，永久阻塞直到有工作或 shutdown

## 42. 全局 `using ThreadPool` 污染命名空间

### 问题

`ThreadPool.h` 底部无条件注入全局命名空间：`using ThreadPool = threadpool::ThreadPool;`。任何包含此头文件的编译单元都被迫接受全局 `ThreadPool` 别名。如果其他库也定义了同名符号，构成 ODR 违规。

### 解决策略

改为 opt-in 宏：

```cpp
#ifdef THREADPOOL_USE_GLOBAL_NAMESPACE
using ThreadPool = threadpool::ThreadPool;
#endif
```

## 43. CMake install 规则缺失

### 问题

项目只有 `add_executable` 和 `add_library`，没有 `install()` 规则。用户只能 copy-paste 头文件，无法用 `find_package` 集成。

### 解决策略

增加 CMake install 规则 + 生成器表达式区分构建和安装路径：

```cmake
target_include_directories(threadpool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
install(TARGETS threadpool EXPORT threadpool-targets)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT threadpool-targets DESTINATION lib/cmake/threadpool)
```

首次提交时 `INSTALL_INTERFACE` 导致 CMake 报错"path prefixed in source directory"——`include/` 的相对路径被 CMake 解释为源码树内路径。修正为生成器表达式写法后解决。

## 44. CI 缺少 sanitizer 门禁

### 问题

CI 只有 Release 构建 + 功能测试。线程池库的核心风险（data race、use-after-free、内存泄漏）没有自动化检测。

### 解决策略

增加三个 CI job，与原有构建并行运行：

- **TSan**：`-fsanitize=thread`，检测 data race
- **ASan + UBSan**：`-fsanitize=address,undefined`，检测内存错误和未定义行为
- **Valgrind memcheck**：`--leak-check=full --error-exitcode=1`，检测内存泄漏

## 45. TSan 报告 GCC `shared_ptr` 伪阳性

### 问题

TSan job 报告 `shared_ptr_base.h:1070` 数据竞争。这是 GCC libstdc++ 的 `std::shared_ptr` 内部引用计数使用了 `__atomic` 内建函数，TSan 无法正确截获，产生误报。

### 解决策略

添加 `tsan_suppressions.txt`，抑制 `std::__shared_count` 和 `std::_Sp_counted_base` 的伪阳性。CI 通过 `TSAN_OPTIONS=suppressions=...` 引用。

首次 CI 运行时 suppression 文件路径写为相对路径（`suppressions=tsan_suppressions.txt`），但 CTest 默认以 build 目录为 CWD，找不到源码树根的文件。改为 `${{ github.workspace }}/tsan_suppressions.txt` 绝对路径。

## 46. ASan 检测到 test 中的 heap-use-after-free

### 问题

协程队列限制测试中，`std::promise<void> blocker` 在 `ThreadPool pool` 之后声明，C++ 以声明逆序析构——`blocker` 先于 `pool` 销毁。`blocker` 析构释放 shared state 时，worker 线程可能仍在 `block_future.get()` 中访问。

### 解决策略

将 `blocker` / `block_future` 声明移到 `pool` 之前。析构顺序变为 `pool` 先（join 所有 worker），再 `blocker`（此时 worker 已安全退出）。

## 47. Valgrind 下动态伸缩测试超时

### 问题

`test_dynamic_workers_can_retire_and_grow_again` 用 `idle_timeout=50ms`，sleep 120ms 给 worker 退休时间。Valgrind 下（10-30x 减速）时间不够，worker 还未退休断言已失败。

### 解决策略

增大 `idle_timeout` 到 500ms，sleep 到 1000ms。同时添加 `valgrind_suppressions.txt` 抑制 pthread TLS 的已知 `possibly lost` 分配（线程局部存储，进程退出时由 OS 回收）。

## 48. 长时运行稳定性没有覆盖

### 问题

CTest 只跑数秒内的功能测试。没有持续运行、反复创建销毁池、混合 submit+yield 的长时验证。

### 解决策略

新增 `long_run_stress.cpp`：可配置时长，持续运行 submit / create-destroy cycle / coroutine yield 以及 shutdown 超时验证。不加入 CTest（时长太长），保留为手动运行工具。

## 49. Benchmark 缺少竞品基线对比

### 问题

benchmark 只测量自身数据，没有参照物。看吞吐数字无法判断好坏。

### 解决策略

增加 `std::async` 基线：

```cpp
void benchmark_std_async(size_t tasks) {
    // 用 std::async(std::launch::async, ...) 提交相同任务
    // 对比 thread pool 的调度开销
}
```

`std::async` 每任务创建一个线程（或从全局线程池分配），其吞吐数字可作为"无调度层开销"的参照。

## 50. `threadpool_stress` 拼写错误保留

### 问题

stress test 目标名自早期代码以来一直拼作 `threadpool_stress`（少了一个 t）。更名会破坏已有 CI 脚本和用户肌肉记忆。

### 解决策略

保留现状。所有引用（CMake target、CTest、二进制文件名、文档）保持一致使用 `stress`。不修正。

---

## 总结

本项目的开发问题集中在五类：

1. **并发正确性**：条件变量语义、TOCTOU 竞态（pop/mark_started、idle 超时、多 worker 退休）、原子操作与锁的粒度配合、引用计数伪阳性。
2. **资源生命周期**：线程启停时序（spawn/retire 计数一致性）、shutdown 超时、异常处理链路、协程帧析构。
3. **可用性**：shutdown 永不阻塞、异常可追踪、空闲省电、容量可查询。
4. **工程化**：CMake install、sanitizer CI（TSan + ASan + UBSan + Valgrind）、benchmark 基线、vcpkg 打包。
5. **跨平台与稳健性**：GCC `shared_ptr` TSan 伪阳性、Valgrind 减速导致测试超时、pthread TLS 静态泄漏。

这些问题的解决过程让项目从"能工作的线程池"演进为"有门禁的正式产品"——每一个修复背后都是生产环境中会遇到的真问题。
