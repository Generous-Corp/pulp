// pulp-mcp — MCP (Model Context Protocol) server for Pulp
// Exposes Pulp operations as tools via stdin/stdout JSON-RPC 2.0

#include <iostream>
#include <string>
#include <string_view>
#include <sstream>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/service.hpp>

#include "pulp_mcp_version.h"

namespace fs = std::filesystem;

#if defined(_WIN32)
#define PULP_POPEN _popen
#define PULP_PCLOSE _pclose
#else
#define PULP_POPEN popen
#define PULP_PCLOSE pclose
#endif

// ── JSON helpers (minimal, no external deps for MCP server) ──────────────────

static std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    out += "\"";
    return out;
}

static std::string json_error(const std::string& id, int code, const std::string& msg) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + json_string(msg) + "}}";
}

static std::string json_result(const std::string& id, const std::string& result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result_json + "}";
}

static std::string json_tool_payload(const std::string& structured_json) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(structured_json) +
           "}],\"structuredContent\":" + structured_json + "}";
}

// ── Extract JSON fields (simple parsing) ─────────────────────────────────────

static std::string extract_string(const std::string& json, const std::string& key) {
    auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};

    auto colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string::npos) return {};

    auto start = json.find('"', colon + 1);
    if (start == std::string::npos) return {};

    auto end = json.find('"', start + 1);
    while (end != std::string::npos && json[end - 1] == '\\') {
        end = json.find('"', end + 1);
    }
    if (end == std::string::npos) return {};

    return json.substr(start + 1, end - start - 1);
}

static std::string extract_raw(const std::string& json, const std::string& key) {
    auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};

    auto colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string::npos) return {};

    // Skip whitespace
    auto start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;

    if (start >= json.size()) return {};

    // If it's a string
    if (json[start] == '"') {
        auto end = json.find('"', start + 1);
        while (end != std::string::npos && json[end - 1] == '\\')
            end = json.find('"', end + 1);
        if (end == std::string::npos) return {};
        return json.substr(start, end - start + 1);
    }

    // If it's a number or null
    auto end = json.find_first_of(",}", start);
    if (end == std::string::npos) end = json.size();
    auto value = json.substr(start, end - start);
    // Trim
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n')) value.pop_back();
    return value;
}

// ── Shell execution ──────────────────────────────────────────────────────────

static std::string exec(const std::string& cmd) {
    std::string result;
    FILE* pipe = PULP_POPEN(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to run command";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        result += buffer;
    int status = PULP_PCLOSE(pipe);
    if (status != 0 && result.empty())
        result = "Command failed with status " + std::to_string(status);
    return result;
}

static fs::path find_project_root() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "core"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ── MCP Tool Handlers ────────────────────────────────────────────────────────

static std::string handle_build(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string output;

    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        output += exec("cmake -B " + build_dir.string() + " -S " + root.string() + " 2>&1");
    }
    output += exec("cmake --build " + build_dir.string() + " 2>&1");

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

static std::string handle_test(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string cmd = "ctest --test-dir " + build_dir.string() + " --output-on-failure";

    auto filter = extract_string(params_json, "filter");
    if (!filter.empty()) cmd += " -R " + filter;

    auto output = exec(cmd + " 2>&1");
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

static std::string handle_status(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::ostringstream out;
    out << "Pulp Project: " << root.string() << "\n";

    auto branch = exec("git -C " + root.string() + " branch --show-current 2>/dev/null");
    if (!branch.empty()) {
        while (!branch.empty() && branch.back() == '\n') branch.pop_back();
        out << "Branch: " << branch << "\n";
    }

    auto build_dir = root / "build";
    out << "Build: " << (fs::exists(build_dir / "CMakeCache.txt") ? "configured" : "not configured") << "\n";

    int src = 0, hdr = 0, tests = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "core")) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".mm") ++src;
        if (ext == ".hpp" || ext == ".h") ++hdr;
    }
    if (fs::exists(root / "test")) {
        for (auto& e : fs::directory_iterator(root / "test")) {
            if (e.path().extension() == ".cpp") ++tests;
        }
    }
    out << "Sources: " << src << " impl, " << hdr << " headers, " << tests << " test files\n";

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(out.str()) + "}]}";
}

static std::string handle_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = root.string() + "/build/tools/cli/pulp validate --json";
    if (params_json.find("\"all\"") != std::string::npos &&
        params_json.find("true") != std::string::npos) {
        cmd += " --all";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

static std::string handle_audio_model_status(const std::string& /*params_json*/) {
    auto status = pulp::tools::audio::query_model_status();
    return json_tool_payload(pulp::tools::audio::to_json(status));
}

static std::string handle_audio_model_list(const std::string& /*params_json*/) {
    auto result = pulp::tools::audio::list_models();
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

static std::string handle_audio_model_activate(const std::string& params_json) {
    auto model_id = extract_string(params_json, "model_id");
    if (model_id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: model_id is required\"}]}";
    }

    auto result = pulp::tools::audio::activate_model(model_id);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

static std::string handle_audio_read_bundle(const std::string& params_json) {
    auto bundle_path = extract_string(params_json, "bundle_path");
    if (bundle_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: bundle_path is required\"}]}";
    }

    auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
    return json_tool_payload(pulp::tools::audio::to_json(bundle));
}

static int extract_int(const std::string& json, const std::string& key, int fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    try {
        return std::stoi(raw);
    } catch (...) {
        return fallback;
    }
}

static double extract_double(const std::string& json, const std::string& key, double fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    try {
        return std::stod(raw);
    } catch (...) {
        return fallback;
    }
}

static bool extract_bool(const std::string& json, const std::string& key, bool fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    return raw == "true" ? true : raw == "false" ? false : fallback;
}

static std::string handle_audio_excerpt_find(const std::string& params_json) {
    auto text = extract_string(params_json, "text");
    auto input_path = extract_string(params_json, "input_path");
    if (text.empty() || input_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: text and input_path are required\"}]}";
    }

    pulp::tools::audio::ExcerptFindRequest request;
    request.text = text;
    request.input_path = input_path;
    request.model_id = extract_string(params_json, "model_id");
    request.recursive = extract_bool(params_json, "recursive", false);
    request.top_k = static_cast<std::size_t>(extract_int(params_json, "top", 5));
    request.window_ms = static_cast<uint64_t>(extract_int(params_json, "window_ms", 1500));
    request.hop_ms = static_cast<uint64_t>(extract_int(params_json, "hop_ms", 250));
    request.min_score = extract_double(params_json, "min_score", 0.0);
    request.max_candidates_per_file =
        static_cast<std::size_t>(extract_int(params_json, "max_candidates_per_file", 3));
    request.bundle_out = extract_string(params_json, "bundle_out");

    auto result = pulp::tools::audio::run_excerpt_find(request);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

// ── MCP Protocol Handler ─────────────────────────────────────────────────────

static std::string tools_list_json() {
    return R"JSON({"tools":[
{"name":"pulp_build","description":"Build the Pulp project (configure + compile)","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_test","description":"Run the Pulp test suite","inputSchema":{"type":"object","properties":{"filter":{"type":"string","description":"Test name filter (regex)"}}}},
{"name":"pulp_status","description":"Show Pulp project status","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_validate","description":"Run plugin format validators (CLAP, VST3/pluginval, AU, optional AAX). Use --all for vstvalidator and full AAX validation. Returns JSON report.","inputSchema":{"type":"object","properties":{"all":{"type":"boolean","description":"Run all validators including vstvalidator and full AAX validation"},"json":{"type":"boolean","description":"Return JSON report (default true via MCP)"}}}},
{"name":"pulp_audio_model_list","description":"List registered audio models and their current install state.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_audio_model_status","description":"Show the configured audio model and whether its recorded checkpoint is loadable now.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_audio_model_activate","description":"Activate an installed audio model by logical id.","inputSchema":{"type":"object","required":["model_id"],"properties":{"model_id":{"type":"string","description":"Registered audio model id"}}}},
{"name":"pulp_audio_excerpt_find","description":"Run WAV-first excerpt-find with the deterministic null backend and create a bundle.","inputSchema":{"type":"object","required":["text","input_path"],"properties":{"text":{"type":"string","description":"Natural language excerpt query"},"input_path":{"type":"string","description":"File or directory path to scan"},"model_id":{"type":"string","description":"Optional registered audio model id"},"recursive":{"type":"boolean","description":"Recurse into input directories"},"top":{"type":"integer","description":"Maximum ranked results to return"},"window_ms":{"type":"integer","description":"Excerpt window size in milliseconds"},"hop_ms":{"type":"integer","description":"Window hop size in milliseconds"},"min_score":{"type":"number","description":"Minimum deterministic stub score threshold"},"max_candidates_per_file":{"type":"integer","description":"Per-file candidate cap before global ranking"},"bundle_out":{"type":"string","description":"Directory to create excerpt bundles in"}}}},
{"name":"pulp_audio_read_bundle","description":"Read an excerpt-find artifact bundle and return parsed manifest/result summary.","inputSchema":{"type":"object","required":["bundle_path"],"properties":{"bundle_path":{"type":"string","description":"Path to an excerpt-find bundle directory"}}}},
{"name":"pulp_screenshot","description":"Render a plugin UI to PNG (base64). Use --demo for a built-in demo or --script for a JS file.","inputSchema":{"type":"object","properties":{"script":{"type":"string","description":"Path to JS UI script"},"width":{"type":"integer","description":"Width in points (default 400)"},"height":{"type":"integer","description":"Height in points (default 300)"},"theme":{"type":"string","description":"Theme: dark, light, pro_audio"},"demo":{"type":"boolean","description":"Render built-in demo UI"}}}},
{"name":"pulp_simulate_click","description":"Simulate a mouse click at coordinates on a demo UI and return the view tree JSON","inputSchema":{"type":"object","properties":{"x":{"type":"number","description":"X coordinate"},"y":{"type":"number","description":"Y coordinate"}}}},
{"name":"pulp_get_view_tree","description":"Get the view tree as JSON for a demo UI","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_create","description":"Scaffold a new plugin project from templates","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Plugin name"},"type":{"type":"string","enum":["effect","instrument"],"description":"Plugin type"},"manufacturer":{"type":"string","description":"Manufacturer name"}}}},
{"name":"pulp_docs_check","description":"Validate docs consistency against the codebase","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_docs_search","description":"Search local docs for a query string","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Search query"}}}},
{"name":"pulp_inspect_dom","description":"Get the view tree of a running plugin's UI via the inspector protocol","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_params","description":"Get all parameter info and current values from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_screenshot","description":"Capture a screenshot from a running plugin via the inspector","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_evaluate","description":"Evaluate a JS expression in a running plugin's script engine","inputSchema":{"type":"object","properties":{"expression":{"type":"string","description":"JS expression to evaluate"}}}},
{"name":"pulp_inspect_performance","description":"Get render performance metrics from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_audio","description":"Get audio configuration and buffer underrun info from a running plugin","inputSchema":{"type":"object","properties":{}}}
]})JSON";
}

static std::string handle_request(const std::string& json) {
    auto method = extract_string(json, "method");
    auto id = extract_raw(json, "id");
    if (id.empty()) id = "null";

    if (method == "initialize") {
        // serverInfo.version tracks the SDK/CLI release (#2067).
        // Held constant at "0.1.0" pre-fix; now wired to PROJECT_VERSION
        // via tools/mcp/pulp_mcp_version.h.in so doctor/launcher can see
        // real drift between an old installed pulp-mcp and a newer plugin.
        std::string payload =
            std::string(R"JSON({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"pulp-mcp","version":")JSON")
            + PULP_MCP_SERVER_VERSION
            + std::string(R"JSON("}})JSON");
        return json_result(id, payload);
    }

    if (method == "notifications/initialized") {
        return {}; // No response for notifications
    }

    if (method == "tools/list") {
        return json_result(id, tools_list_json());
    }

    if (method == "tools/call") {
        auto name = extract_string(json, "name");
        // Extract the arguments sub-object (simplified)
        auto args_pos = json.find("\"arguments\"");
        std::string args_json = "{}";
        if (args_pos != std::string::npos) {
            auto brace = json.find('{', args_pos);
            if (brace != std::string::npos) {
                int depth = 1;
                auto end = brace + 1;
                while (end < json.size() && depth > 0) {
                    if (json[end] == '{') ++depth;
                    if (json[end] == '}') --depth;
                    ++end;
                }
                args_json = json.substr(brace, end - brace);
            }
        }

        std::string result;
        if (name == "pulp_build")          result = handle_build(args_json);
        else if (name == "pulp_test")      result = handle_test(args_json);
        else if (name == "pulp_status")    result = handle_status(args_json);
        else if (name == "pulp_validate")  result = handle_validate(args_json);
        else if (name == "pulp_audio_model_list")     result = handle_audio_model_list(args_json);
        else if (name == "pulp_audio_model_status")   result = handle_audio_model_status(args_json);
        else if (name == "pulp_audio_model_activate") result = handle_audio_model_activate(args_json);
        else if (name == "pulp_audio_excerpt_find")   result = handle_audio_excerpt_find(args_json);
        else if (name == "pulp_audio_read_bundle")    result = handle_audio_read_bundle(args_json);
        else if (name == "pulp_screenshot" || name == "pulp_simulate_click" || name == "pulp_get_view_tree") {
            // These tools delegate to pulp-screenshot binary
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
                if (name == "pulp_screenshot") {
                    auto demo = extract_string(args_json, "demo");
                    auto script = extract_string(args_json, "script");
                    std::string cmd = screenshot_bin.string() + " --base64";
                    if (!script.empty()) cmd += " --script " + script;
                    else cmd += " --demo";
                    auto theme = extract_string(args_json, "theme");
                    if (!theme.empty()) cmd += " --theme " + theme;
                    auto output = exec(cmd + " 2>/dev/null");
                    result = "{\"content\":[{\"type\":\"image\",\"data\":\"" + output + "\",\"mimeType\":\"image/png\"}]}";
                } else {
                    // simulate_click and get_view_tree: run screenshot in demo mode, capture view tree
                    std::string cmd = screenshot_bin.string() + " --demo --output /dev/null 2>/dev/null";
                    exec(cmd);
                    result = "{\"content\":[{\"type\":\"text\",\"text\":\"View tree and event simulation available via pulp-screenshot --demo\"}]}";
                }
            }
        }
        else if (name == "pulp_create") {
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto plugin_name = extract_string(args_json, "name");
                auto plugin_type = extract_string(args_json, "type");
                auto manufacturer = extract_string(args_json, "manufacturer");
                if (plugin_name.empty()) {
                    result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: name is required\"}]}";
                } else {
                    std::string cmd = "python3 " + (root / "tools" / "create-project.py").string();
                    cmd += " \"" + plugin_name + "\"";
                    if (!plugin_type.empty()) cmd += " --type " + plugin_type;
                    if (!manufacturer.empty()) cmd += " --manufacturer \"" + manufacturer + "\"";
                    auto output = exec(cmd + " 2>&1");
                    result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
                }
            }
        }
        else if (name == "pulp_docs_check") {
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto output = exec("bash " + (root / "tools" / "check-docs.sh").string() + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        else if (name == "pulp_docs_search") {
            auto root = find_project_root();
            auto query = extract_string(args_json, "query");
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else if (query.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: query is required\"}]}";
            } else {
                auto output = exec((root / "build" / "tools" / "cli" / "pulp").string() + " docs search \"" + query + "\" 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        // Inspector tools — delegate to pulp inspect CLI for now
        // (in the future these could connect directly via TCP)
        else if (name == "pulp_inspect_dom" || name == "pulp_inspect_params" ||
                 name == "pulp_inspect_screenshot" || name == "pulp_inspect_evaluate" ||
                 name == "pulp_inspect_performance" || name == "pulp_inspect_audio") {
            // Map MCP tool name to inspector protocol method
            std::string inspector_method;
            std::string inspector_params;
            if (name == "pulp_inspect_dom")         inspector_method = "DOM.getDocument";
            else if (name == "pulp_inspect_params")  inspector_method = "State.getParameters";
            else if (name == "pulp_inspect_screenshot") inspector_method = "Capture.screenshot";
            else if (name == "pulp_inspect_evaluate") {
                inspector_method = "Runtime.evaluate";
                auto expr = extract_string(args_json, "expression");
                if (!expr.empty()) inspector_params = " {\"expression\":" + json_string(expr) + "}";
            }
            else if (name == "pulp_inspect_performance") inspector_method = "Performance.getMetrics";
            else if (name == "pulp_inspect_audio")   inspector_method = "Audio.getConfig";

            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto cli = (root / "build" / "tools" / "cli" / "pulp").string();
                auto output = exec(cli + " inspect --command " + inspector_method + inspector_params + " 2>&1");
                if (name == "pulp_inspect_screenshot") {
                    // Screenshot returns base64 PNG — escape for safe JSON embedding
                    result = "{\"content\":[{\"type\":\"image\",\"data\":" + json_string(output) + ",\"mimeType\":\"image/png\"}]}";
                } else {
                    result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
                }
            }
        }
        else return json_error(id, -32601, "Unknown tool: " + name);

        return json_result(id, result);
    }

    if (method == "ping") {
        return json_result(id, "{}");
    }

    return json_error(id, -32601, "Method not found: " + method);
}

// ── Main: stdio JSON-RPC transport ───────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Flag-only invocations short-circuit the JSON-RPC loop so the
    // release-CLI smoke gate and `pulp doctor` can probe the binary
    // without speaking MCP framing. Keep this list narrow — anything
    // that consumes stdin must fall through to the loop below.
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--version" || arg == "-V") {
            std::cout << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n"
                << "MCP (Model Context Protocol) server for Pulp.\n"
                << "Speaks JSON-RPC 2.0 over stdin/stdout — normally\n"
                << "invoked by .mcp.json via tools/mcp/pulp-mcp-launcher.\n"
                << "\n"
                << "Flags:\n"
                << "  --version, -V   Print version and exit\n"
                << "  --help, -h      Show this help\n";
            return 0;
        }
        std::cerr << "pulp-mcp: unknown flag '" << arg
                  << "'. Try --help.\n";
        return 2;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // MCP uses Content-Length header framing
        if (line.find("Content-Length:") == 0) {
            int length = std::stoi(line.substr(15));
            std::getline(std::cin, line); // empty line
            std::string body(length, '\0');
            std::cin.read(body.data(), length);

            auto response = handle_request(body);
            if (!response.empty()) {
                std::cout << "Content-Length: " << response.size() << "\r\n\r\n" << response;
                std::cout.flush();
            }
            continue;
        }

        // Also handle bare JSON (for simpler testing)
        auto response = handle_request(line);
        if (!response.empty()) {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }

    return 0;
}
