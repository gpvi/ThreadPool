#pragma once

namespace threadpool {

#ifdef THREADPOOL_HAS_COROUTINE
inline void ThreadPool::enqueue_coroutine_resume(
    std::coroutine_handle<> handle,
    bool prefer_current_worker
)
{
    runtime_.enqueue_coroutine_resume(handle, prefer_current_worker);
}
#endif

} // namespace threadpool
