#pragma once

// Cross-platform device-hotplug watcher. On Linux it monitors libudev
// subsystems (loaded at runtime — no build-time libudev-dev dependency) and
// invokes a callback on device add/remove. On non-Linux platforms it compiles
// to an honest no-op: library_available() and start() return false, so callers
// can use it unconditionally and simply get no hotplug events off Linux.
//
// Consumers: AlsaSystem (audio) and AlsaMidiSystem (MIDI) both watch the
// "sound" subsystem — which covers ALSA PCM and raw-midi cards — and forward
// the callback to their device-change notification. Lives in runtime so both
// the audio and midi subsystems can share it without a cross-subsystem
// dependency (both already link pulp::runtime).

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <pulp/runtime/dynamic_library.hpp>

namespace pulp::runtime {

/// Classification of a udev action string. Pure + testable on every platform.
enum class UdevChange { added, removed, other };

/// Map a udev `ACTION` string to a change kind. `nullptr`/unknown → `other`.
/// "add" → added, "remove" → removed, "bind"/"change"/"unbind"/etc → other.
UdevChange classify_udev_action(const char* action) noexcept;

class UdevMonitor {
public:
    using ChangeCallback = std::function<void(UdevChange)>;

    UdevMonitor() = default;
    ~UdevMonitor();

    /// True iff libudev can be loaded right now (Linux dlopen probe). Always
    /// false on non-Linux. Cheap; does not start a monitor.
    static bool library_available();

    /// Begin watching `subsystems` (e.g. {"sound"}). `on_change` fires on a
    /// dedicated monitor thread for each add/remove. Returns false (starting
    /// nothing) on non-Linux, when libudev is unavailable, or when the netlink
    /// monitor can't be set up. A second start() while running is a no-op false.
    bool start(const std::vector<std::string>& subsystems, ChangeCallback on_change);

    /// Stop the monitor thread and release handles. Safe when not running and
    /// from the destructor; idempotent.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;

private:
    void run_loop();  // monitor thread body (Linux only)

    DynamicLibrary lib_;
    void* udev_ = nullptr;          // struct udev*
    void* monitor_ = nullptr;       // struct udev_monitor*
    int fd_ = -1;                   // monitor netlink fd
    int wake_pipe_[2] = {-1, -1};   // self-pipe to break poll() on stop()
    std::atomic<bool> running_{false};
    std::thread thread_;
    ChangeCallback on_change_;

    struct Api;                     // resolved libudev entry points
    Api* api_ = nullptr;
};

}  // namespace pulp::runtime
