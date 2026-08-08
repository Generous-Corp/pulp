#include "control_protocol_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <type_traits>

namespace pulp::inspect {
namespace {

using namespace control_protocol_detail;

bool only_fields(ValueView value, std::initializer_list<std::string_view> allowed,
                 ControlProtocolDiagnostics& diagnostics) {
    if (!value.isObject()) {
        diagnostics = {ControlProtocolError::InvalidType, "value must be an object"};
        return false;
    }
    std::set<std::string_view> seen;
    for (std::uint32_t index = 0; index < value.size(); ++index) {
        const auto member = value.getObjectMemberAt(index);
        if (!seen.insert(member.name).second) {
            diagnostics = {ControlProtocolError::InvalidValue,
                           "duplicate field '" + std::string(member.name) + "'"};
            return false;
        }
        if (std::ranges::find(allowed, member.name) == allowed.end()) {
            diagnostics = {ControlProtocolError::UnknownField,
                           "unknown field '" + std::string(member.name) + "'"};
            return false;
        }
    }
    return true;
}

bool required_string(ValueView value, std::string_view name, std::string& out, std::size_t maximum,
                     ControlProtocolDiagnostics& diagnostics, bool token = true) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isString()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be a string"};
        return false;
    }
    out = std::string(field.getString());
    if ((token && !valid_token(out, maximum)) || (!token && !valid_text(out, maximum))) {
        diagnostics = {ControlProtocolError::InvalidValue,
                       "field '" + std::string(name) + "' is invalid"};
        return false;
    }
    return true;
}

bool required_u32(ValueView value, std::string_view name, std::uint32_t& out,
                  ControlProtocolDiagnostics& diagnostics, bool nonzero = true) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an integer"};
        return false;
    }
    const auto number = field.getInt64();
    if (number < (nonzero ? 1 : 0) || number > std::numeric_limits<std::uint32_t>::max()) {
        diagnostics = {ControlProtocolError::InvalidValue,
                       "field '" + std::string(name) + "' is out of range"};
        return false;
    }
    out = static_cast<std::uint32_t>(number);
    return true;
}

bool required_u64(ValueView value, std::string_view name, std::uint64_t& out,
                  ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt() || field.getInt64() < 0) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be a non-negative integer"};
        return false;
    }
    out = static_cast<std::uint64_t>(field.getInt64());
    return true;
}

bool required_i64(ValueView value, std::string_view name, std::int64_t& out,
                  ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an integer"};
        return false;
    }
    out = field.getInt64();
    return true;
}

bool parse_features(ValueView value, std::string_view name, std::vector<std::string>& out,
                    ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isArray()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an array"};
        return false;
    }
    if (field.size() > kMaximumFeatures) {
        diagnostics = {ControlProtocolError::LimitExceeded, "feature limit exceeded"};
        return false;
    }
    for (std::uint32_t index = 0; index < field.size(); ++index) {
        if (!field[index].isString()) {
            diagnostics = {ControlProtocolError::InvalidType, "features must be strings"};
            return false;
        }
        out.emplace_back(field[index].getString());
    }
    if (!valid_features(out)) {
        diagnostics = {ControlProtocolError::InvalidValue, "features are invalid or duplicated"};
        return false;
    }
    return true;
}

choc::value::Value string_array(const std::vector<std::string>& strings) {
    auto result = choc::value::createEmptyArray();
    for (const auto& string : strings)
        result.addArrayElement(choc::value::createString(string));
    return result;
}

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
            } else {
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
