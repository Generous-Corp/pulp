// SPDX-License-Identifier: MIT
#pragma once

#include "isolated_clap_protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>

#if defined(__APPLE__)
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pulp::host::isolation {
using Deadline = std::chrono::steady_clock::time_point;

// Hash executable bytes while preserving the bundle root passed to CLAP init.
inline std::filesystem::path clap_hash_path(const std::filesystem::path& selection) {
    std::error_code ec;
    if (std::filesystem::is_directory(selection, ec))
        return selection / "Contents" / "MacOS" / selection.stem();
    return selection;
}

// Control path only. Every syscall is nonblocking; poll consumes the one
// absolute deadline, including partial transfers and EINTR.
inline bool transfer(int fd, void* data, std::size_t size, bool sending, Deadline deadline) {
    auto* bytes = static_cast<std::byte*>(data);
    while (size != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd item{fd, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        const auto ready =
            ::poll(&item, 1, static_cast<int>(std::max<std::int64_t>(1, left.count())));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (item.revents & (POLLERR | POLLNVAL)))
            return false;
        const auto n =
            sending ? ::send(fd, bytes, size, MSG_DONTWAIT) : ::recv(fd, bytes, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return false;
        bytes += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}
inline bool send_record(int fd, ControlRecord record, Deadline deadline) {
    return transfer(fd, &record, sizeof(record), true, deadline);
}
inline bool receive_record(int fd, ControlRecord& record, Deadline deadline) {
    return transfer(fd, &record, sizeof(record), false, deadline) && record.magic == kMagic &&
           record.abi == kAbi;
}
} // namespace pulp::host::isolation
#endif
