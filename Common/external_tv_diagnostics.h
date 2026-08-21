#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ExternalTvDiagnosticDevice {
    Samsung,
    Vizio
};

enum class ExternalTvDiagnosticOperation {
    Probe,
    Blank,
    Unblank,
    PowerOn,
    PowerOff
};

enum class ExternalTvDiagnosticAction {
    DeviceOperation,
    Snapshot,
    SimulateUserIdle,
    SimulateUserBusy,
    SimulateExtendedIdle,
    SimulateSuspend,
    SimulateShutdown
};

const char* externalTvDiagnosticDeviceName(ExternalTvDiagnosticDevice device);
const char* externalTvDiagnosticOperationName(ExternalTvDiagnosticOperation operation);
const char* externalTvDiagnosticActionName(ExternalTvDiagnosticAction action);

struct ExternalTvDiagnosticRequest {
    std::string request_id;
    ExternalTvDiagnosticAction action{ExternalTvDiagnosticAction::DeviceOperation};
    ExternalTvDiagnosticDevice device{ExternalTvDiagnosticDevice::Samsung};
    ExternalTvDiagnosticOperation operation{ExternalTvDiagnosticOperation::Probe};

    nlohmann::json toJson() const;
    static std::optional<ExternalTvDiagnosticRequest> fromJson(
        const nlohmann::json& node, std::string* error = nullptr);
};

struct ExternalTvDiagnosticSnapshot {
    bool service_running{false};
    bool orchestration_enabled{false};
    std::string idle_state{"unknown"};
    bool deadline_pending{false};
    std::int64_t deadline_remaining_seconds{0};
    std::string samsung_state{"unknown"};
    std::string vizio_state{"unknown"};
    bool samsung_token_present{false};
    bool vizio_auth_present{false};
};

struct ExternalTvLifecycleSimulation {
    std::string lifecycle_state{"unknown"};
    std::vector<std::string> samsung_actions;
    std::vector<std::string> vizio_actions;
    bool deadline_pending{false};
    std::int64_t deadline_remaining_seconds{0};
    bool would_mark_success_if_operations_succeed{false};
};

struct ExternalTvDiagnosticResponse {
    std::string request_id;
    ExternalTvDiagnosticAction action{ExternalTvDiagnosticAction::DeviceOperation};
    ExternalTvDiagnosticDevice device{ExternalTvDiagnosticDevice::Samsung};
    ExternalTvDiagnosticOperation operation{ExternalTvDiagnosticOperation::Probe};
    bool accepted{false};
    bool executed{false};
    bool verified{false};
    std::string resulting_state{"unknown"};
    std::string message;
    std::optional<ExternalTvDiagnosticSnapshot> snapshot;
    std::optional<ExternalTvLifecycleSimulation> simulation;

    bool semanticsValid() const;
    bool matchesRequest(std::string_view expected_request_id) const;
    nlohmann::json toJson() const;
    static std::optional<ExternalTvDiagnosticResponse> fromJson(
        const nlohmann::json& node, std::string* error = nullptr);
};
