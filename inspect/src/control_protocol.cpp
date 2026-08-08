#include "control_protocol_internal.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <type_traits>

namespace pulp::inspect {
namespace control_protocol_detail {

bool valid_token(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum)
        return false;
    return std::ranges::all_of(value, [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}

bool valid_text(std::string_view value, std::size_t maximum) {
    return value.size() <= maximum && value.find('\0') == std::string_view::npos &&
           valid_utf8(value);
}

bool valid_hash(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool valid_features(const std::vector<std::string>& features) {
    if (features.size() > kMaximumFeatures)
        return false;
    std::set<std::string> unique;
    for (const auto& feature : features) {
        if (!valid_token(feature, kMaximumFeatureBytes) || !unique.insert(feature).second)
            return false;
    }
    return true;
}

bool valid_offer(const ControlNegotiationOffer& offer) {
    if (offer.versions.minimum == 0 || offer.versions.minimum > offer.versions.maximum ||
        !valid_features(offer.mandatory_features) || !valid_features(offer.optional_features))
        return false;
    std::set<std::string> mandatory(offer.mandatory_features.begin(),
                                    offer.mandatory_features.end());
    return std::ranges::none_of(offer.optional_features,
                                [&](const auto& feature) { return mandatory.contains(feature); });
}

bool valid_request(const ControlRequestEnvelope& request, bool require_hash) {
    if (!valid_token(request.request_id, kMaximumIdBytes) ||
        !valid_token(request.client_id, kMaximumIdBytes) ||
        !valid_token(request.registration_id, kMaximumIdBytes) ||
        !valid_token(request.grant_id, kMaximumIdBytes) ||
        !valid_token(request.instance_generation, kMaximumIdBytes) ||
        !valid_token(request.operation_id, kMaximumOperationIdBytes) ||
        request.operation_version == 0 || !valid_token(request.idempotency_key, kMaximumIdBytes) ||
        request.expected_state_generation >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        request.deadline_unix_ms <= 0 || (require_hash && !valid_hash(request.request_hash)))
        return false;
    const auto canonical = canonicalize_control_json(request.params_json);
    return canonical && canonical->size() <= kMaximumPayloadBytes;
}

bool valid_progress(const ControlProgressEnvelope& progress) {
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid_token(progress.request_id, kMaximumIdBytes) ||
        !valid_token(progress.receipt_id, kMaximumIdBytes) || progress.sequence == 0 ||
        progress.sequence > signed_max || progress.total == 0 || progress.total > signed_max ||
        progress.current > progress.total)
        return false;
    const auto detail = parse_bounded_control_json(progress.detail_json, kMaximumProgressBytes,
                                                   kMaximumProgressJsonNodes);
    return detail.has_value();
}

bool valid_receipt(const ControlReceiptEnvelope& receipt) {
    if (!valid_token(receipt.request_id, kControlReceiptMaximumRequestIdBytes) ||
        !valid_token(receipt.receipt_id, kMaximumIdBytes) ||
        !valid_token(receipt.operation_id, kControlReceiptMaximumOperationIdBytes) ||
        receipt.operation_version == 0 ||
        !valid_text(receipt.explanation, kControlReceiptMaximumExplanationBytes) ||
        receipt.artifacts.size() > kControlReceiptMaximumArtifacts)
        return false;
    const bool successful = receipt.state == ControlReceiptState::Completed;
    const bool exceptional_terminal =
        control_receipt_state_is_terminal(receipt.state) && !successful;
    if (exceptional_terminal != receipt.result_code.has_value())
        return false;
    if ((receipt.state == ControlReceiptState::Cancelled &&
         receipt.result_code != ControlResultCode::Cancelled) ||
        (receipt.state == ControlReceiptState::CompletedAfterRevocation &&
         receipt.result_code != ControlResultCode::CompletedAfterRevocation) ||
        (receipt.state == ControlReceiptState::UnknownNeedsRefresh &&
         receipt.result_code != ControlResultCode::UnknownNeedsRefresh))
        return false;
    const auto detail = canonicalize_control_result_json(receipt.detail_json);
    if (!detail)
        return false;
    for (const auto& artifact : receipt.artifacts)
        if (!valid_token(artifact.artifact_id, kControlReceiptMaximumArtifactIdBytes) ||
            !valid_token(artifact.media_type, kControlReceiptMaximumArtifactMediaTypeBytes) ||
            artifact.byte_size >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;
    return true;
}

} // namespace control_protocol_detail

using namespace control_protocol_detail;

std::string_view control_negotiation_status_id(ControlNegotiationStatus status) {
    switch (status) {
    case ControlNegotiationStatus::Accepted:
        return "accepted";
    case ControlNegotiationStatus::InvalidOffer:
        return "invalid_offer";
    case ControlNegotiationStatus::NoCommonVersion:
        return "no_common_version";
    case ControlNegotiationStatus::DowngradeRejected:
        return "downgrade_rejected";
    case ControlNegotiationStatus::UnsupportedMandatoryFeature:
        return "unsupported_mandatory_feature";
    }
    return "invalid_offer";
}

std::string_view control_receipt_state_id(ControlReceiptState state) {
    switch (state) {
    case ControlReceiptState::Admitted:
        return "admitted";
    case ControlReceiptState::Running:
        return "running";
    case ControlReceiptState::Completed:
        return "completed";
    case ControlReceiptState::Failed:
        return "failed";
    case ControlReceiptState::Cancelled:
        return "cancelled";
    case ControlReceiptState::CompletedAfterRevocation:
        return "completed_after_revocation";
    case ControlReceiptState::UnknownNeedsRefresh:
        return "unknown_needs_refresh";
    }
    return "unknown_needs_refresh";
}

std::string_view control_result_code_id(ControlResultCode code) {
    switch (code) {
    case ControlResultCode::NotImplemented:
        return "not_implemented";
    case ControlResultCode::NotBuilt:
        return "not_built";
    case ControlResultCode::HostUnavailable:
        return "host_unavailable";
    case ControlResultCode::Inactive:
        return "inactive";
    case ControlResultCode::PolicyDenied:
        return "policy_denied";
    case ControlResultCode::GrantRequired:
        return "grant_required";
    case ControlResultCode::GrantExpired:
        return "grant_expired";
    case ControlResultCode::ConsentRequired:
        return "consent_required";
    case ControlResultCode::LeaseConflict:
        return "lease_conflict";
    case ControlResultCode::StateConflict:
        return "state_conflict";
    case ControlResultCode::SessionStale:
        return "session_stale";
    case ControlResultCode::InvalidRequest:
        return "invalid_request";
    case ControlResultCode::DeadlineExceeded:
        return "deadline_exceeded";
    case ControlResultCode::Cancelled:
        return "cancelled";
    case ControlResultCode::CompletedAfterRevocation:
        return "completed_after_revocation";
    case ControlResultCode::UnknownNeedsRefresh:
        return "unknown_needs_refresh";
    case ControlResultCode::ResourceExhausted:
        return "resource_exhausted";
    case ControlResultCode::InternalError:
        return "internal_error";
    }
    return "internal_error";
}

std::string_view control_retry_classification_id(ControlRetryClassification retry) {
    switch (retry) {
    case ControlRetryClassification::Never:
        return "never";
    case ControlRetryClassification::AfterRefresh:
        return "after_refresh";
    case ControlRetryClassification::AfterGrant:
        return "after_grant";
    case ControlRetryClassification::AfterBackoff:
        return "after_backoff";
    }
    return "never";
}

bool control_receipt_state_is_terminal(ControlReceiptState state) {
    return state == ControlReceiptState::Completed || state == ControlReceiptState::Failed ||
           state == ControlReceiptState::Cancelled ||
           state == ControlReceiptState::CompletedAfterRevocation ||
           state == ControlReceiptState::UnknownNeedsRefresh;
}

bool valid_control_receipt_transition(ControlReceiptState from, ControlReceiptState to) {
    if (control_receipt_state_is_terminal(from) || from == to)
        return false;
    if (from == ControlReceiptState::Admitted)
        return to == ControlReceiptState::Running || control_receipt_state_is_terminal(to);
    return from == ControlReceiptState::Running && control_receipt_state_is_terminal(to);
}

bool valid_control_progress_transition(const ControlProgressEnvelope& previous,
                                       const ControlProgressEnvelope& next) {
    return valid_progress(previous) && valid_progress(next) &&
           previous.request_id == next.request_id && previous.receipt_id == next.receipt_id &&
           next.sequence > previous.sequence && next.current >= previous.current &&
           next.total == previous.total;
}

ControlNegotiationResult negotiate_control_protocol(const ControlNegotiationOffer& local,
                                                    const ControlNegotiationOffer& peer,
                                                    std::uint32_t local_security_floor) {
    if (!valid_offer(local) || !valid_offer(peer))
        return {ControlNegotiationStatus::InvalidOffer, 0, {}, "invalid protocol offer"};
    const auto minimum =
        std::max({local.versions.minimum, peer.versions.minimum, local_security_floor});
    const auto maximum = std::min(local.versions.maximum, peer.versions.maximum);
    if (maximum < minimum) {
        const bool below_floor = maximum < local_security_floor &&
                                 std::min(local.versions.maximum, peer.versions.maximum) > 0;
        return {below_floor ? ControlNegotiationStatus::DowngradeRejected
                            : ControlNegotiationStatus::NoCommonVersion,
                0,
                {},
                below_floor ? "common revision is below the security floor"
                            : "no common protocol revision"};
    }
    std::set<std::string> local_supported(local.optional_features.begin(),
                                          local.optional_features.end());
    local_supported.insert(local.mandatory_features.begin(), local.mandatory_features.end());
    std::set<std::string> peer_supported(peer.optional_features.begin(),
                                         peer.optional_features.end());
    peer_supported.insert(peer.mandatory_features.begin(), peer.mandatory_features.end());
    for (const auto& required : local.mandatory_features)
        if (!peer_supported.contains(required))
            return {ControlNegotiationStatus::UnsupportedMandatoryFeature,
                    0,
                    {},
                    "peer does not support mandatory feature '" + required + "'"};
    for (const auto& required : peer.mandatory_features)
        if (!local_supported.contains(required))
            return {ControlNegotiationStatus::UnsupportedMandatoryFeature,
                    0,
                    {},
                    "local endpoint does not support mandatory feature '" + required + "'"};
    std::vector<std::string> features;
    std::ranges::set_intersection(local_supported, peer_supported, std::back_inserter(features));
    return {ControlNegotiationStatus::Accepted, maximum, std::move(features), {}};
}

std::optional<std::string> control_request_hash(const ControlRequestEnvelope& request) {
    if (!valid_request(request, false))
        return std::nullopt;
    const auto params = canonicalize_control_json(request.params_json);
    std::string binding;
    for (const auto value :
         {std::string_view(request.client_id), std::string_view(request.registration_id),
          std::string_view(request.grant_id), std::string_view(request.instance_generation),
          std::string_view(request.operation_id), std::string_view(request.idempotency_key)}) {
        binding.append(std::to_string(value.size()));
        binding.push_back(':');
        binding.append(value);
        binding.push_back(';');
    }
    binding.append(std::to_string(request.operation_version));
    binding.push_back(';');
    const auto expected_state_generation = std::to_string(request.expected_state_generation);
    binding.append(std::to_string(expected_state_generation.size()));
    binding.push_back(':');
    binding.append(expected_state_generation);
    binding.push_back(';');
    binding.append(*params);
    return runtime::sha256_hex(binding);
}

std::optional<std::string>
encode_control_legacy_inspector_error(const ControlLegacyInspectorError& error) {
    if (!control_protocol_detail::valid_token(error.error_code, kControlMaximumErrorCodeBytes) ||
        !control_protocol_detail::valid_text(error.error_message,
                                             kControlReceiptMaximumExplanationBytes) ||
        !control_protocol_detail::valid_text(error.error_data_json,
                                             kControlMaximumResultDetailBytes) ||
        (!error.error_data_json.empty() &&
         !control_protocol_detail::parse_bounded_control_json(
             error.error_data_json, kControlMaximumResultDetailBytes,
             control_protocol_detail::kMaximumResultJsonNodes))) {
        return std::nullopt;
    }

    auto detail = choc::value::createObject("");
    detail.addMember("error_code", choc::value::createString(error.error_code));
    detail.addMember("error_message", choc::value::createString(error.error_message));
    detail.addMember("error_data_json", choc::value::createString(error.error_data_json));
    return control_protocol_detail::canonicalize_control_result_json(
        choc::json::toString(detail, false));
}

std::optional<ControlLegacyInspectorError>
decode_control_legacy_inspector_error(std::string_view detail_json) {
    const auto detail = control_protocol_detail::parse_bounded_control_json(
        detail_json, kControlMaximumResultDetailBytes,
        control_protocol_detail::kMaximumResultJsonNodes);
    ControlProtocolDiagnostics diagnostics;
    ControlLegacyInspectorError error;
    if (!detail ||
        !control_protocol_detail::only_fields(
            *detail, {"error_code", "error_message", "error_data_json"}, diagnostics) ||
        !control_protocol_detail::required_string(*detail, "error_code", error.error_code,
                                                  kControlMaximumErrorCodeBytes, diagnostics) ||
        !control_protocol_detail::required_string(*detail, "error_message", error.error_message,
                                                  kControlReceiptMaximumExplanationBytes,
                                                  diagnostics, false) ||
        !control_protocol_detail::required_string(*detail, "error_data_json", error.error_data_json,
                                                  kControlMaximumResultDetailBytes, diagnostics,
                                                  false) ||
        (!error.error_data_json.empty() &&
         !control_protocol_detail::parse_bounded_control_json(
             error.error_data_json, kControlMaximumResultDetailBytes,
             control_protocol_detail::kMaximumResultJsonNodes))) {
        return std::nullopt;
    }
    return error;
}

bool control_envelope_allowed(const ControlEnvelope& envelope, ControlEnvelopeDirection direction) {
    return std::visit(
        [direction](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ControlHealthEnvelope>) {
                return direction == ControlEnvelopeDirection::ClientToBroker ||
                       direction == ControlEnvelopeDirection::HostToBroker;
            } else if constexpr (std::is_same_v<T, ControlHealthResult> ||
                                 std::is_same_v<T, ControlErrorEnvelope>) {
                return direction == ControlEnvelopeDirection::BrokerToClient ||
                       direction == ControlEnvelopeDirection::BrokerToHost;
            } else if constexpr (std::is_same_v<T, ControlHostOpenEnvelope> ||
                                 std::is_same_v<T, ControlHostProgressEnvelope> ||
                                 std::is_same_v<T, ControlHostCompleteEnvelope>) {
                return direction == ControlEnvelopeDirection::HostToBroker;
            } else if constexpr (std::is_same_v<T, ControlHostOpenResult> ||
                                 std::is_same_v<T, ControlHostExecuteEnvelope> ||
                                 std::is_same_v<T, ControlHostCancelEnvelope>) {
                return direction == ControlEnvelopeDirection::BrokerToHost;
            } else if constexpr (std::is_same_v<T, ControlHostPreflightResponseEnvelope>) {
                return direction == ControlEnvelopeDirection::HostToLauncher;
            } else if constexpr (std::is_same_v<T, ControlHostPreflightChallengeEnvelope> ||
                                 std::is_same_v<T, ControlHostPreflightBootstrapEnvelope>) {
                return direction == ControlEnvelopeDirection::LauncherToHost;
            } else if constexpr (std::is_same_v<T, ControlNegotiationOffer> ||
                                 std::is_same_v<T, ControlRequestEnvelope> ||
                                 std::is_same_v<T, ControlCancelEnvelope> ||
                                 std::is_same_v<T, ControlSessionOpenEnvelope> ||
                                 std::is_same_v<T, ControlArtifactReadEnvelope>) {
                return direction == ControlEnvelopeDirection::ClientToBroker;
            } else {
                return direction == ControlEnvelopeDirection::BrokerToClient;
            }
        },
        envelope.payload);
}

} // namespace pulp::inspect
