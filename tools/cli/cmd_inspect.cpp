// cmd_inspect.cpp — static metadata, artifact audit, and the private canonical Trace bridge

#include "inspector_shipping_report.hpp"

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_inspector_client.hpp>
#include <pulp/inspect/protocol.hpp>

#include <choc/text/choc_JSON.h>

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

void print_help() {
    std::cout << "pulp inspect — static metadata and offline artifact audit\n\n"
                 "Usage: pulp inspect profiles [--json]\n"
                 "       pulp inspect audit ARTIFACT [--json]\n\n"
                 "Live discovery, raw Inspector calls, and mutations are not exposed.\n";
}

} // namespace

int cmd_inspect(const std::vector<std::string>& args) {
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

    std::cerr << "Warning: `pulp inspect` is a compatibility facade; use `pulp control "
              << (verb == "audit" ? "audit" : "status") << "` for canonical control.\n";

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
        const auto result = request_control_inspector(command, params, std::chrono::seconds(3));
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
