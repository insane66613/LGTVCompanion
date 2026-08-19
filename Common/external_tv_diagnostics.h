#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

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

const char* externalTvDiagnosticDeviceName(ExternalTvDiagnosticDevice device);
const char* externalTvDiagnosticOperationName(ExternalTvDiagnosticOperation operation);

struct ExternalTvDiagnosticRequest {
    std::string request_id;
    ExternalTvDiagnosticDevice device{ExternalTvDiagnosticDevice::Samsung};
    ExternalTvDiagnosticOperation operation{ExternalTvDiagnosticOperation::Probe};

    nlohmann::json toJson() const;
    static std::optional<ExternalTvDiagnosticRequest> fromJson(
        const nlohmann::json& node, std::string* error = nullptr);
};

struct ExternalTvDiagnosticResponse {
    std::string request_id;
    ExternalTvDiagnosticDevice device{ExternalTvDiagnosticDevice::Samsung};
    ExternalTvDiagnosticOperation operation{ExternalTvDiagnosticOperation::Probe};
    bool accepted{false};
    bool executed{false};
    bool verified{false};
    std::string resulting_state{"unknown"};
    std::string message;

    bool semanticsValid() const;
    bool matchesRequest(std::string_view expected_request_id) const;
    nlohmann::json toJson() const;
    static std::optional<ExternalTvDiagnosticResponse> fromJson(
        const nlohmann::json& node, std::string* error = nullptr);
};
