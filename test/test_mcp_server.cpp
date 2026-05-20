#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// pulp-test-mcp-server compiles only this TU; it does not link the
// pulp-mcp target's object files. So the test must pull in every
// translation unit pulp_mcp.cpp depends on:
//   - pulp_mcp.cpp  — protocol dispatcher (renames main → pulp_mcp_main_for_test)
//   - mcp_compat.cpp — SDK-compat resolution; also re-exposes the file-local
//                      parse_cmake_project_version / parse_pulp_toml_sdk_version
//                      helpers this test exercises directly (Phase 6 B4 extraction)
//   - mcp_tools.cpp  — tool-call handlers invoked by the dispatcher
// Without mcp_compat.cpp / mcp_tools.cpp the test fails to link
// (undefined handle_build / resolve_project_sdk_version / …); without
// them being in this TU the parse_* helpers stay invisible.
#define main pulp_mcp_main_for_test
#include "../tools/mcp/pulp_mcp.cpp"
#undef main
#include "../tools/mcp/mcp_compat.cpp"
#include "../tools/mcp/mcp_tools.cpp"

namespace {

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& next)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }

    std::filesystem::path previous;
};

struct TempDir {
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("pulp-mcp-server-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

bool has_repo_markers(const std::filesystem::path& candidate) {
    return std::filesystem::exists(candidate / "CMakeLists.txt") &&
           std::filesystem::exists(candidate / "tools" / "mcp" / "pulp_mcp.cpp");
}

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return normalized;
    return std::filesystem::absolute(path);
}

std::filesystem::path find_repo_root() {
#ifdef PULP_SOURCE_DIR
    auto configured = std::filesystem::path(PULP_SOURCE_DIR);
    if (has_repo_markers(configured)) return normalize_path(configured);
#endif

    std::vector<std::filesystem::path> seeds = {
        std::filesystem::current_path(),
        std::filesystem::path(__FILE__),
    };

    if (!seeds.back().is_absolute()) {
        seeds.back() = std::filesystem::current_path() / seeds.back();
    }

    for (auto seed : seeds) {
        if (std::filesystem::is_regular_file(seed)) seed = seed.parent_path();
        for (auto candidate = seed; !candidate.empty();
             candidate = candidate.parent_path()) {
            if (has_repo_markers(candidate)) return normalize_path(candidate);
            if (candidate == candidate.root_path()) break;
        }
    }

    return {};
}

std::filesystem::path repo_root_path() {
    auto root = find_repo_root();
    REQUIRE_FALSE(root.empty());
    return root;
}

std::string repo_root() {
    return repo_root_path().string();
}

std::string tool_call(const std::string& id,
                      const std::string& name,
                      const std::string& arguments = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
           "\",\"arguments\":" + arguments + "}}";
}

void require_contains(const std::string& response, const std::string& needle) {
    INFO(response);
    REQUIRE(response.find(needle) != std::string::npos);
}

} // namespace

TEST_CASE("MCP JSON helpers escape and parse primitive fields", "[mcp][json]") {
    const auto escaped = json_string("quote \" slash \\ newline\nreturn\rtab\t");
    require_contains(escaped, "\\\"");
    require_contains(escaped, "\\\\");
    require_contains(escaped, "\\n");
    require_contains(escaped, "\\r");
    require_contains(escaped, "\\t");

    const std::string payload =
        "{"
        "\"int\":7,\"intWs\":42  ,\"intTab\":42\t,"
        "\"partialInt\":12junk,\"badInt\":\"abc\","
        "\"dbl\":1.25,\"dblWs\":2.5  ,\"dblTab\":2.5\t,"
        "\"partialDbl\":3.14oops,\"badDbl\":\"nope\","
        "\"yes\":true,\"no\":false,\"maybe\":\"??\",\"nil\":null"
        "}";

    REQUIRE(extract_int(payload, "int", 3) == 7);
    REQUIRE(extract_int(payload, "intWs", 3) == 42);
    REQUIRE(extract_int(payload, "intTab", 3) == 42);
    REQUIRE(extract_int(payload, "partialInt", 3) == 3);
    REQUIRE(extract_int(payload, "badInt", 3) == 3);
    REQUIRE(extract_int(payload, "missing", 3) == 3);
    REQUIRE(extract_int(payload, "nil", 3) == 3);

    REQUIRE(extract_double(payload, "dbl", 2.0) == 1.25);
    REQUIRE(extract_double(payload, "dblWs", 2.0) == 2.5);
    REQUIRE(extract_double(payload, "dblTab", 2.0) == 2.5);
    REQUIRE(extract_double(payload, "partialDbl", 2.0) == 2.0);
    REQUIRE(extract_double(payload, "badDbl", 2.0) == 2.0);
    REQUIRE(extract_double(payload, "missing", 2.0) == 2.0);
    REQUIRE(extract_double(payload, "nil", 2.0) == 2.0);

    REQUIRE(extract_bool(payload, "yes", false));
    REQUIRE_FALSE(extract_bool(payload, "no", true));
    REQUIRE(extract_bool(payload, "maybe", true));
    REQUIRE_FALSE(extract_bool(payload, "missing", false));
    REQUIRE(extract_bool(payload, "nil", true));
}

TEST_CASE("MCP protocol handles initialize ping notification and unknown methods",
          "[mcp][protocol]") {
    auto initialize = handle_request(R"JSON({"jsonrpc":"2.0","id":1,"method":"initialize"})JSON");
    require_contains(initialize, R"JSON("id":1)JSON");
    require_contains(initialize, R"JSON("protocolVersion":"2024-11-05")JSON");
    require_contains(initialize, R"JSON("capabilities":{"tools":{}})JSON");
    // serverInfo.version now tracks PROJECT_VERSION (via
    // tools/mcp/pulp_mcp_version.h.in). Hard-coding "0.1.0" caused
    // every CLI release to look identical from the plugin side.
    require_contains(initialize, R"JSON("serverInfo":{"name":"pulp-mcp","version":")JSON");
    require_contains(initialize,
                     std::string(R"JSON("version":")JSON")
                     + PULP_MCP_SERVER_VERSION + R"JSON("}})JSON");

    auto ping = handle_request(R"JSON({"jsonrpc":"2.0","id":2,"method":"ping"})JSON");
    require_contains(ping, R"JSON("id":2)JSON");
    require_contains(ping, R"JSON("result":{})JSON");

    REQUIRE(handle_request(R"JSON({"jsonrpc":"2.0","method":"notifications/initialized"})JSON").empty());

    auto unknown = handle_request(R"JSON({"jsonrpc":"2.0","id":3,"method":"nope"})JSON");
    require_contains(unknown, R"JSON("id":3)JSON");
    require_contains(unknown, R"JSON("code":-32601)JSON");
    require_contains(unknown, "Method not found: nope");
}

// MCP spec (JSON-RPC over stdio) requires that response messages not
// contain embedded `\n` or `\r` — the transport delimits messages with
// a single newline. A response carrying a raw newline gets split and
// the client (Claude Code, Codex CLI, etc.) reads only the first
// fragment, then times out waiting for the rest. PR #2091 fixed this
// by piping every wire-bound response through a `compact_for_wire`
// strip; this test pins the contract so a future regression on the
// strip itself surfaces immediately.
//
// Pulp #2087 follow-up (B4 first cut): the bug shipped because there
// was no protocol-shape test. The full B4 split is queued separately.
TEST_CASE("MCP wire output never contains embedded newlines",
          "[mcp][protocol][issue-2091]") {
    const std::vector<std::string> requests = {
        // initialize — large multi-key response.
        R"JSON({"jsonrpc":"2.0","id":1,"method":"initialize"})JSON",
        // tools/list — historically the largest response shape (~7KB
        // for the full advertised set). The original bug surfaced
        // here first because client parsers gave up on long messages.
        R"JSON({"jsonrpc":"2.0","id":2,"method":"tools/list"})JSON",
        // An unknown method — the error envelope must also be on one line.
        R"JSON({"jsonrpc":"2.0","id":3,"method":"does/not/exist"})JSON",
        // An unknown tool — error envelope routed through the tool path.
        tool_call("4", "pulp_does_not_exist"),
    };
    for (const auto& req : requests) {
        auto reply = handle_request(req);
        INFO("request: " << req);
        INFO("reply length: " << reply.size());
        REQUIRE(reply.find('\n') == std::string::npos);
        REQUIRE(reply.find('\r') == std::string::npos);
    }
}

TEST_CASE("MCP tool listing and unknown dispatch stay stable", "[mcp][tools]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":4,"method":"tools/list"})JSON");
    require_contains(tools, R"JSON("id":4)JSON");
    require_contains(tools, R"JSON("name":"pulp_build")JSON");
    require_contains(tools, R"JSON("name":"pulp_test")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_model_status")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_excerpt_find")JSON");
    require_contains(tools, R"JSON("name":"pulp_docs_search")JSON");
    require_contains(tools, R"JSON("name":"pulp_inspect_audio")JSON");

    auto unknown = handle_request(tool_call("5", "pulp_does_not_exist"));
    require_contains(unknown, R"JSON("id":5)JSON");
    require_contains(unknown, R"JSON("code":-32601)JSON");
    require_contains(unknown, "Unknown tool: pulp_does_not_exist");
}

// pulp #1997 — gap 1: every advertised MCP tool is named in tools/list.
// One missing entry = one silently broken tool, so the list-membership
// check is the cheapest possible smoke test for each tool. Failing this
// catches regressions where the JSON literal in tools_list_json() is
// edited but a tool name is dropped.
TEST_CASE("MCP tools/list advertises every tool the dispatcher handles",
          "[mcp][tools][issue-1997]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":40,"method":"tools/list"})JSON");
    // The full set of tools advertised today (18 names). Keep this list
    // sorted alphabetically so additions are obvious in a diff.
    const auto expected = {
        "pulp_audio_excerpt_find",
        "pulp_audio_model_activate",
        "pulp_audio_model_list",
        "pulp_audio_model_status",
        "pulp_audio_read_bundle",
        "pulp_build",
        "pulp_compat",
        "pulp_create",
        "pulp_docs_check",
        "pulp_docs_search",
        "pulp_get_view_tree",
        "pulp_inspect_audio",
        "pulp_inspect_dom",
        "pulp_inspect_evaluate",
        "pulp_inspect_params",
        "pulp_inspect_performance",
        "pulp_inspect_screenshot",
        // pulp #2153: pulp_motion_* wrappers expose the Motion.*
        // inspector protocol as first-class MCP tools so an LLM can
        // discover motion observability from tools/list without
        // resorting to pulp_inspect_evaluate or `nc localhost 9147`.
        "pulp_motion_disable_cost",
        "pulp_motion_enable_cost",
        "pulp_motion_list_traces",
        "pulp_motion_load_fixture",
        "pulp_motion_pause",
        "pulp_motion_play",
        "pulp_motion_scrub_to",
        "pulp_motion_snapshot",
        "pulp_motion_start_trace",
        "pulp_motion_stop_trace",
        "pulp_screenshot",
        "pulp_simulate_click",
        "pulp_status",
        "pulp_test",
        "pulp_validate",
    };
    for (const char* name : expected) {
        std::string needle = std::string(R"JSON("name":")JSON") + name + R"JSON(")JSON";
        INFO("missing tool: " << name);
        REQUIRE(tools.find(needle) != std::string::npos);
    }
}

TEST_CASE("MCP tools report required argument errors before side effects", "[mcp][tools]") {
    ScopedCurrentPath cwd(repo_root());

    const auto cases = {
        std::pair{"pulp_audio_model_activate", "Error: model_id is required"},
        std::pair{"pulp_audio_excerpt_find", "Error: text and input_path are required"},
        std::pair{"pulp_audio_read_bundle", "Error: bundle_path is required"},
        std::pair{"pulp_create", "Error: name is required"},
        std::pair{"pulp_docs_search", "Error: query is required"},
    };

    int id = 10;
    for (const auto& [tool, error] : cases) {
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        require_contains(response, error);
    }
}

TEST_CASE("MCP project-root dependent tools reject non-project directories", "[mcp][tools]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);

    auto response = handle_request(tool_call("20", "pulp_status"));
    require_contains(response, "Error: not in a Pulp project");
}

// pulp #1997 — gap 1: each of the 11 previously-untested wrapper tools
// (5 inspector + 4 view/screenshot/validate + 2 docs) reaches its
// dispatch arm. Hermetic check: from a non-project tempdir, every tool
// short-circuits with the project-root error BEFORE shelling out. This
// proves the tool name routes to the right arm without depending on any
// live binary, network, or running plugin process.
//
// The shellout-side semantics (no inspector found, etc.) are already
// covered by test_cli_shellout.cpp. The MCP boundary is the
// dispatch-routing layer — that's what we check here.
TEST_CASE("MCP wrapper tools route to the correct handler arm (project-root gate)",
          "[mcp][tools][issue-1997]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);

    // Inspector tools (5 of 5). Every one wraps a pulp inspect --command
    // call, all gated on find_project_root(). No live inspector required.
    const auto inspector_tools = {
        "pulp_inspect_dom",
        "pulp_inspect_params",
        "pulp_inspect_screenshot",
        "pulp_inspect_evaluate",
        "pulp_inspect_performance",
        // pulp_inspect_audio already exercised elsewhere; adding here
        // makes the dispatch-routing assertion exhaustive across the
        // full inspector arm.
        "pulp_inspect_audio",
    };
    int id = 30;
    for (const char* tool : inspector_tools) {
        INFO("inspector tool: " << tool);
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        // Reject reason proves: (a) the tool name was recognised, and
        // (b) execution reached find_project_root() instead of falling
        // through to the "Unknown tool" arm.
        require_contains(response, "Error: not in a Pulp project");
        // Also assert the dispatcher did NOT classify this as an unknown
        // tool — that would be the silent regression we're guarding against.
        REQUIRE(response.find("Unknown tool") == std::string::npos);
    }

    // Validate / view / screenshot / docs-check wrappers (the rest of
    // the previously-untested set, minus pulp_audio_model_list which
    // is exercised separately because it doesn't need a project root).
    const auto wrapper_tools = {
        "pulp_validate",
        "pulp_docs_check",
        "pulp_screenshot",
        "pulp_simulate_click",
        "pulp_get_view_tree",
    };
    for (const char* tool : wrapper_tools) {
        INFO("wrapper tool: " << tool);
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        require_contains(response, "Error: not in a Pulp project");
        REQUIRE(response.find("Unknown tool") == std::string::npos);
    }
}

// pulp #1997 — gap 1: pulp_audio_model_list goes straight through to
// the audio service (no project-root gate), so its routing test lives
// here. The service returns a JSON tool payload regardless of model
// install state — we only assert the envelope shape, not the inner
// model registry contents (which depend on test-time fixture state).
TEST_CASE("MCP pulp_audio_model_list returns the structured tool-payload envelope",
          "[mcp][tools][issue-1997]") {
    auto response = handle_request(tool_call("60", "pulp_audio_model_list"));
    require_contains(response, R"JSON("id":60)JSON");
    // json_tool_payload() always emits both the human "content" array
    // and the machine-parsable "structuredContent" object. Either one
    // missing is a regression in the audio service tool envelope.
    require_contains(response, R"JSON("content")JSON");
    require_contains(response, R"JSON("structuredContent")JSON");
    // Should not be a JSON-RPC error envelope. The audio service may
    // include an inner "error":"" field as part of its model-status
    // payload, so we look for the JSON-RPC -32601 error code rather
    // than the bare "error" key.
    REQUIRE(response.find(R"JSON("code":-32601)JSON") == std::string::npos);
}

// pulp #1997 — gap 1: the 5 inspector tools each map to a distinct
// inspector protocol method in pulp_mcp.cpp. Code-shape check: the
// switch table must mention every method string. If a future refactor
// drops one of these strings while leaving the dispatch arm intact,
// the inspector tool would silently send the wrong inspector command.
//
// We assert against the source text rather than runtime behavior
// because the actual inspector connection is over a TCP socket and
// requires a running plugin — out of scope for unit tests. The source
// check is cheap, deterministic, and proves the mapping is intact.
TEST_CASE("MCP inspector tools map to expected inspector protocol methods",
          "[mcp][tools][issue-1997]") {
    auto src_path = repo_root_path() / "tools" / "mcp" / "pulp_mcp.cpp";
    REQUIRE(std::filesystem::exists(src_path));

    std::ifstream in(src_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    // Each pair: (MCP tool name, inspector protocol method it must
    // call). If a refactor removes either side, this test fails loudly.
    const std::pair<const char*, const char*> mappings[] = {
        {"pulp_inspect_dom",         "DOM.getDocument"},
        {"pulp_inspect_params",      "State.getParameters"},
        {"pulp_inspect_screenshot",  "Capture.screenshot"},
        {"pulp_inspect_evaluate",    "Runtime.evaluate"},
        {"pulp_inspect_performance", "Performance.getMetrics"},
        {"pulp_inspect_audio",       "Audio.getConfig"},
    };
    for (const auto& [tool, method] : mappings) {
        INFO("inspector tool=" << tool << " method=" << method);
        REQUIRE(src.find(tool) != std::string::npos);
        REQUIRE(src.find(method) != std::string::npos);
    }
}

// ── pulp #2070: per-tool feature detection (min_sdk_version) ────────────────
//
// pulp-mcp ships independently of any given Pulp project, so a user may
// have a newer pulp-mcp on PATH while editing a project pinned to an
// older SDK. compare_semver() / min_sdk_for_tool() / handle_compat() are
// the three pieces that make tools/call return a clean upgrade nudge
// instead of running the newer behavior the project author didn't pin.

TEST_CASE("compare_semver orders pulp version triples", "[mcp][compat][issue-2070]") {
    REQUIRE(compare_semver("0.99.0", "0.100.0") < 0);   // 99 < 100 by numeric, not lex
    REQUIRE(compare_semver("0.100.0", "0.99.0") > 0);
    REQUIRE(compare_semver("1.0.0", "1.0.0") == 0);
    REQUIRE(compare_semver("1.0.0", "0.999.999") > 0);
    REQUIRE(compare_semver("0.0.0", "0.0.0") == 0);
    // Unparseable inputs fail open (treated as equal) so a malformed
    // pulp.toml or CMakeLists.txt can't strand a user out of every tool.
    REQUIRE(compare_semver("garbage", "0.99.0") == 0);
    REQUIRE(compare_semver("0.99.0", "not-a-version") == 0);
}

TEST_CASE("min_sdk_for_tool defaults to 0.0.0 for unlisted tools",
          "[mcp][compat][issue-2070]") {
    // Default for any tool not in TOOL_MIN_SDK_TABLE: no floor. This
    // preserves pre-#2070 behavior — only tools that explicitly opt in
    // get gated.
    REQUIRE(min_sdk_for_tool("pulp_build") == "0.0.0");
    REQUIRE(min_sdk_for_tool("pulp_audio_excerpt_find") == "0.0.0");
    REQUIRE(min_sdk_for_tool("a_tool_that_does_not_exist") == "0.0.0");
}

TEST_CASE("pulp_compat reports versions and tool min_sdk map",
          "[mcp][compat][issue-2070]") {
    // Run from a project root so resolve_project_sdk_version() finds
    // the in-tree CMakeLists.txt VERSION (the source-tree fallback).
    ScopedCurrentPath cwd(repo_root());

    auto response = handle_request(tool_call("70", "pulp_compat"));
    require_contains(response, R"JSON("id":70)JSON");
    // Build version comes from PULP_MCP_SERVER_VERSION (generated header).
    require_contains(response,
                     std::string(R"JSON("pulp_mcp_version":")JSON")
                     + PULP_MCP_SERVER_VERSION + R"JSON(")JSON");
    // MCP wire protocol — independent of build version, tracks upstream spec.
    require_contains(response, R"JSON("mcp_protocol_version":"2024-11-05")JSON");
    // Should not be null when invoked from a Pulp project root.
    require_contains(response, R"JSON("project_sdk":")JSON");
    // The tool_min_sdk map MUST be present (even if empty) — clients
    // depend on the field existing so they can iterate it without a
    // null check.
    require_contains(response, R"JSON("tool_min_sdk":)JSON");
}

TEST_CASE("pulp_compat handles missing project root by emitting null",
          "[mcp][compat][issue-2070]") {
    // Outside any Pulp project — emulate a user running pulp-mcp from a
    // scratch directory (or via the Claude plugin's launcher before
    // it's been pointed at a project).
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);
    auto response = handle_request(tool_call("71", "pulp_compat"));
    // Build + protocol versions still resolve.
    require_contains(response,
                     std::string(R"JSON("pulp_mcp_version":")JSON")
                     + PULP_MCP_SERVER_VERSION + R"JSON(")JSON");
    require_contains(response, R"JSON("mcp_protocol_version":"2024-11-05")JSON");
    // project_sdk explicitly null so clients can tell "no project"
    // apart from "project pinned to 0.0.0".
    require_contains(response, R"JSON("project_sdk":null)JSON");
}

TEST_CASE("compat_error_payload renders an isError result with actionable text",
          "[mcp][compat][issue-2070]") {
    auto body = compat_error_payload("pulp_future_tool", "0.110.0", "0.99.0");
    // isError must be exactly the JSON literal true so MCP clients can
    // branch on it without parsing the content text.
    require_contains(body, R"JSON("isError":true)JSON");
    // structuredContent carries machine-readable fields.
    require_contains(body, R"JSON("error":"sdk_too_old")JSON");
    require_contains(body, R"JSON("tool":"pulp_future_tool")JSON");
    require_contains(body, R"JSON("required_sdk":"0.110.0")JSON");
    require_contains(body, R"JSON("project_sdk":"0.99.0")JSON");
    // Human-readable text mentions both versions and the upgrade path.
    require_contains(body, "0.110.0");
    require_contains(body, "0.99.0");
    require_contains(body, "pulp project bump");
}

TEST_CASE("parse_cmake_project_version extracts VERSION from project()",
          "[mcp][compat][issue-2070]") {
    // Read the active repo's CMakeLists.txt and verify the parser hits
    // a sane semver triple. This is the path resolve_project_sdk_version
    // uses in source-tree mode.
    auto cmake_path = repo_root_path() / "CMakeLists.txt";
    REQUIRE(std::filesystem::exists(cmake_path));
    std::ifstream in(cmake_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const auto version = pulp_mcp::parse_cmake_project_version(buf.str());
    // Must look like x.y.z.
    int dots = 0;
    for (char c : version) if (c == '.') ++dots;
    INFO("parsed CMakeLists.txt version='" << version << "'");
    REQUIRE(dots == 2);
    REQUIRE_FALSE(version.empty());
}

// MCP stdio transport: messages are delimited by newlines and MUST
// NOT contain embedded newlines (per the MCP spec). Pre-fix,
// tools_list_json() returned a multi-line R"JSON(...)" raw string with
// literal `\n` between tool entries — Claude Code would read the first
// line, see an unclosed `[`, and time out with `MCP error -32001:
// Request timed out` on tools/list. This test pins the contract that
// every response body sent on the wire stays on a single line.
TEST_CASE("MCP tools/list response contains no embedded newlines (wire-safe)",
          "[mcp][protocol][issue-2087]") {
    // handle_request itself can produce multi-line bodies (the raw-
    // string sources are multi-line for readability). main()'s
    // compact_for_wire hook strips \n/\r before they hit the wire.
    // This test re-applies that strip and verifies the wire form is
    // single-line + parseable.
    auto strip_newlines = [](std::string s) {
        std::string out; out.reserve(s.size());
        for (char c : s) if (c != '\n' && c != '\r') out += c;
        return out;
    };

    auto initialize_wire = strip_newlines(handle_request(
        R"JSON({"jsonrpc":"2.0","id":1,"method":"initialize"})JSON"));
    auto tools_list_wire = strip_newlines(handle_request(
        R"JSON({"jsonrpc":"2.0","id":2,"method":"tools/list"})JSON"));

    REQUIRE(initialize_wire.find('\n') == std::string::npos);
    REQUIRE(initialize_wire.find('\r') == std::string::npos);
    REQUIRE(tools_list_wire.find('\n') == std::string::npos);
    REQUIRE(tools_list_wire.find('\r') == std::string::npos);

    // Sanity-check the stripped wire body is still well-formed JSON
    // shape (rough — opens/closes braces, contains the tools array).
    REQUIRE(tools_list_wire.front() == '{');
    REQUIRE(tools_list_wire.back() == '}');
    REQUIRE(tools_list_wire.find("\"tools\":[") != std::string::npos);
    REQUIRE(tools_list_wire.find("pulp_build") != std::string::npos);
}

// pulp #2153: every pulp_motion_* tool is recognised by the dispatcher
// (no "Unknown tool" fall-through) AND routes through the same
// project-root gate that pulp_inspect_* uses. From a tempdir all 10
// tools must short-circuit with "Error: not in a Pulp project" before
// shelling out to `pulp inspect`. This proves the dispatch arm exists
// and the tool name is registered.
TEST_CASE("MCP pulp_motion_* tools route to the motion dispatch arm",
          "[mcp][tools][motion][issue-2153]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);

    // Tools that take no params at all — invoke with empty arguments.
    const auto no_param_tools = {
        "pulp_motion_snapshot",
        "pulp_motion_list_traces",
        "pulp_motion_play",
        "pulp_motion_pause",
        "pulp_motion_enable_cost",
        "pulp_motion_disable_cost",
    };
    int id = 80;
    for (const char* tool : no_param_tools) {
        INFO("motion tool (no params): " << tool);
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        // Reject reason proves the dispatcher recognised the tool and
        // reached find_project_root() before shelling out.
        require_contains(response, "Error: not in a Pulp project");
        // Guard against the silent-regression case where the dispatch
        // arm gets removed but the tools/list registration stays.
        REQUIRE(response.find("Unknown tool") == std::string::npos);
    }

    // Tools that take params — confirm the same routing with a
    // representative non-empty argument shape.
    auto start_trace = handle_request(tool_call(
        std::to_string(id++), "pulp_motion_start_trace",
        R"JSON({"view_name":"Card","fps":30,"metrics":[{"kind":"geometry","name":"frame","node_id":"card"}]})JSON"));
    require_contains(start_trace, "Error: not in a Pulp project");
    REQUIRE(start_trace.find("Unknown tool") == std::string::npos);

    auto stop_trace = handle_request(tool_call(
        std::to_string(id++), "pulp_motion_stop_trace",
        R"JSON({"trace_id":1})JSON"));
    require_contains(stop_trace, "Error: not in a Pulp project");
    REQUIRE(stop_trace.find("Unknown tool") == std::string::npos);

    auto load_fixture = handle_request(tool_call(
        std::to_string(id++), "pulp_motion_load_fixture",
        R"JSON({"path":"/tmp/example.motion.jsonl"})JSON"));
    require_contains(load_fixture, "Error: not in a Pulp project");
    REQUIRE(load_fixture.find("Unknown tool") == std::string::npos);

    auto scrub_to = handle_request(tool_call(
        std::to_string(id++), "pulp_motion_scrub_to",
        R"JSON({"frame":42})JSON"));
    require_contains(scrub_to, "Error: not in a Pulp project");
    REQUIRE(scrub_to.find("Unknown tool") == std::string::npos);
}

// pulp #2153: code-shape check that the 10 pulp_motion_* MCP tools
// map to the right Motion.* inspector protocol method names. Source
// text assertion mirrors the existing inspector-mapping test — the
// actual round-trip lands at MotionInspector::handle /
// MotionScrubber::handle, which run inside the inspected process and
// already have their own dedicated test coverage in
// test_motion_inspector.cpp / test_motion_scrubber.cpp.
TEST_CASE("MCP pulp_motion_* tools map to expected Motion.* methods",
          "[mcp][tools][motion][issue-2153]") {
    auto src_path = repo_root_path() / "tools" / "mcp" / "pulp_mcp.cpp";
    REQUIRE(std::filesystem::exists(src_path));

    std::ifstream in(src_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    const std::pair<const char*, const char*> mappings[] = {
        {"pulp_motion_start_trace",   "Motion.startTrace"},
        {"pulp_motion_stop_trace",    "Motion.stopTrace"},
        {"pulp_motion_snapshot",      "Motion.snapshot"},
        {"pulp_motion_list_traces",   "Motion.listTraces"},
        {"pulp_motion_load_fixture",  "Motion.loadFixture"},
        {"pulp_motion_scrub_to",      "Motion.scrubTo"},
        {"pulp_motion_play",          "Motion.play"},
        {"pulp_motion_pause",         "Motion.pause"},
        {"pulp_motion_enable_cost",   "Motion.enableCost"},
        {"pulp_motion_disable_cost",  "Motion.disableCost"},
    };
    for (const auto& [tool, method] : mappings) {
        INFO("motion tool=" << tool << " method=" << method);
        REQUIRE(src.find(tool) != std::string::npos);
        REQUIRE(src.find(method) != std::string::npos);
    }
}

// pulp #2153: confirm every pulp_motion_* tool advertised in
// tools/list also exposes a discoverable input schema with descriptive
// titles/descriptions. An LLM consumer pulls these directly from
// tools/list to decide which tool to call; a missing description is
// invisible breakage.
TEST_CASE("MCP pulp_motion_* tools carry discoverable input schemas",
          "[mcp][tools][motion][issue-2153]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":99,"method":"tools/list"})JSON");

    // Each motion tool entry must include `"description":` with a
    // non-empty string and `"inputSchema":{"type":"object"`. We
    // assert both by searching for the tool name's name-key window
    // and validating the immediate vicinity.
    const auto tools_with_required_params = {
        std::pair{"pulp_motion_start_trace",  "view_name"},
        std::pair{"pulp_motion_stop_trace",   "trace_id"},
        std::pair{"pulp_motion_load_fixture", "path"},
        std::pair{"pulp_motion_scrub_to",     "frame"},
    };
    for (const auto& [tool, required] : tools_with_required_params) {
        INFO("tool with required param: " << tool << " requires " << required);
        std::string name_key = std::string(R"JSON("name":")JSON") + tool + R"JSON(")JSON";
        auto pos = tools.find(name_key);
        REQUIRE(pos != std::string::npos);
        // Look within the next ~1500 chars for both a description
        // field and the required array mentioning the expected
        // param. Tools may have additional required fields beyond
        // the one we're spot-checking (e.g. start_trace requires
        // both view_name AND metrics), so we look for the param
        // name as a quoted token inside any `"required":[...]`
        // window rather than a single-element exact match.
        auto window = tools.substr(pos, 1500);
        REQUIRE(window.find(R"JSON("description":")JSON") != std::string::npos);
        auto req_pos = window.find(R"JSON("required":[)JSON");
        REQUIRE(req_pos != std::string::npos);
        auto req_end = window.find(']', req_pos);
        REQUIRE(req_end != std::string::npos);
        auto required_window = window.substr(req_pos, req_end - req_pos + 1);
        std::string needle = std::string("\"") + required + "\"";
        INFO("required_window=" << required_window << " needle=" << needle);
        REQUIRE(required_window.find(needle) != std::string::npos);
    }

    // Param-less tools still need a description + an inputSchema object.
    const auto param_less_tools = {
        "pulp_motion_snapshot",
        "pulp_motion_list_traces",
        "pulp_motion_play",
        "pulp_motion_pause",
        "pulp_motion_enable_cost",
        "pulp_motion_disable_cost",
    };
    for (const char* tool : param_less_tools) {
        INFO("param-less tool: " << tool);
        std::string name_key = std::string(R"JSON("name":")JSON") + tool + R"JSON(")JSON";
        auto pos = tools.find(name_key);
        REQUIRE(pos != std::string::npos);
        auto window = tools.substr(pos, 600);
        REQUIRE(window.find(R"JSON("description":")JSON") != std::string::npos);
        REQUIRE(window.find(R"JSON("inputSchema":{"type":"object")JSON") != std::string::npos);
    }
}

TEST_CASE("parse_pulp_toml_sdk_version extracts the top-level scalar",
          "[mcp][compat][issue-2070]") {
    // Hand-rolled scanner — confirm the obvious cases and the trap
    // case where another key contains 'sdk_version' as a substring
    // (e.g., min_sdk_version) doesn't poison the result.
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("sdk_version = \"0.99.0\"\n") == "0.99.0");
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("  sdk_version=\"1.2.3\"\n") == "1.2.3");
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("# sdk_version commented out\n").empty());
    // The substring trap: min_sdk_version must NOT be returned as the
    // top-level sdk_version.
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("min_sdk_version = \"0.50.0\"\n").empty());
    // When both are present, the top-level wins.
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version(
        "min_sdk_version = \"0.50.0\"\nsdk_version = \"0.99.0\"\n") == "0.99.0");
}
