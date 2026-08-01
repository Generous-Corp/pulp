// cmd_inspect.cpp — CLI orchestration for authenticated inspector sessions

#include "cmd_inspect_support.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace pulp::cli::inspect_detail;

int cmd_inspect(const std::vector<std::string>& args) {
    std::string action;
    InspectorClientSelection selection;
    std::string command;
    std::string params = "{}";
    std::string output_file;
    std::string parameter_id_text;
    std::string parameter_value_text;
    bool params_provided = false;
    bool normalized = false;
    bool json = false;
    for (const auto& arg : args) {
        if (arg == "--json") {
            json = true;
            break;
        }
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "pulp inspect — authenticated client for explicitly enabled sessions\n\n"
                << "Usage: pulp inspect [profiles|list] [options]\n"
                << "       pulp inspect [capabilities|doctor] --session ID "
                   "--instance ID --publication ID [options]\n"
                << "       pulp inspect set-parameter --id ID --value VALUE "
                   "--session ID --instance ID --publication ID [options]\n"
                << "       pulp inspect --command METHOD --session ID "
                   "--instance ID --publication ID [options]\n"
                << "       pulp inspect --session ID --instance ID "
                   "--publication ID [options]\n\n"
                << "Options:\n"
                << "  --json            Emit stable machine-readable JSON\n"
                << "  --session ID      Select the exact live session\n"
                << "  --instance ID     Select the exact instance within a session\n"
                << "  --publication ID  Pin one non-reusable publication generation\n"
                << "  --host HOST       Filter discovery by host (loopback only)\n"
                << "  --port PORT       Filter discovery by port; never bypasses auth\n"
                << "  --id ID           Numeric parameter ID for set-parameter\n"
                << "  --value VALUE     Finite parameter value for set-parameter\n"
                << "  --normalized      Interpret --value in normalized [0,1] domain\n"
                << "  --command METHOD  Send one command and print its result\n"
                << "  --params JSON     JSON params for --command (default: {})\n"
                << "  --output FILE     Write the one-shot result to FILE\n\n"
                << "Normal launches do not start an inspector endpoint. A standalone\n"
                << "must be explicitly launched with an inspector profile. Discovery is\n"
                << "ephemeral and every connection proves possession of its owner-private\n"
                << "session credential. Runtime.evaluate additionally requires its\n"
                << "separate build and runtime opt-in.\n";
            return 0;
        }
        if (arg == "profiles" || arg == "list" ||
            arg == "capabilities" || arg == "doctor" ||
            arg == "set-parameter") {
            if (!action.empty()) {
                print_cli_error(json, "invalid_arguments",
                                "inspect accepts one subcommand");
                return 2;
            }
            action = arg;
        } else if (arg == "--json") {
            continue;
        } else if (arg == "--host") {
            if (!require_arg_value(
                    args, index, "--host", selection.host, json))
                return 2;
        } else if (arg == "--port") {
            std::string text;
            if (!require_arg_value(args, index, "--port", text, json))
                return 2;
            if (!parse_port(text, selection.port)) {
                print_cli_error(json, "invalid_arguments",
                                "invalid --port value: " + text);
                return 2;
            }
        } else if (arg == "--session") {
            if (!require_arg_value(args, index, "--session",
                                   selection.session_id, json))
                return 2;
        } else if (arg == "--instance") {
            if (!require_arg_value(args, index, "--instance",
                                   selection.instance_id, json))
                return 2;
        } else if (arg == "--publication") {
            if (!require_arg_value(
                    args, index, "--publication", selection.publication_id,
                    json))
                return 2;
        } else if (arg == "--id") {
            if (!require_arg_value(
                    args, index, "--id", parameter_id_text, json))
                return 2;
        } else if (arg == "--value") {
            if (!require_arg_value(
                    args, index, "--value", parameter_value_text, json))
                return 2;
        } else if (arg == "--normalized") {
            normalized = true;
        } else if (arg == "--command") {
            if (!require_arg_value(
                    args, index, "--command", command, json))
                return 2;
        } else if (arg == "--params") {
            if (!require_arg_value(args, index, "--params", params, json))
                return 2;
            params_provided = true;
        } else if (arg == "--output") {
            if (!require_arg_value(
                    args, index, "--output", output_file, json))
                return 2;
        } else {
            print_cli_error(json, "invalid_arguments",
                            "unknown inspect argument: " + arg);
            return 2;
        }
    }

    if (!action.empty() && !command.empty()) {
        print_cli_error(
            json, "invalid_arguments",
            "inspect subcommands cannot be combined with --command");
        return 2;
    }
    if (!output_file.empty() && command.empty()) {
        print_cli_error(json, "invalid_arguments",
                        "--output requires --command");
        return 2;
    }
    if (params_provided && command.empty()) {
        print_cli_error(json, "invalid_arguments",
                        "--params requires --command");
        return 2;
    }
    if (json && action.empty() && command.empty()) {
        print_cli_error(
            json, "invalid_arguments",
            "--json requires a workflow subcommand or --command");
        return 2;
    }
    if (action == "profiles" &&
        (!selection.host.empty() || selection.port != 0 ||
         !selection.session_id.empty() || !selection.instance_id.empty() ||
         !selection.publication_id.empty())) {
        print_cli_error(json, "invalid_arguments",
                        "profiles does not accept live-session selectors");
        return 2;
    }
    const bool parameter_options_present = !parameter_id_text.empty() ||
                                           !parameter_value_text.empty() ||
                                           normalized;
    if (parameter_options_present && action != "set-parameter") {
        print_cli_error(json, "invalid_arguments",
                        "--id, --value, and --normalized require set-parameter");
        return 2;
    }
    std::int64_t parameter_id = 0;
    double parameter_value = 0.0;
    if (action == "set-parameter") {
        if (parameter_id_text.empty() || parameter_value_text.empty()) {
            print_cli_error(json, "invalid_arguments",
                            "set-parameter requires --id and --value");
            return 2;
        }
        if (!parse_parameter_id(parameter_id_text, parameter_id)) {
            print_cli_error(json, "invalid_arguments",
                            "invalid --id value: " + parameter_id_text);
            return 2;
        }
        if (!parse_parameter_value(parameter_value_text, parameter_value)) {
            print_cli_error(json, "invalid_arguments",
                            "invalid --value: " + parameter_value_text);
            return 2;
        }
        if (normalized && (parameter_value < 0.0 || parameter_value > 1.0)) {
            print_cli_error(json, "invalid_arguments",
                            "normalized --value must be from 0 to 1");
            return 2;
        }
    }
    if (action == "profiles") {
        const auto payload = profiles_json();
        if (json) {
            std::cout << payload << "\n";
        } else {
            std::cout << "Inspector profiles:\n";
            for (const auto profile : {InspectorProfile::Off,
                                       InspectorProfile::Observe,
                                       InspectorProfile::Develop,
                                       InspectorProfile::Custom}) {
                std::cout << "  " << profile_id(profile);
                const auto capabilities = profile_capabilities(profile);
                if (profile == InspectorProfile::Custom)
                    std::cout << " (explicit capability list)";
                else
                    std::cout << " (" << capabilities.size()
                              << " capabilities)";
                std::cout << "\n";
            }
        }
        return 0;
    }

    InspectorClientFailure failure;
    if (action == "list") {
        const auto records = discover_inspector_sessions(selection, &failure);
        if (!failure.code.empty()) {
            print_failure(failure, json);
            return 2;
        }
        if (json) {
            auto payload = choc::value::createObject("");
            payload.addMember(
                "schemaVersion",
                choc::value::createString("pulp.inspect.sessions.v1"));
            auto sessions = choc::value::createArray(
                static_cast<std::uint32_t>(records.size()),
                [&records] (std::uint32_t index) {
                    return discovery_json(records[index]);
                });
            payload.addMember("sessions", std::move(sessions));
            std::cout << choc::json::toString(payload, false) << "\n";
        } else if (records.empty()) {
            std::cout << "No live inspector sessions.\n";
        } else {
            std::cout << "SESSION  INSTANCE  PUBLICATION  PLUGIN  PROFILE  ENDPOINT\n";
            for (const auto& record : records) {
                std::cout << record.session_id << "  " << record.instance_id
                          << "  " << record.publication_id << "  "
                          << record.plugin_id << "  "
                          << profile_id(record.profile) << "  "
                          << record.endpoint << "\n";
            }
        }
        return 0;
    }

    if (selection.session_id.empty() || selection.instance_id.empty() ||
        selection.publication_id.empty()) {
        print_cli_error(
            json, "invalid_arguments",
            "live inspect operations require --session, --instance, and "
            "--publication");
        return 2;
    }

    auto client = InspectorClientSession::connect(selection, &failure);
    if (!client) {
        print_failure(failure, json);
        return failure.code == "invalid_selector" ? 2 : 1;
    }
    const auto& selected = client->record();
    auto& status = command.empty() ? std::cout : std::cerr;
    if (!json)
        status << "Connected to inspector session " << selected.session_id
               << " (instance " << selected.instance_id << ")\n";

    if (action == "capabilities")
        command = std::string(methods::kSessionGetCapabilities);

    if (action == "set-parameter") {
        const auto response = client->set_parameter_typed(
            parameter_id, parameter_value, normalized);
        if (!response) {
            print_failure(response.failure, json);
            return 1;
        }
        const auto result = parse_json_object(
            response.response_json, methods::kStateSetParameter, json);
        if (!result)
            return 1;
        if (json) {
            auto payload = choc::value::createObject("");
            payload.addMember(
                "schemaVersion",
                choc::value::createString(
                    "pulp.inspect.set-parameter.v1"));
            payload.addMember("session", discovery_json(selected));
            payload.addMember("parameterId", parameter_id);
            payload.addMember("value", parameter_value);
            payload.addMember("normalized", normalized);
            payload.addMember("result", *result);
            std::cout << choc::json::toString(payload, false) << "\n";
        } else {
            std::cout << "Set parameter " << parameter_id << " to "
                      << parameter_value
                      << (normalized ? " (normalized)" : " (plain)")
                      << "\n";
        }
        return 0;
    }

    if (action == "doctor") {
        const auto capabilities = client->read_capabilities();
        if (!capabilities) {
            print_failure(capabilities.failure, json);
            return 1;
        }
        const auto context = client->read_agent_context();
        if (!context) {
            print_failure(context.failure, json);
            return 1;
        }
        const auto capability_policy = parse_json_object(
            capabilities.response_json, methods::kSessionGetCapabilities,
            json);
        const auto agent_context = parse_json_object(
            context.response_json, methods::kInspectorGetAgentContext, json);
        if (!capability_policy || !agent_context)
            return 1;
        if (json) {
            auto payload = choc::value::createObject("");
            payload.addMember(
                "schemaVersion",
                choc::value::createString("pulp.inspect.doctor.v1"));
            payload.addMember("ready", true);
            payload.addMember("session", discovery_json(selected));
            payload.addMember("capabilities", *capability_policy);
            payload.addMember("agentContext", *agent_context);
            std::cout << choc::json::toString(payload, false) << "\n";
        } else {
            std::cout << color_green() << "✓" << color_reset()
                      << " authenticated, capability policy available, "
                         "agent context available\n";
        }
        return 0;
    }

    if (!command.empty()) {
        if (action == "capabilities") {
            const auto response = client->read_capabilities();
            if (!response) {
                print_failure(response.failure, json);
                return 1;
            }
            auto response_json = response.response_json;
            const auto policy = parse_json_object(
                response_json, methods::kSessionGetCapabilities, json);
            if (!policy)
                return 1;
            if (json) {
                auto payload = choc::value::createObject("");
                payload.addMember(
                    "schemaVersion",
                    choc::value::createString(
                        "pulp.inspect.capabilities.v1"));
                payload.addMember("session", discovery_json(selected));
                payload.addMember("policy", *policy);
                response_json = choc::json::toString(payload, false);
            } else {
                std::cout << "Inspector capabilities for "
                          << selected.session_id << "/"
                          << selected.instance_id << "\n"
                          << "  publication: "
                          << selected.publication_id << "\n"
                          << "  profile: "
                          << profile_id(response.value->profile)
                          << "\n";
                print_capability_list(
                    response.value->available, "available");
                print_capability_list(
                    response.value->effective, "effective");
                return 0;
            }
            std::cout << response_json << "\n";
            return 0;
        }

        const auto response = client->request_controlled(command, params);
        if (response.is_error) {
            print_error(response, json);
            return 1;
        }
        auto response_json = response.params_json;
        if (command == methods::kSessionGetCapabilities) {
            response_json = attach_publication_id(
                std::move(response_json), selected.publication_id);
        }
        if (json && action.empty()) {
            try {
                (void)choc::json::parse(response_json);
            } catch (...) {
                print_cli_error(json, "invalid_response",
                                command + " returned invalid JSON");
                return 1;
            }
        }
        if (!output_file.empty()) {
            std::ofstream output(output_file, std::ios::trunc);
            if (!output || !(output << response_json)) {
                print_cli_error(json, "output_write_failed",
                                "could not write " + output_file);
                return 1;
            }
            if (json) {
                auto payload = choc::value::createObject("");
                payload.addMember(
                    "schemaVersion",
                    choc::value::createString("pulp.inspect.output.v1"));
                payload.addMember(
                    "outputFile", choc::value::createString(output_file));
                std::cout << choc::json::toString(payload, false) << "\n";
            } else {
                std::cout << "Written to " << output_file << "\n";
            }
        } else {
            std::cout << response_json << "\n";
        }
        return 0;
    }

    std::cout << "Inspector REPL. Enter METHOD [JSON_PARAMS], or 'quit'.\n\n";
    client->set_event_handler([](const InspectorMessage& event) {
        std::cout << color_cyan() << "\xe2\x86\x90" << color_reset() << " "
                  << event.method << "\n";
        if (!event.params_json.empty() && event.params_json != "{}")
            std::cout << "  " << event.params_json << "\n";
        std::cout.flush();
    });
    std::string line;
    while (true) {
        std::cout << color_bold() << "inspect> " << color_reset();
        std::cout.flush();
        if (!std::getline(std::cin, line))
            break;
        line = trim(line);
        if (line.empty())
            continue;
        if (line == "quit" || line == "exit" || line == "q")
            break;
        std::string method = line;
        std::string request_params = "{}";
        if (const auto separator = line.find(' ');
            separator != std::string::npos) {
            method = line.substr(0, separator);
            request_params = trim(line.substr(separator + 1));
        }
        const auto response = client->request_controlled(
            method, request_params);
        if (response.is_error) {
            std::cout << color_red() << "✗ [" << response.error_code << "] "
                      << response.params_json << color_reset() << "\n";
        } else {
            std::cout << color_green() << "✓" << color_reset() << " "
                      << response.params_json << "\n";
        }
    }
    return 0;
}
