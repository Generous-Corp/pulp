#pragma once

#include <pulp/view/script_engine.hpp>

// iOS skip: choc::file::Watcher is implemented on top of macOS's
// `FSEventStream*` APIs, which are unavailable on iOS. Hot reload is a
// dev-time-only feature; the iOS AUv3 / HostApp path always passes
// `enable_hot_reload = false` (see au_view_controller_ios.mm), so the
// real implementation never runs there anyway. To keep the
// `HotReloader` symbol available to `scripted_ui.cpp` and any other
// caller, ship a no-op stub class on iOS that satisfies the same
// interface but never instantiates a `choc::file::Watcher`.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if !defined(TARGET_OS_IPHONE)
#define TARGET_OS_IPHONE 0
#endif

#if !TARGET_OS_IPHONE
#include <choc/platform/choc_FileWatcher.h>
#endif

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <unordered_map>

namespace pulp::view {

// Callback invoked when a JS file changes and is ready to be reloaded
using ReloadCallback = std::function<void(const std::string& code)>;

// Watches JS UI files for changes and triggers hot-reload
// File changes are detected on a background thread; the reload callback
// is stored for the UI thread to pick up via poll_reload()
class HotReloader {
public:
    // Watch a specific JS file
    HotReloader(const std::filesystem::path& js_file, ReloadCallback on_reload);

    // Watch a directory for .js file changes
    HotReloader(const std::filesystem::path& directory,
                const std::string& entry_file,
                ReloadCallback on_reload);

    ~HotReloader();

    HotReloader(const HotReloader&) = delete;
    HotReloader& operator=(const HotReloader&) = delete;

    // Call from the UI thread to check if a reload is pending
    // Returns true if a reload happened
    bool poll_reload();

    // Get the path being watched
    const std::filesystem::path& watched_path() const { return watched_path_; }

    // How many reloads have occurred
    uint32_t reload_count() const { return reload_count_.load(); }

private:
    std::filesystem::path watched_path_;
    std::string entry_file_;
    ReloadCallback on_reload_;
#if !TARGET_OS_IPHONE
    std::unique_ptr<choc::file::Watcher> watcher_;
#endif
    std::unordered_map<std::string, std::filesystem::file_time_type> observed_write_times_;

    std::mutex pending_mutex_;
    std::string pending_code_;
    bool has_pending_ = false;
    std::atomic<uint32_t> reload_count_{0};

#if !TARGET_OS_IPHONE
    void on_file_changed(const choc::file::Watcher::Event& event);
#endif
    std::string read_file(const std::filesystem::path& path);
    void seed_observed_write_times();
    bool should_reload_for_modified_file(const std::filesystem::path& path);
};

} // namespace pulp::view
