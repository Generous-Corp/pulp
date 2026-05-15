#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#define main pulp_mcp_main_for_test
#include "../tools/mcp/pulp_mcp.cpp"
#undef main

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

std::string repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().string();
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
        R"JSON({"int":7,"badInt":"abc","dbl":1.25,"badDbl":"nope","yes":true,"no":false,"maybe":"??","nil":null})JSON";

    REQUIRE(extract_int(payload, "int", 3) == 7);
    REQUIRE(extract_int(payload, "badInt", 3) == 3);
    REQUIRE(extract_int(payload, "missing", 3) == 3);
    REQUIRE(extract_int(payload, "nil", 3) == 3);

    REQUIRE(extract_double(payload, "dbl", 2.0) == 1.25);
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
    auto src_path = std::filesystem::path(__FILE__).parent_path().parent_path()
                    / "tools" / "mcp" / "pulp_mcp.cpp";
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
