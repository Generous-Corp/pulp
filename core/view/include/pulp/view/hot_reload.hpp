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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

    // Content-addressed reload gate: returns true only when `path` is a readable
    // .js/.mjs whose content hash differs from the last observed hash (a
    // save-without-edit or an editor's atomic-rename touch does not reload).
    // Updates the observed-hash map as a side effect. Public because it is
    // exercised directly by the unit tests: a private definition that is
    // referenced from only one in-library call site is elided by MSVC (a private
    // method cannot be referenced across a translation unit), so a test reaching
    // it via `#define private public` links on Clang/GCC but fails on MSVC with
    // an unresolved external. Exposing it guarantees an out-of-line symbol on
    // every toolchain.
    bool should_reload_for_modified_file(const std::filesystem::path& path);

private:
    std::filesystem::path watched_path_;
    std::string entry_file_;
    ReloadCallback on_reload_;
#if !TARGET_OS_IPHONE
    std::unique_ptr<choc::file::Watcher> watcher_;
#endif
    std::unordered_map<std::string, std::uint64_t> observed_content_hashes_;

    std::mutex pending_mutex_;
    std::string pending_code_;
    bool has_pending_ = false;
    std::atomic<uint32_t> reload_count_{0};

#if !TARGET_OS_IPHONE
    void on_file_changed(const choc::file::Watcher::Event& event);
#endif
    std::optional<std::string> try_read_file(const std::filesystem::path& path);
    std::string read_file(const std::filesystem::path& path);
    void seed_observed_content_hashes();
    static std::uint64_t content_hash(std::string_view content);
};

} // namespace pulp::view
