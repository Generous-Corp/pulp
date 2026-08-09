#include <pulp/inspect/control_inspector_client.hpp>

#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace pulp::inspect {
namespace {

constexpr std::string_view kTraceOperationId = "dev.pulp.trace/session-control@1";
constexpr std::uint32_t kTraceOperationVersion = 1;

InspectorMessage client_error(std::string message, std::string code,
                              bool may_have_applied = false) {
    return make_error(0, std::move(message), std::move(code),
                      may_have_applied ? R"({"mayHaveApplied":true})"
                                       : R"({"mayHaveApplied":false})");
}

bool valid_session(const InspectorControlSession& session) {
    return session.transport && session.client_id && session.registration_id && session.grant_id &&
           !session.instance_generation.empty() && !session.target.session_id.empty() &&
           !session.target.instance_id.empty() && !session.target.publication_id.empty() &&
           session.expected_state_generation <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

std::optional<std::string> canonical_trace_params(std::string_view method,
                                                  std::string_view params_json) {
    choc::value::Value params;
    try {
        params = choc::json::parse(params_json.empty() ? std::string_view("{}") : params_json);
    } catch (...) {
        return std::nullopt;
    }
    if (!params.isObject())
        return std::nullopt;

    auto canonical = choc::value::createObject("");
    if (method == methods::kTraceStartSession) {
        if (params.hasObjectMember("action"))
            return std::nullopt;
        canonical.addMember("action", choc::value::createString("start"));
        params.getView().visitObjectMembers(
            [&](std::string_view name, const choc::value::ValueView& value) {
                canonical.addMember(name, value);
            });
    } else if (method == methods::kTraceStopSession) {
        bool has_fields = false;
        params.getView().visitObjectMembers(
            [&](std::string_view, const choc::value::ValueView&) { has_fields = true; });
        if (has_fields)
            return std::nullopt;
        canonical.addMember("action", choc::value::createString("stop"));
    } else {
        return std::nullopt;
    }
    return choc::json::toString(canonical, false);
}

std::optional<std::string> random_token(std::string_view prefix) {
    const auto bytes = runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string value(prefix);
    value.reserve(prefix.size() + bytes->size() * 2);
    for (const auto byte : *bytes) {
        value.push_back(hex[byte >> 4]);
        value.push_back(hex[byte & 0x0f]);
    }
    return value;
}

std::int64_t deadline_from(std::chrono::milliseconds timeout) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (timeout.count() > std::numeric_limits<std::int64_t>::max() - now)
        return std::numeric_limits<std::int64_t>::max();
    return now + timeout.count();
}

std::chrono::milliseconds remaining_until(
    std::chrono::steady_clock::time_point deadline,
    const detail::InspectorControlClock& clock = [] {
        return std::chrono::steady_clock::now();
    }) {
    const auto now = clock();
    if (now >= deadline)
        return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

std::optional<std::string> object_string(const choc::value::Value& object,
                                         std::string_view field) {
    const auto key = std::string(field);
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isString() ||
        object[key].getString().empty())
        return std::nullopt;
    return std::string(object[key].getString());
}

std::optional<choc::value::Value> parse_object(std::string_view json) {
    try {
        auto value = choc::json::parse(json);
        if (value.isObject())
            return value;
    } catch (...) {
    }
    return std::nullopt;
}

bool advertises_trace_control(const choc::value::Value& candidate) {
    if (!candidate.isObject() || !candidate.hasObjectMember("capabilities") ||
        !candidate["capabilities"].isArray())
        return false;
    const auto capabilities = candidate["capabilities"];
    for (std::uint32_t index = 0; index < capabilities.size(); ++index)
        if (capabilities[index].isString() &&
            capabilities[index].getString() == kTraceOperationId)
            return true;
    return false;
}

std::filesystem::path resolve_installed_broker(const std::filesystem::path& executable) {
    auto resolved_executable = executable;
#if defined(__APPLE__)
    if (resolved_executable.empty() || !resolved_executable.is_absolute()) {
        std::uint32_t size = 0;
        (void)_NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0)
            resolved_executable = std::filesystem::path(buffer.data());
    }
#endif
    if (resolved_executable.empty())
        return {};
    std::error_code resolved_error;
    resolved_executable = std::filesystem::weakly_canonical(resolved_executable, resolved_error);
    if (resolved_error)
        return {};
    const auto directory = resolved_executable.parent_path();
    const std::vector<std::filesystem::path> candidates{
        directory / "pulp-control-broker",
        directory.parent_path() / "libexec" / "pulp" / "pulp-control-broker",
        directory.parent_path().parent_path() / "inspect" / "pulp-control-broker",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error))
            continue;
        const auto canonical = std::filesystem::weakly_canonical(candidate, error);
        if (!error)
            return canonical;
    }
    return {};
}

class InstalledInspectorControlSessionOpener final : public InspectorControlSessionOpener {
  public:
    InstalledInspectorControlSessionOpener(std::filesystem::path executable,
                                           std::optional<std::string> instance_id)
        : executable_(std::move(executable)), instance_id_(std::move(instance_id)) {}

    std::optional<InspectorControlSession> open(std::chrono::milliseconds timeout) override {
        if (timeout <= std::chrono::milliseconds::zero())
            return std::nullopt;
        return open_until(std::chrono::steady_clock::now() + timeout);
    }

    std::optional<InspectorControlSession>
    open_until(std::chrono::steady_clock::time_point deadline) override {
        const auto broker = resolve_installed_broker(executable_);
        if (broker.empty())
            return std::nullopt;
        const auto connect_timeout = remaining_until(deadline);
        if (connect_timeout <= std::chrono::milliseconds::zero())
            return std::nullopt;
        auto connection = std::make_unique<ControlClientConnection>(ControlClientConnectionConfig{
            .endpoint_path = default_control_endpoint_path(),
            .expected_broker_executable = broker,
            .connect_timeout = connect_timeout,
        });
        if (!connection->connect())
            return std::nullopt;
        const auto enroll_timeout = remaining_until(deadline);
        if (enroll_timeout <= std::chrono::milliseconds::zero())
            return std::nullopt;
        const auto enrolled = connection->manage("enroll", "{}", enroll_timeout);
        const auto enrollment = parse_object(enrolled.data_json);
        const auto client_id = enrollment ? object_string(*enrollment, "client_id") : std::nullopt;
        if (enrolled.status_id != "accepted" || !client_id)
            return std::nullopt;

        const auto inventory_timeout = remaining_until(deadline);
        if (inventory_timeout <= std::chrono::milliseconds::zero())
            return std::nullopt;
        const auto inventory_result = connection->manage("instances", "{}", inventory_timeout);
        if (inventory_result.status_id != "completed")
            return std::nullopt;
        const auto selected = detail::select_trace_control_instance(
            inventory_result.data_json,
            instance_id_ ? std::optional<std::string_view>(*instance_id_) : std::nullopt);
        if (!selected)
            return std::nullopt;
        const auto grant_timeout = remaining_until(deadline);
        if (grant_timeout <= std::chrono::milliseconds::zero())
            return std::nullopt;
        const auto granted = connection->manage(
            "grant-request", detail::trace_control_grant_request_json(selected->instance_id),
            grant_timeout);
        const auto grant = parse_object(granted.data_json);
        const auto grant_id = grant ? object_string(*grant, "grant_id") : std::nullopt;
        if (granted.status_id != "granted" || !grant_id)
            return std::nullopt;

        return InspectorControlSession{
            .transport = std::move(connection),
            .client_id = ControlClientId{*client_id},
            .registration_id = ControlRegistrationId{selected->registration_id},
            .grant_id = ControlGrantId{*grant_id},
            .instance_generation = selected->publication_id,
            .target = {selected->session_id, selected->instance_id,
                       selected->publication_id},
        };
    }

  private:
    std::filesystem::path executable_;
    std::optional<std::string> instance_id_;
};

InspectorMessage invalid_response(std::string explanation, bool may_have_applied = true) {
    return client_error(std::move(explanation), "invalid_control_response", may_have_applied);
}

} // namespace

std::optional<detail::TraceControlInventorySelection>
detail::select_trace_control_instance(
    std::string_view inventory_json,
    std::optional<std::string_view> exact_instance_id) {
    const auto inventory = parse_object(inventory_json);
    if (!inventory || !inventory->hasObjectMember("instances") ||
        !(*inventory)["instances"].isArray())
        return std::nullopt;

    std::optional<choc::value::Value> selected;
    const auto instances = (*inventory)["instances"];
    for (std::uint32_t index = 0; index < instances.size(); ++index) {
        const auto candidate = choc::value::Value(instances[index]);
        const auto id = object_string(candidate, "instance_id");
        if (!id || (exact_instance_id && *id != *exact_instance_id) ||
            (!exact_instance_id && !advertises_trace_control(candidate)))
            continue;
        if (selected)
            return std::nullopt;
        selected = candidate;
    }
    if (!selected)
        return std::nullopt;

    const auto instance_id = object_string(*selected, "instance_id");
    const auto registration_id = object_string(*selected, "registration_id");
    const auto publication_id = object_string(*selected, "publication_id");
    const auto session_id = object_string(*selected, "session_id");
    if (!instance_id || !registration_id || !publication_id || !session_id)
        return std::nullopt;
    return detail::TraceControlInventorySelection{
        *instance_id, *registration_id, *publication_id, *session_id};
}

std::string detail::trace_control_grant_request_json(std::string_view instance_id) {
    auto params = choc::value::createObject("");
    params.addMember("instance_id", choc::value::createString(instance_id));
    params.addMember("operation_id", choc::value::createString(kTraceOperationId));
    return choc::json::toString(params, false);
}

std::filesystem::path
installed_control_broker_executable(const std::filesystem::path& client_executable) {
    return resolve_installed_broker(client_executable);
}

std::unique_ptr<InspectorControlSessionOpener>
make_installed_inspector_control_session_opener(
    std::filesystem::path client_executable,
    std::optional<std::string> exact_instance_id,
    std::string) {
    return std::make_unique<InstalledInspectorControlSessionOpener>(
        std::move(client_executable), std::move(exact_instance_id));
}

InspectorClientResult request_control_inspector(std::string method, std::string params_json,
                                                std::chrono::milliseconds timeout) {
    class DefaultDeniedOpener final : public InspectorControlSessionOpener {
      public:
        std::optional<InspectorControlSession> open(std::chrono::milliseconds) override {
            return std::nullopt;
        }
    } opener;
    return request_control_inspector(opener, std::move(method), std::move(params_json), timeout);
}

InspectorClientResult detail::request_control_inspector_with_clock(
    InspectorControlSessionOpener& opener, std::string method,
    std::string params_json, std::chrono::milliseconds timeout,
    InspectorControlClock clock) {
    InspectorClientResult result;
    if (method != methods::kTraceStartSession && method != methods::kTraceStopSession) {
        result.response = client_error(
            "canonical Inspector client does not support method: " + method, "method_not_found");
        return result;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        result.response =
            client_error("canonical Inspector request deadline has expired", "request_timeout");
        return result;
    }
    const auto deadline = clock() + timeout;
    const auto remaining = [&] { return remaining_until(deadline, clock); };

    const auto* descriptor = resolve_control_operation(kTraceOperationId, kTraceOperationVersion);
    const auto canonical_params = canonical_trace_params(method, params_json);
    ControlJsonSchemaDiagnostics diagnostics;
    if (!descriptor || descriptor->id != kTraceOperationId ||
        descriptor->version != kTraceOperationVersion || descriptor->result_kind != "response" ||
        !canonical_params ||
        !validate_control_json_schema(
            canonical_params.value_or(""),
            descriptor ? descriptor->input_schema_json : std::string_view{}, &diagnostics)) {
        result.response =
            client_error(diagnostics.explanation.empty() ? method + ": invalid params"
                                                         : std::move(diagnostics.explanation),
                         "invalid_params");
        return result;
    }

    auto session = opener.open_until(deadline);
    if (!session || !valid_session(*session)) {
        result.response = remaining() <= std::chrono::milliseconds::zero()
                              ? client_error("canonical Inspector request deadline has expired",
                                             "request_timeout")
                              : client_error("canonical Inspector control session is unavailable",
                                             "control_session_unavailable");
        return result;
    }
    result.target = session->target;

    ControlClient client(*session->transport);
    const ControlNegotiationOffer offer{
        .versions = {kControlProtocolVersion, kControlProtocolVersion},
        .mandatory_features = {"receipts"},
        .optional_features = {"progress"},
    };
    const auto negotiation_timeout = remaining();
    if (negotiation_timeout <= std::chrono::milliseconds::zero()) {
        result.response =
            client_error("canonical Inspector request deadline has expired", "request_timeout");
        return result;
    }
    auto negotiation = client.negotiate(offer, negotiation_timeout);
    if (!negotiation.response) {
        result.response =
            client_error(negotiation.explanation.empty() ? "control negotiation failed"
                                                         : std::move(negotiation.explanation),
                         negotiation.error_code.empty() ? "control_negotiation_failed"
                                                        : std::move(negotiation.error_code));
        return result;
    }
    if (negotiation.response->status != ControlNegotiationStatus::Accepted ||
        negotiation.response->selected_version != kControlProtocolVersion ||
        std::ranges::find(negotiation.response->features, "receipts") ==
            negotiation.response->features.end()) {
        result.response = invalid_response(
            "control negotiation did not select the exact receipt contract", false);
        return result;
    }

    const auto request_id = random_token("inspector-request-");
    const auto idempotency_key = random_token("inspector-idempotency-");
    if (!request_id || !idempotency_key) {
        result.response = client_error("canonical Inspector request entropy is unavailable",
                                       "entropy_unavailable");
        return result;
    }
    ControlRequestEnvelope request{
        .request_id = *request_id,
        .client_id = session->client_id.value,
        .registration_id = session->registration_id.value,
        .grant_id = session->grant_id.value,
        .instance_generation = session->instance_generation,
        .operation_id = std::string(kTraceOperationId),
        .operation_version = kTraceOperationVersion,
        .idempotency_key = *idempotency_key,
        .deadline_unix_ms = deadline_from(timeout),
        .expected_state_generation = session->expected_state_generation,
        .params_json = *canonical_params,
    };
    const auto request_hash = control_request_hash(request);
    if (!request_hash) {
        result.response =
            client_error("canonical Inspector request is invalid", "invalid_control_request");
        return result;
    }
    request.request_hash = *request_hash;

    if (encode_control_envelope({.payload = request}).empty()) {
        result.response = client_error("canonical Inspector session material is invalid",
                                       "invalid_control_session");
        return result;
    }

    const auto request_timeout = remaining();
    if (request_timeout <= std::chrono::milliseconds::zero()) {
        result.response =
            client_error("canonical Inspector request deadline has expired", "request_timeout");
        return result;
    }
    request.deadline_unix_ms = deadline_from(request_timeout);
    auto dispatched = client.request(request, request_timeout);
    if (!dispatched.response) {
        result.response =
            client_error(dispatched.explanation.empty() ? "canonical Inspector request failed"
                                                        : std::move(dispatched.explanation),
                         dispatched.error_code.empty() ? "control_transport_failed"
                                                       : std::move(dispatched.error_code),
                         true);
        return result;
    }

    const auto& receipt = *dispatched.response;
    if (receipt.request_id != request.request_id || receipt.operation_id != request.operation_id ||
        receipt.operation_version != request.operation_version ||
        !control_receipt_state_is_terminal(receipt.state) || !receipt.artifacts.empty()) {
        result.response = invalid_response("control receipt does not match the trace request");
        return result;
    }

    if (receipt.state == ControlReceiptState::Completed) {
        if (!validate_control_output_json_schema(receipt.detail_json,
                                                 descriptor->output_schema_json, &diagnostics)) {
            result.response =
                invalid_response(diagnostics.explanation.empty()
                                     ? "trace receipt result violated its declared schema"
                                     : std::move(diagnostics.explanation));
            return result;
        }
        result.response = make_response(0, receipt.detail_json);
        return result;
    }

    if (receipt.state == ControlReceiptState::Failed) {
        const auto legacy = decode_control_legacy_inspector_error(receipt.detail_json);
        if (legacy) {
            result.response =
                make_error(0, legacy->error_message, legacy->error_code, legacy->error_data_json);
            return result;
        }
        // Broker- or executor-authored failures that occur before trace
        // dispatch carry the typed result and the protocol's empty detail.
        // A non-empty malformed compatibility object still fails closed.
        if (receipt.detail_json != "{}" || !receipt.result_code) {
            result.response = invalid_response(
                "failed trace receipt did not contain legacy-inspector-json-v1 detail");
            return result;
        }
        result.response =
            client_error(receipt.explanation.empty() ? "trace operation failed before execution"
                                                     : receipt.explanation,
                         std::string(control_result_code_id(*receipt.result_code)));
        return result;
    }

    const bool may_have_applied = receipt.state == ControlReceiptState::CompletedAfterRevocation ||
                                  receipt.state == ControlReceiptState::UnknownNeedsRefresh;
    result.response = client_error(
        receipt.explanation.empty() ? "trace operation ended in state " +
                                          std::string(control_receipt_state_id(receipt.state))
                                    : receipt.explanation,
        receipt.result_code ? std::string(control_result_code_id(*receipt.result_code))
                            : "invalid_control_response",
        may_have_applied);
    return result;
}

InspectorClientResult request_control_inspector(InspectorControlSessionOpener& opener,
                                                std::string method, std::string params_json,
                                                std::chrono::milliseconds timeout) {
    return detail::request_control_inspector_with_clock(
        opener, std::move(method), std::move(params_json), timeout,
        [] { return std::chrono::steady_clock::now(); });
}

} // namespace pulp::inspect
