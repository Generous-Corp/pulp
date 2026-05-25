// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::platform {

/// Result of a completed child process.
struct ProcessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out = false;
    bool was_cancelled = false;
};

/// Options for child process execution.
struct ProcessOptions {
    std::string working_directory;
    int timeout_ms = 0;                    ///< 0 = no timeout
    size_t max_output_bytes = 1 << 20;     ///< 1 MB default cap
    bool capture_stdout = true;
    bool capture_stderr = true;
    /// Called for each complete captured line on stdout while output is drained.
    std::function<void(std::string_view line)> on_stdout_line;
    /// Called for each complete captured line on stderr while output is drained.
    std::function<void(std::string_view line)> on_stderr_line;
};

/// Cross-platform child process with timeout, cancellation, and line-by-line
/// output callbacks. Uses posix_spawn on POSIX (sandbox-compatible for AU
/// plugins on macOS) and CreateProcess on Windows.
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Blocking: run a command and return the result.
    static ProcessResult run(const std::string& command,
                             const std::vector<std::string>& args,
                             const ProcessOptions& options = {});

    /// Non-blocking: start a command.
    bool start(const std::string& command,
               const std::vector<std::string>& args,
               const ProcessOptions& options = {});

    /// Check if the started process is still running.
    bool is_running() const;

    /// Return the child process id, or -1 if no process has started.
    /// After wait() or cancel(), the OS may reuse this id; do not use it to
    /// signal a completed process.
    int process_id() const;

    /// Request cancellation. Sends SIGTERM (POSIX) or TerminateProcess (Windows),
    /// waits a short grace period, then sends SIGKILL if still alive.
    void cancel();

    /// Wait for the process to complete and return the result.
    ProcessResult wait();

    /// Read any available output without blocking (for non-blocking mode).
    std::string read_available_output();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Convenience: run a command and capture output (blocking).
ProcessResult exec(const std::string& command,
                   const std::vector<std::string>& args = {},
                   int timeout_ms = 30000);

/// Check if a binary exists on PATH. Returns the full path if found.
std::optional<std::filesystem::path> find_on_path(const std::string& binary_name);

}  // namespace pulp::platform
