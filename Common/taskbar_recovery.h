#pragma once

#include <chrono>
#include <optional>

class TaskbarRecoveryScheduler {
public:
    using Clock = std::chrono::steady_clock;

    explicit TaskbarRecoveryScheduler(std::chrono::milliseconds delay)
        : delay_(delay) {}

    void schedule(Clock::time_point now) {
        deadline_ = now + delay_;
    }

    bool pending() const {
        return deadline_.has_value();
    }

    bool consumeIfDue(Clock::time_point now) {
        if (!deadline_ || now < *deadline_)
            return false;
        deadline_.reset();
        return true;
    }

private:
    std::chrono::milliseconds delay_;
    std::optional<Clock::time_point> deadline_;
};
