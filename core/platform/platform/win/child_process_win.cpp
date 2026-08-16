// SPDX-License-Identifier: MIT
#include <pulp/platform/child_process.hpp>

#ifdef _WIN32

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace pulp::platform {

namespace {

struct WinPipe {
    HANDLE read_end = INVALID_HANDLE_VALUE;
    HANDLE write_end = INVALID_HANDLE_VALUE;

    bool create() {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read_end, &write_end, &sa, 0))
            return false;
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        return true;
    }
    bool create_input() {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read_end, &write_end, &sa, 0))
            return false;
        if (!SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0)) {
            close_all();
            return false;
        }
        return true;
    }
    void close_write() {
        if (write_end != INVALID_HANDLE_VALUE) {
            CloseHandle(write_end);
            write_end = INVALID_HANDLE_VALUE;
        }
    }
    void close_read() {
        if (read_end != INVALID_HANDLE_VALUE) {
            CloseHandle(read_end);
            read_end = INVALID_HANDLE_VALUE;
        }
    }
    void close_all() {
        close_read();
        close_write();
    }
};

bool drain_pipe(HANDLE fd, std::string& full_output, std::string& line_buf,
                size_t max_bytes,
                std::function<void(std::string_view)>* line_cb) {
    if (fd == INVALID_HANDLE_VALUE) return true;
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(fd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            break;

        char chunk[4096];
        DWORD to_read = static_cast<DWORD>(std::min<size_t>(avail, sizeof(chunk)));
        DWORD bytes_read = 0;
        if (!ReadFile(fd, chunk, to_read, &bytes_read, nullptr) || bytes_read == 0)
            break;

        if (full_output.size() < max_bytes)
            full_output.append(chunk, std::min<size_t>(bytes_read, max_bytes - full_output.size()));
        if (line_cb && *line_cb && line_buf.size() < max_bytes)
            line_buf.append(chunk, std::min<size_t>(bytes_read, max_bytes - line_buf.size()));
    }

    if (line_cb && *line_cb && !line_buf.empty()) {
        size_t pos = 0;
        while (pos < line_buf.size()) {
            auto nl = line_buf.find('\n', pos);
            if (nl == std::string::npos) break;
            auto line = std::string_view(line_buf).substr(pos, nl - pos);
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line = line.substr(0, line.size() - 1);
            try {
                (*line_cb)(line);
            } catch (...) {
                *line_cb = {};
                line_buf.clear();
                return false;
            }
            pos = nl + 1;
        }
        if (pos > 0) line_buf.erase(0, pos);
    }
    return true;
}

std::string quote_windows_arg(const std::string& arg) {
    if (arg.empty()) return "\"\"";

    const bool needs_quotes =
        arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs_quotes) return arg;

    std::string quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back('"');

    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted.push_back(c);
        }
    }

    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

bool is_valid_handle(HANDLE handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

HANDLE open_null_device(DWORD access, SECURITY_ATTRIBUTES& sa) {
    return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void wipe_bytes(std::vector<std::uint8_t>& bytes) noexcept {
    if (!bytes.empty())
        SecureZeroMemory(bytes.data(), bytes.size());
}

struct SensitiveInputBytes {
    std::vector<std::uint8_t> value;
    ~SensitiveInputBytes() { wipe_bytes(value); }
};

}  // namespace

struct ChildProcess::Impl {
    mutable std::recursive_mutex mutex;
    HANDLE process = INVALID_HANDLE_VALUE;
    DWORD process_id = 0;
    WinPipe stdout_pipe;
    WinPipe stderr_pipe;
    WinPipe standard_input;
    ProcessOptions options;
    std::string stdout_full;
    std::string stderr_full;
    std::string stdout_lines_buf;
    std::string stderr_lines_buf;
    bool started = false;
    bool finished = false;
    ProcessResult result;
};

ChildProcessInputChannel::~ChildProcessInputChannel() {
    if (handle_ >= 0)
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

ChildProcessInputChannel::ChildProcessInputChannel(ChildProcessInputChannel&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)) {}

ChildProcessInputChannel&
ChildProcessInputChannel::operator=(ChildProcessInputChannel&& other) noexcept {
    if (this != &other) {
        if (handle_ >= 0)
            CloseHandle(reinterpret_cast<HANDLE>(handle_));
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
    const std::string&, const std::vector<std::string>&,
    const StandardInputChannelSession&, const ProcessOptions&) {
    return false;
}

bool ChildProcess::start_impl(const std::string& command, const std::vector<std::string>& args,
                              const ProcessOptions& options,
                              const std::span<const std::uint8_t>* standard_input,
                              const StandardInputByteProvider* standard_input_provider,
                              const StandardInputChannelSession* standard_input_session) {
    if (standard_input_session)
        return false;
    std::unique_lock<std::recursive_mutex> lock(impl_->mutex);
    const bool has_standard_input = standard_input || standard_input_provider;
    if (impl_->started && !impl_->finished) {
        if (is_running())
            cancel();
        else
            wait();
    }

    impl_->process = INVALID_HANDLE_VALUE;
    impl_->process_id = 0;
    impl_->options = options;
    impl_->stdout_full.clear();
    impl_->stderr_full.clear();
    impl_->stdout_lines_buf.clear();
    impl_->stderr_lines_buf.clear();
    impl_->started = false;
    impl_->finished = false;
    impl_->result = {};

    if ((has_standard_input && (options.standard_input_timeout_ms <= 0 ||
                                !impl_->standard_input.create_input())) ||
        (options.capture_stdout && !impl_->stdout_pipe.create()) ||
        (options.capture_stderr && !impl_->stderr_pipe.create())) {
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        return false;
    }

    // Build command line with platform-appropriate quoting.
    // Special case: cmd.exe /c passes everything after /c to the shell,
    // so metacharacters and embedded quotes must be preserved.
    bool is_cmd_c = (command == "cmd" || command == "cmd.exe") && !args.empty() &&
                    (args[0] == "/c" || args[0] == "/C");

    std::string cmdline = quote_windows_arg(command);
    for (size_t i = 0; i < args.size(); ++i) {
        auto& a = args[i];
        if (is_cmd_c && i == args.size() - 1) {
            // Last arg to cmd /c is the shell command — pass through raw
            cmdline += " " + a;
        } else {
            cmdline += " " + quote_windows_arg(a);
        }
    }

    STARTUPINFOEXA startup{};
    auto& si = startup.StartupInfo;
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE null_stdin = INVALID_HANDLE_VALUE;
    HANDLE null_stdout = INVALID_HANDLE_VALUE;
    HANDLE null_stderr = INVALID_HANDLE_VALUE;

    si.hStdInput = has_standard_input ? impl_->standard_input.read_end
                                      : GetStdHandle(STD_INPUT_HANDLE);
    if (!is_valid_handle(si.hStdInput)) {
        null_stdin = open_null_device(GENERIC_READ, sa);
        si.hStdInput = null_stdin;
    }

    if (options.capture_stdout) {
        si.hStdOutput = impl_->stdout_pipe.write_end;
    } else {
        null_stdout = open_null_device(GENERIC_WRITE, sa);
        si.hStdOutput = null_stdout;
    }
    if (options.capture_stderr) {
        si.hStdError = impl_->stderr_pipe.write_end;
    } else {
        null_stderr = open_null_device(GENERIC_WRITE, sa);
        si.hStdError = null_stderr;
    }

    if (!is_valid_handle(si.hStdInput) || !is_valid_handle(si.hStdOutput) ||
        !is_valid_handle(si.hStdError)) {
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        if (is_valid_handle(null_stdin))
            CloseHandle(null_stdin);
        if (is_valid_handle(null_stdout))
            CloseHandle(null_stdout);
        if (is_valid_handle(null_stderr))
            CloseHandle(null_stderr);
        impl_->result.exit_code = -1;
        return false;
    }

    SIZE_T attributes_size = 0;
    std::vector<std::byte> attributes;
    std::vector<HANDLE> inherited;
    DWORD creation_flags = CREATE_NO_WINDOW;
    bool attributes_initialized = false;
    if (has_standard_input) {
        si.cb = sizeof(startup);
        inherited = {si.hStdInput, si.hStdOutput, si.hStdError};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
        attributes.resize(attributes_size);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        attributes_initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                                                   &attributes_size) != FALSE;
        if (!attributes_initialized ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                       inherited.size() * sizeof(HANDLE), nullptr, nullptr)) {
            if (attributes_initialized)
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            impl_->standard_input.close_all();
            impl_->stdout_pipe.close_all();
            impl_->stderr_pipe.close_all();
            if (is_valid_handle(null_stdin))
                CloseHandle(null_stdin);
            if (is_valid_handle(null_stdout))
                CloseHandle(null_stdout);
            if (is_valid_handle(null_stderr))
                CloseHandle(null_stderr);
            impl_->result.exit_code = -1;
            return false;
        }
        creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                             TRUE, // inherit handles
                             creation_flags, nullptr,
                             options.working_directory.empty() ? nullptr
                                                               : options.working_directory.c_str(),
                             &si, &pi);

    if (attributes_initialized)
        DeleteProcThreadAttributeList(startup.lpAttributeList);

    impl_->standard_input.close_read();
    impl_->stdout_pipe.close_write();
    impl_->stderr_pipe.close_write();
    if (is_valid_handle(null_stdin))
        CloseHandle(null_stdin);
    if (is_valid_handle(null_stdout))
        CloseHandle(null_stdout);
    if (is_valid_handle(null_stderr))
        CloseHandle(null_stderr);

    if (!ok) {
        impl_->standard_input.close_all();
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
    }

    impl_->process = pi.hProcess;
    impl_->process_id = pi.dwProcessId;
    CloseHandle(pi.hThread);
    impl_->started = true;
    const auto spawned_process_id = impl_->process_id;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.standard_input_timeout_ms);
    SensitiveInputBytes provided_input;
    std::span<const std::uint8_t> input_bytes;
    if (standard_input_provider) {
        bool provided = false;
        lock.unlock();
        try {
            auto candidate = (*standard_input_provider)(static_cast<int>(spawned_process_id));
            if (candidate) {
                provided_input.value.swap(*candidate);
                provided = true;
            }
        } catch (...) {
            provided = false;
        }
        lock.lock();

        if (impl_->process_id != spawned_process_id || !impl_->started || impl_->finished)
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
        const auto input_handle = impl_->standard_input.write_end;
        std::thread writer([input_handle, input_bytes, completion] {
            std::size_t written = 0;
            bool delivered = true;
            while (written < input_bytes.size()) {
                DWORD count = 0;
                const auto remaining = input_bytes.size() - written;
                const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
                if (!WriteFile(input_handle, input_bytes.data() + written, chunk,
                               &count, nullptr) ||
                    count == 0) {
                    delivered = false;
                    break;
                }
                written += count;
            }
            completion->set_value(delivered);
        });
        auto ready = std::future_status::timeout;
        bool output_drain_failed = false;
        while ((ready = completed.wait_for(std::chrono::milliseconds(1))) !=
                   std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            const bool stdout_ok =
                drain_pipe(impl_->stdout_pipe.read_end, impl_->stdout_full,
                           impl_->stdout_lines_buf, options.max_output_bytes,
                           &impl_->options.on_stdout_line);
            if (!stdout_ok) {
                impl_->options.on_stdout_line = {};
                impl_->options.on_stderr_line = {};
            }
            const bool stderr_ok =
                drain_pipe(impl_->stderr_pipe.read_end, impl_->stderr_full,
                           impl_->stderr_lines_buf, options.max_output_bytes,
                           &impl_->options.on_stderr_line);
            if (!stdout_ok || !stderr_ok) {
                output_drain_failed = true;
                impl_->result.was_cancelled = true;
                impl_->options.on_stdout_line = {};
                impl_->options.on_stderr_line = {};
                TerminateProcess(impl_->process, 1);
                WaitForSingleObject(impl_->process, 1000);
                CancelSynchronousIo(writer.native_handle());
                break;
            }
        }
        if (ready != std::future_status::ready)
            CancelSynchronousIo(writer.native_handle());
        writer.join();
        const bool delivered = ready == std::future_status::ready && completed.get();
        impl_->standard_input.close_write();
        if (output_drain_failed || !delivered) {
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
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

int ChildProcess::process_id() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->process_id != 0 ? static_cast<int>(impl_->process_id) : -1;
}

void ChildProcess::cancel() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->finished) return;
    if (!is_running()) {
        impl_->standard_input.close_all();
        (void)wait();
        return;
    }
    TerminateProcess(impl_->process, 1);
    impl_->standard_input.close_all();
    WaitForSingleObject(impl_->process, 1000);
    DWORD code = 1;
    GetExitCodeProcess(impl_->process, &code);
    impl_->stdout_pipe.close_read();
    impl_->stderr_pipe.close_read();
    impl_->standard_input.close_all();
    CloseHandle(impl_->process);
    impl_->process = INVALID_HANDLE_VALUE;
    impl_->finished = true;
    impl_->result.was_cancelled = true;
    impl_->result.exit_code = static_cast<int>(code);
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
        const bool stdout_ok =
            drain_pipe(impl_->stdout_pipe.read_end, impl_->stdout_full,
                       impl_->stdout_lines_buf, max_bytes, &impl_->options.on_stdout_line);
        if (!stdout_ok) {
            impl_->options.on_stdout_line = {};
            impl_->options.on_stderr_line = {};
        }
        const bool stderr_ok =
            drain_pipe(impl_->stderr_pipe.read_end, impl_->stderr_full,
                       impl_->stderr_lines_buf, max_bytes,
                       &impl_->options.on_stderr_line);
        if (!stdout_ok || !stderr_ok) {
            impl_->result.was_cancelled = true;
            impl_->options.on_stdout_line = {};
            impl_->options.on_stderr_line = {};
            cancel();
            return impl_->result;
        }

        if (WaitForSingleObject(impl_->process, 0) != WAIT_TIMEOUT) {
            // Final drain
            const bool final_stdout_ok =
                drain_pipe(impl_->stdout_pipe.read_end, impl_->stdout_full,
                           impl_->stdout_lines_buf, max_bytes,
                           &impl_->options.on_stdout_line);
            if (!final_stdout_ok) {
                impl_->options.on_stdout_line = {};
                impl_->options.on_stderr_line = {};
            }
            const bool final_stderr_ok =
                drain_pipe(impl_->stderr_pipe.read_end, impl_->stderr_full,
                           impl_->stderr_lines_buf, max_bytes,
                           &impl_->options.on_stderr_line);
            if (!final_stdout_ok || !final_stderr_ok) {
                impl_->result.was_cancelled = true;
                impl_->options.on_stdout_line = {};
                impl_->options.on_stderr_line = {};
            }

            DWORD code = 0;
            GetExitCodeProcess(impl_->process, &code);
            CloseHandle(impl_->process);
            impl_->process = INVALID_HANDLE_VALUE;
            impl_->finished = true;
            impl_->result.exit_code = static_cast<int>(code);
            break;
        }

        if (impl_->options.timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= impl_->options.timeout_ms) {
                TerminateProcess(impl_->process, 1);
                WaitForSingleObject(impl_->process, 1000);
                CloseHandle(impl_->process);
                impl_->process = INVALID_HANDLE_VALUE;
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
    impl_->standard_input.close_all();

    impl_->result.stdout_output = std::move(impl_->stdout_full);
    impl_->result.stderr_output = std::move(impl_->stderr_full);

    return impl_->result;
}

std::string ChildProcess::read_available_output() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!impl_->started) return {};
    std::string full;
    std::string lines;
    drain_pipe(impl_->stdout_pipe.read_end, full, lines,
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

ProcessResult exec(const std::string& command,
                   const std::vector<std::string>& args,
                   int timeout_ms) {
    ProcessOptions opts;
    opts.timeout_ms = timeout_ms;
    return ChildProcess::run(command, args, opts);
}

std::optional<std::filesystem::path> find_on_path(const std::string& binary_name) {
    auto result = exec("where", {binary_name}, 5000);
    if (result.exit_code != 0) return std::nullopt;
    auto path = result.stdout_output;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
        path.pop_back();
    // where returns multiple lines; take the first
    auto nl = path.find('\n');
    if (nl != std::string::npos) path = path.substr(0, nl);
    while (!path.empty() && path.back() == '\r') path.pop_back();
    if (path.empty()) return std::nullopt;
    if (std::filesystem::exists(path)) return path;
    return std::nullopt;
}

}  // namespace pulp::platform

#endif  // _WIN32
