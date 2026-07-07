#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cli_common.hpp"
#include "tools/cli/tartci_lease.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, std::optional<std::string> value)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            had_value_ = true;
            old_value_ = existing;
        }
        if (value) {
#ifdef _WIN32
            _putenv_s(name_.c_str(), value->c_str());
#else
            setenv(name_.c_str(), value->c_str(), 1);
#endif
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), had_value_ ? old_value_.c_str() : "");
#else
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string env_or_empty(const char* name) {
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return {};
}

}  // namespace

TEST_CASE("tartci host-profile shell exports are parsed") {
    const std::string text =
        "TARTCI_ROLE=dev-overflow\n"
        "PULP_BUILD_JOBS=6\n"
        "TARTCI_AGENT_QOS=background\n";

    REQUIRE(parse_shell_assignment_int(text, "PULP_BUILD_JOBS") == 6);
    REQUIRE(parse_shell_assignment(text, "TARTCI_AGENT_QOS") == "background");
    REQUIRE(parse_shell_assignment(text, "MISSING").empty());
}

TEST_CASE("cmake build parallel args are injected when absent") {
    auto plan = cap_cmake_build_parallel_args({"--target", "pulp-cli"}, 6);

    REQUIRE(plan.jobs == 6);
    REQUIRE(plan.args == std::vector<std::string>{"--target", "pulp-cli", "--parallel", "6"});
}

TEST_CASE("cmake build parallel args are capped when user asks for more") {
    auto plan = cap_cmake_build_parallel_args({"--parallel", "24", "--target", "pulp-cli"}, 6);

    REQUIRE(plan.jobs == 6);
    REQUIRE(plan.args == std::vector<std::string>{"--target", "pulp-cli", "--parallel", "6"});
}

TEST_CASE("cmake build parallel args preserve lower explicit user cap") {
    auto plan = cap_cmake_build_parallel_args({"-j2"}, 6);

    REQUIRE(plan.jobs == 2);
    REQUIRE(plan.args == std::vector<std::string>{"--parallel", "2"});
}

TEST_CASE("cmake build parallel parser handles compact forms") {
    auto dash_j = cap_cmake_build_parallel_args({"-j12"}, 8);
    REQUIRE(dash_j.jobs == 8);
    REQUIRE(dash_j.args == std::vector<std::string>{"--parallel", "8"});

    auto equals = cap_cmake_build_parallel_args({"--parallel=3"}, 8);
    REQUIRE(equals.jobs == 3);
    REQUIRE(equals.args == std::vector<std::string>{"--parallel", "3"});

    auto bare = cap_cmake_build_parallel_args({"--parallel", "--target", "pulp-cli"}, 8);
    REQUIRE(bare.jobs == 8);
    REQUIRE(bare.args == std::vector<std::string>{"--target", "pulp-cli", "--parallel", "8"});
}

TEST_CASE("watch loops use one fixed host lease id") {
    TartciAgentLeaseRequest a{"/tmp/project-a", "pulp-dev", true};
    TartciAgentLeaseRequest b{"/tmp/project-b", "pulp-loop", true};
    TartciAgentLeaseRequest c{"/tmp/project-a", "pulp-build", false};

    REQUIRE(tartci_agent_lease_id(a) == "pulp-watch-loop");
    REQUIRE(tartci_agent_lease_id(b) == "pulp-watch-loop");
    REQUIRE(tartci_agent_lease_id(c) != "pulp-watch-loop");
}

TEST_CASE("non-positive caps leave cmake build args unchanged") {
    std::vector<std::string> args{"--target", "pulp-cli"};
    auto plan = cap_cmake_build_parallel_args(args, 0);

    REQUIRE(plan.jobs == 0);
    REQUIRE(plan.args == args);
}

TEST_CASE("scoped build parallel env injects build and test caps") {
    ScopedEnvVar no_pulp_jobs("PULP_BUILD_JOBS", std::nullopt);
    ScopedEnvVar no_cmake_jobs("CMAKE_BUILD_PARALLEL_LEVEL", std::nullopt);
    ScopedEnvVar no_ctest_jobs("CTEST_PARALLEL_LEVEL", std::nullopt);
    ScopedEnvVar no_nested("PULP_TARTCI_LEASE_HELD", std::nullopt);

    {
        ScopedBuildParallelEnv env(5, true);
        REQUIRE(env_or_empty("PULP_BUILD_JOBS") == "5");
        REQUIRE(env_or_empty("CMAKE_BUILD_PARALLEL_LEVEL") == "5");
        REQUIRE(env_or_empty("CTEST_PARALLEL_LEVEL") == "5");
        REQUIRE(env_or_empty("PULP_TARTCI_LEASE_HELD") == "1");
    }

    REQUIRE(env_or_empty("PULP_BUILD_JOBS").empty());
    REQUIRE(env_or_empty("CMAKE_BUILD_PARALLEL_LEVEL").empty());
    REQUIRE(env_or_empty("CTEST_PARALLEL_LEVEL").empty());
    REQUIRE(env_or_empty("PULP_TARTCI_LEASE_HELD").empty());
}

TEST_CASE("background qos wraps build command on macOS only") {
    ScopedEnvVar taskpolicy_enabled("PULP_TARTCI_TASKPOLICY", std::nullopt);
#ifdef __APPLE__
    REQUIRE(apply_agent_build_qos("cmake --build build", "background")
            == "taskpolicy -b cmake --build build");

    ScopedEnvVar taskpolicy_disabled("PULP_TARTCI_TASKPOLICY", "0");
    REQUIRE(apply_agent_build_qos("cmake --build build", "background")
            == "cmake --build build");
#else
    REQUIRE(apply_agent_build_qos("cmake --build build", "background")
            == "cmake --build build");
#endif
    REQUIRE(apply_agent_build_qos("cmake --build build", "normal")
            == "cmake --build build");
}

TEST_CASE("build watchdog wraps only active leased commands") {
    ScopedEnvVar watchdog_default("PULP_TARTCI_WATCHDOG", std::nullopt);

#ifdef _WIN32
    REQUIRE(apply_agent_build_watchdog("cmake --build build", 4, true)
            == "cmake --build build");
#else
    REQUIRE(apply_agent_build_watchdog("cmake --build build", 4, false)
            == "cmake --build build");
    REQUIRE(apply_agent_build_watchdog("cmake --build build", 0, true)
            == "cmake --build build");

    const auto wrapped = apply_agent_build_watchdog("cmake --build build", 4, true);
    REQUIRE(wrapped.find("python3") != std::string::npos);
    REQUIRE(wrapped.find(" -c ") != std::string::npos);
    REQUIRE(wrapped.find("cmake --build build") != std::string::npos);
    REQUIRE(wrapped.find(" kill") != std::string::npos);
#endif
}

TEST_CASE("build watchdog exposes kill switch and monitor-only mode") {
#ifdef _WIN32
    SUCCEED("watchdog wrapper is POSIX-only");
#else
    ScopedEnvVar watchdog_off("PULP_TARTCI_WATCHDOG", "0");
    REQUIRE(apply_agent_build_watchdog("cmake --build build", 4, true)
            == "cmake --build build");

    ScopedEnvVar watchdog_monitor("PULP_TARTCI_WATCHDOG", "monitor");
    const auto wrapped = apply_agent_build_watchdog("cmake --build build", 4, true);
    REQUIRE(wrapped.find("python3") != std::string::npos);
    REQUIRE(wrapped.find(" -c ") != std::string::npos);
    REQUIRE(wrapped.find("monitor") != std::string::npos);
#endif
}

#ifndef _WIN32
TEST_CASE("build watchdog fails open when python is unavailable") {
    ScopedEnvVar watchdog_default("PULP_TARTCI_WATCHDOG", std::nullopt);
    ScopedEnvVar watchdog_python("PULP_TARTCI_WATCHDOG_PYTHON",
                                 "pulp-missing-python-for-watchdog-test");

    REQUIRE(apply_agent_build_watchdog("cmake --build build", 4, true)
            == "cmake --build build");
}

TEST_CASE("build watchdog preserves child exit code in monitor mode") {
    ScopedEnvVar watchdog_monitor("PULP_TARTCI_WATCHDOG", "monitor");
    ScopedEnvVar interval("PULP_TARTCI_WATCHDOG_INTERVAL_SECS", "1");

    const auto wrapped = apply_agent_build_watchdog("sh -c 'exit 7'", 1, true);
    REQUIRE(run(wrapped) == 7);
}

TEST_CASE("build watchdog kills sustained CPU over budget") {
    ScopedEnvVar watchdog_kill("PULP_TARTCI_WATCHDOG", "kill");
    ScopedEnvVar interval("PULP_TARTCI_WATCHDOG_INTERVAL_SECS", "1");
    ScopedEnvVar samples("PULP_TARTCI_WATCHDOG_SAMPLES", "1");
    ScopedEnvVar grace("PULP_TARTCI_WATCHDOG_TERM_GRACE_SECS", "1");
    ScopedEnvVar cpu_per_job("PULP_TARTCI_WATCHDOG_CPU_PER_JOB", "1");

    const auto wrapped = apply_agent_build_watchdog("python3 -c 'while True: pass'", 1, true);
    REQUIRE(run(wrapped) == 124);
}
#endif

#ifndef _WIN32
TEST_CASE("concurrent watch loops are rejected by one fixed host lease") {
    auto root = fs::temp_directory_path()
        / ("pulp-tartci-lease-test-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto script = root / "tartci";
    const auto store = root / "leases.txt";
    const auto log = root / "calls.log";

    {
        std::ofstream out(script);
        out << R"SH(#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "${FAKE_TARTCI_LOG:?}"
if [ "${1:-}" = "host-profile" ]; then
  printf 'PULP_BUILD_JOBS=6\n'
  printf 'TARTCI_AGENT_QOS=background\n'
  exit 0
fi
if [ "${1:-}" != "leases" ]; then
  echo "unexpected command: $*" >&2
  exit 2
fi
sub="${2:-}"
shift 2
id=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --id) id="${2:-}"; shift 2 ;;
    *) shift ;;
  esac
done
case "$sub" in
  acquire)
    touch "${FAKE_TARTCI_STORE:?}"
    if grep -qx "$id" "${FAKE_TARTCI_STORE:?}"; then
      echo '{"ok":false,"reason":"duplicate_lease_id"}'
      exit 73
    fi
    printf '%s\n' "$id" >> "${FAKE_TARTCI_STORE:?}"
    echo '{"ok":true}'
    ;;
  release)
    if [ -f "${FAKE_TARTCI_STORE:?}" ]; then
      grep -vx "$id" "${FAKE_TARTCI_STORE:?}" > "${FAKE_TARTCI_STORE:?}.tmp" || true
      mv "${FAKE_TARTCI_STORE:?}.tmp" "${FAKE_TARTCI_STORE:?}"
    fi
    echo '{"ok":true}'
    ;;
  heartbeat)
    echo '{"ok":true}'
    ;;
  *)
    echo "unexpected leases subcommand: $sub" >&2
    exit 2
    ;;
esac
)SH";
    }
    fs::permissions(script,
                    fs::perms::owner_read | fs::perms::owner_write
                        | fs::perms::owner_exec,
                    fs::perm_options::replace);

    ScopedEnvVar fake_bin("PULP_TARTCI_BIN", script.string());
    ScopedEnvVar fake_store("FAKE_TARTCI_STORE", store.string());
    ScopedEnvVar fake_log("FAKE_TARTCI_LOG", log.string());
    ScopedEnvVar leases_enabled("PULP_TARTCI_LEASES", "1");
    ScopedEnvVar no_nested("PULP_TARTCI_LEASE_HELD", std::nullopt);
    ScopedEnvVar no_user_cap("PULP_BUILD_JOBS", std::nullopt);

    {
        TartciAgentLeaseRequest req{root, "pulp-dev", true};
        auto first = TartciAgentBuildLease::acquire(req);
        REQUIRE(first.ok());
        REQUIRE(first.active());
        REQUIRE(first.jobs() == 6);
        REQUIRE(first.qos() == "background");

        auto second = TartciAgentBuildLease::acquire(req);
        REQUIRE_FALSE(second.ok());
        REQUIRE(second.exit_code() == 73);
        REQUIRE(second.error().find("duplicate_lease_id") != std::string::npos);
    }

    const auto calls = read_file(log);
    REQUIRE(calls.find("leases acquire --id pulp-watch-loop --cores 6 --priority build --kind pulp-dev")
            != std::string::npos);
    REQUIRE(calls.find("leases release --id pulp-watch-loop") != std::string::npos);
    REQUIRE(read_file(store).find("pulp-watch-loop") == std::string::npos);

    fs::remove_all(root);
}
#endif
