#include "control_protocol_internal.hpp"

#include <array>
#include <limits>

namespace pulp::inspect::control_protocol_detail {
namespace {

template <typename Enum, std::size_t Size>
std::optional<Enum> enum_from_id(std::string_view id, const std::array<Enum, Size>& values,
                                 std::string_view (*to_id)(Enum)) {
    for (const auto value : values)
        if (to_id(value) == id)
            return value;
    return std::nullopt;
}

} // namespace

bool valid_host_open(const ControlHostOpenEnvelope& message) {
    const bool admission = valid_token(message.admission_id, kMaximumIdBytes);
    const bool enrollment = valid_token(message.enrollment_id, kMaximumIdBytes);
    return valid_token(message.request_id, kMaximumIdBytes) && admission != enrollment;
}

bool valid_host_open_result(const ControlHostOpenResult& message) {
    if (!valid_token(message.request_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kMaximumExplanationBytes))
        return false;
    if (message.accepted)
        return valid_token(message.registration_id, kMaximumIdBytes) &&
               message.error_code.empty() && message.explanation.empty();
    return message.registration_id.empty() &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty();
}

bool valid_host_execute(const ControlHostExecuteEnvelope& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.route_id, kMaximumIdBytes) &&
           valid_token(message.receipt_id, kMaximumIdBytes) &&
           valid_token(message.operation_id, kMaximumOperationIdBytes) &&
           message.operation_version != 0 && message.deadline_unix_ms > 0 &&
           message.expected_state_generation <= signed_max &&
           valid_control_json_bytes(message.params_json, kMaximumPayloadBytes);
}

bool valid_host_progress(const ControlHostProgressEnvelope& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.route_id, kMaximumIdBytes) && message.total != 0 &&
           message.current <= message.total && message.total <= signed_max &&
           valid_control_json_bytes(message.detail_json, kMaximumProgressBytes,
                                    kMaximumProgressJsonNodes);
}

bool valid_host_cancel(const ControlHostCancelEnvelope& message) {
    return valid_token(message.route_id, kMaximumIdBytes) && !message.reason.empty() &&
           valid_text(message.reason, kMaximumExplanationBytes);
}

bool valid_host_complete(const ControlHostCompleteEnvelope& message) {
    const bool allowed_state = message.terminal_state == ControlReceiptState::Completed ||
                               message.terminal_state == ControlReceiptState::Failed ||
                               message.terminal_state == ControlReceiptState::Cancelled;
    if (!allowed_state || !valid_token(message.route_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kControlReceiptMaximumExplanationBytes) ||
        !valid_text(message.cancellation_reason, kControlReceiptMaximumCancellationReasonBytes) ||
        !valid_control_json_bytes(message.detail_json, kControlMaximumResultDetailBytes,
                                  kMaximumResultJsonNodes))
        return false;
    if (message.terminal_state == ControlReceiptState::Cancelled)
        return message.result_code == ControlResultCode::Cancelled &&
               !message.cancellation_reason.empty();
    if (message.terminal_state == ControlReceiptState::Completed)
        return !message.result_code && message.cancellation_reason.empty();
    return message.result_code && message.result_code != ControlResultCode::Cancelled &&
           message.result_code != ControlResultCode::CompletedAfterRevocation &&
           message.cancellation_reason.empty();
}

bool is_host_control_kind(std::string_view kind) {
    return kind == "host-open" || kind == "host-opened" || kind == "host-execute" ||
           kind == "host-progress" || kind == "host-cancel" || kind == "host-complete";
}

std::optional<ControlEnvelopePayload>
decode_host_control_payload(std::string_view kind, ValueView payload,
                            ControlProtocolDiagnostics& error) {
    if (kind == "host-open") {
        if (!only_fields(payload, {"admission_id", "enrollment_id", "request_id"}, error))
            return std::nullopt;
        ControlHostOpenEnvelope message;
        if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes, error) ||
            (payload.hasObjectMember("admission_id") &&
             !required_string(payload, "admission_id", message.admission_id, kMaximumIdBytes, error,
                              false)) ||
            (payload.hasObjectMember("enrollment_id") &&
             !required_string(payload, "enrollment_id", message.enrollment_id, kMaximumIdBytes,
                              error, false)) ||
            !valid_host_open(message)) {
            error = {ControlProtocolError::InvalidValue, "host-open fields are invalid"};
            return std::nullopt;
        }
        return message;
    }
    if (kind == "host-opened") {
        if (!only_fields(payload,
                         {"accepted", "error_code", "explanation", "registration_id", "request_id"},
                         error))
            return std::nullopt;
        ControlHostOpenResult result;
        if (!required_string(payload, "request_id", result.request_id, kMaximumIdBytes, error) ||
            !required_bool(payload, "accepted", result.accepted, error) ||
            !required_string(payload, "registration_id", result.registration_id, kMaximumIdBytes,
                             error, false) ||
            !required_string(payload, "error_code", result.error_code,
                             kControlMaximumErrorCodeBytes, error, false) ||
            !required_string(payload, "explanation", result.explanation, kMaximumExplanationBytes,
                             error, false) ||
            !valid_host_open_result(result)) {
            error = {ControlProtocolError::InvalidValue,
                     "host-open result fields are inconsistent"};
            return std::nullopt;
        }
        return result;
    }
    if (kind == "host-execute") {
        if (!only_fields(payload,
                         {"deadline_unix_ms", "expected_state_generation", "operation_id",
                          "operation_version", "params", "receipt_id", "route_id"},
                         error))
            return std::nullopt;
        ControlHostExecuteEnvelope message;
        if (!required_string(payload, "route_id", message.route_id, kMaximumIdBytes, error) ||
            !required_string(payload, "receipt_id", message.receipt_id, kMaximumIdBytes, error) ||
            !required_string(payload, "operation_id", message.operation_id,
                             kMaximumOperationIdBytes, error) ||
            !required_u32(payload, "operation_version", message.operation_version, error) ||
            !required_i64(payload, "deadline_unix_ms", message.deadline_unix_ms, error) ||
            !required_u64(payload, "expected_state_generation", message.expected_state_generation,
                          error))
            return std::nullopt;
        if (!payload.hasObjectMember("params")) {
            error = {ControlProtocolError::MissingField, "missing field 'params'"};
            return std::nullopt;
        }
        message.params_json = choc::json::toString(canonical_value(payload["params"]), false);
        if (!valid_host_execute(message)) {
            error = {ControlProtocolError::InvalidValue, "host-execute fields are invalid"};
            return std::nullopt;
        }
        return message;
    }
    if (kind == "host-progress") {
        if (!only_fields(payload, {"current", "detail", "route_id", "total"}, error))
            return std::nullopt;
        ControlHostProgressEnvelope message;
        if (!required_string(payload, "route_id", message.route_id, kMaximumIdBytes, error) ||
            !required_u64(payload, "current", message.current, error) ||
            !required_u64(payload, "total", message.total, error))
            return std::nullopt;
        if (!payload.hasObjectMember("detail")) {
            error = {ControlProtocolError::MissingField, "missing field 'detail'"};
            return std::nullopt;
        }
        message.detail_json = choc::json::toString(canonical_value(payload["detail"]), false);
        if (!valid_host_progress(message)) {
            error = {ControlProtocolError::InvalidValue, "host-progress fields are invalid"};
            return std::nullopt;
        }
        return message;
    }
    if (kind == "host-cancel") {
        if (!only_fields(payload, {"reason", "route_id"}, error))
            return std::nullopt;
        ControlHostCancelEnvelope message;
        if (!required_string(payload, "route_id", message.route_id, kMaximumIdBytes, error) ||
            !required_string(payload, "reason", message.reason, kMaximumExplanationBytes, error,
                             false) ||
            !valid_host_cancel(message)) {
            error = {ControlProtocolError::InvalidValue, "host-cancel fields are invalid"};
            return std::nullopt;
        }
        return message;
    }

    if (!only_fields(payload,
                     {"cancellation_reason", "detail", "explanation", "result_code", "retry",
                      "route_id", "state"},
                     error))
        return std::nullopt;
    ControlHostCompleteEnvelope message;
    std::string state, retry;
    if (!required_string(payload, "route_id", message.route_id, kMaximumIdBytes, error) ||
        !required_string(payload, "state", state, 64, error) ||
        !required_string(payload, "retry", retry, 64, error) ||
        !required_string(payload, "explanation", message.explanation,
                         kControlReceiptMaximumExplanationBytes, error, false) ||
        !required_string(payload, "cancellation_reason", message.cancellation_reason,
                         kControlReceiptMaximumCancellationReasonBytes, error, false))
        return std::nullopt;
    if (!payload.hasObjectMember("detail") || !payload.hasObjectMember("result_code")) {
        error = {ControlProtocolError::MissingField, "host-complete fields are missing"};
        return std::nullopt;
    }
    constexpr std::array states{ControlReceiptState::Completed, ControlReceiptState::Failed,
                                ControlReceiptState::Cancelled};
    constexpr std::array retries{
        ControlRetryClassification::Never, ControlRetryClassification::AfterRefresh,
        ControlRetryClassification::AfterGrant, ControlRetryClassification::AfterBackoff};
    const auto parsed_state = enum_from_id(state, states, control_receipt_state_id);
    const auto parsed_retry = enum_from_id(retry, retries, control_retry_classification_id);
    if (!parsed_state || !parsed_retry) {
        error = {ControlProtocolError::InvalidValue, "host-complete state or retry is invalid"};
        return std::nullopt;
    }
    message.terminal_state = *parsed_state;
    message.retry = *parsed_retry;
    const auto result_code = payload["result_code"];
    if (!result_code.isVoid()) {
        if (!result_code.isString()) {
            error = {ControlProtocolError::InvalidType, "result_code must be a string or null"};
            return std::nullopt;
        }
        constexpr std::array codes{ControlResultCode::NotImplemented,
                                   ControlResultCode::NotBuilt,
                                   ControlResultCode::HostUnavailable,
                                   ControlResultCode::Inactive,
                                   ControlResultCode::PolicyDenied,
                                   ControlResultCode::GrantRequired,
                                   ControlResultCode::GrantExpired,
                                   ControlResultCode::ConsentRequired,
                                   ControlResultCode::LeaseConflict,
                                   ControlResultCode::StateConflict,
                                   ControlResultCode::SessionStale,
                                   ControlResultCode::InvalidRequest,
                                   ControlResultCode::DeadlineExceeded,
                                   ControlResultCode::Cancelled,
                                   ControlResultCode::CompletedAfterRevocation,
                                   ControlResultCode::UnknownNeedsRefresh,
                                   ControlResultCode::ResourceExhausted,
                                   ControlResultCode::InternalError};
        message.result_code =
            enum_from_id<ControlResultCode>(result_code.getString(), codes, control_result_code_id);
        if (!message.result_code) {
            error = {ControlProtocolError::InvalidValue, "result_code is unknown"};
            return std::nullopt;
        }
    }
    message.detail_json = choc::json::toString(canonical_value(payload["detail"]), false);
    if (!valid_host_complete(message)) {
        error = {ControlProtocolError::InvalidValue, "host-complete fields are inconsistent"};
        return std::nullopt;
    }
    return message;
}

} // namespace pulp::inspect::control_protocol_detail
