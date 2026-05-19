// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace pulp::platform;

namespace {

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' '))
        s.erase(s.begin());
    return s;
}

}  // namespace

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

TEST_CASE("find_on_path returns nullopt for missing binary",
          "[child_process][edge]") {
    auto p = find_on_path("pulp-does-not-exist-xyz-12345");
    REQUIRE_FALSE(p.has_value());
}

TEST_CASE("exec with a binary that does not exist reports failure",
          "[child_process][edge]") {
    auto r = exec("pulp-also-does-not-exist-xyz", {}, 1000);
    // Implementations report this as non-zero exit + empty stdout;
    // must not time out and must not claim success.
    REQUIRE(r.exit_code != 0);
    REQUIRE_FALSE(r.timed_out);
}

TEST_CASE("exec captures empty-stdout cleanly", "[child_process][edge]") {
#ifdef _WIN32
    auto r = exec("cmd", {"/c", "rem nothing"}, 5000);
#else
    auto r = exec("/bin/sh", {"-c", ":"}, 5000);  // ':' = no-op builtin
#endif
    REQUIRE(r.exit_code == 0);
    // Accept zero-length or whitespace-only output; the key invariant
    // is that the capture path doesn't crash on a zero-byte read.
    for (char c : r.stdout_output) {
        REQUIRE((c == ' ' || c == '\t' || c == '\r' || c == '\n'));
    }
}

TEST_CASE("find_on_path returns nullopt for nonexistent", "[child_process]") {
    auto p = find_on_path("this_binary_definitely_does_not_exist_xyz123");
    REQUIRE_FALSE(p.has_value());
}

TEST_CASE("run with empty command fails gracefully", "[child_process]") {
    auto r = ChildProcess::run("", {});
    REQUIRE(r.exit_code != 0);
}

#ifdef _WIN32
TEST_CASE("exec preserves embedded quotes and trailing backslashes on Windows",
          "[child_process][win][issue-493]") {
    auto powershell = find_on_path("powershell.exe");
    if (!powershell) {
        SUCCEED("skipped: powershell.exe not found");
        return;
    }

    const std::string expected = "say \"hi\" C:\\tmp\\";
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    auto r = ChildProcess::run(
        powershell->string(),
        {"-NoProfile",
         "-Command",
         "param([string]$x) [Console]::Out.Write($x)",
         expected},
        opts);

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output == expected);
}
#endif

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

TEST_CASE("wait and read before start return default results",
          "[child_process][edge][issue-640]") {
    ChildProcess cp;

    REQUIRE_FALSE(cp.is_running());
    REQUIRE(cp.read_available_output().empty());

    auto r = cp.wait();
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.stdout_output.empty());
    REQUIRE(r.stderr_output.empty());
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
}

TEST_CASE("read_available_output drains stdout while process is running",
          "[child_process][edge][issue-640]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd",
                     {"/c", "<nul set /p dummy=available& ping -n 3 127.0.0.1 >nul"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf available; sleep 1"}));
#endif

    std::string observed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observed.find("available") == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        observed += cp.read_available_output();
        if (observed.find("available") == std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(observed.find("available") != std::string::npos);

    auto r = cp.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
}

TEST_CASE("run honors working directory",
          "[child_process][edge][issue-640]") {
    auto dir = std::filesystem::temp_directory_path()
        / "pulp-child-process-working-dir";
    std::filesystem::create_directories(dir);

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.working_directory = dir.string();

#ifdef _WIN32
    auto r = ChildProcess::run("cmd", {"/c", "cd"}, opts);
#else
    auto r = ChildProcess::run("/bin/pwd", {}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    std::error_code ec;
    auto observed = std::filesystem::weakly_canonical(trim_copy(r.stdout_output), ec);
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::equivalent(observed, dir, ec));
    REQUIRE_FALSE(ec);
}

TEST_CASE("output capture respects max byte limits",
          "[child_process][edge][issue-640]") {
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.max_output_bytes = 3;

#ifdef _WIN32
    auto r = ChildProcess::run("cmd",
        {"/c", "echo abcdef& echo 123456 1>&2"},
        opts);
#else
    auto r = ChildProcess::run("/bin/sh",
        {"-c", "printf abcdef; printf 123456 >&2"}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output == "abc");
    REQUIRE(r.stderr_output == "123");
}

TEST_CASE("stderr line callback fires independently",
          "[child_process][edge][issue-640]") {
    std::vector<std::string> stdout_lines;
    std::vector<std::string> stderr_lines;

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stdout_line = [&](std::string_view line) {
        stdout_lines.emplace_back(line);
    };
    opts.on_stderr_line = [&](std::string_view line) {
        stderr_lines.emplace_back(line);
    };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd",
        {"/c", "echo out1& echo out2& echo err1 1>&2& echo err2 1>&2"},
        opts);
#else
    auto r = ChildProcess::run("/bin/sh",
        {"-c", "printf 'out1\\nout2\\n'; printf 'err1\\nerr2\\n' >&2"},
        opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_lines.size() >= 2);
    REQUIRE(stderr_lines.size() >= 2);
    REQUIRE(stdout_lines[0] == "out1");
    REQUIRE(stdout_lines[1] == "out2");
    REQUIRE(trim_copy(stderr_lines[0]) == "err1");
    REQUIRE(trim_copy(stderr_lines[1]) == "err2");
}

TEST_CASE("line callback buffers partial stdout without trailing newline",
          "[child_process][edge][issue-640]") {
    std::vector<std::string> stdout_lines;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stdout_line = [&](std::string_view line) {
        stdout_lines.emplace_back(line);
    };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd",
        {"/c", "set /p dummy=partial<nul& exit /b 0"},
        opts);
#else
    auto r = ChildProcess::run("/bin/sh", {"-c", "printf partial"}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output == "partial");
    REQUIRE(stdout_lines.empty());
}

TEST_CASE("line callback buffers partial stderr without trailing newline",
          "[child_process][edge][coverage][phase3]") {
    std::vector<std::string> stderr_lines;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stderr_line = [&](std::string_view line) {
        stderr_lines.emplace_back(line);
    };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd",
        {"/c", "<nul set /p dummy=partialerr 1>&2"},
        opts);
#else
    auto r = ChildProcess::run("/bin/sh", {"-c", "printf partialerr >&2"}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stderr_output == "partialerr");
    REQUIRE(stderr_lines.empty());
}

TEST_CASE("move assignment transfers a running child process",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=moved & exit /b 0"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf moved"}));
#endif

    ChildProcess moved;
    moved = std::move(cp);

    auto r = moved.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
#ifdef _WIN32
    REQUIRE(r.stdout_output.find("moved") != std::string::npos);
#else
    REQUIRE(r.stdout_output == "moved");
#endif
}

TEST_CASE("wait is idempotent after process completion",
          "[child_process][edge][issue-640]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=once & exit /b 7"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf once; exit 7"}));
#endif

    auto first = cp.wait();
    auto second = cp.wait();

    REQUIRE(first.exit_code == 7);
    REQUIRE(second.exit_code == first.exit_code);
    REQUIRE(second.stdout_output == first.stdout_output);
    REQUIRE(second.stderr_output == first.stderr_output);
    REQUIRE(second.timed_out == first.timed_out);
    REQUIRE(second.was_cancelled == first.was_cancelled);
    REQUIRE(first.stdout_output.find("once") != std::string::npos);
}

TEST_CASE("timeout preserves output emitted before termination",
          "[child_process][edge][issue-640]") {
    ProcessOptions opts;
    opts.timeout_ms = 500;

#ifdef _WIN32
    auto r = ChildProcess::run(
        "cmd",
        {"/c", "echo before-timeout& ping -n 10 127.0.0.1 >nul"},
        opts);
#else
    auto r = ChildProcess::run(
        "/bin/sh",
        {"-c", "printf before-timeout; sleep 10"},
        opts);
#endif

    REQUIRE(r.timed_out);
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.stdout_output.find("before-timeout") != std::string::npos);
}

TEST_CASE("wait preserves output after is_running observes fast exit",
          "[child_process][edge][issue-640]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=cached & exit /b 0"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf cached"}));
#endif

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (cp.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO("fast-exit child did not finish before the 5s deadline");
    REQUIRE_FALSE(cp.is_running());

    auto r = cp.wait();
    REQUIRE(r.exit_code == 0);
#ifdef _WIN32
    REQUIRE(r.stdout_output.find("cached") != std::string::npos);
#else
    REQUIRE(r.stdout_output == "cached");
#endif
}

#ifndef _WIN32
TEST_CASE("ChildProcess destructor cancels a still-running POSIX child",
          "[child_process][edge][coverage][phase3]") {
    {
        ChildProcess cp;
        REQUIRE(cp.start("sleep", {"10"}));
        REQUIRE(cp.is_running());
    }

    SUCCEED("scope exit returned after cancelling the child");
}

TEST_CASE("ChildProcess move construction transfers a running POSIX child",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess source;
    REQUIRE(source.start("/bin/sh", {"-c", "printf moved"}));

    ChildProcess moved(std::move(source));
    auto result = moved.wait();

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_output == "moved");
}

TEST_CASE("ChildProcess move assignment transfers a running POSIX child",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess source;
    REQUIRE(source.start("/bin/sh", {"-c", "printf assigned; exit 6"}));

    ChildProcess assigned;
    assigned = std::move(source);
    auto result = assigned.wait();

    REQUIRE(result.exit_code == 6);
    REQUIRE(result.stdout_output == "assigned");
}

TEST_CASE("cancel escalates when a POSIX child ignores SIGTERM",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;
    REQUIRE(cp.start(
        "/bin/sh",
        {"-c", "trap '' TERM; printf ready; while :; do sleep 1; done"}));

    std::string observed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observed.find("ready") == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        observed += cp.read_available_output();
        if (observed.find("ready") == std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(observed == "ready");
    REQUIRE(cp.is_running());

    cp.cancel();
    auto result = cp.wait();

    REQUIRE(result.was_cancelled);
    REQUIRE(result.exit_code == -1);
}

TEST_CASE("timeout escalates when a POSIX child ignores SIGTERM",
          "[child_process][edge][coverage][phase3]") {
    ProcessOptions opts;
    opts.timeout_ms = 100;

    auto result = ChildProcess::run(
        "/bin/sh",
        {"-c", "trap '' TERM; printf before-timeout; while :; do sleep 1; done"},
        opts);

    REQUIRE(result.timed_out);
    REQUIRE(result.exit_code == -1);
    REQUIRE(result.stdout_output.find("before-timeout") != std::string::npos);
}
#endif
