#include "tartci_lease.hpp"

#include "cli_common.hpp"
#include "shell_redirect.hpp"

#include <pulp/runtime/system.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define popen _popen
#define pclose _pclose
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

CommandResult capture_command(const std::string& command) {
    CommandResult result;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        result.exit_code = 127;
        result.output = "failed to start command";
        return result;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result.output += buffer;
    }
    result.exit_code = decode_system_status(pclose(pipe));
    return result;
}

std::string env_value(const char* name) {
    if (const char* value = std::getenv(name); value && *value) {
        return value;
    }
    return {};
}

bool env_false(const char* name) {
    auto value = env_value(name);
    return value == "0" || value == "false" || value == "FALSE" || value == "off";
}

int parse_positive_int(const std::string& text) {
    if (text.empty()) return 0;
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size() || value <= 0) return 0;
    return static_cast<int>(std::min<long>(value, 1024));
}

int env_positive_int(const char* name) {
    return parse_positive_int(env_value(name));
}

std::string current_owner() {
    auto user = env_value("USER");
    if (user.empty()) user = env_value("LOGNAME");
    if (user.empty()) user = "unknown";
    return user;
}

std::string lease_label(const fs::path& root) {
    auto name = root.filename().string();
    return name.empty() ? root.string() : name;
}

std::string command_kind(const TartciAgentLeaseRequest& req) {
    if (!req.command_kind.empty()) return req.command_kind;
    return req.watch_loop ? "pulp-watch" : "pulp-build";
}

void restore_env(const ScopedBuildParallelEnv::SavedEnv& saved) {
#ifdef _WIN32
    _putenv_s(saved.name.c_str(), saved.had_value ? saved.value.c_str() : "");
#else
    if (saved.had_value) {
        setenv(saved.name.c_str(), saved.value.c_str(), 1);
    } else {
        unsetenv(saved.name.c_str());
    }
#endif
}

void set_env_value(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

ScopedBuildParallelEnv::SavedEnv save_env(const std::string& name) {
    ScopedBuildParallelEnv::SavedEnv saved;
    saved.name = name;
    if (const char* value = std::getenv(name.c_str())) {
        saved.had_value = true;
        saved.value = value;
    }
    return saved;
}

std::string trim_line(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return trim(value);
}

}  // namespace

std::string parse_shell_assignment(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(in, line)) {
        line = trim_line(line);
        if (line.rfind(prefix, 0) != 0) continue;
        auto value = trim(line.substr(prefix.size()));
        if (value.size() >= 2) {
            const char q = value.front();
            if ((q == '"' || q == '\'') && value.back() == q) {
                value = value.substr(1, value.size() - 2);
            }
        }
        return value;
    }
    return {};
}

int parse_shell_assignment_int(const std::string& text, const std::string& key) {
    return parse_positive_int(parse_shell_assignment(text, key));
}

int tier0_default_build_jobs(unsigned hw_threads, unsigned long long mem_budget_bytes) {
    int cores = hw_threads > 0 ? static_cast<int>(hw_threads) : 1;
    // Budget ~1.5 GiB of resident memory per concurrent C++ compile job. This is
    // a deliberately conservative compile-average estimate whose only job is to
    // keep a wide `--parallel` from exhausting RAM on a memory-constrained host;
    // when a tartci lease is present its per-host budget supersedes this.
    const unsigned long long per_job = 1536ULL * 1024ULL * 1024ULL;
    int mem_jobs = cores;
    if (mem_budget_bytes > 0) {
        mem_jobs = static_cast<int>(mem_budget_bytes / per_job);
    }
    if (mem_jobs < 1) mem_jobs = 1;
    int jobs = std::min(cores, mem_jobs);
    return jobs < 1 ? 1 : jobs;
}

int tier0_default_build_jobs() {
    const unsigned threads = std::thread::hardware_concurrency();
    unsigned long long budget = 0;
    // Explicit override wins (deterministic tests + a user escape hatch). Parse
    // it directly rather than via env_positive_int(), which clamps to a
    // job-count ceiling that would silently truncate a megabyte budget.
    if (const char* raw = std::getenv("PULP_BUILD_MEM_BUDGET_MB")) {
        char* end = nullptr;
        const long long mb = std::strtoll(raw, &end, 10);
        if (end != raw && mb > 0) {
            budget = static_cast<unsigned long long>(mb) * 1024ULL * 1024ULL;
        }
    }
    if (budget == 0) {
        // pulp::runtime already implements per-platform physical-RAM detection;
        // reuse it rather than re-deriving hw.memsize / sysconf here.
        const uint64_t total_mb = pulp::runtime::total_memory_mb();
        if (total_mb > 0) {
            // Reserve ~25% for the OS and window server so a wide build cannot
            // starve the UI / remote-desktop path on a shared desktop machine.
            budget = static_cast<unsigned long long>(total_mb) * 1024ULL * 1024ULL / 4ULL * 3ULL;
        }
    }
    return tier0_default_build_jobs(threads, budget);
}

CmakeParallelPlan cap_cmake_build_parallel_args(const std::vector<std::string>& args,
                                                int max_jobs) {
    CmakeParallelPlan plan;
    if (max_jobs <= 0) {
        plan.args = args;
        return plan;
    }

    int explicit_jobs = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--parallel" || arg == "-j") {
            if (i + 1 < args.size()) {
                const int parsed = parse_positive_int(args[i + 1]);
                if (parsed > 0) {
                    explicit_jobs = parsed;
                    ++i;
                }
            }
            continue;
        }
        if (arg.rfind("--parallel=", 0) == 0) {
            explicit_jobs = parse_positive_int(arg.substr(11));
            continue;
        }
        if (arg.size() > 2 && arg.rfind("-j", 0) == 0) {
            explicit_jobs = parse_positive_int(arg.substr(2));
            if (explicit_jobs > 0) continue;
        }
        plan.args.push_back(arg);
    }

    plan.jobs = explicit_jobs > 0 ? std::min(explicit_jobs, max_jobs) : max_jobs;
    plan.args.push_back("--parallel");
    plan.args.push_back(std::to_string(plan.jobs));
    return plan;
}

std::string tartci_agent_lease_id(const TartciAgentLeaseRequest& req) {
    if (req.watch_loop) {
        return "pulp-watch-loop";
    }
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return "pulp-agent-build-" + std::to_string(getpid()) + "-" + std::to_string(tick);
}

std::string apply_agent_build_qos(const std::string& command, const std::string& qos) {
#ifdef __APPLE__
    if (qos == "background" && !env_false("PULP_TARTCI_TASKPOLICY")) {
        return "taskpolicy -b " + command;
    }
#else
    (void)qos;
#endif
    return command;
}

static std::string tartci_watchdog_mode() {
    auto mode = env_value("PULP_TARTCI_WATCHDOG");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode.empty()) return "kill";
    if (mode == "0" || mode == "false" || mode == "off" || mode == "no") return {};
    if (mode == "monitor") return "monitor";
    if (mode == "kill") return "kill";
    return "kill";
}

static std::string tartci_watchdog_python_command() {
    auto python = env_value("PULP_TARTCI_WATCHDOG_PYTHON");
    if (python.empty()) python = "python3";

    if (python.find('/') == std::string::npos) {
        return find_executable_in_path(python).empty() ? std::string{} : python;
    }
    return fs::exists(python) ? python : std::string{};
}

std::string apply_agent_build_watchdog(const std::string& command,
                                       int jobs,
                                       bool lease_active) {
#ifdef _WIN32
    (void)jobs;
    (void)lease_active;
    return command;
#else
    if (!lease_active || jobs <= 0) return command;
    const auto mode = tartci_watchdog_mode();
    if (mode.empty()) return command;
    const auto python = tartci_watchdog_python_command();
    if (python.empty()) return command;

    static const char* watchdog_py = R"PY(
import os
import signal
import subprocess
import sys
import time

cmd = sys.argv[1]
jobs = max(1, int(sys.argv[2]))
mode = sys.argv[3]

def env_float(name, default):
    try:
        value = float(os.environ.get(name, ""))
        return value if value > 0 else default
    except ValueError:
        return default

def env_int(name, default):
    try:
        value = int(os.environ.get(name, ""))
        return value if value > 0 else default
    except ValueError:
        return default

interval = env_float("PULP_TARTCI_WATCHDOG_INTERVAL_SECS", 5.0)
samples = env_int("PULP_TARTCI_WATCHDOG_SAMPLES", 6)
term_grace = env_float("PULP_TARTCI_WATCHDOG_TERM_GRACE_SECS", 10.0)
cpu_per_job = env_float("PULP_TARTCI_WATCHDOG_CPU_PER_JOB", 125.0)
limit = jobs * cpu_per_job

def note(message):
    print(f"pulp tartci watchdog: {message}", file=sys.stderr, flush=True)

def group_cpu(pgid):
    proc = subprocess.run(
        ["ps", "-o", "pcpu=", "-g", str(pgid)],
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        return 0.0
    total = 0.0
    for line in proc.stdout.splitlines():
        try:
            total += float(line.strip())
        except ValueError:
            pass
    return total

child = subprocess.Popen(cmd, shell=True, preexec_fn=os.setsid)
pgid = os.getpgid(child.pid)
over_samples = 0
killed = False

def stop_group(sig):
    try:
        os.killpg(pgid, sig)
    except ProcessLookupError:
        pass

def relay_stop_signal(signum, _frame):
    note(f"received signal {signum}; terminating process-group={pgid}")
    stop_group(signal.SIGTERM)
    deadline = time.monotonic() + term_grace
    while child.poll() is None and time.monotonic() < deadline:
        time.sleep(0.2)
    if child.poll() is None:
        stop_group(signal.SIGKILL)
    child.wait()
    sys.exit(128 + signum)

signal.signal(signal.SIGINT, relay_stop_signal)
signal.signal(signal.SIGTERM, relay_stop_signal)

try:
    while child.poll() is None:
        time.sleep(interval)
        if child.poll() is not None:
            break
        cpu = group_cpu(pgid)
        if cpu > limit:
            over_samples += 1
            note(
                f"process-group={pgid} cpu={cpu:.1f}% "
                f"limit={limit:.1f}% sample={over_samples}/{samples} mode={mode}"
            )
        else:
            over_samples = 0

        if mode == "kill" and over_samples >= samples:
            killed = True
            note(f"terminating process-group={pgid} after sustained CPU over lease")
            stop_group(signal.SIGTERM)
            deadline = time.monotonic() + term_grace
            while child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            if child.poll() is None:
                note(f"killing process-group={pgid} after TERM grace")
                stop_group(signal.SIGKILL)
            break
finally:
    if child.poll() is None:
        stop_group(signal.SIGTERM)

rc = child.wait()
if killed:
    sys.exit(124)
if rc < 0:
    sys.exit(128 + abs(rc))
sys.exit(rc)
)PY";

    return shell_quote(python) + " -c " + shell_quote(std::string(watchdog_py)) + " "
         + shell_quote(command) + " "
         + std::to_string(jobs) + " "
         + shell_quote(mode);
#endif
}

ScopedBuildParallelEnv::ScopedBuildParallelEnv(int jobs, bool lease_already_held) {
    if (jobs <= 0 && !lease_already_held) return;

    const std::vector<std::pair<std::string, std::string>> values = {
        {"PULP_BUILD_JOBS", jobs > 0 ? std::to_string(jobs) : env_value("PULP_BUILD_JOBS")},
        {"CMAKE_BUILD_PARALLEL_LEVEL", jobs > 0 ? std::to_string(jobs) : env_value("CMAKE_BUILD_PARALLEL_LEVEL")},
        {"CTEST_PARALLEL_LEVEL", jobs > 0 ? std::to_string(jobs) : env_value("CTEST_PARALLEL_LEVEL")},
        {"PULP_TARTCI_LEASE_HELD", lease_already_held ? "1" : env_value("PULP_TARTCI_LEASE_HELD")},
    };
    for (const auto& [name, value] : values) {
        if (value.empty()) continue;
        saved_.push_back(save_env(name));
        set_env_value(name, value);
    }
}

ScopedBuildParallelEnv::~ScopedBuildParallelEnv() {
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
        restore_env(*it);
    }
}

TartciAgentBuildLease::TartciAgentBuildLease(TartciAgentBuildLease&& other) noexcept {
    *this = std::move(other);
}

TartciAgentBuildLease& TartciAgentBuildLease::operator=(TartciAgentBuildLease&& other) noexcept {
    if (this == &other) return *this;
    release();
    ok_ = other.ok_;
    active_ = other.active_;
    exit_code_ = other.exit_code_;
    jobs_ = other.jobs_;
    error_ = std::move(other.error_);
    tartci_bin_ = std::move(other.tartci_bin_);
    lease_id_ = std::move(other.lease_id_);
    qos_ = std::move(other.qos_);
    heartbeat_stop_ = std::move(other.heartbeat_stop_);
    if (other.heartbeat_thread_.joinable()) {
        heartbeat_thread_ = std::move(other.heartbeat_thread_);
    }
    other.active_ = false;
    other.ok_ = true;
    other.jobs_ = 0;
    return *this;
}

TartciAgentBuildLease::~TartciAgentBuildLease() {
    release();
}

TartciAgentBuildLease TartciAgentBuildLease::acquire(const TartciAgentLeaseRequest& req) {
    TartciAgentBuildLease lease;

    const int env_jobs = env_positive_int("PULP_BUILD_JOBS");
    if (env_false("PULP_TARTCI_LEASES") || env_value("PULP_TARTCI_LEASE_HELD") == "1") {
        // Leases explicitly off, or a parent already holds one and exported the
        // bound. Either way we do NOT acquire a lease — but Tier 0 still caps:
        // fall back to the host default so a no-lease build can't fan out
        // unbounded. A parent lease exports PULP_BUILD_JOBS, so env_jobs wins there.
        lease.jobs_ = env_jobs > 0 ? env_jobs : tier0_default_build_jobs();
        return lease;
    }

    auto tartci = env_value("PULP_TARTCI_BIN");
    if (tartci.empty()) {
        tartci = find_executable_in_path("tartci");
    }
    if (tartci.empty()) {
        // No tartci installed — the casual single-machine case (Tier 0). No lease
        // store to consult, but the build is still bounded to a safe host default.
        lease.jobs_ = env_jobs > 0 ? env_jobs : tier0_default_build_jobs();
        return lease;
    }

    auto profile = capture_command(shell_quote(tartci) + " host-profile");
    if (profile.exit_code != 0) {
        lease.ok_ = false;
        lease.exit_code_ = profile.exit_code;
        lease.error_ = "tartci host-profile failed: " + trim(profile.output);
        return lease;
    }

    const int profile_jobs = parse_shell_assignment_int(profile.output, "PULP_BUILD_JOBS");
    if (profile_jobs <= 0 && env_jobs <= 0) {
        lease.ok_ = false;
        lease.exit_code_ = 75;
        lease.error_ = "tartci host-profile did not provide PULP_BUILD_JOBS";
        return lease;
    }

    lease.jobs_ = profile_jobs > 0 ? profile_jobs : env_jobs;
    if (env_jobs > 0 && profile_jobs > 0) {
        lease.jobs_ = std::min(env_jobs, profile_jobs);
    }
    lease.qos_ = parse_shell_assignment(profile.output, "TARTCI_AGENT_QOS");
    lease.tartci_bin_ = tartci;
    lease.lease_id_ = tartci_agent_lease_id(req);

    std::string cmd = shell_quote(tartci)
        + " leases acquire"
        + " --id " + shell_quote(lease.lease_id_)
        + " --cores " + std::to_string(lease.jobs_)
        + " --priority build"
        + " --kind " + shell_quote(command_kind(req))
        + " --owner " + shell_quote(current_owner())
        + " --label " + shell_quote(lease_label(req.project_root))
        + " --json";
    if (auto run_id = env_value("GITHUB_RUN_ID"); !run_id.empty()) {
        cmd += " --job-id " + shell_quote(run_id);
    }

    auto acquired = capture_command(cmd);
    if (acquired.exit_code != 0) {
        lease.ok_ = false;
        lease.exit_code_ = acquired.exit_code;
        lease.error_ = trim(acquired.output);
        if (lease.error_.empty()) {
            lease.error_ = "tartci lease acquire failed";
        }
        return lease;
    }

    lease.active_ = true;
    lease.start_heartbeat();
    return lease;
}

void TartciAgentBuildLease::start_heartbeat() {
    if (!active_ || tartci_bin_.empty() || lease_id_.empty()) return;
    heartbeat_stop_ = std::make_shared<std::atomic<bool>>(false);
    auto stop = heartbeat_stop_;
    auto tartci_bin = tartci_bin_;
    auto lease_id = lease_id_;
    heartbeat_thread_ = std::thread([stop, tartci_bin, lease_id] {
        while (!stop->load()) {
            for (int i = 0; i < 30 && !stop->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (stop->load()) break;
            auto cmd = shell_quote(tartci_bin) + " leases heartbeat --id "
                     + shell_quote(lease_id) + " --json" + output_to_null();
            (void)run(cmd);
        }
    });
}

void TartciAgentBuildLease::stop_heartbeat() {
    if (heartbeat_stop_) {
        heartbeat_stop_->store(true);
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    heartbeat_stop_.reset();
}

void TartciAgentBuildLease::release() {
    stop_heartbeat();
    if (!active_) return;
    auto cmd = shell_quote(tartci_bin_) + " leases release --id "
             + shell_quote(lease_id_) + " --json" + output_to_null();
    (void)run(cmd);
    active_ = false;
}
