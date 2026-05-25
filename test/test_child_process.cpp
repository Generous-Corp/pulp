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

TEST_CASE("process_id is available and stable after wait",
          "[child_process][process-id]") {
    ChildProcess cp;
#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "echo pid"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "echo pid"}));
#endif

    const int pid = cp.process_id();
    REQUIRE(pid > 0);

    auto r = cp.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE(cp.process_id() == pid);
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

TEST_CASE("disabled stream capture discards stdout stderr and callbacks",
          "[child_process][capture]") {
    int stdout_lines = 0;
    int stderr_lines = 0;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    opts.on_stdout_line = [&](std::string_view) { ++stdout_lines; };
    opts.on_stderr_line = [&](std::string_view) { ++stderr_lines; };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd", {"/c", "echo hidden& echo hidden 1>&2"}, opts);
#else
    auto r = ChildProcess::run("/bin/sh",
        {"-c", "printf hidden; printf hidden >&2"}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.empty());
    REQUIRE(r.stderr_output.empty());
    REQUIRE(stdout_lines == 0);
    REQUIRE(stderr_lines == 0);
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

TEST_CASE("read_available_output is empty when stdout capture is disabled",
          "[child_process][capture]") {
    ChildProcess cp;
    ProcessOptions opts;
    opts.capture_stdout = false;

#ifdef _WIN32
    REQUIRE(cp.start("cmd",
                     {"/c", "<nul set /p dummy=hidden& ping -n 2 127.0.0.1 >nul"},
                     opts));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf hidden; sleep 1"}, opts));
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(cp.read_available_output().empty());

    auto r = cp.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.empty());
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

TEST_CASE("zero max output bytes drains without capturing lines",
          "[child_process][edge][coverage][phase3]") {
    std::vector<std::string> stdout_lines;
    std::vector<std::string> stderr_lines;

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.max_output_bytes = 0;
    opts.on_stdout_line = [&](std::string_view line) {
        stdout_lines.emplace_back(line);
    };
    opts.on_stderr_line = [&](std::string_view line) {
        stderr_lines.emplace_back(line);
    };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd",
        {"/c", "echo out-line& echo err-line 1>&2"},
        opts);
#else
    auto r = ChildProcess::run("/bin/sh",
        {"-c", "printf 'out-line\\n'; printf 'err-line\\n' >&2"},
        opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.empty());
    REQUIRE(r.stderr_output.empty());
    REQUIRE(stdout_lines.empty());
    REQUIRE(stderr_lines.empty());
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

TEST_CASE("move constructor transfers a running child process",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=move-ctor & exit /b 0"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf move-ctor"}));
#endif

    ChildProcess moved(std::move(cp));

    auto r = moved.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
#ifdef _WIN32
    REQUIRE(r.stdout_output.find("move-ctor") != std::string::npos);
#else
    REQUIRE(r.stdout_output == "move-ctor");
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

TEST_CASE("start after observed completion waits previous child without cancellation state",
          "[child_process][edge][process-reuse]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=first"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf first"}));
#endif

    for (int i = 0; i < 1000 && cp.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(cp.is_running());

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "<nul set /p dummy=second"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "printf second"}));
#endif

    auto replacement = cp.wait();
    REQUIRE(replacement.exit_code == 0);
    REQUIRE_FALSE(replacement.timed_out);
    REQUIRE_FALSE(replacement.was_cancelled);
    REQUIRE(replacement.stdout_output.find("second") != std::string::npos);
}

TEST_CASE("cancel after natural exit reports completed process result",
          "[child_process][edge][cancel]") {
    ChildProcess cp;

#ifdef _WIN32
    REQUIRE(cp.start("cmd", {"/c", "exit /b 0"}));
#else
    REQUIRE(cp.start("/bin/sh", {"-c", "exit 0"}));
#endif

    for (int i = 0; i < 1000 && cp.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(cp.is_running());

    cp.cancel();
    auto result = cp.wait();
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(result.was_cancelled);
    REQUIRE_FALSE(result.timed_out);
}

#ifndef _WIN32
TEST_CASE("process reuse clears timeout state from prior run",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

    ProcessOptions timeout_opts;
    timeout_opts.timeout_ms = 50;
    REQUIRE(cp.start("sleep", {"10"}, timeout_opts));
    auto timed_out = cp.wait();
    REQUIRE(timed_out.timed_out);
    REQUIRE(timed_out.exit_code == -1);

    ProcessOptions fast_opts;
    fast_opts.timeout_ms = 5000;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf after-timeout"}, fast_opts));
    auto reused = cp.wait();
    REQUIRE(reused.exit_code == 0);
    REQUIRE_FALSE(reused.timed_out);
    REQUIRE_FALSE(reused.was_cancelled);
    REQUIRE(reused.stdout_output.find("after-timeout") != std::string::npos);
    REQUIRE(reused.stderr_output.empty());
}

TEST_CASE("process reuse clears cancellation state from prior run",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

    REQUIRE(cp.start("sleep", {"30"}));
    REQUIRE(cp.is_running());
    cp.cancel();
    auto cancelled = cp.wait();
    REQUIRE(cancelled.was_cancelled);

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf after-cancel"}, opts));
    auto reused = cp.wait();
    REQUIRE(reused.exit_code == 0);
    REQUIRE_FALSE(reused.timed_out);
    REQUIRE_FALSE(reused.was_cancelled);
    REQUIRE(reused.stdout_output.find("after-cancel") != std::string::npos);
}

TEST_CASE("start cancels a running child before launching replacement",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

    REQUIRE(cp.start("sleep", {"30"}));
    REQUIRE(cp.is_running());

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf replacement"}, opts));

    auto replacement = cp.wait();
    REQUIRE(replacement.exit_code == 0);
    REQUIRE_FALSE(replacement.timed_out);
    REQUIRE_FALSE(replacement.was_cancelled);
    REQUIRE(replacement.stdout_output == "replacement");
}

TEST_CASE("failed restart after completion does not retain stale child state",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

    REQUIRE(cp.start("/bin/sh", {"-c", "printf before-failure"}));
    auto first = cp.wait();
    REQUIRE(first.exit_code == 0);
    REQUIRE(first.stdout_output.find("before-failure") != std::string::npos);

    ProcessOptions opts;
    opts.timeout_ms = 250;
    REQUIRE_FALSE(cp.start("pulp-child-process-missing-restart-binary-xyz", {}, opts));
    REQUIRE_FALSE(cp.is_running());
    REQUIRE(cp.read_available_output().empty());

    auto failed = cp.wait();
    REQUIRE(failed.exit_code == -1);
    REQUIRE(failed.stdout_output.empty());
    REQUIRE(failed.stderr_output.empty());
    REQUIRE_FALSE(failed.timed_out);
    REQUIRE_FALSE(failed.was_cancelled);
}

TEST_CASE("argv arguments preserve empty strings spaces and punctuation",
          "[child_process][edge][coverage][phase3]") {
    ProcessOptions opts;
    opts.timeout_ms = 5000;

    auto r = ChildProcess::run(
        "/usr/bin/printf",
        {"%s|%s|%s", "", "two words", "semi;quote\"slash\\"},
        opts);

    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
    REQUIRE(r.stdout_output == "|two words|semi;quote\"slash\\");
    REQUIRE(r.stderr_output.empty());
}

TEST_CASE("line callback emits empty lines and preserves order",
          "[child_process][edge][coverage][phase3]") {
    std::vector<std::string> lines;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stdout_line = [&](std::string_view line) {
        lines.emplace_back(line);
    };

    auto r = ChildProcess::run("/bin/sh", {"-c", "printf 'first\\n\\nthird\\n'"}, opts);

    REQUIRE(r.exit_code == 0);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0] == "first");
    REQUIRE(lines[1].empty());
    REQUIRE(lines[2] == "third");
}

TEST_CASE("line callback joins a line split across drain passes",
          "[child_process][edge][coverage][phase3]") {
    std::vector<std::string> lines;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stdout_line = [&](std::string_view line) {
        lines.emplace_back(line);
    };

    auto r = ChildProcess::run(
        "/bin/sh",
        {"-c", "printf left; sleep 0.05; printf 'right\\n'"},
        opts);

    REQUIRE(r.exit_code == 0);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "leftright");
    REQUIRE(r.stdout_output.find("leftright") != std::string::npos);
}

TEST_CASE("line callback honors output cap before later complete lines",
          "[child_process][edge][coverage][phase3]") {
    std::vector<std::string> lines;
    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.max_output_bytes = 4;
    opts.on_stdout_line = [&](std::string_view line) {
        lines.emplace_back(line);
    };

    auto r = ChildProcess::run("/bin/sh", {"-c", "printf 'ab\\ncd\\n'"}, opts);

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.size() == 4);
    REQUIRE(r.stdout_output.substr(0, 3) == "ab\n");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "ab");
}

TEST_CASE("read_available_output drains stdout without losing stderr result",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    REQUIRE(cp.start(
        "/bin/sh",
        {"-c", "printf early; sleep 0.05; printf 'late-error\\n' >&2"},
        opts));

    std::string drained;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (drained.find("early") == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        drained += cp.read_available_output();
        if (drained.find("early") == std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    REQUIRE(drained == "early");

    auto r = cp.wait();
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.empty());
    REQUIRE(r.stderr_output.find("late-error") != std::string::npos);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE_FALSE(r.was_cancelled);
}
#endif

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

TEST_CASE("starting a POSIX replacement cancels a still-running child",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;
    REQUIRE(cp.start(
        "/bin/sh",
        {"-c", "trap '' TERM; printf old-ready; while :; do sleep 1; done"}));

    std::string observed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observed.find("old-ready") == std::string::npos
           && std::chrono::steady_clock::now() < deadline) {
        observed += cp.read_available_output();
        if (observed.find("old-ready") == std::string::npos)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(observed == "old-ready");
    REQUIRE(cp.is_running());

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf new-ready; exit 4"}, opts));

    auto result = cp.wait();
    REQUIRE(result.exit_code == 4);
    REQUIRE_FALSE(result.was_cancelled);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.stdout_output == "new-ready");
}

TEST_CASE("starting a POSIX replacement drains a previously observed exit",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf first; exit 3"}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (cp.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(cp.is_running());

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf second; exit 5"}, opts));

    auto result = cp.wait();
    REQUIRE(result.exit_code == 5);
    REQUIRE_FALSE(result.was_cancelled);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.stdout_output == "second");
}

TEST_CASE("POSIX wait consumes cached fast-exit status after polling",
          "[child_process][edge][coverage][phase3]") {
    ChildProcess cp;
    REQUIRE(cp.start("/bin/sh", {"-c", "printf cached-status; exit 9"}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (cp.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(cp.is_running());

    auto result = cp.wait();
    REQUIRE(result.exit_code == 9);
    REQUIRE_FALSE(result.was_cancelled);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.stdout_output == "cached-status");
}
#endif
