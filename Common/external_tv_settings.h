#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct SamsungExternalTvSettings {
    bool enabled{false};
    std::string ip;
    std::string mac;
    std::string token;
};

struct VizioExternalTvSettings {
    bool enabled{false};
    std::string ip;
    std::string mac;
    std::string auth;
};

struct ExternalTvSettings {
    bool enabled{false};
    bool preserve_desktop_topology_on_idle{true};
    int extended_idle_minutes{60};
    SamsungExternalTvSettings samsung;
    VizioExternalTvSettings vizio;

    static ExternalTvSettings fromJson(const nlohmann::json& node);
    nlohmann::json toJson() const;
    nlohmann::json toRedactedJson() const;
};
