#pragma once

#include <string>

class WindowsDisplayPowerPolicy {
public:
    // Ensures Windows' monitor idle timeout cannot remove HDMI display endpoints.
    // Returns false and populates error on any power-policy API failure.
    static bool enforceTopologySafeMonitorTimeout(std::string& error);
};
