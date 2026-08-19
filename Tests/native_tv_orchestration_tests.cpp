#include "../LGTV Companion Service/idle_coordinator.h"
#include "../LGTV Companion Service/samsung_controller.h"
#include "../LGTV Companion Service/vizio_controller.h"
#include "../LGTV Companion Service/device_coordinator_core.h"
#include "../Common/external_tv_settings.h"
#include "../Common/external_tv_diagnostics.h"

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

void test_samsung_idle_when_already_pictureoff_is_noop() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.planEnableScreenOff().empty(),
          "idle while already pictureoff must not toggle persistent Screen Off Mode");
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

void test_samsung_restore_pictureoff_reopens_accessibility_before_disable() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.accessibilityState() == SamsungAccessibilityState::Unknown,
          "pictureoff must invalidate cached Accessibility-menu-open state");
    const auto plan = controller.planDisableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Enter, SamsungRemoteKey::Accessibility,
                      SamsungRemoteKey::Enter, SamsungRemoteKey::Return},
               "pictureoff restore must wake, explicitly reopen Accessibility, disable first item, and close");
    check(plan[0].delay_after == 1200ms, "pictureoff wake must wait 1200ms");
    check(plan[1].delay_after == 1400ms, "pictureoff restore must wait after reopening Accessibility");
}

void test_samsung_external_pictureoff_to_on_invalidates_cached_menu_state() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    controller.observePowerState(SamsungPowerState::PictureOff);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    controller.observePowerState(SamsungPowerState::On);
    check(controller.accessibilityState() == SamsungAccessibilityState::Unknown,
          "externally observed pictureoff-to-on transition must invalidate cached menu state");
    check(controller.planDisableScreenOff().empty(),
          "awake TV with externally invalidated menu state must not blindly toggle Screen Off Mode");
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
    check(controller.planDisableScreenOff().empty(), "awake unknown restart state must not blindly toggle Accessibility");
    check(controller.shouldSendPowerToggleForOff(), "on -> off may send KEY_POWER");
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.shouldSendPowerToggleForOff(), "pictureoff -> off may send KEY_POWER");
    controller.observePowerState(SamsungPowerState::Standby);
    check(!controller.shouldSendPowerToggleForOff(), "standby -> off must suppress KEY_POWER");
    controller.observePowerState(SamsungPowerState::Unknown);
    check(!controller.shouldSendPowerToggleForOff(), "unknown -> off must suppress KEY_POWER");
}

void test_samsung_channel_authorization_frames() {
    check(SamsungController::channelAuthorizationFromJson(R"({"event":"ms.channel.connect","data":{"token":"123"}})") ==
              SamsungChannelAuthorization::Authorized,
          "Tizen ms.channel.connect must authorize remote-key delivery");
    check(SamsungController::channelAuthorizationFromJson(R"({"event":"ms.channel.unauthorized"})") ==
              SamsungChannelAuthorization::Unauthorized,
          "Tizen unauthorized event must reject remote-key delivery");
    check(SamsungController::channelAuthorizationFromJson(R"({"event":"ms.channel.clientConnect"})") ==
              SamsungChannelAuthorization::Pending,
          "unrelated Tizen frames must not authorize key delivery");
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

void test_vizio_smartcast_power_value_mapping() {
    check(VizioController::fromSmartCastPowerValue(0) == VizioPowerState::Off,
          "SmartCast power_mode VALUE=0 must map to off");
    check(VizioController::fromSmartCastPowerValue(1) == VizioPowerState::On,
          "SmartCast power_mode VALUE=1 must map to on");
    check(VizioController::fromSmartCastPowerValue(2) == VizioPowerState::On,
          "SmartCast nonzero power_mode values must map to on like pyvizio");
}

void test_vizio_power_off_command_is_state_guarded() {
    check(VizioController::shouldSendPowerOff(VizioPowerState::On),
          "Vizio POW_OFF may be sent only from verified ON state");
    check(!VizioController::shouldSendPowerOff(VizioPowerState::Off),
          "Vizio POW_OFF must be suppressed when already OFF because this model toggles back ON");
    check(!VizioController::shouldSendPowerOff(VizioPowerState::Unknown),
          "Vizio POW_OFF must be suppressed when power state is unknown");

    check(VizioController::shouldRetryPowerOffAfterVerification(VizioPowerState::On, VizioPowerState::On),
          "one POW_OFF retry is allowed only when the first verified transition still proves ON");
    check(!VizioController::shouldRetryPowerOffAfterVerification(VizioPowerState::On, VizioPowerState::Off),
          "POW_OFF must never retry after OFF is observed");
    check(!VizioController::shouldRetryPowerOffAfterVerification(VizioPowerState::On, VizioPowerState::Unknown),
          "POW_OFF must never retry after state becomes unknown");
}

void test_vizio_wake_is_state_aware() {
    check(VizioController::planWakeForObservedPowerState(VizioPowerState::Off) == VizioAction::PowerOn,
          "busy/resume must power on a verified-OFF Vizio instead of trusting BACK ACK");
    check(VizioController::planWakeForObservedPowerState(VizioPowerState::On) == VizioAction::UnblankPanel,
          "busy/resume may unblank a verified-ON Vizio");
    check(VizioController::planWakeForObservedPowerState(VizioPowerState::Unknown) == VizioAction::None,
          "unknown Vizio state must stay in conservative fallback handling");
}

void test_vizio_key_payload_matches_pyvizio_shape() {
    const auto payload = nlohmann::json::parse(VizioController::smartCastKeyPayload(11, 0));
    check(payload.value("_url", "") == "/key_command/",
          "SmartCast key payload must include pyvizio _url metadata");
    check(payload.contains("KEYLIST") && payload["KEYLIST"].is_array() && payload["KEYLIST"].size() == 1,
          "SmartCast key payload must contain one KEYLIST entry");
    const auto& key = payload["KEYLIST"][0];
    check(key.value("CODESET", -1) == 11 && key.value("CODE", -1) == 0 && key.value("ACTION", "") == "KEYPRESS",
          "SmartCast key payload must preserve codeset, code, and KEYPRESS action");
}

void test_vizio_smartcast_hashval_preserves_unsigned_32_bit_range() {
    const std::string response = R"({"STATUS":{"RESULT":"SUCCESS"},"ITEMS":[{"HASHVAL":3453326231}]})";
    const auto hashval = VizioController::smartCastHashValFromJson(response);
    check(hashval.has_value(), "SmartCast HASHVAL must parse from nested response");
    check(hashval == std::optional<std::uint64_t>{3453326231ULL},
          "SmartCast HASHVAL above INT_MAX must be preserved without signed overflow");
    check(!VizioController::smartCastHashValFromJson(R"({"ITEMS":[{"HASHVAL":-1}]})").has_value(),
          "negative SmartCast HASHVAL must be rejected");
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
    check(repeated.actions.empty(), "repeated idle must not re-send Samsung/Vizio blank actions");

    const auto busy = core.onEvent(DeviceLifecycleEvent::UserBusy, t0 + 20min);
    check(contains_action(busy, DeviceAction::SamsungPowerOn), "busy must power Samsung on if extended idle had shut it down");
    check(contains_action(busy, DeviceAction::SamsungRestoreScreenOff), "busy must restore Samsung");
    check(contains_action(busy, DeviceAction::VizioWake), "busy must wake/unblank Vizio");
    check(!busy.deadline.has_value(), "busy must cancel deadline");

    const auto repeated_busy = core.onEvent(DeviceLifecycleEvent::UserBusy, t0 + 21min);
    check(repeated_busy.actions.empty(), "repeated busy while already active must not replay remote restore commands");
    check(!repeated_busy.deadline_changed, "repeated busy must not mutate deadline state");
}

void test_device_coordinator_initial_active_reconciliation_runs_once() {
    DeviceCoordinatorCore core(60min, true);
    const auto t0 = Clock::time_point{};
    const auto first_busy = core.onEvent(DeviceLifecycleEvent::UserBusy, t0);
    check(contains_action(first_busy, DeviceAction::SamsungPowerOn),
          "first active event after service start must reconcile Samsung");
    check(contains_action(first_busy, DeviceAction::VizioWake),
          "first active event after service start must reconcile Vizio");
    const auto repeated_busy = core.onEvent(DeviceLifecycleEvent::UserBusy, t0 + 1s);
    check(repeated_busy.actions.empty(),
          "second active event after startup reconciliation must be idempotent");
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

    const auto resume = core.onEvent(DeviceLifecycleEvent::Resume, t0 + 2min);
    check(contains_action(resume, DeviceAction::SamsungPowerOn), "resume after shutdown must restore Samsung");
    check(contains_action(resume, DeviceAction::VizioWake), "resume after shutdown must restore Vizio");
}

void test_external_tv_settings_round_trip_and_redaction() {
    nlohmann::json node = {
        {"Enabled", true}, {"PreserveDesktopTopologyOnIdle", true}, {"ExtendedIdleMinutes", 60},
        {"Samsung", {{"Enabled", true}, {"IP", "192.0.2.10"}, {"MAC", "AA:BB:CC:DD:EE:FF"}, {"Token", "sensitive-token"}}},
        {"Vizio", {{"Enabled", true}, {"IP", "192.0.2.20"}, {"MAC", "11:22:33:44:55:66"}, {"Auth", "sensitive-auth"}}}
    };
    const auto settings = ExternalTvSettings::fromJson(node);
    check(settings.enabled && settings.samsung.enabled && settings.vizio.enabled, "external device enable flags must parse");
    check(settings.extended_idle_minutes == 60, "extended idle delay must parse");
    check(settings.toJson() == node, "external settings must round-trip without schema loss");
    const auto redacted = settings.toRedactedJson();
    check(redacted["Samsung"]["Token"] == "<redacted>", "Samsung token must be redacted in logs");
    check(redacted["Vizio"]["Auth"] == "<redacted>", "Vizio auth must be redacted in logs");
}

void test_external_tv_settings_safe_defaults() {
    const auto settings = ExternalTvSettings::fromJson(nlohmann::json::object());
    check(!settings.enabled && !settings.samsung.enabled && !settings.vizio.enabled,
          "unconfigured external TVs must remain disabled");
    check(settings.preserve_desktop_topology_on_idle, "topology preservation must default on");
    check(settings.extended_idle_minutes == 60, "extended idle default must be 60 minutes");
}

void test_external_tv_diagnostic_request_contract() {
    const nlohmann::json wire = {
        {"namespace", "external_tv"}, {"request_id", "req-123"},
        {"device", "samsung"}, {"operation", "power_off"}
    };
    std::string error;
    const auto request = ExternalTvDiagnosticRequest::fromJson(wire, &error);
    check(request.has_value(), "valid external-TV diagnostic request must parse");
    check(request && request->request_id == "req-123", "diagnostic request ID must be preserved");
    check(request && request->device == ExternalTvDiagnosticDevice::Samsung,
          "diagnostic request must preserve Samsung selector");
    check(request && request->operation == ExternalTvDiagnosticOperation::PowerOff,
          "diagnostic request must preserve operation");
    check(request && request->toJson() == wire, "diagnostic request must round-trip canonically");
}

void test_external_tv_diagnostic_request_rejects_malformed_or_secret_payloads() {
    std::string error;
    check(!ExternalTvDiagnosticRequest::fromJson({{"namespace", "external_tv"}, {"device", "vizio"}, {"operation", "probe"}}, &error),
          "diagnostic request without request_id must fail closed");
    check(!ExternalTvDiagnosticRequest::fromJson({{"namespace", "external_tv"}, {"request_id", "r"}, {"device", "vizio"}, {"operation", "shutdown"}}, &error),
          "unsupported host-like diagnostic operation must fail closed");
    check(!ExternalTvDiagnosticRequest::fromJson({{"namespace", "external_tv"}, {"request_id", "r"}, {"device", "samsung"}, {"operation", "probe"}, {"Token", "secret"}}, &error),
          "diagnostic IPC must reject credential-bearing fields");
}

void test_external_tv_diagnostic_response_contract_and_semantics() {
    ExternalTvDiagnosticResponse response;
    response.request_id = "req-123";
    response.device = ExternalTvDiagnosticDevice::Vizio;
    response.operation = ExternalTvDiagnosticOperation::PowerOn;
    response.accepted = true;
    response.executed = true;
    response.verified = true;
    response.resulting_state = "on";
    response.message = "Power-on verified";
    check(response.semanticsValid(), "verified diagnostic response must require accepted+executed and known resulting state");
    auto wire = response.toJson();
    check(wire.value("request_id", "") == "req-123", "diagnostic response must echo request ID");
    check(!wire.contains("Token") && !wire.contains("Auth") && !wire.contains("credential"),
          "diagnostic response must contain no credential fields");
    std::string error;
    const auto parsed = ExternalTvDiagnosticResponse::fromJson(wire, &error);
    check(parsed.has_value() && parsed->matchesRequest("req-123"),
          "diagnostic response must support request-ID correlation");

    wire["executed"] = false;
    check(!ExternalTvDiagnosticResponse::fromJson(wire, &error),
          "verified=true with executed=false must be rejected as impossible semantics");
}

}  // namespace

int main() {
    test_first_idle_arms_once();
    test_busy_cancels_deadline();
    test_extended_idle_fires_once();
    test_samsung_pictureoff_is_powered_and_blanked();
    test_samsung_idle_when_already_pictureoff_is_noop();
    test_samsung_enable_leaves_accessibility_open();
    test_samsung_restore_open_awake_menu();
    test_samsung_restore_pictureoff_reopens_accessibility_before_disable();
    test_samsung_external_pictureoff_to_on_invalidates_cached_menu_state();
    test_samsung_restart_safe_unknown_menu_restore();
    test_samsung_power_toggle_is_state_guarded();
    test_samsung_channel_authorization_frames();
    test_vizio_blank_is_distinct_from_poweroff();
    test_vizio_smartcast_power_value_mapping();
    test_vizio_power_off_command_is_state_guarded();
    test_vizio_wake_is_state_aware();
    test_vizio_key_payload_matches_pyvizio_shape();
    test_vizio_smartcast_hashval_preserves_unsigned_32_bit_range();
    test_pictureoff_does_not_invent_menu_open_after_restart();
    test_device_coordinator_maps_idle_and_busy();
    test_device_coordinator_initial_active_reconciliation_runs_once();
    test_device_coordinator_extended_idle_orders_samsung_restore_before_poweroff();
    test_device_coordinator_topology_safe_displayoff_and_shutdown();
    test_external_tv_settings_round_trip_and_redaction();
    test_external_tv_settings_safe_defaults();
    test_external_tv_diagnostic_request_contract();
    test_external_tv_diagnostic_request_rejects_malformed_or_secret_payloads();
    test_external_tv_diagnostic_response_contract_and_semantics();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "native_tv_orchestration_tests: PASS\n";
    return EXIT_SUCCESS;
}
