#pragma once

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

// A throwaway Pulp project root whose build/tools/cli holds `cli`.
//
// The audio tools run the CLI found at <project root>/build/tools/cli
// (tools/mcp/mcp_tools_internal.cpp), so a test run from the checkout executes
// whatever binary sits in <checkout>/build, from whichever build left it there,
// or none at all. Staging a known CLI here fixes the binary under test, and
// PULP_HOME points tool discovery at an empty home so the CLI answers the same
// way on every host. An empty `cli` stages none, for a negative control.
struct CliProjectRoot {
    explicit CliProjectRoot(const std::filesystem::path& cli) {
        std::ofstream(temp.path / "CMakeLists.txt") << "project(mcp_cli_fixture)\n";
        std::filesystem::create_directories(temp.path / "core");
        std::filesystem::create_directories(temp.path / "home");
        if (!cli.empty()) {
            const auto dir = temp.path / "build" / "tools" / "cli";
            std::filesystem::create_directories(dir);
            std::error_code linked;
            std::filesystem::create_symlink(cli, dir / cli.filename(), linked);
            if (linked) {
                std::error_code copied;
                std::filesystem::copy_file(cli, dir / cli.filename(), copied);
                REQUIRE_FALSE(copied);
            }
        }
        if (const char* home = std::getenv("PULP_HOME"))
            previous_home = home;
        set_home((temp.path / "home").string());
    }

    ~CliProjectRoot() {
        if (previous_home) {
            set_home(*previous_home);
        } else {
#if defined(_WIN32)
            _putenv_s("PULP_HOME", "");
#else
            unsetenv("PULP_HOME");
#endif
        }
    }

    CliProjectRoot(const CliProjectRoot&) = delete;
    CliProjectRoot& operator=(const CliProjectRoot&) = delete;

    const std::filesystem::path& path() const { return temp.path; }

  private:
    static void set_home(const std::string& value) {
#if defined(_WIN32)
        _putenv_s("PULP_HOME", value.c_str());
#else
        setenv("PULP_HOME", value.c_str(), 1);
#endif
    }

    TempDir temp;
    std::optional<std::string> previous_home;
};

// What the real `pulp audio compare` prints when the opt-in Audio Quality Lab
// is absent (it is, under CliProjectRoot's empty home). Only a run of the CLI
// produces it; a missing binary yields a shell error instead.
inline constexpr const char* kCompareRanMarker = "Audio Quality Lab is not installed (opt-in developer tool)";

// This build's CLI. Without one the shell-out cases cannot say what ran.
inline std::filesystem::path built_cli() {
#ifdef PULP_CLI_BINARY
    return std::filesystem::path(PULP_CLI_BINARY);
#else
    SKIP("PULP_CLI_BINARY is not defined: this configuration builds no pulp CLI");
    return {};
#endif
}

} // namespace mcp_test
