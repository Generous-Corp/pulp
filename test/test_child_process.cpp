// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::platform;

TEST_CASE("exec captures stdout", "[child_process]") {
#ifdef _WIN32
    auto r = exec("cmd", {"/c", "echo", "hello world"}, 5000);
#else
    auto r = exec("echo", {"hello world"}, 5000);
#endif
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("hello") != std::string::npos);
}

TEST_CASE("exec captures non-zero exit code", "[child_process]") {
#ifdef _WIN32
    auto r = exec("cmd", {"/c", "exit", "42"}, 5000);
#else
    auto r = exec("/bin/sh", {"-c", "exit 42"}, 5000);
#endif
    REQUIRE(r.exit_code == 42);
}

TEST_CASE("exec respects timeout", "[child_process]") {
#ifdef _WIN32
    auto r = exec("ping", {"-n", "10", "127.0.0.1"}, 500);
#else
    auto r = exec("sleep", {"10"}, 500);
#endif
    REQUIRE(r.timed_out);
}

TEST_CASE("line callback fires per line", "[child_process]") {
    std::vector<std::string> lines;
    ProcessOptions opts;
    opts.on_stdout_line = [&](std::string_view line) {
        lines.push_back(std::string(line));
    };
    opts.timeout_ms = 5000;

#ifdef _WIN32
    auto r = ChildProcess::run("cmd", {"/c", "echo line1& echo line2& echo line3"}, opts);
#else
    auto r = ChildProcess::run("/bin/sh", {"-c", "echo line1; echo line2; echo line3"}, opts);
#endif
    REQUIRE(r.exit_code == 0);
    REQUIRE(lines.size() >= 3);
    REQUIRE(lines[0] == "line1");
    REQUIRE(lines[1] == "line2");
    REQUIRE(lines[2] == "line3");
}

TEST_CASE("cancel terminates process", "[child_process]") {
    ChildProcess cp;
#ifdef _WIN32
    REQUIRE(cp.start("ping", {"-n", "100", "127.0.0.1"}));
#else
    REQUIRE(cp.start("sleep", {"30"}));
#endif
    REQUIRE(cp.is_running());
    cp.cancel();
    auto r = cp.wait();
    REQUIRE(r.was_cancelled);
}

TEST_CASE("find_on_path finds known binary", "[child_process]") {
#ifdef _WIN32
    auto p = find_on_path("cmd.exe");
#else
    auto p = find_on_path("sh");
#endif
    REQUIRE(p.has_value());
}

TEST_CASE("find_on_path returns nullopt for nonexistent", "[child_process]") {
    auto p = find_on_path("this_binary_definitely_does_not_exist_xyz123");
    REQUIRE_FALSE(p.has_value());
}

TEST_CASE("run with empty command fails gracefully", "[child_process]") {
    auto r = ChildProcess::run("", {});
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("stderr is captured separately", "[child_process]") {
    ProcessOptions opts;
    opts.timeout_ms = 5000;

#ifdef _WIN32
    auto r = ChildProcess::run("cmd", {"/c", "echo error_output 1>&2"}, opts);
#else
    auto r = ChildProcess::run("/bin/sh", {"-c", "echo error_output >&2"}, opts);
#endif
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output.find("error_output") != std::string::npos);
}
