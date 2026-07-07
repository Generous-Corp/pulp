#include "tartci_lease.hpp"

#include "cli_common.hpp"
#include "shell_redirect.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
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
        lease.jobs_ = env_jobs;
        return lease;
    }

    auto tartci = env_value("PULP_TARTCI_BIN");
    if (tartci.empty()) {
        tartci = find_executable_in_path("tartci");
    }
    if (tartci.empty()) {
        lease.jobs_ = env_jobs;
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
