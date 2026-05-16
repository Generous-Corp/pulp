// pulp-mcp — MCP (Model Context Protocol) server for Pulp
// Exposes Pulp operations as tools via stdin/stdout JSON-RPC 2.0

#include <iostream>
#include <string>
#include <string_view>
#include <sstream>
#include <fstream>
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

// ── Compat: project SDK resolution + per-tool min_sdk floors ────────────────
//
// pulp-mcp ships independently of any given Pulp project. A user can have
// installed pulp-mcp v0.110 (from a recent `pulp upgrade`) while editing
// a project pinned to SDK 0.92.0 in its `pulp.toml`. If a tool's
// implementation needs API surface that landed in SDK >= 0.100.0, calling
// it on the older project would silently misbehave (today) or use a
// newer behavior the project's author didn't ratify.
//
// Per-tool feature detection (#2070): each tool can declare a
// `min_sdk_version` floor below; when the project's SDK is older, the
// tools/call dispatch returns a structured error to the LLM with
// actionable upgrade guidance instead of running the tool. Tools left
// out of `tool_min_sdk_table` (the default) declare no floor — they run
// against any project SDK, matching pre-#2070 behavior.
//
// Design choices the launcher must NOT change:
//   - Tolerance is per-tool, not connection-wide. Mismatched plugin and
//     pulp-mcp must continue to load and handshake (#2067 contract).
//   - Project SDK is read on every call (cheap; reads at most two files
//     from the project root). This avoids stale caching when a user runs
//     `pulp project bump` between MCP calls.

static fs::path find_project_root_any() {
    // Find the nearest ancestor with either:
    //   - pulp.toml (SDK-mode projects), or
    //   - CMakeLists.txt + core/ (source-tree Pulp checkouts).
    // The any-version is used by the compat gate, which must work for
    // both modes. (`find_project_root` above is source-tree-only and is
    // kept for tools that genuinely need a Pulp checkout, like the
    // pulp-screenshot fallback.)
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "pulp.toml")) return dir;
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "core"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

static std::string read_file_text(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string parse_pulp_toml_sdk_version(const std::string& body) {
    // Hand-rolled minimal TOML scan — pulp.toml is small and the
    // sdk_version key is a top-level scalar. We deliberately do NOT
    // pull in a TOML library here: pulp-mcp is intentionally minimal-
    // deps so its binary stays small and load-fast.
    auto pos = body.find("sdk_version");
    while (pos != std::string::npos) {
        // Reject `sdk_version` substrings that are inside other keys
        // (`min_sdk_version`, etc.): require the previous non-space
        // char to be a newline or start-of-file. Leading whitespace
        // on the same line is allowed.
        bool at_key_start = true;
        for (std::size_t i = pos; i > 0; --i) {
            char c = body[i - 1];
            if (c == '\n' || c == '\r') break; // walked back to a line boundary — ok
            if (c != ' ' && c != '\t') { at_key_start = false; break; }
        }
        if (!at_key_start) {
            pos = body.find("sdk_version", pos + 1);
            continue;
        }
        auto eq = body.find('=', pos);
        if (eq == std::string::npos) break;
        auto q1 = body.find('"', eq);
        if (q1 == std::string::npos) break;
        auto q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        return body.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

static std::string parse_cmake_project_version(const std::string& body) {
    // Match `project(<name> ... VERSION x.y.z ...)`. CMakeLists.txt for
    // a Pulp project either calls `project(...)` directly or uses
    // `pulp_add_plugin(... VERSION "x.y.z" ...)`. Try both.
    auto try_match = [&](std::size_t start, char quote_open, char quote_close) -> std::string {
        auto vpos = body.find("VERSION", start);
        if (vpos == std::string::npos) return {};
        std::size_t i = vpos + 7;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t' ||
                                   body[i] == '\n' || body[i] == '\r' ||
                                   body[i] == quote_open)) ++i;
        std::string out;
        while (i < body.size()) {
            char c = body[i];
            if ((c >= '0' && c <= '9') || c == '.') out += c;
            else break;
            ++i;
        }
        // Must look like a semver triple.
        int dots = 0;
        for (char c : out) if (c == '.') ++dots;
        if (dots != 2) return {};
        return out;
    };
    // project(... VERSION x.y.z ...)
    auto p = body.find("project(");
    if (p != std::string::npos) {
        auto v = try_match(p, '(', ')');
        if (!v.empty()) return v;
    }
    // pulp_add_plugin(... VERSION "x.y.z" ...)
    auto pap = body.find("pulp_add_plugin(");
    if (pap != std::string::npos) {
        auto v = try_match(pap, '"', '"');
        if (!v.empty()) return v;
    }
    return {};
}

static std::string resolve_project_sdk_version() {
    auto root = find_project_root_any();
    if (root.empty()) return {};
    // pulp.toml wins when both are present (SDK-mode projects pin
    // explicitly; CMakeLists.txt VERSION there is the *product* version,
    // not the SDK version).
    auto toml_path = root / "pulp.toml";
    if (fs::exists(toml_path)) {
        auto v = parse_pulp_toml_sdk_version(read_file_text(toml_path));
        if (!v.empty()) return v;
    }
    auto cmake_path = root / "CMakeLists.txt";
    if (fs::exists(cmake_path)) {
        auto v = parse_cmake_project_version(read_file_text(cmake_path));
        if (!v.empty()) return v;
    }
    return {};
}

static bool parse_semver_triple(const std::string& s, int& maj, int& min, int& patch) {
    maj = min = patch = -1;
    std::size_t i = 0;
    auto eat = [&](int& out) -> bool {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        int v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0'); ++i;
        }
        out = v; return true;
    };
    if (!eat(maj)) return false;
    if (i >= s.size() || s[i] != '.') return false; ++i;
    if (!eat(min)) return false;
    if (i >= s.size() || s[i] != '.') return false; ++i;
    if (!eat(patch)) return false;
    // Allow trailing prerelease/build metadata; ignore.
    return true;
}

static int compare_semver(const std::string& a, const std::string& b) {
    int aM, am, ap, bM, bm, bp;
    bool va = parse_semver_triple(a, aM, am, ap);
    bool vb = parse_semver_triple(b, bM, bm, bp);
    // Unparseable versions sort as "newer" (i.e. tolerate) — pre-#2070
    // behavior was zero gating, so on parse failure we fail open.
    if (!va || !vb) return 0;
    if (aM != bM) return aM < bM ? -1 : 1;
    if (am != bm) return am < bm ? -1 : 1;
    if (ap != bp) return ap < bp ? -1 : 1;
    return 0;
}

// Per-tool min_sdk floors. Default for tools NOT in this table is
// "0.0.0" (no floor — runs on any project). Add an entry here when a
// tool's implementation begins to rely on a Pulp SDK API that landed
// in a specific release, so older projects get a clean upgrade nudge
// instead of a confusing runtime failure. The table is exposed via
// the `pulp_compat` introspection tool so plugins / clients can
// pre-filter their visible tool list if they want.
struct ToolMinSdk {
    const char* name;
    const char* min_sdk_version;
};
static const ToolMinSdk TOOL_MIN_SDK_TABLE[] = {
    // Format: {"pulp_<tool>", "x.y.z"}.
    // Currently empty — every existing tool runs against any project SDK.
    // Future tools that require a specific SDK API floor declare it here.
    {nullptr, nullptr},
};

static std::string min_sdk_for_tool(const std::string& name) {
    for (const auto& e : TOOL_MIN_SDK_TABLE) {
        if (!e.name) break;
        if (name == e.name) return e.min_sdk_version;
    }
    return "0.0.0";
}

static std::string compat_error_payload(const std::string& tool_name,
                                        const std::string& min_sdk,
                                        const std::string& project_sdk) {
    // isError:true content + structuredContent so LLM clients can read
    // either shape. The text is phrased so the LLM can suggest an
    // actionable fix to the user.
    std::string structured =
        std::string(R"JSON({"error":"sdk_too_old","tool":")JSON")
      + tool_name + R"JSON(","required_sdk":")JSON" + min_sdk
      + R"JSON(","project_sdk":")JSON" + project_sdk + R"JSON("})JSON";
    std::string text =
        "Tool `" + tool_name + "` requires project SDK >= " + min_sdk
        + "; this project is pinned to "
        + (project_sdk.empty() ? std::string("<unknown>") : project_sdk)
        + ". Bump via `pulp project bump`, or run the tool from a "
          "project that pins a newer SDK.";
    return "{\"content\":[{\"type\":\"text\",\"text\":"
         + json_string(text)
         + "}],\"structuredContent\":" + structured
         + ",\"isError\":true}";
}

// Build the JSON body for the `pulp_compat` introspection tool. Returns
// project_sdk, pulp_mcp_version, mcp_protocol_version, and a
// per-tool min_sdk map. Plugins / orchestrators can call this once at
// startup to decide which tools to surface.
static std::string handle_compat() {
    auto project_sdk = resolve_project_sdk_version();
    std::string body =
        std::string(R"JSON({"pulp_mcp_version":")JSON")
      + PULP_MCP_SERVER_VERSION
      + R"JSON(","mcp_protocol_version":"2024-11-05","project_sdk":)JSON"
      + (project_sdk.empty()
            ? std::string("null")
            : (std::string("\"") + project_sdk + "\""))
      + R"JSON(,"tool_min_sdk":{)JSON";
    bool first = true;
    for (const auto& e : TOOL_MIN_SDK_TABLE) {
        if (!e.name) break;
        if (!first) body += ",";
        body += std::string("\"") + e.name + "\":\"" + e.min_sdk_version + "\"";
        first = false;
    }
    body += "}}";
    return json_tool_payload(body);
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
{"name":"pulp_inspect_audio","description":"Get audio configuration and buffer underrun info from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_compat","description":"Report pulp-mcp / MCP protocol / project SDK versions plus per-tool min_sdk_version floors so clients can pre-filter their tool list. Use this once at startup to detect SDK skew (#2070).","inputSchema":{"type":"object","properties":{}}}
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

        // Per-tool feature detection (#2070). If the tool declares a
        // min_sdk floor and the project pins an older SDK, return a
        // structured error result with `isError: true` so the LLM gets
        // actionable upgrade guidance instead of silently running the
        // newer behavior. `pulp_compat` is exempt — clients invoke it
        // *to* discover skew and must always be able to read the
        // matrix. Tools left out of the table default to "0.0.0" so
        // existing behavior is unchanged.
        if (name != "pulp_compat") {
            auto min_sdk = min_sdk_for_tool(name);
            if (min_sdk != "0.0.0") {
                auto project_sdk = resolve_project_sdk_version();
                if (!project_sdk.empty() &&
                    compare_semver(project_sdk, min_sdk) < 0) {
                    return json_result(
                        id, compat_error_payload(name, min_sdk, project_sdk));
                }
                // If we couldn't resolve the project SDK at all we
                // fall open — same as pre-#2070. The launcher has
                // already started; gating on "no project root" would
                // make pulp-mcp unusable from `/tmp` or similar.
            }
        }

        std::string result;
        if (name == "pulp_compat")         result = handle_compat();
        else if (name == "pulp_build")          result = handle_build(args_json);
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

    // MCP spec: messages on the stdio transport are delimited by
    // newlines, and MUST NOT contain embedded newlines. Several of our
    // response builders (tools_list_json() most visibly) use multi-line
    // R"JSON(...)" raw strings for readability — the literal `\n`s in
    // those raw strings would break MCP clients (Claude Code reads one
    // line at a time and times out on the unclosed first line of the
    // tools array, surfacing as "MCP error -32001: Request timed out"
    // on tools/list).
    //
    // Strip `\n` and `\r` from any response body before it goes on the
    // wire. Tested: pre-fix tools/list response = 23 lines; post-fix = 1.
    auto compact_for_wire = [](std::string s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c != '\n' && c != '\r') out += c;
        }
        return out;
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // MCP uses Content-Length header framing
        if (line.find("Content-Length:") == 0) {
            int length = std::stoi(line.substr(15));
            std::getline(std::cin, line); // empty line
            std::string body(length, '\0');
            std::cin.read(body.data(), length);

            auto response = compact_for_wire(handle_request(body));
            if (!response.empty()) {
                std::cout << "Content-Length: " << response.size() << "\r\n\r\n" << response;
                std::cout.flush();
            }
            continue;
        }

        // Also handle bare JSON (for simpler testing AND the
        // newline-delimited MCP stdio transport that Claude Code uses).
        auto response = compact_for_wire(handle_request(line));
        if (!response.empty()) {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }

    return 0;
}
