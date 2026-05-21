# Build & Test

构建、测试和 CI 流程指南。

## 前置要求

| 工具 | 最低版本 | 说明 |
|------|---------|------|
| CMake | 3.14 | 构建系统 |
| GCC | 8+ | Linux 编译器（C++20 协程需 GCC 11+） |
| MSVC | 2019+ | Windows 编译器（C++20 协程需 VS 2022） |
| Clang | 10+ | 替代编译器（协程需 Clang 14+） |

> 如果编译器不支持 C++20 协程，协程相关代码会被自动禁用，不影响 C++14 功能。

---

## CMake 构建

```bash
# 1. 配置（Release 模式）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. 构建（多核并行）
cmake --build build --config Release --parallel

# 3. 运行所有测试
ctest --test-dir build --build-config Release --output-on-failure
```

### 构建目标

| 目标 | 文件名 | 说明 |
|------|--------|------|
| `threadpool_demo` | `main.cpp` | 演示程序 |
| `threadpool_tests` | `test.cpp` | C++14 行为测试 |
| `threadpool_coroutine_tests` | `test_coroutine.cpp` | C++20 协程测试 |
| `threadpool_stress` | `stress_test.cpp` | 压力测试 |
| `threadpool_benchmark` | `benchmark.cpp` | 性能基准 |
| `threadpool_mode_compare` | `mode_compare.cpp` | 两种模式对比 |

---

## 手动编译（不使用 CMake）

```bash
# Linux — C++20 协程测试
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -pthread \
    test_coroutine.cpp -o threadpool_coroutine_tests

# Linux — 普通测试
g++ -std=gnu++14 -Wall -Wextra -Wpedantic -pthread \
    test.cpp -o threadpool_tests
```

---

## 测试套件

### CTest 注册的测试

```bash
ctest --test-dir build --build-config Release --output-on-failure
```

默认运行 3 个测试：

| 测试名 | 命令 | 内容 |
|--------|------|------|
| `threadpool_tests` | 直接执行 | 11 个行为测试用例 |
| `threadpool_coroutine_tests` | 直接执行 | 4 个协程测试用例 |
| `threadpool_stress` | `5000 4 4` | 轻量压力测试 |

> benchmark 和 mode_compare **不在 CTest 中**，因为结果依赖于硬件。

### 行为测试（test.cpp）

| 测试函数 | 验证内容 |
|----------|---------|
| `test_submit_returns_future_value` | submit 返回正确的 future 值 |
| `test_all_queued_tasks_run_before_shutdown` | Drain 模式等待所有任务完成 |
| `test_submit_after_shutdown_throws` | 关闭后提交抛出异常 |
| `test_constructor_starts_workers` | 构造函数自动启动 worker |
| `test_zero_worker_count_throws` | worker 为 0 时抛出异常 |
| `test_wait_idle_observes_completed_work` | wait_idle 正确观察任务完成 |
| `test_cancel_pending_shutdown_breaks_unstarted_futures` | CancelPending 破坏未开始任务的 future |
| `test_namespace_type_is_available` | 命名空间正确可用 |
| `test_wait_idle_for_times_out_and_then_succeeds` | 超时等待正确工作 |
| `test_submit_with_stop_observes_requested_stop` | StopToken 正确传播停止信号 |
| `test_dynamic_workers_can_retire_and_grow_again` | 动态扩缩容正确 |

### 协程测试（test_coroutine.cpp）

| 测试 | 验证内容 |
|------|---------|
| `switch_to_threadpool` | co_await schedule 切换到线程池线程 |
| `yielding_work` | 协程 yield 交错执行，顺序正确 |
| `rejected_coroutine_schedule` | ThreadOnly 模式 schedule 抛出异常 |
| `scheduled_before_shutdown` | 已调度的协程在 CancelPending 后仍执行 |

### 压力测试（stress_test.cpp）

```bash
# 自定义参数: <tasks> <workers> <submitters>
./build/Release/threadpool_stress.exe 50000 8 8
```

| 场景 | 说明 |
|------|------|
| `run_many_future_tasks` | 大量任务 + 返回值校验 |
| `run_parallel_submitters` | 多线程并发提交 |
| `run_cancel_pending_stress` | 取消大量待处理任务 |
| `run_stop_token_stress` | 协作取消压力验证 |

### 模式对比（mode_compare.cpp）

```bash
# <tasks> <coroutines> <yields_per_coroutine> <workers>
./build/Release/threadpool_mode_compare.exe 20000 2000 10 4
```

对比 `ThreadOnly` 和 `ThreadAndCoroutine` 两种模式下的吞吐量，输出格式化的比较表。

---

## 性能基准

```bash
# <tasks> <submitters>
./build/Release/threadpool_benchmark.exe 100000 4
```

| 基准场景 | 说明 |
|----------|------|
| `fire_and_wait` | 纯调度延迟（空任务 + wait_idle） |
| `future_values` | 任务返回值收集延迟 |
| `parallel_submitters` | 多线程并发提交吞吐量 |
| `latency_sample` | P50/P95/P99 延迟分位数 |

输出格式化的性能比较表。

---

## CI 流程

配置文件：`.github/workflows/ci.yml`

```
触发：push/PR 到 main 或 master 分支

┌─ ubuntu-latest ──────────────────────┐
│  cmake configure → build → ctest     │
└──────────────────────────────────────┘
┌─ windows-latest ─────────────────────┐
│  cmake configure → build → ctest     │
└──────────────────────────────────────┘
```

- 两个平台并行执行
- 任何平台失败则 CI 失败
- CTest 输出失败详情（`--output-on-failure`）

---

## 开发工作流

```bash
# 1. 修改代码后 — 构建
cmake --build build --config Release --parallel

# 2. 运行行为测试
./build/Release/threadpool_tests.exe

# 3. 运行协程测试
./build/Release/threadpool_coroutine_tests.exe

# 4. 运行完整测试套件
ctest --test-dir build --build-config Release --output-on-failure

# 5. 提交前 — 运行压力测试
./build/Release/threadpool_stress.exe 50000 8 8
```
