#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
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
    require_contains(initialize, R"JSON("serverInfo":{"name":"pulp-mcp","version":"0.1.0"})JSON");

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
