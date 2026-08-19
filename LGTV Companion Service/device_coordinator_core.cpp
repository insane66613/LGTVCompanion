#include "device_coordinator_core.h"

DeviceCoordinatorCore::DeviceCoordinatorCore(
    IdleCoordinator::Duration extended_idle_delay,
    bool preserve_desktop_topology_on_idle)
    : idle_(extended_idle_delay),
      preserve_desktop_topology_on_idle_(preserve_desktop_topology_on_idle) {
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
