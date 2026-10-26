// ship_child_process.hpp — start a ship-scan tool with no terminal and a clean
// signal state, using only async-signal-safe calls.
//
// The container scan runs ditto, pkgutil and hdiutil with SIGINT, SIGTERM and
// SIGHUP blocked around the spawn, and its signal handler starts hdiutil and rm
// with all three blocked. A child inherits that mask, and an ignored
// disposition, across fork and exec, so without a reset the tool would ignore
// a Ctrl-C meant for it. The child empties its mask and restores the default
// action for the three signals before exec.

#pragma once

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pulp::cli {

inline constexpr int kShipScanSignals[3] = {SIGINT, SIGTERM, SIGHUP};

// Forks and execs `path` with stdin, stdout and stderr on /dev/null, an empty
// signal mask and the default action for kShipScanSignals. Returns the child's
// pid, or -1 when fork fails. Safe to call from a signal handler.
inline pid_t spawn_quiet(const char* path, char* const argv[]) noexcept {
    const pid_t pid = fork();
    if (pid == 0) {
        for (const int sig : kShipScanSignals)
            signal(sig, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, 0);
            dup2(null_fd, 1);
            dup2(null_fd, 2);
        }
        execv(path, argv);
        _exit(127);
    }
    return pid;
}

// waitpid through EINTR. Returns the raw wait status, or -1 when the child
// cannot be waited for. Safe to call from a signal handler.
inline int wait_status(pid_t pid) noexcept {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

// The child's exit code, or -1 when it was killed or cannot be waited for.
inline int wait_exit_code(pid_t pid) noexcept {
    const int status = wait_status(pid);
    return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace pulp::cli
#endif
