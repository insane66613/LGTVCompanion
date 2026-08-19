#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <powrprof.h>
#pragma comment(lib, "PowrProf.lib")

#include "windows_display_power_policy.h"

#include <sstream>

namespace {
std::string errorText(const char* operation, DWORD code) {
    std::ostringstream out;
    out << operation << " failed with Win32 error " << code;
    return out.str();
}
}

bool WindowsDisplayPowerPolicy::enforceTopologySafeMonitorTimeout(std::string& error) {
    GUID* active = nullptr;
    DWORD rc = PowerGetActiveScheme(nullptr, &active);
    if (rc != ERROR_SUCCESS || active == nullptr) {
        error = errorText("PowerGetActiveScheme", rc);
        return false;
    }

    const auto release = [&active]() {
        if (active) LocalFree(active);
        active = nullptr;
    };

    rc = PowerWriteACValueIndex(nullptr, active, &GUID_VIDEO_SUBGROUP,
                                &GUID_VIDEO_POWERDOWN_TIMEOUT, 0);
    if (rc != ERROR_SUCCESS) {
        error = errorText("PowerWriteACValueIndex", rc);
        release();
        return false;
    }
    rc = PowerWriteDCValueIndex(nullptr, active, &GUID_VIDEO_SUBGROUP,
                                &GUID_VIDEO_POWERDOWN_TIMEOUT, 0);
    if (rc != ERROR_SUCCESS) {
        error = errorText("PowerWriteDCValueIndex", rc);
        release();
        return false;
    }
    rc = PowerSetActiveScheme(nullptr, active);
    release();
    if (rc != ERROR_SUCCESS) {
        error = errorText("PowerSetActiveScheme", rc);
        return false;
    }
    error.clear();
    return true;
}
