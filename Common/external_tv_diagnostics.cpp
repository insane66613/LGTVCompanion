#include "external_tv_diagnostics.h"

#include <array>

namespace {
constexpr const char* kNamespace = "external_tv";
constexpr std::size_t kMaxRequestIdLength = 128;
constexpr std::size_t kMaxStateLength = 64;
constexpr std::size_t kMaxMessageLength = 512;

template <std::size_t N>
bool containsOnlyKeys(const nlohmann::json& node, const std::array<const char*, N>& allowed) {
    if (!node.is_object()) return false;
    for (auto it = node.begin(); it != node.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

void setError(std::string* error, const char* message) {
    if (error) *error = message;
}

std::optional<ExternalTvDiagnosticDevice> parseDevice(const std::string& value) {
    if (value == "samsung") return ExternalTvDiagnosticDevice::Samsung;
    if (value == "vizio") return ExternalTvDiagnosticDevice::Vizio;
    return std::nullopt;
}

std::optional<ExternalTvDiagnosticOperation> parseOperation(const std::string& value) {
    if (value == "probe") return ExternalTvDiagnosticOperation::Probe;
    if (value == "blank") return ExternalTvDiagnosticOperation::Blank;
    if (value == "unblank") return ExternalTvDiagnosticOperation::Unblank;
    if (value == "power_on") return ExternalTvDiagnosticOperation::PowerOn;
    if (value == "power_off") return ExternalTvDiagnosticOperation::PowerOff;
    return std::nullopt;
}

bool boundedString(const nlohmann::json& node, const char* key, std::size_t max_length) {
    return node.contains(key) && node[key].is_string() &&
           !node[key].get_ref<const std::string&>().empty() &&
           node[key].get_ref<const std::string&>().size() <= max_length;
}
}  // namespace

const char* externalTvDiagnosticDeviceName(ExternalTvDiagnosticDevice device) {
    switch (device) {
    case ExternalTvDiagnosticDevice::Samsung: return "samsung";
    case ExternalTvDiagnosticDevice::Vizio: return "vizio";
    }
    return "unknown";
}

const char* externalTvDiagnosticOperationName(ExternalTvDiagnosticOperation operation) {
    switch (operation) {
    case ExternalTvDiagnosticOperation::Probe: return "probe";
    case ExternalTvDiagnosticOperation::Blank: return "blank";
    case ExternalTvDiagnosticOperation::Unblank: return "unblank";
    case ExternalTvDiagnosticOperation::PowerOn: return "power_on";
    case ExternalTvDiagnosticOperation::PowerOff: return "power_off";
    }
    return "unknown";
}

nlohmann::json ExternalTvDiagnosticRequest::toJson() const {
    return {{"namespace", kNamespace}, {"request_id", request_id},
            {"device", externalTvDiagnosticDeviceName(device)},
            {"operation", externalTvDiagnosticOperationName(operation)}};
}

std::optional<ExternalTvDiagnosticRequest> ExternalTvDiagnosticRequest::fromJson(
    const nlohmann::json& node, std::string* error) {
    static constexpr std::array<const char*, 4> allowed{
        "namespace", "request_id", "device", "operation"};
    if (!containsOnlyKeys(node, allowed)) {
        setError(error, "unexpected or credential-bearing diagnostic request field");
        return std::nullopt;
    }
    if (!boundedString(node, "namespace", 32) || node["namespace"] != kNamespace ||
        !boundedString(node, "request_id", kMaxRequestIdLength) ||
        !boundedString(node, "device", 16) || !boundedString(node, "operation", 32)) {
        setError(error, "invalid diagnostic request envelope");
        return std::nullopt;
    }
    const auto device = parseDevice(node["device"].get<std::string>());
    const auto operation = parseOperation(node["operation"].get<std::string>());
    if (!device || !operation) {
        setError(error, "unsupported diagnostic device or operation");
        return std::nullopt;
    }
    if (error) error->clear();
    return ExternalTvDiagnosticRequest{
        node["request_id"].get<std::string>(), *device, *operation};
}

bool ExternalTvDiagnosticResponse::semanticsValid() const {
    if (request_id.empty() || request_id.size() > kMaxRequestIdLength) return false;
    if (executed && !accepted) return false;
    if (verified && (!accepted || !executed)) return false;
    if (resulting_state.empty() || resulting_state.size() > kMaxStateLength) return false;
    if (message.size() > kMaxMessageLength) return false;
    if (verified && resulting_state == "unknown") return false;
    return true;
}

bool ExternalTvDiagnosticResponse::matchesRequest(std::string_view expected_request_id) const {
    return request_id == expected_request_id;
}

nlohmann::json ExternalTvDiagnosticResponse::toJson() const {
    return {{"namespace", kNamespace}, {"request_id", request_id},
            {"device", externalTvDiagnosticDeviceName(device)},
            {"operation", externalTvDiagnosticOperationName(operation)},
            {"accepted", accepted}, {"executed", executed}, {"verified", verified},
            {"resulting_state", resulting_state}, {"message", message}};
}

std::optional<ExternalTvDiagnosticResponse> ExternalTvDiagnosticResponse::fromJson(
    const nlohmann::json& node, std::string* error) {
    static constexpr std::array<const char*, 9> allowed{
        "namespace", "request_id", "device", "operation", "accepted",
        "executed", "verified", "resulting_state", "message"};
    if (!containsOnlyKeys(node, allowed) || !boundedString(node, "namespace", 32) ||
        node["namespace"] != kNamespace || !boundedString(node, "request_id", kMaxRequestIdLength) ||
        !boundedString(node, "device", 16) || !boundedString(node, "operation", 32) ||
        !boundedString(node, "resulting_state", kMaxStateLength) ||
        !node.contains("message") || !node["message"].is_string() ||
        node["message"].get_ref<const std::string&>().size() > kMaxMessageLength ||
        !node.contains("accepted") || !node["accepted"].is_boolean() ||
        !node.contains("executed") || !node["executed"].is_boolean() ||
        !node.contains("verified") || !node["verified"].is_boolean()) {
        setError(error, "invalid diagnostic response envelope");
        return std::nullopt;
    }
    const auto device = parseDevice(node["device"].get<std::string>());
    const auto operation = parseOperation(node["operation"].get<std::string>());
    if (!device || !operation) {
        setError(error, "unsupported diagnostic response device or operation");
        return std::nullopt;
    }
    ExternalTvDiagnosticResponse response;
    response.request_id = node["request_id"].get<std::string>();
    response.device = *device;
    response.operation = *operation;
    response.accepted = node["accepted"].get<bool>();
    response.executed = node["executed"].get<bool>();
    response.verified = node["verified"].get<bool>();
    response.resulting_state = node["resulting_state"].get<std::string>();
    response.message = node["message"].get<std::string>();
    if (!response.semanticsValid()) {
        setError(error, "impossible diagnostic response semantics");
        return std::nullopt;
    }
    if (error) error->clear();
    return response;
}
