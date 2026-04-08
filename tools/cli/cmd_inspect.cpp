// cmd_inspect.cpp — pulp inspect: connect to a running plugin's inspector server
// Interactive REPL or one-shot command mode.

#include <pulp/inspect/protocol.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/system.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Minimal helpers (these exist in pulp_cli.cpp as static — we duplicate the tiny ones)
static std::string inspect_trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Simple color helpers for the inspect command
static bool g_inspect_color = true;
static std::string ic_dim()   { return g_inspect_color ? "\033[2m"  : ""; }
static std::string ic_bold()  { return g_inspect_color ? "\033[1m"  : ""; }
static std::string ic_cyan()  { return g_inspect_color ? "\033[36m" : ""; }
static std::string ic_green() { return g_inspect_color ? "\033[32m" : ""; }
static std::string ic_red()   { return g_inspect_color ? "\033[31m" : ""; }
static std::string ic_reset() { return g_inspect_color ? "\033[0m"  : ""; }

namespace {

using namespace pulp::inspect;
using namespace pulp::events;

// Find the inspector port by reading discovery files
static int discover_port() {
    std::string tmp_dir;
#ifdef _WIN32
    if (auto env = pulp::runtime::get_env("TEMP")) tmp_dir = *env;
    else tmp_dir = ".";
#else
    if (auto env = pulp::runtime::get_env("TMPDIR")) tmp_dir = *env;
    else tmp_dir = "/tmp";
#endif

    // Look for pulp-inspector-*.port files, pick the most recent
    int found_port = 0;
    fs::file_time_type newest_time{};

    for (auto& entry : fs::directory_iterator(tmp_dir)) {
        auto name = entry.path().filename().string();
        if (name.find("pulp-inspector-") == 0 && name.find(".port") != std::string::npos) {
            std::ifstream f(entry.path());
            int port = 0;
            if (f >> port && port > 0) {
                auto mtime = fs::last_write_time(entry.path());
                if (mtime > newest_time || found_port == 0) {
                    newest_time = mtime;
                    found_port = port;
                }
            }
        }
    }

    return found_port;
}

// Send a request and wait for response
static std::string send_request(InterprocessConnection& conn, const std::string& method,
                                 const std::string& params = "{}") {
    static std::atomic<int64_t> next_id{1};
    auto id = next_id++;
    auto msg = make_request(id, method, params);
    auto json = encode_message(msg);
    conn.send_message(json);

    // Wait for response (simple blocking — fine for CLI)
    // The response will arrive via on_text_message
    // For now, we use a simple sleep+poll approach
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return "";  // Response handled by callback
}

} // anonymous namespace

int cmd_inspect(const std::vector<std::string>& args) {
    // Parse flags
    std::string host = "127.0.0.1";
    int port = 0;
    std::string one_shot_command;
    std::string one_shot_params = "{}";
    std::string output_file;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp inspect — connect to a running plugin's inspector\n\n";
            std::cout << "Usage: pulp inspect [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --host HOST       Connect to HOST (default: 127.0.0.1)\n";
            std::cout << "  --port PORT       Connect to PORT (default: auto-discover)\n";
            std::cout << "  --command METHOD  Send one command and print result\n";
            std::cout << "  --params JSON     JSON params for --command (default: {})\n";
            std::cout << "  --output FILE     Write result to FILE (with --command)\n\n";
            std::cout << "Examples:\n";
            std::cout << "  pulp inspect                              # Interactive REPL\n";
            std::cout << "  pulp inspect --command DOM.getDocument     # One-shot query\n";
            std::cout << "  pulp inspect --command Capture.screenshot --output shot.png\n";
            std::cout << "  pulp inspect --host 192.168.1.42          # Remote debugging\n";
            return 0;
        }
        if (args[i] == "--host" && i + 1 < args.size()) host = args[++i];
        else if (args[i] == "--port" && i + 1 < args.size()) port = std::stoi(args[++i]);
        else if (args[i] == "--command" && i + 1 < args.size()) one_shot_command = args[++i];
        else if (args[i] == "--params" && i + 1 < args.size()) one_shot_params = args[++i];
        else if (args[i] == "--output" && i + 1 < args.size()) output_file = args[++i];
    }

    // Auto-discover port if not specified
    if (port == 0) {
        port = discover_port();
        if (port == 0) {
            std::cerr << "Error: no running Pulp inspector found.\n";
            std::cerr << "  Launch a plugin with inspector enabled, or specify --port.\n";
            return 1;
        }
        std::cout << ic_dim() << "Found inspector on port " << port << ic_reset() << "\n";
    }

    // Connect
    InterprocessConnection conn;
    auto address = host + ":" + std::to_string(port);
    std::cout << "Connecting to " << address << "...\n";

    // Set up message handler
    std::atomic<bool> got_response{false};
    std::atomic<bool> got_error{false};
    std::string last_response;
    conn.on_text_message = [&](std::string_view msg) {
        InspectorMessage response;
        if (decode_message(std::string(msg), response)) {
            if (!one_shot_command.empty()) {
                // One-shot mode: capture result
                if (response.is_error) {
                    std::cerr << "Error: " << response.params_json << "\n";
                    got_error = true;
                } else {
                    last_response = response.params_json;
                }
                got_response = true;
            } else {
                // REPL mode: print events and responses
                if (!response.method.empty()) {
                    // Event
                    std::cout << ic_cyan() << "← " << response.method << ic_reset() << "\n";
                    if (!response.params_json.empty() && response.params_json != "{}")
                        std::cout << "  " << response.params_json << "\n";
                } else {
                    // Response
                    if (response.is_error) {
                        std::cout << ic_red() << "✗ Error: " << response.params_json << ic_reset() << "\n";
                    } else {
                        std::cout << ic_green() << "✓" << ic_reset() << " "
                                  << response.params_json << "\n";
                    }
                }
            }
        }
    };

    if (!conn.connect(address, IpcTransport::Socket)) {
        std::cerr << "Error: could not connect to " << address << "\n";
        return 1;
    }

    std::cout << "  " << ic_green() << "\xe2\x9c\x93" << ic_reset() << " Connected to inspector\n";

    // One-shot mode
    if (!one_shot_command.empty()) {
        auto msg = make_request(1, one_shot_command, one_shot_params);
        auto json = encode_message(msg);
        conn.send_message(json);

        // Wait for response
        for (int i = 0; i < 50 && !got_response; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!got_response) {
            std::cerr << "Error: no response received (timeout)\n";
            return 1;
        }

        if (got_error) return 1;

        if (!output_file.empty()) {
            std::ofstream f(output_file);
            f << last_response;
            std::cout << "Written to " << output_file << "\n";
        } else {
            std::cout << last_response << "\n";
        }

        return 0;
    }

    // REPL mode
    std::cout << "Inspector REPL. Type a method name (e.g., DOM.getDocument) or 'quit'.\n\n";

    std::string line;
    int64_t request_id = 1;
    while (true) {
        std::cout << ic_bold() << "inspect> " << ic_reset();
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;
        line = inspect_trim(line);
        if (line.empty()) continue;
        if (line == "quit" || line == "exit" || line == "q") break;

        // Parse: METHOD [JSON_PARAMS]
        std::string method = line;
        std::string params = "{}";
        auto space = line.find(' ');
        if (space != std::string::npos) {
            method = line.substr(0, space);
            params = inspect_trim(line.substr(space + 1));
        }

        auto msg = make_request(request_id++, method, params);
        auto json = encode_message(msg);
        conn.send_message(json);

        // Brief pause to receive response
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    conn.disconnect();
    return 0;
}
