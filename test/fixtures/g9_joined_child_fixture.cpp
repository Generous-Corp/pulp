#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pulp::platform;

namespace {

void print_cleanup(const char* label, const JoinedChildCleanup& cleanup) {
    std::cout << label << " status=" << static_cast<int>(cleanup.status)
              << " child_reaped=" << (cleanup.child_reaped ? 1 : 0)
              << " term_sent=" << (cleanup.term_sent ? 1 : 0)
              << " kill_sent=" << (cleanup.kill_sent ? 1 : 0) << '\n';
}

void keep_anchor_alive() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

int run_join() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;

    JoinedChildCustody custody;
    ChildProcess worker;
    ProcessOptions options;
    bool vendor_permission = false;
    options.suspended_process_validator = [&](int pid) {
        vendor_permission = (::getpgid(pid) == ::getpgrp());
        std::cout << "VENDOR_PERMISSION pgid=" << ::getpgid(pid)
                  << " allowed=" << (vendor_permission ? 1 : 0) << '\n';
        return vendor_permission;
    };
    if (!worker.start_joined_to_current_group(
            *group, custody, "/bin/sh", {"-c", "trap '' TERM; printf 'TERM_READY\\n'; sleep 30"},
            options))
        return 3;
    const auto worker_pid = worker.process_id();
    std::cout << "READY anchor=" << ::getpid() << " anchor_group=" << ::getpgrp()
              << " worker=" << worker_pid << '\n';
    std::cout.flush();

    ChildProcess moved(std::move(worker));
    std::string worker_output;
    bool term_ready = false;
    for (int attempt = 0; attempt < 250 && !term_ready; ++attempt) {
        worker_output += moved.read_available_output();
        term_ready = worker_output.find("TERM_READY\n") != std::string::npos;
        if (!term_ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "WORKER_TERM_READY=" << (term_ready ? 1 : 0)
              << " WORKER_GROUP_MATCH=" << (::getpgid(worker_pid) == ::getpgrp() ? 1 : 0) << '\n';
    std::cout.flush();
    worker = ChildProcess{};
    std::cout << "HANDLE_MOVED worker=" << moved.process_id() << '\n';
    moved.cancel();
    JoinedChildCustody moved_custody(std::move(custody));
    std::cout << "CUSTODY_MOVED observation="
              << static_cast<int>(moved_custody.joined_child_observation().state) << '\n';
    print_cleanup("CLEANUP", moved_custody.cleanup_joined_child(std::chrono::steady_clock::now() +
                                                                std::chrono::seconds(1)));
    std::cout << "ANCHOR_ALIVE=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return vendor_permission && term_ready ? 0 : 4;
#endif
}

int run_failed_handshake() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    ChildProcess worker;
    const auto started = worker.start_joined_to_current_group_with_standard_input_channel(
        *group, custody, "/bin/sh", {"-c", "sleep 30"},
        [](int, ChildProcessInputChannel, auto) { return false; });
    std::cout << "HANDSHAKE started=" << (started ? 1 : 0) << '\n';
    print_cleanup("CLEANUP", custody.cleanup_joined_child(std::chrono::steady_clock::now() +
                                                          std::chrono::seconds(1)));
    std::cout << "ANCHOR_ALIVE=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return started ? 3 : 0;
#endif
}

int run_stale_capability() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    // A moved-from capability must be rejected before spawn. The private
    // group identity is deliberately not reconstructible by callers.
    CurrentProcessGroup moved(std::move(*group));
    JoinedChildCustody custody;
    ChildProcess worker;
    const auto started = worker.start_joined_to_current_group(*group, custody, "/bin/true", {});
    std::cout << "STALE_CAPABILITY started=" << (started ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return started ? 3 : 0;
#endif
}

int run_deadline() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    ChildProcess worker;
    if (!worker.start_joined_to_current_group(
            *group, custody, "/bin/sh", {"-c", "trap '' TERM; printf 'TERM_READY\\n'; sleep 30"}))
        return 3;
    const auto worker_pid = worker.process_id();
    std::string worker_output;
    bool term_ready = false;
    for (int attempt = 0; attempt < 250 && !term_ready; ++attempt) {
        worker_output += worker.read_available_output();
        term_ready = worker_output.find("TERM_READY\n") != std::string::npos;
        if (!term_ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto cleanup_started = std::chrono::steady_clock::now();
    const auto expired = custody.cleanup_joined_child(std::chrono::steady_clock::now());
    const auto completed =
        custody.cleanup_joined_child(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - cleanup_started)
                                .count();
    int status = 0;
    errno = 0;
    const bool identity_reaped = ::waitpid(worker_pid, &status, WNOHANG) == -1 && errno == ECHILD;
    std::cout << "DEADLINE ready=" << (term_ready ? 1 : 0)
              << " first=" << static_cast<int>(expired.status)
              << " identity=" << static_cast<int>(expired.observation.state)
              << " second=" << static_cast<int>(completed.status)
              << " child_reaped=" << (completed.child_reaped ? 1 : 0)
              << " term_sent=" << (completed.term_sent ? 1 : 0)
              << " kill_sent=" << (completed.kill_sent ? 1 : 0) << " elapsed_ms=" << elapsed_ms
              << " identity_reaped=" << (identity_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return term_ready && completed.status == JoinedChildCleanupStatus::Complete &&
                   completed.term_sent && completed.kill_sent && identity_reaped &&
                   elapsed_ms < 1000
               ? 0
               : 4;
#endif
}

int run_destruction() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    int worker_pid = -1;
    {
        ChildProcess worker;
        if (!worker.start_joined_to_current_group(*group, custody, "/bin/sleep", {"30"}))
            return 3;
        worker_pid = worker.process_id();
    }
    const auto observed = custody.joined_child_observation();
    const auto cleanup =
        custody.cleanup_joined_child(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    std::cout << "DESTRUCTOR worker=" << worker_pid
              << " observed=" << static_cast<int>(observed.state)
              << " cleanup=" << static_cast<int>(cleanup.status)
              << " child_reaped=" << (cleanup.child_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return cleanup.status == JoinedChildCleanupStatus::Complete ? 0 : 4;
#endif
}

int run_failed_spawn() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    ChildProcess worker;
    const auto started = worker.start_joined_to_current_group(
        *group, custody, "/nonexistent/pulp-g9-joined-worker", {});
    const auto observation = custody.joined_child_observation();
    const auto cleanup = custody.cleanup_joined_child(std::chrono::steady_clock::now());
    std::cout << "FAILED_SPAWN started=" << (started ? 1 : 0) << " pid=" << worker.process_id()
              << " state=" << static_cast<int>(observation.state)
              << " cleanup=" << static_cast<int>(cleanup.status)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return !started && worker.process_id() == -1 &&
                   cleanup.status == JoinedChildCleanupStatus::Complete
               ? 0
               : 4;
#endif
}

int run_external_reap() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    ChildProcess worker;
    if (!worker.start_joined_to_current_group(*group, custody, "/bin/sh", {"-c", "sleep 1"}))
        return 3;
    const auto worker_pid = worker.process_id();
    int status = 0;
    const auto reaped = ::waitpid(worker_pid, &status, 0) == worker_pid;
    const auto observation = custody.joined_child_observation();
    const auto cleanup = custody.cleanup_joined_child(std::chrono::steady_clock::now() +
                                                      std::chrono::milliseconds(20));
    std::cout << "EXTERNAL_REAP reaped=" << (reaped ? 1 : 0)
              << " observed=" << static_cast<int>(observation.state)
              << " cleanup=" << static_cast<int>(cleanup.status)
              << " term_sent=" << (cleanup.term_sent ? 1 : 0)
              << " kill_sent=" << (cleanup.kill_sent ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return reaped && cleanup.status == JoinedChildCleanupStatus::CustodyLost ? 0 : 4;
#endif
}

int run_sigchld(bool no_cld_wait) {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    (void)no_cld_wait;
    return 77;
#else
    struct sigaction original{};
    if (::sigaction(SIGCHLD, nullptr, &original) != 0)
        return 2;
    auto restore = [&] { (void)::sigaction(SIGCHLD, &original, nullptr); };
    struct sigaction changed{};
    sigemptyset(&changed.sa_mask);
    changed.sa_handler = no_cld_wait ? SIG_DFL : SIG_IGN;
    changed.sa_flags = no_cld_wait ? SA_NOCLDWAIT : 0;
    if (::sigaction(SIGCHLD, &changed, nullptr) != 0)
        return 3;
    auto group = CurrentProcessGroup::capture();
    JoinedChildCustody custody;
    ChildProcess worker;
    const auto started =
        group && worker.start_joined_to_current_group(*group, custody, "/bin/sleep", {"30"});
    restore();
    std::cout << "SIGCHLD mode=" << (no_cld_wait ? "nocldwait" : "ignored")
              << " started=" << (started ? 1 : 0) << " pid=" << worker.process_id()
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return started ? 4 : 0;
#endif
}

int run_occupied() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody custody;
    ChildProcess first;
    if (!first.start_joined_to_current_group(*group, custody, "/bin/sleep", {"30"}))
        return 3;
    bool callback_called = false;
    ProcessOptions options;
    options.suspended_process_validator = [&](int) {
        callback_called = true;
        return true;
    };
    ChildProcess second;
    const auto started =
        second.start_joined_to_current_group(*group, custody, "/bin/true", {}, options);
    const auto cleanup =
        custody.cleanup_joined_child(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    std::cout << "OCCUPIED first=1 second=" << (started ? 1 : 0)
              << " callback=" << (callback_called ? 1 : 0) << " second_pid=" << second.process_id()
              << " child_reaped=" << (cleanup.child_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return !started && !callback_called && cleanup.status == JoinedChildCleanupStatus::Complete ? 0
                                                                                                : 4;
#endif
}

int run_two() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody first_custody, second_custody;
    ChildProcess first, second;
    const auto first_started =
        first.start_joined_to_current_group(*group, first_custody, "/bin/sleep", {"30"});
    const auto second_started =
        second.start_joined_to_current_group(*group, second_custody, "/bin/sleep", {"30"});
    const auto first_observation = first_custody.joined_child_observation();
    const auto second_observation = second_custody.joined_child_observation();
    const bool both_live = first_observation.state == JoinedChildState::Running &&
                           second_observation.state == JoinedChildState::Running;
    const auto first_cleanup = first_custody.cleanup_joined_child(std::chrono::steady_clock::now() +
                                                                  std::chrono::seconds(1));
    const auto second_cleanup = second_custody.cleanup_joined_child(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    const bool same_group =
        first_observation.process_group_id == second_observation.process_group_id;
    std::cout << "TWO first=" << (first_started ? 1 : 0) << " second=" << (second_started ? 1 : 0)
              << " both_live=" << (both_live ? 1 : 0) << " same_group=" << (same_group ? 1 : 0)
              << " first_reaped=" << (first_cleanup.child_reaped ? 1 : 0)
              << " second_reaped=" << (second_cleanup.child_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return first_started && second_started && same_group &&
                   first_cleanup.status == JoinedChildCleanupStatus::Complete &&
                   second_cleanup.status == JoinedChildCleanupStatus::Complete
               ? 0
               : 4;
#endif
}

int run_moves() {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    JoinedChildCustody first_custody, second_custody;
    ChildProcess first, second;
    if (!first.start_joined_to_current_group(*group, first_custody, "/bin/sleep", {"30"}) ||
        !second.start_joined_to_current_group(*group, second_custody, "/bin/sleep", {"30"}))
        return 3;
    const auto first_pid = first.process_id();
    const auto second_pid = second.process_id();
    // The first handle still owns its unresolved reservation while its
    // reservation object is replaced by the second one.
    first_custody = std::move(second_custody);
    const auto first_cleanup =
        first.cleanup_joined_child(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    const auto second_cleanup = first_custody.cleanup_joined_child(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    std::cout << "MOVES first_pid=" << first_pid << " second_pid=" << second_pid
              << " first_cleanup=" << static_cast<int>(first_cleanup.status)
              << " second_cleanup=" << static_cast<int>(second_cleanup.status)
              << " first_reaped=" << (first_cleanup.child_reaped ? 1 : 0)
              << " second_reaped=" << (second_cleanup.child_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    keep_anchor_alive();
    return first_cleanup.status == JoinedChildCleanupStatus::Complete &&
                   second_cleanup.status == JoinedChildCleanupStatus::Complete
               ? 0
               : 4;
#endif
}

bool fd_leaked(int argc, char** argv, int first_descriptor) {
    bool leaked = false;
    for (int index = first_descriptor; index < argc; ++index) {
        const auto descriptor = std::stoi(argv[index]);
        if (::fcntl(descriptor, F_GETFD) >= 0)
            leaked = true;
    }
    return leaked;
}

int run_fd_check(int argc, char** argv) {
    const bool leaked = fd_leaked(argc, argv, 2);
    std::cout << "FD_LEAK=" << (leaked ? 1 : 0) << '\n';
    std::cout.flush();
    return leaked ? 4 : 0;
}

bool fd_is_dev_null() {
    struct stat stdin_stat{};
    struct stat null_stat{};
    return ::fstat(STDIN_FILENO, &stdin_stat) == 0 && ::stat("/dev/null", &null_stat) == 0 &&
           stdin_stat.st_dev == null_stat.st_dev && stdin_stat.st_ino == null_stat.st_ino;
}

int run_fd_check_descendant(int argc, char** argv) {
    const bool direct_leaked = fd_leaked(argc, argv, 2);
    std::vector<std::string> args = {argv[0], "fd-check"};
    for (int index = 2; index < argc; ++index)
        args.emplace_back(argv[index]);
    std::vector<char*> child_argv;
    child_argv.reserve(args.size() + 1);
    for (auto& arg : args)
        child_argv.push_back(arg.data());
    child_argv.push_back(nullptr);
    const auto child = ::fork();
    if (child == 0) {
        ::execv(child_argv[0], child_argv.data());
        _exit(127);
    }
    int status = 127;
    if (child > 0) {
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    const bool descendant_ok = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << "FD_LEAK=" << (direct_leaked ? 1 : 0)
              << " DESCENDANT_FD_LEAK=" << (descendant_ok ? 0 : 1)
              << " STDIN_NULL=" << (fd_is_dev_null() ? 1 : 0) << '\n';
    std::cout.flush();
    return direct_leaked || !descendant_ok || !fd_is_dev_null() ? 4 : 0;
}

int run_fd_aliases(const char* self) {
#if !defined(__APPLE__) || TARGET_OS_IPHONE
    (void)self;
    return 77;
#else
    auto group = CurrentProcessGroup::capture();
    if (!group)
        return 2;
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0)
        return 3;
    const auto alias_read = ::fcntl(pipe_fds[0], F_DUPFD, STDERR_FILENO + 1);
    const auto alias_write = ::fcntl(pipe_fds[1], F_DUPFD, STDERR_FILENO + 1);
    if (alias_read < 0 || alias_write < 0)
        return 4;
    for (const auto descriptor : {pipe_fds[0], pipe_fds[1], alias_read, alias_write}) {
        const auto flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC) != 0)
            return 5;
    }
    JoinedChildCustody custody;
    ChildProcess worker;
    const auto started = worker.start_joined_to_current_group(
        *group, custody, self,
        {"fd-check-descendant", std::to_string(pipe_fds[0]), std::to_string(pipe_fds[1]),
         std::to_string(alias_read), std::to_string(alias_write)});
    std::string output;
    for (int attempt = 0; attempt < 100 && output.find("STDIN_NULL=") == std::string::npos;
         ++attempt) {
        output += worker.read_available_output();
        if (output.find("STDIN_NULL=") == std::string::npos)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto cleanup =
        custody.cleanup_joined_child(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    std::cout << "FD_ALIASES started=" << (started ? 1 : 0) << " output=" << output
              << " child_reaped=" << (cleanup.child_reaped ? 1 : 0)
              << " anchor_alive=" << (::kill(::getpid(), 0) == 0 ? 1 : 0) << '\n';
    std::cout.flush();
    for (const auto descriptor : {pipe_fds[0], pipe_fds[1], alias_read, alias_write})
        ::close(descriptor);
    keep_anchor_alive();
    return started && output.find("FD_LEAK=0") != std::string::npos &&
                   output.find("DESCENDANT_FD_LEAK=0") != std::string::npos &&
                   output.find("STDIN_NULL=1") != std::string::npos &&
                   cleanup.status == JoinedChildCleanupStatus::Complete
               ? 0
               : 4;
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    const std::string mode = argc > 1 ? argv[1] : "join";
    if (mode == "join")
        return run_join();
    if (mode == "failed-handshake")
        return run_failed_handshake();
    if (mode == "stale-capability")
        return run_stale_capability();
    if (mode == "deadline")
        return run_deadline();
    if (mode == "destruction")
        return run_destruction();
    if (mode == "failed-spawn")
        return run_failed_spawn();
    if (mode == "external-reap")
        return run_external_reap();
    if (mode == "sigchld-ignored")
        return run_sigchld(false);
    if (mode == "sigchld-nocldwait")
        return run_sigchld(true);
    if (mode == "occupied")
        return run_occupied();
    if (mode == "two")
        return run_two();
    if (mode == "moves")
        return run_moves();
    if (mode == "fd-check")
        return run_fd_check(argc, argv);
    if (mode == "fd-check-descendant")
        return run_fd_check_descendant(argc, argv);
    if (mode == "fd-aliases")
        return run_fd_aliases(argv[0]);
    return 64;
}
