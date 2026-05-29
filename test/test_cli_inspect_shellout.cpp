// Focused shell-out coverage for `pulp inspect`.
//
// These tests launch the built CLI against a real in-process
// InspectorServer so cmd_inspect.cpp's one-shot success, discovery,
// output-file, and protocol-error paths are covered end-to-end.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/runtime/socket.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pulp_test_cli;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorServer;
using pulp::inspect::make_error;
using pulp::inspect::make_response;
using pulp::runtime::Socket;
using pulp::runtime::SocketType;

namespace {

std::uint16_t inspect_socket_seed() {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(getpid());
#endif
    return static_cast<std::uint16_t>((now + pid * 97u) % 20000u);
}

std::optional<std::uint16_t> start_test_inspector(InspectorServer& server) {
    const auto seed = inspect_socket_seed();
    for (std::uint16_t i = 0; i < 200; ++i) {
        const auto port = static_cast<std::uint16_t>(
            20000 + ((seed + i) % 20000));
        if (server.start(port))
            return port;
    }
    return std::nullopt;
}

std::optional<std::uint16_t> find_unbound_port() {
    const auto seed = inspect_socket_seed();
    for (std::uint16_t i = 0; i < 200; ++i) {
        const auto port = static_cast<std::uint16_t>(
            20000 + ((seed + i) % 20000));
        Socket socket;
        if (socket.create(SocketType::TCP) && socket.bind("127.0.0.1", port))
            return port;
    }
    return std::nullopt;
}

const char* temp_env_name() {
#ifdef _WIN32
    return "TEMP";
#else
    return "TMPDIR";
#endif
}

struct InspectServerFixture {
    fs::path temp = unique_temp_dir("pulp-cli-inspect-shellout");
    ScopedEnvVar temp_env{temp_env_name()};
    ScopedEnvVar update_disabled{"PULP_UPDATE_CHECK_DISABLED"};
    InspectorServer server;
    std::uint16_t port = 0;
    std::vector<InspectorMessage> seen;

    InspectServerFixture() {
        fs::create_directories(temp);
        temp_env.set(temp.string());
        update_disabled.set("1");

        auto started = start_test_inspector(server);
        REQUIRE(started.has_value());
        port = *started;
    }

    ~InspectServerFixture() {
        server.stop();
        std::error_code ec;
        fs::remove_all(temp, ec);
    }

    std::string port_string() const { return std::to_string(port); }
};

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string compact_json_for_assertion(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    return text;
}

}  // namespace

TEST_CASE("pulp inspect one-shot prints a server response",
          "[cli][shellout][inspect][coverage][requested]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.server.set_request_handler([&](const InspectorMessage& request) {
        fixture.seen.push_back(request);
        return make_response(request.id,
                             R"({"ok":true,"source":"inspect-test"})");
    });

    auto result = run_pulp({"inspect",
                            "--host", "127.0.0.1",
                            "--port", fixture.port_string(),
                            "--command", "DOM.getDocument",
                            "--params", R"({"depth":2})"},
                           10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.empty());
    REQUIRE(result.stdout_output.find("Connecting to 127.0.0.1:" +
                                      fixture.port_string()) !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("Connected to inspector") !=
            std::string::npos);
    REQUIRE(compact_json_for_assertion(result.stdout_output)
                .find(R"({"ok":true,"source":"inspect-test"})") !=
            std::string::npos);
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].id == 1);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE(compact_json_for_assertion(fixture.seen[0].params_json) ==
            R"({"depth":2})");
}

TEST_CASE("pulp inspect one-shot can discover the advertised server port",
          "[cli][shellout][inspect][coverage][requested]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.server.set_request_handler([&](const InspectorMessage& request) {
        fixture.seen.push_back(request);
        return make_response(request.id, R"({"discovered":true})");
    });

    auto result = run_pulp({"inspect", "--command", "Runtime.evaluate"}, 10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.empty());
    REQUIRE(result.stdout_output.find("Found inspector on port " +
                                      fixture.port_string()) !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("Connecting to 127.0.0.1:" +
                                      fixture.port_string()) !=
            std::string::npos);
    REQUIRE(compact_json_for_assertion(result.stdout_output)
                .find(R"({"discovered":true})") !=
            std::string::npos);
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "Runtime.evaluate");
    REQUIRE((fixture.seen[0].params_json.empty() ||
             fixture.seen[0].params_json == "{}"));
}

TEST_CASE("pulp inspect one-shot writes output files and propagates errors",
          "[cli][shellout][inspect][coverage][requested]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    const auto out = fixture.temp / "inspect-response.json";

    fixture.server.set_request_handler([&](const InspectorMessage& request) {
        fixture.seen.push_back(request);
        if (request.method == "Inspector.fail")
            return make_error(request.id, "server rejected request");
        return make_response(request.id, R"({"file":true,"value":42})");
    });

    auto written = run_pulp({"inspect",
                             "--port", fixture.port_string(),
                             "--command", "Inspector.snapshot",
                             "--output", out.string()},
                            10000);

    REQUIRE_FALSE(written.timed_out);
    REQUIRE(written.exit_code == 0);
    REQUIRE(written.stderr_output.empty());
    REQUIRE(written.stdout_output.find("Written to " + out.string()) !=
            std::string::npos);
    REQUIRE(fs::exists(out));
    REQUIRE(compact_json_for_assertion(read_text_file(out)) ==
            R"({"file":true,"value":42})");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "Inspector.snapshot");

    auto failed = run_pulp({"inspect",
                            "--port", fixture.port_string(),
                            "--command", "Inspector.fail",
                            "--output", (fixture.temp / "failed.json").string()},
                           10000);

    REQUIRE_FALSE(failed.timed_out);
    REQUIRE(failed.exit_code == 1);
    REQUIRE(failed.stderr_output.find("server rejected request") !=
            std::string::npos);
    REQUIRE_FALSE(fs::exists(fixture.temp / "failed.json"));
    REQUIRE(fixture.seen.size() == 2);
    REQUIRE(fixture.seen[1].method == "Inspector.fail");
}

TEST_CASE("pulp inspect validates missing values before connecting",
          "[cli][shellout][inspect][coverage][requested]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    const auto unused_port = find_unbound_port();
    REQUIRE(unused_port.has_value());

    struct Case {
        std::vector<std::string> args;
        std::string diagnostic;
    };

    const std::vector<Case> cases = {
        {{"inspect", "--host"}, "--host requires a value"},
        {{"inspect", "--command"}, "--command requires a value"},
        {{"inspect", "--params"}, "--params requires a value"},
        {{"inspect", "--output"}, "--output requires a value"},
    };

    for (const auto& c : cases) {
        INFO(c.diagnostic);
        auto result = run_pulp(c.args, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
        REQUIRE(result.stderr_output.find(c.diagnostic) != std::string::npos);
        REQUIRE(result.stdout_output.find("Connecting to") == std::string::npos);
    }

    auto invalid_negative = run_pulp({"inspect", "--port", "-1"}, 10000);
    REQUIRE_FALSE(invalid_negative.timed_out);
    REQUIRE(invalid_negative.exit_code == 2);
    REQUIRE(invalid_negative.stderr_output.find("invalid --port value: -1") !=
            std::string::npos);
    REQUIRE(invalid_negative.stdout_output.find("Connecting to") ==
            std::string::npos);
}
