#include <pulp/events/volume_detector.hpp>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <condition_variable>

namespace pulp::events {

// ── MountedVolumeListChangeDetector ─────────────────────────────────────

MountedVolumeListChangeDetector::~MountedVolumeListChangeDetector() { stop(); }

void MountedVolumeListChangeDetector::start(std::chrono::milliseconds interval) {
    stop();
    running_.store(true);
    last_volumes_ = get_mounted_volumes();

    thread_ = std::thread([this, interval]() {
        while (running_.load()) {
            std::this_thread::sleep_for(interval);
            if (!running_.load()) break;

            auto current = get_mounted_volumes();
            if (current != last_volumes_) {
                last_volumes_ = current;
                if (on_change) on_change(current);
            }
        }
    });
}

void MountedVolumeListChangeDetector::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

std::vector<std::string> MountedVolumeListChangeDetector::get_mounted_volumes() {
    std::vector<std::string> volumes;

#ifdef __APPLE__
    // macOS: /Volumes/*
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator("/Volumes", ec))
        if (entry.is_directory())
            volumes.push_back(entry.path().string());
#elif defined(__linux__)
    // Linux: /media/$USER/* and /mnt/*
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator("/mnt", ec))
        if (entry.is_directory())
            volumes.push_back(entry.path().string());

    const char* user = std::getenv("USER");
    if (user) {
        std::string media_path = "/media/" + std::string(user);
        for (auto& entry : std::filesystem::directory_iterator(media_path, ec))
            if (entry.is_directory())
                volumes.push_back(entry.path().string());
    }
#elif defined(_WIN32)
    // Windows: drive letters
    for (char c = 'A'; c <= 'Z'; ++c) {
        std::string drive = std::string(1, c) + ":\\";
        if (std::filesystem::exists(drive))
            volumes.push_back(drive);
    }
#endif

    std::sort(volumes.begin(), volumes.end());
    return volumes;
}

// ── NetworkServiceDiscovery ─────────────────────────────────────────────

NetworkServiceDiscovery::~NetworkServiceDiscovery() { stop(); }

void NetworkServiceDiscovery::browse(std::string_view /*service_type*/) {
    // Real implementation would use mdns.h or platform APIs
    // (dns_sd.h on macOS, Avahi on Linux)
    // Stub: just start the browse thread
    running_.store(true);
}

void NetworkServiceDiscovery::stop() {
    running_.store(false);
    if (browse_thread_.joinable()) browse_thread_.join();
}

bool NetworkServiceDiscovery::register_service(std::string_view /*name*/,
                                                std::string_view /*type*/,
                                                uint16_t /*port*/) {
    // Platform-specific: dns_sd.h on macOS, Avahi on Linux
    return false;  // Not yet implemented per-platform
}

void NetworkServiceDiscovery::unregister_service() {}

// ── LockingAsyncUpdater ─────────────────────────────────────────────────

void LockingAsyncUpdater::trigger_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    pending_.store(true);

    // Process immediately (since we don't have an event loop reference)
    handle_async_update();
    pending_.store(false);
    cv_.notify_all();
}

}  // namespace pulp::events
