// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <time.h>
#include <unistd.h>

inline std::int64_t now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}
inline void record(const std::string& dir, const char* role, const char* event,
                   long long a = 0, long long b = 0, long long c = 0) {
    const int saved = errno;
    char line[512];
    const int n = std::snprintf(line, sizeof(line),
        "{\"t_ns\":%lld,\"pid\":%d,\"ppid\":%d,\"pgid\":%d,\"role\":\"%s\","
        "\"event\":\"%s\",\"a\":%lld,\"b\":%lld,\"c\":%lld}\n",
        static_cast<long long>(now_ns()), getpid(), getppid(), getpgrp(), role, event, a, b, c);
    int fd = open((dir + "/events.jsonl").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (n > 0 && n < static_cast<int>(sizeof(line))) (void)write(fd, line, n);
        close(fd);
    }
    errno = saved;
}
inline bool exists(const std::string& dir, const char* name) {
    return access((dir + "/" + name).c_str(), F_OK) == 0;
}
inline void touch(const std::string& dir, const char* name) {
    int fd = open((dir + "/" + name).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}
