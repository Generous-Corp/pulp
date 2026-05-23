// SPDX-License-Identifier: MIT
#include <pulp/platform/child_process.hpp>

#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#ifndef __ANDROID__
#include <spawn.h>
#endif
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace pulp::platform {

namespace {

struct Pipe {
    int fd[2] = {-1, -1};
    bool create() {
        if (pipe(fd) != 0) return false;
        fcntl(fd[0], F_SETFL, O_NONBLOCK);
        return true;
    }
    void close_write() { if (fd[1] >= 0) { close(fd[1]); fd[1] = -1; } }
    void close_read()  { if (fd[0] >= 0) { close(fd[0]); fd[0] = -1; } }
    void close_all()   { close_read(); close_write(); }
    int read_end() const { return fd[0]; }
    int write_end() const { return fd[1]; }
};

// Read available bytes from a non-blocking fd.
// Appends to full_output (for ProcessResult), and to line_buf (for
// line-by-line callback splitting). The two buffers are independent.
size_t drain_pipe(int fd, std::string& full_output, std::string& line_buf,
                  size_t max_bytes,
                  const std::function<void(std::string_view)>& line_cb) {
    char chunk[4096];
    size_t total = 0;
    while (true) {
        auto n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        auto bytes = static_cast<size_t>(n);
        total += bytes;
        // Always accumulate into full_output (capped)
        if (full_output.size() < max_bytes)
            full_output.append(chunk, std::min(bytes, max_bytes - full_output.size()));
        // If line callback, also accumulate into line_buf for splitting (capped)
        if (line_cb && line_buf.size() < max_bytes)
            line_buf.append(chunk, std::min(bytes, max_bytes - line_buf.size()));
    }
    // Fire callbacks for complete lines
    if (line_cb && !line_buf.empty()) {
        size_t pos = 0;
        while (pos < line_buf.size()) {
            auto nl = line_buf.find('\n', pos);
            if (nl == std::string::npos) break;
            line_cb(std::string_view(line_buf).substr(pos, nl - pos));
            pos = nl + 1;
        }
        if (pos > 0) line_buf.erase(0, pos);
    }
    return total;
}

}  // namespace

struct ChildProcess::Impl {
    pid_t pid = -1;
    Pipe stdout_pipe;
    Pipe stderr_pipe;
    ProcessOptions options;
    std::string stdout_full;       // complete captured output for ProcessResult
    std::string stderr_full;
    std::string stdout_lines_buf;  // partial line accumulator for callbacks
    std::string stderr_lines_buf;
    bool started = false;
    bool finished = false;
    mutable int cached_exit_status = -1; // cached by is_running() for wait()
    mutable bool exit_cached = false;
    ProcessResult result;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    if (impl_ && impl_->started && !impl_->finished) {
        cancel();
        wait();
    }
}
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;

bool ChildProcess::start(const std::string& command,
                         const std::vector<std::string>& args,
                         const ProcessOptions& options) {
    if (impl_->started && !impl_->finished) {
        cancel();
        wait();
    }

    impl_->pid = -1;
    impl_->options = options;
    impl_->stdout_full.clear();
    impl_->stderr_full.clear();
    impl_->stdout_lines_buf.clear();
    impl_->stderr_lines_buf.clear();
    impl_->started = false;
    impl_->finished = false;
    impl_->exit_cached = false;
    impl_->cached_exit_status = -1;
    impl_->result = {};

    if (!impl_->stdout_pipe.create() || !impl_->stderr_pipe.create()) {
        impl_->result.exit_code = -1;
        return false;
    }

    // Build argv
    std::vector<const char*> argv;
    argv.push_back(command.c_str());
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int rc;

#ifdef __ANDROID__
    // Android Bionic doesn't reliably support posix_spawn. Use fork/exec.
    impl_->pid = fork();
    if (impl_->pid == 0) {
        // Child process
        dup2(impl_->stdout_pipe.write_end(), STDOUT_FILENO);
        dup2(impl_->stderr_pipe.write_end(), STDERR_FILENO);
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        if (!options.working_directory.empty())
            chdir(options.working_directory.c_str());
        execvp(command.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);  // exec failed
    }
    rc = (impl_->pid > 0) ? 0 : errno;
#else
    // Set up file actions: redirect stdout/stderr to pipes
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, impl_->stdout_pipe.write_end(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, impl_->stderr_pipe.write_end(), STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, impl_->stdout_pipe.read_end());
    posix_spawn_file_actions_addclose(&actions, impl_->stderr_pipe.read_end());

    // Working directory
    // posix_spawn_file_actions_addchdir_np is available on macOS 10.15+
    // and glibc 2.29+, but NOT on iOS/tvOS/watchOS simulators.
#if (defined(__APPLE__) && !(TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)) \
    || (defined(__GLIBC__) && __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 29)
    if (!options.working_directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, options.working_directory.c_str());
    }
#else
    // working_directory is silently ignored on iOS/older glibc
#endif

    rc = posix_spawnp(&impl_->pid,
                          command.c_str(),
                          &actions,
                          nullptr,  // default attributes
                          const_cast<char* const*>(argv.data()),
                          environ);

    posix_spawn_file_actions_destroy(&actions);
#endif

    // Close write ends in parent
    impl_->stdout_pipe.close_write();
    impl_->stderr_pipe.close_write();

    if (rc != 0) {
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->pid = -1;
        impl_->result.exit_code = -1;
        return false;
    }

    impl_->started = true;
    return true;
}

bool ChildProcess::is_running() const {
    if (!impl_->started || impl_->finished) return false;
    if (impl_->exit_cached) return false;
    // Use waitpid WNOHANG to detect exit including zombies.
    // Cache the exit status but do NOT set finished — wait() still needs
    // to drain pipes and build the full ProcessResult.
    int status = 0;
    auto rc = waitpid(impl_->pid, &status, WNOHANG);
    if (rc > 0) {
        impl_->cached_exit_status = status;
        impl_->exit_cached = true;
        return false;
    }
    return rc == 0;
}

void ChildProcess::cancel() {
    if (!impl_->started || impl_->finished) return;
    kill(impl_->pid, SIGTERM);
    // Grace period
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        if (waitpid(impl_->pid, &status, WNOHANG) != 0) {
            impl_->stdout_pipe.close_read();
            impl_->stderr_pipe.close_read();
            impl_->finished = true;
            impl_->result.was_cancelled = true;
            impl_->result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            impl_->result.stdout_output = std::move(impl_->stdout_full);
            impl_->result.stderr_output = std::move(impl_->stderr_full);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    kill(impl_->pid, SIGKILL);
    int status = 0;
    waitpid(impl_->pid, &status, 0);
    impl_->stdout_pipe.close_read();
    impl_->stderr_pipe.close_read();
    impl_->finished = true;
    impl_->result.was_cancelled = true;
    impl_->result.exit_code = -1;
    impl_->result.stdout_output = std::move(impl_->stdout_full);
    impl_->result.stderr_output = std::move(impl_->stderr_full);
}

ProcessResult ChildProcess::wait() {
    if (!impl_->started) return impl_->result;
    if (impl_->finished) return impl_->result;

    auto start_time = std::chrono::steady_clock::now();
    auto max_bytes = impl_->options.max_output_bytes;

    while (true) {
        // Drain pipes — full output goes to stdout_full/stderr_full,
        // line splitting goes to lines_buf (independent buffers)
        drain_pipe(impl_->stdout_pipe.read_end(), impl_->stdout_full,
                   impl_->stdout_lines_buf, max_bytes, impl_->options.on_stdout_line);
        drain_pipe(impl_->stderr_pipe.read_end(), impl_->stderr_full,
                   impl_->stderr_lines_buf, max_bytes, impl_->options.on_stderr_line);

        // Check if process exited (use cached result from is_running() if available)
        int status = 0;
        bool exited = false;
        if (impl_->exit_cached) {
            status = impl_->cached_exit_status;
            exited = true;
        } else {
            auto rc = waitpid(impl_->pid, &status, WNOHANG);
            exited = (rc > 0);
        }
        if (exited) {
            // Process exited — drain remaining output
            drain_pipe(impl_->stdout_pipe.read_end(), impl_->stdout_full,
                       impl_->stdout_lines_buf, max_bytes, impl_->options.on_stdout_line);
            drain_pipe(impl_->stderr_pipe.read_end(), impl_->stderr_full,
                       impl_->stderr_lines_buf, max_bytes, impl_->options.on_stderr_line);

            impl_->finished = true;
            impl_->result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }

        // Check timeout
        if (impl_->options.timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= impl_->options.timeout_ms) {
                kill(impl_->pid, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (waitpid(impl_->pid, &status, WNOHANG) == 0) {
                    kill(impl_->pid, SIGKILL);
                    waitpid(impl_->pid, &status, 0);
                }
                impl_->finished = true;
                impl_->result.timed_out = true;
                impl_->result.exit_code = -1;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    impl_->stdout_pipe.close_read();
    impl_->stderr_pipe.close_read();

    // Full captured output goes to result
    impl_->result.stdout_output = std::move(impl_->stdout_full);
    impl_->result.stderr_output = std::move(impl_->stderr_full);

    return impl_->result;
}

std::string ChildProcess::read_available_output() {
    if (!impl_->started) return {};
    std::string full, lines;
    drain_pipe(impl_->stdout_pipe.read_end(), full, lines,
               impl_->options.max_output_bytes, nullptr);
    return full;
}

ProcessResult ChildProcess::run(const std::string& command,
                                const std::vector<std::string>& args,
                                const ProcessOptions& options) {
    ChildProcess cp;
    if (!cp.start(command, args, options))
        return {-1, {}, {}, false, false};
    return cp.wait();
}

// ── Convenience functions ──

ProcessResult exec(const std::string& command,
                   const std::vector<std::string>& args,
                   int timeout_ms) {
    ProcessOptions opts;
    opts.timeout_ms = timeout_ms;
    return ChildProcess::run(command, args, opts);
}

std::optional<std::filesystem::path> find_on_path(const std::string& binary_name) {
    auto result = exec("/usr/bin/which", {binary_name}, 5000);
    if (result.exit_code != 0) return std::nullopt;
    auto path = result.stdout_output;
    // Trim whitespace
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
        path.pop_back();
    if (path.empty()) return std::nullopt;
    if (std::filesystem::exists(path)) return path;
    return std::nullopt;
}

}  // namespace pulp::platform

#endif  // !_WIN32
