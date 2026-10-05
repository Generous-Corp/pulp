#pragma once

// A temp directory that no other test process, and no other call in this
// process, can be handed.
//
// CTest runs each Catch2 case as its own process and runs many at once, so a
// directory named from a clock reading alone is shared whenever two cases read
// the same tick (a duplicate turns up within a few thousand concurrent
// `steady_clock` reads on Apple silicon), and the cases then read and delete
// each other's files. The name here carries the process id and a per-process
// serial, and the directory is used only when this call created it: a name
// that already exists (left over by an earlier run whose pid was reused) is
// skipped, never shared.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp::test {

inline std::uint64_t current_process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// The candidate name for one attempt: <prefix>-<pid>-<serial>. Exposed so a
// test can check the name carries both parts.
inline std::string unique_temp_dir_name(std::string_view prefix, std::uint64_t pid,
                                        std::uint64_t serial) {
    std::string name(prefix);
    name += '-';
    name += std::to_string(pid);
    name += '-';
    name += std::to_string(serial);
    return name;
}

// Creates a new, empty directory under `parent` (the system temp directory by
// default) and returns its path. Throws std::runtime_error if none can be
// created.
inline std::filesystem::path make_unique_temp_dir(
    std::string_view prefix,
    const std::filesystem::path& parent = std::filesystem::temp_directory_path()) {
    static std::atomic<std::uint64_t> serial{0};
    std::filesystem::create_directories(parent);
    const auto pid = current_process_id();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto candidate =
            parent / unique_temp_dir_name(prefix, pid, serial.fetch_add(1));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) return candidate;
        if (ec) {
            throw std::runtime_error("make_unique_temp_dir: cannot create " +
                                     candidate.string() + ": " + ec.message());
        }
        // Already exists: another run's leftover. Never share it; take the next.
    }
    throw std::runtime_error("make_unique_temp_dir: no free name under " + parent.string());
}

}  // namespace pulp::test
