// SPDX-License-Identifier: MIT
// Test-only companions to the existing allocation/mutex RT interceptors.
#include "../../native_components/rt_test_scope.hpp"
#include <cstdlib>
#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void guard() {
    if (pulp::native_components::test::rt_test_in_no_alloc_scope())
        std::abort();
}
template <typename T> T next(const char* name) {
    auto result = reinterpret_cast<T>(::dlsym(RTLD_NEXT, name));
    if (!result)
        std::abort();
    return result;
}
} // namespace
extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    guard();
    static auto real = next<int (*)(pollfd*, nfds_t, int)>("poll");
    return real(fds, count, timeout);
}
extern "C" int usleep(useconds_t time) {
    guard();
    static auto real = next<int (*)(useconds_t)>("usleep");
    return real(time);
}
extern "C" ssize_t send(int fd, const void* bytes, size_t size, int flags) {
    guard();
    static auto real = next<ssize_t (*)(int, const void*, size_t, int)>("send");
    return real(fd, bytes, size, flags);
}
extern "C" ssize_t recv(int fd, void* bytes, size_t size, int flags) {
    guard();
    static auto real = next<ssize_t (*)(int, void*, size_t, int)>("recv");
    return real(fd, bytes, size, flags);
}
extern "C" int kill(pid_t pid, int signal) {
    guard();
    static auto real = next<int (*)(pid_t, int)>("kill");
    return real(pid, signal);
}
