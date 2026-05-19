// test_cli_shellout_helpers.hpp — shared helpers for the test_cli_shellout
// suite and its sibling TUs extracted in the Phase 5 P5-4 refactor.
//
// Per CLAUDE.md: shell-out CLI tests assert exit code + stderr content
// against the built binary. These helpers wrap the platform-specific
// env-var / process / path plumbing every shell-out test repeats.
//
// Helpers are header-inline so each translation unit can include this
// without ODR issues. They were previously living in an anonymous
// namespace inside test_cli_shellout.cpp.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

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

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

inline fs::path unique_temp_dir(const std::string& prefix) {
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
}

// After the Rust CLI cutover, the instrumented C++ delegate lands at
// <build>/tools/cli/pulp-cpp. Older/pre-cutover builds used
// <build>/tools/cli/pulp. Prefer pulp-cpp so coverage builds exercise
// the C++ implementation directly, but keep the old fallback for
// compatibility. PULP_CLI_PATH can still override either path.
inline fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    auto cpp = fs::current_path() / ".." / "tools" / "cli" / "pulp-cpp";
    if (fs::exists(cpp)) return cpp;
    return fs::current_path() / ".." / "tools" / "cli" / "pulp";
}

inline ProcessResult run_pulp(const std::vector<std::string>& args,
                              int timeout_ms = 10000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

inline bool binary_exists() { return fs::exists(pulp_binary()); }

inline std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
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
