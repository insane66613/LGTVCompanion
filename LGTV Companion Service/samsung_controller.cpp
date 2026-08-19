#include "samsung_controller.h"

using namespace std::chrono_literals;

void SamsungController::observePowerState(SamsungPowerState state) noexcept {
    // PowerState=pictureoff proves the TV is powered with its panel blanked, but
    // it does not prove that this process still knows the Accessibility menu is
    // open. Preserve menu knowledge separately so process-restart recovery can
    // safely reopen Accessibility before toggling the first item.
    power_state_ = state;
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
    // pictureoff is direct evidence that persistent Screen Off Mode is already
    // active. Never toggle it again merely because this process does not know
    // whether the Accessibility menu itself is still open after a restart.
    if (!isPowered(power_state_) || power_state_ == SamsungPowerState::PictureOff ||
        accessibility_state_ == SamsungAccessibilityState::OpenEnabled) {
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
    // On a fresh process start, an ordinary awake TV does not establish that
    // persistent Screen Off Mode is enabled. Do not blindly reopen Accessibility
    // and risk enabling it. PictureOff is the restart-safe evidence that a
    // selection-preserving reconciliation is required.
    if (power_state_ == SamsungPowerState::On &&
        accessibility_state_ == SamsungAccessibilityState::Unknown) {
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
