#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "device_coordinator.h"

#include <chrono>
#include <utility>

using namespace std::chrono_literals;

namespace {
ExternalTvDiagnosticResponse baseResponse(const ExternalTvDiagnosticRequest& request) {
    ExternalTvDiagnosticResponse response;
    response.request_id = request.request_id;
    response.device = request.device;
    response.operation = request.operation;
    return response;
}

const char* samsungStateName(SamsungPowerState state) {
    switch (state) {
    case SamsungPowerState::On: return "on";
    case SamsungPowerState::PictureOff: return "pictureoff";
    case SamsungPowerState::Standby: return "standby";
    case SamsungPowerState::ReachableNoState: return "reachable_no_state";
    default: return "unknown";
    }
}

const char* vizioStateName(VizioPowerState state) {
    switch (state) {
    case VizioPowerState::On: return "on";
    case VizioPowerState::Off: return "off";
    default: return "unknown";
    }
}

std::string redactSecret(std::string message, const std::string& secret) {
    if (secret.empty()) return message;
    std::size_t pos = 0;
    while ((pos = message.find(secret, pos)) != std::string::npos) {
        message.replace(pos, secret.size(), "<redacted>");
        pos += 10;
    }
    return message;
}
}  // namespace

void DeviceCoordinator::runDiagnostic(ExternalTvDiagnosticRequest request, DiagnosticCallback callback) {
    if (!callback) return;
    ExternalTvDiagnosticResponse rejected = baseResponse(request);
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        if (!settings_.enabled || stopped_) {
            rejected.message = stopped_ ? "External-TV coordinator is stopping" : "External-TV orchestration is disabled";
        } else if (request.device == ExternalTvDiagnosticDevice::Samsung && !samsung_transport_) {
            rejected.message = "Samsung external-TV control is disabled";
        } else if (request.device == ExternalTvDiagnosticDevice::Vizio && !vizio_transport_) {
            rejected.message = "Vizio external-TV control is disabled";
        } else {
            rejected.accepted = true;
            if (request.device == ExternalTvDiagnosticDevice::Samsung) {
                boost::asio::post(samsung_strand_, [this, request = std::move(request), callback = std::move(callback)]() mutable {
                    callback(runSamsungDiagnostic(request));
                });
            } else {
                boost::asio::post(vizio_strand_, [this, request = std::move(request), callback = std::move(callback)]() mutable {
                    callback(runVizioDiagnostic(request));
                });
            }
            return;
        }
    }
    callback(std::move(rejected));
}

ExternalTvDiagnosticResponse DeviceCoordinator::runSamsungDiagnostic(
    const ExternalTvDiagnosticRequest& request) {
    auto response = baseResponse(request);
    response.accepted = true;
    std::string error;
    auto probe = [&]() {
        auto state = samsung_transport_->queryPowerState(error);
        if (state != SamsungPowerState::Unknown)
            samsung_controller_.observePowerState(state);
        response.resulting_state = samsungStateName(state);
        return state;
    };
    auto verify = [&](auto predicate, int attempts, std::chrono::milliseconds delay) {
        SamsungPowerState state = SamsungPowerState::Unknown;
        for (int i = 0; i < attempts; ++i) {
            if (i > 0) std::this_thread::sleep_for(delay);
            state = probe();
            if (predicate(state)) return true;
        }
        return false;
    };

    auto state = probe();
    if (request.operation == ExternalTvDiagnosticOperation::Probe) {
        response.executed = state != SamsungPowerState::Unknown;
        response.verified = response.executed;
        response.message = response.verified ? "Samsung state verified" : redactSecret(error, settings_.samsung.token);
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::Blank) {
        if (state == SamsungPowerState::PictureOff) {
            response.executed = response.verified = true;
            response.message = "Samsung is already picture-off";
            return response;
        }
        if (state != SamsungPowerState::On) {
            response.message = "Samsung must be ON before screen-off diagnostics";
            return response;
        }
        const auto steps = samsung_controller_.planEnableScreenOff();
        response.executed = executeSamsungSteps(steps);
        if (response.executed)
            response.verified = verify([](SamsungPowerState s) { return s == SamsungPowerState::PictureOff; }, 8, 500ms);
        response.message = response.verified ? "Samsung picture-off verified" :
            (response.executed ? "Samsung screen-off command executed but picture-off was not verified" : "Samsung screen-off command failed");
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::Unblank) {
        if (state == SamsungPowerState::On) {
            response.executed = response.verified = true;
            response.message = "Samsung is already unblanked";
            return response;
        }
        if (state != SamsungPowerState::PictureOff) {
            response.message = "Samsung must be picture-off before unblank diagnostics";
            return response;
        }
        const auto steps = samsung_controller_.planDisableScreenOff();
        response.executed = executeSamsungSteps(steps);
        if (response.executed)
            response.verified = verify([](SamsungPowerState s) { return s == SamsungPowerState::On; }, 8, 500ms);
        response.message = response.verified ? "Samsung unblank verified" :
            (response.executed ? "Samsung unblank executed but ON state was not verified" : "Samsung unblank command failed");
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::PowerOn) {
        if (SamsungController::isPowered(state)) {
            response.executed = response.verified = true;
            response.message = "Samsung is already powered";
            return response;
        }
        std::string wake_error;
        response.executed = samsung_transport_->wake(wake_error);
        std::this_thread::sleep_for(2500ms);
        state = probe();
        if (!SamsungController::isPowered(state) && state == SamsungPowerState::Standby) {
            if (samsung_controller_.shouldSendPowerToggleForOnFallback()) {
                std::string key_error;
                response.executed = samsung_transport_->sendKey(SamsungRemoteKey::Power, key_error) || response.executed;
                if (!key_error.empty()) error = key_error;
            }
        }
        response.verified = verify([](SamsungPowerState s) { return SamsungController::isPowered(s); }, 10, 750ms);
        response.message = response.verified ? "Samsung powered state verified" :
            redactSecret(error.empty() ? wake_error : error, settings_.samsung.token);
        if (response.message.empty() && !response.verified)
            response.message = "Samsung power-on executed but powered state was not verified";
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::PowerOff) {
        if (state == SamsungPowerState::Standby) {
            response.executed = response.verified = true;
            response.message = "Samsung is already in standby";
            return response;
        }
        if (state == SamsungPowerState::PictureOff) {
            const auto restore = samsung_controller_.planDisableScreenOff();
            if (!executeSamsungSteps(restore)) {
                response.message = "Samsung screen-off mode could not be restored before power-off";
                return response;
            }
            if (!verify([](SamsungPowerState s) { return s == SamsungPowerState::On; }, 8, 500ms)) {
                response.executed = true;
                response.message = "Samsung unblank executed but ON state was not verified; power toggle suppressed";
                return response;
            }
            state = samsung_controller_.powerState();
        }
        if (state != SamsungPowerState::On || !samsung_controller_.shouldSendPowerToggleForOff()) {
            response.message = "Samsung power state is not safe for KEY_POWER; power-off suppressed";
            return response;
        }
        std::string key_error;
        response.executed = samsung_transport_->sendKey(SamsungRemoteKey::Power, key_error);
        if (response.executed)
            response.verified = verify([](SamsungPowerState s) { return s == SamsungPowerState::Standby; }, 12, 750ms);
        response.message = response.verified ? "Samsung standby verified" :
            redactSecret(key_error.empty() ? error : key_error, settings_.samsung.token);
        if (response.message.empty() && !response.verified)
            response.message = "Samsung power-off executed but standby was not verified";
        return response;
    }

    response.message = "Unsupported Samsung diagnostic operation";
    return response;
}

ExternalTvDiagnosticResponse DeviceCoordinator::runVizioDiagnostic(
    const ExternalTvDiagnosticRequest& request) {
    auto response = baseResponse(request);
    response.accepted = true;
    std::string error;
    auto probe = [&]() {
        auto state = vizio_transport_->queryPowerState(error);
        response.resulting_state = vizioStateName(state);
        return state;
    };
    auto state = probe();
    if (request.operation == ExternalTvDiagnosticOperation::Probe) {
        response.executed = state != VizioPowerState::Unknown;
        response.verified = response.executed;
        response.message = response.verified ? "Vizio power state verified" : redactSecret(error, settings_.vizio.auth);
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::Blank) {
        if (state != VizioPowerState::On) {
            response.message = "Vizio must be ON before panel-blank diagnostics";
            return response;
        }
        response.executed = vizio_transport_->blankPanel(error);
        state = probe();
        response.verified = false;
        response.message = response.executed
            ? "Vizio blank command executed; SmartCast does not expose independent panel-blank verification"
            : redactSecret(error, settings_.vizio.auth);
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::Unblank) {
        if (state != VizioPowerState::On) {
            response.message = "Vizio must be ON before panel-unblank diagnostics";
            return response;
        }
        response.executed = vizio_transport_->unblankPanel(error);
        state = probe();
        response.verified = false;
        response.message = response.executed
            ? "Vizio unblank command executed; SmartCast does not expose independent panel-blank verification"
            : redactSecret(error, settings_.vizio.auth);
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::PowerOn) {
        if (state == VizioPowerState::On) {
            response.executed = response.verified = true;
            response.message = "Vizio is already ON";
            return response;
        }
        response.executed = vizio_transport_->powerOn(error);
        if (response.executed) {
            response.resulting_state = "on";
            response.verified = true;
            response.message = "Vizio ON state verified";
        } else {
            probe();
            response.message = redactSecret(error, settings_.vizio.auth);
        }
        return response;
    }

    if (request.operation == ExternalTvDiagnosticOperation::PowerOff) {
        if (state == VizioPowerState::Off) {
            response.executed = response.verified = true;
            response.message = "Vizio is already OFF";
            return response;
        }
        response.executed = vizio_transport_->powerOff(error);
        if (response.executed) {
            response.resulting_state = "off";
            response.verified = true;
            response.message = "Vizio OFF state verified";
        } else {
            probe();
            response.message = redactSecret(error, settings_.vizio.auth);
        }
        return response;
    }

    response.message = "Unsupported Vizio diagnostic operation";
    return response;
}
