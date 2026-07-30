// cmd_inspect.cpp — authenticated client for explicitly enabled inspector sessions

#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

static std::string inspect_trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool g_inspect_color = true;
static std::string ic_bold()  { return g_inspect_color ? "\033[1m"  : ""; }
static std::string ic_cyan()  { return g_inspect_color ? "\033[36m" : ""; }
static std::string ic_green() { return g_inspect_color ? "\033[32m" : ""; }
static std::string ic_red()   { return g_inspect_color ? "\033[31m" : ""; }
static std::string ic_reset() { return g_inspect_color ? "\033[0m"  : ""; }

namespace {

using namespace pulp::inspect;

bool require_arg_value(const std::vector<std::string>& args,
                       std::size_t& index,
                       const char* flag,
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
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value <= 0 || value > 65535)
        return false;
    output = value;
    return true;
}

std::string endpoint_host(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    return separator == std::string_view::npos
               ? std::string(endpoint)
               : std::string(endpoint.substr(0, separator));
}

int endpoint_port(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    int value = 0;
    if (separator == std::string_view::npos ||
        !parse_port(endpoint.substr(separator + 1), value))
        return 0;
    return value;
}

void print_error(const InspectorMessage& response) {
    std::cerr << "Error";
    if (!response.error_code.empty())
        std::cerr << " [" << response.error_code << "]";
    std::cerr << ": " << response.params_json << "\n";
    if (!response.error_data_json.empty() &&
        response.error_data_json != "{}")
        std::cerr << response.error_data_json << "\n";
}

} // namespace

int cmd_inspect(const std::vector<std::string>& args) {
    std::string host;
    int port = 0;
    std::string session_id;
    std::string instance_id;
    std::string command;
    std::string params = "{}";
    std::string output_file;
    bool params_provided = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "pulp inspect — authenticated client for explicitly enabled sessions\n\n"
                << "Usage: pulp inspect [options]\n\n"
                << "Options:\n"
                << "  --session ID      Select the exact live session\n"
                << "  --instance ID     Select the exact instance within a session\n"
                << "  --host HOST       Filter discovery by host (loopback only)\n"
                << "  --port PORT       Filter discovery by port; never bypasses auth\n"
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
        if (arg == "--host") {
            if (!require_arg_value(args, index, "--host", host)) return 2;
        } else if (arg == "--port") {
            std::string text;
            if (!require_arg_value(args, index, "--port", text)) return 2;
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
        } else if (arg == "--command") {
            if (!require_arg_value(args, index, "--command", command)) return 2;
        } else if (arg == "--params") {
            if (!require_arg_value(args, index, "--params", params)) return 2;
            params_provided = true;
        } else if (arg == "--output") {
            if (!require_arg_value(args, index, "--output", output_file))
                return 2;
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
    if (!host.empty() && host != "127.0.0.1" && host != "localhost") {
        std::cerr << "Error: inspector sessions are loopback-only\n";
        return 2;
    }
    if (host == "localhost")
        host = "127.0.0.1";

    InspectorDiscoveryReader discovery;
    auto records = discovery.list();
    records.erase(
        std::remove_if(records.begin(), records.end(), [&](const auto& record) {
            return (!host.empty() && endpoint_host(record.endpoint) != host) ||
                   (port != 0 && endpoint_port(record.endpoint) != port);
        }),
        records.end());
    std::string selection_error;
    const auto selected = select_inspector_session(
        records, session_id, instance_id, &selection_error);
    if (!selected) {
        std::cerr << "Error: " << selection_error;
        if (!host.empty() || port != 0 || !session_id.empty() ||
            !instance_id.empty()) {
            std::cerr << " (requested";
            if (!host.empty()) std::cerr << " host " << host;
            if (port != 0) std::cerr << " port " << port;
            if (!session_id.empty()) std::cerr << " session " << session_id;
            if (!instance_id.empty())
                std::cerr << " instance " << instance_id;
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
            std::cout << ic_cyan() << "← " << event.method << ic_reset()
                      << "\n";
            if (!event.params_json.empty() && event.params_json != "{}")
                std::cout << "  " << event.params_json << "\n";
        });
    }
    if (!client.connect(*selected, discovery)) {
        std::cerr << "Error: authentication or connection failed for session "
                  << selected->session_id << "\n";
        return 1;
    }
    status << "  " << ic_green() << "✓" << ic_reset()
           << " Connected to inspector\n";

    if (!command.empty()) {
        const auto* descriptor = find_inspector_method(command);
        const bool needs_controller =
            descriptor &&
            descriptor->kind == InspectorMethodKind::Request &&
            capability_requires_controller_lease(descriptor->capability) &&
            command != methods::kSessionAcquireController &&
            command != methods::kSessionRenewController &&
            command != methods::kSessionReleaseController;
        if (needs_controller) {
            const auto lease =
                client.request(std::string(methods::kSessionAcquireController));
            if (lease.is_error) {
                print_error(lease);
                return 1;
            }
        }
        const auto response = client.request(command, params);
        if (response.is_error) {
            print_error(response);
            return 1;
        }
        if (!output_file.empty()) {
            std::ofstream output(output_file, std::ios::trunc);
            if (!output || !(output << response.params_json)) {
                std::cerr << "Error: could not write " << output_file << "\n";
                return 1;
            }
            std::cout << "Written to " << output_file << "\n";
        } else {
            std::cout << response.params_json << "\n";
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
        if (const auto separator = line.find(' ');
            separator != std::string::npos) {
            method = line.substr(0, separator);
            request_params = inspect_trim(line.substr(separator + 1));
        }
        const auto response = client.request(method, request_params);
        if (response.is_error) {
            std::cout << ic_red() << "✗ [" << response.error_code << "] "
                      << response.params_json << ic_reset() << "\n";
        } else {
            std::cout << ic_green() << "✓" << ic_reset() << " "
                      << response.params_json << "\n";
        }
    }
    return 0;
}
