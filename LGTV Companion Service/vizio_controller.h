#pragma once

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
