#pragma once

#include "../Common/external_tv_settings.h"
#include "samsung_controller.h"
#include "vizio_controller.h"

#include <string>

class SamsungTizenTransport {
public:
    explicit SamsungTizenTransport(SamsungExternalTvSettings settings);

    SamsungPowerState queryPowerState(std::string& error) const;
    bool sendKey(SamsungRemoteKey key, std::string& error) const;
    bool wake(std::string& error) const;

private:
    bool sendKeyTls(SamsungRemoteKey key, std::string& error) const;
    bool sendKeyPlain(SamsungRemoteKey key, std::string& error) const;
    SamsungExternalTvSettings settings_;
};

class VizioSmartCastTransport {
public:
    explicit VizioSmartCastTransport(VizioExternalTvSettings settings);

    VizioPowerState queryPowerState(std::string& error) const;
    bool blankPanel(std::string& error) const;
    bool unblankPanel(std::string& error) const;
    bool powerOff(std::string& error) const;
    bool powerOn(std::string& error) const;
    bool wake(std::string& error) const;

private:
    bool keyPress(int codeset, int code, std::string& error) const;
    bool request(const std::string& method, const std::string& target,
                 const std::string& body, std::string& response,
                 std::string& error) const;
    VizioExternalTvSettings settings_;
};
