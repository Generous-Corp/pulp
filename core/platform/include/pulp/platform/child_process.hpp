// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
    /// POSIX directory descriptor consumed atomically by spawn instead of
    /// resolving working_directory. The caller retains ownership through start().
    int working_directory_descriptor = -1;
    /// macOS-only validation hook. When set, the process is born suspended;
    /// returning true resumes that exact process, while false terminates it.
    std::function<bool(int process_id)> suspended_process_validator;
    int timeout_ms = 0;                    ///< 0 = no timeout
    size_t max_output_bytes = 1 << 20;     ///< 1 MB default cap
    bool capture_stdout = true;
    bool capture_stderr = true;
    /// Called for each complete captured line on stdout while output is drained.
    /// An exception is treated as cancellation and never escapes ChildProcess.
    std::function<void(std::string_view line)> on_stdout_line;
    /// Called for each complete captured line on stderr while output is drained.
    /// An exception is treated as cancellation and never escapes ChildProcess.
    std::function<void(std::string_view line)> on_stderr_line;
    int standard_input_timeout_ms = 3000;  ///< Bound inherited-input delivery
    size_t max_standard_input_provider_bytes = 1 << 20;  ///< Cap provider-returned bytes
};

/// Produces private standard-input bytes after the child has been spawned.
/// Returning std::nullopt rejects the launch. The callback runs synchronously
/// outside ChildProcess's internal lock, may call non-mutating observers, and
/// must not block indefinitely: it is caller code and cannot be preempted.
/// Time spent in the callback consumes the standard-input delivery deadline.
using StandardInputByteProvider =
    std::function<std::optional<std::vector<std::uint8_t>>(int child_process_id)>;

/// Move-only parent endpoint of the child's inherited standard-input channel.
/// The native handle remains owned by this object and is closed on destruction.
class ChildProcessInputChannel {
public:
    ~ChildProcessInputChannel();
    ChildProcessInputChannel(const ChildProcessInputChannel&) = delete;
    ChildProcessInputChannel& operator=(const ChildProcessInputChannel&) = delete;
    ChildProcessInputChannel(ChildProcessInputChannel&& other) noexcept;
    ChildProcessInputChannel& operator=(ChildProcessInputChannel&& other) noexcept;

    std::intptr_t native_handle() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ >= 0; }

private:
    friend class ChildProcess;
    explicit ChildProcessInputChannel(std::intptr_t handle) noexcept : handle_(handle) {}
    std::intptr_t handle_ = -1;
};

/// Owns a post-spawn exchange over the child's private inherited input channel.
/// The callback runs synchronously outside ChildProcess's internal lock and
/// must bound every blocking operation to the supplied deadline. Like the byte
/// provider above, it is caller code and cannot safely be preempted.
using StandardInputChannelSession =
    std::function<bool(int child_process_id, ChildProcessInputChannel channel,
                       std::chrono::steady_clock::time_point deadline)>;

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

    /// Start with bytes delivered through a private inherited standard-input
    /// pipe. Delivery is bounded and unrelated child handles are closed.
    bool start_with_standard_input(const std::string& command, const std::vector<std::string>& args,
                                   std::span<const std::uint8_t> bytes,
                                   const ProcessOptions& options = {});

    /// Spawn the child blocked on a private inherited standard-input pipe, then
    /// obtain the bytes from a provider that receives the actual child id.
    /// Provider rejection, exceptions, oversize output, or delivery failure
    /// terminate and join the child before this returns false.
    bool start_with_standard_input(const std::string& command, const std::vector<std::string>& args,
                                   const StandardInputByteProvider& provider,
                                   const ProcessOptions& options = {});

    /// Spawn with a private duplex standard-input channel and transfer the
    /// parent endpoint to a post-spawn session. Unsupported platforms fail
    /// before spawning. Session failure terminates and joins the child.
    bool start_with_standard_input_channel(const std::string& command,
                                           const std::vector<std::string>& args,
                                           const StandardInputChannelSession& session,
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
    bool start_impl(const std::string& command, const std::vector<std::string>& args,
                    const ProcessOptions& options,
                    const std::span<const std::uint8_t>* standard_input,
                    const StandardInputByteProvider* standard_input_provider,
                    const StandardInputChannelSession* standard_input_session);

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
