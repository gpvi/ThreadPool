#pragma once

#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace threadpool {

template <typename T>
class SafeQueue {
private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;

public:
    SafeQueue() = default;
    SafeQueue(const SafeQueue &) = delete;
    SafeQueue(SafeQueue &&) = delete;
    SafeQueue &operator=(const SafeQueue &) = delete;
    SafeQueue &operator=(SafeQueue &&) = delete;
    ~SafeQueue() = default;

    bool empty() const
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    std::size_t size() const
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    void push(T t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.push(std::move(t));
    }

    bool pop(T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;

        t = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    std::size_t clear()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const std::size_t removed = m_queue.size();
        std::queue<T> empty;
        m_queue.swap(empty);
        return removed;
    }
};

} // namespace threadpool
