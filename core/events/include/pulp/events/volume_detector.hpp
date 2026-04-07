#pragma once

// MountedVolumeListChangeDetector — monitors filesystem for volume mount/unmount.
// ScopedLowPowerModeDisabler is in async_updater.hpp.
// LockingAsyncUpdater is in async_updater.hpp.
// NetworkServiceDiscovery — mDNS-based service discovery.

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace pulp::events {

/// Detects when volumes are mounted or unmounted.
/// Polls the filesystem at a configurable interval.
class MountedVolumeListChangeDetector {
public:
    MountedVolumeListChangeDetector() = default;
    ~MountedVolumeListChangeDetector();

    /// Start monitoring with the given poll interval.
    void start(std::chrono::milliseconds interval = std::chrono::seconds(2));

    /// Stop monitoring.
    void stop();

    /// Whether monitoring is active.
    bool is_running() const { return running_.load(); }

    /// Get current list of mounted volumes.
    static std::vector<std::string> get_mounted_volumes();

    /// Called when the volume list changes.
    std::function<void(const std::vector<std::string>& volumes)> on_change;

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<std::string> last_volumes_;
};

/// mDNS-based network service discovery (Bonjour/DNS-SD).
/// Discovers services on the local network.
class NetworkServiceDiscovery {
public:
    struct Service {
        std::string name;
        std::string type;      // e.g., "_http._tcp"
        std::string hostname;
        std::string address;
        uint16_t port = 0;
    };

    NetworkServiceDiscovery() = default;
    ~NetworkServiceDiscovery();

    /// Start browsing for services of the given type.
    void browse(std::string_view service_type);

    /// Stop browsing.
    void stop();

    /// Register a service on this machine.
    bool register_service(std::string_view name, std::string_view type, uint16_t port);

    /// Unregister a previously registered service.
    void unregister_service();

    /// Called when a service is discovered.
    std::function<void(const Service&)> on_service_found;

    /// Called when a service is lost.
    std::function<void(const Service&)> on_service_lost;

    /// Currently discovered services.
    const std::vector<Service>& discovered() const { return services_; }

private:
    std::vector<Service> services_;
    std::atomic<bool> running_{false};
    std::thread browse_thread_;
};

/// Locking variant of AsyncUpdater — blocks the trigger thread until
/// the update is processed on the event thread. Use with caution.
class LockingAsyncUpdater {
public:
    virtual ~LockingAsyncUpdater() = default;

    /// Trigger and wait for the update to be processed.
    void trigger_and_wait();

    /// Override to handle the update.
    virtual void handle_async_update() = 0;

private:
    std::atomic<bool> pending_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace pulp::events
