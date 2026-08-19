#include "vizio_controller.h"

#include <nlohmann/json.hpp>

namespace {
bool findUnsignedIntegerKey(const nlohmann::json& node, const std::string& key, std::uint64_t& value) {
    if (node.is_object()) {
        const auto it = node.find(key);
        if (it != node.end()) {
            if (it->is_number_unsigned()) {
                value = it->get<std::uint64_t>();
                return true;
            }
            if (it->is_number_integer()) {
                const auto signed_value = it->get<std::int64_t>();
                if (signed_value >= 0) {
                    value = static_cast<std::uint64_t>(signed_value);
                    return true;
                }
            }
        }
        for (const auto& item : node.items()) {
            if (findUnsignedIntegerKey(item.value(), key, value)) return true;
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            if (findUnsignedIntegerKey(item, key, value)) return true;
        }
    }
    return false;
}
}

VizioPowerState VizioController::fromSmartCastPowerValue(int value) noexcept {
    return value == 0 ? VizioPowerState::Off : VizioPowerState::On;
}

bool VizioController::shouldSendPowerOff(VizioPowerState state) noexcept {
    return state == VizioPowerState::On;
}

bool VizioController::shouldRetryPowerOffAfterVerification(VizioPowerState initial_state,
                                                           VizioPowerState verified_state) noexcept {
    return initial_state == VizioPowerState::On && verified_state == VizioPowerState::On;
}

VizioAction VizioController::planWakeForObservedPowerState(VizioPowerState state) noexcept {
    if (state == VizioPowerState::Off) return VizioAction::PowerOn;
    if (state == VizioPowerState::On) return VizioAction::UnblankPanel;
    return VizioAction::None;
}

std::string VizioController::smartCastKeyPayload(int codeset, int code) {
    nlohmann::ordered_json payload = {
        {"_url", "/key_command/"},
        {"KEYLIST", nlohmann::ordered_json::array({nlohmann::ordered_json{
            {"CODESET", codeset},
            {"CODE", code},
            {"ACTION", "KEYPRESS"},
        }})},
    };
    return payload.dump();
}

std::optional<std::uint64_t> VizioController::smartCastHashValFromJson(const std::string& response) noexcept {
    try {
        const auto json = nlohmann::json::parse(response);
        std::uint64_t value = 0;
        if (findUnsignedIntegerKey(json, "HASHVAL", value) ||
            findUnsignedIntegerKey(json, "hashval", value)) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

void VizioController::observeState(VizioPowerState power, bool blanked) noexcept {
    power_state_ = power;
    blanked_ = power == VizioPowerState::On && blanked;
}

VizioAction VizioController::planIdleBlank() const noexcept {
    if (power_state_ == VizioPowerState::On && !blanked_) {
        return VizioAction::BlankPanel;
    }
    return VizioAction::None;
}

VizioAction VizioController::planBusyOrResume() const noexcept {
    if (power_state_ == VizioPowerState::On && blanked_) {
        return VizioAction::UnblankPanel;
    }
    if (power_state_ == VizioPowerState::Off) {
        return VizioAction::PowerOn;
    }
    return VizioAction::None;
}

VizioAction VizioController::planExtendedIdle() const noexcept {
    return power_state_ == VizioPowerState::On ? VizioAction::PowerOff : VizioAction::None;
}

VizioAction VizioController::planSuspendOrShutdown() const noexcept {
    return planExtendedIdle();
}
