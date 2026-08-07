#include <pulp/inspect/control_operations.hpp>

#include "control_operation_internal.hpp"
#include "control_protocol_internal.hpp"

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>

namespace pulp::inspect {
namespace detail {

constexpr std::array<std::string_view, 36> kReceiptFields{
    "schema",
    "schema_version",
    "receipt_id",
    "state",
    "cancellation_requested",
    "cancellation_reason",
    "request_id",
    "broker_id",
    "client_principal",
    "client_id",
    "registration_id",
    "grant_id",
    "session_id",
    "instance_id",
    "publication_id",
    "instance_generation",
    "capability_id",
    "operation_id",
    "operation_version",
    "consent_decision_id",
    "manifest_digest",
    "producer_artifact_digest",
    "idempotency_key",
    "canonical_request_hash",
    "deadline_unix_ms",
    "expected_state_generation",
    "binding_hash",
    "result_code",
    "retry",
    "explanation",
    "detail",
    "result_cancellation_reason",
    "artifacts",
    "evidence_ids",
    "created_at_unix_ms",
    "updated_at_unix_ms",
};

constexpr std::size_t kMaximumIdentityBytes = 512;
constexpr std::size_t kMaximumIdempotencyKeyBytes = 128;
constexpr std::size_t kMaximumReceiptJsonNodes =
    control_protocol_detail::kMaximumResultJsonNodes + 512;

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (character < 0x20) {
                out += "\\u00";
                out.push_back(hex[character >> 4]);
                out.push_back(hex[character & 0x0f]);
            } else {
                out.push_back(static_cast<char>(character));
            }
        }
    }
    return out;
}

bool lowercase_sha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool bounded_nonempty(std::string_view value, std::size_t maximum) {
    return !value.empty() && control_protocol_detail::valid_text(value, maximum);
}

bool bounded_token(std::string_view value, std::size_t maximum) {
    return bounded_nonempty(value, maximum) &&
           std::ranges::all_of(value, [](unsigned char character) {
               return character >= 0x21 && character <= 0x7e;
           });
}

void append_framed(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
    output.push_back(';');
}

std::string idempotency_scope_hash(const ControlOperationBinding& binding) {
    std::string framed;
    append_framed(framed, binding.broker_id.value);
    append_framed(framed, binding.client_principal);
    append_framed(framed, binding.client_id.value);
    append_framed(framed, binding.registration_id.value);
    append_framed(framed, binding.grant_id.value);
    append_framed(framed, binding.session_id);
    append_framed(framed, binding.instance_id);
    append_framed(framed, binding.publication_id);
    append_framed(framed, binding.instance_generation);
    append_framed(framed, capability_contract_id(binding.capability));
    append_framed(framed, binding.operation_id);
    append_framed(framed, std::to_string(binding.operation_version));
    append_framed(framed, binding.consent_decision_id);
    append_framed(framed, binding.manifest_digest);
    append_framed(framed, binding.producer_artifact_digest);
    append_framed(framed, binding.idempotency_key);
    return runtime::sha256_hex(framed);
}

std::string request_scope_hash(const ControlOperationBinding& binding) {
    std::string framed;
    append_framed(framed, binding.client_id.value);
    append_framed(framed, binding.request_id);
    return runtime::sha256_hex(framed);
}

std::string binding_hash(const ControlOperationBinding& binding) {
    std::string framed;
    append_framed(framed, "dev.pulp.control/operation-binding@1");
    append_framed(framed, std::to_string(kControlAuthorityBindingVersion));
    append_framed(framed, binding.broker_id.value);
    append_framed(framed, binding.client_principal);
    append_framed(framed, binding.client_id.value);
    append_framed(framed, binding.registration_id.value);
    append_framed(framed, binding.grant_id.value);
    append_framed(framed, binding.session_id);
    append_framed(framed, binding.instance_id);
    append_framed(framed, binding.publication_id);
    append_framed(framed, binding.instance_generation);
    append_framed(framed, capability_contract_id(binding.capability));
    append_framed(framed, binding.operation_id);
    append_framed(framed, std::to_string(binding.operation_version));
    append_framed(framed, binding.consent_decision_id);
    append_framed(framed, binding.manifest_digest);
    append_framed(framed, binding.producer_artifact_digest);
    append_framed(framed, std::to_string(binding.deadline_unix_ms));
    append_framed(framed, std::to_string(binding.expected_state_generation));
    append_framed(framed, binding.request_id);
    append_framed(framed, binding.idempotency_key);
    append_framed(framed, binding.canonical_request_hash);
    return runtime::sha256_hex(framed);
}

std::string idempotency_content_hash(const ControlOperationBinding& binding) {
    auto content = binding;
    content.request_id.clear();
    content.deadline_unix_ms = 0;
    return binding_hash(content);
}

bool valid_binding(const ControlOperationBinding& binding) {
    return bounded_token(binding.request_id, kControlReceiptMaximumRequestIdBytes) &&
           bounded_nonempty(binding.broker_id.value, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.client_principal, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.client_id.value, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.registration_id.value, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.grant_id.value, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.session_id, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.instance_id, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.publication_id, kMaximumIdentityBytes) &&
           bounded_nonempty(binding.instance_generation, kMaximumIdentityBytes) &&
           capability_from_contract_id(capability_contract_id(binding.capability)) ==
               binding.capability &&
           bounded_token(binding.operation_id, kControlReceiptMaximumOperationIdBytes) &&
           binding.operation_version > 0 &&
           bounded_nonempty(binding.consent_decision_id, kMaximumIdentityBytes) &&
           lowercase_sha256(binding.manifest_digest) &&
           lowercase_sha256(binding.producer_artifact_digest) &&
           bounded_nonempty(binding.idempotency_key, kMaximumIdempotencyKeyBytes) &&
           lowercase_sha256(binding.canonical_request_hash) && binding.deadline_unix_ms > 0 &&
           binding.expected_state_generation <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

bool state_requires_result_code(ControlReceiptState state) {
    return state == ControlReceiptState::Failed || state == ControlReceiptState::Cancelled ||
           state == ControlReceiptState::CompletedAfterRevocation ||
           state == ControlReceiptState::UnknownNeedsRefresh;
}

bool valid_store_transition(ControlReceiptState from, ControlReceiptState to) {
    if (!valid_control_receipt_transition(from, to))
        return false;
    if (from == ControlReceiptState::Admitted) {
        // Running is entered only through ControlOperationStore::begin(),
        // which resolves cancellation and deadline under the same lock.
        return to == ControlReceiptState::Failed || to == ControlReceiptState::Cancelled;
    }
    return from == ControlReceiptState::Running && control_receipt_state_is_terminal(to);
}

bool valid_receipt_id(std::string_view value) {
    constexpr std::string_view prefix = "receipt-";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 32)
        return false;
    return std::ranges::all_of(value.substr(prefix.size()), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

std::optional<std::string> random_receipt_id() {
    const auto random = runtime::secure_random_bytes(16);
    if (!random)
        return std::nullopt;
    return "receipt-" + runtime::hex_encode(*random);
}

std::int64_t unix_milliseconds(std::chrono::system_clock::time_point time) {
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return std::max<std::int64_t>(0, value);
}

std::string serialize_receipt(const ControlOperationReceipt& receipt) {
    const auto quote = [](std::string_view value) { return "\"" + json_escape(value) + "\""; };
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": " << quote(kControlOperationReceiptSchemaId) << ",\n"
        << "  \"schema_version\": " << receipt.schema_version << ",\n"
        << "  \"receipt_id\": " << quote(receipt.receipt_id.value) << ",\n"
        << "  \"state\": " << quote(control_receipt_state_id(receipt.state)) << ",\n"
        << "  \"cancellation_requested\": " << (receipt.cancellation_requested ? "true" : "false")
        << ",\n"
        << "  \"cancellation_reason\": " << quote(receipt.cancellation_reason) << ",\n"
        << "  \"request_id\": " << quote(receipt.binding.request_id) << ",\n"
        << "  \"broker_id\": " << quote(receipt.binding.broker_id.value) << ",\n"
        << "  \"client_principal\": " << quote(receipt.binding.client_principal) << ",\n"
        << "  \"client_id\": " << quote(receipt.binding.client_id.value) << ",\n"
        << "  \"registration_id\": " << quote(receipt.binding.registration_id.value) << ",\n"
        << "  \"grant_id\": " << quote(receipt.binding.grant_id.value) << ",\n"
        << "  \"session_id\": " << quote(receipt.binding.session_id) << ",\n"
        << "  \"instance_id\": " << quote(receipt.binding.instance_id) << ",\n"
        << "  \"publication_id\": " << quote(receipt.binding.publication_id) << ",\n"
        << "  \"instance_generation\": " << quote(receipt.binding.instance_generation) << ",\n"
        << "  \"capability_id\": " << quote(capability_contract_id(receipt.binding.capability))
        << ",\n"
        << "  \"operation_id\": " << quote(receipt.binding.operation_id) << ",\n"
        << "  \"operation_version\": " << receipt.binding.operation_version << ",\n"
        << "  \"consent_decision_id\": " << quote(receipt.binding.consent_decision_id) << ",\n"
        << "  \"manifest_digest\": " << quote(receipt.binding.manifest_digest) << ",\n"
        << "  \"producer_artifact_digest\": " << quote(receipt.binding.producer_artifact_digest)
        << ",\n"
        << "  \"idempotency_key\": " << quote(receipt.binding.idempotency_key) << ",\n"
        << "  \"canonical_request_hash\": " << quote(receipt.binding.canonical_request_hash)
        << ",\n"
        << "  \"deadline_unix_ms\": " << receipt.binding.deadline_unix_ms << ",\n"
        << "  \"expected_state_generation\": " << receipt.binding.expected_state_generation << ",\n"
        << "  \"binding_hash\": " << quote(receipt.binding_hash) << ",\n"
        << "  \"result_code\": ";
    if (receipt.result.result_code)
        out << quote(control_result_code_id(*receipt.result.result_code));
    else
        out << "null";
    out << ",\n"
        << "  \"retry\": " << quote(control_retry_classification_id(receipt.result.retry)) << ",\n"
        << "  \"explanation\": " << quote(receipt.result.explanation) << ",\n"
        << "  \"detail\": " << receipt.result.detail_json << ",\n"
        << "  \"result_cancellation_reason\": " << quote(receipt.result.cancellation_reason)
        << ",\n"
        << "  \"artifacts\": [";
    for (std::size_t index = 0; index < receipt.result.artifacts.size(); ++index) {
        if (index != 0)
            out << ",";
        const auto& artifact = receipt.result.artifacts[index];
        out << "{\"artifact_id\":" << quote(artifact.artifact_id)
            << ",\"media_type\":" << quote(artifact.media_type)
            << ",\"byte_size\":" << artifact.byte_size << "}";
    }
    out << "],\n  \"evidence_ids\": [";
    for (std::size_t index = 0; index < receipt.result.evidence_ids.size(); ++index) {
        if (index != 0)
            out << ",";
        out << quote(receipt.result.evidence_ids[index]);
    }
    out << "],\n"
        << "  \"created_at_unix_ms\": " << receipt.created_at_unix_ms << ",\n"
        << "  \"updated_at_unix_ms\": " << receipt.updated_at_unix_ms << "\n"
        << "}\n";
    return out.str();
}

bool read_string(const choc::value::ValueView& root, std::string_view name, std::string& output) {
    const auto value = root[name];
    if (!value.isString())
        return false;
    output = std::string(value.getString());
    return true;
}

bool read_nonnegative_integer(const choc::value::ValueView& root, std::string_view name,
                              std::int64_t& output) {
    const auto value = root[name];
    if (!value.isInt32() && !value.isInt64())
        return false;
    output = value.getInt64();
    return output >= 0;
}

std::optional<ControlOperationReceipt> parse_receipt(std::string_view contents,
                                                     std::size_t maximum_bytes) {
    try {
        const auto parsed = control_protocol_detail::parse_bounded_control_json(
            contents, maximum_bytes, kMaximumReceiptJsonNodes);
        if (!parsed)
            return std::nullopt;
        const auto& root = *parsed;
        if (!root.isObject())
            return std::nullopt;
        bool unknown = false;
        std::size_t fields = 0;
        root.getView().visitObjectMembers(
            [&](std::string_view name, const choc::value::ValueView&) {
                ++fields;
                if (std::find(kReceiptFields.begin(), kReceiptFields.end(), name) ==
                    kReceiptFields.end())
                    unknown = true;
            });
        if (unknown || fields != kReceiptFields.size())
            return std::nullopt;

        std::string schema;
        std::int64_t schema_version = 0;
        std::int64_t operation_version = 0;
        std::int64_t expected_state_generation = 0;
        std::string capability_id;
        ControlOperationReceipt receipt;
        std::string state;
        std::string retry;
        const auto cancellation_requested = root["cancellation_requested"];
        if (!read_string(root, "schema", schema) || schema != kControlOperationReceiptSchemaId ||
            !read_nonnegative_integer(root, "schema_version", schema_version) ||
            schema_version != kControlOperationReceiptSchemaVersion ||
            !read_string(root, "receipt_id", receipt.receipt_id.value) ||
            !read_string(root, "state", state) || !cancellation_requested.isBool() ||
            !read_string(root, "cancellation_reason", receipt.cancellation_reason) ||
            !read_string(root, "request_id", receipt.binding.request_id) ||
            !read_string(root, "broker_id", receipt.binding.broker_id.value) ||
            !read_string(root, "client_principal", receipt.binding.client_principal) ||
            !read_string(root, "client_id", receipt.binding.client_id.value) ||
            !read_string(root, "registration_id", receipt.binding.registration_id.value) ||
            !read_string(root, "grant_id", receipt.binding.grant_id.value) ||
            !read_string(root, "session_id", receipt.binding.session_id) ||
            !read_string(root, "instance_id", receipt.binding.instance_id) ||
            !read_string(root, "publication_id", receipt.binding.publication_id) ||
            !read_string(root, "instance_generation", receipt.binding.instance_generation) ||
            !read_string(root, "capability_id", capability_id) ||
            !read_string(root, "operation_id", receipt.binding.operation_id) ||
            !read_nonnegative_integer(root, "operation_version", operation_version) ||
            !read_string(root, "consent_decision_id", receipt.binding.consent_decision_id) ||
            !read_string(root, "manifest_digest", receipt.binding.manifest_digest) ||
            !read_string(root, "producer_artifact_digest",
                         receipt.binding.producer_artifact_digest) ||
            !read_string(root, "idempotency_key", receipt.binding.idempotency_key) ||
            !read_string(root, "canonical_request_hash", receipt.binding.canonical_request_hash) ||
            !read_nonnegative_integer(root, "deadline_unix_ms", receipt.binding.deadline_unix_ms) ||
            !read_nonnegative_integer(root, "expected_state_generation",
                                      expected_state_generation) ||
            !read_string(root, "binding_hash", receipt.binding_hash) ||
            !read_string(root, "retry", retry) ||
            !read_string(root, "explanation", receipt.result.explanation) ||
            !read_string(root, "result_cancellation_reason", receipt.result.cancellation_reason) ||
            !read_nonnegative_integer(root, "created_at_unix_ms", receipt.created_at_unix_ms) ||
            !read_nonnegative_integer(root, "updated_at_unix_ms", receipt.updated_at_unix_ms)) {
            return std::nullopt;
        }
        receipt.cancellation_requested = cancellation_requested.getBool();
        if (operation_version <= 0 || operation_version > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        receipt.binding.operation_version = static_cast<std::uint32_t>(operation_version);
        receipt.binding.expected_state_generation =
            static_cast<std::uint64_t>(expected_state_generation);
        const auto capability = capability_from_contract_id(capability_id);
        if (!capability)
            return std::nullopt;
        receipt.binding.capability = *capability;
        constexpr std::array states{
            ControlReceiptState::Admitted,
            ControlReceiptState::Running,
            ControlReceiptState::Completed,
            ControlReceiptState::Failed,
            ControlReceiptState::Cancelled,
            ControlReceiptState::CompletedAfterRevocation,
            ControlReceiptState::UnknownNeedsRefresh,
        };
        constexpr std::array result_codes{
            ControlResultCode::NotImplemented,
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
            ControlResultCode::InternalError,
        };
        constexpr std::array retries{
            ControlRetryClassification::Never,
            ControlRetryClassification::AfterRefresh,
            ControlRetryClassification::AfterGrant,
            ControlRetryClassification::AfterBackoff,
        };
        const auto find_state = [&]() -> std::optional<ControlReceiptState> {
            for (const auto candidate : states)
                if (control_receipt_state_id(candidate) == state)
                    return candidate;
            return std::nullopt;
        };
        const auto find_retry = [&]() -> std::optional<ControlRetryClassification> {
            for (const auto candidate : retries)
                if (control_retry_classification_id(candidate) == retry)
                    return candidate;
            return std::nullopt;
        };
        const auto parsed_state = find_state();
        const auto parsed_retry = find_retry();
        const auto result = root["result_code"];
        if (result.isString()) {
            for (const auto candidate : result_codes) {
                if (control_result_code_id(candidate) == result.getString()) {
                    receipt.result.result_code = candidate;
                    break;
                }
            }
            if (!receipt.result.result_code)
                return std::nullopt;
        } else if (!result.isVoid()) {
            return std::nullopt;
        }
        receipt.result.detail_json = choc::json::toString(root["detail"], false);
        const auto canonical_detail =
            control_protocol_detail::canonicalize_control_result_json(receipt.result.detail_json);
        if (!canonical_detail)
            return std::nullopt;
        receipt.result.detail_json = *canonical_detail;
        const auto artifacts = root["artifacts"];
        const auto evidence_ids = root["evidence_ids"];
        if (!artifacts.isArray() || !evidence_ids.isArray() ||
            artifacts.size() > kControlReceiptMaximumArtifacts ||
            evidence_ids.size() > kControlReceiptMaximumEvidenceIds)
            return std::nullopt;
        for (std::uint32_t index = 0; index < artifacts.size(); ++index) {
            const auto artifact = artifacts[index];
            if (!artifact.isObject())
                return std::nullopt;
            std::size_t artifact_fields = 0;
            bool unknown_artifact_field = false;
            artifact.visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
                ++artifact_fields;
                if (name != "artifact_id" && name != "media_type" && name != "byte_size")
                    unknown_artifact_field = true;
            });
            ControlArtifactHandle parsed_artifact;
            std::int64_t byte_size = 0;
            if (unknown_artifact_field || artifact_fields != 3 ||
                !read_string(artifact, "artifact_id", parsed_artifact.artifact_id) ||
                !read_string(artifact, "media_type", parsed_artifact.media_type) ||
                !read_nonnegative_integer(artifact, "byte_size", byte_size) ||
                !bounded_token(parsed_artifact.artifact_id,
                               kControlReceiptMaximumArtifactIdBytes) ||
                !bounded_token(parsed_artifact.media_type,
                               kControlReceiptMaximumArtifactMediaTypeBytes))
                return std::nullopt;
            parsed_artifact.byte_size = static_cast<std::uint64_t>(byte_size);
            receipt.result.artifacts.push_back(std::move(parsed_artifact));
        }
        for (std::uint32_t index = 0; index < evidence_ids.size(); ++index) {
            const auto evidence = evidence_ids[index];
            if (!evidence.isString() ||
                !bounded_nonempty(evidence.getString(), kControlReceiptMaximumEvidenceIdBytes))
                return std::nullopt;
            receipt.result.evidence_ids.emplace_back(evidence.getString());
        }
        const bool nonterminal_result_is_empty =
            parsed_retry && *parsed_retry == ControlRetryClassification::Never &&
            receipt.result.explanation.empty() && receipt.result.cancellation_reason.empty() &&
            receipt.result.artifacts.empty() && receipt.result.evidence_ids.empty() &&
            receipt.result.detail_json == "{}";
        if (!parsed_state || !parsed_retry ||
            state_requires_result_code(*parsed_state) != receipt.result.result_code.has_value() ||
            (!control_receipt_state_is_terminal(*parsed_state) && !nonterminal_result_is_empty) ||
            (*parsed_state == ControlReceiptState::Cancelled &&
             receipt.result.result_code != ControlResultCode::Cancelled) ||
            (*parsed_state == ControlReceiptState::CompletedAfterRevocation &&
             receipt.result.result_code != ControlResultCode::CompletedAfterRevocation) ||
            (*parsed_state == ControlReceiptState::UnknownNeedsRefresh &&
             receipt.result.result_code != ControlResultCode::UnknownNeedsRefresh) ||
            !valid_receipt_id(receipt.receipt_id.value) || !valid_binding(receipt.binding) ||
            receipt.binding_hash != binding_hash(receipt.binding) ||
            receipt.cancellation_requested != !receipt.cancellation_reason.empty() ||
            !control_protocol_detail::valid_text(receipt.cancellation_reason,
                                                 kControlReceiptMaximumCancellationReasonBytes) ||
            !control_protocol_detail::valid_text(receipt.result.explanation,
                                                 kControlReceiptMaximumExplanationBytes) ||
            !control_protocol_detail::valid_text(receipt.result.cancellation_reason,
                                                 kControlReceiptMaximumCancellationReasonBytes) ||
            receipt.updated_at_unix_ms < receipt.created_at_unix_ms) {
            return std::nullopt;
        }
        receipt.schema_version = static_cast<std::uint32_t>(schema_version);
        receipt.state = *parsed_state;
        receipt.result.retry = *parsed_retry;
        return receipt;
    } catch (...) {
        return std::nullopt;
    }
}

bool normalize_transition_result(ControlReceiptState next, ControlOperationResult& result) {
    const auto canonical_detail =
        control_protocol_detail::canonicalize_control_result_json(result.detail_json);
    const bool valid_lineage =
        result.artifacts.size() <= kControlReceiptMaximumArtifacts &&
        result.evidence_ids.size() <= kControlReceiptMaximumEvidenceIds &&
        std::ranges::all_of(
            result.artifacts,
            [](const auto& artifact) {
                return bounded_token(artifact.artifact_id, kControlReceiptMaximumArtifactIdBytes) &&
                       bounded_token(artifact.media_type,
                                     kControlReceiptMaximumArtifactMediaTypeBytes) &&
                       artifact.byte_size <=
                           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            }) &&
        std::ranges::all_of(result.evidence_ids, [](const auto& evidence) {
            return bounded_nonempty(evidence, kControlReceiptMaximumEvidenceIdBytes);
        });
    const bool nonterminal_result_is_empty =
        result.retry == ControlRetryClassification::Never && result.explanation.empty() &&
        result.cancellation_reason.empty() && result.artifacts.empty() &&
        result.evidence_ids.empty() && canonical_detail && *canonical_detail == "{}";
    if (!canonical_detail || state_requires_result_code(next) != result.result_code.has_value() ||
        (!control_receipt_state_is_terminal(next) && !nonterminal_result_is_empty) ||
        (next == ControlReceiptState::Cancelled &&
         result.result_code != ControlResultCode::Cancelled) ||
        (next == ControlReceiptState::CompletedAfterRevocation &&
         result.result_code != ControlResultCode::CompletedAfterRevocation) ||
        (next == ControlReceiptState::UnknownNeedsRefresh &&
         result.result_code != ControlResultCode::UnknownNeedsRefresh) ||
        !control_protocol_detail::valid_text(result.explanation,
                                             kControlReceiptMaximumExplanationBytes) ||
        !control_protocol_detail::valid_text(result.cancellation_reason,
                                             kControlReceiptMaximumCancellationReasonBytes) ||
        !valid_lineage) {
        return false;
    }
    result.detail_json = *canonical_detail;
    return true;
}

bool elapsed_at_least(std::int64_t now, std::int64_t since, std::chrono::milliseconds duration) {
    return now >= since &&
           static_cast<std::uint64_t>(now - since) >= static_cast<std::uint64_t>(duration.count());
}

} // namespace detail

std::string_view control_operation_store_status_id(ControlOperationStoreStatus status) {
    switch (status) {
    case ControlOperationStoreStatus::Opened:
        return "opened";
    case ControlOperationStoreStatus::Admitted:
        return "admitted";
    case ControlOperationStoreStatus::Replay:
        return "replay";
    case ControlOperationStoreStatus::ReplayWindowExpired:
        return "replay-window-expired";
    case ControlOperationStoreStatus::Transitioned:
        return "transitioned";
    case ControlOperationStoreStatus::CancellationRequested:
        return "cancellation-requested";
    case ControlOperationStoreStatus::IdempotencyConflict:
        return "idempotency-conflict";
    case ControlOperationStoreStatus::RequestIdConflict:
        return "request-id-conflict";
    case ControlOperationStoreStatus::InvalidRequest:
        return "invalid-request";
    case ControlOperationStoreStatus::InvalidTransition:
        return "invalid-transition";
    case ControlOperationStoreStatus::NotFound:
        return "not-found";
    case ControlOperationStoreStatus::ResourceExhausted:
        return "resource-exhausted";
    case ControlOperationStoreStatus::StoreUnavailable:
        return "store-unavailable";
    case ControlOperationStoreStatus::MalformedStore:
        return "malformed-store";
    case ControlOperationStoreStatus::PersistenceError:
        return "persistence-error";
    }
    return "store-unavailable";
}

} // namespace pulp::inspect
