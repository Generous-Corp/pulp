// SPDX-License-Identifier: MIT
#include "record.hpp"
#include <csignal>

// This test-owned module is loaded only by W. It does not implement CLAP/audio.
// The finite fuse is hygiene; a cleanup oracle may never count fuse expiry as success.
extern "C" void g9_vendor_run(const char* directory, std::int64_t deadline, bool healthy) {
    const std::string dir(directory);
    signal(SIGTERM, SIG_IGN);
    record(dir, "W", "vendor_enter");
    auto spawn_descendant = [&] {
        const auto child = fork();
        if (child == 0) {
            record(dir, "D", "born");
            while (now_ns() < deadline)
                usleep(1000);
            record(dir, "D", "fuse_expired");
            _exit(0);
        }
        record(dir, "W", "vendor_fork", child);
    };
    if (!healthy)
        spawn_descendant();
    touch(dir, "vendor_ready");
    bool forked_on_retry = false;
    while (now_ns() < deadline) {
        if (!forked_on_retry && exists(dir, "retry_fork")) {
            forked_on_retry = true;
            spawn_descendant();
            touch(dir, "retry_forked");
        }
        if (healthy && exists(dir, "trigger"))
            return;
        usleep(500);
    }
    record(dir, "W", "fuse_expired");
}
