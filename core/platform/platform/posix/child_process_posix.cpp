// SPDX-License-Identifier: MIT
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pulp/platform/child_process.hpp>

#ifndef _WIN32

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifndef __ANDROID__
#include <spawn.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <features.h>
#if __GLIBC_PREREQ(2, 34)
#define PULP_HAS_POSIX_SPAWN_CLOSEFROM 1
#endif
#endif

extern char** environ;

namespace pulp::platform {

namespace {

bool move_descriptor_out_of_standard_range(int& descriptor) {
    if (descriptor < 0 || descriptor > STDERR_FILENO)
        return descriptor >= 0;
    const auto replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (replacement < 0)
        return false;
    ::close(descriptor);
    descriptor = replacement;
    return true;
}

struct Pipe {
    int fd[2] = {-1, -1};
    bool create() {
        if (pipe(fd) != 0) return false;
        if (!move_descriptor_out_of_standard_range(fd[0]) ||
            !move_descriptor_out_of_standard_range(fd[1])) {
            close_all();
            return false;
        }
        fcntl(fd[0], F_SETFL, O_NONBLOCK);
        return true;
    }
    void close_write() { if (fd[1] >= 0) { close(fd[1]); fd[1] = -1; } }
    void close_read()  { if (fd[0] >= 0) { close(fd[0]); fd[0] = -1; } }
    void close_all()   { close_read(); close_write(); }
    int read_end() const { return fd[0]; }
    int write_end() const { return fd[1]; }
};

struct InputChannel {
    int parent = -1;
    int child = -1;

    bool create() {
        int pair[2] = {-1, -1};
#if defined(SOCK_CLOEXEC)
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
            return false;
#else
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
            return false;
        for (const auto descriptor : pair) {
            const auto flags = ::fcntl(descriptor, F_GETFD);
            if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
                ::close(pair[0]);
                ::close(pair[1]);
                return false;
            }
        }
#endif
        parent = pair[0];
        child = pair[1];
        if (!move_descriptor_out_of_standard_range(parent) ||
            !move_descriptor_out_of_standard_range(child)) {
            close_all();
            return false;
        }
        const auto status_flags = ::fcntl(parent, F_GETFL);
        if (status_flags < 0 || ::fcntl(parent, F_SETFL, status_flags | O_NONBLOCK) != 0) {
            close_all();
            return false;
        }
        return true;
    }

    void close_parent() {
        if (parent >= 0) {
            ::close(parent);
            parent = -1;
        }
    }
    void close_child() {
        if (child >= 0) {
            ::close(child);
            child = -1;
        }
    }
    int release_parent() {
        const auto result = parent;
        parent = -1;
        return result;
    }
    void close_all() {
        close_parent();
        close_child();
    }
};

bool write_all(int descriptor, std::span<const std::uint8_t> bytes,
               std::chrono::steady_clock::time_point deadline) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            return false;
        pollfd ready{.fd = descriptor, .events = POLLOUT, .revents = 0};
        const auto wait_ms = static_cast<int>(
            std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max()));
        const auto polled = ::poll(&ready, 1, wait_ms);
        if (polled < 0 && errno == EINTR)
            continue;
        if (polled <= 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return false;
        const auto count = ::send(descriptor, bytes.data() + written, bytes.size() - written,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void wipe_bytes(std::vector<std::uint8_t>& bytes) noexcept {
    volatile std::uint8_t* cursor = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index)
        cursor[index] = 0;
}

struct SensitiveInputBytes {
    std::vector<std::uint8_t> value;
    ~SensitiveInputBytes() { wipe_bytes(value); }
};

// Read available bytes from a non-blocking fd.
// Appends to full_output (for ProcessResult), and to line_buf (for
// line-by-line callback splitting). The two buffers are independent.
size_t drain_pipe(int fd, std::string& full_output, std::string& line_buf,
                  size_t max_bytes,
                  const std::function<void(std::string_view)>& line_cb) {
    if (fd < 0) return 0;
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
    mutable std::recursive_mutex mutex;
    pid_t pid = -1;
    Pipe stdout_pipe;
    Pipe stderr_pipe;
    InputChannel standard_input;
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

ChildProcessInputChannel::~ChildProcessInputChannel() {
    if (handle_ >= 0)
        ::close(static_cast<int>(handle_));
}

ChildProcessInputChannel::ChildProcessInputChannel(ChildProcessInputChannel&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)) {}

ChildProcessInputChannel&
ChildProcessInputChannel::operator=(ChildProcessInputChannel&& other) noexcept {
    if (this != &other) {
        if (handle_ >= 0)
            ::close(static_cast<int>(handle_));
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    if (impl_ && impl_->started && !impl_->finished) {
        if (is_running())
            cancel();
        wait();
    }
}
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;

bool ChildProcess::start(const std::string& command, const std::vector<std::string>& args,
                         const ProcessOptions& options) {
    return start_impl(command, args, options, nullptr, nullptr, nullptr);
}

bool ChildProcess::start_with_standard_input(const std::string& command,
                                             const std::vector<std::string>& args,
                                             std::span<const std::uint8_t> bytes,
                                             const ProcessOptions& options) {
    return start_impl(command, args, options, &bytes, nullptr, nullptr);
}

bool ChildProcess::start_with_standard_input(const std::string& command,
                                             const std::vector<std::string>& args,
                                             const StandardInputByteProvider& provider,
                                             const ProcessOptions& options) {
    return start_impl(command, args, options, nullptr, &provider, nullptr);
}

bool ChildProcess::start_with_standard_input_channel(
    const std::string& command, const std::vector<std::string>& args,
    const StandardInputChannelSession& session, const ProcessOptions& options) {
    return start_impl(command, args, options, nullptr, nullptr, &session);
}

bool ChildProcess::start_impl(const std::string& command, const std::vector<std::string>& args,
                              const ProcessOptions& options,
                              const std::span<const std::uint8_t>* standard_input,
                              const StandardInputByteProvider* standard_input_provider,
                              const StandardInputChannelSession* standard_input_session) {
    std::unique_lock<std::recursive_mutex> lock(impl_->mutex);
    const bool has_standard_input = standard_input || standard_input_provider ||
                                    standard_input_session;
    if (impl_->started && !impl_->finished) {
        if (is_running())
            cancel();
        else
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

#ifdef __ANDROID__
    // Android's fork/exec path cannot provide the suspended-before-first-code
    // contract. Reject the option before creating a child rather than silently
    // executing an unvalidated process.
    if (options.suspended_process_validator) {
        impl_->result.exit_code = -1;
        return false;
    }
#endif

    if ((has_standard_input && (options.standard_input_timeout_ms <= 0 ||
                                !impl_->standard_input.create())) ||
        (options.capture_stdout && !impl_->stdout_pipe.create()) ||
        (options.capture_stderr && !impl_->stderr_pipe.create())) {
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
    }

    // Build argv
    std::vector<const char*> argv;
    argv.push_back(command.c_str());
    for (auto& a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int rc;

#ifdef __ANDROID__
    // Android Bionic doesn't reliably support posix_spawn. Use fork/exec.
    impl_->pid = fork();
    if (impl_->pid == 0) {
        // Child process
        if (has_standard_input && dup2(impl_->standard_input.child, STDIN_FILENO) < 0)
            _exit(126);
        if (options.capture_stdout) {
            dup2(impl_->stdout_pipe.write_end(), STDOUT_FILENO);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }
        }
        if (options.capture_stderr) {
            dup2(impl_->stderr_pipe.write_end(), STDERR_FILENO);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        // Resolve the pinned directory before closing inherited descriptors.
        // The descriptor itself is intentionally part of the close sweep; the
        // process retains the directory reference after a successful fchdir.
        if (options.working_directory_descriptor >= 0) {
            if (fchdir(options.working_directory_descriptor) != 0)
                _exit(126);
        } else if (!options.working_directory.empty() &&
                   chdir(options.working_directory.c_str()) != 0) {
            _exit(126);
        }
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->standard_input.close_all();
        if (has_standard_input) {
            const auto descriptor_limit = ::sysconf(_SC_OPEN_MAX);
            const auto upper = descriptor_limit > 0 ? descriptor_limit : 1024;
            for (int descriptor = 3; descriptor < upper; ++descriptor)
                ::close(descriptor);
        }
        execvp(command.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    }
    rc = (impl_->pid > 0) ? 0 : errno;
#else
    // Set up file actions: redirect stdout/stderr to pipes
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
    }
    int setup_error = 0;
    const auto add_action = [&](int result) {
        if (setup_error == 0 && result != 0)
            setup_error = result;
    };
    posix_spawnattr_t attributes;
    posix_spawnattr_t* attributes_pointer = nullptr;
    if (has_standard_input || options.suspended_process_validator) {
#if defined(__APPLE__)
        const auto attribute_error = posix_spawnattr_init(&attributes);
        if (attribute_error == 0) {
            attributes_pointer = &attributes;
            short flags = 0;
            if (has_standard_input)
                flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
            if (options.suspended_process_validator)
                flags |= POSIX_SPAWN_START_SUSPENDED;
            add_action(posix_spawnattr_setflags(&attributes, flags));
        } else {
            setup_error = attribute_error;
        }
#else
        if (options.suspended_process_validator)
            setup_error = ENOTSUP;
#endif
    }
    if (has_standard_input) {
        add_action(posix_spawn_file_actions_adddup2(&actions, impl_->standard_input.child,
                                                    STDIN_FILENO));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->standard_input.parent));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->standard_input.child));
    }
    if (options.capture_stdout) {
        add_action(posix_spawn_file_actions_adddup2(&actions, impl_->stdout_pipe.write_end(),
                                                    STDOUT_FILENO));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->stdout_pipe.read_end()));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->stdout_pipe.write_end()));
    } else {
        add_action(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY,
                                                    0));
    }
    if (options.capture_stderr) {
        add_action(posix_spawn_file_actions_adddup2(&actions, impl_->stderr_pipe.write_end(),
                                                    STDERR_FILENO));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->stderr_pipe.read_end()));
        add_action(posix_spawn_file_actions_addclose(&actions, impl_->stderr_pipe.write_end()));
    } else {
        add_action(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY,
                                                    0));
    }

    // Working directory
    // posix_spawn_file_actions_addchdir_np is available on macOS 10.15+
    // and glibc 2.29+, but NOT on iOS/tvOS/watchOS simulators.
#if (defined(__APPLE__) && !(TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)) ||              \
    (defined(__GLIBC__) && __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 29)
    if (options.working_directory_descriptor >= 0) {
        add_action(posix_spawn_file_actions_addfchdir_np(
            &actions, options.working_directory_descriptor));
    } else if (!options.working_directory.empty()) {
        add_action(posix_spawn_file_actions_addchdir_np(&actions,
                                                        options.working_directory.c_str()));
    }
#else
    // working_directory is silently ignored on iOS/older glibc
#endif

    if (has_standard_input) {
#if defined(PULP_HAS_POSIX_SPAWN_CLOSEFROM)
        add_action(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#elif !defined(__APPLE__)
        posix_spawn_file_actions_destroy(&actions);
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
#endif
    }

    if (setup_error != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (attributes_pointer)
            posix_spawnattr_destroy(attributes_pointer);
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
    }

    rc = posix_spawnp(&impl_->pid, command.c_str(), &actions, attributes_pointer,
                      const_cast<char* const*>(argv.data()), environ);

    posix_spawn_file_actions_destroy(&actions);
    if (attributes_pointer)
        posix_spawnattr_destroy(attributes_pointer);
#endif

    impl_->standard_input.close_child();
    // Close write ends in parent
    impl_->stdout_pipe.close_write();
    impl_->stderr_pipe.close_write();

    if (rc != 0) {
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->standard_input.close_all();
        impl_->pid = -1;
        impl_->result.exit_code = -1;
        return false;
    }

    if (options.suspended_process_validator) {
#if defined(__APPLE__)
        bool accepted = false;
        try {
            accepted = options.suspended_process_validator(impl_->pid);
        } catch (...) {
            accepted = false;
        }
        if (!accepted || ::kill(impl_->pid, SIGCONT) != 0) {
            (void)::kill(impl_->pid, SIGKILL);
            int status = 0;
            while (::waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {}
            impl_->stdout_pipe.close_all();
            impl_->stderr_pipe.close_all();
            impl_->standard_input.close_all();
            impl_->pid = -1;
            impl_->result.exit_code = -1;
            return false;
        }
#endif
    }

    impl_->started = true;
    const auto spawned_pid = impl_->pid;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.standard_input_timeout_ms);
    SensitiveInputBytes provided_input;
    std::span<const std::uint8_t> input_bytes;
    if (standard_input_session) {
        auto channel = ChildProcessInputChannel(impl_->standard_input.release_parent());
        std::atomic<bool> session_finished{false};
        std::atomic<bool> output_drain_failed{false};
        std::thread output_drainer([this, spawned_pid, &session_finished, &output_drain_failed] {
            while (!session_finished.load(std::memory_order_acquire)) {
                try {
                    std::lock_guard<std::recursive_mutex> drain_lock(impl_->mutex);
                    if (impl_->pid != spawned_pid || !impl_->started || impl_->finished)
                        break;
                    drain_pipe(impl_->stdout_pipe.read_end(), impl_->stdout_full,
                               impl_->stdout_lines_buf, impl_->options.max_output_bytes,
                               impl_->options.on_stdout_line);
                    drain_pipe(impl_->stderr_pipe.read_end(), impl_->stderr_full,
                               impl_->stderr_lines_buf, impl_->options.max_output_bytes,
                               impl_->options.on_stderr_line);
                } catch (...) {
                    output_drain_failed.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        bool completed = false;
        lock.unlock();
        try {
            completed = (*standard_input_session)(static_cast<int>(spawned_pid),
                                                  std::move(channel), deadline);
        } catch (...) {
            completed = false;
        }
        session_finished.store(true, std::memory_order_release);
        output_drainer.join();
        const bool drain_failed = output_drain_failed.load(std::memory_order_acquire);
        if (drain_failed)
            completed = false;
        lock.lock();
        if (impl_->pid != spawned_pid || !impl_->started || impl_->finished)
            return false;
        if (drain_failed) {
            impl_->options.on_stdout_line = {};
            impl_->options.on_stderr_line = {};
        }
        if (!completed || std::chrono::steady_clock::now() >= deadline) {
            cancel();
            (void)wait();
            return false;
        }
        return true;
    } else if (standard_input_provider) {
        bool provided = false;
        lock.unlock();
        try {
            auto candidate = (*standard_input_provider)(static_cast<int>(spawned_pid));
            if (candidate) {
                provided_input.value.swap(*candidate);
                provided = true;
            }
        } catch (...) {
            provided = false;
        }
        lock.lock();

        if (impl_->pid != spawned_pid || !impl_->started || impl_->finished)
            return false;
        if (!provided || provided_input.value.size() > options.max_standard_input_provider_bytes ||
            std::chrono::steady_clock::now() >= deadline) {
            cancel();
            (void)wait();
            return false;
        }
        input_bytes = provided_input.value;
    } else if (standard_input) {
        input_bytes = *standard_input;
    }

    if (has_standard_input) {
        auto completion = std::make_shared<std::promise<bool>>();
        auto completed = completion->get_future();
        const auto input_handle = impl_->standard_input.parent;
        std::thread writer([input_handle, input_bytes, deadline, completion] {
            completion->set_value(write_all(input_handle, input_bytes, deadline));
        });
        while (completed.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
            drain_pipe(impl_->stdout_pipe.read_end(), impl_->stdout_full, impl_->stdout_lines_buf,
                       options.max_output_bytes, options.on_stdout_line);
            drain_pipe(impl_->stderr_pipe.read_end(), impl_->stderr_full, impl_->stderr_lines_buf,
                       options.max_output_bytes, options.on_stderr_line);
        }
        writer.join();
        const bool delivered = completed.get();
        ::shutdown(impl_->standard_input.parent, SHUT_WR);
        impl_->standard_input.close_parent();
        if (!delivered) {
            cancel();
            (void)wait();
            return false;
        }
    }
    return true;
}

bool ChildProcess::is_running() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
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
    if (rc < 0 && errno == ECHILD) {
        impl_->cached_exit_status = -1;
        impl_->exit_cached = true;
        return false;
    }
    return rc == 0;
}

int ChildProcess::process_id() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->pid > 0 ? static_cast<int>(impl_->pid) : -1;
}

void ChildProcess::cancel() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->finished) return;
    // Reap or cache an already-exited child before signalling. While a live
    // child remains ours, an exit becomes a zombie and its PID cannot be
    // reused between this check and kill().
    if (!is_running()) {
        impl_->standard_input.close_all();
        (void)wait();
        return;
    }
    kill(impl_->pid, SIGTERM);
    impl_->standard_input.close_all();
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
    impl_->standard_input.close_all();
    impl_->finished = true;
    impl_->result.was_cancelled = true;
    impl_->result.exit_code = -1;
    impl_->result.stdout_output = std::move(impl_->stdout_full);
    impl_->result.stderr_output = std::move(impl_->stderr_full);
}

ProcessResult ChildProcess::wait() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!impl_->started) return impl_->result;
    if (impl_->finished) return impl_->result;

    auto start_time = std::chrono::steady_clock::now();
    auto max_bytes = impl_->options.max_output_bytes;

    while (true) {
        // Drain pipes — full output goes to stdout_full/stderr_full,
        // line splitting goes to lines_buf (independent buffers)
        const auto stdout_bytes =
            drain_pipe(impl_->stdout_pipe.read_end(), impl_->stdout_full,
                       impl_->stdout_lines_buf, max_bytes, impl_->options.on_stdout_line);
        const auto stderr_bytes =
            drain_pipe(impl_->stderr_pipe.read_end(), impl_->stderr_full,
                       impl_->stderr_lines_buf, max_bytes, impl_->options.on_stderr_line);

        // Check if process exited (use cached result from is_running() if available)
        int status = 0;
        bool exited = false;
        if (impl_->exit_cached) {
            if (impl_->cached_exit_status < 0) {
                impl_->finished = true;
                impl_->result.exit_code = -1;
                break;
            }
            status = impl_->cached_exit_status;
            exited = true;
        } else {
            auto rc = waitpid(impl_->pid, &status, WNOHANG);
            exited = (rc > 0);
            if (rc < 0 && errno == ECHILD) {
                impl_->finished = true;
                impl_->result.exit_code = -1;
                break;
            }
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

        if (stdout_bytes == 0 && stderr_bytes == 0) {
            std::array<pollfd, 2> ready{};
            nfds_t ready_count = 0;
            for (const auto descriptor : {impl_->stdout_pipe.read_end(),
                                          impl_->stderr_pipe.read_end()}) {
                if (descriptor >= 0)
                    ready[ready_count++] = {.fd = descriptor, .events = POLLIN, .revents = 0};
            }
            if (ready_count > 0)
                (void)::poll(ready.data(), ready_count, 1);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    impl_->stdout_pipe.close_read();
    impl_->stderr_pipe.close_read();
    impl_->standard_input.close_all();

    // Full captured output goes to result
    impl_->result.stdout_output = std::move(impl_->stdout_full);
    impl_->result.stderr_output = std::move(impl_->stderr_full);

    return impl_->result;
}

std::string ChildProcess::read_available_output() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
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
