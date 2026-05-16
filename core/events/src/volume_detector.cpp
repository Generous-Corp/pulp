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
            {
                std::unique_lock lock(mutex_);
                if (cv_.wait_for(lock, interval, [this] { return !running_.load(); })) {
                    break;
                }
            }

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
    cv_.notify_all();
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
//
// #302: the core is a dispatcher that delegates to an installed
// Backend. Without a backend, every op is an honest no-op — browse()
// does NOT set running_=true (that was the pre-#302 stub's silent-
// success bug), register_service() returns false. A future follow-up
// ships concrete backends (dns-sd on mac, Avahi on linux, etc.).

NetworkServiceDiscovery::~NetworkServiceDiscovery() { stop(); }

void NetworkServiceDiscovery::install_backend(std::unique_ptr<Backend> backend) {
    // Codex P2 on #310: swapping backends clears the cached
    // discoveries so stale services from the previous backend don't
    // leak into queries against the new one.
    //
    // Codex P2 follow-up on #314: assign backend_ BEFORE emitting
    // on_service_lost, not after. If a subscriber's on_service_lost
    // handler re-enters the NSD API (has_backend(), register_service(),
    // etc.) while we're evicting, it must see the NEW backend state,
    // not the old one we just tore down via stop(). Swap first, then
    // drain lost-callbacks.
    stop();
    backend_ = std::move(backend);
    if (!services_.empty()) {
        auto lost = std::move(services_);
        services_.clear();
        if (on_service_lost) {
            for (const auto& s : lost) on_service_lost(s);
        }
    }
}

void NetworkServiceDiscovery::browse(std::string_view service_type) {
    if (!backend_) {
        // Explicit no-op: do NOT set running_=true. Callers that rely
        // on running_ as a proxy for "browsing will yield results"
        // must now pair it with has_backend().
        return;
    }
    running_.store(true);
    backend_->browse(service_type, *this);
}

void NetworkServiceDiscovery::stop() {
    running_.store(false);
    if (backend_) backend_->stop();
    if (browse_thread_.joinable()) browse_thread_.join();
}

bool NetworkServiceDiscovery::register_service(std::string_view name,
                                                std::string_view type,
                                                uint16_t port) {
    if (!backend_) return false;
    return backend_->register_service(name, type, port);
}

void NetworkServiceDiscovery::unregister_service() {
    if (backend_) backend_->unregister_service();
}

void NetworkServiceDiscovery::notify_service_found(const Service& svc) {
    // Codex P2 on #310: re-announces from the same backend must
    // refresh the cached entry (hostname/address/port can change
    // when a service restarts on a new port or moves to a new IP)
    // instead of silently dropping. Match by (name, type); replace
    // the entry if anything else differs and fire on_service_found
    // so subscribers learn about the change.
    for (auto& existing : services_) {
        if (existing.name == svc.name && existing.type == svc.type) {
            const bool changed =
                existing.hostname != svc.hostname
                || existing.address  != svc.address
                || existing.port     != svc.port;
            if (!changed) return; // true duplicate — dedup silently
            existing = svc;
            if (on_service_found) on_service_found(svc);
            return;
        }
    }
    services_.push_back(svc);
    if (on_service_found) on_service_found(svc);
}

void NetworkServiceDiscovery::notify_service_lost(const Service& svc) {
    auto it = std::find_if(services_.begin(), services_.end(),
        [&](const Service& s) { return s.name == svc.name && s.type == svc.type; });
    if (it == services_.end()) return;
    services_.erase(it);
    if (on_service_lost) on_service_lost(svc);
}

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
