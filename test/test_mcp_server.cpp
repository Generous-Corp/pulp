#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mcp_server_test_support.hpp"
#include "inspector_client_test_support.hpp"

#include "mcp_compat.hpp"
#include "mcp_json.hpp"
#include "mcp_server.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"
#include "pulp_mcp_version.h"
#include "timeline_mcp_tools.h"

#include <pulp/inspect/agent_request_queue.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

namespace {

using namespace mcp_test;
using namespace pulp_mcp;
using namespace pulp_mcp::server;

class ScopedStreamRedirect {
  public:
    ScopedStreamRedirect(std::ostream& stream, std::streambuf* replacement)
        : stream_(stream), original_(stream.rdbuf(replacement)) {}

    ~ScopedStreamRedirect() {
        stream_.rdbuf(original_);
    }

    ScopedStreamRedirect(const ScopedStreamRedirect&) = delete;
    ScopedStreamRedirect& operator=(const ScopedStreamRedirect&) = delete;

  private:
    std::ostream& stream_;
    std::streambuf* original_;
};

class ScopedEnvVar {
  public:
    ScopedEnvVar(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            had_previous_ = true;
            previous_ = previous;
        }
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
#if defined(_WIN32)
        if (had_previous_) {
            _putenv_s(name_.c_str(), previous_.c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
#else
        if (had_previous_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

// Several tests below shell out to `git -C <tempdir> …` on throwaway repos.
// If this binary is launched from a git-invoked context — a hook (pre-push),
// `shipyard`, or any parent `git` process — then GIT_DIR / GIT_WORK_TREE are
// inherited in the environment. A set GIT_DIR *overrides* `git -C <dir>`
// repository discovery, so those commands would silently operate on the
// SOURCE worktree's repo instead of their temp repo: stray "initial" commits,
// throwaway branches, and `core.bare` flips in the shared config. Clearing the
// inherited git environment makes temp-repo git deterministic regardless of
// who launched the test. (The pre-push hook and local_diff_cover.sh scrub the
// same vars; this is the in-process belt-and-suspenders for other runners.)
void clear_inherited_git_env() {
    for (const char* var :
         {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_COMMON_DIR",
          "GIT_PREFIX", "GIT_NAMESPACE", "GIT_QUARANTINE_PATH"}) {
#if defined(_WIN32)
        _putenv_s(var, "");
#else
        ::unsetenv(var);
#endif
    }
}

bool has_repo_markers(const std::filesystem::path& candidate) {
    return std::filesystem::exists(candidate / "CMakeLists.txt") &&
           std::filesystem::exists(candidate / "tools" / "mcp" / "pulp_mcp.cpp");
}

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return normalized;
    return std::filesystem::absolute(path);
}

std::filesystem::path find_repo_root() {
#ifdef PULP_SOURCE_DIR
    auto configured = std::filesystem::path(PULP_SOURCE_DIR);
    if (has_repo_markers(configured))
        return normalize_path(configured);
#endif

    std::vector<std::filesystem::path> seeds = {
        std::filesystem::current_path(),
        std::filesystem::path(__FILE__),
    };

    if (!seeds.back().is_absolute()) {
        seeds.back() = std::filesystem::current_path() / seeds.back();
    }

    for (auto seed : seeds) {
        if (std::filesystem::is_regular_file(seed))
            seed = seed.parent_path();
        for (auto candidate = seed; !candidate.empty(); candidate = candidate.parent_path()) {
            if (has_repo_markers(candidate))
                return normalize_path(candidate);
            if (candidate == candidate.root_path())
                break;
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

void require_tool_name(const std::string& tools, std::string_view name) {
    const auto compact = "\"name\":\"" + std::string(name) + "\"";
    const auto formatted = "\"name\" : \"" + std::string(name) + "\"";
    INFO("missing tool: " << name);
    REQUIRE((tools.find(compact) != std::string::npos ||
             tools.find(formatted) != std::string::npos));
}

std::filesystem::path make_fake_pulp_cli(const std::filesystem::path& root) {
    const auto cli = root / "build" / "tools" / "cli" / "pulp";
    std::filesystem::create_directories(cli.parent_path());
    std::ofstream script(cli);
#if defined(_WIN32)
    script << "@echo off\r\n"
           << "echo fake-pulp";
#else
    script << "#!/bin/sh\n"
           << "printf 'fake-pulp'\n"
           << "for arg in \"$@\"; do printf ' [%s]' \"$arg\"; done\n";
#endif
    script.close();
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    return cli;
}

std::filesystem::path make_fake_inspector_cli(
    const std::filesystem::path& root) {
#if defined(_WIN32)
    const auto cli =
        root / "build" / "tools" / "cli" / "pulp-cpp.exe";
#else
    const auto cli = root / "build" / "tools" / "cli" / "pulp-cpp";
#endif
    std::filesystem::create_directories(cli.parent_path());
    std::ofstream script(cli);
#if defined(_WIN32)
    script << "fake";
#else
    script << "#!/bin/sh\n"
           << "printf 'fake-inspector'\n"
           << "for arg in \"$@\"; do printf ' [%s]' \"$arg\"; done\n";
#endif
    script.close();
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    return cli;
}

std::filesystem::path make_package_workflow_fake_pulp_cli(const std::filesystem::path& root,
                                                          const std::filesystem::path& log_path) {
    const auto cli = root / "build" / "tools" / "cli" / "pulp";
    std::filesystem::create_directories(cli.parent_path());
    std::ofstream script(cli);
#if defined(_WIN32)
    script << "@echo off\r\n"
           << "echo package workflow fake CLI is POSIX-only";
#else
    script << "#!/bin/sh\n"
           << "log=" << shell_quote(log_path.string()) << "\n"
           << "printf '%s\\n' \"$*\" >> \"$log\"\n"
           << "case \"$1 $2\" in\n"
           << "  'kit search') echo "
              "'{\"ok\":true,\"lane\":\"kit\",\"results\":[{\"id\":\"dev.pulp.fixtures.basic-ui-"
              "kit\",\"lane\":\"kit\"}]}' ;;\n"
           << "  'kit inspect') echo "
              "'{\"ok\":true,\"summary\":{\"id\":\"dev.pulp.fixtures.basic-ui-kit\"}}' ;;\n"
           << "  'kit plan') echo "
              "'{\"ok\":true,\"actions\":[{\"kind\":\"lock-entry\"},{\"kind\":\"generated-cmake\"}]"
              "}' ;;\n"
           << "  'kit apply') echo 'OK: Applied kit dev.pulp.fixtures.basic-ui-kit' ;;\n"
           << "  'content validate') echo "
              "'{\"ok\":true,\"content\":{\"id\":\"dev.pulp.fixtures.basic-content-pack\"}}' ;;\n"
           << "  'content preview') echo "
              "'{\"ok\":true,\"requires_restart\":false,\"install_policy\":[{\"kind\":\"presets\","
              "\"policy\":\"manual-rescan\"}]}' ;;\n"
           << "  'content install') echo 'OK: Installed content pack "
              "dev.pulp.fixtures.basic-content-pack' ;;\n"
           << "  'validate --json') echo '{\"ok\":true,\"reports\":[]}' ;;\n"
           << "  *) echo \"fake-pulp $*\" ;;\n"
           << "esac\n";
#endif
    script.close();
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    return cli;
}

std::filesystem::path make_fake_command(const std::filesystem::path& dir, const std::string& name,
                                        const std::string& label) {
    std::filesystem::create_directories(dir);
    const auto command = dir / name;
    std::ofstream script(command);
#if defined(_WIN32)
    script << "@echo off\r\n"
           << "echo " << label;
#else
    script << "#!/bin/sh\n"
           << "printf '" << label << "'\n"
           << "for arg in \"$@\"; do printf ' [%s]' \"$arg\"; done\n";
#endif
    script.close();
    std::filesystem::permissions(command,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    return command;
}

} // namespace

TEST_CASE("MCP JSON helpers escape and parse primitive fields", "[mcp][json]") {
    const auto escaped = json_string(std::string("quote \" slash \\ newline\nreturn\rtab\tunit") +
                                     static_cast<char>(0x01) + static_cast<char>(0x1f));
    require_contains(escaped, "\\\"");
    require_contains(escaped, "\\\\");
    require_contains(escaped, "\\n");
    require_contains(escaped, "\\r");
    require_contains(escaped, "\\t");
    require_contains(escaped, "\\u0001");
    require_contains(escaped, "\\u001f");

    const std::string payload = "{"
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

TEST_CASE("MCP JSON helpers preserve raw tokens and reject partial scalars", "[mcp][json]") {
    const std::string payload = "{"
                                "\"message\":\"hello \\\"pulp\\\"\","
                                "\"negative\":-12,\"positive\":+7,"
                                "\"floatExp\":6.25e-2,\"badFloat\":6.25e-2x,"
                                "\"truthy\":true,\"falsy\":false,"
                                "\"word\":\"true\",\"array\":[1,2],"
                                "\"spaced\":  5 \n,"
                                "\"tabbed\":\t9\t"
                                "}";

    REQUIRE(extract_string(payload, "message") == R"(hello \"pulp\")");
    REQUIRE(extract_string(payload, "negative").empty());
    REQUIRE(extract_raw(payload, "message") == "\"hello \\\"pulp\\\"\"");
    REQUIRE(extract_raw(payload, "negative") == "-12");
    REQUIRE(extract_raw(payload, "positive") == "+7");
    REQUIRE(extract_raw(payload, "floatExp") == "6.25e-2");
    REQUIRE(extract_raw(payload, "truthy") == "true");
    REQUIRE(extract_raw(payload, "falsy") == "false");
    REQUIRE(extract_raw(payload, "missing").empty());

    REQUIRE(extract_int(payload, "negative", 0) == -12);
    REQUIRE(extract_int(payload, "positive", 0) == 7);
    REQUIRE(extract_int(payload, "spaced", 0) == 5);
    REQUIRE(extract_int(payload, "tabbed", 0) == 9);
    REQUIRE(extract_int(payload, "floatExp", 99) == 99);
    REQUIRE(extract_int(payload, "array", 99) == 99);

    REQUIRE(extract_double(payload, "negative", 0.0) == -12.0);
    REQUIRE(extract_double(payload, "floatExp", 0.0) == 0.0625);
    REQUIRE(extract_double(payload, "badFloat", 1.5) == 1.5);
    REQUIRE(extract_double(payload, "array", 1.5) == 1.5);

    REQUIRE(extract_bool(payload, "truthy", false));
    REQUIRE_FALSE(extract_bool(payload, "falsy", true));
    REQUIRE(extract_bool(payload, "word", true));
    REQUIRE_FALSE(extract_bool(payload, "array", false));
}

TEST_CASE("MCP JSON helpers preserve nested arrays and objects", "[mcp][json]") {
    const std::string payload =
        R"JSON({"categories":["dsp","render,gpu",{"nested":[1,{"label":"x}y"}]}],"metrics":[{"kind":"geometry","properties":["x","y"]},{"kind":"scroll-geometry","source":{"axis":"vertical","options":{"clamp":true}}}],"tail":17})JSON";

    REQUIRE(extract_raw(payload, "categories") ==
            R"JSON(["dsp","render,gpu",{"nested":[1,{"label":"x}y"}]}])JSON");
    REQUIRE(extract_raw(payload, "metrics") ==
            R"JSON([{"kind":"geometry","properties":["x","y"]},{"kind":"scroll-geometry","source":{"axis":"vertical","options":{"clamp":true}}}])JSON");
    REQUIRE(extract_raw(payload, "tail") == "17");
}

TEST_CASE("MCP JSON helpers keep adjacent keys and string escapes isolated", "[mcp][json]") {
    const std::string payload =
        R"({"tool":"pulp_build","tool_extra":"wrong","text":"a \"quoted\" value","n":17})";

    REQUIRE(extract_string(payload, "tool") == "pulp_build");
    REQUIRE(extract_string(payload, "tool_extra") == "wrong");
    REQUIRE(extract_string(payload, "text") == R"(a \"quoted\" value)");
    REQUIRE(extract_raw(payload, "n") == "17");

    auto wrapped = json_tool_payload(R"({"ok":true,"value":"x"})");
    require_contains(wrapped, R"("structuredContent":{"ok":true,"value":"x"})");
    require_contains(wrapped, R"("text":"{\"ok\":true,\"value\":\"x\"}")");
}

TEST_CASE("MCP JSON helpers keep numeric-looking keys distinct", "[mcp][json]") {
    const std::string payload =
        R"({"id":5,"id2":99,"id_text":"5","method":"tools/call","methodExtra":"wrong"})";

    REQUIRE(extract_int(payload, "id", -1) == 5);
    REQUIRE(extract_int(payload, "id2", -1) == 99);
    REQUIRE(extract_int(payload, "id_text", -1) == -1);
    REQUIRE(extract_string(payload, "method") == "tools/call");
    REQUIRE(extract_string(payload, "methodExtra") == "wrong");
    REQUIRE(extract_raw(payload, "missing").empty());
}

TEST_CASE("MCP JSON-RPC envelopes escape structured payloads", "[mcp][json]") {
    const auto error = json_error("null", -32602, "bad \"arg\"\nline");
    require_contains(error, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(error, R"JSON("id":null)JSON");
    require_contains(error, R"JSON("code":-32602)JSON");
    require_contains(error, R"JSON("message":"bad \"arg\"\nline")JSON");
    REQUIRE(error.find('\n') == std::string::npos);

    const auto result = json_result("7", R"JSON({"ok":true,"count":3})JSON");
    REQUIRE(result == R"JSON({"jsonrpc":"2.0","id":7,"result":{"ok":true,"count":3}})JSON");

    const auto payload =
        json_tool_payload(R"JSON({"ok":true,"text":"a \"quoted\" value","items":[1,2]})JSON");
    require_contains(payload, R"JSON("content":[{"type":"text","text":")JSON");
    require_contains(payload, R"JSON(\"text\":\"a \\\"quoted\\\" value\")JSON");
    require_contains(
        payload,
        R"JSON("structuredContent":{"ok":true,"text":"a \"quoted\" value","items":[1,2]})JSON");
    REQUIRE(payload.find('\n') == std::string::npos);
    REQUIRE(payload.find(R"JSON("isError")JSON") == std::string::npos);
}

TEST_CASE("MCP shell_quote keeps shell arguments atomic", "[mcp][shell]") {
#if defined(_WIN32)
    REQUIRE(shell_quote(R"(C:\Program Files\Pulp\pulp.exe)") ==
            R"("C:\Program Files\Pulp\pulp.exe")");
    REQUIRE(shell_quote(R"(say "hi")") == "\"say \\\"hi\\\"\"");
#else
    REQUIRE(shell_quote("/tmp/Pulp Project/build") == "'/tmp/Pulp Project/build'");
    REQUIRE(shell_quote("MCP protocol's edge") == "'MCP protocol'\\''s edge'");
    REQUIRE(shell_quote("") == "''");
#endif
}

TEST_CASE("MCP CLI resolver finds Windows multi-config delegates", "[mcp][shell]") {
    TempDir project;
    const auto cli = project.path / "build" / "tools" / "cli" / "Release" / "pulp-cpp.exe";
    std::filesystem::create_directories(cli.parent_path());
    std::ofstream(cli) << "fake";

    REQUIRE(pulp_mcp::resolve_cli_binary(project.path) == cli);
}

TEST_CASE("MCP shell exec returns stdout and failure diagnostics", "[mcp][shell]") {
#if defined(_WIN32)
    auto ok = exec("cmd /c echo|set /p=pulp-mcp");
#else
    auto ok = exec("printf 'pulp-mcp'");
#endif
    REQUIRE(ok == "pulp-mcp");

#if defined(_WIN32)
    auto failed = exec("cmd /c exit 7");
#else
    auto failed = exec("sh -c 'exit 7'");
#endif
    REQUIRE(failed.find("Command failed with status") != std::string::npos);
}

TEST_CASE("MCP find_project_root walks upward and reports absence", "[mcp][shell]") {
    TempDir temp;
    auto project = temp.path / "project";
    auto nested = project / "plugins" / "demo";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directories(project / "core");
    {
        std::ofstream cmake(project / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.25)\n";
    }

    {
        ScopedCurrentPath cwd(nested);
        REQUIRE(std::filesystem::weakly_canonical(find_project_root()) ==
                std::filesystem::weakly_canonical(project));
    }

    auto not_project = temp.path / "not-project" / "child";
    std::filesystem::create_directories(not_project);
    {
        ScopedCurrentPath cwd(not_project);
        REQUIRE(find_project_root().empty());
    }
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
    require_contains(initialize, std::string(R"JSON("version":")JSON") + PULP_MCP_SERVER_VERSION +
                                     R"JSON("}})JSON");

    auto ping = handle_request(R"JSON({"jsonrpc":"2.0","id":2,"method":"ping"})JSON");
    require_contains(ping, R"JSON("id":2)JSON");
    require_contains(ping, R"JSON("result":{})JSON");

    REQUIRE(handle_request(R"JSON({"jsonrpc":"2.0","method":"notifications/initialized"})JSON")
                .empty());

    auto unknown = handle_request(R"JSON({"jsonrpc":"2.0","id":3,"method":"nope"})JSON");
    require_contains(unknown, R"JSON("id":3)JSON");
    require_contains(unknown, R"JSON("code":-32601)JSON");
    require_contains(unknown, "Method not found: nope");
}

TEST_CASE("MCP pulp_minos tool measures a binary and validates its arg", "[mcp][minos]") {
    // A project root so handle_minos gets past its "not in a Pulp project"
    // guard and exercises the full body (resolve CLI, shell out, return).
    TempDir temp;
    auto project = temp.path / "project";
    std::filesystem::create_directories(project / "core");
    {
        std::ofstream cmake(project / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.25)\n";
    }
    ScopedCurrentPath cwd(project);

    // pulp_minos appears in the advertised tool list.
    require_contains(tools_list_json(), R"JSON("name":"pulp_minos")JSON");

    // With a binary argument the whole handler runs and wraps the CLI's
    // measurement output in an MCP text-content envelope. (The measured value
    // depends on the host binary; we only assert the envelope shape so the test
    // is deterministic regardless of whether a CLI binary is resolvable here.)
    auto ok = handle_request(
        R"JSON({"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"pulp_minos","arguments":{"binary":"/some/binary"}}})JSON");
    require_contains(ok, R"JSON("id":41)JSON");
    require_contains(ok, R"JSON("content")JSON");

    // Missing the required binary is a clear, shell-out-free error.
    auto err = handle_request(
        R"JSON({"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"pulp_minos","arguments":{}}})JSON");
    require_contains(err, "binary is required");
}

TEST_CASE("pulp-mcp flag-only invocations do not enter the JSON-RPC loop", "[mcp][main]") {
    std::ostringstream out;
    std::ostringstream err;
    ScopedStreamRedirect capture_out(std::cout, out.rdbuf());
    ScopedStreamRedirect capture_err(std::cerr, err.rdbuf());

    char program[] = "pulp-mcp";
    char version[] = "--version";
    char help[] = "--help";
    char unknown[] = "--bad";

    char* version_argv[] = {program, version};
    REQUIRE(pulp_mcp::server::run(2, version_argv) == 0);
    require_contains(out.str(), std::string("pulp-mcp ") + PULP_MCP_SERVER_VERSION);
    REQUIRE(err.str().empty());

    out.str("");
    out.clear();
    err.str("");
    err.clear();
    char* help_argv[] = {program, help};
    REQUIRE(pulp_mcp::server::run(2, help_argv) == 0);
    require_contains(out.str(), "MCP (Model Context Protocol) server for Pulp.");
    require_contains(out.str(), "--version, -V");
    REQUIRE(err.str().empty());

    out.str("");
    out.clear();
    err.str("");
    err.clear();
    char* unknown_argv[] = {program, unknown};
    REQUIRE(pulp_mcp::server::run(2, unknown_argv) == 2);
    REQUIRE(out.str().empty());
    require_contains(err.str(), "unknown flag '--bad'");
}

// MCP spec (JSON-RPC over stdio) requires that response messages not
// contain embedded `\n` or `\r` — the transport delimits messages with
// a single newline. A response carrying a raw newline gets split and
// the client (Claude Code, Codex CLI, etc.) reads only the first
// fragment, then times out waiting for the rest. The fix pipes every wire-bound
// response through a `compact_for_wire` strip; this test pins the contract so a
// future regression on the strip itself surfaces immediately.
//
// Pulp #2087 follow-up (B4 first cut): the bug shipped because there
// was no protocol-shape test. The full B4 split is queued separately.
TEST_CASE("MCP wire output never contains embedded newlines", "[mcp][protocol][issue-2091]") {
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

TEST_CASE("MCP JSON scalar extraction rejects partial typed values", "[mcp][json]") {
    const std::string payload =
        R"JSON({"whole":12,"partial":"12x","truth":true,"lie":false,"nil":null,"float":3.25,"badFloat":"3.25x","quoted":"hello \"pulp\""})JSON";

    REQUIRE(extract_raw(payload, "whole") == "12");
    REQUIRE(extract_string(payload, "quoted") == R"JSON(hello \"pulp\")JSON");
    REQUIRE(extract_int(payload, "whole", -1) == 12);
    REQUIRE(extract_int(payload, "partial", -1) == -1);
    REQUIRE(extract_double(payload, "float", -1.0) == 3.25);
    REQUIRE(extract_double(payload, "badFloat", -1.0) == -1.0);
    REQUIRE(extract_bool(payload, "truth", false));
    REQUIRE_FALSE(extract_bool(payload, "lie", true));
    REQUIRE(extract_bool(payload, "nil", true));
    REQUIRE_FALSE(extract_bool(payload, "missing", false));
}

TEST_CASE("MCP tool listing and unknown dispatch stay stable", "[mcp][tools]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":4,"method":"tools/list"})JSON");
    require_contains(tools, R"JSON("id":4)JSON");
    require_contains(tools, R"JSON("name":"pulp_build")JSON");
    require_contains(tools, R"JSON("name":"pulp_test")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_model_status")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_excerpt_find")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_probe_json")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_scope")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_plugin_inspect")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_render")JSON");
    require_contains(tools, R"JSON("name":"pulp_audio_compare")JSON");
    require_contains(tools, R"JSON("name":"pulp_docs_search")JSON");
    require_contains(tools, R"JSON("name":"pulp_inspect_audio")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit_search")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit_validate")JSON");
    require_contains(tools, "validation.generatedProjectDiffs review evidence");
    require_contains(tools, R"JSON("name":"pulp_kit_plan")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit_verify")JSON");
    require_contains(tools, "UI-kit integration preview artifacts");
    require_contains(tools, R"JSON("name":"pulp_kit_apply")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit_remove")JSON");
    require_contains(tools, "constrained lock-recorded kit paths");
    require_contains(tools, R"JSON("name":"pulp_kit_pack")JSON");
    require_contains(tools, R"JSON("name":"pulp_kit_publish_check")JSON");
    require_contains(tools, R"JSON("name":"pulp_content")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_validate")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_preview")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_install")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_update")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_list")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_rescan")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_remove")JSON");
    require_contains(tools, R"JSON("name":"pulp_content_reveal")JSON");
    require_tool_name(tools, "pulp_timeline_project_open");
    require_tool_name(tools, "pulp_timeline_command_apply");
    require_tool_name(tools, "pulp_timeline_diff");
    require_tool_name(tools, "pulp_timeline_undo");
    require_tool_name(tools, "pulp_timeline_redo");
    require_tool_name(tools, "pulp_timeline_validate");
    require_tool_name(tools, "pulp_timeline_explain");
    require_tool_name(tools, "pulp_timeline_render");
    require_tool_name(tools, "pulp_timeline_export");
    require_tool_name(tools, "pulp_timeline_import");
    require_contains(tools, "pulp.timeline.command.set_clip_playback_properties");

    auto unknown = handle_request(tool_call("5", "pulp_does_not_exist"));
    require_contains(unknown, R"JSON("id":5)JSON");
    require_contains(unknown, R"JSON("code":-32601)JSON");
    require_contains(unknown, "Unknown tool: pulp_does_not_exist");
}

TEST_CASE("generated timeline MCP names are advertised and callable", "[mcp][tools][timeline]") {
    const auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":41,"method":"tools/list"})JSON");
    for (const auto name : pulp_mcp::kTimelineMcpToolNames) {
        require_tool_name(tools, name);
    }

    std::ifstream fixture(repo_root_path() / "test/fixtures/timeline/v1/minimal.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    const auto project_args = "{\"project\":" + json_string(project) + "}";

    require_contains(
        handle_request(tool_call("410", std::string(kTimelineProjectOpenToolName), project_args)),
        R"JSON("project":)JSON");
    require_contains(
        handle_request(tool_call("411", std::string(kTimelineCommandApplyToolName),
                                 "{\"commands\":[],\"project\":" + json_string(project) + "}")),
        "commands must be a non-empty array");
    require_contains(handle_request(tool_call("412", std::string(kTimelineDiffToolName), "{}")),
                     "session_id is required");
    require_contains(handle_request(tool_call("413", std::string(kTimelineUndoToolName), "{}")),
                     "session_id is required");
    require_contains(handle_request(tool_call("414", std::string(kTimelineRedoToolName), "{}")),
                     "session_id is required");
    require_contains(
        handle_request(tool_call("415", std::string(kTimelineValidateToolName), project_args)),
        R"JSON("diagnostics":[])JSON");
    require_contains(
        handle_request(tool_call("416", std::string(kTimelineExplainToolName), project_args)),
        R"JSON("tracks":[])JSON");
    require_contains(
        handle_request(tool_call("417", std::string(kTimelineRenderToolName), project_args)),
        "project and output are required");
    require_contains(
        handle_request(tool_call("418", std::string(kTimelineExportToolName), project_args)),
        "project and format are required");
    require_contains(handle_request(tool_call("419", std::string(kTimelineImportToolName), "{}")),
                     "input, format, and output are required");
}

TEST_CASE("generated timeline bindings preserve protocol apply undo redo lifecycle",
          "[mcp][tools][timeline][iteration]") {
    std::ifstream fixture(repo_root_path() / "test/fixtures/timeline/v4/sequence-markers.json");
    REQUIRE(fixture);
    const std::string project{std::istreambuf_iterator<char>(fixture),
                              std::istreambuf_iterator<char>()};
    const auto opened = handle_request(tool_call("420", std::string(kTimelineProjectOpenToolName),
                                                 "{\"project\":" + json_string(project) + "}"));
    const auto session_id = extract_string(opened, "session_id");
    REQUIRE_FALSE(session_id.empty());
    const auto session_argument = "\"session_id\":" + json_string(session_id);
    const std::string commands =
        R"JSON([{"data":{"marker":{"data":{"id":"8","name":"protocol-marker","position":"0"},"type_name":"pulp.timeline.marker","version":1},"sequence_id":"2"},"type_name":"pulp.timeline.command.insert_marker","version":1}])JSON";

    const auto applied =
        handle_request(tool_call("421", std::string(kTimelineCommandApplyToolName),
                                 "{\"commands\":" + commands + "," + session_argument + "}"));
    require_contains(applied, "protocol-marker");
    require_contains(applied, R"JSON("can_undo":true)JSON");

    const auto undone = handle_request(
        tool_call("422", std::string(kTimelineUndoToolName), "{" + session_argument + "}"));
    REQUIRE(undone.find("protocol-marker") == std::string::npos);
    require_contains(undone, R"JSON("can_redo":true)JSON");

    const auto redone = handle_request(
        tool_call("423", std::string(kTimelineRedoToolName), "{" + session_argument + "}"));
    require_contains(redone, "protocol-marker");
    require_contains(redone, R"JSON("can_undo":true)JSON");
}

// pulp #1997 — gap 1: every advertised MCP tool is named in tools/list.
// One missing entry = one silently broken tool, so the list-membership
// check is the cheapest possible smoke test for each tool. Failing this
// catches regressions where the JSON literal in tools_list_json() is
// edited but a tool name is dropped.
TEST_CASE("MCP tools/list advertises every tool the dispatcher handles",
          "[mcp][tools][issue-1997]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":40,"method":"tools/list"})JSON");
    // The full set of tools advertised today. Keep this list
    // sorted alphabetically so additions are obvious in a diff.
    const auto expected = {
        "pulp_audio_compare",
        "pulp_audio_excerpt_find",
        "pulp_audio_model_activate",
        "pulp_audio_model_list",
        "pulp_audio_model_status",
        "pulp_audio_plugin_inspect",
        "pulp_audio_probe_json",
        "pulp_audio_read_bundle",
        "pulp_audio_render",
        "pulp_audio_scope",
        "pulp_build",
        "pulp_compat",
        "pulp_content",
        "pulp_content_install",
        "pulp_content_list",
        "pulp_content_preview",
        "pulp_content_rescan",
        "pulp_content_remove",
        "pulp_content_reveal",
        "pulp_content_update",
        "pulp_content_validate",
        "pulp_create",
        "pulp_docs_check",
        "pulp_docs_search",
        "pulp_get_view_tree",
        "pulp_inspect_audio",
        "pulp_inspect_capabilities",
        "pulp_inspect_context",
        "pulp_inspect_dom",
        "pulp_inspect_evaluate",
        "pulp_inspect_inject_midi",
        "pulp_inspect_list",
        "pulp_inspect_params",
        "pulp_inspect_performance",
        "pulp_inspect_screenshot",
        "pulp_inspect_set_param",
        "pulp_inspect_set_transport",
        "pulp_inspect_value_channels",
        "pulp_kit",
        "pulp_kit_apply",
        "pulp_kit_init",
        "pulp_kit_inspect",
        "pulp_kit_plan",
        "pulp_kit_search",
        "pulp_kit_verify",
        "pulp_kit_validate",
        "pulp_kit_remove",
        "pulp_kit_pack",
        "pulp_kit_publish_check",
        // pulp_motion_* wrappers expose the Motion.*
        // inspector protocol as first-class MCP tools so an LLM can
        // discover motion observability from tools/list without
        // resorting to pulp_inspect_evaluate or `nc localhost 9147`.
        "pulp_motion_disable_cost",
        "pulp_motion_enable_cost",
        "pulp_motion_list_traces",
        "pulp_motion_pause",
        "pulp_motion_play",
        "pulp_motion_scrub_to",
        "pulp_motion_snapshot",
        "pulp_motion_start_trace",
        "pulp_motion_stop_trace",
        // pulp_trace_* wrappers expose the Perfetto Trace.* live-session
        // inspector RPCs as first-class MCP tools (client-side doctor / open /
        // fetch and offline `query --trace` have no inspector RPC, so no tool).
        "pulp_trace_start",
        "pulp_trace_stop",
        "pulp_trace_snapshot",
        "pulp_trace_query",
        "pulp_trace_explain",
        "pulp_screenshot",
        "pulp_simulate_click",
        "pulp_status",
        "pulp_test",
        "pulp_timeline_command_apply",
        "pulp_timeline_diff",
        "pulp_timeline_explain",
        "pulp_timeline_export",
        "pulp_timeline_import",
        "pulp_timeline_project_open",
        "pulp_timeline_redo",
        "pulp_timeline_render",
        "pulp_timeline_undo",
        "pulp_timeline_validate",
        "pulp_validate",
    };
    for (const char* name : expected) {
        require_tool_name(tools, name);
    }
}

TEST_CASE("MCP tools report required argument errors before side effects", "[mcp][tools]") {
    ScopedCurrentPath cwd(repo_root());

    const auto cases = {
        std::pair{"pulp_audio_model_activate", "Error: model_id is required"},
        std::pair{"pulp_audio_excerpt_find", "Error: text and input_path are required"},
        std::pair{"pulp_audio_read_bundle", "Error: bundle_path is required"},
        std::pair{"pulp_audio_plugin_inspect", "Error: plugin is required"},
        std::pair{"pulp_audio_render", "Error: plugin is required"},
        std::pair{"pulp_audio_compare", "Error: reference and candidate are required"},
        std::pair{"pulp_create", "Error: name is required"},
        std::pair{"pulp_docs_search", "Error: query is required"},
        std::pair{"pulp_content", "Error: subcommand is required"},
        std::pair{"pulp_content_validate", "Error: path is required"},
        std::pair{"pulp_content_preview", "Error: path is required"},
        std::pair{"pulp_content_install", "Error: path is required"},
        std::pair{"pulp_content_update", "Error: path is required"},
        std::pair{"pulp_content_remove", "Error: id is required"},
        std::pair{"pulp_content_reveal", "Error: id is required"},
        std::pair{"pulp_kit", "Error: subcommand is required"},
        std::pair{"pulp_kit_validate", "Error: path is required"},
        std::pair{"pulp_kit_inspect", "Error: path is required"},
        std::pair{"pulp_kit_plan", "Error: path is required"},
        std::pair{"pulp_kit_verify", "Error: path is required"},
        std::pair{"pulp_kit_apply", "Error: path is required"},
        std::pair{"pulp_kit_remove", "Error: id is required"},
        std::pair{"pulp_kit_pack", "Error: path is required"},
        std::pair{"pulp_kit_publish_check", "Error: path is required"},
        std::pair{"pulp_kit_init", "Error: kind and id are required"},
        std::pair{"pulp_timeline_project_open", "Error: project is required"},
        std::pair{"pulp_timeline_command_apply",
                  "Error: exactly one of project or session_id is required"},
        std::pair{"pulp_timeline_diff", "Error: session_id is required"},
        std::pair{"pulp_timeline_undo", "Error: session_id is required"},
        std::pair{"pulp_timeline_redo", "Error: session_id is required"},
        std::pair{"pulp_timeline_validate", "Error: project is required"},
        std::pair{"pulp_timeline_explain", "Error: project is required"},
        std::pair{"pulp_timeline_render", "Error: project and output are required"},
        std::pair{"pulp_timeline_export", "Error: project and format are required"},
        std::pair{"pulp_timeline_import", "Error: input, format, and output are required"},
    };

    int id = 10;
    for (const auto& [tool, error] : cases) {
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        require_contains(response, error);
    }
}

TEST_CASE("pulp_inspect_set_param uses live discovery instead of a CLI delegate",
          "[mcp][tools][mcp-set-param]") {
#if defined(_WIN32)
    SUCCEED("The Windows multi-config pulp-cpp path is covered by the resolver test");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\n";
    make_fake_inspector_cli(project.path);
    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    auto response = handle_request(tool_call(
        "60", "pulp_inspect_set_param",
        R"({"id":0,"value":1.0,"normalized":true,"session_id":"session-a","instance_id":"instance-b","publication_id":"publication-c"})"));
    require_contains(response, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(response, "session_selection_failed");
    require_contains(response, R"JSON("structuredContent")JSON");
    REQUIRE(response.find("fake-inspector") == std::string::npos);
#endif
}

TEST_CASE("MCP inspector families use the shared in-process client",
          "[mcp][tools][inspect][delegate]") {
#if defined(_WIN32)
    SUCCEED("The Windows multi-config pulp-cpp path is covered by the resolver test");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\n";
    make_fake_inspector_cli(project.path);
    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    const std::pair<const char*, const char*> cases[] = {
        {"pulp_inspect_dom", "DOM.getDocument"},
        {"pulp_motion_snapshot", "Motion.snapshot"},
        {"pulp_trace_snapshot", "Trace.snapshot"},
    };
    int id = 61;
    for (const auto& [tool, method] : cases) {
        INFO("tool=" << tool << " method=" << method);
        const auto response =
            handle_request(tool_call(std::to_string(id++), tool));
        require_contains(response, "invalid_selector");
        require_contains(response, R"JSON("structuredContent")JSON");
        REQUIRE(response.find("fake-inspector") == std::string::npos);
    }
#endif
}

TEST_CASE("MCP capture workflow discovery is independent of CLI delegates",
          "[mcp][tools][inspect][selection]") {
#if defined(_WIN32)
    SUCCEED("The Windows multi-config pulp-cpp path is covered by the resolver test");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\n";
    const auto cli = make_fake_inspector_cli(project.path);
    std::ofstream script(cli, std::ios::trunc);
    script
        << "#!/bin/sh\n"
        << "case \" $* \" in\n"
        << "  *' Session.getCapabilities '*) "
           "printf '{\"sessionId\":\"session-a\","
           "\"instanceId\":\"instance-b\","
           "\"publicationId\":\"publication-c\"}' ;;\n"
        << "  *) printf 'fake-inspector'; "
           "for arg in \"$@\"; do printf ' [%s]' \"$arg\"; done ;;\n"
        << "esac\n";
    script.close();
    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    const auto trace_start = handle_request(tool_call(
        "62", "pulp_trace_start", R"JSON({"categories":["dsp"]})JSON"));
    require_contains(trace_start, "invalid_selector");
    require_contains(trace_start, R"JSON("structuredContent")JSON");
    REQUIRE(trace_start.find("fake-inspector") == std::string::npos);
#endif
}

TEST_CASE("MCP inspector families do not execute a failing CLI delegate",
          "[mcp][tools][inspect][delegate][failure]") {
#if defined(_WIN32)
    SUCCEED("Nonzero native process status is covered by the shared process runner");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\n";
    const auto cli = make_fake_inspector_cli(project.path);
    std::ofstream failing(cli, std::ios::trunc);
    failing << "#!/bin/sh\n"
            << "printf 'authenticated inspector failure' >&2\n"
            << "exit 7\n";
    failing.close();
    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    for (const char* tool : {
             "pulp_inspect_dom",
             "pulp_motion_snapshot",
             "pulp_trace_snapshot",
         }) {
        INFO("tool=" << tool);
        const auto response = handle_request(tool_call("64", tool));
        require_contains(response, "invalid_selector");
        require_contains(response, R"JSON("isError":true)JSON");
        REQUIRE(response.find("authenticated inspector failure") ==
                std::string::npos);
    }
#endif
}

TEST_CASE("MCP inspector discovery failures do not depend on spawning a CLI",
          "[mcp][tools][inspect][delegate][failure][spawn]") {
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt")
        << "cmake_minimum_required(VERSION 3.24)\n";
    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    const auto response =
        handle_request(tool_call("65", "pulp_inspect_dom"));
    require_contains(response, R"JSON("isError":true)JSON");
    require_contains(response, "invalid_selector");
    REQUIRE(response.find("subprocess") == std::string::npos);
}

TEST_CASE("pulp_audio_compare validates its arguments before shelling out", "[mcp][tools][audio]") {
    // Each invocation drives one guard branch in handle_audio_compare so the
    // typed validation fails fast (with an actionable message) instead of
    // spawning the delegated command on bad input.
    ScopedCurrentPath cwd(repo_root());
    int id = 70;
    auto call = [&](const char* args) {
        return handle_request(tool_call(std::to_string(id++), "pulp_audio_compare", args));
    };

    // Option-looking paths (leading '-') are rejected, not forwarded as flags.
    require_contains(call(R"JSON({"reference":"-x.wav","candidate":"b.wav"})JSON"),
                     "must be WAV paths, not options");
    // Profile is now passed THROUGH to the Python `_AXES` registry (the single source of truth for
    // the valid set, which grows as axes are added); the MCP only rejects an option-looking value.
    // reference_role stays a small closed set validated locally.
    require_contains(call(R"JSON({"reference":"a.wav","candidate":"b.wav","profile":"-x"})JSON"),
                     "profile must be an axis name, not an option");
    require_contains(
        call(R"JSON({"reference":"a.wav","candidate":"b.wav","reference_role":"truth"})JSON"),
        "reference_role must be peer or golden");
    // A newer axis (not in the old hardcoded pair) flows straight through to a well-formed result.
    require_contains(
        call(
            R"JSON({"reference":"/nope/a.wav","candidate":"/nope/b.wav","profile":"noise-roughness"})JSON"),
        R"JSON("jsonrpc":"2.0")JSON");
    // Threshold is passed through to the Python registry (the per-axis source of truth: a
    // fraction for tonal-balance, dB for added-hf); the MCP guards only the universal invariant
    // that it is a finite positive magnitude. A negative threshold is rejected here...
    require_contains(call(R"JSON({"reference":"a.wav","candidate":"b.wav","threshold":-0.5})JSON"),
                     "threshold must be a finite positive number");
    // ...but a dB-scale threshold (e.g. 3.0, valid for added-hf) is NO LONGER rejected by a
    // hardcoded (0,1) fraction bound — it flows through to the delegated tool.
    require_contains(
        call(
            R"JSON({"reference":"a.wav","candidate":"b.wav","profile":"added-hf","threshold":3.0})JSON"),
        R"JSON("jsonrpc":"2.0")JSON");
}

TEST_CASE("pulp_audio_compare forwards valid options through the delegated shell-out",
          "[mcp][tools][audio]") {
    // Valid profile + reference_role + a tiny threshold flow past every guard and
    // into the shell-out tail (the threshold is serialized via std::to_chars, so a
    // small value is preserved rather than floored to fixed decimals). The opt-in
    // tool is absent here, so the handler returns its install/upgrade hint — but the
    // response is a well-formed JSON-RPC result, which is all we assert.
    ScopedCurrentPath cwd(repo_root());
    auto response = handle_request(tool_call(
        "68", "pulp_audio_compare",
        R"JSON({"reference":"/nonexistent/ref.wav","candidate":"/nonexistent/cand.wav",)JSON"
        R"JSON("profile":"added-hf","reference_role":"golden","threshold":0.0001})JSON"));
    require_contains(response, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(response, R"JSON("content")JSON");
}

TEST_CASE("pulp_audio_compare dispatch reaches the delegated shell-out", "[mcp][tools][audio]") {
    // Exercise handle_audio_compare's tail past argument validation: from a project
    // root it makes the private temp report dir, builds the `pulp audio compare …
    // --json <temp>` command, shells out, and folds the result. The opt-in Audio
    // Quality Lab tool is not installed here, so the delegated command writes no
    // report and the handler returns its actionable install/upgrade hint — but the
    // response is a well-formed JSON-RPC result wrapping that text, which is all we
    // assert (and it covers the temp-dir → exec → empty-report → hint branch that
    // the required-argument case returns before ever reaching).
    ScopedCurrentPath cwd(repo_root());
    auto response = handle_request(tool_call(
        "62", "pulp_audio_compare",
        R"JSON({"reference":"/nonexistent/ref.wav","candidate":"/nonexistent/cand.wav"})JSON"));
    require_contains(response, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(response, R"JSON("content")JSON");
}

TEST_CASE("MCP project-root dependent tools reject non-project directories", "[mcp][tools]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);

    auto response = handle_request(tool_call("20", "pulp_status"));
    require_contains(response, "Error: not in a Pulp project");

    // pulp_audio_compare resolves the delegated CLI relative to the project root,
    // so it too must refuse to run outside a project.
    auto compare = handle_request(tool_call(
        "21", "pulp_audio_compare", R"JSON({"reference":"a.wav","candidate":"b.wav"})JSON"));
    require_contains(compare, "Error: not in a Pulp project");
}

TEST_CASE("MCP audio probe JSON validates frames before shelling out", "[mcp][tools][audio]") {
    ScopedCurrentPath cwd(repo_root_path());

    auto zero_frames =
        handle_request(tool_call("45", "pulp_audio_probe_json", R"JSON({"frames":0})JSON"));
    require_contains(zero_frames, R"JSON("id":45)JSON");
    require_contains(zero_frames, "Error: frames must be a positive integer");
    REQUIRE(zero_frames.find("Unknown tool") == std::string::npos);

    auto string_frames =
        handle_request(tool_call("46", "pulp_audio_probe_json", R"JSON({"frames":"90"})JSON"));
    require_contains(string_frames, R"JSON("id":46)JSON");
    require_contains(string_frames, "Error: frames must be a positive integer");
    REQUIRE(string_frames.find("Unknown tool") == std::string::npos);

    auto option_target =
        handle_request(tool_call("49", "pulp_audio_probe_json", R"JSON({"target":"--watch"})JSON"));
    require_contains(option_target, R"JSON("id":49)JSON");
    require_contains(option_target,
                     "Error: target must be a standalone target name, not an option");
    REQUIRE(option_target.find("Unknown tool") == std::string::npos);
}

TEST_CASE("MCP audio probe JSON reports missing project roots", "[mcp][tools][audio]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);

    auto response = handle_request(tool_call("54", "pulp_audio_probe_json"));
    require_contains(response, R"JSON("id":54)JSON");
    require_contains(response, "Error: not in a Pulp project");
    REQUIRE(response.find("Unknown tool") == std::string::npos);
}

TEST_CASE("MCP audio scope validates parameters before shelling out",
          "[mcp][tools][audio][scope]") {
    ScopedCurrentPath cwd(repo_root_path());

    auto zero_frames =
        handle_request(tool_call("61", "pulp_audio_scope", R"JSON({"frames":0})JSON"));
    require_contains(zero_frames, R"JSON("id":61)JSON");
    require_contains(zero_frames, "Error: frames must be a positive integer");

    auto zero_window =
        handle_request(tool_call("62", "pulp_audio_scope", R"JSON({"window":0})JSON"));
    require_contains(zero_window, "Error: window must be a positive integer");

    auto bad_channel =
        handle_request(tool_call("63", "pulp_audio_scope", R"JSON({"channel":-1})JSON"));
    require_contains(bad_channel, "Error: channel must be a non-negative integer");

    auto bad_trigger =
        handle_request(tool_call("64", "pulp_audio_scope", R"JSON({"trigger":"smooth"})JSON"));
    require_contains(bad_trigger, "Error: trigger must be one of none, raw, off, rising-zero");

    auto option_target =
        handle_request(tool_call("65", "pulp_audio_scope", R"JSON({"target":"--watch"})JSON"));
    require_contains(option_target,
                     "Error: target must be a standalone target name, not an option");

    auto live_and_offline = handle_request(
        tool_call("67", "pulp_audio_scope", R"JSON({"target":"Demo","input_wav":"in.wav"})JSON"));
    require_contains(live_and_offline, "Error: target and input_wav are mutually exclusive");

    auto png_without_offline =
        handle_request(tool_call("68", "pulp_audio_scope", R"JSON({"png_path":"scope.png"})JSON"));
    require_contains(png_without_offline, "Error: png_path is only supported with input_wav");
    REQUIRE(option_target.find("Unknown tool") == std::string::npos);
}

TEST_CASE("MCP build and test handlers quote project paths and filters", "[mcp][tools][shell]") {
#if defined(_WIN32)
    SKIP("PATH-injected POSIX fake tools are only used on non-Windows");
#else
    TempDir scratch;
    const auto project = scratch.path / "Project With Spaces";
    std::filesystem::create_directories(project / "core");
    std::filesystem::create_directories(project / "build");
    std::ofstream(project / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    TempDir fake_bin;
    make_fake_command(fake_bin.path, "cmake", "fake-cmake");
    make_fake_command(fake_bin.path, "ctest", "fake-ctest");
    const char* old_path = std::getenv("PATH");
    ScopedEnvVar path_env("PATH", fake_bin.path.string() + ":" + (old_path ? old_path : ""));
    ScopedCurrentPath cwd(project);
    const auto canonical_build = normalize_path(project / "build").string();

    auto build_response = handle_request(tool_call("41", "pulp_build"));
    require_contains(build_response, R"JSON("id":41)JSON");
    require_contains(build_response, "fake-cmake [--build] [" + canonical_build + "]");
    REQUIRE(build_response.find("[Project]") == std::string::npos);
    REQUIRE(build_response.find("[With]") == std::string::npos);
    REQUIRE(build_response.find("[Spaces/build]") == std::string::npos);

    auto test_response =
        handle_request(tool_call("42", "pulp_test", R"JSON({"filter":"MCP protocol's edge"})JSON"));
    require_contains(test_response, R"JSON("id":42)JSON");
    require_contains(test_response, "fake-ctest [--test-dir] [" + canonical_build + "]");
    require_contains(test_response, "[--output-on-failure] [-R] [MCP protocol's edge]");
    REQUIRE(test_response.find("[MCP] [protocol") == std::string::npos);
#endif
}

TEST_CASE("MCP docs_search and create quote user arguments", "[mcp][tools][shell]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir scratch;
    const auto project = scratch.path / "Project With Spaces";
    std::filesystem::create_directories(project / "core");
    std::filesystem::create_directories(project / "tools");
    std::ofstream(project / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";
    make_fake_pulp_cli(project);

    const auto create_project = project / "tools" / "create-project.py";
    {
        std::ofstream script(create_project);
        script << "#!/usr/bin/env python3\n"
               << "import sys\n"
               << "print('fake-create' + ''.join(f' [{arg}]' for arg in sys.argv[1:]))\n";
    }

    ScopedCurrentPath cwd(project);

    auto docs_search = handle_request(
        tool_call("43", "pulp_docs_search", R"JSON({"query":"gain staging's guide"})JSON"));
    require_contains(docs_search, R"JSON("id":43)JSON");
    require_contains(docs_search, "fake-pulp [docs] [search] [gain staging's guide]");
    REQUIRE(docs_search.find("[gain] [staging") == std::string::npos);

    auto kit_validate = handle_request(
        tool_call("45", "pulp_kit_validate",
                  R"JSON({"path":"fixtures/packages/basic ui's kit","strict":true})JSON"));
    require_contains(kit_validate, R"JSON("id":45)JSON");
    require_contains(
        kit_validate,
        "fake-pulp [kit] [validate] [fixtures/packages/basic ui's kit] [--json] [--strict]");
    REQUIRE(kit_validate.find("[fixtures/packages/basic] [ui") == std::string::npos);

    auto kit_umbrella = handle_request(tool_call(
        "48", "pulp_kit",
        R"JSON({"subcommand":"validate","path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_umbrella, R"JSON("id":48)JSON");
    require_contains(kit_umbrella,
                     "fake-pulp [kit] [validate] [fixtures/packages/basic ui's kit] [--json]");

    auto kit_preview_alias = handle_request(
        tool_call("52", "pulp_kit",
                  R"JSON({"subcommand":"preview","path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_preview_alias, R"JSON("id":52)JSON");
    require_contains(kit_preview_alias,
                     "Error: use kit plan; preview is reserved for content compatibility checks");
    REQUIRE(kit_preview_alias.find("[kit] [plan]") == std::string::npos);

    auto kit_inspect = handle_request(tool_call(
        "46", "pulp_kit_inspect", R"JSON({"path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_inspect, R"JSON("id":46)JSON");
    require_contains(kit_inspect,
                     "fake-pulp [kit] [inspect] [fixtures/packages/basic ui's kit] [--json]");

    auto kit_plan = handle_request(
        tool_call("49", "pulp_kit_plan", R"JSON({"path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_plan, R"JSON("id":49)JSON");
    require_contains(kit_plan,
                     "fake-pulp [kit] [plan] [fixtures/packages/basic ui's kit] [--project]");
    require_contains(kit_plan, "[--json]");
    REQUIRE(kit_plan.find("[fixtures/packages/basic] [ui") == std::string::npos);

    auto kit_verify = handle_request(tool_call(
        "50", "pulp_kit_verify", R"JSON({"path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_verify, R"JSON("id":50)JSON");
    require_contains(kit_verify,
                     "fake-pulp [kit] [verify] [fixtures/packages/basic ui's kit] [--project]");
    require_contains(kit_verify, "[--json]");
    REQUIRE(kit_verify.find("[fixtures/packages/basic] [ui") == std::string::npos);

    auto kit_verify_screenshots = handle_request(tool_call(
        "51", "pulp_kit_verify",
        R"JSON({"path":"fixtures/packages/basic ui's kit","execute_screenshots":true,"screenshot_backend":"skia","screenshot_output_dir":"artifacts/kit shots"})JSON"));
    require_contains(kit_verify_screenshots, R"JSON("id":51)JSON");
    require_contains(kit_verify_screenshots,
                     "fake-pulp [kit] [verify] [fixtures/packages/basic ui's kit] [--project]");
    require_contains(kit_verify_screenshots,
                     "[--json] [--execute-screenshots] [--screenshot-backend] [skia] "
                     "[--screenshot-output-dir] [artifacts/kit shots]");
    REQUIRE(kit_verify_screenshots.find("[artifacts/kit] [shots]") == std::string::npos);

    auto kit_apply_missing_yes = handle_request(tool_call(
        "54", "pulp_kit_apply", R"JSON({"path":"fixtures/packages/basic ui's kit"})JSON"));
    require_contains(kit_apply_missing_yes, R"JSON("id":54)JSON");
    require_contains(kit_apply_missing_yes, "Error: yes=true is required");

    auto kit_apply = handle_request(
        tool_call("55", "pulp_kit_apply",
                  R"JSON({"path":"fixtures/packages/basic ui's kit","yes":true})JSON"));
    require_contains(kit_apply, R"JSON("id":55)JSON");
    require_contains(kit_apply,
                     "fake-pulp [kit] [apply] [fixtures/packages/basic ui's kit] [--project]");
    require_contains(kit_apply, "[--yes]");
    REQUIRE(kit_apply.find("[fixtures/packages/basic] [ui") == std::string::npos);

    auto kit_remove_missing_yes =
        handle_request(tool_call("56", "pulp_kit_remove", R"JSON({"id":"dev.pulp.fixture"})JSON"));
    require_contains(kit_remove_missing_yes, R"JSON("id":56)JSON");
    require_contains(kit_remove_missing_yes, "Error: yes=true is required");

    auto kit_remove = handle_request(
        tool_call("57", "pulp_kit_remove", R"JSON({"id":"dev.pulp.fixture","yes":true})JSON"));
    require_contains(kit_remove, R"JSON("id":57)JSON");
    require_contains(kit_remove, "fake-pulp [kit] [remove] [dev.pulp.fixture] [--project]");
    require_contains(kit_remove, "[--yes]");

    auto kit_pack = handle_request(tool_call(
        "58", "pulp_kit_pack",
        R"JSON({"path":"fixtures/packages/basic ui's kit","output":"dist/basic ui's kit.pulpkit"})JSON"));
    require_contains(kit_pack, R"JSON("id":58)JSON");
    require_contains(kit_pack, "fake-pulp [kit] [pack] [fixtures/packages/basic ui's kit] [--json] "
                               "[--output] [dist/basic ui's kit.pulpkit]");
    REQUIRE(kit_pack.find("[dist/basic] [ui") == std::string::npos);

    auto kit_publish = handle_request(tool_call(
        "59", "pulp_kit_publish_check",
        R"JSON({"path":"fixtures/packages/basic ui's kit","registry_manifest":"registry/signed manifest's.json"})JSON"));
    require_contains(kit_publish, R"JSON("id":59)JSON");
    require_contains(kit_publish,
                     "fake-pulp [kit] [publish] [fixtures/packages/basic ui's kit] [--dry-run] "
                     "[--json] [--registry-manifest] [registry/signed manifest's.json]");
    REQUIRE(kit_publish.find("[fixtures/packages/basic] [ui") == std::string::npos);

    auto kit_search = handle_request(tool_call(
        "58", "pulp_kit_search",
        R"JSON({"query":"basic ui","root":"fixtures/packages with spaces","kind":"ui-kit","lane":"kit"})JSON"));
    require_contains(kit_search, R"JSON("id":58)JSON");
    require_contains(kit_search, "fake-pulp [kit] [search] [basic ui] [--root] [fixtures/packages "
                                 "with spaces] [--kind] [ui-kit] [--lane] [kit] [--json]");
    REQUIRE(kit_search.find("[basic] [ui]") == std::string::npos);

    auto kit_init = handle_request(tool_call(
        "47", "pulp_kit_init",
        R"JSON({"kind":"ui-kit","id":"dev.pulp.fixture","name":"Fixture UI","dir":"tmp/kit dir","force":true})JSON"));
    require_contains(kit_init, R"JSON("id":47)JSON");
    require_contains(kit_init, "fake-pulp [kit] [init] [--kind] [ui-kit] [--id] [dev.pulp.fixture] "
                               "[--name] [Fixture UI] [--dir] [tmp/kit dir] [--force]");
    REQUIRE(kit_init.find("[Fixture] [UI]") == std::string::npos);

    auto content_validate =
        handle_request(tool_call("59", "pulp_content_validate",
                                 R"JSON({"path":"packs/basic content's pack.pulpcontent"})JSON"));
    require_contains(content_validate, R"JSON("id":59)JSON");
    require_contains(
        content_validate,
        "fake-pulp [content] [validate] [packs/basic content's pack.pulpcontent] [--json]");
    REQUIRE(content_validate.find("[packs/basic] [content") == std::string::npos);

    auto content_umbrella = handle_request(tool_call(
        "60", "pulp_content",
        R"JSON({"subcommand":"validate","path":"packs/basic content's pack.pulpcontent"})JSON"));
    require_contains(content_umbrella, R"JSON("id":60)JSON");
    require_contains(
        content_umbrella,
        "fake-pulp [content] [validate] [packs/basic content's pack.pulpcontent] [--json]");

    auto content_preview = handle_request(tool_call(
        "70", "pulp_content_preview",
        R"JSON({"path":"packs/basic content's pack.pulpcontent","plugin_runtime":"bundles/My Plugin/pulp.plugin-runtime.json","plugin":"dev.pulp fixture"})JSON"));
    require_contains(content_preview, R"JSON("id":70)JSON");
    require_contains(
        content_preview,
        "fake-pulp [content] [preview] [packs/basic content's pack.pulpcontent] [--plugin-runtime] "
        "[bundles/My Plugin/pulp.plugin-runtime.json] [--json] [--plugin] [dev.pulp fixture]");
    REQUIRE(content_preview.find("[My] [Plugin]") == std::string::npos);
    REQUIRE(content_preview.find("[dev.pulp] [fixture]") == std::string::npos);

    auto content_install_missing_yes = handle_request(tool_call(
        "61", "pulp_content_install",
        R"JSON({"path":"packs/basic content's pack.pulpcontent","plugin":"dev.pulp fixture"})JSON"));
    require_contains(content_install_missing_yes, R"JSON("id":61)JSON");
    require_contains(content_install_missing_yes, "Error: yes=true is required");

    auto content_install = handle_request(tool_call(
        "62", "pulp_content_install",
        R"JSON({"path":"packs/basic content's pack.pulpcontent","plugin":"dev.pulp fixture","root":"tmp/content root","yes":true})JSON"));
    require_contains(content_install, R"JSON("id":62)JSON");
    require_contains(content_install,
                     "fake-pulp [content] [install] [packs/basic content's pack.pulpcontent] "
                     "[--plugin] [dev.pulp fixture] [--yes] [--root] [tmp/content root]");
    REQUIRE(content_install.find("[dev.pulp] [fixture]") == std::string::npos);

    auto content_update_missing_yes = handle_request(tool_call(
        "68", "pulp_content_update",
        R"JSON({"path":"packs/basic content's pack.pulpcontent","plugin":"dev.pulp fixture"})JSON"));
    require_contains(content_update_missing_yes, R"JSON("id":68)JSON");
    require_contains(content_update_missing_yes, "Error: yes=true is required");

    auto content_update = handle_request(tool_call(
        "69", "pulp_content_update",
        R"JSON({"path":"packs/basic content's pack.pulpcontent","plugin":"dev.pulp fixture","root":"tmp/content root","yes":true})JSON"));
    require_contains(content_update, R"JSON("id":69)JSON");
    require_contains(content_update,
                     "fake-pulp [content] [update] [packs/basic content's pack.pulpcontent] "
                     "[--plugin] [dev.pulp fixture] [--yes] [--root] [tmp/content root]");
    REQUIRE(content_update.find("[dev.pulp] [fixture]") == std::string::npos);

    auto content_list = handle_request(
        tool_call("63", "pulp_content_list",
                  R"JSON({"plugin":"dev.pulp fixture","root":"tmp/content root"})JSON"));
    require_contains(content_list, R"JSON("id":63)JSON");
    require_contains(content_list, "fake-pulp [content] [list] [--json] [--plugin] [dev.pulp "
                                   "fixture] [--root] [tmp/content root]");

    auto content_rescan = handle_request(
        tool_call("67", "pulp_content_rescan", R"JSON({"root":"tmp/content root"})JSON"));
    require_contains(content_rescan, R"JSON("id":67)JSON");
    require_contains(content_rescan,
                     "fake-pulp [content] [rescan] [--json] [--root] [tmp/content root]");

    auto content_remove_missing_yes = handle_request(
        tool_call("64", "pulp_content_remove",
                  R"JSON({"id":"dev.pulp.content","plugin":"dev.pulp fixture"})JSON"));
    require_contains(content_remove_missing_yes, R"JSON("id":64)JSON");
    require_contains(content_remove_missing_yes, "Error: yes=true is required");

    auto content_remove = handle_request(tool_call(
        "65", "pulp_content_remove",
        R"JSON({"id":"dev.pulp.content","plugin":"dev.pulp fixture","version":"0.1.0","root":"tmp/content root","yes":true})JSON"));
    require_contains(content_remove, R"JSON("id":65)JSON");
    require_contains(content_remove,
                     "fake-pulp [content] [remove] [dev.pulp.content] [--plugin] [dev.pulp "
                     "fixture] [--yes] [--version] [0.1.0] [--root] [tmp/content root]");

    auto content_reveal = handle_request(tool_call(
        "66", "pulp_content_reveal",
        R"JSON({"id":"dev.pulp.content","plugin":"dev.pulp fixture","version":"0.1.0","root":"tmp/content root"})JSON"));
    require_contains(content_reveal, R"JSON("id":66)JSON");
    require_contains(content_reveal,
                     "fake-pulp [content] [reveal] [dev.pulp.content] [--plugin] [dev.pulp "
                     "fixture] [--version] [0.1.0] [--root] [tmp/content root]");

    auto create = handle_request(
        tool_call("44", "pulp_create",
                  R"JSON({"name":"Tape Echo","type":"effect","manufacturer":"ACME Audio"})JSON"));
    require_contains(create, R"JSON("id":44)JSON");
    require_contains(create,
                     "fake-create [Tape Echo] [--type] [effect] [--manufacturer] [ACME Audio]");
    REQUIRE(create.find("[Tape] [Echo]") == std::string::npos);
    REQUIRE(create.find("[ACME] [Audio]") == std::string::npos);
#endif
}

TEST_CASE("MCP docs_check runs the project docs check script", "[mcp][tools][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir scratch;
    const auto project = scratch.path / "Project With Docs Check";
    std::filesystem::create_directories(project / "core");
    std::filesystem::create_directories(project / "tools");
    std::ofstream(project / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    {
        std::ofstream script(project / "tools" / "check-docs.sh");
        script << "#!/bin/sh\n"
               << "printf 'fake-docs-check [%s]\\n' \"$PWD\"\n";
    }

    ScopedCurrentPath cwd(project);
    auto response = handle_request(tool_call("44", "pulp_docs_check"));
    require_contains(response, R"JSON("id":44)JSON");
    require_contains(response, "fake-docs-check");
    REQUIRE(response.find("Error: not in a Pulp project") == std::string::npos);
#endif
}

TEST_CASE("MCP package workflow preserves inspect plan approve apply gates",
          "[mcp][tools][packages][workflow]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir scratch;
    const auto project = scratch.path / "Project";
    const auto bin = scratch.path / "bin";
    const auto log = scratch.path / "package-workflow.log";
    std::filesystem::create_directories(project / "core");
    std::filesystem::create_directories(project / "build" / "tools" / "screenshot");
    std::ofstream(project / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";
    std::ofstream(project / "build" / "CMakeCache.txt") << "CMAKE_BUILD_TYPE:STRING=Release\n";
    make_package_workflow_fake_pulp_cli(project, log);
    make_fake_command(bin, "cmake", "fake-cmake");
    {
        const auto screenshot = project / "build" / "tools" / "screenshot" / "pulp-screenshot";
        std::ofstream script(screenshot);
        script << "#!/bin/sh\n"
               << "printf 'iVBORfakePackageWorkflowPng'\n";
        script.close();
        std::filesystem::permissions(screenshot,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
    }

    const auto previous_path =
        std::getenv("PATH") ? std::string(std::getenv("PATH")) : std::string{};
    ScopedEnvVar path_env("PATH", bin.string() + ":" + previous_path);
    ScopedCurrentPath cwd(project);
    const auto effective_project = std::filesystem::current_path().string();

    auto search = handle_request(
        tool_call("101", "pulp_kit_search",
                  R"JSON({"query":"basic","root":"fixtures/packages","lane":"kit"})JSON"));
    require_contains(search, R"JSON("id":101)JSON");
    require_contains(search, "dev.pulp.fixtures.basic-ui-kit");

    auto inspect = handle_request(tool_call(
        "102", "pulp_kit_inspect", R"JSON({"path":"fixtures/packages/basic-ui-kit"})JSON"));
    require_contains(inspect, R"JSON("id":102)JSON");
    require_contains(inspect, "dev.pulp.fixtures.basic-ui-kit");

    auto plan = handle_request(
        tool_call("103", "pulp_kit_plan", R"JSON({"path":"fixtures/packages/basic-ui-kit"})JSON"));
    require_contains(plan, R"JSON("id":103)JSON");
    require_contains(plan, R"JSON(\"kind\":\"lock-entry\")JSON");

    auto apply_missing_yes = handle_request(
        tool_call("104", "pulp_kit_apply", R"JSON({"path":"fixtures/packages/basic-ui-kit"})JSON"));
    require_contains(apply_missing_yes, "Error: yes=true is required after reviewing the kit plan");

    auto apply = handle_request(
        tool_call("105", "pulp_kit_apply",
                  R"JSON({"path":"fixtures/packages/basic-ui-kit","yes":true})JSON"));
    require_contains(apply, "OK: Applied kit dev.pulp.fixtures.basic-ui-kit");

    auto build = handle_request(tool_call("106", "pulp_build"));
    require_contains(build, "fake-cmake [--build]");

    auto validate = handle_request(tool_call("107", "pulp_validate"));
    require_contains(validate, R"JSON(\"ok\":true)JSON");

    auto screenshot =
        handle_request(tool_call("108", "pulp_screenshot", R"JSON({"demo":true})JSON"));
    require_contains(screenshot, R"JSON("mimeType":"image/png")JSON");
    require_contains(screenshot, "iVBORfakePackageWorkflowPng");

    auto content_validate =
        handle_request(tool_call("109", "pulp_content_validate",
                                 R"JSON({"path":"fixtures/packages/basic-content-pack"})JSON"));
    require_contains(content_validate, "dev.pulp.fixtures.basic-content-pack");

    auto content_preview = handle_request(tool_call(
        "110", "pulp_content_preview",
        R"JSON({"path":"fixtures/packages/basic-content-pack","plugin_runtime":"pulp.plugin-runtime.json","plugin":"dev.pulp.fixtures.content-target"})JSON"));
    require_contains(content_preview, R"JSON(\"requires_restart\":false)JSON");

    auto content_install_missing_yes = handle_request(tool_call(
        "111", "pulp_content_install",
        R"JSON({"path":"fixtures/packages/basic-content-pack","plugin":"dev.pulp.fixtures.content-target"})JSON"));
    require_contains(content_install_missing_yes,
                     "Error: yes=true is required after reviewing the content install target");

    auto content_install = handle_request(tool_call(
        "112", "pulp_content_install",
        R"JSON({"path":"fixtures/packages/basic-content-pack","plugin":"dev.pulp.fixtures.content-target","root":"content-root","yes":true})JSON"));
    require_contains(content_install,
                     "OK: Installed content pack dev.pulp.fixtures.basic-content-pack");

    std::ifstream log_stream(log);
    const std::string log_text((std::istreambuf_iterator<char>(log_stream)),
                               std::istreambuf_iterator<char>());
    require_contains(log_text, "kit search basic --root fixtures/packages --lane kit --json");
    require_contains(log_text, "kit inspect fixtures/packages/basic-ui-kit --json");
    require_contains(log_text, "kit plan fixtures/packages/basic-ui-kit --project " +
                                   effective_project + " --json");
    require_contains(log_text, "kit apply fixtures/packages/basic-ui-kit --project " +
                                   effective_project + " --yes");
    require_contains(log_text, "validate --json");
    require_contains(log_text, "content validate fixtures/packages/basic-content-pack --json");
    require_contains(log_text,
                     "content preview fixtures/packages/basic-content-pack --plugin-runtime "
                     "pulp.plugin-runtime.json --json --plugin dev.pulp.fixtures.content-target");
    require_contains(log_text, "content install fixtures/packages/basic-content-pack --plugin "
                               "dev.pulp.fixtures.content-target --yes --root content-root");
    REQUIRE(log_text.find("kit apply fixtures/packages/basic-ui-kit --project " +
                          effective_project + " --json") == std::string::npos);
#endif
}

TEST_CASE("MCP status reports import-design defaults", "[mcp][tools]") {
    TempDir home;
    {
        std::ofstream cfg(home.path / "config.toml");
        cfg << "[import_design]\n"
            << "default_mode = 'baked'\n";
    }
    ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
    ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
    ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");
    ScopedCurrentPath cwd(repo_root_path());

    auto response = handle_request(tool_call("21", "pulp_status"));
    require_contains(response,
                     "Import design defaults: --mode baked (config:import_design.default_mode), "
                     "--emit ir-json (implied by config:import_design.default_mode)");
}

TEST_CASE("MCP status quotes project roots before reading Git branch", "[mcp][tools][shell]") {
#if defined(_WIN32)
    SKIP("POSIX shell quoting assertion is only used on non-Windows");
#else
    clear_inherited_git_env(); // never let an inherited GIT_DIR hijack `git -C`
    TempDir scratch;
    const auto project = scratch.path / "Project With Spaces And 'Quotes'";
    std::filesystem::create_directories(project / "core");
    std::filesystem::create_directories(project / "test");
    std::filesystem::create_directories(project / "build");
    std::ofstream(project / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    REQUIRE(std::system(("git -C " + shell_quote(project.string()) + " init --quiet").c_str()) ==
            0);
    REQUIRE(std::system(("git -C " + shell_quote(project.string()) +
                         " checkout -b mcp-status-quoted-root --quiet")
                            .c_str()) == 0);

    ScopedCurrentPath cwd(project);
    auto response = handle_request(tool_call("37", "pulp_status"));
    require_contains(response, R"JSON("id":37)JSON");
    require_contains(response, "Project With Spaces And 'Quotes'");
    require_contains(response, "Branch: mcp-status-quoted-root");
#endif
}

// Regression: a throwaway temp repo must stay isolated even when the process
// inherited a GIT_DIR/GIT_WORK_TREE pointing at another repo (as happens when
// the suite runs under a git hook). Before the fix, `git -C <temp>` honored the
// inherited GIT_DIR and mutated the WRONG repo — corrupting live worktrees
// during pre-push / shipyard ctest runs.
TEST_CASE("temp-repo git stays isolated despite an inherited GIT_DIR",
          "[mcp][git-isolation][regression]") {
#if defined(_WIN32)
    SKIP("POSIX git-environment isolation assertion is only used on non-Windows");
#else
    // A "sentinel" repo standing in for the source worktree. If isolation
    // breaks, the temp-repo git commands below would land here.
    TempDir sentinel;
    REQUIRE(std::system(
                ("git -C " + shell_quote(sentinel.path.string()) + " init --quiet").c_str()) == 0);
    REQUIRE(std::system(("git -C " + shell_quote(sentinel.path.string()) +
                         " symbolic-ref HEAD refs/heads/sentinel-main")
                            .c_str()) == 0);

    // Simulate the hook environment: export GIT_DIR/GIT_WORK_TREE at the
    // sentinel, exactly what a parent `git` process would inject.
    ScopedEnvVar git_dir("GIT_DIR", (sentinel.path / ".git").string());
    ScopedEnvVar git_work_tree("GIT_WORK_TREE", sentinel.path.string());

    // The code under test: scrub the inherited git environment.
    clear_inherited_git_env();
    REQUIRE(std::getenv("GIT_DIR") == nullptr);
    REQUIRE(std::getenv("GIT_WORK_TREE") == nullptr);

    // Now a temp-repo git op must target the temp repo, not the sentinel.
    TempDir project;
    REQUIRE(std::system(
                ("git -C " + shell_quote(project.path.string()) + " init --quiet").c_str()) == 0);
    REQUIRE(std::system(("git -C " + shell_quote(project.path.string()) +
                         " checkout -b isolated-branch --quiet")
                            .c_str()) == 0);

    // The project's own repo was created and switched (proves the temp-repo
    // git commands actually ran against the temp repo).
    REQUIRE(std::filesystem::exists(project.path / ".git" / "HEAD"));
    // The sentinel repo is untouched: it still has no isolated-branch.
    const int sentinel_has_branch =
        std::system(("git -C " + shell_quote(sentinel.path.string()) +
                     " rev-parse --verify --quiet refs/heads/isolated-branch >/dev/null")
                        .c_str());
    REQUIRE(sentinel_has_branch != 0); // sentinel must NOT have the branch
#endif
}

TEST_CASE("MCP status resolves import-design defaults from config and env",
          "[mcp][tools][import-design]") {
    ScopedCurrentPath cwd(repo_root_path());

    SECTION("built-ins stay live and js when no config or env is present") {
        TempDir home;
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("22", "pulp_status"));
        require_contains(response, "Import design defaults: --mode live (built-in)");
        require_contains(response, "--emit js (built-in)");
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
        REQUIRE(response.find("implied by") == std::string::npos);
    }

    SECTION("config emit ir-json implies baked mode") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "# leading comments are ignored\n"
                << "[other]\n"
                << "default_emit = 'js'\n"
                << "[import_design]\n"
                << "default_emit = 'ir-json' # inline comments are ignored\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("23", "pulp_status"));
        require_contains(
            response,
            "Import design defaults: --mode baked (implied by config:import_design.default_emit)");
        require_contains(response, "--emit ir-json (config:import_design.default_emit)");
        REQUIRE(response.find("[other]") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
    }

    SECTION("config baked mode implies ir-json emit") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = \"baked\"\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("24", "pulp_status"));
        require_contains(response, "--mode baked (config:import_design.default_mode)");
        require_contains(response, "--emit ir-json (implied by config:import_design.default_mode)");
        REQUIRE(response.find("built-in") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
    }

    SECTION("config emit cpp also implies baked mode") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_emit = \"cpp\"\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("25", "pulp_status"));
        require_contains(
            response,
            "Import design defaults: --mode baked (implied by config:import_design.default_emit)");
        require_contains(response, "--emit cpp (config:import_design.default_emit)");
        REQUIRE(response.find("--mode live") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
    }

    SECTION("empty env vars are ignored so config still applies") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'baked'\n"
                << "default_emit = 'ir-json'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("26", "pulp_status"));
        require_contains(response, "--mode baked (config:import_design.default_mode)");
        require_contains(response, "--emit ir-json (config:import_design.default_emit)");
        REQUIRE(response.find("env:PULP_IMPORT_DESIGN_DEFAULT_MODE") == std::string::npos);
        REQUIRE(response.find("env:PULP_IMPORT_DESIGN_DEFAULT_EMIT") == std::string::npos);
    }

    SECTION("environment values override config and normalize case") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'live'\n"
                << "default_emit = 'js'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "BAKED");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "CPP");

        auto response = handle_request(tool_call("27", "pulp_status"));
        require_contains(
            response, "Import design defaults: --mode baked (env:PULP_IMPORT_DESIGN_DEFAULT_MODE)");
        require_contains(response, "--emit cpp (env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)");
        REQUIRE(response.find("config:import_design.default_mode") == std::string::npos);
        REQUIRE(response.find("config:import_design.default_emit") == std::string::npos);
    }

    SECTION("environment can explicitly pair live mode with ir-json emit") {
        TempDir home;
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "LIVE");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "IR-JSON");

        auto response = handle_request(tool_call("28", "pulp_status"));
        require_contains(
            response, "Import design defaults: --mode live (env:PULP_IMPORT_DESIGN_DEFAULT_MODE)");
        require_contains(response, "--emit ir-json (env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)");
        REQUIRE(response.find("implied by") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
    }

    SECTION("environment emit overrides config while config mode still applies") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'baked'\n"
                << "default_emit = 'ir-json'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "js");

        auto response = handle_request(tool_call("34", "pulp_status"));
        require_contains(response, "--mode baked (config:import_design.default_mode)");
        require_contains(response, "--emit js (env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)");
        REQUIRE(response.find("config:import_design.default_emit") == std::string::npos);
        REQUIRE(response.find("implied by") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
        REQUIRE(response.find("ir-json") == std::string::npos);
    }

    SECTION("environment mode overrides config while config emit still applies") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'baked'\n"
                << "default_emit = 'cpp'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "live");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("35", "pulp_status"));
        require_contains(response, "--mode live (env:PULP_IMPORT_DESIGN_DEFAULT_MODE)");
        require_contains(response, "--emit cpp (config:import_design.default_emit)");
        REQUIRE(response.find("config:import_design.default_mode") == std::string::npos);
        REQUIRE(response.find("implied by") == std::string::npos);
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
        REQUIRE(response.find("--mode baked") == std::string::npos);
    }

    SECTION("config parser trims section names keys values and inline comments") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[ import_design ]\n"
                << " default_mode = ' BAKED ' # whitespace is trimmed before validation\n"
                << " default_emit = \" CPP \" # quoted values normalize\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("36", "pulp_status"));
        require_contains(response, "--mode baked (config:import_design.default_mode)");
        require_contains(response, "--emit cpp (config:import_design.default_emit)");
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
        REQUIRE(response.find("#") == std::string::npos);
        REQUIRE(response.find(" BAKED ") == std::string::npos);
        REQUIRE(response.find(" CPP ") == std::string::npos);
    }

    SECTION("environment trims quoted defaults before validation") {
        TempDir home;
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "  'BAKED'  ");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "  \"CPP\"  ");

        auto response = handle_request(tool_call("29", "pulp_status"));
        require_contains(
            response, "Import design defaults: --mode baked (env:PULP_IMPORT_DESIGN_DEFAULT_MODE)");
        require_contains(response, "--emit cpp (env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)");
        REQUIRE(response.find("Import design defaults: invalid") == std::string::npos);
    }

    SECTION("invalid environment mode reports env source after valid emit") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'baked'\n"
                << "default_emit = 'ir-json'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "native");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "cpp");

        auto response = handle_request(tool_call("30", "pulp_status"));
        require_contains(response,
                         "Import design defaults: invalid (import_design.default_mode must be one "
                         "of: live, baked from env:PULP_IMPORT_DESIGN_DEFAULT_MODE)");
        REQUIRE(response.find("--emit cpp") == std::string::npos);
        REQUIRE(response.find("config:import_design.default_mode") == std::string::npos);
    }

    SECTION("invalid emit reports its source before considering mode") {
        TempDir home;
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "baked");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "python");

        auto response = handle_request(tool_call("31", "pulp_status"));
        require_contains(response,
                         "Import design defaults: invalid (import_design.default_emit must be one "
                         "of: js, ir-json, cpp from env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)");
        REQUIRE(response.find("--mode baked") == std::string::npos);
        REQUIRE(response.find("--emit") == std::string::npos);
        REQUIRE(response.find("config:") == std::string::npos);
    }

    SECTION("invalid config emit reports config source") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_emit = 'python'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("32", "pulp_status"));
        require_contains(response,
                         "Import design defaults: invalid (import_design.default_emit must be one "
                         "of: js, ir-json, cpp from config:import_design.default_emit)");
        REQUIRE(response.find("--emit python") == std::string::npos);
        REQUIRE(response.find("env:PULP_IMPORT_DESIGN_DEFAULT_EMIT") == std::string::npos);
        REQUIRE(response.find("implied by") == std::string::npos);
    }

    SECTION("invalid mode reports config source when env is absent") {
        TempDir home;
        {
            std::ofstream cfg(home.path / "config.toml");
            cfg << "[import_design]\n"
                << "default_mode = 'native'\n"
                << "default_emit = 'js'\n";
        }
        ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        auto response = handle_request(tool_call("33", "pulp_status"));
        require_contains(response,
                         "Import design defaults: invalid (import_design.default_mode must be one "
                         "of: live, baked from config:import_design.default_mode)");
        require_contains(response, "Pulp Project:");
        REQUIRE(response.find("--mode native") == std::string::npos);
        REQUIRE(response.find("env:PULP_IMPORT_DESIGN_DEFAULT_MODE") == std::string::npos);
    }
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
TEST_CASE("MCP wrapper tools reach live discovery outside a source checkout",
          "[mcp][tools][issue-1997]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", temp.path.string());

    // Live inspector tools use owner-private discovery directly; no project
    // root or sibling CLI is involved. No live inspector is required here.
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
        // Live inspector tools are independent of a source checkout. An empty
        // owner-private runtime directory therefore reaches shared-client
        // discovery and fails with a structured selection error.
        require_contains(response, "invalid_selector");
        require_contains(response, R"JSON("structuredContent")JSON");
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
        "pulp_content_validate",
        "pulp_content_install",
        "pulp_content_list",
        "pulp_content_preview",
        "pulp_content_rescan",
        "pulp_content_remove",
        "pulp_content_reveal",
        "pulp_content_update",
        "pulp_kit",
        "pulp_kit_search",
        "pulp_kit_validate",
        "pulp_kit_inspect",
        "pulp_kit_plan",
        "pulp_kit_apply",
        "pulp_kit_verify",
        "pulp_kit_remove",
        "pulp_kit_pack",
        "pulp_kit_publish_check",
        "pulp_kit_init",
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

TEST_CASE("MCP inspect screenshot and evaluate wrappers preserve unavailable text",
          "[mcp][tools][inspect]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";
    make_fake_inspector_cli(project.path);

    ScopedCurrentPath cwd(project.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", project.path.string());

    auto evaluate = handle_request(tool_call("50", "pulp_inspect_evaluate",
                                             R"JSON({"expression":"window.title + ' ok'"})JSON"));
    require_contains(evaluate, R"JSON("id":50)JSON");
    require_contains(evaluate, "invalid_selector");
    require_contains(evaluate, R"JSON("structuredContent")JSON");
    REQUIRE(evaluate.find("fake-inspector") == std::string::npos);

    auto screenshot = handle_request(tool_call("51", "pulp_inspect_screenshot"));
    require_contains(screenshot, R"JSON("id":51)JSON");
    require_contains(screenshot, R"JSON("type":"text")JSON");
    require_contains(screenshot, "invalid_selector");
    REQUIRE(screenshot.find(R"JSON("type":"image")JSON") == std::string::npos);
    REQUIRE(screenshot.find(R"JSON("mimeType":"image/png")JSON") == std::string::npos);
#endif
}

TEST_CASE("MCP validate only passes --all for the explicit all flag", "[mcp][tools][validate]") {
#if defined(_WIN32)
    SKIP("fake extensionless pulp CLI is only executable through popen on POSIX");
#else
    TempDir project;
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";
    std::filesystem::create_directories(project.path / "core");
    make_fake_pulp_cli(project.path);

    ScopedCurrentPath cwd(project.path);

    auto default_response = handle_request(tool_call("50", "pulp_validate"));
    require_contains(default_response, R"JSON("id":50)JSON");
    require_contains(default_response, "fake-pulp [validate] [--json]");
    REQUIRE(default_response.find("[--all]") == std::string::npos);

    auto all_response = handle_request(tool_call("51", "pulp_validate", R"JSON({"all":true})JSON"));
    require_contains(all_response, R"JSON("id":51)JSON");
    require_contains(all_response, "fake-pulp [validate] [--json] [--all]");

    auto false_with_other_true =
        handle_request(tool_call("52", "pulp_validate", R"JSON({"all":false,"json":true})JSON"));
    require_contains(false_with_other_true, R"JSON("id":52)JSON");
    require_contains(false_with_other_true, "fake-pulp [validate] [--json]");
    REQUIRE(false_with_other_true.find("[--all]") == std::string::npos);
    REQUIRE(false_with_other_true.find("[--screenshot]") == std::string::npos);

    auto string_true =
        handle_request(tool_call("53", "pulp_validate", R"JSON({"all":"true"})JSON"));
    require_contains(string_true, R"JSON("id":53)JSON");
    require_contains(string_true, "fake-pulp [validate] [--json]");
    REQUIRE(string_true.find("[--all]") == std::string::npos);

    auto screenshot_response =
        handle_request(tool_call("54", "pulp_validate", R"JSON({"screenshot":true})JSON"));
    require_contains(screenshot_response, R"JSON("id":54)JSON");
    require_contains(screenshot_response, "fake-pulp [validate] [--json] [--screenshot]");
    REQUIRE(screenshot_response.find("[--all]") == std::string::npos);

    auto screenshot_false =
        handle_request(tool_call("55", "pulp_validate", R"JSON({"screenshot":false})JSON"));
    require_contains(screenshot_false, R"JSON("id":55)JSON");
    require_contains(screenshot_false, "fake-pulp [validate] [--json]");
    REQUIRE(screenshot_false.find("[--screenshot]") == std::string::npos);
#endif
}

TEST_CASE("MCP audio probe JSON wraps pulp run and returns structured content",
          "[mcp][tools][audio]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::filesystem::create_directories(project.path / "build");
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    const auto cli = project.path / "build" / "tools" / "cli" / "Release" / "pulp-cpp.exe";
    std::filesystem::create_directories(cli.parent_path());
    {
        std::ofstream script(cli);
        script << "#!/bin/sh\n"
               << "out=''\n"
               << "frames=''\n"
               << "target_seen=false\n"
               << "while [ \"$#\" -gt 0 ]; do\n"
               << "  case \"$1\" in\n"
               << "    \"Special Target\") target_seen=true ;;\n"
               << "    --audio-probe-json) shift; out=\"$1\" ;;\n"
               << "    --frames) shift; frames=\"$1\" ;;\n"
               << "  esac\n"
               << "  shift\n"
               << "done\n"
               << "printf "
                  "'{\"stage\":\"standalone_output_boundary\",\"frames\":%s,\"target_seen\":%s,"
                  "\"callbacks\":7}' \"$frames\" \"$target_seen\" > \"$out\"\n";
    }
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    ScopedCurrentPath cwd(project.path);
    auto response = handle_request(tool_call("47", "pulp_audio_probe_json",
                                             R"JSON({"target":"Special Target","frames":42})JSON"));
    require_contains(response, R"JSON("id":47)JSON");
    require_contains(response, R"JSON("structuredContent")JSON");
    require_contains(response, R"JSON("stage": "standalone_output_boundary")JSON");
    require_contains(response, R"JSON("frames": 42)JSON");
    require_contains(response, R"JSON("target_seen": true)JSON");
    require_contains(response, R"JSON("callbacks": 7)JSON");
    REQUIRE(response.find("Error:") == std::string::npos);
#endif
}

TEST_CASE("MCP audio scope wraps pulp audio scope and returns structured content",
          "[mcp][tools][audio][scope]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::filesystem::create_directories(project.path / "build");
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    const auto cli = project.path / "build" / "tools" / "cli" / "Release" / "pulp-cpp.exe";
    std::filesystem::create_directories(cli.parent_path());
    {
        std::ofstream script(cli);
        script << "#!/bin/sh\n"
               << "out=''\n"
               << "frames=''\n"
               << "window=''\n"
               << "trigger=''\n"
               << "channel=''\n"
               << "input_wav=''\n"
               << "png_path=''\n"
               << "target_seen=false\n"
               << "while [ \"$#\" -gt 0 ]; do\n"
               << "  case \"$1\" in\n"
               << "    audio|scope) ;;\n"
               << "    \"Special Target\") target_seen=true ;;\n"
               << "    --input-wav) shift; input_wav=\"$1\" ;;\n"
               << "    --png) shift; png_path=\"$1\"; printf 'PNG' > \"$png_path\" ;;\n"
               << "    --json) shift; out=\"$1\" ;;\n"
               << "    --frames) shift; frames=\"$1\" ;;\n"
               << "    --window) shift; window=\"$1\" ;;\n"
               << "    --trigger) shift; trigger=\"$1\" ;;\n"
               << "    --channel) shift; channel=\"$1\" ;;\n"
               << "  esac\n"
               << "  shift\n"
               << "done\n"
               << "printf "
                  "'{\"schema\":\"pulp.audio.scope.v1\",\"frames\":%s,\"window\":%s,\"trigger\":\"%"
                  "s\",\"channel\":%s,\"target_seen\":%s,\"input_wav\":\"%s\",\"png_path\":\"%s\"}'"
                  " \"$frames\" \"$window\" \"$trigger\" \"$channel\" \"$target_seen\" "
                  "\"$input_wav\" \"$png_path\" > \"$out\"\n";
    }
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    ScopedCurrentPath cwd(project.path);
    auto response = handle_request(tool_call(
        "66", "pulp_audio_scope",
        R"JSON({"target":"Special Target","frames":42,"window":512,"trigger":"raw","channel":0})JSON"));
    require_contains(response, R"JSON("id":66)JSON");
    require_contains(response, R"JSON("structuredContent")JSON");
    require_contains(response, R"JSON("schema": "pulp.audio.scope.v1")JSON");
    require_contains(response, R"JSON("frames": 42)JSON");
    require_contains(response, R"JSON("window": 512)JSON");
    require_contains(response, R"JSON("trigger": "raw")JSON");
    require_contains(response, R"JSON("channel": 0)JSON");
    require_contains(response, R"JSON("target_seen": true)JSON");
    REQUIRE(response.find("Error:") == std::string::npos);

    const auto png = project.path / "scope.png";
    auto offline = handle_request(tool_call(
        "69", "pulp_audio_scope",
        std::string(
            R"JSON({"input_wav":"tone.wav","window":256,"trigger":"rising-zero","channel":1,"png_path":")JSON") +
            png.string() + R"JSON("})JSON"));
    require_contains(offline, R"JSON("id":69)JSON");
    require_contains(offline, R"JSON("input_wav": "tone.wav")JSON");
    require_contains(offline, std::string(R"JSON("png_path": ")JSON") + png.string());
    REQUIRE(std::filesystem::exists(png));
#endif
}

TEST_CASE("MCP audio probe JSON rejects malformed child output", "[mcp][tools][audio]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::filesystem::create_directories(project.path / "build");
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    const auto cli = project.path / "build" / "tools" / "cli" / "pulp-cpp";
    std::filesystem::create_directories(cli.parent_path());
    {
        std::ofstream script(cli);
        script << "#!/bin/sh\n"
               << "out=''\n"
               << "while [ \"$#\" -gt 0 ]; do\n"
               << "  if [ \"$1\" = \"--audio-probe-json\" ]; then shift; out=\"$1\"; fi\n"
               << "  shift\n"
               << "done\n"
               << "printf 'not-json' > \"$out\"\n";
    }
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    ScopedCurrentPath cwd(project.path);
    auto response = handle_request(tool_call("48", "pulp_audio_probe_json"));
    require_contains(response, R"JSON("id":48)JSON");
    require_contains(response, "Error: failed to parse audio probe JSON");
    REQUIRE(response.find(R"JSON("structuredContent")JSON") == std::string::npos);
    REQUIRE(response.find(R"JSON("code":-32601)JSON") == std::string::npos);
#endif
}

TEST_CASE("MCP audio probe JSON reports child runs that write no JSON", "[mcp][tools][audio]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir project;
    std::filesystem::create_directories(project.path / "core");
    std::filesystem::create_directories(project.path / "build");
    std::ofstream(project.path / "CMakeLists.txt") << "project(FakePulp VERSION 1.2.3)\n";

    const auto cli = project.path / "build" / "tools" / "cli" / "pulp-cpp";
    std::filesystem::create_directories(cli.parent_path());
    {
        std::ofstream script(cli);
        script << "#!/bin/sh\n"
               << "printf 'probe script ran without output file\\n'\n";
    }
    std::filesystem::permissions(cli,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    ScopedCurrentPath cwd(project.path);
    auto response = handle_request(tool_call("55", "pulp_audio_probe_json"));
    require_contains(response, R"JSON("id":55)JSON");
    require_contains(response, "Error: pulp run did not write audio probe JSON");
    require_contains(response, "probe script ran without output file");
    REQUIRE(response.find(R"JSON("structuredContent")JSON") == std::string::npos);
#endif
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

TEST_CASE("MCP audio tools return structured diagnostics without a project root",
          "[mcp][tools][audio]") {
    TempDir home;
    ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
    TempDir cwd_dir;
    ScopedCurrentPath cwd(cwd_dir.path);

    auto status = handle_request(tool_call("61", "pulp_audio_model_status"));
    require_contains(status, R"JSON("id":61)JSON");
    require_contains(status, R"JSON("structuredContent")JSON");
    require_contains(status, R"JSON("state_file_found": false)JSON");
    require_contains(status, R"JSON("loadable": false)JSON");
    require_contains(status, "no configured audio model");
    REQUIRE(status.find(R"JSON("code":-32601)JSON") == std::string::npos);

    auto list = handle_request(tool_call("62", "pulp_audio_model_list"));
    require_contains(list, R"JSON("id":62)JSON");
    require_contains(list, R"JSON("structuredContent")JSON");
    require_contains(list, R"JSON("active_model_id": "")JSON");
    require_contains(list, R"JSON("status": "not_installed")JSON");
    require_contains(list, "clap_music_audioset_v1");
    REQUIRE(list.find(R"JSON("code":-32601)JSON") == std::string::npos);

    auto activate = handle_request(tool_call("63", "pulp_audio_model_activate",
                                             R"JSON({"model_id":"definitely_missing_model"})JSON"));
    require_contains(activate, R"JSON("id":63)JSON");
    require_contains(activate, R"JSON("structuredContent")JSON");
    require_contains(activate, R"JSON("ok": false)JSON");
    require_contains(activate, "unknown model_id: definitely_missing_model");
}

TEST_CASE("MCP audio excerpt-find validates request fields through the handler",
          "[mcp][tools][audio]") {
    TempDir home;
    ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);
    auto input = temp.path / "input.wav";
    {
        std::ofstream file(input);
        file << "not needed for pre-audio validation";
    }

    auto only_text =
        handle_request(tool_call("64", "pulp_audio_excerpt_find", R"JSON({"text":"texture"})JSON"));
    require_contains(only_text, R"JSON("id":64)JSON");
    require_contains(only_text, "Error: text and input_path are required");
    REQUIRE(only_text.find(R"JSON("structuredContent")JSON") == std::string::npos);

    auto only_input = handle_request(
        tool_call("65", "pulp_audio_excerpt_find",
                  std::string(R"JSON({"input_path":")JSON") + input.string() + R"JSON("})JSON"));
    require_contains(only_input, R"JSON("id":65)JSON");
    require_contains(only_input, "Error: text and input_path are required");

    auto bad_top =
        handle_request(tool_call("66", "pulp_audio_excerpt_find",
                                 std::string(R"JSON({"text":"texture","input_path":")JSON") +
                                     input.string() + R"JSON(","top":0})JSON"));
    require_contains(bad_top, R"JSON("id":66)JSON");
    require_contains(bad_top, R"JSON("structuredContent")JSON");
    require_contains(bad_top, R"JSON("ok": false)JSON");
    require_contains(bad_top, "top and max_candidates_per_file must be >= 1");

    auto bad_window =
        handle_request(tool_call("67", "pulp_audio_excerpt_find",
                                 std::string(R"JSON({"text":"texture","input_path":")JSON") +
                                     input.string() + R"JSON(","window_ms":0})JSON"));
    require_contains(bad_window, R"JSON("id":67)JSON");
    require_contains(bad_window, "window_ms and hop_ms must be >= 1");

    auto unknown_model =
        handle_request(tool_call("68", "pulp_audio_excerpt_find",
                                 std::string(R"JSON({"text":"texture","input_path":")JSON") +
                                     input.string() + R"JSON(","model_id":"missing_model"})JSON"));
    require_contains(unknown_model, R"JSON("id":68)JSON");
    require_contains(unknown_model, R"JSON("query": "texture")JSON");
    require_contains(unknown_model, "unknown model_id: missing_model");
    REQUIRE(unknown_model.find(R"JSON("code":-32601)JSON") == std::string::npos);
}

TEST_CASE("MCP audio read-bundle reports missing bundles as structured content",
          "[mcp][tools][audio]") {
    TempDir home;
    ScopedEnvVar pulp_home("PULP_HOME", home.path.string());
    TempDir temp;

    auto response =
        handle_request(tool_call("69", "pulp_audio_read_bundle",
                                 std::string(R"JSON({"bundle_path":")JSON") +
                                     (temp.path / "missing-bundle").string() + R"JSON("})JSON"));

    require_contains(response, R"JSON("id":69)JSON");
    require_contains(response, R"JSON("structuredContent")JSON");
    require_contains(response, R"JSON("ok": false)JSON");
    require_contains(response, "bundle path does not exist");
    REQUIRE(response.find(R"JSON("code":-32601)JSON") == std::string::npos);
}

TEST_CASE("MCP live registry is the single method capability and schema inventory",
          "[mcp][tools][inspect][registry]") {
    using namespace pulp::inspect;
    const std::pair<std::string_view, std::string_view> expected[] = {
        {"pulp_inspect_list", ""},
        {"pulp_inspect_capabilities", methods::kSessionGetCapabilities},
        {"pulp_inspect_context", methods::kInspectorGetAgentContext},
        {"pulp_inspect_dom", methods::kDOMGetDocument},
        {"pulp_inspect_params", methods::kStateGetParameters},
        {"pulp_inspect_value_channels", methods::kStateGetValueChannels},
        {"pulp_inspect_set_param", methods::kStateSetParameter},
        {"pulp_inspect_inject_midi", methods::kTestInjectMidi},
        {"pulp_inspect_set_transport", methods::kTestSetTransport},
        {"pulp_inspect_screenshot", methods::kCaptureScreenshot},
        {"pulp_inspect_evaluate", methods::kRuntimeEvaluate},
        {"pulp_inspect_performance", methods::kPerfGetMetrics},
        {"pulp_inspect_audio", methods::kAudioGetConfig},
        {"pulp_motion_start_trace", methods::kMotionStartTrace},
        {"pulp_motion_stop_trace", methods::kMotionStopTrace},
        {"pulp_motion_snapshot", methods::kMotionSnapshot},
        {"pulp_motion_list_traces", methods::kMotionListTraces},
        {"pulp_motion_scrub_to", methods::kMotionScrubTo},
        {"pulp_motion_play", methods::kMotionPlay},
        {"pulp_motion_pause", methods::kMotionPause},
        {"pulp_motion_enable_cost", methods::kMotionEnableCost},
        {"pulp_motion_disable_cost", methods::kMotionDisableCost},
        {"pulp_trace_start", methods::kTraceStartSession},
        {"pulp_trace_stop", methods::kTraceStopSession},
        {"pulp_trace_snapshot", methods::kTraceSnapshot},
        {"pulp_trace_query", methods::kTraceQuery},
        {"pulp_trace_explain", methods::kTraceExplain},
    };
    const auto registry = inspector_mcp_tool_registry();
    REQUIRE(registry.size() == std::size(expected));
    const auto tools = tools_list_json();
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto& descriptor = registry[index];
        CAPTURE(index, descriptor.name, descriptor.method);
        REQUIRE(descriptor.name == expected[index].first);
        REQUIRE(descriptor.method == expected[index].second);
        REQUIRE(find_inspector_mcp_tool(descriptor.name) == &descriptor);
        require_contains(tools, "\"name\":\"" + std::string(descriptor.name) + "\"");
        if (descriptor.method.empty())
            continue;
        const auto* method = find_inspector_method(descriptor.method);
        REQUIRE(method != nullptr);
        REQUIRE(method->kind == InspectorMethodKind::Request);
        const auto capability = capability_id(method->capability);
        REQUIRE(inspector_mcp_tool_capability(descriptor) == capability);
        require_contains(tools, "\"description\":\"Inspector method " +
                                    std::string(descriptor.method) +
                                    " (capability " + std::string(capability) + "). ");
    }
    REQUIRE(find_inspector_mcp_tool("pulp_inspect_missing") == nullptr);
    auto unregistered = tools +
        R"JSON({"name":"pulp_inspect_unregistered","description":""})JSON";
    REQUIRE_FALSE(decorate_inspector_mcp_tool_descriptions(unregistered));
}

TEST_CASE("Every live inspector MCP schema accepts an exact publication selector",
          "[mcp][tools][inspect][selection]") {
    const auto tools = handle_request(
        R"JSON({"jsonrpc":"2.0","id":2510,"method":"tools/list"})JSON");
    const char* live_tools[] = {
        "pulp_inspect_capabilities", "pulp_inspect_context",
        "pulp_inspect_dom",          "pulp_inspect_params",
        "pulp_inspect_value_channels", "pulp_inspect_set_param",
        "pulp_inspect_inject_midi", "pulp_inspect_set_transport",
        "pulp_inspect_screenshot",   "pulp_inspect_evaluate",
        "pulp_inspect_performance",  "pulp_inspect_audio",
        "pulp_motion_start_trace",   "pulp_motion_stop_trace",
        "pulp_motion_snapshot",      "pulp_motion_list_traces",
        "pulp_motion_scrub_to",      "pulp_motion_play",
        "pulp_motion_pause",         "pulp_motion_enable_cost",
        "pulp_motion_disable_cost",  "pulp_trace_start",
        "pulp_trace_stop",           "pulp_trace_snapshot",
        "pulp_trace_query",          "pulp_trace_explain",
    };
    for (const char* tool : live_tools) {
        INFO("live inspector tool=" << tool);
        const auto position =
            tools.find(std::string(R"JSON("name":")JSON") + tool + "\"");
        REQUIRE(position != std::string::npos);
        const auto next = tools.find(R"JSON({"name":")JSON", position + 8);
        const auto schema = tools.substr(position, next - position);
        const auto required_position = schema.find(R"JSON("required":[)JSON");
        REQUIRE(required_position != std::string::npos);
        const auto properties_position =
            schema.find(R"JSON("properties":)JSON", required_position);
        REQUIRE(properties_position != std::string::npos);
        const auto required = schema.substr(
            required_position, properties_position - required_position);
        require_contains(required, R"JSON("session_id")JSON");
        require_contains(required, R"JSON("instance_id")JSON");
        require_contains(required, R"JSON("publication_id")JSON");
        if (std::string_view(tool) == "pulp_inspect_evaluate")
            require_contains(required, R"JSON("expression")JSON");
        if (std::string_view(tool) == "pulp_inspect_inject_midi") {
            require_contains(required, R"JSON("kind")JSON");
            require_contains(required, R"JSON("channel")JSON");
            require_contains(required, R"JSON("note")JSON");
            require_contains(schema, R"JSON("additionalProperties":false)JSON");
        }
        if (std::string_view(tool) == "pulp_inspect_set_transport")
            require_contains(schema, R"JSON("additionalProperties":false)JSON");
        require_contains(schema, R"JSON("session_id":{"type":"string")JSON");
        require_contains(schema, R"JSON("instance_id":{"type":"string")JSON");
        require_contains(schema, R"JSON("publication_id":{"type":"string")JSON");
    }
}

TEST_CASE("MCP direct client completes authenticated read mutation and denial round trips",
          "[mcp][tools][inspect][authenticated]") {
    AuthenticatedFixture fixture(true);
    ScopedEnvVar runtime_dir("PULP_INSPECTOR_RUNTIME_DIR",
                             fixture.temporary.path.string());
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const auto& record = records.front();
    const auto selector =
        std::string(R"JSON({"session_id":")JSON") + record.session_id +
        R"JSON(","instance_id":")JSON" + record.instance_id +
        R"JSON(","publication_id":")JSON" + record.publication_id + "\"}";
    std::vector<InspectorMessage> observed_requests;
    fixture.observe_request = [&](const InspectorMessage& request) {
        observed_requests.push_back(request);
    };

    const auto discovered =
        handle_request(tool_call("2520", "pulp_inspect_list"));
    require_contains(discovered, record.session_id);
    require_contains(discovered, record.instance_id);
    require_contains(discovered, record.publication_id);
    require_contains(discovered, R"JSON("ok":true)JSON");

    const auto capabilities = handle_request(
        tool_call("2521", "pulp_inspect_capabilities", selector));
    require_contains(capabilities, R"JSON("ok":true)JSON");
    require_contains(capabilities, "state.write");
    require_contains(capabilities, record.publication_id);

    const auto context = handle_request(
        tool_call("2522", "pulp_inspect_context", selector));
    require_contains(context, R"JSON("buildId": "test-build")JSON");
    require_contains(context, record.publication_id);

    auto arguments_before_name = selector;
    arguments_before_name.pop_back();
    arguments_before_name +=
        R"JSON(,"name":"pulp_inspect_set_param","nested":{"session_id":"other-session","value":0.99}})JSON";
    const auto reordered_envelope = handle_request(
        std::string(R"JSON({"jsonrpc":"2.0","id":2534,"method":"tools/call","params":{"arguments":)JSON") +
        arguments_before_name +
        R"JSON(,"name":"pulp_inspect_context"}})JSON");
    require_contains(reordered_envelope, R"JSON("buildId": "test-build")JSON");
    REQUIRE(std::none_of(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "State.setParameter";
        }));

    auto evaluate_arguments = [&](std::string_view expression_field) {
        auto arguments = selector;
        arguments.pop_back();
        if (!expression_field.empty()) {
            arguments += ',';
            arguments += expression_field;
        }
        return arguments + "}";
    };
    const std::pair<std::string, std::string> invalid_expressions[] = {
        {evaluate_arguments(""), "expression is required"},
        {evaluate_arguments(R"JSON("expression":7)JSON"),
         "expression must be a string"},
        {evaluate_arguments(R"JSON("expression":"")JSON"),
         "expression must not be empty"},
    };
    int invalid_expression_id = 2538;
    for (const auto& [arguments, expected] : invalid_expressions) {
        const auto response = handle_request(tool_call(
            std::to_string(invalid_expression_id++),
            "pulp_inspect_evaluate", arguments));
        require_contains(response, R"JSON("code":"invalid_arguments")JSON");
        require_contains(response, expected);
        REQUIRE(std::none_of(
            observed_requests.begin(), observed_requests.end(),
            [](const auto& request) {
                return request.method == "Runtime.evaluate";
            }));
    }

    const auto parameters = handle_request(
        tool_call("2523", "pulp_inspect_params", selector));
    require_contains(parameters, R"JSON("name": "gain")JSON");
    require_contains(parameters, R"JSON("value": -6)JSON");

    auto mutation_args = selector;
    mutation_args.pop_back();
    mutation_args += R"JSON(,"id":0,"value":0.75,"normalized":false})JSON";
    const auto mutation = handle_request(
        tool_call("2524", "pulp_inspect_set_param", mutation_args));
    require_contains(mutation, R"JSON("ok": true)JSON");
    require_contains(mutation, record.publication_id);

    auto nested_alternates =
        R"JSON({"nested":{"session_id":"other-session","instance_id":"other-instance","publication_id":"other-publication","id":99,"value":0.875},)JSON" +
        selector.substr(1);
    nested_alternates.pop_back();
    nested_alternates +=
        R"JSON(,"id":7,"value":0.125,"normalized":false})JSON";
    const auto nested_mutation = handle_request(
        tool_call("2535", "pulp_inspect_set_param", nested_alternates));
    require_contains(nested_mutation, R"JSON("ok": true)JSON");
    const auto nested_request = std::find_if(
        observed_requests.rbegin(), observed_requests.rend(), [](const auto& request) {
            return request.method == "State.setParameter";
        });
    REQUIRE(nested_request != observed_requests.rend());
    const auto nested_params = choc::json::parse(nested_request->params_json);
    REQUIRE(nested_params["id"].getInt64() == 7);
    REQUIRE(nested_params["value"].getFloat64() == 0.125);

    auto large_id_args = selector;
    large_id_args.pop_back();
    large_id_args +=
        R"JSON(,"id":2147483648,"value":0.5,"normalized":false})JSON";
    const auto large_id_mutation = handle_request(
        tool_call("2527", "pulp_inspect_set_param", large_id_args));
    require_contains(large_id_mutation, R"JSON("ok": true)JSON");
    REQUIRE(std::any_of(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "State.setParameter" &&
                   request.params_json.find("2147483648") != std::string::npos;
        }));

    auto maximum_id_args = selector;
    maximum_id_args.pop_back();
    maximum_id_args +=
        R"JSON(,"id":4294967295,"value":0.5,"normalized":false})JSON";
    const auto maximum_id_mutation = handle_request(
        tool_call("2532", "pulp_inspect_set_param", maximum_id_args));
    require_contains(maximum_id_mutation, R"JSON("ok": true)JSON");
    REQUIRE(std::any_of(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "State.setParameter" &&
                   request.params_json.find("4294967295") != std::string::npos;
        }));

    const auto mutation_count = std::count_if(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "State.setParameter";
        });
    auto wrong_selector_args =
        std::string(R"JSON({"session_id":")JSON") + record.session_id +
        R"JSON(","instance_id":")JSON" + record.instance_id +
        R"JSON(","publication_id":42,"id":7,"value":0.25})JSON";
    const auto wrong_selector = handle_request(
        tool_call("2528", "pulp_inspect_set_param", wrong_selector_args));
    require_contains(wrong_selector, "publication_id must be a string");
    require_contains(wrong_selector, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(std::count_if(
                observed_requests.begin(), observed_requests.end(),
                [](const auto& request) {
                    return request.method == "State.setParameter";
                }) == mutation_count);

    auto wrong_normalized_args = selector;
    wrong_normalized_args.pop_back();
    wrong_normalized_args +=
        R"JSON(,"id":7,"value":0.25,"normalized":"false"})JSON";
    const auto wrong_normalized = handle_request(
        tool_call("2536", "pulp_inspect_set_param", wrong_normalized_args));
    require_contains(wrong_normalized, "normalized must be a boolean");
    REQUIRE(std::count_if(
                observed_requests.begin(), observed_requests.end(),
                [](const auto& request) {
                    return request.method == "State.setParameter";
                }) == mutation_count);

    auto mismatched_selector_args = selector;
    const auto session_position =
        mismatched_selector_args.find(record.session_id);
    REQUIRE(session_position != std::string::npos);
    mismatched_selector_args.replace(
        session_position, record.session_id.size(), "other-session");
    mismatched_selector_args.pop_back();
    mismatched_selector_args += R"JSON(,"id":7,"value":0.25})JSON";
    const auto mismatched_selector = handle_request(
        tool_call("2533", "pulp_inspect_set_param", mismatched_selector_args));
    require_contains(mismatched_selector, "session_selection_failed");
    REQUIRE(std::count_if(
                observed_requests.begin(), observed_requests.end(),
                [](const auto& request) {
                    return request.method == "State.setParameter";
                }) == mutation_count);

    auto oversized_id_args = selector;
    oversized_id_args.pop_back();
    oversized_id_args += R"JSON(,"id":4294967296,"value":0.5})JSON";
    const auto oversized_id = handle_request(
        tool_call("2529", "pulp_inspect_set_param", oversized_id_args));
    require_contains(oversized_id, "uint32 range");
    REQUIRE(std::count_if(
                observed_requests.begin(), observed_requests.end(),
                [](const auto& request) {
                    return request.method == "State.setParameter";
                }) == mutation_count);

    auto trace_args = selector;
    trace_args.pop_back();
    trace_args += R"JSON(,"categories":["dsp","render","gpu"],"ring_mb":32})JSON";
    const auto trace = handle_request(
        tool_call("2530", "pulp_trace_start", trace_args));
    require_contains(trace, R"JSON("applied": true)JSON");
    const auto trace_request = std::find_if(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "Trace.startSession";
        });
    REQUIRE(trace_request != observed_requests.end());
    const auto trace_params = choc::json::parse(trace_request->params_json);
    REQUIRE(trace_params["categories"].isArray());
    REQUIRE(trace_params["categories"].size() == 3);
    REQUIRE(trace_params["categories"][2].getString() == "gpu");

    auto motion_args = selector;
    motion_args.pop_back();
    motion_args +=
        R"JSON(,"view_name":"Card \"A\"\nLine","fps":30,"metrics":[{"kind":"geometry","name":"frame","node_id":"card","properties":["x","y","width"]},{"kind":"scroll-geometry","name":"scroll","node_id":"list","properties":["offset","extent"],"source":{"axis":"vertical","nested":{"enabled":true}}}]})JSON";
    const auto motion = handle_request(
        tool_call("2531", "pulp_motion_start_trace", motion_args));
    require_contains(motion, R"JSON("applied": true)JSON");
    const auto motion_request = std::find_if(
        observed_requests.begin(), observed_requests.end(), [](const auto& request) {
            return request.method == "Motion.startTrace";
        });
    REQUIRE(motion_request != observed_requests.end());
    const auto motion_params = choc::json::parse(motion_request->params_json);
    REQUIRE(motion_params["view_name"].getString() == "Card \"A\"\nLine");
    REQUIRE(motion_params["metrics"].isArray());
    REQUIRE(motion_params["metrics"].size() == 2);
    REQUIRE(motion_params["metrics"][1]["source"]["nested"]["enabled"].getBool());

    const auto screenshot = handle_request(
        tool_call("2526", "pulp_inspect_screenshot", selector));
    require_contains(screenshot, R"JSON("mimeType": "image/png")JSON");
    require_contains(screenshot, R"JSON("width": 4)JSON");
    require_contains(screenshot, "iVBORw0KGgo=");

    // ui.read is deliberately absent from the fixture's available set. The
    // live server denial must remain structured and retain the exact selected
    // identity so an MCP client can attribute the failure without guessing.
    const auto denied = handle_request(
        tool_call("2525", "pulp_inspect_dom", selector));
    require_contains(denied, R"JSON("ok":false)JSON");
    require_contains(denied, R"JSON("isError":true)JSON");
    require_contains(denied, "capability_unavailable");
    require_contains(denied, R"JSON("session_id":"session-client-test")JSON");
    require_contains(denied, R"JSON("instance_id":"instance-client-test")JSON");
    require_contains(denied,
                     std::string(R"JSON("publication_id":")JSON") +
                         record.publication_id + "\"");
}

TEST_CASE("Inspector MCP selection failures are structured and source-tree independent",
          "[mcp][tools][inspect][selection][errors]") {
    TempDir runtime;
    TempDir outside_project;
    ScopedEnvVar runtime_dir("PULP_INSPECTOR_RUNTIME_DIR",
                             runtime.path.string());
    ScopedCurrentPath cwd(outside_project.path);

    const auto missing = handle_request(tool_call("2511", "pulp_inspect_context"));
    require_contains(missing, R"JSON("structuredContent")JSON");
    require_contains(missing, R"JSON("ok":false)JSON");
    require_contains(missing, "session_id is required");
    REQUIRE(missing.find("not in a Pulp project") == std::string::npos);

    const auto partial = handle_request(tool_call(
        "2512", "pulp_inspect_params", R"JSON({"instance_id":"instance-a"})JSON"));
    require_contains(partial, "invalid_selector");
    require_contains(partial, "session_id is required");
    require_contains(partial, R"JSON("isError":true)JSON");

    const auto invalid_mutation = handle_request(tool_call(
        "2513", "pulp_inspect_set_param", R"JSON({"id":0})JSON"));
    require_contains(invalid_mutation, "session_id is required");
    require_contains(invalid_mutation, R"JSON("code":"invalid_selector")JSON");

    const char* exact_tools[] = {
        "pulp_inspect_capabilities", "pulp_inspect_context",
        "pulp_inspect_dom",          "pulp_inspect_params",
        "pulp_inspect_value_channels", "pulp_inspect_set_param",
        "pulp_inspect_inject_midi", "pulp_inspect_set_transport",
        "pulp_inspect_screenshot",   "pulp_inspect_evaluate",
        "pulp_inspect_performance",  "pulp_inspect_audio",
        "pulp_motion_start_trace",   "pulp_motion_stop_trace",
        "pulp_motion_snapshot",      "pulp_motion_list_traces",
        "pulp_motion_scrub_to",      "pulp_motion_play",
        "pulp_motion_pause",         "pulp_motion_enable_cost",
        "pulp_motion_disable_cost",  "pulp_trace_start",
        "pulp_trace_stop",           "pulp_trace_snapshot",
        "pulp_trace_query",          "pulp_trace_explain",
    };
    int id = 2540;
    for (const char* tool : exact_tools) {
        INFO("exact live tool=" << tool);
        const auto response =
            handle_request(tool_call(std::to_string(id++), tool, "{}"));
        require_contains(response, R"JSON("code":"invalid_selector")JSON");
        require_contains(response, "session_id is required");
        REQUIRE(response.find("session_selection_failed") == std::string::npos);
    }
}

TEST_CASE("Typed test-input MCP schemas reject malformed and unowned fields",
          "[mcp][tools][inspect][test-input][arguments]") {
    const auto selector =
        R"JSON("session_id":"session-a","instance_id":"instance-b","publication_id":"publication-c")JSON";

    const auto missing_velocity = handle_request(tool_call(
        "2565", "pulp_inspect_inject_midi",
        std::string("{") + selector +
            R"JSON(,"kind":"note_on","channel":1,"note":60})JSON"));
    require_contains(missing_velocity, "velocity is required for note_on");
    require_contains(missing_velocity, R"JSON("code":"invalid_arguments")JSON");

    const auto raw_status = handle_request(tool_call(
        "2566", "pulp_inspect_inject_midi",
        std::string("{") + selector +
            R"JSON(,"kind":"note_off","channel":1,"note":60,"status":128})JSON"));
    require_contains(raw_status, "unknown test-input field: status");

    const auto bad_channel = handle_request(tool_call(
        "2567", "pulp_inspect_inject_midi",
        std::string("{") + selector +
            R"JSON(,"kind":"note_off","channel":17,"note":60})JSON"));
    require_contains(bad_channel, "channel is outside the supported range");

    const auto empty_transport = handle_request(tool_call(
        "2568", "pulp_inspect_set_transport",
        std::string("{") + selector + "}"));
    require_contains(empty_transport,
                     "set transport requires playing, position_samples, or tempo_bpm");

    const auto bad_tempo = handle_request(tool_call(
        "2569", "pulp_inspect_set_transport",
        std::string("{") + selector + R"JSON(,"tempo_bpm":401})JSON"));
    require_contains(bad_tempo, "tempo_bpm must be finite and from 20 to 400");

    const auto script = handle_request(tool_call(
        "2570", "pulp_inspect_set_transport",
        std::string("{") + selector +
            R"JSON(,"playing":true,"expression":"process.exit()"})JSON"));
    require_contains(script, "unknown test-input field: expression");
}

TEST_CASE("Motion and trace required arguments fail before live dispatch",
          "[mcp][tools][inspect][arguments]") {
    AuthenticatedFixture fixture;
    ScopedEnvVar runtime_dir("PULP_INSPECTOR_RUNTIME_DIR",
                             fixture.temporary.path.string());
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const auto& record = records.front();
    std::vector<InspectorMessage> observed_requests;
    fixture.observe_request = [&](const InspectorMessage& request) {
        observed_requests.push_back(request);
    };

    struct ToolCase {
        const char* name;
        const char* extras;
    };
    const ToolCase tools[] = {
        {"pulp_motion_start_trace", R"JSON("view_name":"Card","metrics":[])JSON"},
        {"pulp_motion_stop_trace", R"JSON("trace_id":1)JSON"},
        {"pulp_motion_snapshot", ""},
        {"pulp_motion_list_traces", ""},
        {"pulp_motion_scrub_to", R"JSON("frame":1)JSON"},
        {"pulp_motion_play", ""},
        {"pulp_motion_pause", ""},
        {"pulp_motion_enable_cost", ""},
        {"pulp_motion_disable_cost", ""},
        {"pulp_trace_start", ""},
        {"pulp_trace_stop", ""},
        {"pulp_trace_snapshot", ""},
        {"pulp_trace_query", ""},
        {"pulp_trace_explain", R"JSON("question":"why")JSON"},
    };
    const char* selectors[] = {
        "session_id", "instance_id", "publication_id",
    };
    auto arguments = [&](const ToolCase& tool,
                         std::string_view omitted_selector) {
        std::string json = "{";
        auto append = [&](std::string_view name, const std::string& value) {
            if (json.size() != 1) json += ',';
            json += json_string(std::string(name)) + ':' + json_string(value);
        };
        if (tool.extras[0] != '\0') json += tool.extras;
        if (omitted_selector != "session_id")
            append("session_id", record.session_id);
        if (omitted_selector != "instance_id")
            append("instance_id", record.instance_id);
        if (omitted_selector != "publication_id")
            append("publication_id", record.publication_id);
        return json + "}";
    };

    int id = 2580;
    for (const auto& tool : tools) {
        for (const char* selector : selectors) {
            INFO("tool=" << tool.name << " omitted=" << selector);
            const auto response = handle_request(tool_call(
                std::to_string(id++), tool.name, arguments(tool, selector)));
            require_contains(response, R"JSON("code":"invalid_selector")JSON");
            require_contains(response, std::string(selector) + " is required");
            REQUIRE(observed_requests.empty());
        }
    }

    const ToolCase missing_extras[] = {
        {"pulp_motion_start_trace", R"JSON("metrics":[])JSON"},
        {"pulp_motion_start_trace", R"JSON("view_name":"Card")JSON"},
        {"pulp_motion_stop_trace", ""},
        {"pulp_motion_scrub_to", ""},
        {"pulp_trace_explain", ""},
    };
    const char* missing_names[] = {
        "view_name", "metrics", "trace_id", "frame", "question",
    };
    for (std::size_t index = 0; index < std::size(missing_extras); ++index) {
        INFO("tool=" << missing_extras[index].name
                      << " omitted=" << missing_names[index]);
        const auto response = handle_request(tool_call(
            std::to_string(id++), missing_extras[index].name,
            arguments(missing_extras[index], "")));
        require_contains(response, R"JSON("code":"invalid_arguments")JSON");
        require_contains(response,
                         std::string(missing_names[index]) + " is required");
        REQUIRE(observed_requests.empty());
    }

    const ToolCase wrong_types[] = {
        {"pulp_motion_start_trace", R"JSON("view_name":7,"metrics":[])JSON"},
        {"pulp_motion_start_trace", R"JSON("view_name":"Card","metrics":{})JSON"},
        {"pulp_motion_start_trace", R"JSON("view_name":"Card","metrics":[{}])JSON"},
        {"pulp_motion_start_trace", R"JSON("view_name":"Card","metrics":[],"fps":"30")JSON"},
        {"pulp_motion_stop_trace", R"JSON("trace_id":"1")JSON"},
        {"pulp_motion_scrub_to", R"JSON("frame":1.5)JSON"},
        {"pulp_trace_start", R"JSON("categories":{})JSON"},
        {"pulp_trace_start", R"JSON("categories":["dsp",7])JSON"},
        {"pulp_trace_start", R"JSON("ring_mb":0)JSON"},
        {"pulp_trace_query", R"JSON("sql":7)JSON"},
        {"pulp_trace_query", R"JSON("preset":false)JSON"},
        {"pulp_trace_query", R"JSON("format":"xml")JSON"},
        {"pulp_trace_explain", R"JSON("question":[])JSON"},
    };
    for (const auto& tool : wrong_types) {
        INFO("wrong required type tool=" << tool.name);
        const auto response = handle_request(tool_call(
            std::to_string(id++), tool.name, arguments(tool, "")));
        require_contains(response, R"JSON("code":"invalid_arguments")JSON");
        REQUIRE(observed_requests.empty());
    }
}

// Per-tool SDK feature detection (min_sdk_version).
//
// pulp-mcp ships independently of any given Pulp project, so a user may
// have a newer pulp-mcp on PATH while editing a project pinned to an
// older SDK. compare_semver() / min_sdk_for_tool() / handle_compat() are
// the three pieces that make tools/call return a clean upgrade nudge
// instead of running the newer behavior the project author didn't pin.

TEST_CASE("compare_semver orders pulp version triples", "[mcp][compat][issue-2070]") {
    REQUIRE(compare_semver("0.99.0", "0.100.0") < 0); // 99 < 100 by numeric, not lex
    REQUIRE(compare_semver("0.100.0", "0.99.0") > 0);
    REQUIRE(compare_semver("1.0.0", "1.0.0") == 0);
    REQUIRE(compare_semver("1.0.0", "0.999.999") > 0);
    REQUIRE(compare_semver("0.0.0", "0.0.0") == 0);
    // Unparseable inputs fail open (treated as equal) so a malformed
    // pulp.toml or CMakeLists.txt can't strand a user out of every tool.
    REQUIRE(compare_semver("garbage", "0.99.0") == 0);
    REQUIRE(compare_semver("0.99.0", "not-a-version") == 0);
}

TEST_CASE("min_sdk_for_tool defaults to 0.0.0 for unlisted tools", "[mcp][compat][issue-2070]") {
    // Default for any tool not in TOOL_MIN_SDK_TABLE: no floor. This
    // preserves pre-#2070 behavior — only tools that explicitly opt in
    // get gated.
    REQUIRE(min_sdk_for_tool("pulp_build") == "0.0.0");
    REQUIRE(min_sdk_for_tool("pulp_audio_excerpt_find") == "0.0.0");
    REQUIRE(min_sdk_for_tool("a_tool_that_does_not_exist") == "0.0.0");
}

TEST_CASE("min_sdk_for_tool reports explicit floors for listed tools", "[mcp][compat]") {
    REQUIRE(min_sdk_for_tool("pulp_audio_render") == "0.513.0");
    REQUIRE(min_sdk_for_tool("pulp_audio_plugin_inspect") == "0.671.0");
}

TEST_CASE("pulp_compat reports versions and tool min_sdk map", "[mcp][compat][issue-2070]") {
    // Run from a project root so resolve_project_sdk_version() finds
    // the in-tree CMakeLists.txt VERSION (the source-tree fallback).
    ScopedCurrentPath cwd(repo_root());

    auto response = handle_request(tool_call("70", "pulp_compat"));
    require_contains(response, R"JSON("id":70)JSON");
    // Build version comes from PULP_MCP_SERVER_VERSION (generated header).
    require_contains(response, std::string(R"JSON("pulp_mcp_version":")JSON") +
                                   PULP_MCP_SERVER_VERSION + R"JSON(")JSON");
    // MCP wire protocol — independent of build version, tracks upstream spec.
    require_contains(response, R"JSON("mcp_protocol_version":"2024-11-05")JSON");
    // Should not be null when invoked from a Pulp project root.
    require_contains(response, R"JSON("project_sdk":")JSON");
    // The tool_min_sdk map MUST be present (even if empty) — clients
    // depend on the field existing so they can iterate it without a
    // null check.
    require_contains(response, R"JSON("tool_min_sdk":)JSON");
    require_contains(response, R"JSON("pulp_audio_render":"0.513.0")JSON");
    require_contains(response, R"JSON("pulp_audio_plugin_inspect":"0.671.0")JSON");
}

TEST_CASE("pulp_audio_render is gated for older pinned project SDKs", "[mcp][compat][audio]") {
    TempDir scratch;
    const auto project = scratch.path / "old-project";
    std::filesystem::create_directories(project / "core");
    std::ofstream(project / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.24)\n"
                                              << "project(OldPulpProject VERSION 0.512.0)\n";

    ScopedCurrentPath cwd(project);
    auto response = handle_request(tool_call(
        "72", "pulp_audio_render", R"JSON({"plugin":"Demo.clap","duration_frames":64})JSON"));
    require_contains(response, R"JSON("id":72)JSON");
    require_contains(response, R"JSON("isError":true)JSON");
    require_contains(response, R"JSON("tool":"pulp_audio_render")JSON");
    require_contains(response, R"JSON("required_sdk":"0.513.0")JSON");
    require_contains(response, R"JSON("project_sdk":"0.512.0")JSON");
    require_contains(response, "pulp project bump");
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
    require_contains(response, std::string(R"JSON("pulp_mcp_version":")JSON") +
                                   PULP_MCP_SERVER_VERSION + R"JSON(")JSON");
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
    for (char c : version)
        if (c == '.')
            ++dots;
    INFO("parsed CMakeLists.txt version='" << version << "'");
    REQUIRE(dots == 2);
    REQUIRE_FALSE(version.empty());
}

TEST_CASE("parse_cmake_project_version handles plugin VERSION fallback", "[mcp][compat]") {
    REQUIRE(pulp_mcp::parse_cmake_project_version(
                "cmake_minimum_required(VERSION 3.25)\n"
                "pulp_add_plugin(MyPlugin VERSION \"1.2.3\")\n") == "1.2.3");

    REQUIRE(pulp_mcp::parse_cmake_project_version("pulp_add_plugin(\n"
                                                  "  MyPlugin\n"
                                                  "  FORMAT VST3\n"
                                                  "  VERSION 2.3.4\n"
                                                  ")\n") == "2.3.4");

    REQUIRE(pulp_mcp::parse_cmake_project_version(
                "project(Product VERSION 4.5.6)\n"
                "pulp_add_plugin(MyPlugin VERSION \"1.2.3\")\n") == "4.5.6");

    REQUIRE(pulp_mcp::parse_cmake_project_version("pulp_add_plugin(MyPlugin NAME Only)\n").empty());
    REQUIRE(pulp_mcp::parse_cmake_project_version("pulp_add_plugin(MyPlugin VERSION \"1.2\")\n")
                .empty());
    REQUIRE(pulp_mcp::parse_cmake_project_version("pulp_add_plugin(MyPlugin VERSION \"1.2.3.4\")\n")
                .empty());
    REQUIRE(pulp_mcp::parse_cmake_project_version(
                "pulp_add_plugin(MyPlugin VERSION \"1.2.3-beta\")\n") == "1.2.3");
}

TEST_CASE("compare_semver tolerates metadata and leading zeroes", "[mcp][compat]") {
    REQUIRE(compare_semver("1.2.3-alpha", "1.2.3") == 0);
    REQUIRE(compare_semver("1.2.4+build", "1.2.3") > 0);
    REQUIRE(compare_semver("01.002.0003", "1.2.3") == 0);
    REQUIRE(compare_semver("1.10.0", "1.2.99") > 0);
    REQUIRE(compare_semver("2.0", "1.9.9") == 0);
    REQUIRE(compare_semver("", "1.9.9") == 0);
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
        std::string out;
        out.reserve(s.size());
        for (char c : s)
            if (c != '\n' && c != '\r')
                out += c;
        return out;
    };

    auto initialize_wire =
        strip_newlines(handle_request(R"JSON({"jsonrpc":"2.0","id":1,"method":"initialize"})JSON"));
    auto tools_list_wire =
        strip_newlines(handle_request(R"JSON({"jsonrpc":"2.0","id":2,"method":"tools/list"})JSON"));

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

// Every pulp_motion_* tool must be recognized by the dispatcher (no "Unknown
// tool" fall-through) and reach the shared direct-client path. An empty
// owner-private runtime directory produces structured discovery/selection
// failures without requiring a source checkout.
TEST_CASE("MCP pulp_motion_* tools route to the motion dispatch arm",
          "[mcp][tools][motion][issue-2153]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", temp.path.string());

    // Tools that take no params at all — invoke with empty arguments.
    const auto no_param_tools = {
        "pulp_motion_snapshot", "pulp_motion_list_traces", "pulp_motion_play",
        "pulp_motion_pause",    "pulp_motion_enable_cost", "pulp_motion_disable_cost",
    };
    int id = 80;
    for (const char* tool : no_param_tools) {
        INFO("motion tool (no params): " << tool);
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        // Structured failure proves the dispatcher reached direct discovery.
        require_contains(response, R"JSON("isError":true)JSON");
        require_contains(response, R"JSON("structuredContent")JSON");
        // Guard against the silent-regression case where the dispatch
        // arm gets removed but the tools/list registration stays.
        REQUIRE(response.find("Unknown tool") == std::string::npos);
    }

    // Tools that take params — confirm the same routing with a
    // representative non-empty argument shape.
    auto start_trace = handle_request(tool_call(
        std::to_string(id++), "pulp_motion_start_trace",
        R"JSON({"view_name":"Card","fps":30,"metrics":[{"kind":"geometry","name":"frame","node_id":"card"}]})JSON"));
    require_contains(start_trace, "invalid_selector");
    REQUIRE(start_trace.find("Unknown tool") == std::string::npos);

    auto stop_trace = handle_request(
        tool_call(std::to_string(id++), "pulp_motion_stop_trace", R"JSON({"trace_id":1})JSON"));
    require_contains(stop_trace, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(stop_trace.find("Unknown tool") == std::string::npos);

    auto scrub_to = handle_request(
        tool_call(std::to_string(id++), "pulp_motion_scrub_to", R"JSON({"frame":42})JSON"));
    require_contains(scrub_to, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(scrub_to.find("Unknown tool") == std::string::npos);
}

TEST_CASE("MCP does not expose filesystem-backed motion fixture loading",
          "[mcp][tools][motion][security]") {
    const auto tools = handle_request(
        R"JSON({"jsonrpc":"2.0","id":99,"method":"tools/list"})JSON");
    REQUIRE(tools.find("pulp_motion_load_fixture") == std::string::npos);
    const auto response = handle_request(tool_call(
        "100", "pulp_motion_load_fixture",
        R"JSON({"path":"/tmp/example.motion.jsonl"})JSON"));
    require_contains(response, "Unknown tool: pulp_motion_load_fixture");
}

// This code-shape check proves that the grantable pulp_motion_* MCP tools
// map to the right canonical Motion.* inspector protocol constants. The source
// text assertion mirrors the existing inspector-mapping test — the
// actual round-trip lands at MotionInspector::handle /
// MotionScrubber::handle, which run inside the inspected process and
// already have their own dedicated test coverage in
// test_motion_inspector.cpp / test_motion_scrubber.cpp.
TEST_CASE("MCP pulp_motion_* tools map to expected Motion.* methods",
          "[mcp][tools][motion][issue-2153]") {
    auto src_path = repo_root_path() / "tools" / "mcp" /
                    "mcp_inspect_tools.cpp";
    REQUIRE(std::filesystem::exists(src_path));

    std::ifstream in(src_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    const std::pair<const char*, const char*> mappings[] = {
        {"pulp_motion_start_trace", "methods::kMotionStartTrace"},
        {"pulp_motion_stop_trace", "methods::kMotionStopTrace"},
        {"pulp_motion_snapshot", "methods::kMotionSnapshot"},
        {"pulp_motion_list_traces", "methods::kMotionListTraces"},
        {"pulp_motion_scrub_to", "methods::kMotionScrubTo"},
        {"pulp_motion_play", "methods::kMotionPlay"},
        {"pulp_motion_pause", "methods::kMotionPause"},
        {"pulp_motion_enable_cost", "methods::kMotionEnableCost"},
        {"pulp_motion_disable_cost", "methods::kMotionDisableCost"},
    };
    for (const auto& [tool, method] : mappings) {
        INFO("motion tool=" << tool << " method=" << method);
        REQUIRE(src.find(tool) != std::string::npos);
        REQUIRE(src.find(method) != std::string::npos);
    }
}

// Confirm every pulp_motion_* tool advertised in
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
        std::pair{"pulp_motion_start_trace", "view_name"},
        std::pair{"pulp_motion_stop_trace", "trace_id"},
        std::pair{"pulp_motion_scrub_to", "frame"},
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
    const auto motion_start =
        tools.find(R"JSON("name":"pulp_motion_start_trace")JSON");
    const auto motion_stop = tools.find(R"JSON("name":"pulp_motion_stop_trace")JSON");
    REQUIRE(motion_start != std::string::npos);
    REQUIRE(motion_stop != std::string::npos);
    const auto motion_start_schema =
        tools.substr(motion_start, motion_stop - motion_start);
    require_contains(motion_start_schema, R"JSON("session_id":{"type":"string")JSON");
    require_contains(motion_start_schema, R"JSON("instance_id":{"type":"string")JSON");
    require_contains(motion_start_schema, R"JSON("publication_id":{"type":"string")JSON");
    const auto motion_stop_schema = tools.substr(motion_stop, 1200);
    require_contains(motion_stop_schema,
                     R"JSON("required":["trace_id","session_id","instance_id","publication_id"])JSON");
    for (const char* tool : {
             "pulp_motion_play",
             "pulp_motion_pause",
             "pulp_motion_enable_cost",
             "pulp_motion_disable_cost",
         }) {
        INFO("exact-session motion tool: " << tool);
        const auto position =
            tools.find(std::string(R"JSON("name":")JSON") + tool + "\"");
        REQUIRE(position != std::string::npos);
        require_contains(
            tools.substr(position, 900),
            R"JSON("required":["session_id","instance_id","publication_id"])JSON");
    }
    const auto scrub = tools.find(R"JSON("name":"pulp_motion_scrub_to")JSON");
    REQUIRE(scrub != std::string::npos);
    require_contains(
        tools.substr(scrub, 1000),
        R"JSON("required":["frame","session_id","instance_id","publication_id"])JSON");

    // Param-less tools still need a description + an inputSchema object.
    const auto param_less_tools = {
        "pulp_motion_snapshot", "pulp_motion_list_traces", "pulp_motion_play",
        "pulp_motion_pause",    "pulp_motion_enable_cost", "pulp_motion_disable_cost",
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

TEST_CASE("MCP pulp_trace_* tools route to the trace dispatch arm", "[mcp][tools][trace]") {
    TempDir temp;
    ScopedCurrentPath cwd(temp.path);
    ScopedEnvVar runtime("PULP_INSPECTOR_RUNTIME_DIR", temp.path.string());

    const auto no_param_tools = {"pulp_trace_snapshot"};
    int id = 90;
    for (const char* tool : no_param_tools) {
        INFO("trace tool (no params): " << tool);
        auto response = handle_request(tool_call(std::to_string(id++), tool));
        // Reaching structured discovery proves the dispatch arm is present.
        require_contains(response, "invalid_selector");
        REQUIRE(response.find("Unknown tool") == std::string::npos);
    }

    auto start =
        handle_request(tool_call(std::to_string(id++), "pulp_trace_start",
                                 R"JSON({"categories":["dsp","render"],"ring_mb":32})JSON"));
    require_contains(start, "invalid_selector");
    REQUIRE(start.find("Unknown tool") == std::string::npos);

    auto stop = handle_request(tool_call(
        std::to_string(id++), "pulp_trace_stop",
        R"JSON({"session_id":"session-a","instance_id":"instance-b"})JSON"));
    require_contains(stop, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(stop.find("Unknown tool") == std::string::npos);

    const auto tools = handle_request(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    const auto trace_start = tools.find(R"("name":"pulp_trace_start")");
    REQUIRE(trace_start != std::string::npos);
    const auto trace_stop = tools.find(R"("name":"pulp_trace_stop")", trace_start);
    REQUIRE(trace_stop != std::string::npos);
    const auto trace_schema = tools.substr(
        trace_start, trace_stop - trace_start);
    require_contains(trace_schema, R"("minimum":1)");
    require_contains(trace_schema, R"("maximum":512)");
    require_contains(trace_schema, R"JSON("session_id":{"type":"string")JSON");
    require_contains(trace_schema, R"JSON("instance_id":{"type":"string")JSON");
    require_contains(trace_schema, R"JSON("publication_id":{"type":"string")JSON");
    REQUIRE(trace_schema.find(R"("out_path")") == std::string::npos);
    const auto trace_stop_schema =
        tools.substr(trace_stop, tools.find(R"("name":"pulp_trace_snapshot")",
                                           trace_stop) - trace_stop);
    require_contains(
        trace_stop_schema,
        R"JSON("required":["session_id","instance_id","publication_id"])JSON");

    auto query = handle_request(tool_call(std::to_string(id++), "pulp_trace_query",
                                          R"JSON({"sql":"select 1","format":"json"})JSON"));
    require_contains(query, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(query.find("Unknown tool") == std::string::npos);

    auto explain = handle_request(tool_call(std::to_string(id++), "pulp_trace_explain",
                                            R"JSON({"question":"why is startup slow?"})JSON"));
    require_contains(explain, R"JSON("code":"invalid_selector")JSON");
    REQUIRE(explain.find("Unknown tool") == std::string::npos);
}

// Code-shape check that the pulp_trace_* MCP tools map to the right canonical
// Trace.* inspector method constants. Source-text assertion mirrors the motion mapping
// test; the round-trip itself lands at TraceInspector::handle, covered by
// test_trace_inspector.cpp.
TEST_CASE("MCP pulp_trace_* tools map to expected Trace.* methods", "[mcp][tools][trace]") {
    auto src_path = repo_root_path() / "tools" / "mcp" /
                    "mcp_inspect_tools.cpp";
    REQUIRE(std::filesystem::exists(src_path));

    std::ifstream in(src_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    const std::pair<const char*, const char*> mappings[] = {
        {"pulp_trace_start", "methods::kTraceStartSession"},
        {"pulp_trace_stop", "methods::kTraceStopSession"},
        {"pulp_trace_snapshot", "methods::kTraceSnapshot"},
        {"pulp_trace_query", "methods::kTraceQuery"},
        {"pulp_trace_explain", "methods::kTraceExplain"},
    };
    for (const auto& [tool, method] : mappings) {
        INFO("trace tool=" << tool << " method=" << method);
        REQUIRE(src.find(tool) != std::string::npos);
        REQUIRE(src.find(method) != std::string::npos);
    }
}

TEST_CASE("MCP pulp_trace_* tools carry discoverable input schemas", "[mcp][tools][trace]") {
    auto tools = handle_request(R"JSON({"jsonrpc":"2.0","id":98,"method":"tools/list"})JSON");

    // Explain requires a question in addition to the exact capture selector.
    {
        std::string name_key = R"JSON("name":"pulp_trace_explain")JSON";
        auto pos = tools.find(name_key);
        REQUIRE(pos != std::string::npos);
        auto window = tools.substr(pos, 1500);
        REQUIRE(window.find(R"JSON("description":")JSON") != std::string::npos);
        auto req_pos = window.find(R"JSON("required":[)JSON");
        REQUIRE(req_pos != std::string::npos);
        auto req_end = window.find(']', req_pos);
        REQUIRE(req_end != std::string::npos);
        auto required_window = window.substr(req_pos, req_end - req_pos + 1);
        REQUIRE(required_window.find(R"JSON("question")JSON") != std::string::npos);
    }
    for (const char* tool : {"pulp_trace_query", "pulp_trace_explain"}) {
        const auto position =
            tools.find(std::string(R"JSON("name":")JSON") + tool + "\"");
        REQUIRE(position != std::string::npos);
        const auto schema = tools.substr(position, 1600);
        require_contains(
            schema,
            R"JSON("session_id","instance_id","publication_id")JSON");
    }

    // Every trace tool needs a description + an inputSchema object.
    const auto all_trace_tools = {
        "pulp_trace_start", "pulp_trace_stop",    "pulp_trace_snapshot",
        "pulp_trace_query", "pulp_trace_explain",
    };
    for (const char* tool : all_trace_tools) {
        INFO("trace tool: " << tool);
        std::string name_key = std::string(R"JSON("name":")JSON") + tool + R"JSON(")JSON";
        auto pos = tools.find(name_key);
        REQUIRE(pos != std::string::npos);
        auto window = tools.substr(pos, 900);
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
    // SDK-mode projects generated by `pulp create` put the pin under
    // [pulp]; unrelated section-local sdk_version keys describe something
    // else and must not be accepted.
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("[pulp]\nsdk_version = \"1.2.3\"\n") == "1.2.3");
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version(
                "[pulp]\nsdk_version = \"1.2.3\"\n[dependency]\nsdk_version = \"9.9.9\"\n") ==
            "1.2.3");
    REQUIRE(
        pulp_mcp::parse_pulp_toml_sdk_version("[dependency]\nsdk_version = \"9.9.9\"\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version(
                "sdk_version = \"1.2.3\"\n[dependency]\nsdk_version = \"9.9.9\"\n") == "1.2.3");
}

TEST_CASE("parse_pulp_toml_sdk_version rejects ambiguous sdk pins", "[mcp][compat]") {
    REQUIRE(
        pulp_mcp::parse_pulp_toml_sdk_version("[pulp.extra]\nsdk_version = \"9.9.9\"\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("sdk_version = '1.2.3'\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("sdk_version = 1.2.3\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("sdk_versionish = \"1.2.3\"\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version("sdk_version = \"1.2.3\n").empty());
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version(
                "[pulp]\nsdk_version = \"2.3.4\" # generated pin\n") == "2.3.4");
    REQUIRE(pulp_mcp::parse_pulp_toml_sdk_version(
                "[tools]\nsdk_version = \"9.9.9\"\n[pulp]\nsdk_version = \"2.3.4\"\n") == "2.3.4");
}

TEST_CASE("pulp_inspect_pending_requests returns a project's unconsumed queue",
          "[mcp][tools][agent-queue]") {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "pulp_mcp_pending_requests_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string project = dir.string();
    const std::string path = pulp::inspect::queue_path(project);
    auto args = [&] { return std::string(R"({"project_dir":")") + project + R"("})"; };

    // An absent queue reads as an empty array, never an error.
    auto empty = handle_request(tool_call("80", "pulp_inspect_pending_requests", args()));
    require_contains(empty, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(empty, "[]");

    // Seed one pending + one consumed request; only the pending one comes back.
    pulp::inspect::AgentRequest a;
    a.text = "make it airy";
    a.design = "synth-panel";
    a.created_at = "2026-07-06T18:00:00Z";
    REQUIRE(pulp::inspect::enqueue_to_file(path, a).has_value());
    pulp::inspect::AgentRequest b;
    b.text = "already handled";
    auto id_b = pulp::inspect::enqueue_to_file(path, b);
    REQUIRE(id_b.has_value());
    REQUIRE(pulp::inspect::ack_in_file(path, *id_b));

    auto resp = handle_request(tool_call("81", "pulp_inspect_pending_requests", args()));
    require_contains(resp, "make it airy");
    require_contains(resp, "synth-panel");
    REQUIRE(resp.find("already handled") == std::string::npos); // consumed → filtered

    fs::remove_all(dir, ec);
}

TEST_CASE("pulp_audio_render validates its latency arguments before shelling out",
          "[mcp][tools][audio][latency]") {
    // Each guard below returns an actionable arg_error BEFORE the CLI is
    // spawned, so a bad latency request costs nothing and says what is wrong.
    // The proof itself is exercised end-to-end by the CLI shellout tests; what
    // is asserted here is that the MCP surface refuses garbage rather than
    // forwarding it.
    ScopedCurrentPath cwd(repo_root());
    int id = 90;
    auto call = [&](const char* args) {
        return handle_request(tool_call(std::to_string(id++), "pulp_audio_render", args));
    };

    // A negative tolerance / intrinsic / expected latency is not a delay.
    require_contains(
        call(R"({"plugin":"X.clap","duration_ms":100,"latency":true,"latency_tolerance":-1})"),
        "latency_tolerance must be an integer >= 0");
    require_contains(
        call(R"({"plugin":"X.clap","duration_ms":100,"latency":true,"latency_intrinsic":-5})"),
        "latency_intrinsic must be an integer >= 0");
    require_contains(
        call(R"({"plugin":"X.clap","duration_ms":100,"latency":true,"latency_expect":-2})"),
        "latency_expect must be an integer >= 0");

    // A well-formed latency request reaches the shellout (which fails here --
    // the plugin does not exist -- but as a well-formed JSON-RPC result, not a
    // validation error). This is the branch that assembles the CLI flags.
    auto ok = call(R"({"plugin":"X.clap","duration_ms":100,"latency":true,
                       "input_signal":"noise","latency_policy":"delayed-null",
                       "latency_tolerance":0,"latency_expect":512})");
    require_contains(ok, R"JSON("jsonrpc":"2.0")JSON");
    require_contains(ok, R"JSON("content")JSON");
}
