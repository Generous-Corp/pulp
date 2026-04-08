// SPDX-License-Identifier: MIT
#include <pulp/platform/child_process.hpp>

#ifdef _WIN32

#include <chrono>
#include <thread>

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
        if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        return true;
    }
    void close_write() { if (write_end != INVALID_HANDLE_VALUE) { CloseHandle(write_end); write_end = INVALID_HANDLE_VALUE; } }
    void close_read()  { if (read_end != INVALID_HANDLE_VALUE) { CloseHandle(read_end); read_end = INVALID_HANDLE_VALUE; } }
    void close_all()   { close_read(); close_write(); }
};

size_t drain_pipe(HANDLE fd, std::string& buffer, size_t max_bytes,
                  const std::function<void(std::string_view)>& line_cb) {
    size_t total = 0;
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(fd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            break;

        char chunk[4096];
        DWORD to_read = static_cast<DWORD>(std::min<size_t>(avail, sizeof(chunk)));
        DWORD bytes_read = 0;
        if (!ReadFile(fd, chunk, to_read, &bytes_read, nullptr) || bytes_read == 0)
            break;

        total += bytes_read;
        if (buffer.size() < max_bytes)
            buffer.append(chunk, std::min<size_t>(bytes_read, max_bytes - buffer.size()));
    }

    if (line_cb) {
        size_t pos = 0;
        while (pos < buffer.size()) {
            auto nl = buffer.find('\n', pos);
            if (nl == std::string::npos) break;
            auto line = std::string_view(buffer).substr(pos, nl - pos);
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line = line.substr(0, line.size() - 1);
            line_cb(line);
            pos = nl + 1;
        }
        if (pos > 0) buffer.erase(0, pos);
    }
    return total;
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE process = INVALID_HANDLE_VALUE;
    WinPipe stdout_pipe;
    WinPipe stderr_pipe;
    ProcessOptions options;
    std::string stdout_lines_buf;
    std::string stderr_lines_buf;
    bool started = false;
    bool finished = false;
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
    impl_->options = options;
    impl_->stdout_lines_buf.clear();
    impl_->stderr_lines_buf.clear();
    impl_->finished = false;

    if (!impl_->stdout_pipe.create() || !impl_->stderr_pipe.create()) return false;

    // Build command line with platform-appropriate quoting.
    // Special case: cmd.exe /c passes everything after /c to the shell,
    // so metacharacters and embedded quotes must be preserved.
    bool is_cmd_c = (command == "cmd" || command == "cmd.exe") &&
                    !args.empty() && (args[0] == "/c" || args[0] == "/C");

    std::string cmdline = "\"" + command + "\"";
    for (size_t i = 0; i < args.size(); ++i) {
        auto& a = args[i];
        if (a.empty()) {
            cmdline += " \"\"";  // preserve empty args
        } else if (is_cmd_c && i == args.size() - 1) {
            // Last arg to cmd /c is the shell command — pass through raw
            cmdline += " " + a;
        } else if (a.find(' ') != std::string::npos && a.find('"') == std::string::npos) {
            cmdline += " \"" + a + "\"";
        } else {
            cmdline += " " + a;
        }
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = impl_->stdout_pipe.write_end;
    si.hStdError = impl_->stderr_pipe.write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr,
        cmdline.data(),
        nullptr, nullptr,
        TRUE,  // inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        options.working_directory.empty() ? nullptr : options.working_directory.c_str(),
        &si, &pi);

    impl_->stdout_pipe.close_write();
    impl_->stderr_pipe.close_write();

    if (!ok) {
        impl_->stdout_pipe.close_all();
        impl_->stderr_pipe.close_all();
        impl_->result.exit_code = -1;
        return false;
    }

    impl_->process = pi.hProcess;
    CloseHandle(pi.hThread);
    impl_->started = true;
    return true;
}

bool ChildProcess::is_running() const {
    if (!impl_->started || impl_->finished) return false;
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

void ChildProcess::cancel() {
    if (!impl_->started || impl_->finished) return;
    TerminateProcess(impl_->process, 1);
    WaitForSingleObject(impl_->process, 1000);
    DWORD code = 1;
    GetExitCodeProcess(impl_->process, &code);
    CloseHandle(impl_->process);
    impl_->process = INVALID_HANDLE_VALUE;
    impl_->finished = true;
    impl_->result.was_cancelled = true;
    impl_->result.exit_code = static_cast<int>(code);
}

ProcessResult ChildProcess::wait() {
    if (!impl_->started) return impl_->result;
    if (impl_->finished) return impl_->result;

    auto start_time = std::chrono::steady_clock::now();
    auto max_bytes = impl_->options.max_output_bytes;

    while (true) {
        drain_pipe(impl_->stdout_pipe.read_end, impl_->stdout_lines_buf,
                   max_bytes, impl_->options.on_stdout_line);
        drain_pipe(impl_->stderr_pipe.read_end, impl_->stderr_lines_buf,
                   max_bytes, impl_->options.on_stderr_line);

        if (WaitForSingleObject(impl_->process, 0) != WAIT_TIMEOUT) {
            // Final drain
            drain_pipe(impl_->stdout_pipe.read_end, impl_->stdout_lines_buf,
                       max_bytes, impl_->options.on_stdout_line);
            drain_pipe(impl_->stderr_pipe.read_end, impl_->stderr_lines_buf,
                       max_bytes, impl_->options.on_stderr_line);

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

    impl_->result.stdout_output = std::move(impl_->stdout_lines_buf);
    impl_->result.stderr_output = std::move(impl_->stderr_lines_buf);

    return impl_->result;
}

std::string ChildProcess::read_available_output() {
    if (!impl_->started) return {};
    std::string buf;
    drain_pipe(impl_->stdout_pipe.read_end, buf,
               impl_->options.max_output_bytes, nullptr);
    return buf;
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
