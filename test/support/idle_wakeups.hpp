#pragma once

// Idle-wakeup measurement for tests: how often this test process wakes the
// CPU while a component under test is supposed to be idle.
//
// Instruments:
// - macOS: proc_pid_rusage(RUSAGE_INFO_V4), ri_interrupt_wkups +
//   ri_pkg_idle_wkups for the whole process. Activity Monitor's "Idle Wake
//   Ups" column (top -stats idlew) is ri_pkg_idle_wkups alone. That only counts
//   wakeups that bring the whole CPU package out of idle, so it depends on the
//   rest of the machine: a process sleeping 10 ms in a loop read 76.6/s here
//   (all interrupt wakeups) and 0/s in top on a busy host. The interrupt count
//   follows the component's own timer wakeups whatever the host is doing.
// - Linux: voluntary context switches summed over /proc/self/task/*/status, a
//   proxy (each sleep counts) whose thresholds are advisory.
// - Elsewhere: unsupported; tests SKIP with the reason, never pass.
//
// A measurement means nothing unless the same instrument can see a wakeup
// source: call instrument_sees_polling() first (a thread sleeping 10 ms in a
// loop must read >= 50/s), and subtract floor_wakeups_per_second(), taken
// without the component, from each reading.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fstream>
#endif

namespace pulp::test {

// Cumulative wakeup count of this process, or nothing where unsupported.
inline std::optional<std::uint64_t> process_wakeups() {
#if defined(__APPLE__)
    rusage_info_v4 info{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return std::nullopt;
    return info.ri_interrupt_wkups + info.ri_pkg_idle_wkups;
#elif defined(__linux__)
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr)
        return std::nullopt;
    std::uint64_t total = 0;
    while (const dirent* entry = readdir(tasks)) {
        if (entry->d_name[0] == '.')
            continue;
        std::ifstream status(std::string("/proc/self/task/") + entry->d_name + "/status");
        std::string key;
        while (status >> key) {
            if (key == "voluntary_ctxt_switches:") {
                std::uint64_t value = 0;
                status >> value;
                total += value;
                break;
            }
        }
    }
    closedir(tasks);
    return total;
#else
    return std::nullopt;
#endif
}

inline bool idle_wakeups_supported() {
    return process_wakeups().has_value();
}

// Name of the counter in use, for test output.
inline const char* idle_wakeups_instrument() {
#if defined(__APPLE__)
    return "proc_pid_rusage interrupt + package-idle wakeups";
#elif defined(__linux__)
    return "voluntary context switches (proxy)";
#else
    return "unsupported";
#endif
}

// Wakeups per second of this process over `window`, the calling thread
// sleeping throughout. Zero when unsupported.
inline double wakeups_per_second(std::chrono::milliseconds window) {
    const auto before = process_wakeups();
    std::this_thread::sleep_for(window);
    const auto after = process_wakeups();
    if (!before || !after || window.count() <= 0)
        return 0.0;
    return static_cast<double>(*after - *before) * 1000.0 / static_cast<double>(window.count());
}

// The positive control: a thread that wakes every 10 ms must be visible.
inline bool instrument_sees_polling(double minimum_per_second = 50.0) {
    std::atomic<bool> run{true};
    std::thread poller([&] {
        // unbounded-wait: allow the control poller; this function clears run after 1 s
        while (run.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    const double rate = wakeups_per_second(std::chrono::milliseconds(1000));
    run = false;
    poller.join();
    return rate >= minimum_per_second;
}

// The process's own idle rate before the component exists.
inline double
floor_wakeups_per_second(std::chrono::milliseconds window = std::chrono::milliseconds(1000)) {
    return wakeups_per_second(window);
}

} // namespace pulp::test
