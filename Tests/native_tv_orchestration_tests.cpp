#include "../LGTV Companion Service/idle_coordinator.h"
#include "../LGTV Companion Service/samsung_controller.h"
#include "../LGTV Companion Service/vizio_controller.h"
#include "../LGTV Companion Service/device_coordinator_core.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename T>
void check_eq(const T& actual, const T& expected, const std::string& message) {
    if (!(actual == expected)) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void test_first_idle_arms_once() {
    IdleCoordinator idle(60min);
    const auto t0 = Clock::time_point{} + 10min;
    check(idle.onIdle(t0), "first idle must transition active -> idle");
    check(idle.deadline().has_value(), "first idle must arm extended deadline");
    check(*idle.deadline() == t0 + 60min, "deadline must be first idle + delay");
    check(!idle.onIdle(t0 + 15min), "repeated idle must be idempotent");
    check(*idle.deadline() == t0 + 60min, "repeated idle must not refresh deadline");
}

void test_busy_cancels_deadline() {
    IdleCoordinator idle(60min);
    idle.onIdle(Clock::time_point{});
    check(idle.onBusy(), "busy must transition idle -> active");
    check(!idle.deadline().has_value(), "busy must cancel extended deadline");
    check(idle.state() == IdleCoordinator::State::Active, "busy must restore Active state");
}

void test_extended_idle_fires_once() {
    IdleCoordinator idle(60min);
    const auto t0 = Clock::time_point{};
    idle.onIdle(t0);
    check(!idle.onDeadline(t0 + 59min), "deadline must not fire early");
    check(idle.onDeadline(t0 + 60min), "deadline must fire when due");
    check(idle.state() == IdleCoordinator::State::ExtendedIdle, "deadline must enter ExtendedIdle");
    check(!idle.onDeadline(t0 + 61min), "extended deadline must fire exactly once");
}

void check_keys(const std::vector<SamsungCommandStep>& steps,
                const std::vector<SamsungRemoteKey>& expected,
                const std::string& message) {
    if (steps.size() != expected.size()) {
        ++failures;
        std::cerr << "FAIL: " << message << " (size)\n";
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (steps[i].key != expected[i]) {
            ++failures;
            std::cerr << "FAIL: " << message << " (index " << i << ")\n";
            return;
        }
    }
}

void test_samsung_pictureoff_is_powered_and_blanked() {
    check(SamsungController::isPowered(SamsungPowerState::PictureOff), "pictureoff must count as powered");
    check(SamsungController::isBlanked(SamsungPowerState::PictureOff), "pictureoff must count as blanked");
    check(!SamsungController::isPowered(SamsungPowerState::Standby), "standby must not count as powered");
}

void test_samsung_enable_leaves_accessibility_open() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    const auto plan = controller.planEnableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Accessibility, SamsungRemoteKey::Enter},
               "enable must be AD then ENTER only");
    check(plan.size() == 2 && plan[0].delay_after == 1400ms, "enable must wait 1400ms after AD");
    check(controller.accessibilityState() == SamsungAccessibilityState::OpenEnabled,
          "successful enable plan must track menu open/enabled");
}

void test_samsung_restore_open_awake_menu() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    const auto plan = controller.planDisableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Enter, SamsungRemoteKey::Return},
               "awake known-open restore must ENTER then RETURN");
}

void test_samsung_restore_pictureoff_preserves_selection() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::PictureOff);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    const auto plan = controller.planDisableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Enter, SamsungRemoteKey::Enter, SamsungRemoteKey::Return},
               "pictureoff known-open restore must wake with ENTER and preserve first item");
    check(plan[0].delay_after == 1200ms, "pictureoff wake must wait 1200ms");
}

void test_samsung_restart_safe_unknown_menu_restore() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::PictureOff);
    controller.assumeAccessibilityState(SamsungAccessibilityState::Unknown);
    const auto plan = controller.planDisableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Enter, SamsungRemoteKey::Accessibility,
                      SamsungRemoteKey::Enter, SamsungRemoteKey::Return},
               "unknown menu restore must wake, reopen Accessibility, toggle first item, close");
    check(plan[1].delay_after == 1400ms, "reopened Accessibility must wait 1400ms");
}

void test_samsung_power_toggle_is_state_guarded() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    check(controller.shouldSendPowerToggleForOff(), "on -> off may send KEY_POWER");
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.shouldSendPowerToggleForOff(), "pictureoff -> off may send KEY_POWER");
    controller.observePowerState(SamsungPowerState::Standby);
    check(!controller.shouldSendPowerToggleForOff(), "standby -> off must suppress KEY_POWER");
    controller.observePowerState(SamsungPowerState::Unknown);
    check(!controller.shouldSendPowerToggleForOff(), "unknown -> off must suppress KEY_POWER");
}

void test_vizio_blank_is_distinct_from_poweroff() {
    VizioController controller;
    controller.observeState(VizioPowerState::On, false);
    const auto blank = controller.planIdleBlank();
    check(blank == VizioAction::BlankPanel, "short idle must blank Vizio panel");
    controller.observeState(VizioPowerState::On, true);
    check(controller.planIdleBlank() == VizioAction::None, "repeated idle must not power off an already blanked Vizio");
    check(controller.planExtendedIdle() == VizioAction::PowerOff, "extended idle must power Vizio off");
}

void test_pictureoff_does_not_invent_menu_open_after_restart() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.accessibilityState() == SamsungAccessibilityState::Unknown,
          "pictureoff alone must not prove Accessibility menu is open");
}

bool contains_action(const DevicePlan& plan, DeviceAction action) {
    for (const auto candidate : plan.actions) {
        if (candidate == action) return true;
    }
    return false;
}

void test_device_coordinator_maps_idle_and_busy() {
    DeviceCoordinatorCore core(60min, true);
    const auto t0 = Clock::time_point{} + 5min;
    const auto first = core.onEvent(DeviceLifecycleEvent::UserIdle, t0);
    check(contains_action(first, DeviceAction::SamsungEnableScreenOff), "idle must blank Samsung");
    check(contains_action(first, DeviceAction::VizioBlankPanel), "idle must blank Vizio");
    check(first.deadline_changed, "first idle must arm deadline");
    check(first.deadline == std::optional<Clock::time_point>{t0 + 60min}, "first idle deadline must be fixed");

    const auto repeated = core.onEvent(DeviceLifecycleEvent::UserIdle, t0 + 10min);
    check(!repeated.deadline_changed, "repeated idle must not change deadline");
    check(repeated.deadline == first.deadline, "repeated idle must preserve original deadline");

    const auto busy = core.onEvent(DeviceLifecycleEvent::UserBusy, t0 + 20min);
    check(contains_action(busy, DeviceAction::SamsungRestoreScreenOff), "busy must restore Samsung");
    check(contains_action(busy, DeviceAction::VizioWake), "busy must wake/unblank Vizio");
    check(!busy.deadline.has_value(), "busy must cancel deadline");
}

void test_device_coordinator_extended_idle_orders_samsung_restore_before_poweroff() {
    DeviceCoordinatorCore core(60min, true);
    const auto t0 = Clock::time_point{};
    core.onEvent(DeviceLifecycleEvent::UserIdle, t0);
    const auto plan = core.onDeadline(t0 + 60min);
    check(contains_action(plan, DeviceAction::VizioPowerOff), "extended idle must power Vizio off");
    auto restore = std::find(plan.actions.begin(), plan.actions.end(), DeviceAction::SamsungRestoreScreenOff);
    auto poweroff = std::find(plan.actions.begin(), plan.actions.end(), DeviceAction::SamsungPowerOff);
    check(restore != plan.actions.end() && poweroff != plan.actions.end() && restore < poweroff,
          "extended idle must disable Samsung Screen Off Mode before KEY_POWER off");
}

void test_device_coordinator_topology_safe_displayoff_and_shutdown() {
    DeviceCoordinatorCore core(60min, true);
    const auto t0 = Clock::time_point{};
    const auto displayoff = core.onEvent(DeviceLifecycleEvent::DisplayOff, t0);
    check(contains_action(displayoff, DeviceAction::SamsungEnableScreenOff), "topology-safe display off must panel-blank Samsung");
    check(!contains_action(displayoff, DeviceAction::SamsungPowerOff), "topology-safe display off must not power Samsung off");
    check(!contains_action(displayoff, DeviceAction::VizioPowerOff), "topology-safe display off must not power Vizio off");

    const auto shutdown = core.onEvent(DeviceLifecycleEvent::Shutdown, t0 + 1min);
    check(contains_action(shutdown, DeviceAction::SamsungRestoreScreenOff), "shutdown must first restore persistent Samsung mode");
    check(contains_action(shutdown, DeviceAction::SamsungPowerOff), "shutdown must power Samsung off");
    check(contains_action(shutdown, DeviceAction::VizioPowerOff), "shutdown must power Vizio off");
    check(!shutdown.deadline.has_value(), "shutdown must cancel extended idle deadline");
}

}  // namespace

int main() {
    test_first_idle_arms_once();
    test_busy_cancels_deadline();
    test_extended_idle_fires_once();
    test_samsung_pictureoff_is_powered_and_blanked();
    test_samsung_enable_leaves_accessibility_open();
    test_samsung_restore_open_awake_menu();
    test_samsung_restore_pictureoff_preserves_selection();
    test_samsung_restart_safe_unknown_menu_restore();
    test_samsung_power_toggle_is_state_guarded();
    test_vizio_blank_is_distinct_from_poweroff();
    test_pictureoff_does_not_invent_menu_open_after_restart();
    test_device_coordinator_maps_idle_and_busy();
    test_device_coordinator_extended_idle_orders_samsung_restore_before_poweroff();
    test_device_coordinator_topology_safe_displayoff_and_shutdown();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "native_tv_orchestration_tests: PASS\n";
    return EXIT_SUCCESS;
}
