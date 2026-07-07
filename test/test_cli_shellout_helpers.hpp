// Shared helpers for the test_cli_shellout suite and its sibling TUs.
//
// Per CLAUDE.md: shell-out CLI tests assert exit code + stderr content
// against the built binary. These helpers wrap the platform-specific
// env-var / process / path plumbing every shell-out test repeats.
//
// Helpers are header-inline so each translation unit can include this
// without ODR issues.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp_test_cli {

namespace fs = std::filesystem;
using pulp::platform::ProcessResult;
using pulp::platform::exec;

// Platform-aware setenv/unsetenv: Windows MSVC doesn't provide POSIX
// `setenv` / `unsetenv` — it uses `_putenv_s` instead (empty string
// value for unset). Wrap both so every test body stays portable.
inline int pulp_setenv(const char* name, const char* value, int /*overwrite*/) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

inline int pulp_unsetenv(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name) {
        if (const char* value = std::getenv(name)) {
            had_value_ = true;
            old_value_ = value;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            pulp_setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            pulp_unsetenv(name_.c_str());
        }
    }

    void set(const std::string& value) {
        pulp_setenv(name_.c_str(), value.c_str(), 1);
    }

    void unset() {
        pulp_unsetenv(name_.c_str());
    }

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

inline fs::path unique_temp_dir(const std::string& prefix) {
    static std::atomic<unsigned long long> counter{0};
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
    const auto serial = counter.fetch_add(1, std::memory_order_relaxed);
    return fs::temp_directory_path() /
           (prefix + "-" + std::to_string(pid) + "-" +
            std::to_string(tick) + "-" + std::to_string(serial));
}

// Shellout tests must use the CMake-provided CLI path for this configure.
// Warm CI build directories can contain stale <build>/tools/cli/pulp-cpp
// artifacts from a previous GPU-enabled build even when the current
// PULP_ENABLE_GPU=OFF configure skips the CLI target. PULP_CLI_PATH is still
// an explicit test override.
inline fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
#if defined(PULP_CLI_BINARY)
    return fs::path(PULP_CLI_BINARY);
#else
    return {};
#endif
}

// Default timeout is deliberately generous. These tests spawn `pulp`, which for
// some subcommands itself spawns AND CAPTURES a grandchild — clang-format (`pulp
// fmt`), or the `shipyard --version` pin-probe (`pulp pr`/`pulp status`). Under the
// CI gate's `ctest -j8` on a core-limited macOS VM, that grandchild is CPU-starved
// and a tight 10s budget flakily times out (`r.timed_out`) — the macOS-runner
// starvation class that reddens the required leg on unlucky, loaded runs while the
// retry usually saves it. 30s tolerates the contention without serializing the
// suite (RUN_SERIAL/PROCESSORS would over-serialize the 63 sibling cases here).
inline ProcessResult run_pulp(const std::vector<std::string>& args,
                              int timeout_ms = 30000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const fs::path& next)
        : previous_(fs::current_path()) {
        fs::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        fs::current_path(previous_, ec);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    fs::path previous_;
};

inline ProcessResult run_pulp_in_directory(const fs::path& dir,
                                           const std::vector<std::string>& args,
                                           int timeout_ms = 30000) {  // see run_pulp: grandchild-spawn budget
    const auto bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(bin));
    ScopedCurrentPath cwd(dir);
    return exec(bin.string(), args, timeout_ms);
}

inline bool binary_exists() { return fs::exists(pulp_binary()); }

inline std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

inline std::string normalize_newlines(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

inline void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << text;
    REQUIRE(f.good());
}

inline char path_separator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

inline void prepend_to_path(const fs::path& dir) {
    const char* old = std::getenv("PATH");
    auto next = dir.string();
    if (old && *old) {
        next += path_separator();
        next += old;
    }
    pulp_setenv("PATH", next.c_str(), 1);
}

#if !defined(_WIN32)
inline std::string pinned_shipyard_version_for_test() {
    auto toml = read_file(fs::current_path().parent_path().parent_path() /
                          "tools" / "shipyard.toml");
    std::istringstream lines(toml);
    std::string line;
    while (std::getline(lines, line)) {
        auto pos = line.find("version");
        if (pos == std::string::npos) continue;
        auto eq = line.find('=', pos);
        if (eq == std::string::npos) continue;
        auto value = line.substr(eq + 1);
        auto first = value.find('"');
        auto last = value.find_last_of('"');
        if (first != std::string::npos && last != std::string::npos && last > first) {
            return value.substr(first + 1, last - first - 1);
        }
    }
    return "v0.56.2";
}

inline fs::path write_fake_shipyard(const fs::path& dir, const std::string& version) {
    auto path = dir / "shipyard";
    write_text(path,
               "#!/bin/sh\n"
               "if [ \"$1\" = \"--version\" ]; then\n"
               "  echo \"shipyard " + version + "\"\n"
               "  exit 0\n"
               "fi\n"
               "if [ \"$1\" = \"pr\" ]; then\n"
               "  echo \"fake shipyard pr $2\"\n"
               "  exit 0\n"
               "fi\n"
               "echo \"fake shipyard\"\n");
    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::owner_read |
                    fs::perms::owner_write,
                    fs::perm_options::add);
    return path;
}
#endif

}  // namespace pulp_test_cli
