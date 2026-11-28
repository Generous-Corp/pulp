#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>

namespace pulp::test {

namespace fs = std::filesystem;

namespace detail {

struct ScopedEnvVar {
    explicit ScopedEnvVar(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            old_value_ = std::string(existing);
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (old_value_) {
            set(*old_value_);
            return;
        }

#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

  private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    std::optional<std::string> old_value_;
};

inline std::string unique_token(const char* prefix) {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream out;
    out << prefix << "-" << std::hex << rng();
    return out.str();
}

} // namespace detail

struct PresetTestSandbox {
    explicit PresetTestSandbox(const char* prefix)
        : root(fs::temp_directory_path() / detail::unique_token(prefix)),
          home("HOME", root.string()), appdata("APPDATA", (root / "AppData").string()),
          xdg_config("XDG_CONFIG_HOME", (root / ".config").string()) {
        std::error_code ec;
        fs::create_directories(root / "AppData", ec);
        fs::create_directories(root / ".config", ec);
    }

    ~PresetTestSandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;

  private:
    detail::ScopedEnvVar home;
    detail::ScopedEnvVar appdata;
    detail::ScopedEnvVar xdg_config;
};

} // namespace pulp::test
