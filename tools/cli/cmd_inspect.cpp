// cmd_inspect.cpp — authenticated client for explicitly enabled inspector sessions

#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

static std::string inspect_trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool g_inspect_color = true;
static std::string ic_bold() {
    return g_inspect_color ? "\033[1m" : "";
}
static std::string ic_cyan() {
    return g_inspect_color ? "\033[36m" : "";
}
static std::string ic_green() {
    return g_inspect_color ? "\033[32m" : "";
}
static std::string ic_red() {
    return g_inspect_color ? "\033[31m" : "";
}
static std::string ic_reset() {
    return g_inspect_color ? "\033[0m" : "";
}

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

bool parse_port(std::string_view text, int& output) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0 || value > 65535)
        return false;
    output = value;
    return true;
}

std::string endpoint_host(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    return separator == std::string_view::npos ? std::string(endpoint)
                                               : std::string(endpoint.substr(0, separator));
}

int endpoint_port(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    int value = 0;
    if (separator == std::string_view::npos || !parse_port(endpoint.substr(separator + 1), value))
        return 0;
    return value;
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

std::string attach_publication_id(std::string response_json, std::string_view publication_id) {
    try {
        auto value = choc::json::parse(response_json);
        if (!value.isObject())
            return response_json;
        value.addMember("publicationId", choc::value::createString(publication_id));
        return choc::json::toString(value, false);
    } catch (...) {
        return response_json;
    }
}

std::string risk_id(InspectorCapabilityRisk risk) {
    switch (risk) {
    case InspectorCapabilityRisk::Observe:
        return "observe";
    case InspectorCapabilityRisk::Control:
        return "control";
    case InspectorCapabilityRisk::HighRisk:
        return "high-risk";
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

} // namespace

int cmd_inspect(const std::vector<std::string>& args) {
    std::string host;
    int port = 0;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string command;
    std::string params = "{}";
    std::string output_file;
    bool params_provided = false;
    bool json_output = false;
    std::string verb;

    std::size_t first_option = 0;
    if (!args.empty() && !args.front().starts_with("-")) {
        verb = args.front();
        first_option = 1;
        if (verb != "profiles" && verb != "list" && verb != "capabilities" && verb != "doctor") {
            std::cerr << "Error: unknown inspect command: " << verb << "\n";
            return 2;
        }
    }

    for (std::size_t index = first_option; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            std::cout << "pulp inspect — authenticated client for explicitly enabled sessions\n\n"
                      << "Usage: pulp inspect <profiles|list|capabilities|doctor> [options]\n"
                      << "       pulp inspect [legacy one-shot options]\n\n"
                      << "Options:\n"
                      << "  --session ID      Select the exact live session\n"
                      << "  --instance ID     Select the exact instance within a session\n"
                      << "  --publication ID  Pin one non-reusable publication generation\n"
                      << "  --host HOST       Filter discovery by host (loopback only)\n"
                      << "  --port PORT       Filter discovery by port; never bypasses auth\n"
                      << "  --command METHOD  Send one command and print its result\n"
                      << "  --params JSON     JSON params for --command (default: {})\n"
                      << "  --output FILE     Write the one-shot result to FILE\n"
                      << "  --json            Stable JSON output for named commands\n\n"
                      << "Normal launches do not start an inspector endpoint. A standalone\n"
                      << "must be explicitly launched with an inspector profile. Discovery is\n"
                      << "ephemeral and every connection proves possession of its owner-private\n"
                      << "session credential. Runtime.evaluate additionally requires its\n"
                      << "separate build and runtime opt-in.\n";
            return 0;
        }
        if (arg == "--host") {
            if (!require_arg_value(args, index, "--host", host))
                return 2;
        } else if (arg == "--port") {
            std::string text;
            if (!require_arg_value(args, index, "--port", text))
                return 2;
            if (!parse_port(text, port)) {
                std::cerr << "Error: invalid --port value: " << text << "\n";
                return 2;
            }
        } else if (arg == "--session") {
            if (!require_arg_value(args, index, "--session", session_id))
                return 2;
        } else if (arg == "--instance") {
            if (!require_arg_value(args, index, "--instance", instance_id))
                return 2;
        } else if (arg == "--publication") {
            if (!require_arg_value(args, index, "--publication", publication_id))
                return 2;
        } else if (arg == "--command") {
            if (!require_arg_value(args, index, "--command", command))
                return 2;
        } else if (arg == "--params") {
            if (!require_arg_value(args, index, "--params", params))
                return 2;
            params_provided = true;
        } else if (arg == "--output") {
            if (!require_arg_value(args, index, "--output", output_file))
                return 2;
        } else if (arg == "--json") {
            json_output = true;
        } else {
            std::cerr << "Error: unknown inspect argument: " << arg << "\n"
                      << "Run `pulp inspect --help` for usage.\n";
            return 2;
        }
    }

    if (!output_file.empty() && command.empty()) {
        std::cerr << "Error: --output requires --command\n";
        return 2;
    }
    if (params_provided && command.empty()) {
        std::cerr << "Error: --params requires --command\n";
        return 2;
    }
    if (!publication_id.empty() && (session_id.empty() || instance_id.empty())) {
        std::cerr << "Error: --publication requires --session and --instance\n";
        return 2;
    }
    if (!host.empty() && host != "127.0.0.1" && host != "localhost") {
        std::cerr << "Error: inspector sessions are loopback-only\n";
        return 2;
    }
    if (host == "localhost")
        host = "127.0.0.1";

    if (!verb.empty() && (!command.empty() || params_provided || !output_file.empty())) {
        std::cerr << "Error: named inspect commands do not accept legacy "
                     "--command/--params/--output options\n";
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

    InspectorDiscoveryReader discovery;
    std::string discovery_issue;
    auto records = discovery.list(&discovery_issue);
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const auto& record) {
                                     return (!host.empty() &&
                                             endpoint_host(record.endpoint) != host) ||
                                            (port != 0 && endpoint_port(record.endpoint) != port);
                                 }),
                  records.end());

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

    if (verb == "capabilities") {
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
                          << response["profile"].getString() << ")\n"
                          << "Available:\n";
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

    std::string selection_error;
    const auto selected = select_inspector_session(records, session_id, instance_id, publication_id,
                                                   &selection_error);
    if (!selected) {
        std::cerr << "Error: " << selection_error;
        if (!host.empty() || port != 0 || !session_id.empty() || !instance_id.empty() ||
            !publication_id.empty()) {
            std::cerr << " (requested";
            if (!host.empty())
                std::cerr << " host " << host;
            if (port != 0)
                std::cerr << " port " << port;
            if (!session_id.empty())
                std::cerr << " session " << session_id;
            if (!instance_id.empty())
                std::cerr << " instance " << instance_id;
            if (!publication_id.empty())
                std::cerr << " publication " << publication_id;
            std::cerr << ")";
        }
        std::cerr << "\n";
        return 1;
    }

    auto& status = command.empty() ? std::cout : std::cerr;
    status << "Found inspector session " << selected->session_id << "\n"
           << "Connecting to " << selected->endpoint << "...\n";
    InspectorClient client;
    if (command.empty()) {
        client.set_event_handler([](const InspectorMessage& event) {
            std::cout << ic_cyan() << "← " << event.method << ic_reset() << "\n";
            if (!event.params_json.empty() && event.params_json != "{}")
                std::cout << "  " << event.params_json << "\n";
        });
    }
    if (!client.connect(*selected, discovery)) {
        std::cerr << "Error: authentication or connection failed for session "
                  << selected->session_id << "\n";
        return 1;
    }
    status << "  " << ic_green() << "✓" << ic_reset() << " Connected to inspector\n";

    if (!command.empty()) {
        const auto* descriptor = find_inspector_method(command);
        const bool needs_controller =
            descriptor && descriptor->kind == InspectorMethodKind::Request &&
            capability_requires_controller_lease(descriptor->capability) &&
            command != methods::kSessionAcquireController &&
            command != methods::kSessionRenewController &&
            command != methods::kSessionReleaseController;
        bool controller_acquired = false;
        if (needs_controller) {
            const auto lease = client.request(std::string(methods::kSessionAcquireController));
            if (lease.is_error) {
                print_error(lease);
                return 1;
            }
            controller_acquired = true;
        }
        const auto response = client.request(command, params);
        if (controller_acquired) {
            // Release explicitly before this one-shot connection exits. EOF is
            // still the fallback if the transport is already broken, but a
            // following CLI/MCP mutation must not race asynchronous EOF
            // processing for controller ownership.
            (void)client.request(std::string(methods::kSessionReleaseController));
        }
        if (response.is_error) {
            print_error(response);
            return 1;
        }
        auto response_json = response.params_json;
        if (command == methods::kSessionGetCapabilities) {
            response_json =
                attach_publication_id(std::move(response_json), selected->publication_id);
        }
        if (!output_file.empty()) {
            std::ofstream output(output_file, std::ios::trunc);
            if (!output || !(output << response_json)) {
                std::cerr << "Error: could not write " << output_file << "\n";
                return 1;
            }
            std::cout << "Written to " << output_file << "\n";
        } else {
            std::cout << response_json << "\n";
        }
        return 0;
    }

    std::cout << "Inspector REPL. Enter METHOD [JSON_PARAMS], or 'quit'.\n\n";
    std::string line;
    while (true) {
        std::cout << ic_bold() << "inspect> " << ic_reset();
        std::cout.flush();
        if (!std::getline(std::cin, line))
            break;
        line = inspect_trim(line);
        if (line.empty())
            continue;
        if (line == "quit" || line == "exit" || line == "q")
            break;
        std::string method = line;
        std::string request_params = "{}";
        if (const auto separator = line.find(' '); separator != std::string::npos) {
            method = line.substr(0, separator);
            request_params = inspect_trim(line.substr(separator + 1));
        }
        const auto response = client.request(method, request_params);
        if (response.is_error) {
            std::cout << ic_red() << "✗ [" << response.error_code << "] " << response.params_json
                      << ic_reset() << "\n";
        } else {
            std::cout << ic_green() << "✓" << ic_reset() << " " << response.params_json << "\n";
        }
    }
    return 0;
}
