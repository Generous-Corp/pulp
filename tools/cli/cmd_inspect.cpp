// cmd_inspect.cpp — static metadata, artifact audit, and the private canonical Trace bridge

#include "cli_common.hpp"
#include "inspector_shipping_report.hpp"

#include <pulp/inspect/control_inspector_client.hpp>
#include <pulp/inspect/protocol.hpp>

#include <chrono>
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

void print_help() {
    std::cout << "pulp inspect — offline artifact audit\n\n"
                 "Usage: pulp inspect profiles [--json]  (deprecated; use `pulp control profiles`; "
                 "removed Pulp 0.800.0 on 2026-10-01)\n"
                 "       pulp inspect audit ARTIFACT [--json]\n\n"
                 "Live discovery, raw Inspector calls, and mutations are not exposed.\n";
}

} // namespace

int cmd_inspect(const std::vector<std::string>& args) {
    if (!args.empty() && args.front() == "profiles") {
        std::vector<std::string> canonical_args{"profiles"};
        canonical_args.insert(canonical_args.end(), args.begin() + 1, args.end());
        std::cerr << "Warning: `pulp inspect profiles` is deprecated; use `pulp control profiles`. "
                     "It will be removed in Pulp 0.800.0 on 2026-10-01.\n";
        return cmd_control(canonical_args);
    }
    std::string verb;
    std::string command;
    std::string params = "{}";
    bool params_provided = false;
    bool json_output = false;

    std::size_t first_option = 0;
    if (!args.empty() && !args.front().starts_with("-")) {
        verb = args.front();
        first_option = 1;
        if (verb != "profiles" && verb != "audit") {
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
                             "Verify the manifest, profile, digest, and linked control surfaces "
                             "without activating the artifact.\n";
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
        if (arg == "--command") {
            if (!require_arg_value(args, index, "--command", command))
                return 2;
        } else if (arg == "--params") {
            if (!require_arg_value(args, index, "--params", params))
                return 2;
            params_provided = true;
        } else if (arg == "--json") {
            json_output = true;
        } else {
            std::cerr << "Error: unknown inspect argument: " << arg << "\n"
                      << "Run `pulp inspect --help` for usage.\n";
            return 2;
        }
    }

    if (!command.empty()) {
        if (!verb.empty() || json_output) {
            std::cerr << "Error: the private canonical Trace bridge does not accept a named "
                         "inspect command or --json\n";
            return 2;
        }
        if (command != methods::kTraceStartSession && command != methods::kTraceStopSession) {
            std::cerr << "Error: inspect --command accepts only Trace.startSession or "
                         "Trace.stopSession through canonical control\n";
            return 2;
        }
        auto opener = make_installed_inspector_control_session_opener(current_executable_path());
        const auto result = request_control_inspector(*opener, command, params,
                                                      std::chrono::seconds(3));
        if (!result.succeeded()) {
            print_error(result.response);
            return 1;
        }
        std::cout << result.response.params_json << "\n";
        return 0;
    }

    if (params_provided) {
        std::cerr << "Error: --params requires --command\n";
        return 2;
    }
    if (verb.empty()) {
        print_help();
        return 2;
    }

    // `profiles` returned above through the canonical control command. All remaining
    // public inspect verbs are deliberately offline-only.
    print_help();
    return 2;
}
