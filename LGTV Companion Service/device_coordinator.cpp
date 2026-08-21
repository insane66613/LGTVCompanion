#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "device_coordinator.h"

#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

DeviceCoordinator::DeviceCoordinator(ExternalTvSettings settings, std::shared_ptr<Logging> log)
    : settings_(std::move(settings)),
      log_(std::move(log)),
      core_(std::chrono::minutes(settings_.extended_idle_minutes),
            settings_.preserve_desktop_topology_on_idle),
      control_work_(boost::asio::make_work_guard(control_ioc_)),
      deadline_timer_(control_ioc_) {
    if (settings_.samsung.enabled)
        samsung_transport_ = std::make_unique<SamsungTizenTransport>(settings_.samsung);
    if (settings_.vizio.enabled)
        vizio_transport_ = std::make_unique<VizioSmartCastTransport>(settings_.vizio);
    control_thread_ = std::thread([this]() { control_ioc_.run(); });
}

DeviceCoordinator::~DeviceCoordinator() {
    shutdown();
}

void DeviceCoordinator::handleEvent(DeviceLifecycleEvent event) {
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (!settings_.enabled || stopped_) return;
    // Once admitted under the same lock used by shutdown(), an event must drain
    // even if teardown starts before the control executor reaches it.
    boost::asio::post(control_ioc_, [this, event]() {
        if (log_) log_->debug("ExternalTV", "Lifecycle event admitted: " + std::to_string(static_cast<int>(event)));
        applyPlan(core_.onEvent(event, IdleCoordinator::Clock::now()));
    });
}

void DeviceCoordinator::scheduleDeadline(const DevicePlan& plan) {
    if (!plan.deadline_changed) return;
    const auto cancelled = deadline_timer_.cancel();
    if (!plan.deadline) {
        if (log_) log_->debug("ExternalTV", "Extended-idle deadline cancelled; pending waits: " + std::to_string(cancelled));
        return;
    }
    const auto now = IdleCoordinator::Clock::now();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(*plan.deadline - now).count();
    if (log_) log_->debug("ExternalTV", "Extended-idle deadline armed in " + std::to_string(seconds) + " second(s)");
    deadline_timer_.expires_at(*plan.deadline);
    deadline_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            if (log_) log_->debug("ExternalTV", "Extended-idle timer wait ended: " + ec.message());
            return;
        }
        if (stopped_) {
            if (log_) log_->debug("ExternalTV", "Extended-idle timer fired after coordinator stop; ignored");
            return;
        }
        if (log_) log_->debug("ExternalTV", "Extended-idle timer fired");
        applyPlan(core_.onDeadline(IdleCoordinator::Clock::now()));
    });
}

void DeviceCoordinator::applyPlan(DevicePlan plan) {
    scheduleDeadline(plan);
    if (log_) log_->debug("ExternalTV", "Applying device plan; action count: " + std::to_string(plan.actions.size()));
    std::vector<DeviceAction> samsung;
    std::vector<DeviceAction> vizio;
    for (const auto action : plan.actions) {
        switch (action) {
        case DeviceAction::SamsungEnableScreenOff:
        case DeviceAction::SamsungRestoreScreenOff:
        case DeviceAction::SamsungPowerOn:
        case DeviceAction::SamsungPowerOff:
            if (samsung_transport_) samsung.push_back(action);
            break;
        case DeviceAction::VizioBlankPanel:
        case DeviceAction::VizioWake:
        case DeviceAction::VizioPowerOff:
            if (vizio_transport_) vizio.push_back(action);
            break;
        }
    }
    if (!samsung.empty()) {
        boost::asio::post(samsung_strand_, [this, actions = std::move(samsung)]() mutable {
            executeSamsungActions(std::move(actions));
        });
    }
    if (!vizio.empty()) {
        boost::asio::post(vizio_strand_, [this, actions = std::move(vizio)]() mutable {
            executeVizioActions(std::move(actions));
        });
    }
}

bool DeviceCoordinator::executeSamsungSteps(const std::vector<SamsungCommandStep>& steps) {
    for (const auto& step : steps) {
        std::string error;
        if (!samsung_transport_->sendKey(step.key, error)) {
            logFailure("Samsung", "remote key", error);
            return false;
        }
        if (step.delay_after.count() > 0)
            std::this_thread::sleep_for(step.delay_after);
    }
    return true;
}

void DeviceCoordinator::executeSamsungActions(std::vector<DeviceAction> actions) {
    for (const auto action : actions) {
        if (log_) log_->debug("ExternalTV", "Samsung action start: " + std::to_string(static_cast<int>(action)));
        // Actions admitted before shutdown must finish; stopped_ prevents new
        // plans from entering but does not abandon an in-flight power sequence.
        std::string error;
        auto state = samsung_transport_->queryPowerState(error);
        samsung_observed_state_.store(state);
        if (state != SamsungPowerState::Unknown)
            samsung_controller_.observePowerState(state);

        switch (action) {
        case DeviceAction::SamsungEnableScreenOff: {
            if (state == SamsungPowerState::Unknown) {
                logFailure("Samsung", "state probe before screen off", error);
                break;
            }
            const auto plan = samsung_controller_.planEnableScreenOff();
            if (!executeSamsungSteps(plan))
                samsung_controller_.assumeAccessibilityState(SamsungAccessibilityState::Unknown);
            break;
        }
        case DeviceAction::SamsungRestoreScreenOff: {
            if (state == SamsungPowerState::Unknown) {
                logFailure("Samsung", "state probe before restore", error);
                break;
            }
            const auto plan = samsung_controller_.planDisableScreenOff();
            if (!executeSamsungSteps(plan))
                samsung_controller_.assumeAccessibilityState(SamsungAccessibilityState::Unknown);
            break;
        }
        case DeviceAction::SamsungPowerOn: {
            if (state == SamsungPowerState::On || state == SamsungPowerState::PictureOff)
                break;
            std::string wake_error;
            if (!samsung_transport_->wake(wake_error))
                logFailure("Samsung", "WOL", wake_error);
            std::this_thread::sleep_for(2500ms);
            state = samsung_transport_->queryPowerState(error);
            samsung_observed_state_.store(state);
            if (state != SamsungPowerState::Unknown)
                samsung_controller_.observePowerState(state);
            if (samsung_controller_.shouldSendPowerToggleForOnFallback()) {
                if (!samsung_transport_->sendKey(SamsungRemoteKey::Power, error))
                    logFailure("Samsung", "guarded power-on fallback", error);
            }
            break;
        }
        case DeviceAction::SamsungPowerOff: {
            if (state == SamsungPowerState::Unknown) {
                logFailure("Samsung", "state probe before power off; KEY_POWER suppressed", error);
                break;
            }
            if (samsung_controller_.shouldSendPowerToggleForOff()) {
                if (!samsung_transport_->sendKey(SamsungRemoteKey::Power, error))
                    logFailure("Samsung", "guarded power off", error);
            }
            break;
        }
        default:
            break;
        }
    }
}

void DeviceCoordinator::executeVizioActions(std::vector<DeviceAction> actions) {
    for (const auto action : actions) {
        if (log_) log_->debug("ExternalTV", "Vizio action start: " + std::to_string(static_cast<int>(action)));
        // Actions admitted before shutdown must finish; stopped_ prevents new
        // plans from entering but does not abandon an in-flight power sequence.
        std::string error;
        bool ok = true;
        switch (action) {
        case DeviceAction::VizioBlankPanel:
            ok = vizio_transport_->blankPanel(error);
            break;
        case DeviceAction::VizioWake: {
            std::string state_error;
            auto state = vizio_transport_->queryPowerState(state_error);
            vizio_observed_state_.store(state);
            const auto wake_plan = VizioController::planWakeForObservedPowerState(state);

            if (wake_plan == VizioAction::PowerOn) {
                ok = vizio_transport_->powerOn(error);
                if (ok) vizio_observed_state_.store(VizioPowerState::On);
                break;
            }

            // A powered-but-blank TV should be restored with BACK, but an OFF
            // TV may ACK BACK without waking. For unknown state, every accepted
            // BACK is therefore followed by a real power-state probe.
            ok = false;
            for (int attempt = 0; attempt <= 10; ++attempt) {
                if (attempt > 0) std::this_thread::sleep_for(3000ms);
                std::string unblank_error;
                if (!vizio_transport_->unblankPanel(unblank_error)) {
                    error = unblank_error;
                    continue;
                }
                if (wake_plan == VizioAction::UnblankPanel) {
                    ok = true;
                    break;
                }

                std::string after_error;
                state = vizio_transport_->queryPowerState(after_error);
                vizio_observed_state_.store(state);
                if (state == VizioPowerState::On) {
                    ok = true;
                    break;
                }
                if (state == VizioPowerState::Off) {
                    ok = vizio_transport_->powerOn(error);
                    if (ok) vizio_observed_state_.store(VizioPowerState::On);
                    break;
                }
                error = "BACK accepted but power state remains unknown: " + after_error;
            }
            if (!ok && state_error.size() > 0 && error.size() == 0)
                error = "initial power-state check failed: " + state_error;
            break;
        }
        case DeviceAction::VizioPowerOff:
            ok = vizio_transport_->powerOff(error);
            if (ok) vizio_observed_state_.store(VizioPowerState::Off);
            break;
        default:
            break;
        }
        if (!ok) {
            logFailure("Vizio", "device action", error);
        } else if (log_) {
            log_->debug("ExternalTV", "Vizio action accepted: " + std::to_string(static_cast<int>(action)));
        }
    }
}

void DeviceCoordinator::logFailure(const char* device, const char* action, const std::string& error) {
    if (log_) log_->warning("ExternalTV", std::string(device) + " " + action + " failed: " + error);
}

void DeviceCoordinator::shutdown() {
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) return;
    }

    // Service teardown is not itself a Windows power event. Close admission,
    // cancel future idle escalation, and let previously admitted events drain.
    // This preserves an already-admitted Windows Shutdown/Suspend transition
    // without powering TVs off for an ordinary service stop or update restart.
    boost::asio::post(control_ioc_, [this]() {
        deadline_timer_.cancel();
        control_work_.reset();
    });

    // Do not stop the io_context: queued lifecycle work must finish before the
    // per-device worker pool is joined.
    if (control_thread_.joinable()) control_thread_.join();
    network_pool_.join();
}
