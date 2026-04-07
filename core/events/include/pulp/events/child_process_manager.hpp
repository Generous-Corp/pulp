#pragma once

// ConnectedChildProcess — launch a child process with an IPC channel.
// ChildProcessManager — manage a pool of child processes (for plugin scanning etc.)

#include <pulp/runtime/child_process.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace pulp::events {

/// A child process with a bidirectional IPC channel.
/// The child process connects back to the parent via a named pipe.
class ConnectedChildProcess {
public:
    ConnectedChildProcess() = default;
    ~ConnectedChildProcess();

    /// Launch a child process with an IPC pipe.
    /// The pipe name is passed to the child as a command-line argument.
    bool launch(std::string_view executable,
                const std::vector<std::string>& extra_args = {});

    /// Whether the child process is running.
    bool is_running() const { return running_.load(); }

    /// Get the child's PID.
    int pid() const { return pid_; }

    /// Send a message to the child.
    bool send_message(std::string_view message);

    /// Kill the child process.
    void kill();

    /// Wait for the child to exit. Returns exit code.
    int wait_for_exit(int timeout_ms = 0);

    /// Called when a message is received from the child.
    std::function<void(std::string_view)> on_message;

    /// Called when the child process exits.
    std::function<void(int exit_code)> on_exit;

    /// The IPC connection (for advanced use)
    InterprocessConnection& connection() { return connection_; }

    // No copy
    ConnectedChildProcess(const ConnectedChildProcess&) = delete;
    ConnectedChildProcess& operator=(const ConnectedChildProcess&) = delete;

private:
    InterprocessConnection connection_;
    std::string pipe_name_;
    int pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
};

/// Manages a pool of child processes.
/// Useful for crash-isolated plugin scanning where each plugin is loaded
/// in a separate process to prevent host crashes.
class ChildProcessManager {
public:
    ChildProcessManager() = default;
    ~ChildProcessManager();

    /// Launch a child process and add it to the pool.
    /// Returns a pointer to the managed process (owned by the manager).
    ConnectedChildProcess* launch(std::string_view executable,
                                   const std::vector<std::string>& args = {});

    /// Number of active child processes.
    int active_count() const;

    /// Kill all child processes.
    void kill_all();

    /// Wait for all children to exit.
    void wait_all(int timeout_ms = 0);

    /// Remove completed processes from the pool.
    void cleanup();

    /// Called when any child exits.
    std::function<void(ConnectedChildProcess*, int exit_code)> on_child_exit;

    // No copy
    ChildProcessManager(const ChildProcessManager&) = delete;
    ChildProcessManager& operator=(const ChildProcessManager&) = delete;

private:
    std::vector<std::unique_ptr<ConnectedChildProcess>> children_;
    mutable std::mutex mutex_;
};

}  // namespace pulp::events
