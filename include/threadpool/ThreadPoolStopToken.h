#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace threadpool {

class StopToken {
private:
    std::shared_ptr<std::atomic<bool>> stop_requested_;

public:
    StopToken()
        : stop_requested_(std::make_shared<std::atomic<bool>>(false))
    {
    }

    bool stop_requested() const noexcept
    {
        return stop_requested_->load(std::memory_order_acquire);
    }

private:
    friend class StopSource;

    explicit StopToken(std::shared_ptr<std::atomic<bool>> state)
        : stop_requested_(std::move(state))
    {
    }
};

class StopSource {
private:
    std::shared_ptr<std::atomic<bool>> stop_requested_;

public:
    StopSource()
        : stop_requested_(std::make_shared<std::atomic<bool>>(false))
    {
    }

    StopToken token() const
    {
        return StopToken(stop_requested_);
    }

    void request_stop() const noexcept
    {
        stop_requested_->store(true, std::memory_order_release);
    }
};

} // namespace threadpool
