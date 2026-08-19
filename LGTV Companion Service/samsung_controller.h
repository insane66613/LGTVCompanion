#pragma once

#include <chrono>
#include <vector>

enum class SamsungPowerState {
    Unknown,
    On,
    PictureOff,
    Standby,
    ReachableNoState,
};

enum class SamsungAccessibilityState {
    Unknown,
    OpenEnabled,
    ClosedDisabled,
};

enum class SamsungRemoteKey {
    Accessibility,
    Enter,
    Return,
    Power,
};

struct SamsungCommandStep {
    SamsungRemoteKey key;
    std::chrono::milliseconds delay_after{0};
};

class SamsungController {
public:
    void observePowerState(SamsungPowerState state) noexcept;
    void assumeAccessibilityState(SamsungAccessibilityState state) noexcept;

    SamsungPowerState powerState() const noexcept { return power_state_; }
    SamsungAccessibilityState accessibilityState() const noexcept { return accessibility_state_; }

    static bool isPowered(SamsungPowerState state) noexcept;
    static bool isBlanked(SamsungPowerState state) noexcept;

    std::vector<SamsungCommandStep> planEnableScreenOff();
    std::vector<SamsungCommandStep> planDisableScreenOff();

    bool shouldSendPowerToggleForOff() const noexcept;
    bool shouldSendPowerToggleForOnFallback() const noexcept;

private:
    SamsungPowerState power_state_{SamsungPowerState::Unknown};
    SamsungAccessibilityState accessibility_state_{SamsungAccessibilityState::Unknown};
};
