#pragma once

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mcp_test {

inline std::atomic<unsigned long long> temp_dir_counter{0};

inline unsigned long long current_process_id_for_temp_path() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(_getpid());
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& next)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }

    std::filesystem::path previous;
};

struct TempDir {
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto sequence = temp_dir_counter.fetch_add(1, std::memory_order_relaxed);
        path = std::filesystem::temp_directory_path() /
               ("pulp-mcp-server-test-" + std::to_string(current_process_id_for_temp_path()) + "-" +
                std::to_string(sequence) + "-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

inline std::string tool_call(const std::string& id, const std::string& name,
                             const std::string& arguments = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
           "\",\"arguments\":" + arguments + "}}";
}

inline void require_contains(const std::string& response, const std::string& needle) {
    INFO(response);
    REQUIRE(response.find(needle) != std::string::npos);
}

} // namespace mcp_test
