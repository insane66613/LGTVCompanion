#include "device_coordinator_core.h"

DeviceCoordinatorCore::DeviceCoordinatorCore(
    IdleCoordinator::Duration extended_idle_delay,
    bool preserve_desktop_topology_on_idle)
    : idle_(extended_idle_delay),
      preserve_desktop_topology_on_idle_(preserve_desktop_topology_on_idle) {
}

DeviceCoordinatorCoreSnapshot DeviceCoordinatorCore::snapshot() const noexcept {
    return {idle_.state(), idle_.deadline(), restore_needed_};
}

DevicePlanPreview DeviceCoordinatorCore::previewEvent(
    DeviceLifecycleEvent event, IdleCoordinator::TimePoint now) const {
    auto copy = *this;

    // Diagnostic scenarios exercise the production transition from the
    // meaningful precondition rather than becoming a no-op because the live
    // coordinator already happens to be in the target state. All normalization
    // happens on the copy and therefore cannot mutate live orchestration state.
    if (event == DeviceLifecycleEvent::UserIdle &&
        copy.idle_.state() != IdleCoordinator::State::Active) {
        copy.onEvent(DeviceLifecycleEvent::UserBusy, now);
    } else if (event == DeviceLifecycleEvent::UserBusy &&
               copy.idle_.state() == IdleCoordinator::State::Active) {
        copy.onEvent(DeviceLifecycleEvent::UserIdle, now);
    }

    auto plan = copy.onEvent(event, now);
    return {std::move(plan), copy.snapshot()};
}

DevicePlanPreview DeviceCoordinatorCore::previewExtendedIdle(
    IdleCoordinator::TimePoint now) const {
    auto copy = *this;
    if (copy.idle_.state() == IdleCoordinator::State::ExtendedIdle)
        copy.onEvent(DeviceLifecycleEvent::UserBusy, now);
    if (copy.idle_.state() == IdleCoordinator::State::Active)
        copy.onEvent(DeviceLifecycleEvent::UserIdle, now);

    DevicePlan plan;
    if (copy.idle_.state() == IdleCoordinator::State::Idle) {
        const auto due = copy.idle_.deadline().value_or(now);
        plan = copy.onDeadline(due);
    }
    return {std::move(plan), copy.snapshot()};
}

DevicePlan DeviceCoordinatorCore::shortIdle(IdleCoordinator::TimePoint now) {
    DevicePlan plan;
    const bool first_idle = idle_.onIdle(now);
    plan.deadline_changed = first_idle;
    plan.deadline = idle_.deadline();
    if (first_idle) {
        restore_needed_ = true;
        plan.actions = {
            DeviceAction::SamsungEnableScreenOff,
            DeviceAction::VizioBlankPanel,
        };
    }
    return plan;
}

DevicePlan DeviceCoordinatorCore::restoreActive() {
    DevicePlan plan;
    plan.deadline_changed = idle_.onBusy();
    plan.deadline = idle_.deadline();
    if (restore_needed_) {
        restore_needed_ = false;
        plan.actions = {
            DeviceAction::SamsungPowerOn,
            DeviceAction::SamsungRestoreScreenOff,
            DeviceAction::VizioWake,
        };
    }
    return plan;
}

DevicePlan DeviceCoordinatorCore::fullPowerOff() {
    DevicePlan plan;
    plan.deadline_changed = idle_.onBusy();
    plan.deadline = idle_.deadline();
    restore_needed_ = true;
    plan.actions = {
        DeviceAction::VizioPowerOff,
        DeviceAction::SamsungRestoreScreenOff,
        DeviceAction::SamsungPowerOff,
    };
    return plan;
}

DevicePlan DeviceCoordinatorCore::onEvent(DeviceLifecycleEvent event,
                                           IdleCoordinator::TimePoint now) {
    switch (event) {
    case DeviceLifecycleEvent::UserIdle:
        return shortIdle(now);
    case DeviceLifecycleEvent::DisplayOff:
        if (preserve_desktop_topology_on_idle_) return shortIdle(now);
        return fullPowerOff();
    case DeviceLifecycleEvent::UserBusy:
    case DeviceLifecycleEvent::DisplayOn:
    case DeviceLifecycleEvent::Resume:
        return restoreActive();
    case DeviceLifecycleEvent::Suspend:
    case DeviceLifecycleEvent::Shutdown:
        return fullPowerOff();
    }
    return {};
}

DevicePlan DeviceCoordinatorCore::onDeadline(IdleCoordinator::TimePoint now) {
    if (!idle_.onDeadline(now)) {
        DevicePlan plan;
        plan.deadline = idle_.deadline();
        return plan;
    }

    DevicePlan plan;
    plan.deadline_changed = true;
    restore_needed_ = true;
    plan.actions = {
        DeviceAction::VizioPowerOff,
        DeviceAction::SamsungRestoreScreenOff,
        DeviceAction::SamsungPowerOff,
    };
    return plan;
}
