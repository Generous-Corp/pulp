// cmd_inspect.cpp — read-only metadata for explicitly enabled inspector sessions

#include "inspector_shipping_report.hpp"

#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_inspector_client.hpp>
#include <pulp/inspect/discovery.hpp>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace pulp::inspect;

bool require_arg_value(const std::vector<std::string>& args, std::size_t& index, const char* flag,
                       std::string& output) {
    if (index + 1 >= args.size()) {
        std::cerr << "Error: " << flag << " requires a value\n";
        return false;
    }
    output = args[++index];
    if (output.empty()) {
        std::cerr << "Error: " << flag << " requires a non-empty value\n";
        return false;
    }
    return true;
}

void print_error(const InspectorMessage& response) {
    std::cerr << "Error";
    if (!response.error_code.empty())
        std::cerr << " [" << response.error_code << "]";
    std::cerr << ": " << response.params_json << "\n";
    if (!response.error_data_json.empty() && response.error_data_json != "{}")
        std::cerr << response.error_data_json << "\n";
}

std::string command_error_json(const InspectorMessage& response) {
    auto root = choc::value::createObject("");
    root.addMember("schemaVersion", choc::value::createInt32(1));
    root.addMember("ok", choc::value::createBool(false));
    auto error = choc::value::createObject("");
    error.addMember("code",
                    choc::value::createString(response.error_code.empty() ? "command_failed"
                                                                          : response.error_code));
    error.addMember("message", choc::value::createString(response.params_json));
    if (!response.error_data_json.empty() && response.error_data_json != "{}") {
        try {
            error.addMember("data", choc::json::parse(response.error_data_json));
        } catch (...) {
            error.addMember("data", choc::value::createString(response.error_data_json));
        }
    }
    root.addMember("error", error);
    return choc::json::toString(root, false);
}

std::string risk_id(InspectorCapabilityRisk risk) {
    switch (risk) {
    case InspectorCapabilityRisk::Observe:
        return "observe";
    case InspectorCapabilityRisk::Sensitive:
        return "sensitive";
    case InspectorCapabilityRisk::StreamingSensitive:
        return "streaming-sensitive";
    case InspectorCapabilityRisk::ResourceConsuming:
        return "resource-consuming";
    case InspectorCapabilityRisk::Control:
        return "control";
    case InspectorCapabilityRisk::HighRisk:
        return "high-risk";
    case InspectorCapabilityRisk::Critical:
        return "critical";
    case InspectorCapabilityRisk::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

choc::value::Value publication_json(const InspectorDiscoveryRecord& record) {
    auto value = choc::value::createObject("");
    value.addMember("sessionId", choc::value::createString(record.session_id));
    value.addMember("instanceId", choc::value::createString(record.instance_id));
    value.addMember("publicationId", choc::value::createString(record.publication_id));
    value.addMember("pluginId", choc::value::createString(record.plugin_id));
    value.addMember("profile", choc::value::createString(profile_id(record.profile)));
    value.addMember("endpoint", choc::value::createString(record.endpoint));
    value.addMember("protocolVersion", choc::value::createString(record.protocol_version));
    value.addMember("processId", choc::value::createInt64(record.process_id));
    value.addMember("expiresAtUnixMs", choc::value::createInt64(record.expires_at_unix_ms));
    return value;
}

std::string profiles_json() {
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
    return choc::json::toString(root, false);
}

std::string list_json(const std::vector<InspectorDiscoveryRecord>& records) {
    auto sessions = choc::value::createEmptyArray();
    for (const auto& record : records)
        sessions.addArrayElement(publication_json(record));
    auto root = choc::value::createObject("");
    root.addMember("schemaVersion", choc::value::createInt32(1));
    root.addMember("sessions", sessions);
    return choc::json::toString(root, false);
}

bool exact_target(std::string_view session_id, std::string_view instance_id,
                  std::string_view publication_id) {
    return !session_id.empty() && !instance_id.empty() && !publication_id.empty();
}

void print_help() {
    std::cout
        << "pulp inspect — read-only metadata for explicitly enabled sessions\n\n"
        << "Usage: pulp inspect <profiles|list|capabilities|doctor> [options]\n"
        << "       pulp inspect audit ARTIFACT [--json]\n\n"
        << "Options:\n"
        << "  --session ID      Select the exact live session (capabilities only)\n"
        << "  --instance ID     Select the exact instance (capabilities only)\n"
        << "  --publication ID  Pin one publication generation (capabilities only)\n"
        << "  --json            Stable JSON output\n\n"
        << "Raw inspector calls and live mutations are not exposed by this command.\n";
}

} // namespace

int cmd_inspect(const std::vector<std::string>& args) {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    bool json_output = false;
    std::string verb;

    std::size_t first_option = 0;
    if (!args.empty() && !args.front().starts_with("-")) {
        verb = args.front();
        first_option = 1;
        if (verb != "profiles" && verb != "list" && verb != "capabilities" &&
            verb != "doctor" && verb != "audit") {
            std::cerr << "Error: unknown inspect command: " << verb << "\n";
            return 2;
        }
    }

    if (verb == "audit") {
        std::filesystem::path artifact;
        for (std::size_t index = first_option; index < args.size(); ++index) {
            if (args[index] == "--json") {
                json_output = true;
            } else if (args[index] == "--help" || args[index] == "-h") {
                std::cout << "Usage: pulp inspect audit ARTIFACT [--json]\n"
                             "Verify the manifest, profile, digest, and linked "
                             "control surfaces without activating the artifact.\n";
                return 0;
            } else if (args[index].starts_with("-")) {
                std::cerr << "Error: unknown inspect audit argument: " << args[index] << "\n";
                return 2;
            } else if (!artifact.empty()) {
                std::cerr << "Error: inspect audit accepts exactly one artifact\n";
                return 2;
            } else {
                artifact = args[index];
            }
        }
        if (artifact.empty()) {
            std::cerr << "Error: inspect audit requires ARTIFACT\n";
            return 2;
        }
        const auto report = pulp::cli::inspector_shipping::audit_artifact(artifact);
        std::cout << (json_output ? pulp::cli::inspector_shipping::audit_json(report) + "\n"
                                  : pulp::cli::inspector_shipping::audit_human(report));
        return report.complete ? 0 : 1;
    }

    for (std::size_t index = first_option; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        if (arg == "--session") {
            if (!require_arg_value(args, index, "--session", session_id))
                return 2;
        } else if (arg == "--instance") {
            if (!require_arg_value(args, index, "--instance", instance_id))
                return 2;
        } else if (arg == "--publication") {
            if (!require_arg_value(args, index, "--publication", publication_id))
                return 2;
        } else if (arg == "--json") {
            json_output = true;
        } else {
            std::cerr << "Error: unknown inspect argument: " << arg << "\n"
                      << "Run `pulp inspect --help` for usage.\n";
            return 2;
        }
    }

    if (verb.empty()) {
        print_help();
        return 2;
    }
    if ((!session_id.empty() || !instance_id.empty() || !publication_id.empty()) &&
        verb != "capabilities") {
        std::cerr << "Error: exact session options apply only to capabilities\n";
        return 2;
    }
    const bool canonical_trace_lifecycle =
        command == pulp::inspect::methods::kTraceStartSession ||
        command == pulp::inspect::methods::kTraceStopSession;
    if (!canonical_trace_lifecycle && command.starts_with("Trace.")) {
        std::cerr << "Error: legacy Trace.* Inspector authority was removed; use "
                     "`pulp trace start|stop` through canonical control or offline trace tools\n";
        return 2;
    }
    if (canonical_trace_lifecycle &&
        (!host.empty() || port != 0 || !session_id.empty() || !instance_id.empty() ||
         !publication_id.empty())) {
        std::cerr << "Error: canonical trace lifecycle does not accept legacy "
                     "--host/--port/--session/--instance/--publication selectors\n";
        return 2;
    }
    if (!publication_id.empty() && (session_id.empty() || instance_id.empty())) {
        std::cerr << "Error: --publication requires --session and --instance\n";
        return 2;
    }

    if (verb == "profiles") {
        if (json_output) {
            std::cout << profiles_json() << "\n";
        } else {
            for (const auto profile :
                 {InspectorProfile::Off, InspectorProfile::Observe, InspectorProfile::Develop}) {
                std::cout << profile_id(profile) << "\n";
                for (const auto capability : profile_capabilities(profile))
                    std::cout << "  " << capability_id(capability) << "\n";
            }
        }
        return 0;
    }

    if (canonical_trace_lifecycle) {
        const auto result = request_control_inspector(command, params, std::chrono::seconds(3));
        if (!result.succeeded()) {
            print_error(result.response);
            return 1;
        }
        std::cout << result.response.params_json << "\n";
        return 0;
    }

    InspectorDiscoveryReader discovery;
    std::string discovery_issue;
    const auto records = discovery.list(&discovery_issue);

    if (verb == "list") {
        if (!discovery_issue.empty()) {
            if (json_output) {
                auto root = choc::value::createObject("");
                root.addMember("schemaVersion", choc::value::createInt32(1));
                root.addMember("ok", choc::value::createBool(false));
                auto error = choc::value::createObject("");
                error.addMember("code", choc::value::createString("discovery_unavailable"));
                error.addMember("message", choc::value::createString(discovery_issue));
                error.addMember("runtimeDirectory",
                                choc::value::createString(discovery.runtime_directory().string()));
                root.addMember("error", error);
                std::cout << choc::json::toString(root, false) << "\n";
            } else {
                std::cerr << "Error: " << discovery_issue << "\n";
            }
            return 1;
        }
        if (json_output) {
            std::cout << list_json(records) << "\n";
        } else if (records.empty()) {
            std::cout << "No live inspector sessions.\n";
        } else {
            for (const auto& record : records) {
                std::cout << record.session_id << "  " << record.instance_id << "  "
                          << record.publication_id << "\n"
                          << "  " << record.plugin_id << "  " << profile_id(record.profile) << "  "
                          << record.endpoint << "\n";
            }
        }
        return 0;
    }

    if (verb == "doctor") {
        const bool ok = discovery_issue.empty();
        if (json_output) {
            auto root = choc::value::createObject("");
            root.addMember("schemaVersion", choc::value::createInt32(1));
            root.addMember("ok", choc::value::createBool(ok));
            root.addMember("runtimeDirectory",
                           choc::value::createString(discovery.runtime_directory().string()));
            root.addMember("sessionCount",
                           choc::value::createInt64(static_cast<std::int64_t>(records.size())));
            auto issues = choc::value::createEmptyArray();
            if (!ok)
                issues.addArrayElement(choc::value::createString(discovery_issue));
            root.addMember("issues", issues);
            std::cout << choc::json::toString(root, false) << "\n";
        } else {
            std::cout << "Inspector discovery: " << (ok ? "ok" : "error") << "\n"
                      << "Runtime directory: " << discovery.runtime_directory().string() << "\n"
                      << "Live sessions: " << records.size() << "\n";
            if (!ok)
                std::cout << "Issue: " << discovery_issue << "\n";
        }
        return ok ? 0 : 1;
    }

    if (!exact_target(session_id, instance_id, publication_id)) {
        std::cerr << "Error: capabilities requires --session, --instance, "
                     "and --publication from `pulp inspect list`\n";
        return 2;
    }
    const auto result = request_inspector(std::string(methods::kSessionGetCapabilities), "{}",
                                          {session_id, instance_id, publication_id},
                                          std::chrono::seconds(3), discovery);
    if (!result.succeeded()) {
        if (json_output)
            std::cout << command_error_json(result.response) << "\n";
        else
            print_error(result.response);
        return 1;
    }
    try {
        auto response = choc::json::parse(result.response.params_json);
        const auto string_array = [](const auto& value) {
            if (!value.isArray())
                return false;
            for (std::uint32_t i = 0; i < value.size(); ++i) {
                if (!value[i].isString())
                    return false;
            }
            return true;
        };
        if (!response.isObject() || !response.hasObjectMember("sessionId") ||
            !response["sessionId"].isString() || !response.hasObjectMember("profile") ||
            !response["profile"].isString() || !response.hasObjectMember("available") ||
            !string_array(response["available"]) || !response.hasObjectMember("effective") ||
            !string_array(response["effective"])) {
            throw 0;
        }
        response.addMember("publicationId", choc::value::createString(publication_id));
        response.addMember("schemaVersion", choc::value::createInt32(1));
        if (json_output) {
            std::cout << choc::json::toString(response, false) << "\n";
        } else {
            std::cout << "Session " << response["sessionId"].getString() << " ("
                      << response["profile"].getString() << ")\nAvailable:\n";
            for (std::uint32_t i = 0; i < response["available"].size(); ++i) {
                const auto id = response["available"][i].getString();
                const auto capability = capability_from_id(id);
                std::cout << "  " << id;
                if (capability)
                    std::cout << "  " << risk_id(capability_risk(*capability));
                std::cout << "\n";
            }
            std::cout << "Effective:\n";
            for (std::uint32_t i = 0; i < response["effective"].size(); ++i) {
                const auto id = response["effective"][i].getString();
                const auto capability = capability_from_id(id);
                std::cout << "  " << id;
                if (capability)
                    std::cout << "  " << risk_id(capability_risk(*capability));
                std::cout << "\n";
            }
        }
        return 0;
    } catch (...) {
        constexpr auto message = "Inspector capabilities response is malformed";
        if (json_output) {
            auto root = choc::value::createObject("");
            root.addMember("schemaVersion", choc::value::createInt32(1));
            root.addMember("ok", choc::value::createBool(false));
            auto error = choc::value::createObject("");
            error.addMember("code", choc::value::createString("invalid_response"));
            error.addMember("message", choc::value::createString(message));
            root.addMember("error", error);
            std::cout << choc::json::toString(root, false) << "\n";
        } else {
            std::cerr << "Error [invalid_response]: " << message << "\n";
        }
        return 1;
    }
}
