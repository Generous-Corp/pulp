#pragma once

// MountedVolumeListChangeDetector — monitors filesystem for volume mount/unmount.
// ScopedLowPowerModeDisabler is in async_updater.hpp.
// LockingAsyncUpdater is in async_updater.hpp.
// NetworkServiceDiscovery — mDNS-based service discovery.

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> last_volumes_;
};

/// mDNS-based network service discovery (Bonjour/DNS-SD).
///
/// **Status — experimental (#302).** The core/events module ships
/// the API surface but no concrete backend. `browse()` with no
/// backend installed is a no-op (does not claim running); nothing
/// will ever be discovered until a host application installs a
/// backend via `install_backend()`. `register_service()` without
/// a backend returns false.
///
/// A future follow-up will ship per-platform backends
/// (Bonjour/dns-sd on mac, Avahi on Linux, WinRT on Windows,
/// NsdManager on Android). Until then consumers that need real
/// mDNS must supply their own backend (e.g., linking an
/// application-owned Bonjour wrapper and plumbing the callbacks).
class NetworkServiceDiscovery {
public:
    /// TXT records carry arbitrary key=value metadata published with a
    /// service registration (RFC 6763 §6). Keys are case-insensitive
    /// ASCII; values may be any bytes (binary-safe via std::string).
    /// A backend that cannot encode TXT records (e.g., a degraded UDP
    /// heartbeat) should still publish the base service and silently
    /// drop the records — never fail registration.
    using TxtRecords = std::unordered_map<std::string, std::string>;

    struct Service {
        std::string name;
        std::string type;      // e.g., "_http._tcp"
        std::string hostname;
        std::string address;
        uint16_t port = 0;
        TxtRecords txt_records;
    };

    /// Backend interface the host installs to get real mDNS.
    /// Every method can no-op on unsupported backends. The core
    /// layer dispatches browse/register/unregister to the backend
    /// and forwards discovered/lost events back through
    /// on_service_found / on_service_lost.
    class Backend {
    public:
        virtual ~Backend() = default;
        virtual void browse(std::string_view service_type,
                            NetworkServiceDiscovery& owner) = 0;
        virtual void stop() = 0;
        virtual bool register_service(std::string_view name,
                                      std::string_view type,
                                      uint16_t port) = 0;
        /// Default implementation drops the TXT records and forwards
        /// to the legacy 3-arg register_service. Backends with real
        /// TXT support (e.g., Bonjour) override this method.
        virtual bool register_service(std::string_view name,
                                      std::string_view type,
                                      uint16_t port,
                                      const TxtRecords& /*txt_records*/) {
            return register_service(name, type, port);
        }
        virtual void unregister_service() = 0;
    };

    NetworkServiceDiscovery() = default;
    ~NetworkServiceDiscovery();

    /// Install a concrete backend. Takes ownership.
    /// Pass nullptr to remove.
    void install_backend(std::unique_ptr<Backend> backend);

    /// Whether a concrete backend is installed. Callers can use this
    /// to detect "no mDNS available" instead of silently discovering
    /// nothing.
    bool has_backend() const noexcept { return backend_ != nullptr; }

    /// Start browsing for services of the given type.
    /// Without an installed backend this is a no-op.
    void browse(std::string_view service_type);

    /// Stop browsing.
    void stop();

    /// Register a service on this machine. Returns false when no
    /// backend is installed or the backend can't register.
    bool register_service(std::string_view name, std::string_view type, uint16_t port);

    /// Register a service with TXT records (RFC 6763 §6 metadata).
    /// Returns false when no backend is installed or registration
    /// fails. Backends without TXT support silently drop the records.
    bool register_service(std::string_view name,
                          std::string_view type,
                          uint16_t port,
                          const TxtRecords& txt_records);

    /// Unregister a previously registered service.
    void unregister_service();

    /// Push a discovered service into the core layer. Backends call
    /// this when they learn about a new service; triggers
    /// on_service_found.
    void notify_service_found(const Service& svc);

    /// Push a lost service into the core layer. Backends call this
    /// when a service disappears; triggers on_service_lost and
    /// removes it from the discovered() list.
    void notify_service_lost(const Service& svc);

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
    std::unique_ptr<Backend> backend_;
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
