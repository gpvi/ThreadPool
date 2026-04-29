#pragma once

namespace threadpool {

class ThreadPoolWorker {
private:
    ThreadPoolRuntime *runtime_;
    std::size_t worker_id_;

public:
    ThreadPoolWorker(ThreadPoolRuntime *runtime, std::size_t worker_id)
        : runtime_(runtime),
          worker_id_(worker_id)
    {
    }

    void operator()()
    {
        runtime_->set_current_worker(worker_id_);

        std::size_t coroutine_burst = 0;

        while (true) {
            ThreadPoolRuntime::WorkerWork work =
                runtime_->wait_for_work(worker_id_, coroutine_burst);

            if (work.exit) {
                break;
            }

            if (!work.has_task) {
                continue;
            }

            try {
                work.task();
            } catch (...) {
                // packaged_task 会将异常保存到 future。
                // 这里兜底，防止非 packaged_task 任务导致 worker 线程异常退出。
            }

            runtime_->finish_work(work.popped_coroutine, coroutine_burst);
        }

        runtime_->clear_current_worker();
    }
};

} // namespace threadpool
