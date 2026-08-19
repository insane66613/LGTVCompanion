#include "samsung_controller.h"

using namespace std::chrono_literals;

void SamsungController::observePowerState(SamsungPowerState state) noexcept {
    power_state_ = state;
    if (state == SamsungPowerState::PictureOff) {
        accessibility_state_ = SamsungAccessibilityState::OpenEnabled;
    }
}

void SamsungController::assumeAccessibilityState(SamsungAccessibilityState state) noexcept {
    accessibility_state_ = state;
}

bool SamsungController::isPowered(SamsungPowerState state) noexcept {
    return state == SamsungPowerState::On || state == SamsungPowerState::PictureOff;
}

bool SamsungController::isBlanked(SamsungPowerState state) noexcept {
    return state == SamsungPowerState::PictureOff;
}

std::vector<SamsungCommandStep> SamsungController::planEnableScreenOff() {
    if (!isPowered(power_state_) || accessibility_state_ == SamsungAccessibilityState::OpenEnabled) {
        return {};
    }
    accessibility_state_ = SamsungAccessibilityState::OpenEnabled;
    return {
        {SamsungRemoteKey::Accessibility, 1400ms},
        {SamsungRemoteKey::Enter, 0ms},
    };
}

std::vector<SamsungCommandStep> SamsungController::planDisableScreenOff() {
    if (!isPowered(power_state_) || accessibility_state_ == SamsungAccessibilityState::ClosedDisabled) {
        return {};
    }

    std::vector<SamsungCommandStep> result;
    const bool picture_off = power_state_ == SamsungPowerState::PictureOff;
    if (picture_off) {
        result.push_back({SamsungRemoteKey::Enter, 1200ms});
        power_state_ = SamsungPowerState::On;
    }

    if (accessibility_state_ != SamsungAccessibilityState::OpenEnabled) {
        result.push_back({SamsungRemoteKey::Accessibility, 1400ms});
    }

    result.push_back({SamsungRemoteKey::Enter, 350ms});
    result.push_back({SamsungRemoteKey::Return, 0ms});
    accessibility_state_ = SamsungAccessibilityState::ClosedDisabled;
    return result;
}

bool SamsungController::shouldSendPowerToggleForOff() const noexcept {
    return power_state_ == SamsungPowerState::On || power_state_ == SamsungPowerState::PictureOff;
}

bool SamsungController::shouldSendPowerToggleForOnFallback() const noexcept {
    return power_state_ == SamsungPowerState::Standby || power_state_ == SamsungPowerState::ReachableNoState;
}
