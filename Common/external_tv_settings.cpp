#include "external_tv_settings.h"

namespace {
template <typename T>
T value_or(const nlohmann::json& node, const char* key, T fallback) {
    const auto it = node.find(key);
    if (it == node.end() || it->is_null()) return fallback;
    try { return it->get<T>(); } catch (...) { return fallback; }
}
}

ExternalTvSettings ExternalTvSettings::fromJson(const nlohmann::json& node) {
    ExternalTvSettings settings;
    if (!node.is_object()) return settings;
    settings.enabled = value_or<bool>(node, "Enabled", false);
    settings.preserve_desktop_topology_on_idle = value_or<bool>(node, "PreserveDesktopTopologyOnIdle", true);
    settings.extended_idle_minutes = value_or<int>(node, "ExtendedIdleMinutes", 60);
    if (settings.extended_idle_minutes < 1) settings.extended_idle_minutes = 1;
    if (settings.extended_idle_minutes > 1440) settings.extended_idle_minutes = 1440;
    const auto s = node.value("Samsung", nlohmann::json::object());
    settings.samsung.enabled = value_or<bool>(s, "Enabled", false);
    settings.samsung.ip = value_or<std::string>(s, "IP", "");
    settings.samsung.mac = value_or<std::string>(s, "MAC", "");
    settings.samsung.token = value_or<std::string>(s, "Token", "");
    const auto v = node.value("Vizio", nlohmann::json::object());
    settings.vizio.enabled = value_or<bool>(v, "Enabled", false);
    settings.vizio.ip = value_or<std::string>(v, "IP", "");
    settings.vizio.mac = value_or<std::string>(v, "MAC", "");
    settings.vizio.auth = value_or<std::string>(v, "Auth", "");
    return settings;
}

nlohmann::json ExternalTvSettings::toJson() const {
    return {
        {"Enabled", enabled},
        {"PreserveDesktopTopologyOnIdle", preserve_desktop_topology_on_idle},
        {"ExtendedIdleMinutes", extended_idle_minutes},
        {"Samsung", {{"Enabled", samsung.enabled}, {"IP", samsung.ip}, {"MAC", samsung.mac}, {"Token", samsung.token}}},
        {"Vizio", {{"Enabled", vizio.enabled}, {"IP", vizio.ip}, {"MAC", vizio.mac}, {"Auth", vizio.auth}}}
    };
}

nlohmann::json ExternalTvSettings::toRedactedJson() const {
    auto result = toJson();
    if (!samsung.token.empty()) result["Samsung"]["Token"] = "<redacted>";
    if (!vizio.auth.empty()) result["Vizio"]["Auth"] = "<redacted>";
    return result;
}
