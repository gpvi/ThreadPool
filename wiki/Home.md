# ThreadPool Wiki

ThreadPool 是一个 **header-only** 的 C++ 线程池库，C++14 基线，条件编译支持 C++20 协程调度。

## 核心特性

| 特性 | 说明 |
|------|------|
| **零依赖** | 纯头文件，只需 `#include "ThreadPool.h"` |
| **C++14 基线** | 兼容 C++14/17/20，C++20 协程特性条件启用 |
| **任务提交** | `submit(f, args...)` 返回 `std::future<R>` |
| **协程调度** | `co_await pool.schedule()` / `co_await pool.yield()` |
| **工作窃取** | 空闲 worker 从其他 worker 的协程队列窃取任务 |
| **动态扩缩** | 按需增长、闲置超时自动缩容 |
| **两种关闭** | `Drain`（等待完成）/ `CancelPending`（取消未开始） |
| **协作取消** | `StopToken` / `StopSource`，不强制终止线程 |
| **双执行模式** | `ThreadOnly`（纯线程）/ `ThreadAndCoroutine`（混合） |
| **跨平台** | Windows / Linux，GitHub Actions CI |

## 五分钟快速上手

```cpp
#include "ThreadPool.h"
#include <iostream>

int main() {
    // 1. 创建线程池（4 个 worker）
    ThreadPool pool(4);

    // 2. 提交任务，获取 future
    auto future = pool.submit([](int a, int b) {
        return a + b;
    }, 10, 20);

    std::cout << future.get() << '\n';  // 30

    // 3. 优雅关闭（等待所有已提交任务完成）
    pool.shutdown();

    return 0;
}
```

## 导航

| 页面 | 内容 |
|------|------|
| [API Reference](API-Reference.md) | 全部公开 API 的签名和说明 |
| [Architecture](Architecture.md) | 四层架构、组件职责、数据流 |
| [Task Submission](Task-Submission.md) | 类型擦除链路、packaged_task、返回值 |
| [Coroutine Scheduling](Coroutine-Scheduling.md) | C++20 协程调度、schedule/yield、awaiter |
| [Thread Safety](Thread-Safety.md) | 锁层级、SafeQueue、条件变量、线程模型 |
| [Lifecycle](Lifecycle.md) | 生命周期状态机、启动、关闭语义 |
| [Configuration](Configuration.md) | ThreadPoolOptions、ExecutionMode、调优 |
| [Build & Test](Build-Test.md) | CMake 构建、测试套件、CI 流程 |
| [Examples](Examples.md) | 常用模式与代码示例 |
| [Internals](Internals.md) | 内部实现细节、设计决策、扩展点 |

## 文件组织

```
ThreadPool.h                  ← 唯一公开头文件（用户只 include 这个）
├── ThreadPoolOptions.h       ← 配置结构体、枚举
├── ThreadPoolStopToken.h     ← 协作取消
├── ThreadPoolTypeTraits.h    ← C++14/17 兼容层
├── SafeQueue.h               ← 线程安全队列
├── TaskScheduler.h           ← 调度器（全局队列 + 协程队列 + 窃取）
├── ThreadPoolRuntime.h       ← 运行时协调器声明
├── ThreadPoolRuntimeImpl.h   ← 运行时协调器实现
├── ThreadPoolWorker.h        ← Worker 线程入口
├── WorkerGroup.h             ← 线程组管理
├── WorkerGroupImpl.h         ← spawn 实现
├── ThreadPoolSubmit.h        ← submit/submit_with_stop 模板实现
└── ThreadPoolImpl.h          ← 协程转发实现
```

> **提示**：如果你是第一次阅读源码，建议按 [Architecture](Architecture.md) → [Task Submission](Task-Submission.md) → [Thread Safety](Thread-Safety.md) 的顺序学习。
