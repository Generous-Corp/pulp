#include "cli_common.hpp"
#include "inspector_shipping_report.hpp"

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace pulp::inspect;

constexpr std::int64_t kDefaultRequestTimeoutMs = 3'000;
constexpr std::int64_t kMaximumRequestTimeoutMs = 300'000;

struct Connection {
    std::unique_ptr<ControlClientConnection> transport;
    std::string client_id;
};

std::filesystem::path broker_executable() {
    const auto self = current_executable_path();
    const std::vector<std::filesystem::path> candidates{
        self.parent_path() / "pulp-control-broker",
        self.parent_path().parent_path().parent_path() / "inspect" / "pulp-control-broker",
        self.parent_path().parent_path() / "libexec" / "pulp" / "pulp-control-broker",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return std::filesystem::weakly_canonical(candidate, error);
    }
    return {};
}

std::string json_error(std::string_view code, std::string_view explanation) {
    auto value = choc::value::createObject("");
    value.addMember("error", choc::value::createString(code));
    value.addMember("explanation", choc::value::createString(explanation));
    value.addMember("schema", choc::value::createString("dev.pulp.control/cli-error@1"));
    return choc::json::toString(value, false);
}

int fail(std::string_view code, std::string_view explanation, bool json) {
    if (json)
        std::cout << json_error(code, explanation) << '\n';
    else
        std::cerr << "Error [" << code << "]: " << explanation << '\n';
    return code == "invalid-request" ? 2 : 1;
}

std::optional<Connection> connect(bool json, std::chrono::milliseconds frame_read_timeout) {
    const auto broker = broker_executable();
    if (broker.empty()) {
        (void)fail("broker-unavailable", "the installed pulp-control-broker was not found", json);
        return std::nullopt;
    }
    auto transport = std::make_unique<ControlClientConnection>(ControlClientConnectionConfig{
        .endpoint_path = default_control_endpoint_path(),
        .expected_broker_executable = broker,
        .frame_read_timeout = frame_read_timeout,
    });
    if (!transport->connect()) {
        (void)fail(transport->last_error_code(), transport->last_error_explanation(), json);
        return std::nullopt;
    }
    const auto enrolled = transport->manage("enroll");
    if (enrolled.status_id != "accepted") {
        (void)fail(enrolled.status_id, enrolled.explanation, json);
        return std::nullopt;
    }
    try {
        const auto data = choc::json::parse(enrolled.data_json);
        if (!data.isObject() || !data.hasObjectMember("client_id") ||
            !data["client_id"].isString()) {
            (void)fail("malformed-response", "broker enrollment omitted client_id", json);
            return std::nullopt;
        }
        return Connection{std::move(transport), std::string(data["client_id"].getString())};
    } catch (...) {
        (void)fail("malformed-response", "broker enrollment returned invalid data", json);
        return std::nullopt;
    }
}

std::optional<choc::value::Value> instances(Connection& connection, bool json) {
    const auto result = connection.transport->manage("instances");
    if (result.status_id != "completed") {
        (void)fail(result.status_id, result.explanation, json);
        return std::nullopt;
    }
    try {
        auto data = choc::json::parse(result.data_json);
        if (!data.isObject() || !data.hasObjectMember("instances") ||
            !data["instances"].isArray()) {
            (void)fail("malformed-response", "broker inventory omitted instances", json);
            return std::nullopt;
        }
        return data;
    } catch (...) {
        (void)fail("malformed-response", "broker inventory returned invalid data", json);
        return std::nullopt;
    }
}

std::optional<choc::value::Value> exact_instance(Connection& connection,
                                                 std::string_view instance_id, bool json) {
    auto data = instances(connection, json);
    if (!data)
        return std::nullopt;
    std::optional<choc::value::Value> match;
    const auto values = (*data)["instances"];
    for (std::uint32_t index = 0; index < values.size(); ++index) {
        if (values[index]["instance_id"].getString() == instance_id) {
            if (match) {
                (void)fail("ambiguous-instance", "the exact instance selector is ambiguous", json);
                return std::nullopt;
            }
            match = choc::value::Value(values[index]);
        }
    }
    if (!match)
        (void)fail("instance-not-found", "the exact instance is not live", json);
    return match;
}

std::optional<std::string> token(std::string_view prefix) {
    const auto bytes = pulp::runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return std::string(prefix) + pulp::runtime::hex_encode(*bytes);
}

void help() {
    std::cout << "pulp control — authenticated local capability control\n\n"
                 "Usage: pulp control profiles [--json]\n"
                 "       pulp control instances [--json]\n"
                 "       pulp control status --instance ID [--explain] [--json]\n"
                 "       pulp control grant-request --instance ID --profile PROFILE [--json]\n"
                 "       pulp control call --instance ID OPERATION [--grant ID] [--profile "
                 "PROFILE] [--params JSON] [--timeout-ms MS] [--json]\n"
                 "       pulp control watch --instance ID RESOURCE [--grant ID] [--profile "
                 "PROFILE] [--params JSON] [--timeout-ms MS] [--json]\n"
                 "       pulp control artifact --id ID --out FILE [--json]\n"
                 "       pulp control revoke --grant ID [--json]\n"
                 "       pulp control audit ARTIFACT [--baseline ARTIFACT] [--json]\n\n"
                 "No host, port, discovery-file, or raw protocol options are exposed.\n";
}

void print_profiles(bool json) {
    if (json) {
        auto profiles = choc::value::createEmptyArray();
        for (const auto profile :
             {InspectorProfile::Off, InspectorProfile::Observe, InspectorProfile::Develop}) {
            auto value = choc::value::createObject("");
            value.addMember("id", choc::value::createString(profile_id(profile)));
            auto capabilities = choc::value::createEmptyArray();
            for (const auto capability : profile_capabilities(profile))
                capabilities.addArrayElement(choc::value::createString(capability_id(capability)));
            value.addMember("capabilities", capabilities);
            profiles.addArrayElement(value);
        }
        auto root = choc::value::createObject("");
        root.addMember("schemaVersion", choc::value::createInt32(1));
        root.addMember("profiles", profiles);
        std::cout << choc::json::toString(root, false) << '\n';
        return;
    }
    for (const auto profile :
         {InspectorProfile::Off, InspectorProfile::Observe, InspectorProfile::Develop}) {
        std::cout << profile_id(profile) << '\n';
        for (const auto capability : profile_capabilities(profile))
            std::cout << "  " << capability_id(capability) << '\n';
    }
}

bool take(const std::vector<std::string>& args, std::size_t& i, std::string& value) {
    if (++i >= args.size() || args[i].empty())
        return false;
    value = args[i];
    return true;
}

} // namespace

int cmd_control(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        help();
        return args.empty() ? 2 : 0;
    }
    const auto verb = args[0];
    bool json = false, explain = false, params_provided = false;
    std::string instance_id, grant_id, profile, params = "{}", output, artifact_id, baseline;
    std::string timeout_text;
    std::string positional;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--json")
            json = true;
        else if (arg == "--explain")
            explain = true;
        else if (arg == "--instance") {
            if (!take(args, i, instance_id))
                return fail("invalid-request", "--instance requires ID", json);
        } else if (arg == "--grant") {
            if (!take(args, i, grant_id))
                return fail("invalid-request", "--grant requires ID", json);
        } else if (arg == "--profile") {
            if (!take(args, i, profile))
                return fail("invalid-request", "--profile requires PROFILE", json);
        } else if (arg == "--params") {
            if (!take(args, i, params))
                return fail("invalid-request", "--params requires JSON", json);
            params_provided = true;
        } else if (arg == "--out") {
            if (!take(args, i, output))
                return fail("invalid-request", "--out requires FILE", json);
        } else if (arg == "--id") {
            if (!take(args, i, artifact_id))
                return fail("invalid-request", "--id requires ID", json);
        } else if (arg == "--baseline") {
            if (!take(args, i, baseline))
                return fail("invalid-request", "--baseline requires ARTIFACT", json);
        } else if (arg == "--timeout-ms") {
            if (!take(args, i, timeout_text))
                return fail("invalid-request", "--timeout-ms requires MS", json);
        } else if (arg == "--help" || arg == "-h") {
            help();
            return 0;
        } else if (arg.starts_with('-') || !positional.empty())
            return fail("invalid-request", "unknown or repeated argument: " + arg, json);
        else
            positional = arg;
    }

    if (!timeout_text.empty() && verb != "call" && verb != "watch")
        return fail("invalid-request", "--timeout-ms is valid only for call or watch", json);

    if (verb == "audit") {
        if (positional.empty())
            return fail("invalid-request", "audit requires ARTIFACT", json);
        const auto report = pulp::cli::inspector_shipping::audit_artifact(positional);
        if (!baseline.empty()) {
            const auto baseline_report = pulp::cli::inspector_shipping::audit_artifact(baseline);
            std::set<std::string> baseline_capabilities;
            std::set<std::string> current_capabilities;
            auto collect = [](const auto& source, auto& destination) {
                for (const auto& manifest : source.manifests) {
                    const auto& ids = manifest.control_capabilities.empty()
                                          ? manifest.capabilities
                                          : manifest.control_capabilities;
                    destination.insert(ids.begin(), ids.end());
                }
            };
            collect(baseline_report, baseline_capabilities);
            collect(report, current_capabilities);
            std::vector<std::string> additions;
            std::ranges::set_difference(current_capabilities, baseline_capabilities,
                                        std::back_inserter(additions));
            if (report.ships_inspector && !baseline_report.ships_inspector)
                additions.emplace_back("control-endpoint");
            if (report.ships_runtime_eval && !baseline_report.ships_runtime_eval)
                additions.emplace_back("runtime-eval");
            const bool passed = report.complete && baseline_report.complete && additions.empty();
            if (json) {
                auto root = choc::value::createObject("");
                root.addMember(
                    "baseline",
                    choc::json::parse(pulp::cli::inspector_shipping::audit_json(baseline_report)));
                root.addMember("current", choc::json::parse(
                                              pulp::cli::inspector_shipping::audit_json(report)));
                auto added = choc::value::createEmptyArray();
                for (const auto& value : additions)
                    added.addArrayElement(choc::value::createString(value));
                root.addMember("newAuthority", added);
                root.addMember("ok", choc::value::createBool(passed));
                root.addMember("schema",
                               choc::value::createString("pulp.control.audit-baseline.v1"));
                root.addMember("verdict", choc::value::createString(passed ? "pass" : "block"));
                std::cout << choc::json::toString(root, false) << '\n';
            } else {
                std::cout << "Control baseline audit: " << (passed ? "PASS" : "BLOCK") << '\n';
                for (const auto& value : additions)
                    std::cout << "  new authority: " << value << '\n';
                if (!report.complete)
                    std::cout << "  current: " << report.error << '\n';
                if (!baseline_report.complete)
                    std::cout << "  baseline: " << baseline_report.error << '\n';
            }
            return passed ? 0 : 1;
        }
        std::cout << (json ? pulp::cli::inspector_shipping::audit_json(report) + "\n"
                           : pulp::cli::inspector_shipping::audit_human(report));
        return report.complete ? 0 : 1;
    }
    if (verb == "profiles") {
        if (!positional.empty() || explain || !instance_id.empty() || !grant_id.empty() ||
            !profile.empty() || params_provided || !output.empty() || !artifact_id.empty() ||
            !baseline.empty())
            return fail("invalid-request", "profiles accepts only --json", json);
        print_profiles(json);
        return 0;
    }
    if (verb != "instances" && verb != "status" && verb != "grant-request" && verb != "call" &&
        verb != "watch" && verb != "artifact" && verb != "revoke")
        return fail("invalid-request", "unknown control command: " + verb, json);
    if (!positional.empty() && verb != "call" && verb != "watch")
        return fail("invalid-request", verb + " does not accept a positional argument", json);
    if (explain && verb != "status")
        return fail("invalid-request", "--explain is valid only for status", json);
    if (!baseline.empty())
        return fail("invalid-request", "--baseline is valid only for audit", json);
    if (!output.empty() && verb != "artifact")
        return fail("invalid-request", "--out is valid only for artifact", json);
    if (!artifact_id.empty() && verb != "artifact")
        return fail("invalid-request", "--id is valid only for artifact", json);
    if (!instance_id.empty() && verb != "status" && verb != "grant-request" && verb != "call" &&
        verb != "watch")
        return fail("invalid-request", "--instance is not valid for " + verb, json);
    if (!profile.empty() && verb != "grant-request" && verb != "call" && verb != "watch")
        return fail("invalid-request", "--profile is not valid for " + verb, json);
    if (!grant_id.empty() && verb != "call" && verb != "watch" && verb != "revoke")
        return fail("invalid-request", "--grant is not valid for " + verb, json);
    if ((verb == "status" || verb == "grant-request" || verb == "call" || verb == "watch") &&
        instance_id.empty())
        return fail("invalid-request", verb + " requires --instance ID", json);
    if (verb == "grant-request" && profile.empty())
        return fail("invalid-request", "grant-request requires --profile PROFILE", json);
    if (verb == "revoke" && grant_id.empty())
        return fail("invalid-request", "revoke requires --grant ID", json);
    if (verb == "artifact" && (artifact_id.empty() || output.empty()))
        return fail("invalid-request", "artifact requires --id ID --out FILE", json);

    std::int64_t timeout_ms = kDefaultRequestTimeoutMs;
    if (!timeout_text.empty()) {
        const auto* begin = timeout_text.data();
        const auto* end = begin + timeout_text.size();
        const auto parsed = std::from_chars(begin, end, timeout_ms);
        if (parsed.ec != std::errc{} || parsed.ptr != end || timeout_ms <= 0 ||
            timeout_ms > kMaximumRequestTimeoutMs) {
            return fail("invalid-request", "--timeout-ms must be an integer from 1 to 300000",
                        json);
        }
    }
    const auto request_timeout = std::chrono::milliseconds(timeout_ms);

    auto connection = connect(json, request_timeout);
    if (!connection)
        return 1;
    if (verb == "instances") {
        auto data = instances(*connection, json);
        if (!data)
            return 1;
        if (json)
            std::cout << choc::json::toString(*data, false) << '\n';
        else {
            const auto values = (*data)["instances"];
            if (values.size() == 0)
                std::cout << "No live control instances.\n";
            for (std::uint32_t i = 0; i < values.size(); ++i)
                std::cout << values[i]["instance_id"].getString() << "  "
                          << values[i]["plugin_id"].getString() << "  "
                          << values[i]["profile"].getString() << '\n';
        }
        return 0;
    }
    if (verb == "status") {
        auto item = exact_instance(*connection, instance_id, json);
        if (!item)
            return 1;
        auto root = choc::value::createObject("");
        root.addMember("instance", *item);
        root.addMember("schema", choc::value::createString("pulp.control.status.v1"));
        if (explain) {
            auto terms = choc::value::createObject("");
            for (const auto term :
                 {"implemented", "built", "host_available", "activated", "session_live"})
                terms.addMember(term, choc::value::createString("satisfied"));
            terms.addMember("policy_eligible", choc::value::createString("evaluated_at_call_time"));
            terms.addMember("client_granted", choc::value::createString("grant_required"));
            root.addMember("permission_terms", terms);
        }
        if (json)
            std::cout << choc::json::toString(root, false) << '\n';
        else {
            std::cout << "Instance: " << instance_id << '\n'
                      << "Plugin: " << (*item)["plugin_id"].getString() << '\n'
                      << "Publication: " << (*item)["publication_id"].getString() << '\n';
            if (explain)
                std::cout << "Authority: live and built; policy is evaluated per call; a matching "
                             "grant is required.\n";
        }
        return 0;
    }
    if (verb == "grant-request" || verb == "revoke") {
        auto object = choc::value::createObject("");
        const auto command = verb;
        if (verb == "grant-request") {
            object.addMember("instance_id", choc::value::createString(instance_id));
            object.addMember("profile", choc::value::createString(profile));
        } else
            object.addMember("grant_id", choc::value::createString(grant_id));
        const auto result =
            connection->transport->manage(command, choc::json::toString(object, false));
        if (json) {
            auto root = choc::value::createObject("");
            root.addMember("data", choc::json::parse(result.data_json));
            root.addMember("explanation", choc::value::createString(result.explanation));
            root.addMember("status", choc::value::createString(result.status_id));
            std::cout << choc::json::toString(root, false) << '\n';
        } else
            std::cout << result.status_id
                      << (result.explanation.empty() ? "" : ": " + result.explanation) << '\n';
        return (result.status_id == "granted" || result.status_id == "revoked") ? 0 : 1;
    }
    if (verb == "artifact") {
        ControlClient client(*connection->transport);
        const auto negotiation =
            client.negotiate({.versions = {kControlProtocolVersion, kControlProtocolVersion},
                              .mandatory_features = {"artifacts", "receipts"}});
        if (!negotiation.response ||
            negotiation.response->status != ControlNegotiationStatus::Accepted)
            return fail(negotiation.error_code.empty() ? "negotiation-failed"
                                                       : negotiation.error_code,
                        negotiation.explanation, json);
        using pulp::runtime::detail::DurableFileCommitOutcome;
        using pulp::runtime::detail::DurableFileReplacement;
        auto replacement = DurableFileReplacement::create(output);
        if (!replacement)
            return fail("io-error", "cannot create a temporary artifact output", json);
        std::uint64_t offset = 0;
        ControlArtifactReadResult result;
        do {
            result = connection->transport->read_artifact(
                artifact_id, offset, kControlMaximumArtifactReadBytes, std::chrono::seconds(3));
            if (result.status != ControlArtifactStatus::Read)
                return fail(std::string(control_artifact_status_id(result.status)),
                            result.explanation, json);
            if (!replacement->write_all(std::span<const std::uint8_t>{result.bytes}))
                return fail("io-error", "artifact output write failed", json);
            offset += result.bytes.size();
        } while (!result.eof);
        switch (replacement->commit()) {
        case DurableFileCommitOutcome::ReplacedDurably:
            break;
        case DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
            return fail("io-error",
                        "artifact output was replaced but its directory could not be synced",
                        json);
        case DurableFileCommitOutcome::NotReplaced:
            return fail("io-error", "artifact output could not be atomically replaced", json);
        }
        if (json) {
            auto root = choc::value::createObject("");
            root.addMember("artifact_id", choc::value::createString(artifact_id));
            root.addMember("byte_size",
                           choc::value::createInt64(static_cast<std::int64_t>(offset)));
            root.addMember("output", choc::value::createString(output));
            root.addMember("schema", choc::value::createString("pulp.control.artifact.v1"));
            std::cout << choc::json::toString(root, false) << '\n';
        } else
            std::cout << "Retrieved " << artifact_id << " to " << output << " (" << offset
                      << " bytes)\n";
        return 0;
    }

    auto item = exact_instance(*connection, instance_id, json);
    if (!item)
        return 1;
    const auto operation =
        verb == "watch" && (positional == "telemetry" || positional == "telemetry/subscribe")
            ? std::string{"dev.pulp.telemetry/subscribe@1"}
            : positional;
    if (operation.empty())
        return fail("invalid-request", verb + " requires OPERATION or RESOURCE", json);
    const auto* descriptor = resolve_control_operation(operation, 1);
    if (!descriptor)
        return fail("invalid-request", "unknown operation: " + operation, json);
    ControlJsonSchemaDiagnostics diagnostics;
    if (!validate_control_json_schema(params, descriptor->input_schema_json, &diagnostics))
        return fail("invalid-request", diagnostics.explanation, json);
    if (grant_id.empty()) {
        auto grant_params = choc::value::createObject("");
        grant_params.addMember("instance_id", choc::value::createString(instance_id));
        grant_params.addMember(
            "profile", choc::value::createString(profile.empty() ? "inspect-readonly" : profile));
        const auto issued = connection->transport->manage(
            "grant-request", choc::json::toString(grant_params, false));
        if (issued.status_id != "granted")
            return fail(issued.status_id, issued.explanation, json);
        try {
            const auto data = choc::json::parse(issued.data_json);
            if (!data.isObject() || !data.hasObjectMember("grant_id") ||
                !data["grant_id"].isString())
                return fail("malformed-response", "broker grant response omitted grant_id", json);
            grant_id = std::string(data["grant_id"].getString());
        } catch (...) {
            return fail("malformed-response", "broker grant response was invalid", json);
        }
    }
    ControlClient client(*connection->transport);
    const auto negotiation =
        client.negotiate({.versions = {kControlProtocolVersion, kControlProtocolVersion},
                          .mandatory_features = {"receipts"},
                          .optional_features = {"artifacts", "progress"}});
    if (!negotiation.response || negotiation.response->status != ControlNegotiationStatus::Accepted)
        return fail(negotiation.error_code.empty() ? "negotiation-failed" : negotiation.error_code,
                    negotiation.explanation, json);
    const auto request_id = token("cli-request-");
    const auto idempotency = token("cli-idempotency-");
    if (!request_id || !idempotency)
        return fail("entropy-unavailable", "request entropy is unavailable", json);
    ControlRequestEnvelope request{
        .request_id = *request_id,
        .client_id = connection->client_id,
        .registration_id = std::string((*item)["registration_id"].getString()),
        .grant_id = grant_id,
        .instance_generation = std::string((*item)["publication_id"].getString()),
        .operation_id = operation,
        .operation_version = 1,
        .idempotency_key = *idempotency,
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            timeout_ms,
        .params_json = params};
    request.request_hash = control_request_hash(request).value_or("");
    const auto result = client.request(request, request_timeout);
    if (!result.response)
        return fail(result.error_code, result.explanation, json);
    const auto& receipt = *result.response;
    auto root = choc::value::createObject("");
    root.addMember("detail", choc::json::parse(receipt.detail_json));
    root.addMember("explanation", choc::value::createString(receipt.explanation));
    root.addMember("operation_id", choc::value::createString(receipt.operation_id));
    root.addMember("receipt_id", choc::value::createString(receipt.receipt_id));
    root.addMember("schema", choc::value::createString("pulp.control.receipt.v1"));
    root.addMember("state", choc::value::createString(control_receipt_state_id(receipt.state)));
    if (json)
        std::cout << choc::json::toString(root, false) << '\n';
    else
        std::cout << receipt.operation_id << ": " << control_receipt_state_id(receipt.state)
                  << " (receipt " << receipt.receipt_id << ")\n";
    return receipt.state == ControlReceiptState::Completed ? 0 : 1;
}
