#pragma once

// Linux device hotplug via libudev, loaded at runtime (no build-time
// dependency on libudev-dev). Watches one or more udev subsystems (e.g.
// "sound", which covers both ALSA PCM and ALSA raw-midi devices) and invokes
// a callback on device add/remove. Honest-fail when libudev is unavailable:
// `library_available()` returns false and `start()` returns false, so a host
// without udev simply gets no hotplug events rather than a crash.
//
// Backends (AlsaSystem, AlsaMidiSystem) own a UdevMonitor and forward its
// callback to `fire_device_change()` — the extension point the AudioSystem
// base documents for "ALSA udev monitor".

#ifndef __linux__
#error "udev_monitor.hpp is Linux-only"
#endif

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <pulp/runtime/dynamic_library.hpp>

namespace pulp::audio::linux_platform {

/// Classification of a udev action string. Pure + testable — no libudev needed.
enum class UdevChange { added, removed, other };

/// Map a udev `ACTION` string to a change kind. `nullptr`/unknown → `other`.
/// "add" → added, "remove" → removed, "bind"/"change"/"unbind"/etc → other.
UdevChange classify_udev_action(const char* action) noexcept;

class UdevMonitor {
public:
    using ChangeCallback = std::function<void(UdevChange)>;

    UdevMonitor() = default;
    ~UdevMonitor();

    /// True iff libudev can be loaded on this host right now (dlopen probe).
    /// Cheap to call; does not start a monitor.
    static bool library_available();

    /// Begin watching `subsystems` (e.g. {"sound"}). `on_change` fires on a
    /// dedicated monitor thread for each add/remove. Returns false (and starts
    /// nothing) if libudev is unavailable or the netlink monitor can't be set
    /// up. Idempotent guard: a second start() while running is a no-op false.
    bool start(const std::vector<std::string>& subsystems, ChangeCallback on_change);

    /// Stop the monitor thread and release libudev handles. Safe to call when
    /// not running and from the destructor.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;

private:
    void run_loop();  // monitor thread body

    runtime::DynamicLibrary lib_;
    void* udev_ = nullptr;      // struct udev*
    void* monitor_ = nullptr;   // struct udev_monitor*
    int fd_ = -1;               // monitor netlink fd
    int wake_pipe_[2] = {-1, -1};  // self-pipe to break poll() on stop()
    std::atomic<bool> running_{false};
    std::thread thread_;
    ChangeCallback on_change_;

    // Resolved libudev entry points (opaque pointers as void*).
    struct Api;
    Api* api_ = nullptr;
};

}  // namespace pulp::audio::linux_platform
