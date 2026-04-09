#include <pulp/runtime/child_process.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#ifndef __ANDROID__
#include <spawn.h>
#endif
#include <poll.h>
#endif

#include <array>
#include <cstring>

extern "C" {
    extern char** environ;
}

namespace pulp::runtime {

#ifdef _WIN32

std::optional<ProcessResult> run_process(
    std::string_view executable,
    const std::vector<std::string>& args,
    std::string_view working_dir,
    int timeout_ms)
{
    // Build command line
    std::string cmd = "\"" + std::string(executable) + "\"";
    for (auto& a : args)
        cmd += " \"" + a + "\"";

    // Create pipes for stdout/stderr
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read, stdout_write, stderr_read, stderr_write;
    CreatePipe(&stdout_read, &stdout_write, &sa, 0);
    CreatePipe(&stderr_read, &stderr_write, &sa, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::string wd(working_dir);

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr,
                        wd.empty() ? nullptr : wd.c_str(), &si, &pi)) {
        CloseHandle(stdout_read); CloseHandle(stdout_write);
        CloseHandle(stderr_read); CloseHandle(stderr_write);
        return std::nullopt;
    }

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    ProcessResult result;
    char buf[4096];
    DWORD bytes_read;

    while (ReadFile(stdout_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0)
        result.stdout_output.append(buf, bytes_read);
    while (ReadFile(stderr_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0)
        result.stderr_output.append(buf, bytes_read);

    DWORD wait_result = WaitForSingleObject(pi.hProcess,
        timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE);

    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result.exit_code = -1;
    } else {
        DWORD exit_code;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        result.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);

    return result;
}

int launch_process(std::string_view executable, const std::vector<std::string>& args) {
    std::string cmd = "\"" + std::string(executable) + "\"";
    for (auto& a : args)
        cmd += " \"" + a + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;

    int pid = static_cast<int>(pi.dwProcessId);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return pid;
}

bool is_process_running(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD exit_code;
    bool running = GetExitCodeProcess(h, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
}

#else  // POSIX

std::optional<ProcessResult> run_process(
    std::string_view executable,
    const std::vector<std::string>& args,
    std::string_view working_dir,
    int timeout_ms)
{
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
        return std::nullopt;

    // Build argv
    std::vector<const char*> argv;
    std::string exe_str(executable);
    argv.push_back(exe_str.c_str());
    for (auto& a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid;
    int status;

#ifdef __ANDROID__
    // Android Bionic doesn't support posix_spawn on older API levels.
    // Use fork/exec instead.
    (void)working_dir;
    pid = fork();
    if (pid == 0) {
        // Child process
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        execvp(exe_str.c_str(), const_cast<char**>(argv.data()));
        _exit(127);  // exec failed
    }
    status = (pid > 0) ? 0 : -1;
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);

    // Working directory change for spawned process
    // posix_spawn_file_actions_addchdir_np is a non-standard extension
    // available on macOS and glibc 2.29+. Skip if unavailable.
#if defined(__APPLE__) || (defined(__GLIBC__) && __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 29)
    if (!working_dir.empty()) {
        std::string wd(working_dir);
        posix_spawn_file_actions_addchdir_np(&actions, wd.c_str());
    }
#else
    (void)working_dir;  // Not supported on this platform
#endif

    status = posix_spawn(&pid, exe_str.c_str(), &actions, nullptr,
                             const_cast<char**>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&actions);
#endif

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    if (status != 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        return std::nullopt;
    }

    ProcessResult result;
    char buf[4096];

    // Read stdout and stderr using poll
    struct pollfd fds[2] = {
        {stdout_pipe[0], POLLIN, 0},
        {stderr_pipe[0], POLLIN, 0}
    };
    int open_fds = 2;

    while (open_fds > 0) {
        int ret = poll(fds, 2, timeout_ms > 0 ? timeout_ms : -1);
        if (ret <= 0) break;

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(stdout_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stdout_output.append(buf, static_cast<size_t>(n));
            else { fds[0].fd = -1; --open_fds; }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(stderr_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stderr_output.append(buf, static_cast<size_t>(n));
            else { fds[1].fd = -1; --open_fds; }
        }
    }

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);
    result.exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    return result;
}

int launch_process(std::string_view executable, const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::string exe_str(executable);
    argv.push_back(exe_str.c_str());
    for (auto& a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid;
#ifdef __ANDROID__
    pid = fork();
    if (pid == 0) {
        execvp(exe_str.c_str(), const_cast<char**>(argv.data()));
        _exit(127);
    }
    return (pid > 0) ? static_cast<int>(pid) : -1;
#else
    int status = posix_spawn(&pid, exe_str.c_str(), nullptr, nullptr,
                             const_cast<char**>(argv.data()), environ);
    return (status == 0) ? static_cast<int>(pid) : -1;
#endif
}

bool is_process_running(int pid) {
    return kill(static_cast<pid_t>(pid), 0) == 0;
}

#endif

}  // namespace pulp::runtime
