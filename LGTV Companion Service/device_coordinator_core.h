#pragma once

#include "idle_coordinator.h"

#include <optional>
#include <vector>

enum class DeviceLifecycleEvent {
    UserIdle,
    UserBusy,
    DisplayOff,
    DisplayOn,
    Resume,
    Suspend,
    Shutdown,
};

enum class DeviceAction {
    SamsungEnableScreenOff,
    SamsungRestoreScreenOff,
    SamsungPowerOn,
    SamsungPowerOff,
    VizioBlankPanel,
    VizioWake,
    VizioPowerOff,
};

struct DevicePlan {
    std::vector<DeviceAction> actions;
    std::optional<IdleCoordinator::TimePoint> deadline;
    bool deadline_changed{false};
};

class DeviceCoordinatorCore {
public:
    DeviceCoordinatorCore(IdleCoordinator::Duration extended_idle_delay,
                          bool preserve_desktop_topology_on_idle);

    DevicePlan onEvent(DeviceLifecycleEvent event, IdleCoordinator::TimePoint now);
    DevicePlan onDeadline(IdleCoordinator::TimePoint now);

private:
    DevicePlan shortIdle(IdleCoordinator::TimePoint now);
    DevicePlan restoreActive();
    DevicePlan fullPowerOff();

    IdleCoordinator idle_;
    bool preserve_desktop_topology_on_idle_;
};
