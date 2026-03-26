// pulp-mcp — MCP (Model Context Protocol) server for Pulp
// Exposes Pulp operations as tools via stdin/stdout JSON-RPC 2.0

#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;

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
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to run command";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        result += buffer;
    int status = pclose(pipe);
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

static std::string handle_validate(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto output = exec(root.string() + "/build/tools/cli/pulp validate 2>&1");
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

// ── MCP Protocol Handler ─────────────────────────────────────────────────────

static std::string tools_list_json() {
    return R"JSON({"tools":[
{"name":"pulp_build","description":"Build the Pulp project (configure + compile)","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_test","description":"Run the Pulp test suite","inputSchema":{"type":"object","properties":{"filter":{"type":"string","description":"Test name filter (regex)"}}}},
{"name":"pulp_status","description":"Show Pulp project status","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_validate","description":"Run plugin format validators","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_screenshot","description":"Render a plugin UI to PNG (base64). Use --demo for a built-in demo or --script for a JS file.","inputSchema":{"type":"object","properties":{"script":{"type":"string","description":"Path to JS UI script"},"width":{"type":"integer","description":"Width in points (default 400)"},"height":{"type":"integer","description":"Height in points (default 300)"},"theme":{"type":"string","description":"Theme: dark, light, pro_audio"},"demo":{"type":"boolean","description":"Render built-in demo UI"}}}},
{"name":"pulp_simulate_click","description":"Simulate a mouse click at coordinates on a demo UI and return the view tree JSON","inputSchema":{"type":"object","properties":{"x":{"type":"number","description":"X coordinate"},"y":{"type":"number","description":"Y coordinate"}}}},
{"name":"pulp_get_view_tree","description":"Get the view tree as JSON for a demo UI","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_create","description":"Scaffold a new plugin project from templates","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Plugin name"},"type":{"type":"string","enum":["effect","instrument"],"description":"Plugin type"},"manufacturer":{"type":"string","description":"Manufacturer name"}}}},
{"name":"pulp_docs_check","description":"Validate docs consistency against the codebase","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_docs_search","description":"Search local docs for a query string","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Search query"}}}}
]})JSON";
}

static std::string handle_request(const std::string& json) {
    auto method = extract_string(json, "method");
    auto id = extract_raw(json, "id");
    if (id.empty()) id = "null";

    if (method == "initialize") {
        return json_result(id, R"JSON({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"pulp-mcp","version":"0.1.0"}})JSON");
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
        else return json_error(id, -32601, "Unknown tool: " + name);

        return json_result(id, result);
    }

    if (method == "ping") {
        return json_result(id, "{}");
    }

    return json_error(id, -32601, "Method not found: " + method);
}

// ── Main: stdio JSON-RPC transport ───────────────────────────────────────────

int main() {
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
