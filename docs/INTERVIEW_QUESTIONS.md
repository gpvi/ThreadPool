# ThreadPool 项目面试问题

本文档按难度递进、分专题整理，每道题附关键回答要点。

---

## 一、热身：项目概览

### Q1. 用三句话介绍这个项目。

**要点：**
- header-only C++ 线程池库，C++14 基线，C++20 协程条件启用
- 四层架构：Facade → Runtime → Scheduler/WorkerGroup → Worker/SafeQueue
- 核心能力：submit 普通任务 + schedule/yield 协程调度 + 协作式取消 + 双关闭语义 + work stealing

### Q2. 为什么选择做线程池而不是别的并发组件？

**要点：**
- 线程池是最实用的并发基础组件，能深入理解线程同步、任务调度、协程
- 可深可浅：基础功能 500 行能写完，但要补协程调度、work stealing、CI 能写到上万行
- 可测试：行为测试、压力测试、benchmark 都能覆盖

---

## 二、任务模型

### Q3. submit 内部做了什么？从 `pool.submit(f, args...)` 到 future 返回，每一步解释。

**要点：**
```
1. invoke_result_t 推导 ReturnType
2. std::bind 绑定参数 → 无参可调用对象
3. std::packaged_task<ReturnType()> 包装（shared_ptr，因为不可拷贝）
4. get_future() 取出 future 返回给调用方
5. lambda [packaged]{ (*packaged)(); } 包进 std::function<void()>
6. submit_task 入全局队列 + notify_one 唤醒 worker
```

追问：**为什么用 shared_ptr 包 packaged_task？**
→ packaged_task 不可拷贝，但 std::function 要求其包装对象可拷贝。shared_ptr 共享底层对象，引用计数管理生命周期。

### Q4. 为什么内部队列只存 `std::function<void()>` 而不存模板类型？

**要点：**
- 类型擦除 — worker 不需要知道任务签名
- 优点：队列类型统一、worker 实现简单、一种队列存所有任务
- 缺点：`std::function` 有虚函数调用开销（对极短任务有影响）
- 对比：模板化队列（零虚函数但每种签名一套队列）vs 函数指针（不安全、不支持 lambda）

### Q5. 为什么不手写 promise/set_value，而用 packaged_task？

**要点：**
- packaged_task 一次性封装三个职责：调任务、写返回值、异常写入
- 手写需要 try/catch + set_value/set_exception，容易遗漏异常路径
- packaged_task 析构时如果 promise 未满足会自动设置 broken promise

### Q6. 任务提交后、执行前，线程池就 shutdown 了，future.get() 会怎样？

**要点：**
- Drain 模式：已入队任务继续执行完毕，future 正常返回
- CancelPending 模式：排队任务被清空，packaged_task 析构时 promise 未满足 → `std::future_error`（broken promise）
- 这是一个合理但需要调用方知晓的行为

---

## 三、Worker 循环与调度

### Q7. Worker 线程的主循环在做什么？画出流程图。

**要点：**
```cpp
while (true) {
    wait_for_work();  // 条件变量阻塞等活
    if (exit) break;  // 关闭或缩容
    if (!has_task) continue;  // 虚假唤醒

    try { task(); } catch(...) {}  // 执行

    finish_work();  // 更新 burst 计数 + active 减 1
}
```

### Q8. Worker 取任务的四级优先级是什么？为什么这样排？

**要点：**

| 优先级 | 来源 | 条件 |
|--------|------|------|
| 1 | 全局普通任务 | burst >= 8（防饥饿） |
| 2 | 本 worker 协程队列 | 队列非空 |
| 3 | 全局普通任务 | 队列非空 |
| 4 | steal 其他人协程 | 有可偷 |

这样排的原因：
- 协程优先 → 保证协程响应性
- burst_limit 插入 → 防止普通任务饥饿
- steal 放最后 → 只在真没活干时才做，不浪费 CPU 抢别人

### Q9. burst_limit 是什么？设成 0、8、99999 各有什么问题？

**要点：**
- 连续执行 N 个协程恢复后，强制取全局普通任务
- 0：每次循环都先看普通任务 → 协程完全没优势
- 8：默认值，经验平衡点，可配置
- 99999：协程几乎永远优先 → 普通任务饿死

### Q10. 为什么 wait_for_work 的 predicate 是 `has_any_work_for_worker` 而不仅仅是检查自己的协程队列？

**要点：**
- `has_any_work_for_worker` 的搜索范围：全局普通队列、自己协程队列、其他 worker 协程队列
- 如果只看自己的协程队列，那么某个 worker 只会在自己队列有活时才醒——即使全局队列有活或能 steal，它也不会醒
- 这保证 worker 在有 stealable 任务时也能被唤醒

---

## 四、协程调度

### Q11. `co_await pool.schedule()` 从挂起到恢复，每一步发生了什么？

**要点：**
```
1. await_ready() → false（总是挂起，保证公平）
2. await_suspend(coroutine_handle)
   → round-robin 选目标 worker
   → 把 handle.resume() 包装成 std::function<void()>
   → 放入目标 worker 的本地协程队列
3. notify_all() 唤醒 worker
4. Worker 在 pop_for_worker 中取出该任务
5. handle.resume() → 协程在 Worker 线程上从 co_await 下一行恢复
```

### Q12. schedule() 和 yield() 的区别是什么？

**要点：**

| | schedule() | yield() |
|---|---|---|
| 语义 | 首次切换到线程池 | 主动让出，等下再回来 |
| 初始位置 | 任意线程 | 已在 worker 上 |
| 分配策略 | round-robin | 优先回当前 worker（亲和性） |
| prefer_current_worker | false | true |

### Q13. yield() 的亲和性是怎么实现的？有没有保证？

**要点：**
- 通过 `thread_local` 变量 `current_runtime()` 和 `current_worker_id()` 判断调用者是否为本 pool 的 worker
- 如果是 → `target = current_worker_id()`，放回当前 worker 协程队列
- 如果不是 → 回退到 round-robin
- 没有保证一定能回到同一个 worker——取决于队列排队情况和 steal

追问：**如果在 main 线程上调 yield() 会怎样？**
→ 亲和性条件不成立（`current_runtime() != pool`），退化为 round-robin 分配。不报错。

### Q14. 协程入队为什么用 notify_all() 而不是 notify_one()？

**要点：**
- yield() 有目标 worker，但该 worker 可能正在忙
- 如果只有 notify_one，醒的可能是别的 worker，而别的 worker 不会优先看不是自己的协程队列
- notify_all 让目标 worker 有机会醒来，同时其他人也可能通过 steal 拿走
- 调度路径的唤醒比普通任务更"慷慨"——宁可多醒几个，也不要协程长时间挂着

### Q15. C++20 协程给了什么、没给什么？你这个项目补了什么？

**要点：**
- 给了：`co_await`、`coroutine_handle`、`promise_type` — 挂起和恢复的语言机制
- 没给：调度器 — 协程在哪个线程恢复、何时恢复
- 我补的：在 `await_suspend` 中手动把 `handle.resume()` 分配到指定 worker 的队列，由线程池决定恢复时机

---

## 五、线程安全

### Q16. 项目里有多少把锁？每把锁保护什么？

**要点：**

| 锁 | 位置 | 保护数据 |
|-----|------|---------|
| m_mutex | SafeQueue | 底层 std::queue |
| mutex_ | TaskScheduler | active_tasks_、next_coroutine_worker_、idle_cv_ |
| mutex_ | WorkerGroup | workers_ 容器、retired_worker_ids_ |
| mutex_ | ThreadPoolRuntime | shutdown_、started_、work_cv_ |

`live_workers_` 和 `next_worker_id_` 是 `atomic`，不加锁。

### Q17. 为什么不用一把大锁？

**要点：**
- 大锁下提交线程和 worker 互锁 — worker 在执行任务时，提交线程无法入队
- 分拆后，队列操作（SafeQueue 内部锁）和任务执行（锁外）可并行
- 原则："谁拥有数据，谁管理锁" — 每层只锁自己拥有的状态

### Q18. notify_one() 为什么放在锁外面？

**要点：**
- 锁内 notify → 被唤醒线程第一件事就是抢同一把锁 → 必然阻塞 → 白费一次唤醒
- 锁外 notify → 被唤醒线程能直接拿到锁 → 有效唤醒

### Q19. `condition_variable::wait(lock, predicate)` 为什么必须用 predicate 形式？

**要点：**
- 虚假唤醒：OS 可能无故唤醒线程，predicate 让它在不是真正满足条件时继续睡
- 通知丢失：任务入队和线程进入 wait 之间存在时序窗口 — notify 发生在 wait 之前就会丢失。predicate 检查共享状态而非依赖通知
- 统一处理关闭和任务等待：一个 predicate 覆盖两类事件

### Q20. WorkerGroup 中 `live_workers_` 为什么用 atomic 而不是在 mutex 保护下？

**要点：**
- live_workers_ 被多个线程频繁读：maybe_grow（判断是否扩容）、should_exit_on_idle（判断是否缩容）
- atomic 避免每次只读都加锁
- 写入也有顺序保证：spawn 时先创建线程成功，再 fetch_add

---

## 六、生命周期

### Q21. 为什么构造函数自动 start？析构函数自动 shutdown？

**要点：**

自动 start：
- 早期版本需手动 init()，用户创建后直接 submit → 任务入队但没 worker → future 永久阻塞
- 构造函数自动启动，init() 保留为幂等接口

自动 shutdown：
- `std::thread` joinable 析构 → `std::terminate` 终止进程
- 析构函数调 shutdown() + try/catch 保护 + noexcept

### Q22. Drain 和 CancelPending 的区别？CancelPending 为什么不清空协程队列？

**要点：**

| | Drain | CancelPending |
|---|---|---|
| 新任务 | 抛异常 | 抛异常 |
| 已排队普通任务 | 执行完 | 清空丢弃 |
| 正在执行的任务 | 执行完 | 执行完 |
| 协程队列任务 | 继续执行 | **不清空** |

为什么不清协程队列：
- 协程挂起后恢复它的唯一途径就是队列中的 `handle.resume()`
- 清空 → 协程永远醒不来 → promise 不完成 → 等待方永久阻塞
- 正确做法：让协程恢复一次，后续 schedule 因 pool 已关闭收到异常

### Q23. 空闲 worker 退出时，为什么不能自己 join 自己？

**要点：**
- 线程 join 自己 → 死锁（等待自己结束，不可能）
- 退出的 worker 把 `thread::id` 写入 `retired_worker_ids_`
- 下一次 spawn() 时由其他线程调用 `reap_retired_workers()` 执行 join 和 erase
- shutdown 时由 `join_all()` 做最终清理

### Q24. spawn() 为什么"先创建线程、成功后再 +1 计数"，而不是反过来？

**要点：**
- 异常安全：先加计数再创建线程 → 线程创建失败（抛异常）→ 计数显示有 worker 但实际没有
- 先创建成功再 `fetch_add(1)` → 计数与实际状态永远一致

---

## 七、Work Stealing

### Q25. Work stealing 解决什么问题？怎么实现的？

**要点：**
- 问题：协程队列 per-worker，某些 worker 协程多、某些空闲，负载不均
- 实现：从 `(worker_id + 1)` 开始线性搜索第一个非空的目标，取一个就走
- 为什么只 steal 协程？普通任务在全局队列大家公平抢，不需要偷
- 偷取不影响 burst 计数（不惩罚 steal）

### Q26. 如果有一个协程队列特别长（100 个任务），其他 worker 会怎么应对？

**要点：**
- 空闲 worker 在 pop_for_worker 第四级进入 steal_coroutine
- 线性搜索到该协程队列 → pop 一个 → 执行
- 执行完再次循环 → 自己队列空、全局空 → 再次 steal → 再偷一个
- 多个空闲 worker 会同时从该队列偷 → 自动负载均衡

---

## 八、协作式取消

### Q27. C++ 为什么不能强制杀线程？你的取消方案是什么？

**要点：**
- 强制杀线程：锁不释放、析构不执行、共享状态损坏、资源泄漏
- 方案：StopSource/StopToken 共享一个 `shared_ptr<atomic<bool>>`
- Source 写 stop（`memory_order_release`），Token 读（`memory_order_acquire`）
- 任务自己在安全点检查 `token.stop_requested()` 并主动退出
- 不能强制终止运行的任务，但符合 C++ 资源管理习惯

---

## 九、工程化 & 踩坑

### Q28. 项目开发中踩过哪些坑？挑一个印象最深的讲。

**要点（任选一个深讲）：**

1. **`std::result_of` 在 MSVC/C++20 下不可用** → 增加 TypeTraits 条件编译层
2. **`microseconds::rep` 跨平台不同** → 不硬编码 `long long`，用 `::rep`
3. **条件变量缺少 predicate** → 虚假唤醒 + 通知丢失，改用 predicate 形式
4. **协程恢复不能混全局队列** → 分离 per-worker 协程队列
5. **burst_limit 缺失导致普通任务饥饿** → 增加连续协程配额

### Q29. CI 为什么选择 ubuntu + windows？发现了什么问题？

**要点：**
- 覆盖 GCC 和 MSVC 两种编译器
- 实际发现的差异：Linux 上 `microseconds::rep` 是 `long` 不是 `long long`、MSVC 不提供 `bits/stdc++.h`、MSVC C++20 下不提供 `std::result_of`
- 不跑 benchmark 进 CTest — 性能结果不稳定，不适合 CI 门禁

### Q30. 这个项目的文档是怎么组织的？为什么这样分层？

**要点：**
- README — 快速上手和基本用法
- ARCHITECTURE.md — 代码结构、数据流、锁层次
- DESIGN.md — 设计决策和取舍
- IMPLEMENTATION_PRINCIPLE.md — 逐层的实现原理
- DEVELOPMENT_NOTES.md — 踩坑记录和复盘
- 分层目的：不同读者按需阅读，首页不沉重

---

## 十、开放性 & 深度追问

### Q31. 这个项目中你最自豪的一个设计决策是什么？

**无标准答案，参考方向：**
- 协程队列和全局队列分离 + 亲和性 + work stealing — 一个线程承载多个协程
- 四层架构从一个大类重构而来 — 职责清晰，可独立测试
- 锁分层策略 — 不被"大锁"诱惑
- 所有 29 个开发问题的解决过程都有文档记录

### Q32. 如果要升级到生产级，你最优先做的三件事？

**参考：**
1. per-worker 普通任务队列 + work stealing（解决全局队列锁竞争瓶颈）
2. 完整 `Task<T>` 协程模型 + continuation + structured concurrency
3. 更丰富的可观测性：per-worker metrics、任务等待时间分布、prometheus 格式

### Q33. 如果有人 fork 了你的项目，想加任务优先级，你会建议他怎么改？

**要点：**
- TaskScheduler 中增加多队列（高/中/低优先级）
- pop_for_worker 第一步改为"优先取高优队列"
- 需要新增优先级饥饿防护（低优任务不能永远等）
- 接口层新增 `submit_priority(f, Priority::High, args...)`
- 协程队列和优先级的关系需要额外设计（协程是否也应该有优先级？）

### Q34. 现在有一个场景：4 个 worker，10000 个任务是纯计算（1μs 完成），100 个任务是大量 I/O（100ms 完成）。你会怎么设计调度策略避免长任务堵死短任务？

**要点：**
- 当前实现不能区分任务类型，先入队的先执行
- 可能的方案：分离计算队列和 I/O 队列，不同的 worker 群
- 或：任务超时机制 + 长任务自动降级到专用 worker
- 或：引入任务元信息（预估执行时间），调度时加权分配
- 承认当前限制：FIFO 对长尾任务不友好

### Q35. 解释一下 '谁拥有数据，谁管理锁' 这个原则在你项目中的体现。

**要点：** 见 Q16 的锁分层表。每把锁只保护自己组件拥有的数据。不跨层持锁、锁外 notify。例子：submit_task 中 mutex_ 保护 shutdown 检查，push_task 用 SafeQueue 自己的锁，两者不嵌套。

### Q36. 线程和协程在这个项目中到底是什么关系？

**要点：**
- 线程是物理执行载体（OS 管理），协程是逻辑执行单元（编译器管理）
- 一个 worker 线程可以承载多个协程：协程 A resume → A yield → 协程 B resume → B yield → 协程 A resume
- 协程不是抢占式：不写 co_await 就一直占着 worker
- 线程有锁和 condition_variable 管理，协程靠 coroutine_handle 和 promise_type

---

## 难度对照

| 级别 | 题号 | 适用场景 |
|------|------|---------|
| 基础 | Q1-Q6 | 简历筛选、一面暖场 |
| 中等 | Q7-Q15 | 技术面、考察理解深度 |
| 进阶 | Q16-Q30 | 二面/三面、考察并发和工程能力 |
| 开放 | Q31-Q36 | 终面、考察架构视野和 trade-off 判断 |
