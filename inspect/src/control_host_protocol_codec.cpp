#include "control_protocol_internal.hpp"

#include <algorithm>
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
               valid_token(message.broker_id, kMaximumIdBytes) &&
               valid_token(message.session_id, kMaximumIdBytes) &&
               valid_token(message.instance_id, kMaximumIdBytes) &&
               valid_token(message.publication_id, kMaximumIdBytes) &&
               message.instance_generation == message.publication_id &&
               valid_hash(message.manifest_digest) &&
               valid_hash(message.producer_artifact_digest) &&
               message.error_code.empty() && message.explanation.empty();
    return message.registration_id.empty() && message.broker_id.empty() &&
           message.session_id.empty() && message.instance_id.empty() &&
           message.publication_id.empty() && message.instance_generation.empty() &&
           message.manifest_digest.empty() && message.producer_artifact_digest.empty() &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty();
}

bool valid_host_ready(const ControlHostReadyEnvelope& message) {
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.registration_id, kMaximumIdBytes);
}

bool valid_host_ready_result(const ControlHostReadyResult& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid_token(message.request_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kMaximumExplanationBytes))
        return false;
    if (message.accepted)
        return message.liveness_generation != 0 && message.liveness_generation <= signed_max &&
               message.error_code.empty() &&
               message.explanation.empty();
    return message.liveness_generation == 0 &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty();
}

bool valid_host_heartbeat(const ControlHostHeartbeatEnvelope& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.registration_id, kMaximumIdBytes) &&
           message.liveness_generation != 0 && message.liveness_generation <= signed_max;
}

bool valid_host_heartbeat_result(const ControlHostHeartbeatResult& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid_token(message.request_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kMaximumExplanationBytes))
        return false;
    if (message.accepted)
        return message.liveness_generation != 0 && message.liveness_generation <= signed_max &&
               message.error_code.empty() &&
               message.explanation.empty();
    return message.liveness_generation == 0 &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty();
}

bool valid_host_execute(const ControlHostExecuteEnvelope& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.route_id, kMaximumIdBytes) &&
           valid_token(message.receipt_id, kMaximumIdBytes) &&
           valid_token(message.authority_id, kMaximumIdBytes) &&
           valid_token(message.controller_authority_id, kMaximumIdBytes) &&
           valid_token(message.broker_id, kMaximumIdBytes) &&
           valid_token(message.session_id, kMaximumIdBytes) &&
           valid_token(message.instance_id, kMaximumIdBytes) &&
           valid_token(message.publication_id, kMaximumIdBytes) &&
           message.instance_generation == message.publication_id &&
           valid_token(message.capability_id, kMaximumOperationIdBytes) &&
           valid_hash(message.manifest_digest) &&
           valid_hash(message.producer_artifact_digest) &&
           valid_token(message.operation_id, kMaximumOperationIdBytes) &&
           message.operation_version != 0 && message.deadline_unix_ms > 0 &&
           message.expected_state_generation <= signed_max &&
           valid_control_json_bytes(message.params_json, kMaximumPayloadBytes);
}

bool valid_host_authority_end(const ControlHostAuthorityEndEnvelope& message) {
    return valid_token(message.authority_id, kMaximumIdBytes) && !message.reason.empty() &&
           valid_text(message.reason, kMaximumExplanationBytes);
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

bool valid_host_artifact_publication(const ControlHostArtifactPublication& publication) {
    constexpr auto maximum_encoded =
        4 * ((kControlHostMaximumArtifactPublicationBytes + 2) / 3);
    if (!valid_token(publication.reference_id, kMaximumIdBytes) ||
        publication.bytes_base64.empty() || publication.bytes_base64.size() > maximum_encoded ||
        publication.bytes_base64.size() % 4 != 0 ||
        !valid_text(publication.content_type, 256) || publication.content_type.empty() ||
        publication.lifetime_ms <= 0 || publication.lifetime_ms > 86'400'000)
        return false;
    std::size_t padding = 0;
    for (std::size_t i = 0; i < publication.bytes_base64.size(); ++i) {
        const auto c = publication.bytes_base64[i];
        const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (c == '=') {
            ++padding;
            if (i + 2 < publication.bytes_base64.size() || padding > 2)
                return false;
        } else if (!alphabet || padding != 0) {
            return false;
        }
    }
    const auto decoded = publication.bytes_base64.size() / 4 * 3 - padding;
    return decoded != 0 && decoded <= kControlHostMaximumArtifactPublicationBytes;
}

bool valid_host_complete(const ControlHostCompleteEnvelope& message) {
    const bool allowed_state = message.terminal_state == ControlReceiptState::Completed ||
                               message.terminal_state == ControlReceiptState::Failed ||
                               message.terminal_state == ControlReceiptState::Cancelled;
    if (!allowed_state || !valid_token(message.route_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kControlReceiptMaximumExplanationBytes) ||
        !valid_text(message.cancellation_reason, kControlReceiptMaximumCancellationReasonBytes) ||
        !valid_control_json_bytes(message.detail_json, kControlMaximumResultDetailBytes,
                                  kMaximumResultJsonNodes) ||
        message.artifact_publications.size() > kControlHostMaximumArtifactPublications)
        return false;
    if (!std::ranges::all_of(message.artifact_publications,
                             valid_host_artifact_publication))
        return false;
    if (message.terminal_state == ControlReceiptState::Cancelled)
        return message.artifact_publications.empty() &&
               message.result_code == ControlResultCode::Cancelled &&
               !message.cancellation_reason.empty();
    if (message.terminal_state == ControlReceiptState::Completed)
        return !message.result_code && message.cancellation_reason.empty();
    return message.artifact_publications.empty() && message.result_code &&
           message.result_code != ControlResultCode::Cancelled &&
           message.result_code != ControlResultCode::CompletedAfterRevocation &&
           message.cancellation_reason.empty();
}

bool is_host_control_kind(std::string_view kind) {
    return kind == "host-open" || kind == "host-opened" || kind == "host-execute" ||
           kind == "host-ready" || kind == "host-ready-result" ||
           kind == "host-heartbeat" || kind == "host-heartbeat-result" ||
           kind == "host-progress" || kind == "host-cancel" ||
           kind == "host-authority-end" || kind == "host-complete";
}

bool is_host_preflight_kind(std::string_view kind) {
    return kind == "host-preflight-challenge" || kind == "host-preflight-response" ||
           kind == "host-preflight-bootstrap";
}

std::optional<ControlEnvelopePayload>
decode_host_preflight_payload(std::string_view kind, ValueView payload,
                              ControlProtocolDiagnostics& error) {
    if (kind == "host-preflight-challenge" || kind == "host-preflight-response") {
        if (!only_fields(payload, {"nonce"}, error))
            return std::nullopt;
        std::string nonce;
        if (!required_string(payload, "nonce", nonce, 64, error) || !valid_hash(nonce)) {
            error = {ControlProtocolError::InvalidValue, "host preflight nonce is invalid"};
            return std::nullopt;
        }
        if (kind == "host-preflight-challenge")
            return ControlHostPreflightChallengeEnvelope{std::move(nonce)};
        return ControlHostPreflightResponseEnvelope{std::move(nonce)};
    }
    if (!only_fields(payload, {"bootstrap_base64", "nonce"}, error))
        return std::nullopt;
    ControlHostPreflightBootstrapEnvelope message;
    if (!required_string(payload, "nonce", message.nonce, 64, error) ||
        !required_string(payload, "bootstrap_base64", message.bootstrap_base64,
                         kControlHostPreflightMaximumBootstrapBase64Bytes, error) ||
        !valid_hash(message.nonce) || message.bootstrap_base64.empty()) {
        error = {ControlProtocolError::InvalidValue, "host preflight bootstrap fields are invalid"};
        return std::nullopt;
    }
    return message;
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
                         {"accepted", "broker_id", "error_code", "explanation", "instance_generation",
                          "instance_id", "manifest_digest", "producer_artifact_digest",
                          "publication_id", "registration_id", "request_id", "session_id"},
                         error))
            return std::nullopt;
        ControlHostOpenResult result;
        if (!required_string(payload, "request_id", result.request_id, kMaximumIdBytes, error) ||
            !required_bool(payload, "accepted", result.accepted, error) ||
            !required_string(payload, "registration_id", result.registration_id, kMaximumIdBytes,
                             error, false) ||
            !required_string(payload, "broker_id", result.broker_id, kMaximumIdBytes, error,
                             false) ||
            !required_string(payload, "session_id", result.session_id, kMaximumIdBytes, error,
                             false) ||
            !required_string(payload, "instance_id", result.instance_id, kMaximumIdBytes, error,
                             false) ||
            !required_string(payload, "publication_id", result.publication_id, kMaximumIdBytes,
                             error, false) ||
            !required_string(payload, "instance_generation", result.instance_generation,
                             kMaximumIdBytes, error, false) ||
            !required_string(payload, "manifest_digest", result.manifest_digest, 64, error,
                             false) ||
            !required_string(payload, "producer_artifact_digest",
                             result.producer_artifact_digest, 64, error, false) ||
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
    if (kind == "host-ready") {
        if (!only_fields(payload, {"registration_id", "request_id"}, error))
            return std::nullopt;
        ControlHostReadyEnvelope message;
        if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes, error) ||
            !required_string(payload, "registration_id", message.registration_id,
                             kMaximumIdBytes, error) || !valid_host_ready(message))
            return std::nullopt;
        return message;
    }
    if (kind == "host-ready-result" || kind == "host-heartbeat-result") {
        if (!only_fields(payload,
                         {"accepted", "error_code", "explanation", "liveness_generation",
                          "request_id"}, error))
            return std::nullopt;
        ControlHostReadyResult result;
        if (!required_string(payload, "request_id", result.request_id, kMaximumIdBytes, error) ||
            !required_bool(payload, "accepted", result.accepted, error) ||
            !required_u64(payload, "liveness_generation", result.liveness_generation, error) ||
            !required_string(payload, "error_code", result.error_code,
                             kControlMaximumErrorCodeBytes, error, false) ||
            !required_string(payload, "explanation", result.explanation,
                             kMaximumExplanationBytes, error, false) ||
            !valid_host_ready_result(result))
            return std::nullopt;
        if (kind == "host-ready-result")
            return result;
        return ControlHostHeartbeatResult{result.request_id, result.accepted,
                                          result.liveness_generation, result.error_code,
                                          result.explanation};
    }
    if (kind == "host-heartbeat") {
        if (!only_fields(payload,
                         {"liveness_generation", "registration_id", "request_id"}, error))
            return std::nullopt;
        ControlHostHeartbeatEnvelope message;
        if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes, error) ||
            !required_string(payload, "registration_id", message.registration_id,
                             kMaximumIdBytes, error) ||
            !required_u64(payload, "liveness_generation", message.liveness_generation, error) ||
            !valid_host_heartbeat(message))
            return std::nullopt;
        return message;
    }
    if (kind == "host-execute") {
        if (!only_fields(payload,
                         {"authority_id", "broker_id", "capability_id",
                          "controller_authority_id", "deadline_unix_ms",
                          "expected_state_generation", "instance_generation", "instance_id",
                          "manifest_digest", "operation_id", "operation_version", "params",
                          "producer_artifact_digest", "publication_id", "receipt_id", "route_id",
                          "session_id"},
                         error))
            return std::nullopt;
        ControlHostExecuteEnvelope message;
        if (!required_string(payload, "route_id", message.route_id, kMaximumIdBytes, error) ||
            !required_string(payload, "receipt_id", message.receipt_id, kMaximumIdBytes, error) ||
            !required_string(payload, "authority_id", message.authority_id, kMaximumIdBytes,
                             error) ||
            !required_string(payload, "controller_authority_id",
                             message.controller_authority_id, kMaximumIdBytes, error) ||
            !required_string(payload, "broker_id", message.broker_id, kMaximumIdBytes, error) ||
            !required_string(payload, "session_id", message.session_id, kMaximumIdBytes, error) ||
            !required_string(payload, "instance_id", message.instance_id, kMaximumIdBytes, error) ||
            !required_string(payload, "publication_id", message.publication_id, kMaximumIdBytes,
                             error) ||
            !required_string(payload, "instance_generation", message.instance_generation,
                             kMaximumIdBytes, error) ||
            !required_string(payload, "capability_id", message.capability_id,
                             kMaximumOperationIdBytes, error) ||
            !required_string(payload, "manifest_digest", message.manifest_digest, 64, error) ||
            !required_string(payload, "producer_artifact_digest",
                             message.producer_artifact_digest, 64, error) ||
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
    if (kind == "host-authority-end") {
        if (!only_fields(payload, {"authority_id", "reason"}, error))
            return std::nullopt;
        ControlHostAuthorityEndEnvelope message;
        if (!required_string(payload, "authority_id", message.authority_id, kMaximumIdBytes,
                             error) ||
            !required_string(payload, "reason", message.reason, kMaximumExplanationBytes, error,
                             false) || !valid_host_authority_end(message))
            return std::nullopt;
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
                     {"artifact_publications", "cancellation_reason", "detail", "explanation",
                      "result_code", "retry", "route_id", "state"},
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
    if (payload.hasObjectMember("artifact_publications")) {
        const auto publications = payload["artifact_publications"];
        if (!publications.isArray()) {
            error = {ControlProtocolError::InvalidType,
                     "host artifact publications must be an array"};
            return std::nullopt;
        }
        if (publications.size() > kControlHostMaximumArtifactPublications) {
            error = {ControlProtocolError::LimitExceeded,
                     "host artifact publication count exceeds its bound"};
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < publications.size(); ++i) {
            const auto value = publications[i];
            if (!only_fields(value,
                             {"bytes_base64", "content_type", "lifetime_ms", "redaction_state",
                              "reference_id", "sensitivity"},
                             error))
                return std::nullopt;
            ControlHostArtifactPublication publication;
            std::int64_t sensitivity = 0, redaction = 0;
            if (!required_string(value, "reference_id", publication.reference_id,
                                 kMaximumIdBytes, error) ||
                !required_string(value, "bytes_base64", publication.bytes_base64,
                                 4 * ((kControlHostMaximumArtifactPublicationBytes + 2) / 3),
                                 error, false) ||
                !required_string(value, "content_type", publication.content_type, 256, error,
                                 false) ||
                !required_i64(value, "sensitivity", sensitivity, error) ||
                !required_i64(value, "redaction_state", redaction, error) ||
                !required_i64(value, "lifetime_ms", publication.lifetime_ms, error) ||
                sensitivity < 0 || sensitivity > 3 || redaction < 0 || redaction > 1)
                return std::nullopt;
            publication.sensitivity = static_cast<ControlArtifactSensitivity>(sensitivity);
            publication.redaction_state = static_cast<ControlArtifactRedactionState>(redaction);
            if (!valid_host_artifact_publication(publication)) {
                error = {ControlProtocolError::InvalidValue,
                         "host artifact publication is malformed or oversized"};
                return std::nullopt;
            }
            message.artifact_publications.push_back(std::move(publication));
        }
    }
    if (!valid_host_complete(message)) {
        error = {ControlProtocolError::InvalidValue, "host-complete fields are inconsistent"};
        return std::nullopt;
    }
    return message;
}

} // namespace pulp::inspect::control_protocol_detail
