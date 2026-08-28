#include "../LGTV Companion Service/idle_coordinator.h"
#include "../LGTV Companion Service/samsung_controller.h"
#include "../LGTV Companion Service/vizio_controller.h"
#include "../LGTV Companion Service/device_coordinator_core.h"
#include "../LGTV Companion Service/external_tv_transport.h"
#include "../Common/external_tv_settings.h"
#include "../Common/external_tv_diagnostics.h"
#include "../Common/ipc_v2.h"
#include "../Common/taskbar_recovery.h"

#include <algorithm>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

struct AsyncPipeTestState {
    std::atomic<bool> received{false};
    std::mutex mutex;
    std::wstring payload;
};

void asyncPipeTestCallback(std::wstring message, LPVOID object) {
    auto* state = static_cast<AsyncPipeTestState*>(object);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->payload = std::move(message);
    }
    state->received = true;
}

void test_ipc_client_async_send_runs_on_owned_io_thread() {
    const auto caller_thread = std::this_thread::get_id();
    const std::wstring pipe_name = L"\\\\.\\pipe\\LGTVCompanionAsyncSendTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    AsyncPipeTestState server_state;
    IpcServer2 server(pipe_name, asyncPipeTestCallback, &server_state, true);
    IpcClient2 client(pipe_name, nullptr, nullptr, true);

    auto completion = std::make_shared<std::promise<std::pair<bool, std::thread::id>>>();
    auto future = completion->get_future();
    client.sendAsync(L"async-send-test", [completion](bool ok) {
        completion->set_value({ok, std::this_thread::get_id()});
    });

    check(future.wait_for(3s) == std::future_status::ready,
          "async IPC send must complete without blocking the caller");
    if (future.wait_for(0s) == std::future_status::ready) {
        const auto result = future.get();
        check(result.first, "async IPC send must report success");
        check(result.second != caller_thread,
              "async IPC send must execute on the client's owned IO thread");
    }

    const auto receive_deadline = Clock::now() + 3s;
    while (!server_state.received && Clock::now() < receive_deadline)
        std::this_thread::sleep_for(10ms);
    check(server_state.received, "async IPC send must reach the named-pipe server");
    if (server_state.received) {
        std::lock_guard<std::mutex> lock(server_state.mutex);
        check(server_state.payload == L"async-send-test", "async IPC payload must be preserved");
    }
    client.terminate();
    server.terminate();

    const std::wstring missing_pipe = pipe_name + L"-missing";
    IpcClient2 unavailable_client(missing_pipe, nullptr, nullptr, true);
    auto failure_completion = std::make_shared<std::promise<std::pair<bool, std::thread::id>>>();
    auto failure_future = failure_completion->get_future();
    const auto call_started = Clock::now();
    unavailable_client.sendAsync(L"expected-failure", [failure_completion](bool ok) {
        failure_completion->set_value({ok, std::this_thread::get_id()});
    });
    const auto call_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - call_started);
    check(call_elapsed < 100ms,
          "async IPC submission must return promptly when the service pipe is unavailable");
    check(failure_future.wait_for(2s) == std::future_status::ready,
          "async IPC retry window must fail closed within its bounded interval");
    if (failure_future.wait_for(0s) == std::future_status::ready) {
        const auto result = failure_future.get();
        check(!result.first, "async IPC send must report failure when the pipe stays unavailable");
        check(result.second != caller_thread,
              "async IPC failure completion must stay on the client's owned IO thread");
    }
    unavailable_client.terminate();
}

struct SyncPipeStressState {
    std::atomic<unsigned> received_chars{0};
};

void syncPipeStressCallback(std::wstring message, LPVOID object) {
    auto* state = static_cast<SyncPipeStressState*>(object);
    if (std::all_of(message.begin(), message.end(), [](wchar_t ch) { return ch == L'x'; }))
        state->received_chars.fetch_add(static_cast<unsigned>(message.size()), std::memory_order_relaxed);
}

void test_ipc_client_sync_send_does_not_poison_owned_iocp() {
    const std::wstring pipe_name = L"\\\\.\\pipe\\LGTVCompanionSyncSendStress-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    SyncPipeStressState state;
    IpcServer2 server(pipe_name, syncPipeStressCallback, &state, false);
    IpcClient2 client(pipe_name, nullptr, nullptr, false);

    constexpr unsigned target = 20000;
    unsigned sent = 0;
    const auto send_deadline = Clock::now() + 10s;
    while (sent < target && Clock::now() < send_deadline) {
        if (client.send(L"x"))
            ++sent;
        else
            std::this_thread::sleep_for(1ms);
    }
    check(sent == target, "synchronous IPC stress must complete all writes without corrupting the owned IOCP");

    const auto receive_deadline = Clock::now() + 3s;
    while (state.received_chars.load(std::memory_order_relaxed) < sent && Clock::now() < receive_deadline)
        std::this_thread::sleep_for(1ms);
    check(state.received_chars.load(std::memory_order_relaxed) == sent,
          "byte-mode IPC stress must preserve every synchronous write");

    client.terminate();
    server.terminate();
}

struct PendingSyncPipeState {
    std::atomic<bool> client_read_seen{false};
};

void pendingSyncPipeCallback(std::wstring message, LPVOID object) {
    auto* state = static_cast<PendingSyncPipeState*>(object);
    if (message == L"ready")
        state->client_read_seen.store(true, std::memory_order_release);
}

void test_ipc_client_pending_sync_send_does_not_enter_owned_iocp() {
    const std::wstring pipe_name = L"\\\\.\\pipe\\LGTVCompanionPendingSyncSend-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    constexpr std::size_t payload_chars = 4 * 1024 * 1024;
    const DWORD payload_bytes = static_cast<DWORD>(payload_chars * sizeof(wchar_t));
    PendingSyncPipeState state;
    std::atomic<bool> server_failed{false};
    std::atomic<bool> begin_drain{false};
    std::atomic<unsigned long long> received_bytes{0};

    std::thread server_thread([&] {
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            server_failed.store(true, std::memory_order_release);
            return;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            server_failed.store(true, std::memory_order_release);
            CloseHandle(pipe);
            return;
        }
        const wchar_t ready[] = L"ready";
        DWORD written = 0;
        if (!WriteFile(pipe, ready, 5 * sizeof(wchar_t), &written, nullptr) || written != 5 * sizeof(wchar_t))
            server_failed.store(true, std::memory_order_release);

        while (!begin_drain.load(std::memory_order_acquire) && !server_failed.load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(250ms);

        std::vector<char> buffer(64 * 1024);
        while (received_bytes.load(std::memory_order_relaxed) < payload_bytes) {
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) || bytes_read == 0) {
                server_failed.store(true, std::memory_order_release);
                break;
            }
            received_bytes.fetch_add(bytes_read, std::memory_order_relaxed);
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });

    IpcClient2 client(pipe_name, pendingSyncPipeCallback, &state, false);
    const auto association_deadline = Clock::now() + 3s;
    while (!state.client_read_seen.load(std::memory_order_acquire) &&
           !server_failed.load(std::memory_order_acquire) && Clock::now() < association_deadline)
        std::this_thread::sleep_for(1ms);
    check(state.client_read_seen.load(std::memory_order_acquire),
          "client read callback must prove the pipe is owned by the Asio IOCP before the pending write");

    std::wstring payload(payload_chars, L'p');
    begin_drain.store(true, std::memory_order_release);
    const bool sent = client.send(payload);
    check(sent, "pending synchronous IPC write must complete without poisoning the owned IOCP");

    server_thread.join();
    check(!server_failed.load(std::memory_order_acquire), "pending-write test server must complete cleanly");
    check(received_bytes.load(std::memory_order_relaxed) == payload_bytes,
          "pending synchronous IPC write must deliver the complete payload");
    client.terminate();
}

void test_taskbar_recovery_coalesces_and_fires_once() {
    TaskbarRecoveryScheduler scheduler(1500ms);
    const auto t0 = TaskbarRecoveryScheduler::Clock::time_point{};

    scheduler.schedule(t0);
    scheduler.schedule(t0 + 500ms);
    check(scheduler.pending(), "taskbar recovery must stay armed after repeated display events");
    check(!scheduler.consumeIfDue(t0 + 1499ms),
          "repeated display events must push the recovery deadline out");
    check(scheduler.consumeIfDue(t0 + 2000ms),
          "taskbar recovery must fire after the final debounce delay");
    check(!scheduler.consumeIfDue(t0 + 10s),
          "taskbar recovery must be one-shot and never become periodic");
}

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

void test_samsung_restore_pictureoff_invalidates_menu_and_reopens() {
    SamsungController controller;
    controller.observePowerState(SamsungPowerState::On);
    controller.assumeAccessibilityState(SamsungAccessibilityState::OpenEnabled);
    controller.observePowerState(SamsungPowerState::PictureOff);
    check(controller.accessibilityState() == SamsungAccessibilityState::Unknown,
          "pictureoff must invalidate cached Accessibility-menu-open state");
    const auto plan = controller.planDisableScreenOff();
    check_keys(plan, {SamsungRemoteKey::Enter, SamsungRemoteKey::Accessibility,
                      SamsungRemoteKey::Enter, SamsungRemoteKey::Return},
               "pictureoff restore must wake, reopen Accessibility, disable first item, then close");
    check(plan[0].delay_after == 1200ms, "pictureoff wake must wait 1200ms");
    check(plan[1].delay_after == 1400ms, "pictureoff restore must wait after reopening Accessibility");
    check(plan[2].delay_after == 350ms, "disable toggle must wait 350ms before closing Accessibility");
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

void test_device_coordinator_read_only_lifecycle_previews() {
    DeviceCoordinatorCore core(60min, true);
    const auto t0 = Clock::time_point{} + 10min;
    const auto initial = core.snapshot();
    check(initial.idle_state == IdleCoordinator::State::Active,
          "fresh coordinator snapshot must report active state");
    check(!initial.deadline.has_value(), "fresh coordinator snapshot must have no deadline");

    const auto idle = core.previewEvent(DeviceLifecycleEvent::UserIdle, t0);
    check(idle.after.idle_state == IdleCoordinator::State::Idle,
          "User Idle preview must report Idle post-state");
    check(idle.after.deadline == std::optional<Clock::time_point>{t0 + 60min},
          "User Idle preview must expose the extended-idle deadline");
    check(contains_action(idle.plan, DeviceAction::SamsungEnableScreenOff) &&
          contains_action(idle.plan, DeviceAction::VizioBlankPanel),
          "User Idle preview must reuse production panel-blank policy");
    check(core.snapshot().idle_state == IdleCoordinator::State::Active &&
          !core.snapshot().deadline.has_value(),
          "User Idle preview must not mutate the live coordinator");

    const auto busy = core.previewEvent(DeviceLifecycleEvent::UserBusy, t0 + 1min);
    check(busy.after.idle_state == IdleCoordinator::State::Active,
          "User Busy preview must report Active post-state");
    check(contains_action(busy.plan, DeviceAction::SamsungPowerOn) &&
          contains_action(busy.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(busy.plan, DeviceAction::VizioWake),
          "User Busy preview must reuse production restore policy");

    const auto extended = core.previewExtendedIdle(t0);
    check(extended.after.idle_state == IdleCoordinator::State::ExtendedIdle,
          "Extended Idle preview must report ExtendedIdle post-state");
    check(contains_action(extended.plan, DeviceAction::VizioPowerOff) &&
          contains_action(extended.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(extended.plan, DeviceAction::SamsungPowerOff),
          "Extended Idle preview must reuse production full-power-off policy");

    const auto suspend = core.previewEvent(DeviceLifecycleEvent::Suspend, t0 + 2min);
    check(contains_action(suspend.plan, DeviceAction::VizioPowerOff) &&
          contains_action(suspend.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(suspend.plan, DeviceAction::SamsungPowerOff),
          "Suspend preview must reuse production full-power-off policy");

    const auto shutdown = core.previewEvent(DeviceLifecycleEvent::Shutdown, t0 + 3min);
    check(contains_action(shutdown.plan, DeviceAction::VizioPowerOff) &&
          contains_action(shutdown.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(shutdown.plan, DeviceAction::SamsungPowerOff),
          "Shutdown preview must reuse production full-power-off policy");

    const auto final_state = core.snapshot();
    check(final_state.idle_state == initial.idle_state && final_state.deadline == initial.deadline,
          "all lifecycle simulations must be read-only with respect to live coordinator state");

    core.onEvent(DeviceLifecycleEvent::UserIdle, t0);
    const auto live_idle = core.snapshot();
    const auto idle_while_idle = core.previewEvent(DeviceLifecycleEvent::UserIdle, t0 + 1min);
    check(contains_action(idle_while_idle.plan, DeviceAction::SamsungEnableScreenOff) &&
          contains_action(idle_while_idle.plan, DeviceAction::VizioBlankPanel),
          "User Idle diagnostic must exercise first-stage blanking even when live state is already Idle");
    check(core.snapshot().idle_state == live_idle.idle_state && core.snapshot().deadline == live_idle.deadline,
          "User Idle diagnostic normalization must not mutate live Idle state");

    core.onEvent(DeviceLifecycleEvent::UserBusy, t0 + 2min);
    const auto live_active = core.snapshot();
    const auto busy_while_active = core.previewEvent(DeviceLifecycleEvent::UserBusy, t0 + 3min);
    check(contains_action(busy_while_active.plan, DeviceAction::SamsungPowerOn) &&
          contains_action(busy_while_active.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(busy_while_active.plan, DeviceAction::VizioWake),
          "User Busy diagnostic must exercise restore behavior even when live state is already Active");
    check(core.snapshot().idle_state == live_active.idle_state && core.snapshot().deadline == live_active.deadline,
          "User Busy diagnostic normalization must not mutate live Active state");

    core.onEvent(DeviceLifecycleEvent::UserIdle, t0 + 4min);
    core.onDeadline(t0 + 64min);
    const auto already_extended = core.previewExtendedIdle(t0 + 65min);
    check(contains_action(already_extended.plan, DeviceAction::VizioPowerOff) &&
          contains_action(already_extended.plan, DeviceAction::SamsungRestoreScreenOff) &&
          contains_action(already_extended.plan, DeviceAction::SamsungPowerOff),
          "Extended Idle diagnostic must directly preview the production full-power-off plan even when live state is already ExtendedIdle");
    check(core.snapshot().idle_state == IdleCoordinator::State::ExtendedIdle,
          "Extended Idle diagnostic must not alter an already-ExtendedIdle live coordinator");
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
        {"type", "external_tv_diagnostic"}, {"namespace", "external_tv"},
        {"request_id", "req-123"}, {"action", "device_operation"},
        {"device", "samsung"}, {"operation", "power_off"}
    };
    std::string error;
    const auto request = ExternalTvDiagnosticRequest::fromJson(wire, &error);
    check(request.has_value(), "valid external-TV diagnostic request must parse");
    check(request && request->request_id == "req-123", "diagnostic request ID must be preserved");
    check(request && request->action == ExternalTvDiagnosticAction::DeviceOperation,
          "device diagnostic request must preserve action selector");
    check(request && request->device == ExternalTvDiagnosticDevice::Samsung,
          "diagnostic request must preserve Samsung selector");
    check(request && request->operation == ExternalTvDiagnosticOperation::PowerOff,
          "diagnostic request must preserve operation");
    check(request && request->toJson() == wire, "diagnostic request must round-trip canonically");

    const nlohmann::json snapshot_wire = {
        {"type", "external_tv_diagnostic"}, {"namespace", "external_tv"},
        {"request_id", "req-snapshot"}, {"action", "snapshot"}
    };
    const auto snapshot_request = ExternalTvDiagnosticRequest::fromJson(snapshot_wire, &error);
    check(snapshot_request && snapshot_request->action == ExternalTvDiagnosticAction::Snapshot,
          "snapshot request must use the normal external-TV diagnostic envelope");
    check(snapshot_request && snapshot_request->toJson() == snapshot_wire,
          "snapshot request must not invent physical-device fields");
}

void test_external_tv_diagnostic_request_rejects_malformed_or_secret_payloads() {
    std::string error;
    check(!ExternalTvDiagnosticRequest::fromJson({{"type", "external_tv_diagnostic"}, {"namespace", "external_tv"}, {"action", "snapshot"}}, &error),
          "diagnostic request without request_id must fail closed");
    check(!ExternalTvDiagnosticRequest::fromJson({{"type", "external_tv_diagnostic"}, {"namespace", "external_tv"}, {"request_id", "r"}, {"action", "device_operation"}, {"device", "vizio"}, {"operation", "shutdown"}}, &error),
          "unsupported physical-device operation must fail closed");
    check(!ExternalTvDiagnosticRequest::fromJson({{"type", "external_tv_diagnostic"}, {"namespace", "external_tv"}, {"request_id", "r"}, {"action", "device_operation"}, {"device", "samsung"}, {"operation", "probe"}, {"Token", "secret"}}, &error),
          "diagnostic IPC must reject credential-bearing fields");
    check(!ExternalTvDiagnosticRequest::fromJson({{"type", "external_tv_diagnostic"}, {"namespace", "external_tv"}, {"request_id", "r"}, {"action", "snapshot"}, {"device", "samsung"}}, &error),
          "snapshot request must reject irrelevant device fields");
}

void test_external_tv_diagnostic_response_contract_and_semantics() {
    ExternalTvDiagnosticResponse response;
    response.request_id = "req-123";
    response.action = ExternalTvDiagnosticAction::DeviceOperation;
    response.device = ExternalTvDiagnosticDevice::Vizio;
    response.operation = ExternalTvDiagnosticOperation::PowerOn;
    response.accepted = true;
    response.executed = true;
    response.verified = true;
    response.resulting_state = "on";
    response.message = "Power-on verified";
    check(response.semanticsValid(), "verified diagnostic response must require accepted+executed and known resulting state");
    auto wire = response.toJson();
    check(wire.value("type", "") == "response", "diagnostic result must use the existing duplex response envelope");
    check(wire.value("request_id", "") == "req-123", "diagnostic response must echo request ID");
    check(!wire.contains("Token") && !wire.contains("Auth") && !wire.contains("credential"),
          "diagnostic response must contain no credential fields");
    std::string error;
    const auto parsed = ExternalTvDiagnosticResponse::fromJson(wire, &error);
    check(parsed.has_value() && parsed->matchesRequest("req-123"),
          "diagnostic response must support request-ID correlation");
    check(parsed && !parsed->matchesRequest("wrong-id"),
          "mismatched diagnostic response IDs must be rejected by correlation");

    wire["executed"] = false;
    check(!ExternalTvDiagnosticResponse::fromJson(wire, &error),
          "verified=true with executed=false must be rejected as impossible semantics");
}

class BlackholeTcpServer {
public:
    BlackholeTcpServer(unsigned short port, std::chrono::milliseconds hold)
        : acceptor_(ioc_, {boost::asio::ip::address_v4::loopback(), port}),
          hold_(hold), thread_([this] { run(); }) {}

    ~BlackholeTcpServer() {
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        boost::asio::ip::tcp::socket socket(ioc_);
        boost::system::error_code ec;
        acceptor_.accept(socket, ec);
        if (!ec) std::this_thread::sleep_for(hold_);
        socket.close(ec);
    }

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::chrono::milliseconds hold_;
    std::thread thread_;
};

void test_external_tv_transport_timeouts_are_bounded() {
    {
        BlackholeTcpServer server(8001, 6500ms);
        SamsungExternalTvSettings settings;
        settings.ip = "127.0.0.1";
        SamsungTizenTransport transport(settings);
        std::string error;
        const auto started = Clock::now();
        const auto state = transport.queryPowerState(error);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        check(state == SamsungPowerState::Unknown,
              "black-holed Samsung state probe must fail closed to Unknown");
        check(elapsed < 5500ms,
              "Samsung state probe timeout must be actively bounded, not depend on peer close");
    }
    {
        BlackholeTcpServer server(7345, 7500ms);
        VizioExternalTvSettings settings;
        settings.ip = "127.0.0.1";
        VizioSmartCastTransport transport(settings);
        std::string error;
        const auto started = Clock::now();
        const auto state = transport.queryPowerState(error);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        check(state == VizioPowerState::Unknown,
              "black-holed Vizio TLS probe must fail closed to Unknown");
        check(elapsed < 6500ms,
              "Vizio TLS/HTTP timeout must be actively bounded, not depend on peer close");
    }
}

std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input), {}) : std::string{};
}

bool contains_utf16le_ascii(const std::string& bytes, const std::string& text) {
    std::string needle;
    for (const char ch : text) {
        needle.push_back(ch);
        needle.push_back('\0');
    }
    return bytes.find(needle) != std::string::npos;
}

void test_daemon_recovery_watchdog_contract() {
    const auto root = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path();
    const auto recovery_xml = read_binary_file(root / "LGTV Companion Setup" / "LGTVCdaemonRecovery.xml");
    check(!recovery_xml.empty(), "daemon recovery watchdog task XML must be installed from source");
    if (!recovery_xml.empty()) {
        check(contains_utf16le_ascii(recovery_xml, "<TimeTrigger>"),
              "daemon recovery watchdog must use a registration-independent time trigger");
        check(contains_utf16le_ascii(recovery_xml, "<StartBoundary>2025-03-16T00:00:00</StartBoundary>"),
              "daemon recovery watchdog must use a stable past boundary so it becomes schedulable immediately");
        check(contains_utf16le_ascii(recovery_xml, "<Interval>PT1M</Interval>"), "daemon recovery watchdog must probe every minute");
        check(contains_utf16le_ascii(recovery_xml, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"), "daemon recovery watchdog must not overlap probes");
        check(contains_utf16le_ascii(recovery_xml, "-recovery_probe"), "daemon recovery watchdog must invoke the bounded recovery probe mode");
    }
    const auto installer = read_binary_file(root / "LGTV Companion Setup" / "Product.wxs");
    check(installer.find("LGTVC Daemon Recovery Task") != std::string::npos, "installer must create and manage the daemon recovery watchdog task");
}

void test_external_tv_snapshot_and_simulation_payload_round_trip() {
    ExternalTvDiagnosticResponse snapshot;
    snapshot.request_id = "req-snapshot";
    snapshot.action = ExternalTvDiagnosticAction::Snapshot;
    snapshot.accepted = snapshot.executed = snapshot.verified = true;
    snapshot.resulting_state = "idle";
    snapshot.message = "Read-only coordinator snapshot";
    snapshot.snapshot = ExternalTvDiagnosticSnapshot{
        true, true, "idle", true, 123, "pictureoff", "on", true, true};
    const auto snapshot_wire = snapshot.toJson();
    check(snapshot_wire.contains("snapshot") && !snapshot_wire.contains("device") &&
          !snapshot_wire.contains("operation") && snapshot_wire.dump().find("secret") == std::string::npos,
          "snapshot response must carry structured read-only data without physical-device selectors or credentials");
    std::string error;
    const auto parsed_snapshot = ExternalTvDiagnosticResponse::fromJson(snapshot_wire, &error);
    check(parsed_snapshot && parsed_snapshot->snapshot &&
          parsed_snapshot->snapshot->deadline_remaining_seconds == 123 &&
          parsed_snapshot->snapshot->samsung_token_present &&
          parsed_snapshot->snapshot->vizio_auth_present,
          "snapshot response must round-trip service/deadline state and credential-presence booleans");

    ExternalTvDiagnosticResponse simulation;
    simulation.request_id = "req-sim";
    simulation.action = ExternalTvDiagnosticAction::SimulateShutdown;
    simulation.accepted = simulation.executed = simulation.verified = true;
    simulation.resulting_state = "active";
    simulation.message = "Plan-only simulation; no live state mutated";
    simulation.simulation = ExternalTvLifecycleSimulation{
        "active", {"restore_screen_off", "power_off"}, {"power_off"},
        false, 0, true};
    const auto simulation_wire = simulation.toJson();
    check(simulation_wire.contains("simulation") &&
          simulation_wire["simulation"].value("would_mark_success_if_operations_succeed", false),
          "simulation response must state the hypothetical success-mark outcome");
    const auto parsed_simulation = ExternalTvDiagnosticResponse::fromJson(simulation_wire, &error);
    check(parsed_simulation && parsed_simulation->simulation &&
          parsed_simulation->simulation->samsung_actions.size() == 2 &&
          parsed_simulation->simulation->vizio_actions.size() == 1,
          "simulation response must round-trip deterministic per-device action plans");
}

}  // namespace

int main() {
    test_ipc_client_async_send_runs_on_owned_io_thread();
    test_ipc_client_sync_send_does_not_poison_owned_iocp();
    test_ipc_client_pending_sync_send_does_not_enter_owned_iocp();
    test_taskbar_recovery_coalesces_and_fires_once();
    test_daemon_recovery_watchdog_contract();
    test_first_idle_arms_once();
    test_busy_cancels_deadline();
    test_extended_idle_fires_once();
    test_samsung_pictureoff_is_powered_and_blanked();
    test_samsung_idle_when_already_pictureoff_is_noop();
    test_samsung_enable_leaves_accessibility_open();
    test_samsung_restore_open_awake_menu();
    test_samsung_restore_pictureoff_invalidates_menu_and_reopens();
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
    test_device_coordinator_read_only_lifecycle_previews();
    test_external_tv_settings_round_trip_and_redaction();
    test_external_tv_settings_safe_defaults();
    test_external_tv_diagnostic_request_contract();
    test_external_tv_diagnostic_request_rejects_malformed_or_secret_payloads();
    test_external_tv_diagnostic_response_contract_and_semantics();
    test_external_tv_snapshot_and_simulation_payload_round_trip();
    test_external_tv_transport_timeouts_are_bounded();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "native_tv_orchestration_tests: PASS\n";
    return EXIT_SUCCESS;
}
