#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace pulp_test_cli {

// Points PULP_HOME at a fresh, empty directory for the life of the test
// process, and turns the update check off. The CLI and the import tool read
// config.toml (import-design default_emit/default_mode) and an update cache
// from that home, so without this a suite's answers move with whatever the
// host's ~/.pulp holds. A case that needs a configured home still sets
// PULP_HOME itself and restores this value afterwards.
//
// The one thing carried over is the managed browser install
// (tools/chrome-for-testing), linked rather than copied: a browser-capture case
// needs the binary, and it holds no configuration.
class IsolatedPulpHome {
public:
    IsolatedPulpHome() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = std::filesystem::temp_directory_path() /
               ("pulp-test-home-" + std::to_string(stamp));
        std::filesystem::create_directories(dir_);
        if (const char* home = std::getenv("PULP_HOME"))
            previous_home_ = home;
        link_managed_browser();
        if (const char* check = std::getenv("PULP_UPDATE_CHECK_DISABLED"))
            previous_check_ = check;
        set("PULP_HOME", dir_.string());
        set("PULP_UPDATE_CHECK_DISABLED", "1");
    }

    ~IsolatedPulpHome() {
        restore("PULP_HOME", previous_home_);
        restore("PULP_UPDATE_CHECK_DISABLED", previous_check_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    IsolatedPulpHome(const IsolatedPulpHome&) = delete;
    IsolatedPulpHome& operator=(const IsolatedPulpHome&) = delete;

    const std::filesystem::path& path() const { return dir_; }

private:
    void link_managed_browser() const {
        std::filesystem::path original;
        if (previous_home_ && !previous_home_->empty()) {
            original = *previous_home_;
        } else {
#if defined(_WIN32)
            const char* user = std::getenv("USERPROFILE");
#else
            const char* user = std::getenv("HOME");
#endif
            if (user != nullptr && *user != '\0')
                original = std::filesystem::path(user) / ".pulp";
        }
        const auto browsers = original / "tools" / "chrome-for-testing";
        std::error_code ec;
        if (original.empty() || !std::filesystem::is_directory(browsers, ec))
            return;
        std::filesystem::create_directories(dir_ / "tools", ec);
        std::filesystem::create_directory_symlink(browsers, dir_ / "tools" / "chrome-for-testing", ec);
    }

    static void set(const char* name, const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    static void restore(const char* name, const std::optional<std::string>& value) {
        if (value) {
            set(name, *value);
        } else {
#if defined(_WIN32)
            _putenv_s(name, "");
#else
            unsetenv(name);
#endif
        }
    }

    std::filesystem::path dir_;
    std::optional<std::string> previous_home_;
    std::optional<std::string> previous_check_;
};

}  // namespace pulp_test_cli
