// mcp_compat.cpp — project SDK-compatibility resolution for pulp-mcp.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor. See mcp_compat.hpp for the public surface; the SDK-version
// parsing internals (pulp.toml / CMakeLists scanners, semver triple
// parser, per-tool min-SDK table) stay file-local (static) here.

#include "mcp_compat.hpp"
#include "mcp_json.hpp"
#include "pulp_mcp_version.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace pulp_mcp {

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
    // sdk_version key is either a top-level scalar or lives under the
    // generated [pulp] table. We deliberately do NOT pull in a TOML
    // library here: pulp-mcp is intentionally minimal-deps so its binary
    // stays small and load-fast.
    std::istringstream lines(body);
    std::string line;
    bool in_other_section = false;
    while (std::getline(lines, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        auto last = line.find_last_not_of(" \t\r");
        auto trimmed = line.substr(first, last - first + 1);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            auto section = trimmed.substr(1, trimmed.size() - 2);
            in_other_section = section != "pulp";
            continue;
        }
        if (in_other_section) continue;
        if (trimmed.rfind("sdk_version", 0) != 0) continue;
        auto key_end = std::string("sdk_version").size();
        if (trimmed.size() > key_end
            && trimmed[key_end] != ' ' && trimmed[key_end] != '\t'
            && trimmed[key_end] != '=') {
            continue;
        }
        auto eq = trimmed.find('=', key_end);
        if (eq == std::string::npos) continue;
        auto q1 = trimmed.find('"', eq);
        if (q1 == std::string::npos) continue;
        auto q2 = trimmed.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        return trimmed.substr(q1 + 1, q2 - q1 - 1);
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

std::string resolve_project_sdk_version() {
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

int compare_semver(const std::string& a, const std::string& b) {
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

std::string min_sdk_for_tool(const std::string& name) {
    for (const auto& e : TOOL_MIN_SDK_TABLE) {
        if (!e.name) break;
        if (name == e.name) return e.min_sdk_version;
    }
    return "0.0.0";
}

std::string compat_error_payload(const std::string& tool_name,
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
std::string handle_compat() {
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


}  // namespace pulp_mcp
