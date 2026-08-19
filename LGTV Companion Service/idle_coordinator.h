#pragma once

#include <chrono>
#include <optional>

class IdleCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    enum class State {
        Active,
        Idle,
        ExtendedIdle,
    };

    explicit IdleCoordinator(Duration extended_idle_delay);

    // Returns true only when this call begins a new idle interval.
    bool onIdle(TimePoint now);

    // Returns true only when activity transitions the coordinator back to Active.
    bool onBusy();

    // Returns true exactly once when the currently armed deadline becomes due.
    bool onDeadline(TimePoint now);

    State state() const noexcept { return state_; }
    const std::optional<TimePoint>& deadline() const noexcept { return deadline_; }

private:
    Duration extended_idle_delay_;
    State state_{State::Active};
    std::optional<TimePoint> deadline_;
};
