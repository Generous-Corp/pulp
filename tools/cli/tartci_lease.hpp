// tartci_lease.hpp - optional tartci host lease integration for local build loops.
#pragma once

#include <filesystem>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct TartciAgentLeaseRequest {
    fs::path project_root;
    std::string command_kind;
    bool watch_loop = false;
};

struct CmakeParallelPlan {
    std::vector<std::string> args;
    int jobs = 0;
};

std::string parse_shell_assignment(const std::string& text, const std::string& key);
int parse_shell_assignment_int(const std::string& text, const std::string& key);
CmakeParallelPlan cap_cmake_build_parallel_args(const std::vector<std::string>& args,
                                                int max_jobs);

// Default build parallelism used when no tartci host lease is available: the
// bound that keeps a single machine from being oversubscribed by an unbounded
// `--parallel`. Pure form is `min(cores, RAM_budget / per_job)`, kept free of
// host calls so it is unit-testable; the host form reads hardware concurrency
// and a RAM budget (`PULP_BUILD_MEM_BUDGET_MB` override, else ~75% of physical
// RAM). Always returns >= 1.
int tier0_default_build_jobs(unsigned hw_threads, unsigned long long mem_budget_bytes);
int tier0_default_build_jobs();
std::string tartci_agent_lease_id(const TartciAgentLeaseRequest& req);
std::string apply_agent_build_qos(const std::string& command, const std::string& qos);
std::string apply_agent_build_watchdog(const std::string& command,
                                       int jobs,
                                       bool lease_active);

class ScopedBuildParallelEnv {
public:
    struct SavedEnv {
        std::string name;
        bool had_value = false;
        std::string value;
    };

    ScopedBuildParallelEnv(int jobs, bool lease_already_held);
    ~ScopedBuildParallelEnv();

    ScopedBuildParallelEnv(const ScopedBuildParallelEnv&) = delete;
    ScopedBuildParallelEnv& operator=(const ScopedBuildParallelEnv&) = delete;

private:
    std::vector<SavedEnv> saved_;
};

class TartciAgentBuildLease {
public:
    TartciAgentBuildLease() = default;
    TartciAgentBuildLease(TartciAgentBuildLease&& other) noexcept;
    TartciAgentBuildLease& operator=(TartciAgentBuildLease&& other) noexcept;
    ~TartciAgentBuildLease();

    TartciAgentBuildLease(const TartciAgentBuildLease&) = delete;
    TartciAgentBuildLease& operator=(const TartciAgentBuildLease&) = delete;

    static TartciAgentBuildLease acquire(const TartciAgentLeaseRequest& req);

    bool ok() const { return ok_; }
    bool active() const { return active_; }
    int exit_code() const { return exit_code_; }
    int jobs() const { return jobs_; }
    const std::string& qos() const { return qos_; }
    const std::string& error() const { return error_; }

private:
    void release();
    void start_heartbeat();
    void stop_heartbeat();

    bool ok_ = true;
    bool active_ = false;
    int exit_code_ = 0;
    int jobs_ = 0;
    std::string error_;
    std::string tartci_bin_;
    std::string lease_id_;
    std::string qos_;
    std::shared_ptr<std::atomic<bool>> heartbeat_stop_;
    std::thread heartbeat_thread_;
};
