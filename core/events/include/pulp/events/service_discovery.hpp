#pragma once

// pulp::events::ServiceBrowser / ServicePublisher — RAII wrappers around
// NetworkServiceDiscovery for the common single-publish / single-browse
// shapes called out in the reference-framework gap analysis (Phase 2:
// "NetworkServiceDiscovery platform backends — Bonjour / Avahi / WinRT").
//
// The underlying NSD object remains the configurable dispatcher when a
// caller needs install_backend / multiple browses; these wrappers exist
// because 80% of consumers want "publish this service, stop publishing
// on scope exit" or "browse this type, stop browsing on scope exit"
// without managing the NSD lifecycle by hand.

#include <pulp/events/volume_detector.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::events {

/// Action delivered to a ServiceBrowser callback.
enum class ServiceDiscoveryAction {
    ServiceFound,
    ServiceLost,
};

/// Try to install the host platform's default mDNS backend on `nsd`.
/// Returns true if a real backend was installed (Bonjour on Apple
/// platforms today; Avahi / WinRT are deferred — see the gap doc).
/// Returns false when the build was not compiled with a backend for
/// this OS, so callers can `install_backend(my_own)` and try again or
/// log a clear "no mDNS available" message instead of silently
/// browsing nothing.
bool install_default_backend(NetworkServiceDiscovery& nsd);

/// RAII service publisher. Builds an internal NetworkServiceDiscovery
/// configured with the platform's default backend, then registers the
/// requested service. Destructor unregisters and releases the backend.
///
/// `is_published()` returns true when the underlying backend accepted
/// the registration. On platforms with no backend it returns false
/// and callers can decide whether to surface the failure or proceed
/// without service publication.
class ServicePublisher {
public:
    ServicePublisher(std::string_view name,
                     std::string_view type,
                     uint16_t port,
                     NetworkServiceDiscovery::TxtRecords txt_records = {});
    ~ServicePublisher();

    ServicePublisher(const ServicePublisher&) = delete;
    ServicePublisher& operator=(const ServicePublisher&) = delete;

    bool is_published() const noexcept { return published_; }

    /// Access the underlying NSD object — for tests that want to
    /// install a fake backend instead of the platform default.
    NetworkServiceDiscovery& nsd() noexcept { return nsd_; }

    /// Construct a publisher that uses a caller-supplied backend
    /// instead of the platform default. Primarily for tests.
    static std::unique_ptr<ServicePublisher> with_backend(
        std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
        std::string_view name,
        std::string_view type,
        uint16_t port,
        NetworkServiceDiscovery::TxtRecords txt_records = {});

private:
    struct WithBackendTag {};
    ServicePublisher(WithBackendTag,
                     std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
                     std::string_view name,
                     std::string_view type,
                     uint16_t port,
                     NetworkServiceDiscovery::TxtRecords txt_records);

    NetworkServiceDiscovery nsd_;
    bool published_ = false;
};

/// RAII service browser. Builds an internal NetworkServiceDiscovery
/// configured with the platform's default backend, then starts a
/// browse for `service_type`. Destructor stops browsing.
///
/// The callback receives `ServiceFound` and `ServiceLost` events for
/// every (re)discovery and disappearance the backend reports.
class ServiceBrowser {
public:
    using Callback = std::function<void(ServiceDiscoveryAction,
                                        const NetworkServiceDiscovery::Service&)>;

    ServiceBrowser(std::string_view service_type, Callback callback);
    ~ServiceBrowser();

    ServiceBrowser(const ServiceBrowser&) = delete;
    ServiceBrowser& operator=(const ServiceBrowser&) = delete;

    bool is_browsing() const noexcept { return has_backend_; }

    NetworkServiceDiscovery& nsd() noexcept { return nsd_; }

    /// Construct a browser that uses a caller-supplied backend
    /// instead of the platform default. Primarily for tests.
    static std::unique_ptr<ServiceBrowser> with_backend(
        std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
        std::string_view service_type,
        Callback callback);

private:
    struct WithBackendTag {};
    ServiceBrowser(WithBackendTag,
                   std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
                   std::string_view service_type,
                   Callback callback);

    void wire_callbacks(Callback callback);

    NetworkServiceDiscovery nsd_;
    bool has_backend_ = false;
};

}  // namespace pulp::events
