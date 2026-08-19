#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class VizioPowerState {
    Unknown,
    Off,
    On,
};

enum class VizioAction {
    None,
    BlankPanel,
    UnblankPanel,
    PowerOn,
    PowerOff,
};

class VizioController {
public:
    static VizioPowerState fromSmartCastPowerValue(int value) noexcept;
    static bool shouldSendPowerOff(VizioPowerState state) noexcept;
    static bool shouldRetryPowerOffAfterVerification(VizioPowerState initial_state,
                                                     VizioPowerState verified_state) noexcept;
    static VizioAction planWakeForObservedPowerState(VizioPowerState state) noexcept;
    static std::string smartCastKeyPayload(int codeset, int code);
    static std::optional<std::uint64_t> smartCastHashValFromJson(const std::string& response) noexcept;

    void observeState(VizioPowerState power, bool blanked) noexcept;

    VizioAction planIdleBlank() const noexcept;
    VizioAction planBusyOrResume() const noexcept;
    VizioAction planExtendedIdle() const noexcept;
    VizioAction planSuspendOrShutdown() const noexcept;

    VizioPowerState powerState() const noexcept { return power_state_; }
    bool blanked() const noexcept { return blanked_; }

private:
    VizioPowerState power_state_{VizioPowerState::Unknown};
    bool blanked_{false};
};
