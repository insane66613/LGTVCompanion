#include "vizio_controller.h"

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
