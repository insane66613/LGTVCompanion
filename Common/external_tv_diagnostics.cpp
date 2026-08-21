#include "external_tv_diagnostics.h"

#include <array>

namespace {
constexpr const char* kNamespace = "external_tv";
constexpr const char* kRequestType = "external_tv_diagnostic";
constexpr const char* kResponseType = "response";
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

std::optional<ExternalTvDiagnosticAction> parseAction(const std::string& value) {
    if (value == "device_operation") return ExternalTvDiagnosticAction::DeviceOperation;
    if (value == "snapshot") return ExternalTvDiagnosticAction::Snapshot;
    if (value == "simulate_user_idle") return ExternalTvDiagnosticAction::SimulateUserIdle;
    if (value == "simulate_user_busy") return ExternalTvDiagnosticAction::SimulateUserBusy;
    if (value == "simulate_extended_idle") return ExternalTvDiagnosticAction::SimulateExtendedIdle;
    if (value == "simulate_suspend") return ExternalTvDiagnosticAction::SimulateSuspend;
    if (value == "simulate_shutdown") return ExternalTvDiagnosticAction::SimulateShutdown;
    return std::nullopt;
}

bool boundedString(const nlohmann::json& node, const char* key, std::size_t max_length) {
    return node.contains(key) && node[key].is_string() &&
           !node[key].get_ref<const std::string&>().empty() &&
           node[key].get_ref<const std::string&>().size() <= max_length;
}

bool nonNegativeInteger(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return true;
    return value.is_number_integer() && value.get<std::int64_t>() >= 0;
}

std::uint64_t unsignedValue(const nlohmann::json& value) {
    return value.is_number_unsigned() ? value.get<std::uint64_t>()
                                      : static_cast<std::uint64_t>(value.get<std::int64_t>());
}

bool boundedStringArray(const nlohmann::json& value, std::size_t max_items, std::size_t max_length) {
    if (!value.is_array() || value.size() > max_items) return false;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty() ||
            item.get_ref<const std::string&>().size() > max_length)
            return false;
    }
    return true;
}

std::optional<ExternalTvDiagnosticSnapshot> parseSnapshotPayload(const nlohmann::json& node) {
    static constexpr std::array<const char*, 9> allowed{
        "service_running", "orchestration_enabled", "idle_state", "deadline_pending",
        "deadline_remaining_seconds", "samsung_state", "vizio_state",
        "samsung_token_present", "vizio_auth_present"};
    if (!containsOnlyKeys(node, allowed) || node.size() != allowed.size() ||
        !node["service_running"].is_boolean() || !node["orchestration_enabled"].is_boolean() ||
        !boundedString(node, "idle_state", 32) || !node["deadline_pending"].is_boolean() ||
        !nonNegativeInteger(node["deadline_remaining_seconds"]) ||
        !boundedString(node, "samsung_state", 64) || !boundedString(node, "vizio_state", 64) ||
        !node["samsung_token_present"].is_boolean() || !node["vizio_auth_present"].is_boolean())
        return std::nullopt;

    ExternalTvDiagnosticSnapshot value;
    value.service_running = node["service_running"].get<bool>();
    value.orchestration_enabled = node["orchestration_enabled"].get<bool>();
    value.idle_state = node["idle_state"].get<std::string>();
    value.deadline_pending = node["deadline_pending"].get<bool>();
    value.deadline_remaining_seconds = node["deadline_remaining_seconds"].get<std::int64_t>();
    value.samsung_state = node["samsung_state"].get<std::string>();
    value.vizio_state = node["vizio_state"].get<std::string>();
    value.samsung_token_present = node["samsung_token_present"].get<bool>();
    value.vizio_auth_present = node["vizio_auth_present"].get<bool>();
    return value;
}

std::optional<ExternalTvLifecycleSimulation> parseSimulationPayload(const nlohmann::json& node) {
    static constexpr std::array<const char*, 6> allowed{
        "lifecycle_state", "samsung_actions", "vizio_actions", "deadline_pending",
        "deadline_remaining_seconds", "would_mark_success_if_operations_succeed"};
    if (!containsOnlyKeys(node, allowed) || node.size() != allowed.size() ||
        !boundedString(node, "lifecycle_state", 32) ||
        !boundedStringArray(node["samsung_actions"], 16, 64) ||
        !boundedStringArray(node["vizio_actions"], 16, 64) ||
        !node["deadline_pending"].is_boolean() ||
        !nonNegativeInteger(node["deadline_remaining_seconds"]) ||
        !node["would_mark_success_if_operations_succeed"].is_boolean())
        return std::nullopt;

    ExternalTvLifecycleSimulation value;
    value.lifecycle_state = node["lifecycle_state"].get<std::string>();
    value.samsung_actions = node["samsung_actions"].get<std::vector<std::string>>();
    value.vizio_actions = node["vizio_actions"].get<std::vector<std::string>>();
    value.deadline_pending = node["deadline_pending"].get<bool>();
    value.deadline_remaining_seconds = node["deadline_remaining_seconds"].get<std::int64_t>();
    value.would_mark_success_if_operations_succeed =
        node["would_mark_success_if_operations_succeed"].get<bool>();
    return value;
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

const char* externalTvDiagnosticActionName(ExternalTvDiagnosticAction action) {
    switch (action) {
    case ExternalTvDiagnosticAction::DeviceOperation: return "device_operation";
    case ExternalTvDiagnosticAction::Snapshot: return "snapshot";
    case ExternalTvDiagnosticAction::SimulateUserIdle: return "simulate_user_idle";
    case ExternalTvDiagnosticAction::SimulateUserBusy: return "simulate_user_busy";
    case ExternalTvDiagnosticAction::SimulateExtendedIdle: return "simulate_extended_idle";
    case ExternalTvDiagnosticAction::SimulateSuspend: return "simulate_suspend";
    case ExternalTvDiagnosticAction::SimulateShutdown: return "simulate_shutdown";
    }
    return "unknown";
}

nlohmann::json ExternalTvDiagnosticRequest::toJson() const {
    nlohmann::json node = {{"type", kRequestType}, {"namespace", kNamespace},
                           {"request_id", request_id},
                           {"action", externalTvDiagnosticActionName(action)}};
    if (action == ExternalTvDiagnosticAction::DeviceOperation) {
        node["device"] = externalTvDiagnosticDeviceName(device);
        node["operation"] = externalTvDiagnosticOperationName(operation);
    }
    return node;
}

std::optional<ExternalTvDiagnosticRequest> ExternalTvDiagnosticRequest::fromJson(
    const nlohmann::json& node, std::string* error) {
    static constexpr std::array<const char*, 6> allowed{
        "type", "namespace", "request_id", "action", "device", "operation"};
    if (!containsOnlyKeys(node, allowed)) {
        setError(error, "unexpected or credential-bearing diagnostic request field");
        return std::nullopt;
    }
    if (!boundedString(node, "type", 32) || node["type"] != kRequestType ||
        !boundedString(node, "namespace", 32) || node["namespace"] != kNamespace ||
        !boundedString(node, "request_id", kMaxRequestIdLength) ||
        !boundedString(node, "action", 32)) {
        setError(error, "invalid diagnostic request envelope");
        return std::nullopt;
    }
    const auto action = parseAction(node["action"].get<std::string>());
    if (!action) {
        setError(error, "unsupported diagnostic action");
        return std::nullopt;
    }

    ExternalTvDiagnosticRequest request;
    request.request_id = node["request_id"].get<std::string>();
    request.action = *action;
    if (*action == ExternalTvDiagnosticAction::DeviceOperation) {
        if (!boundedString(node, "device", 16) || !boundedString(node, "operation", 32)) {
            setError(error, "device diagnostic request requires device and operation");
            return std::nullopt;
        }
        const auto device = parseDevice(node["device"].get<std::string>());
        const auto operation = parseOperation(node["operation"].get<std::string>());
        if (!device || !operation) {
            setError(error, "unsupported diagnostic device or operation");
            return std::nullopt;
        }
        request.device = *device;
        request.operation = *operation;
    } else if (node.contains("device") || node.contains("operation")) {
        setError(error, "non-device diagnostic action must not include device fields");
        return std::nullopt;
    }
    if (error) error->clear();
    return request;
}

bool ExternalTvDiagnosticResponse::semanticsValid() const {
    if (request_id.empty() || request_id.size() > kMaxRequestIdLength) return false;
    if (executed && !accepted) return false;
    if (verified && (!accepted || !executed)) return false;
    if (resulting_state.empty() || resulting_state.size() > kMaxStateLength) return false;
    if (message.size() > kMaxMessageLength) return false;
    if (verified && resulting_state == "unknown") return false;

    if (action == ExternalTvDiagnosticAction::DeviceOperation)
        return !snapshot.has_value() && !simulation.has_value();
    if (action == ExternalTvDiagnosticAction::Snapshot) {
        if (simulation.has_value()) return false;
        return !accepted || !executed || snapshot.has_value();
    }
    if (snapshot.has_value()) return false;
    return !accepted || !executed || simulation.has_value();
}

bool ExternalTvDiagnosticResponse::matchesRequest(std::string_view expected_request_id) const {
    return request_id == expected_request_id;
}

nlohmann::json ExternalTvDiagnosticResponse::toJson() const {
    nlohmann::json node = {{"type", kResponseType}, {"namespace", kNamespace},
                           {"request_id", request_id},
                           {"action", externalTvDiagnosticActionName(action)},
                           {"accepted", accepted}, {"executed", executed},
                           {"verified", verified}, {"resulting_state", resulting_state},
                           {"message", message}};
    if (action == ExternalTvDiagnosticAction::DeviceOperation) {
        node["device"] = externalTvDiagnosticDeviceName(device);
        node["operation"] = externalTvDiagnosticOperationName(operation);
    }
    if (snapshot) {
        node["snapshot"] = {
            {"service_running", snapshot->service_running},
            {"orchestration_enabled", snapshot->orchestration_enabled},
            {"idle_state", snapshot->idle_state},
            {"deadline_pending", snapshot->deadline_pending},
            {"deadline_remaining_seconds", snapshot->deadline_remaining_seconds},
            {"samsung_state", snapshot->samsung_state}, {"vizio_state", snapshot->vizio_state},
            {"samsung_token_present", snapshot->samsung_token_present},
            {"vizio_auth_present", snapshot->vizio_auth_present}};
    }
    if (simulation) {
        node["simulation"] = {
            {"lifecycle_state", simulation->lifecycle_state},
            {"samsung_actions", simulation->samsung_actions},
            {"vizio_actions", simulation->vizio_actions},
            {"deadline_pending", simulation->deadline_pending},
            {"deadline_remaining_seconds", simulation->deadline_remaining_seconds},
            {"would_mark_success_if_operations_succeed",
             simulation->would_mark_success_if_operations_succeed}};
    }
    return node;
}

std::optional<ExternalTvDiagnosticResponse> ExternalTvDiagnosticResponse::fromJson(
    const nlohmann::json& node, std::string* error) {
    static constexpr std::array<const char*, 13> allowed{
        "type", "namespace", "request_id", "action", "device", "operation",
        "accepted", "executed", "verified", "resulting_state", "message",
        "snapshot", "simulation"};
    if (!containsOnlyKeys(node, allowed) || !boundedString(node, "type", 32) ||
        node["type"] != kResponseType || !boundedString(node, "namespace", 32) ||
        node["namespace"] != kNamespace || !boundedString(node, "request_id", kMaxRequestIdLength) ||
        !boundedString(node, "action", 32) || !boundedString(node, "resulting_state", kMaxStateLength) ||
        !node.contains("message") || !node["message"].is_string() ||
        node["message"].get_ref<const std::string&>().size() > kMaxMessageLength ||
        !node.contains("accepted") || !node["accepted"].is_boolean() ||
        !node.contains("executed") || !node["executed"].is_boolean() ||
        !node.contains("verified") || !node["verified"].is_boolean()) {
        setError(error, "invalid diagnostic response envelope");
        return std::nullopt;
    }
    const auto action = parseAction(node["action"].get<std::string>());
    if (!action) {
        setError(error, "unsupported diagnostic response action");
        return std::nullopt;
    }

    ExternalTvDiagnosticResponse response;
    response.request_id = node["request_id"].get<std::string>();
    response.action = *action;
    response.accepted = node["accepted"].get<bool>();
    response.executed = node["executed"].get<bool>();
    response.verified = node["verified"].get<bool>();
    response.resulting_state = node["resulting_state"].get<std::string>();
    response.message = node["message"].get<std::string>();

    if (*action == ExternalTvDiagnosticAction::DeviceOperation) {
        if (!boundedString(node, "device", 16) || !boundedString(node, "operation", 32) ||
            node.contains("snapshot") || node.contains("simulation")) {
            setError(error, "invalid device diagnostic response payload");
            return std::nullopt;
        }
        const auto device = parseDevice(node["device"].get<std::string>());
        const auto operation = parseOperation(node["operation"].get<std::string>());
        if (!device || !operation) {
            setError(error, "unsupported diagnostic response device or operation");
            return std::nullopt;
        }
        response.device = *device;
        response.operation = *operation;
    } else {
        if (node.contains("device") || node.contains("operation")) {
            setError(error, "non-device diagnostic response must not include device fields");
            return std::nullopt;
        }
        if (*action == ExternalTvDiagnosticAction::Snapshot) {
            if (node.contains("simulation")) {
                setError(error, "snapshot response must not include simulation payload");
                return std::nullopt;
            }
            if (node.contains("snapshot")) {
                response.snapshot = parseSnapshotPayload(node["snapshot"]);
                if (!response.snapshot) {
                    setError(error, "invalid diagnostic snapshot payload");
                    return std::nullopt;
                }
            }
        } else {
            if (node.contains("snapshot")) {
                setError(error, "simulation response must not include snapshot payload");
                return std::nullopt;
            }
            if (node.contains("simulation")) {
                response.simulation = parseSimulationPayload(node["simulation"]);
                if (!response.simulation) {
                    setError(error, "invalid lifecycle simulation payload");
                    return std::nullopt;
                }
            }
        }
    }
    if (!response.semanticsValid()) {
        setError(error, "impossible diagnostic response semantics");
        return std::nullopt;
    }
    if (error) error->clear();
    return response;
}
