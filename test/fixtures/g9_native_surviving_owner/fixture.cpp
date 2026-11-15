// SPDX-License-Identifier: MIT
#include "record.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <optional>
#include <poll.h>
#include <pulp/platform/child_process.hpp>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

using namespace pulp::platform;
using Clock = std::chrono::steady_clock;
static std::string directory, role, mode;
static pid_t worker = -1;
static bool injected = false, reaped = false;

// Executable-local syscall witnesses exercise the unchanged native translation unit.
// Delegation reaches libc; injection is explicitly a test boundary, not a kernel error.
extern "C" int kill(pid_t target, int sig) {
    static auto actual = reinterpret_cast<int (*)(pid_t, int)>(dlsym(RTLD_NEXT, "kill"));
    const bool observed = role == "A" && target == -worker && worker > 0;
    if (observed && mode == "retry" && sig == SIGKILL && !injected) {
        injected = true;
        record(directory, "A", "kill_injected", target, sig, EAGAIN);
        touch(directory, "retry_fork");
        errno = EAGAIN;
        return -1;
    }
    int result = actual(target, sig);
    if (observed) {
        record(directory, "A", "signal", target, sig, result);
        if (reaped)
            record(directory, "A", "signal_after_reap", target, sig);
    } else if (role == "H")
        record(directory, "H", "signal", target, sig, result);
    return result;
}
extern "C" int sysctl(int* mib, u_int count, void* out, size_t* bytes, void* in, size_t size) {
    static auto actual = reinterpret_cast<int (*)(int*, u_int, void*, size_t*, void*, size_t)>(
        dlsym(RTLD_NEXT, "sysctl"));
    bool observed = role == "A" && count == 4 && mib[0] == CTL_KERN && mib[1] == KERN_PROC &&
                    mib[2] == KERN_PROC_PGRP && mib[3] == worker;
    if (observed && mode == "census_error") {
        record(directory, "A", "census_error", EIO);
        errno = EIO;
        return -1;
    }
    int result = actual(mib, count, out, bytes, in, size);
    if (observed)
        record(directory, "A", "census", mib[3], result, bytes ? *bytes : 0);
    return result;
}
extern "C" int waitid(idtype_t type, id_t id, siginfo_t* info, int flags) {
    static auto actual =
        reinterpret_cast<int (*)(idtype_t, id_t, siginfo_t*, int)>(dlsym(RTLD_NEXT, "waitid"));
    int result = actual(type, id, info, flags);
    if (role == "A" && static_cast<pid_t>(id) == worker)
        record(directory, "A", flags & WNOWAIT ? "retained_wait" : "nonretained_wait", id, result,
               result == 0 ? info->si_code : errno);
    return result;
}
extern "C" pid_t waitpid(pid_t target, int* status, int flags) {
    static auto actual = reinterpret_cast<pid_t (*)(pid_t, int*, int)>(dlsym(RTLD_NEXT, "waitpid"));
    pid_t result = actual(target, status, flags);
    if (role == "A" && target == worker) {
        record(directory, "A", "reap", target, result, flags);
        if (result == target)
            reaped = true;
    }
    return result;
}
extern "C" int posix_spawnattr_setflags(posix_spawnattr_t* attr, short flags) {
    static auto actual = reinterpret_cast<int (*)(posix_spawnattr_t*, short)>(
        dlsym(RTLD_NEXT, "posix_spawnattr_setflags"));
    if (role == "A") {
        record(directory, "A", "spawn_flags", flags, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (mode == "leak") {
            flags &= ~POSIX_SPAWN_CLOEXEC_DEFAULT;
            record(directory, "A", "leak_injected");
        }
    }
    return actual(attr, flags);
}

static bool ready_fd(int fd, short events, std::int64_t deadline) {
    while (now_ns() < deadline) {
        pollfd p{fd, events, 0};
        const auto remaining = deadline - now_ns();
        if (remaining <= 0)
            return false;
        const int wait_ms = static_cast<int>(std::clamp<std::int64_t>(remaining / 1000000, 0, 10));
        const int result = poll(&p, 1, wait_ms);
        if (result > 0)
            return (p.revents & (events | POLLHUP)) != 0;
        if (result < 0 && errno != EINTR)
            return false;
    }
    return false;
}
static bool send_alias(int fd) {
    char byte = 'G';
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL) == 1;
}
static int receive_alias(std::int64_t deadline) {
    if (!ready_fd(0, POLLIN, deadline))
        return -1;
    char byte = 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(0, &message, MSG_DONTWAIT) != 1 || byte != 'G')
        return -1;
    auto* header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}
static bool wait_file(const char* name, std::int64_t deadline) {
    while (now_ns() < deadline) {
        if (exists(directory, name))
            return true;
        usleep(1000);
    }
    return false;
}
static void observation(RetainedProcessCustody& custody) {
    const auto o = custody.retained_process_observation();
    record(directory, role.c_str(), "observation", int(o.state), o.identity_retained, o.process_id);
}

int main(int argc, char** argv) {
    if (argc != 6)
        return 64;
    role = argv[1];
    directory = argv[2];
    mode = argv[3];
    const std::string vendor = argv[4];
    const auto deadline = std::stoll(argv[5]);
    const auto cleanup_deadline = [&] {
        return Clock::now() +
               std::chrono::nanoseconds(std::max<std::int64_t>(0, deadline - now_ns()));
    };
    record(directory, role.c_str(), "born");
    if (role == "W") {
        record(directory, "W", "fd_audit", fcntl(80, F_GETFD) >= 0, fcntl(81, F_GETFD) >= 0,
               fcntl(0, F_GETFD) >= 0);
        // A's retained parent channel must be a distinct, usable lifecycle channel.
        char ready = 'W';
        if (send(0, &ready, 1, MSG_NOSIGNAL) != 1)
            return 65;
        void* module = dlopen(vendor.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module)
            return 66;
        auto run = reinterpret_cast<void (*)(const char*, std::int64_t, bool)>(
            dlsym(module, "g9_vendor_run"));
        if (!run)
            return 67;
        run(directory.c_str(), deadline, mode == "healthy");
        _exit(0);
    }
    RetainedProcessCustody custody;
    ChildProcess child;
    std::optional<ChildProcessInputChannel> channel;
    ProcessOptions options;
    options.standard_input_timeout_ms = 1000;
    const std::vector<std::string> arguments{role == "H" ? "A" : "W", directory, mode, vendor,
                                             argv[5]};
    if (role == "A") {
        // Deliberately hold both aliases of H's endpoint before launching W.
        // Correct native close-by-default must exclude them from the vendor.
        int alias = receive_alias(std::min(deadline, now_ns() + 1000000000LL));
        if (alias < 0 || dup2(alias, 80) != 80 || dup2(alias, 81) != 81)
            return 68;
        close(alias);
    }
    const auto exchange = [&](int pid, ChildProcessInputChannel input, Clock::time_point bound) {
        (void)bound;
        channel.emplace(std::move(input));
        const int fd = static_cast<int>(channel->native_handle());
        if (role == "H")
            return send_alias(fd);
        worker = pid;
        if (!ready_fd(fd, POLLIN, std::min(deadline, now_ns() + 1000000000LL)))
            return false;
        char value = 0;
        return recv(fd, &value, 1, MSG_DONTWAIT) == 1 && value == 'W';
    };
    if (!child.start_retained_with_standard_input_channel(custody, argv[0], arguments, exchange,
                                                          options)) {
        record(directory, role.c_str(), "launch_refused");
        _exit(69);
    }
    if (role == "A") {
        close(80);
        close(81);
        auto o = custody.retained_process_observation();
        worker = o.process_id;
        record(directory, "A", "worker_started", worker, o.process_group_id, o.identity_retained);
        if (!wait_file("vendor_ready", deadline))
            _exit(70);
        record(directory, "A", "ready");
        touch(directory, "anchor_ready");
        auto heartbeat = now_ns();
        while (now_ns() < deadline) {
            if (now_ns() - heartbeat > 10000000) {
                const char beat = 'B';
                (void)send(0, &beat, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
                record(directory, "A", "heartbeat");
                heartbeat = now_ns();
            }
            pollfd p{0, POLLIN, 0};
            if (poll(&p, 1, 1) > 0 && (p.revents & (POLLIN | POLLHUP))) {
                char byte;
                const auto n = recv(0, &byte, 1, MSG_DONTWAIT);
                if (n == 0) {
                    record(directory, "A", "host_eof");
                    observation(custody);
                    record(directory, "A", "cleanup_start", worker);
                    auto result = custody.cleanup_retained_process_group(cleanup_deadline());
                    record(directory, "A", "cleanup_result", int(result.status),
                           result.child_reaped, result.descendants_gone);
                    observation(custody);
                    if (result.status == RetainedCleanupStatus::Complete && result.child_reaped &&
                        result.descendants_gone) {
                        record(directory, "A", "complete");
                        // Remain observable until the independent observer takes its snapshot.
                        while (now_ns() < deadline && !exists(directory, "observed"))
                            usleep(1000);
                        return 0;
                    }
                    record(directory, "A", "quarantine");
                    while (now_ns() < deadline && !exists(directory, "hygiene"))
                        usleep(1000);
                    record(directory, "A", "hygiene_release");
                    _exit(71);
                }
            }
        }
        record(directory, "A", "unknown_deadline");
        _exit(72);
    }
    if (!wait_file("anchor_ready", deadline))
        _exit(73);
    record(directory, "H", "ready", child.process_id());
    if (mode == "anchor_stop" || mode == "anchor_kill") {
        // This observer has authority over A only. It never acquires W custody.
        if (!wait_file("trigger", deadline))
            _exit(76);
        auto anchor = custody.retained_process_observation();
        if (!anchor.identity_retained || anchor.process_id != child.process_id())
            _exit(77);
        const int fault = mode == "anchor_stop" ? SIGSTOP : SIGKILL;
        int injected_result = ::kill(anchor.process_id, fault);
        record(directory, "H", "anchor_fault_injected", anchor.process_id, fault, injected_result);
        auto last_beat = now_ns();
        const int fd = static_cast<int>(channel->native_handle());
        while (now_ns() < deadline) {
            pollfd p{fd, POLLIN, 0};
            (void)poll(&p, 1, 10);
            char beats[64];
            auto count = recv(fd, beats, sizeof(beats), MSG_DONTWAIT);
            if (count > 0)
                last_beat = now_ns();
            if (count == 0 || now_ns() - last_beat >= 100000000)
                break;
        }
        observation(custody);
        record(directory, "H", "anchor_unknown", last_beat);
        while (now_ns() < deadline && !exists(directory, "hygiene"))
            usleep(1000);
        if (mode == "anchor_stop") {
            record(directory, "H", "hygiene_anchor_resume", anchor.process_id);
            (void)::kill(anchor.process_id, SIGCONT);
        }
        channel.reset();
        _exit(74);
    }
    if (!wait_file("trigger", deadline))
        _exit(75);
    channel.reset();
    record(directory, "H", "host_channel_closed");
    // No host-side cleanup may turn the surviving-owner result into a false positive.
    while (now_ns() < deadline && !exists(directory, "hygiene"))
        usleep(1000);
    _exit(0);
}
