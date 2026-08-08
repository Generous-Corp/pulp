#include "control_protocol_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <type_traits>

namespace pulp::inspect {
namespace {

using namespace control_protocol_detail;

std::optional<ControlNegotiationStatus> negotiation_status_from_id(std::string_view id) {
    for (const auto status :
         {ControlNegotiationStatus::Accepted, ControlNegotiationStatus::InvalidOffer,
          ControlNegotiationStatus::NoCommonVersion, ControlNegotiationStatus::DowngradeRejected,
          ControlNegotiationStatus::UnsupportedMandatoryFeature})
        if (control_negotiation_status_id(status) == id)
            return status;
    return std::nullopt;
}

template <typename Enum, std::size_t Size>
std::optional<Enum> enum_from_id(std::string_view id, const std::array<Enum, Size>& values,
                                 std::string_view (*to_id)(Enum)) {
    for (const auto value : values)
        if (to_id(value) == id)
            return value;
    return std::nullopt;
}

bool valid_base64(std::string_view value) {
    if (value.size() > kControlMaximumArtifactChunkBase64Bytes || value.size() % 4 != 0)
        return false;
    const auto padding = value.ends_with("==") ? 2u : value.ends_with('=') ? 1u : 0u;
    const auto content_size = value.size() - padding;
    for (std::size_t index = 0; index < content_size; ++index) {
        const auto c = static_cast<unsigned char>(value[index]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '/'))
            return false;
    }
    return std::ranges::all_of(value.substr(content_size), [](char c) { return c == '='; });
}

bool valid_artifact_wire_metadata(const ControlArtifactWireMetadata& metadata) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::array<std::string_view, 11> lineage{
        metadata.broker_id,
        metadata.receipt_id,
        metadata.producer_client_id,
        metadata.producer_registration_id,
        metadata.session_id,
        metadata.instance_id,
        metadata.publication_id,
        metadata.producer_capability_id,
        metadata.producer_operation_id,
        metadata.original_grant_id,
        metadata.consent_decision_id,
    };
    return valid_token(metadata.artifact_id, kControlReceiptMaximumArtifactIdBytes) &&
           std::ranges::all_of(lineage,
                               [](std::string_view value) {
                                   return valid_token(value,
                                                      kControlMaximumArtifactMetadataFieldBytes);
                               }) &&
           metadata.producer_operation_version != 0 && valid_hash(metadata.manifest_digest) &&
           valid_hash(metadata.producer_artifact_digest) && valid_hash(metadata.sha256) &&
           metadata.byte_size <= signed_max && metadata.created_at_unix_ms != 0 &&
           metadata.created_at_unix_ms <= signed_max && metadata.expires_at_unix_ms <= signed_max &&
           metadata.expires_at_unix_ms > metadata.created_at_unix_ms &&
           valid_token(metadata.content_type, kControlMaximumArtifactContentTypeBytes) &&
           valid_token(metadata.sensitivity_id, kControlMaximumStatusIdBytes) &&
           valid_token(metadata.deletion_state_id, kControlMaximumStatusIdBytes) &&
           valid_token(metadata.redaction_state_id, kControlMaximumStatusIdBytes);
}

bool valid_session_open(const ControlSessionOpenEnvelope& message) {
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.admission_id, kMaximumIdBytes);
}

bool valid_session_open_result(const ControlSessionOpenResult& message) {
    if (!valid_token(message.request_id, kMaximumIdBytes) ||
        !valid_text(message.explanation, kMaximumExplanationBytes))
        return false;
    if (message.accepted)
        return valid_token(message.client_id, kMaximumIdBytes) && message.error_code.empty() &&
               message.explanation.empty();
    return message.client_id.empty() &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty();
}

bool valid_artifact_read(const ControlArtifactReadEnvelope& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.artifact_id, kControlReceiptMaximumArtifactIdBytes) &&
           message.offset <= signed_max && message.maximum_bytes > 0 &&
           message.maximum_bytes <= kControlMaximumArtifactReadBytes;
}

bool valid_artifact_read_response(const ControlArtifactReadResponseEnvelope& message) {
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.status_id, kControlMaximumStatusIdBytes) &&
           (!message.metadata || valid_artifact_wire_metadata(*message.metadata)) &&
           valid_base64(message.bytes_base64) &&
           (message.bytes_base64.empty() || message.metadata.has_value()) &&
           valid_text(message.explanation, kMaximumExplanationBytes);
}

bool valid_health(const ControlHealthEnvelope& message) {
    return valid_token(message.request_id, kMaximumIdBytes);
}

bool valid_health_result(const ControlHealthResult& message) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.sdk_version, kControlMaximumSdkVersionBytes) &&
           message.protocol_versions.minimum != 0 &&
           message.protocol_versions.minimum <= message.protocol_versions.maximum &&
           valid_token(message.broker_id, kMaximumIdBytes) && message.process_generation != 0 &&
           message.process_generation <= signed_max;
}

bool valid_error(const ControlErrorEnvelope& message) {
    return valid_token(message.request_id, kMaximumIdBytes) &&
           valid_token(message.error_code, kControlMaximumErrorCodeBytes) &&
           !message.explanation.empty() &&
           valid_text(message.explanation, kMaximumExplanationBytes);
}

choc::value::Value encode_artifact_wire_metadata(const ControlArtifactWireMetadata& metadata) {
    auto value = choc::value::createObject("");
    value.addMember("artifact_id", choc::value::createString(metadata.artifact_id));
    value.addMember("broker_id", choc::value::createString(metadata.broker_id));
    value.addMember("byte_size",
                    choc::value::createInt64(static_cast<std::int64_t>(metadata.byte_size)));
    value.addMember("consent_decision_id", choc::value::createString(metadata.consent_decision_id));
    value.addMember("content_type", choc::value::createString(metadata.content_type));
    value.addMember("created_at_unix_ms", choc::value::createInt64(static_cast<std::int64_t>(
                                              metadata.created_at_unix_ms)));
    value.addMember("deletion_state_id", choc::value::createString(metadata.deletion_state_id));
    value.addMember("expires_at_unix_ms", choc::value::createInt64(static_cast<std::int64_t>(
                                              metadata.expires_at_unix_ms)));
    value.addMember("instance_id", choc::value::createString(metadata.instance_id));
    value.addMember("manifest_digest", choc::value::createString(metadata.manifest_digest));
    value.addMember("original_grant_id", choc::value::createString(metadata.original_grant_id));
    value.addMember("producer_artifact_digest",
                    choc::value::createString(metadata.producer_artifact_digest));
    value.addMember("producer_capability_id",
                    choc::value::createString(metadata.producer_capability_id));
    value.addMember("producer_client_id", choc::value::createString(metadata.producer_client_id));
    value.addMember("producer_operation_id",
                    choc::value::createString(metadata.producer_operation_id));
    value.addMember("producer_operation_version",
                    choc::value::createInt64(metadata.producer_operation_version));
    value.addMember("producer_registration_id",
                    choc::value::createString(metadata.producer_registration_id));
    value.addMember("publication_id", choc::value::createString(metadata.publication_id));
    value.addMember("receipt_id", choc::value::createString(metadata.receipt_id));
    value.addMember("redaction_state_id", choc::value::createString(metadata.redaction_state_id));
    value.addMember("sensitivity_id", choc::value::createString(metadata.sensitivity_id));
    value.addMember("session_id", choc::value::createString(metadata.session_id));
    value.addMember("sha256", choc::value::createString(metadata.sha256));
    return value;
}

bool decode_artifact_wire_metadata(ValueView value, ControlArtifactWireMetadata& metadata,
                                   ControlProtocolDiagnostics& diagnostics) {
    if (!only_fields(value,
                     {"artifact_id",
                      "broker_id",
                      "byte_size",
                      "consent_decision_id",
                      "content_type",
                      "created_at_unix_ms",
                      "deletion_state_id",
                      "expires_at_unix_ms",
                      "instance_id",
                      "manifest_digest",
                      "original_grant_id",
                      "producer_artifact_digest",
                      "producer_capability_id",
                      "producer_client_id",
                      "producer_operation_id",
                      "producer_operation_version",
                      "producer_registration_id",
                      "publication_id",
                      "receipt_id",
                      "redaction_state_id",
                      "sensitivity_id",
                      "session_id",
                      "sha256"},
                     diagnostics))
        return false;
    if (!required_string(value, "artifact_id", metadata.artifact_id,
                         kControlReceiptMaximumArtifactIdBytes, diagnostics) ||
        !required_string(value, "broker_id", metadata.broker_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_u64(value, "byte_size", metadata.byte_size, diagnostics) ||
        !required_string(value, "consent_decision_id", metadata.consent_decision_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "content_type", metadata.content_type,
                         kControlMaximumArtifactContentTypeBytes, diagnostics) ||
        !required_u64(value, "created_at_unix_ms", metadata.created_at_unix_ms, diagnostics) ||
        !required_string(value, "deletion_state_id", metadata.deletion_state_id,
                         kControlMaximumStatusIdBytes, diagnostics) ||
        !required_u64(value, "expires_at_unix_ms", metadata.expires_at_unix_ms, diagnostics) ||
        !required_string(value, "instance_id", metadata.instance_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "manifest_digest", metadata.manifest_digest, 64, diagnostics) ||
        !required_string(value, "original_grant_id", metadata.original_grant_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "producer_artifact_digest", metadata.producer_artifact_digest, 64,
                         diagnostics) ||
        !required_string(value, "producer_capability_id", metadata.producer_capability_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "producer_client_id", metadata.producer_client_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "producer_operation_id", metadata.producer_operation_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_u32(value, "producer_operation_version", metadata.producer_operation_version,
                      diagnostics) ||
        !required_string(value, "producer_registration_id", metadata.producer_registration_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "publication_id", metadata.publication_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "receipt_id", metadata.receipt_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "redaction_state_id", metadata.redaction_state_id,
                         kControlMaximumStatusIdBytes, diagnostics) ||
        !required_string(value, "sensitivity_id", metadata.sensitivity_id,
                         kControlMaximumStatusIdBytes, diagnostics) ||
        !required_string(value, "session_id", metadata.session_id,
                         kControlMaximumArtifactMetadataFieldBytes, diagnostics) ||
        !required_string(value, "sha256", metadata.sha256, 64, diagnostics))
        return false;
    if (!valid_artifact_wire_metadata(metadata)) {
        diagnostics = {ControlProtocolError::InvalidValue,
                       "artifact wire metadata fields are invalid"};
        return false;
    }
    return true;
}

} // namespace

std::string encode_control_envelope(const ControlEnvelope& envelope) {
    if (envelope.schema_version != kControlProtocolVersion)
        return {};
    auto root = choc::value::createObject("");
    root.addMember("kind", choc::value::createString(""));
    root.addMember("payload", choc::value::createObject(""));
    root.addMember("schema", choc::value::createString(kControlEnvelopeSchema));
    root.addMember("schema_version", choc::value::createInt64(envelope.schema_version));
    auto payload = choc::value::createObject("");
    std::string_view kind;
    bool valid = true;
    std::visit(
        [&](const auto& message) {
            using T = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<T, ControlNegotiationOffer>) {
                valid = valid_offer(message);
                kind = "negotiate";
                if (!valid)
                    return;
                payload.addMember("mandatory_features", string_array(message.mandatory_features));
                payload.addMember("max_version",
                                  choc::value::createInt64(message.versions.maximum));
                payload.addMember("min_version",
                                  choc::value::createInt64(message.versions.minimum));
                payload.addMember("optional_features", string_array(message.optional_features));
            } else if constexpr (std::is_same_v<T, ControlNegotiationResult>) {
                valid = valid_features(message.features) &&
                        valid_text(message.explanation, kMaximumExplanationBytes) &&
                        ((message.status == ControlNegotiationStatus::Accepted) ==
                         (message.selected_version > 0));
                kind = "negotiated";
                if (!valid)
                    return;
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("features", string_array(message.features));
                payload.addMember("selected_version",
                                  choc::value::createInt64(message.selected_version));
                payload.addMember("status", choc::value::createString(
                                                control_negotiation_status_id(message.status)));
            } else if constexpr (std::is_same_v<T, ControlRequestEnvelope>) {
                valid = valid_request(message, true) &&
                        control_request_hash(message) == message.request_hash;
                kind = "request";
                if (!valid)
                    return;
                payload.addMember("client_id", choc::value::createString(message.client_id));
                payload.addMember("deadline_unix_ms",
                                  choc::value::createInt64(message.deadline_unix_ms));
                payload.addMember("expected_state_generation",
                                  choc::value::createInt64(static_cast<std::int64_t>(
                                      message.expected_state_generation)));
                payload.addMember("grant_id", choc::value::createString(message.grant_id));
                payload.addMember("idempotency_key",
                                  choc::value::createString(message.idempotency_key));
                payload.addMember("instance_generation",
                                  choc::value::createString(message.instance_generation));
                payload.addMember("operation_id", choc::value::createString(message.operation_id));
                payload.addMember("operation_version",
                                  choc::value::createInt64(message.operation_version));
                if (valid)
                    payload.addMember("params",
                                      canonical_value(choc::json::parse(message.params_json)));
                payload.addMember("registration_id",
                                  choc::value::createString(message.registration_id));
                payload.addMember("request_hash", choc::value::createString(message.request_hash));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlCancelEnvelope>) {
                valid = valid_token(message.request_id, kMaximumIdBytes) &&
                        !message.reason.empty() &&
                        valid_text(message.reason, kMaximumExplanationBytes);
                kind = "cancel";
                if (!valid)
                    return;
                payload.addMember("reason", choc::value::createString(message.reason));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlProgressEnvelope>) {
                valid = valid_progress(message);
                kind = "progress";
                if (!valid)
                    return;
                payload.addMember("current", choc::value::createInt64(
                                                 static_cast<std::int64_t>(message.current)));
                if (valid)
                    payload.addMember("detail",
                                      canonical_value(choc::json::parse(message.detail_json)));
                payload.addMember("receipt_id", choc::value::createString(message.receipt_id));
                payload.addMember("request_id", choc::value::createString(message.request_id));
                payload.addMember("sequence", choc::value::createInt64(
                                                  static_cast<std::int64_t>(message.sequence)));
                payload.addMember(
                    "total", choc::value::createInt64(static_cast<std::int64_t>(message.total)));
            } else if constexpr (std::is_same_v<T, ControlReceiptEnvelope>) {
                valid = valid_receipt(message);
                kind = "receipt";
                if (!valid)
                    return;
                auto artifacts = choc::value::createEmptyArray();
                for (const auto& artifact : message.artifacts) {
                    auto item = choc::value::createObject("");
                    item.addMember("artifact_id", choc::value::createString(artifact.artifact_id));
                    item.addMember("byte_size", choc::value::createInt64(
                                                    static_cast<std::int64_t>(artifact.byte_size)));
                    item.addMember("media_type", choc::value::createString(artifact.media_type));
                    artifacts.addArrayElement(item);
                }
                payload.addMember("artifacts", artifacts);
                if (valid)
                    payload.addMember("detail",
                                      canonical_value(choc::json::parse(message.detail_json)));
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("operation_id", choc::value::createString(message.operation_id));
                payload.addMember("operation_version",
                                  choc::value::createInt64(message.operation_version));
                payload.addMember("receipt_id", choc::value::createString(message.receipt_id));
                payload.addMember("request_id", choc::value::createString(message.request_id));
                payload.addMember(
                    "result_code",
                    message.result_code
                        ? choc::value::createString(control_result_code_id(*message.result_code))
                        : choc::value::Value{});
                payload.addMember("retry", choc::value::createString(
                                               control_retry_classification_id(message.retry)));
                payload.addMember(
                    "state", choc::value::createString(control_receipt_state_id(message.state)));
            } else if constexpr (std::is_same_v<T, ControlSessionOpenEnvelope>) {
                valid = valid_session_open(message);
                kind = "session-open";
                if (!valid)
                    return;
                payload.addMember("admission_id", choc::value::createString(message.admission_id));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlSessionOpenResult>) {
                valid = valid_session_open_result(message);
                kind = "session-opened";
                if (!valid)
                    return;
                payload.addMember("accepted", choc::value::createBool(message.accepted));
                payload.addMember("client_id", choc::value::createString(message.client_id));
                payload.addMember("error_code", choc::value::createString(message.error_code));
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlHostOpenEnvelope>) {
                valid = valid_host_open(message);
                kind = "host-open";
                payload.addMember("admission_id", choc::value::createString(message.admission_id));
                payload.addMember("enrollment_id",
                                  choc::value::createString(message.enrollment_id));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlHostOpenResult>) {
                valid = valid_host_open_result(message);
                kind = "host-opened";
                payload.addMember("accepted", choc::value::createBool(message.accepted));
                payload.addMember("error_code", choc::value::createString(message.error_code));
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("registration_id",
                                  choc::value::createString(message.registration_id));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlHostExecuteEnvelope>) {
                valid = valid_host_execute(message);
                kind = "host-execute";
                payload.addMember("deadline_unix_ms",
                                  choc::value::createInt64(message.deadline_unix_ms));
                payload.addMember("expected_state_generation",
                                  choc::value::createInt64(static_cast<std::int64_t>(
                                      message.expected_state_generation)));
                payload.addMember("operation_id", choc::value::createString(message.operation_id));
                payload.addMember("operation_version",
                                  choc::value::createInt64(message.operation_version));
                if (const auto params =
                        parse_bounded_control_json(message.params_json, kMaximumPayloadBytes))
                    payload.addMember("params", *params);
                else
                    valid = false;
                payload.addMember("receipt_id", choc::value::createString(message.receipt_id));
                payload.addMember("route_id", choc::value::createString(message.route_id));
            } else if constexpr (std::is_same_v<T, ControlHostProgressEnvelope>) {
                valid = valid_host_progress(message);
                kind = "host-progress";
                payload.addMember("current", choc::value::createInt64(
                                                 static_cast<std::int64_t>(message.current)));
                if (const auto detail = parse_bounded_control_json(
                        message.detail_json, kMaximumProgressBytes, kMaximumProgressJsonNodes))
                    payload.addMember("detail", *detail);
                else
                    valid = false;
                payload.addMember("route_id", choc::value::createString(message.route_id));
                payload.addMember(
                    "total", choc::value::createInt64(static_cast<std::int64_t>(message.total)));
            } else if constexpr (std::is_same_v<T, ControlHostCancelEnvelope>) {
                valid = valid_host_cancel(message);
                kind = "host-cancel";
                payload.addMember("reason", choc::value::createString(message.reason));
                payload.addMember("route_id", choc::value::createString(message.route_id));
            } else if constexpr (std::is_same_v<T, ControlHostCompleteEnvelope>) {
                valid = valid_host_complete(message);
                kind = "host-complete";
                payload.addMember("cancellation_reason",
                                  choc::value::createString(message.cancellation_reason));
                if (const auto detail = parse_bounded_control_json(message.detail_json,
                                                                   kControlMaximumResultDetailBytes,
                                                                   kMaximumResultJsonNodes))
                    payload.addMember("detail", *detail);
                else
                    valid = false;
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember(
                    "result_code",
                    message.result_code
                        ? choc::value::createString(control_result_code_id(*message.result_code))
                        : choc::value::Value{});
                payload.addMember("retry", choc::value::createString(
                                               control_retry_classification_id(message.retry)));
                payload.addMember("route_id", choc::value::createString(message.route_id));
                payload.addMember("state", choc::value::createString(
                                               control_receipt_state_id(message.terminal_state)));
            } else if constexpr (std::is_same_v<T, ControlArtifactReadEnvelope>) {
                valid = valid_artifact_read(message);
                kind = "artifact-read";
                if (!valid)
                    return;
                payload.addMember("artifact_id", choc::value::createString(message.artifact_id));
                payload.addMember(
                    "maximum_bytes",
                    choc::value::createInt64(static_cast<std::int64_t>(message.maximum_bytes)));
                payload.addMember(
                    "offset", choc::value::createInt64(static_cast<std::int64_t>(message.offset)));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlArtifactReadResponseEnvelope>) {
                valid = valid_artifact_read_response(message);
                kind = "artifact-read-response";
                if (!valid)
                    return;
                payload.addMember("bytes_base64", choc::value::createString(message.bytes_base64));
                payload.addMember("eof", choc::value::createBool(message.eof));
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("metadata", message.metadata
                                                  ? encode_artifact_wire_metadata(*message.metadata)
                                                  : choc::value::Value{});
                payload.addMember("request_id", choc::value::createString(message.request_id));
                payload.addMember("status_id", choc::value::createString(message.status_id));
            } else if constexpr (std::is_same_v<T, ControlHealthEnvelope>) {
                valid = valid_health(message);
                kind = "health";
                if (!valid)
                    return;
                payload.addMember("request_id", choc::value::createString(message.request_id));
            } else if constexpr (std::is_same_v<T, ControlHealthResult>) {
                valid = valid_health_result(message);
                kind = "health-result";
                if (!valid)
                    return;
                payload.addMember("broker_id", choc::value::createString(message.broker_id));
                payload.addMember("max_version",
                                  choc::value::createInt64(message.protocol_versions.maximum));
                payload.addMember("min_version",
                                  choc::value::createInt64(message.protocol_versions.minimum));
                payload.addMember("process_generation",
                                  choc::value::createInt64(
                                      static_cast<std::int64_t>(message.process_generation)));
                payload.addMember("request_id", choc::value::createString(message.request_id));
                payload.addMember("sdk_version", choc::value::createString(message.sdk_version));
            } else if constexpr (std::is_same_v<T, ControlErrorEnvelope>) {
                valid = valid_error(message);
                kind = "error";
                if (!valid)
                    return;
                payload.addMember("error_code", choc::value::createString(message.error_code));
                payload.addMember("explanation", choc::value::createString(message.explanation));
                payload.addMember("request_id", choc::value::createString(message.request_id));
            }
        },
        envelope.payload);
    if (!valid)
        return {};
    root.setMember("kind", choc::value::createString(kind));
    root.setMember("payload", payload);
    const auto encoded = choc::json::toString(root, false);
    return encoded.size() <= kControlMaximumEnvelopeBytes ? encoded : std::string{};
}

std::optional<ControlEnvelope> decode_control_envelope(std::string_view json,
                                                       ControlProtocolDiagnostics* diagnostics) {
    ControlProtocolDiagnostics local;
    auto& error = diagnostics ? *diagnostics : local;
    error = {};
    if (json.size() > kControlMaximumEnvelopeBytes) {
        error = {ControlProtocolError::EnvelopeTooLarge, "control envelope exceeds byte limit"};
        return std::nullopt;
    }
    if (json.empty()) {
        error = {ControlProtocolError::Parse, "control envelope is empty"};
        return std::nullopt;
    }
    if (!valid_control_json_bytes(json, kControlMaximumEnvelopeBytes,
                                  kMaximumResultJsonNodes + 256)) {
        error = {ControlProtocolError::Parse,
                 "control envelope has invalid UTF-8 or string tokens"};
        return std::nullopt;
    }
    try {
        // See canonicalize_control_json: ensure CHOC's sentinel read remains
        // inside owned storage for a non-NUL-terminated string_view.
        const std::string terminated(json);
        const auto root = choc::json::parse(terminated);
        if (!root.isObject()) {
            error = {ControlProtocolError::RootType, "control envelope root must be an object"};
            return std::nullopt;
        }
        if (!only_fields(root, {"kind", "payload", "schema", "schema_version"}, error))
            return std::nullopt;
        std::string schema, kind;
        std::uint32_t schema_version = 0;
        if (!required_string(root, "schema", schema, 64, error) ||
            !required_u32(root, "schema_version", schema_version, error) ||
            !required_string(root, "kind", kind, 32, error))
            return std::nullopt;
        if (schema != kControlEnvelopeSchema) {
            error = {ControlProtocolError::UnsupportedSchema,
                     "unsupported control envelope schema"};
            return std::nullopt;
        }
        if (schema_version != kControlProtocolVersion) {
            error = {ControlProtocolError::UnsupportedVersion,
                     "unsupported control envelope version"};
            return std::nullopt;
        }
        if (!root.hasObjectMember("payload")) {
            error = {ControlProtocolError::MissingField, "missing field 'payload'"};
            return std::nullopt;
        }
        const auto payload = root["payload"];
        ControlEnvelope envelope;
        if (kind == "negotiate") {
            if (!only_fields(
                    payload,
                    {"mandatory_features", "max_version", "min_version", "optional_features"},
                    error))
                return std::nullopt;
            ControlNegotiationOffer offer;
            if (!required_u32(payload, "min_version", offer.versions.minimum, error) ||
                !required_u32(payload, "max_version", offer.versions.maximum, error) ||
                !parse_features(payload, "mandatory_features", offer.mandatory_features, error) ||
                !parse_features(payload, "optional_features", offer.optional_features, error))
                return std::nullopt;
            if (offer.versions.minimum > offer.versions.maximum) {
                error = {ControlProtocolError::InvalidValue, "protocol version range is reversed"};
                return std::nullopt;
            }
            if (!valid_offer(offer)) {
                error = {ControlProtocolError::InvalidValue, "protocol offer is inconsistent"};
                return std::nullopt;
            }
            envelope.payload = std::move(offer);
        } else if (kind == "negotiated") {
            if (!only_fields(payload, {"explanation", "features", "selected_version", "status"},
                             error))
                return std::nullopt;
            ControlNegotiationResult result;
            std::string status;
            if (!required_string(payload, "status", status, 64, error) ||
                !required_u32(payload, "selected_version", result.selected_version, error, false) ||
                !parse_features(payload, "features", result.features, error) ||
                !required_string(payload, "explanation", result.explanation,
                                 kMaximumExplanationBytes, error, false))
                return std::nullopt;
            const auto parsed = negotiation_status_from_id(status);
            if (!parsed || ((*parsed == ControlNegotiationStatus::Accepted) !=
                            (result.selected_version > 0))) {
                error = {ControlProtocolError::InvalidValue, "negotiation status is invalid"};
                return std::nullopt;
            }
            result.status = *parsed;
            envelope.payload = std::move(result);
        } else if (kind == "request") {
            if (!only_fields(payload,
                             {"client_id", "deadline_unix_ms", "expected_state_generation",
                              "grant_id", "idempotency_key", "instance_generation", "operation_id",
                              "operation_version", "params", "registration_id", "request_hash",
                              "request_id"},
                             error))
                return std::nullopt;
            ControlRequestEnvelope request;
            if (!required_string(payload, "request_id", request.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "client_id", request.client_id, kMaximumIdBytes, error) ||
                !required_string(payload, "registration_id", request.registration_id,
                                 kMaximumIdBytes, error) ||
                !required_string(payload, "grant_id", request.grant_id, kMaximumIdBytes, error) ||
                !required_string(payload, "instance_generation", request.instance_generation,
                                 kMaximumIdBytes, error) ||
                !required_string(payload, "operation_id", request.operation_id,
                                 kMaximumOperationIdBytes, error) ||
                !required_u32(payload, "operation_version", request.operation_version, error) ||
                !required_string(payload, "idempotency_key", request.idempotency_key,
                                 kMaximumIdBytes, error) ||
                !required_string(payload, "request_hash", request.request_hash, 64, error) ||
                !required_i64(payload, "deadline_unix_ms", request.deadline_unix_ms, error) ||
                !required_u64(payload, "expected_state_generation",
                              request.expected_state_generation, error))
                return std::nullopt;
            if (!payload.hasObjectMember("params")) {
                error = {ControlProtocolError::MissingField, "missing field 'params'"};
                return std::nullopt;
            }
            request.params_json = choc::json::toString(canonical_value(payload["params"]), false);
            if (!valid_request(request, true)) {
                error = {ControlProtocolError::InvalidValue, "request fields are invalid"};
                return std::nullopt;
            }
            if (control_request_hash(request) != request.request_hash) {
                error = {ControlProtocolError::HashMismatch,
                         "request hash does not match canonical request"};
                return std::nullopt;
            }
            envelope.payload = std::move(request);
        } else if (kind == "cancel") {
            if (!only_fields(payload, {"reason", "request_id"}, error))
                return std::nullopt;
            ControlCancelEnvelope cancel;
            if (!required_string(payload, "request_id", cancel.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "reason", cancel.reason, kMaximumExplanationBytes, error,
                                 false))
                return std::nullopt;
            if (cancel.reason.empty()) {
                error = {ControlProtocolError::InvalidValue, "field 'reason' must not be empty"};
                return std::nullopt;
            }
            envelope.payload = std::move(cancel);
        } else if (kind == "progress") {
            if (!only_fields(payload,
                             {"current", "detail", "receipt_id", "request_id", "sequence", "total"},
                             error))
                return std::nullopt;
            ControlProgressEnvelope progress;
            if (!required_string(payload, "request_id", progress.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "receipt_id", progress.receipt_id, kMaximumIdBytes,
                                 error) ||
                !required_u64(payload, "sequence", progress.sequence, error) ||
                !required_u64(payload, "current", progress.current, error) ||
                !required_u64(payload, "total", progress.total, error))
                return std::nullopt;
            if (!payload.hasObjectMember("detail")) {
                error = {ControlProtocolError::MissingField, "missing field 'detail'"};
                return std::nullopt;
            }
            progress.detail_json = choc::json::toString(canonical_value(payload["detail"]), false);
            if (!valid_progress(progress)) {
                error = {ControlProtocolError::InvalidValue, "progress fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(progress);
        } else if (kind == "receipt") {
            if (!only_fields(payload,
                             {"artifacts", "detail", "explanation", "operation_id",
                              "operation_version", "receipt_id", "request_id", "result_code",
                              "retry", "state"},
                             error))
                return std::nullopt;
            ControlReceiptEnvelope receipt;
            std::string state, retry;
            if (!required_string(payload, "request_id", receipt.request_id,
                                 kControlReceiptMaximumRequestIdBytes, error) ||
                !required_string(payload, "receipt_id", receipt.receipt_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "operation_id", receipt.operation_id,
                                 kControlReceiptMaximumOperationIdBytes, error) ||
                !required_u32(payload, "operation_version", receipt.operation_version, error) ||
                !required_string(payload, "state", state, 64, error) ||
                !required_string(payload, "retry", retry, 64, error) ||
                !required_string(payload, "explanation", receipt.explanation,
                                 kControlReceiptMaximumExplanationBytes, error, false))
                return std::nullopt;
            constexpr std::array states{ControlReceiptState::Admitted,
                                        ControlReceiptState::Running,
                                        ControlReceiptState::Completed,
                                        ControlReceiptState::Failed,
                                        ControlReceiptState::Cancelled,
                                        ControlReceiptState::CompletedAfterRevocation,
                                        ControlReceiptState::UnknownNeedsRefresh};
            constexpr std::array retries{
                ControlRetryClassification::Never, ControlRetryClassification::AfterRefresh,
                ControlRetryClassification::AfterGrant, ControlRetryClassification::AfterBackoff};
            const auto parsed_state = enum_from_id(state, states, control_receipt_state_id);
            const auto parsed_retry = enum_from_id(retry, retries, control_retry_classification_id);
            if (!parsed_state || !parsed_retry) {
                error = {ControlProtocolError::InvalidValue,
                         "receipt state or retry classification is invalid"};
                return std::nullopt;
            }
            receipt.state = *parsed_state;
            receipt.retry = *parsed_retry;
            if (!payload.hasObjectMember("result_code") || !payload.hasObjectMember("detail") ||
                !payload.hasObjectMember("artifacts")) {
                error = {ControlProtocolError::MissingField, "receipt fields are missing"};
                return std::nullopt;
            }
            const auto result_code = payload["result_code"];
            if (!result_code.isVoid()) {
                if (!result_code.isString()) {
                    error = {ControlProtocolError::InvalidType,
                             "result_code must be a string or null"};
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
                receipt.result_code = enum_from_id<ControlResultCode>(
                    result_code.getString(), codes, control_result_code_id);
                if (!receipt.result_code) {
                    error = {ControlProtocolError::InvalidValue, "result_code is unknown"};
                    return std::nullopt;
                }
            }
            receipt.detail_json = choc::json::toString(canonical_value(payload["detail"]), false);
            const auto artifacts = payload["artifacts"];
            if (!artifacts.isArray() || artifacts.size() > kControlReceiptMaximumArtifacts) {
                error = {ControlProtocolError::LimitExceeded, "artifacts must be a bounded array"};
                return std::nullopt;
            }
            for (std::uint32_t index = 0; index < artifacts.size(); ++index) {
                const auto item = artifacts[index];
                if (!only_fields(item, {"artifact_id", "byte_size", "media_type"}, error))
                    return std::nullopt;
                ControlArtifactHandle artifact;
                if (!required_string(item, "artifact_id", artifact.artifact_id,
                                     kControlReceiptMaximumArtifactIdBytes, error) ||
                    !required_string(item, "media_type", artifact.media_type,
                                     kControlReceiptMaximumArtifactMediaTypeBytes, error) ||
                    !required_u64(item, "byte_size", artifact.byte_size, error))
                    return std::nullopt;
                receipt.artifacts.push_back(std::move(artifact));
            }
            if (!valid_receipt(receipt)) {
                error = {ControlProtocolError::InvalidValue, "receipt fields are inconsistent"};
                return std::nullopt;
            }
            envelope.payload = std::move(receipt);
        } else if (kind == "session-open") {
            if (!only_fields(payload, {"admission_id", "request_id"}, error))
                return std::nullopt;
            ControlSessionOpenEnvelope message;
            if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "admission_id", message.admission_id, kMaximumIdBytes,
                                 error))
                return std::nullopt;
            if (!valid_session_open(message)) {
                error = {ControlProtocolError::InvalidValue, "session-open fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(message);
        } else if (kind == "session-opened") {
            if (!only_fields(payload,
                             {"accepted", "client_id", "error_code", "explanation", "request_id"},
                             error))
                return std::nullopt;
            ControlSessionOpenResult result;
            if (!required_string(payload, "request_id", result.request_id, kMaximumIdBytes,
                                 error) ||
                !required_bool(payload, "accepted", result.accepted, error) ||
                !required_string(payload, "client_id", result.client_id, kMaximumIdBytes, error,
                                 false) ||
                !required_string(payload, "error_code", result.error_code,
                                 kControlMaximumErrorCodeBytes, error, false) ||
                !required_string(payload, "explanation", result.explanation,
                                 kMaximumExplanationBytes, error, false))
                return std::nullopt;
            if (!valid_session_open_result(result)) {
                error = {ControlProtocolError::InvalidValue,
                         "session-open result fields are inconsistent"};
                return std::nullopt;
            }
            envelope.payload = std::move(result);
        } else if (is_host_control_kind(kind)) {
            auto host = decode_host_control_payload(kind, payload, error);
            if (!host)
                return std::nullopt;
            envelope.payload = std::move(*host);
        } else if (kind == "artifact-read") {
            if (!only_fields(payload, {"artifact_id", "maximum_bytes", "offset", "request_id"},
                             error))
                return std::nullopt;
            ControlArtifactReadEnvelope message;
            std::uint64_t maximum_bytes = 0;
            if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "artifact_id", message.artifact_id,
                                 kControlReceiptMaximumArtifactIdBytes, error) ||
                !required_u64(payload, "offset", message.offset, error) ||
                !required_u64(payload, "maximum_bytes", maximum_bytes, error))
                return std::nullopt;
            if (maximum_bytes > std::numeric_limits<std::size_t>::max()) {
                error = {ControlProtocolError::LimitExceeded,
                         "field 'maximum_bytes' exceeds the platform limit"};
                return std::nullopt;
            }
            message.maximum_bytes = static_cast<std::size_t>(maximum_bytes);
            if (!valid_artifact_read(message)) {
                error = {ControlProtocolError::InvalidValue, "artifact-read fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(message);
        } else if (kind == "artifact-read-response") {
            if (!only_fields(
                    payload,
                    {"bytes_base64", "eof", "explanation", "metadata", "request_id", "status_id"},
                    error))
                return std::nullopt;
            ControlArtifactReadResponseEnvelope response;
            if (!required_string(payload, "request_id", response.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "status_id", response.status_id,
                                 kControlMaximumStatusIdBytes, error) ||
                !required_string(payload, "bytes_base64", response.bytes_base64,
                                 kControlMaximumArtifactChunkBase64Bytes, error, false) ||
                !required_bool(payload, "eof", response.eof, error) ||
                !required_string(payload, "explanation", response.explanation,
                                 kMaximumExplanationBytes, error, false))
                return std::nullopt;
            if (!payload.hasObjectMember("metadata")) {
                error = {ControlProtocolError::MissingField, "missing field 'metadata'"};
                return std::nullopt;
            }
            const auto metadata = payload["metadata"];
            if (!metadata.isVoid()) {
                ControlArtifactWireMetadata decoded_metadata;
                if (!decode_artifact_wire_metadata(metadata, decoded_metadata, error))
                    return std::nullopt;
                response.metadata = std::move(decoded_metadata);
            }
            if (!valid_artifact_read_response(response)) {
                error = {ControlProtocolError::InvalidValue,
                         "artifact-read response fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(response);
        } else if (kind == "health") {
            if (!only_fields(payload, {"request_id"}, error))
                return std::nullopt;
            ControlHealthEnvelope message;
            if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes, error))
                return std::nullopt;
            envelope.payload = std::move(message);
        } else if (kind == "health-result") {
            if (!only_fields(payload,
                             {"broker_id", "max_version", "min_version", "process_generation",
                              "request_id", "sdk_version"},
                             error))
                return std::nullopt;
            ControlHealthResult result;
            if (!required_string(payload, "request_id", result.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "sdk_version", result.sdk_version,
                                 kControlMaximumSdkVersionBytes, error) ||
                !required_u32(payload, "min_version", result.protocol_versions.minimum, error) ||
                !required_u32(payload, "max_version", result.protocol_versions.maximum, error) ||
                !required_string(payload, "broker_id", result.broker_id, kMaximumIdBytes, error) ||
                !required_u64(payload, "process_generation", result.process_generation, error))
                return std::nullopt;
            if (!valid_health_result(result)) {
                error = {ControlProtocolError::InvalidValue, "health result fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(result);
        } else if (kind == "error") {
            if (!only_fields(payload, {"error_code", "explanation", "request_id"}, error))
                return std::nullopt;
            ControlErrorEnvelope message;
            if (!required_string(payload, "request_id", message.request_id, kMaximumIdBytes,
                                 error) ||
                !required_string(payload, "error_code", message.error_code,
                                 kControlMaximumErrorCodeBytes, error) ||
                !required_string(payload, "explanation", message.explanation,
                                 kMaximumExplanationBytes, error, false))
                return std::nullopt;
            if (!valid_error(message)) {
                error = {ControlProtocolError::InvalidValue, "error fields are invalid"};
                return std::nullopt;
            }
            envelope.payload = std::move(message);
        } else {
            error = {ControlProtocolError::InvalidValue, "unknown control envelope kind"};
            return std::nullopt;
        }
        envelope.schema_version = schema_version;
        return envelope;
    } catch (...) {
        error = {ControlProtocolError::Parse, "control envelope is not valid JSON"};
        return std::nullopt;
    }
}

} // namespace pulp::inspect
