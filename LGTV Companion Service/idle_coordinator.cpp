#include "idle_coordinator.h"

IdleCoordinator::IdleCoordinator(Duration extended_idle_delay)
    : extended_idle_delay_(extended_idle_delay) {
}

bool IdleCoordinator::onIdle(TimePoint now) {
    if (state_ != State::Active) {
        return false;
    }
    state_ = State::Idle;
    deadline_ = now + extended_idle_delay_;
    return true;
}

bool IdleCoordinator::onBusy() {
    if (state_ == State::Active) {
        return false;
    }
    state_ = State::Active;
    deadline_.reset();
    return true;
}

bool IdleCoordinator::onDeadline(TimePoint now) {
    if (state_ != State::Idle || !deadline_ || now < *deadline_) {
        return false;
    }
    state_ = State::ExtendedIdle;
    deadline_.reset();
    return true;
}
