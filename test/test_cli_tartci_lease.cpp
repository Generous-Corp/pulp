#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cli_common.hpp"
#include "tools/cli/tartci_lease.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <thread>

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

TEST_CASE("tier-0 default is core-bound when memory is plentiful") {
    // 16 GiB budget / 1.5 GiB per job = 10 memory-jobs, so 8 cores is the bind.
    REQUIRE(tier0_default_build_jobs(8, 16ULL * 1024 * 1024 * 1024) == 8);
}

TEST_CASE("tier-0 default is memory-bound on a constrained host") {
    // 8 cores but only 6 GiB of budget → 6/1.5 = 4 jobs is the bind.
    REQUIRE(tier0_default_build_jobs(8, 6ULL * 1024 * 1024 * 1024) == 4);
}

TEST_CASE("tier-0 default never drops below one job") {
    REQUIRE(tier0_default_build_jobs(0, 0) == 1);
    REQUIRE(tier0_default_build_jobs(4, 256ULL * 1024 * 1024) == 1);  // <1.5 GiB budget
}

TEST_CASE("tier-0 default falls back to cores when RAM is unknown") {
    // A zero budget means "could not read RAM" — bound by cores alone, not by 1.
    REQUIRE(tier0_default_build_jobs(6, 0) == 6);
}

TEST_CASE("tier-0 memory-budget override is not clamped to a job ceiling") {
    // A 3 GiB budget is 2 compile jobs (3 / 1.5). If the MB value were routed
    // through the job-count parser (ceiling 1024), the budget would collapse to
    // 1 GiB → <1.5 GiB/job → exactly 1 job. So >1 proves the clamp is gone.
    ScopedEnvVar no_jobs("PULP_BUILD_JOBS", std::nullopt);
    ScopedEnvVar budget("PULP_BUILD_MEM_BUDGET_MB", std::string{"3072"});
    const unsigned hw = std::thread::hardware_concurrency();
    const int jobs = tier0_default_build_jobs();
    REQUIRE(jobs == (hw >= 2 ? 2 : 1));
}

TEST_CASE("no-lease build acquisition falls back to a bounded tier-0 cap") {
    // With leases disabled there is no host store, so acquire() takes the same
    // tier-0 fallback expression the "no tartci installed" branch uses:
    // jobs = env_jobs>0 ? env_jobs : tier0_default_build_jobs(). The build is
    // therefore always bounded — never the old 0/unbounded no-op.
    ScopedEnvVar no_jobs("PULP_BUILD_JOBS", std::nullopt);
    ScopedEnvVar leases_off("PULP_TARTCI_LEASES", std::string{"0"});

    auto lease = TartciAgentBuildLease::acquire({fs::current_path(), "pulp-build", false});
    REQUIRE(lease.ok());
    REQUIRE_FALSE(lease.active());   // no store → not a real lease…
    REQUIRE(lease.jobs() >= 1);      // …but still a bounded cap, never 0/unbounded.
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

TEST_CASE("build acquisition degrades to a bounded cap when host-profile fails") {
    // A tartci that cannot answer `host-profile` (an older/incompatible deploy
    // without the subcommand) must NOT fail the build — it degrades to the
    // bounded tier-0 default. This is the on-pool reality that would otherwise
    // make `pulp build` exit non-zero.
    auto root = fs::temp_directory_path()
        / ("pulp-tartci-lease-hp-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto script = root / "tartci";
    {
        std::ofstream out(script);
        out << "#!/usr/bin/env bash\n"
               "echo \"unknown command: ${1:-}\" >&2\n"
               "exit 2\n";
    }
    fs::permissions(script,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    ScopedEnvVar fake_bin("PULP_TARTCI_BIN", script.string());
    ScopedEnvVar leases_enabled("PULP_TARTCI_LEASES", std::string{"1"});
    ScopedEnvVar no_held("PULP_TARTCI_LEASE_HELD", std::nullopt);
    ScopedEnvVar no_user_cap("PULP_BUILD_JOBS", std::nullopt);

    auto lease = TartciAgentBuildLease::acquire({root, "pulp-build", false});
    REQUIRE(lease.ok());            // must not fail the build
    REQUIRE_FALSE(lease.active());  // no usable lease store
    REQUIRE(lease.jobs() >= 1);     // …but a bounded tier-0 fallback

    fs::remove_all(root);
}
#endif

TEST_CASE("governance tier defaults to Tier 0 when no tartci store is present") {
    // Leases explicitly off (or no reachable host store) → always the built-in
    // bounded local builds. Orchard must be absent so Tier 2 does not preempt.
    ScopedEnvVar no_orchard("TARTCI_ORCHARD_URL", std::nullopt);
    ScopedEnvVar leases_off("PULP_TARTCI_LEASES", std::string{"0"});
    ScopedEnvVar no_bin("PULP_TARTCI_BIN", std::nullopt);

    const auto g = detect_build_governance();
    REQUIRE(g.tier == 0);
    REQUIRE(g.detail == "bounded local builds");
}

TEST_CASE("governance tier is Tier 0 when tartci bin points at a missing path") {
    // A PULP_TARTCI_BIN that does not resolve must degrade, never throw: the
    // host-profile exec fails and we fall back to Tier 0.
    ScopedEnvVar no_orchard("TARTCI_ORCHARD_URL", std::nullopt);
    ScopedEnvVar leases_on("PULP_TARTCI_LEASES", std::string{"1"});
    ScopedEnvVar bad_bin("PULP_TARTCI_BIN",
                         std::string{"/nonexistent/pulp-tartci-for-governance-test"});

    const auto g = detect_build_governance();
    REQUIRE(g.tier == 0);
    REQUIRE(g.detail == "bounded local builds");
}

TEST_CASE("governance tier is Tier 2 when an orchard fleet is configured") {
    ScopedEnvVar orchard("TARTCI_ORCHARD_URL", std::string{"https://orchard.example:8443"});
    // Orchard preempts Tier 1 detection even if leases are enabled.
    ScopedEnvVar leases_on("PULP_TARTCI_LEASES", std::string{"1"});
    ScopedEnvVar no_bin("PULP_TARTCI_BIN", std::nullopt);

    const auto g = detect_build_governance();
    REQUIRE(g.tier == 2);
    REQUIRE(g.detail == "orchard fleet");
}

#ifndef _WIN32
TEST_CASE("governance tier is Tier 1 when a tartci host-profile succeeds") {
    auto root = fs::temp_directory_path()
        / ("pulp-tartci-gov-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto script = root / "tartci";
    {
        std::ofstream out(script);
        out << "#!/usr/bin/env bash\n"
               "if [ \"${1:-}\" = \"host-profile\" ]; then\n"
               "  printf 'PULP_BUILD_JOBS=12\\n'\n"
               "  printf 'PULP_BUILD_MEM_BUDGET_MB=81920\\n'\n"
               "  exit 0\n"
               "fi\n"
               "echo \"unexpected command: ${1:-}\" >&2\n"
               "exit 2\n";
    }
    fs::permissions(script,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    ScopedEnvVar no_orchard("TARTCI_ORCHARD_URL", std::nullopt);
    ScopedEnvVar leases_on("PULP_TARTCI_LEASES", std::string{"1"});
    ScopedEnvVar fake_bin("PULP_TARTCI_BIN", script.string());

    const auto g = detect_build_governance();
    REQUIRE(g.tier == 1);
    REQUIRE(g.jobs == 12);
    REQUIRE(g.mem_budget_mb == 81920);
    REQUIRE(g.detail.find("tartci host lease") != std::string::npos);
    REQUIRE(g.detail.find("12 jobs") != std::string::npos);
    REQUIRE(g.detail.find("80 GB budget") != std::string::npos);

    fs::remove_all(root);
}
#endif
