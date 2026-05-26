#include <pulp/events/service_discovery.hpp>

#include <memory>
#include <utility>

namespace pulp::events {

#if defined(__APPLE__)
// Implemented in platform/mac/bonjour_backend.cpp.
std::unique_ptr<NetworkServiceDiscovery::Backend> make_bonjour_backend();
#elif defined(__linux__)
// Implemented in platform/linux/avahi_backend.cpp.
// Returns nullptr when libavahi-client.so.3 isn't installed; caller
// degrades to the "no mDNS available" path.
std::unique_ptr<NetworkServiceDiscovery::Backend> make_avahi_backend();
#elif defined(_WIN32)
// Implemented in platform/win/bonjour_backend.cpp.
// Returns nullptr when dnssd.dll isn't installed; caller degrades to
// the "no mDNS available" path.
std::unique_ptr<NetworkServiceDiscovery::Backend> make_windows_bonjour_backend();
#endif

bool install_default_backend(NetworkServiceDiscovery& nsd) {
#if defined(__APPLE__)
    auto backend = make_bonjour_backend();
    if (!backend) return false;
    nsd.install_backend(std::move(backend));
    return true;
#elif defined(__linux__)
    // Avahi backend uses runtime dlopen of libavahi-client.so.3. When
    // the daemon isn't installed (minimal containers, dev workstations
    // without avahi-daemon) we honestly report no backend rather than
    // silently faking discovery.
    auto backend = make_avahi_backend();
    if (!backend) return false;
    nsd.install_backend(std::move(backend));
    return true;
#elif defined(_WIN32)
    // Bonjour SDK for Windows ships `dnssd.dll`; we resolve it at
    // runtime. Returns false on stock Windows boxes that have never
    // had the SDK installed — same honest contract as Linux without
    // avahi-daemon.
    auto backend = make_windows_bonjour_backend();
    if (!backend) return false;
    nsd.install_backend(std::move(backend));
    return true;
#else
    (void)nsd;
    return false;
#endif
}

// ── ServicePublisher ─────────────────────────────────────────────────

ServicePublisher::ServicePublisher(std::string_view name,
                                   std::string_view type,
                                   uint16_t port,
                                   NetworkServiceDiscovery::TxtRecords txt_records) {
    if (install_default_backend(nsd_)) {
        published_ = nsd_.register_service(name, type, port, txt_records);
    }
}

ServicePublisher::ServicePublisher(WithBackendTag,
                                   std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
                                   std::string_view name,
                                   std::string_view type,
                                   uint16_t port,
                                   NetworkServiceDiscovery::TxtRecords txt_records) {
    if (backend) {
        nsd_.install_backend(std::move(backend));
        published_ = nsd_.register_service(name, type, port, txt_records);
    }
}

ServicePublisher::~ServicePublisher() {
    if (published_) {
        nsd_.unregister_service();
    }
}

std::unique_ptr<ServicePublisher> ServicePublisher::with_backend(
    std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
    std::string_view name,
    std::string_view type,
    uint16_t port,
    NetworkServiceDiscovery::TxtRecords txt_records) {
    return std::unique_ptr<ServicePublisher>(new ServicePublisher(
        WithBackendTag{}, std::move(backend), name, type, port, std::move(txt_records)));
}

// ── ServiceBrowser ───────────────────────────────────────────────────

ServiceBrowser::ServiceBrowser(std::string_view service_type, Callback callback) {
    if (install_default_backend(nsd_)) {
        has_backend_ = true;
        wire_callbacks(std::move(callback));
        nsd_.browse(service_type);
    }
}

ServiceBrowser::ServiceBrowser(WithBackendTag,
                               std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
                               std::string_view service_type,
                               Callback callback) {
    if (backend) {
        nsd_.install_backend(std::move(backend));
        has_backend_ = true;
        wire_callbacks(std::move(callback));
        nsd_.browse(service_type);
    }
}

ServiceBrowser::~ServiceBrowser() {
    if (has_backend_) {
        nsd_.stop();
    }
}

void ServiceBrowser::wire_callbacks(Callback callback) {
    // Share ownership of the callback between the found/lost handlers
    // so the lambda payload is held alive for the full lifetime of
    // the browser, even if one of the std::function slots is later
    // overwritten by external code on the underlying NSD.
    auto shared = std::make_shared<Callback>(std::move(callback));
    nsd_.on_service_found = [shared](const NetworkServiceDiscovery::Service& s) {
        if (*shared) (*shared)(ServiceDiscoveryAction::ServiceFound, s);
    };
    nsd_.on_service_lost = [shared](const NetworkServiceDiscovery::Service& s) {
        if (*shared) (*shared)(ServiceDiscoveryAction::ServiceLost, s);
    };
}

std::unique_ptr<ServiceBrowser> ServiceBrowser::with_backend(
    std::unique_ptr<NetworkServiceDiscovery::Backend> backend,
    std::string_view service_type,
    Callback callback) {
    return std::unique_ptr<ServiceBrowser>(new ServiceBrowser(
        WithBackendTag{}, std::move(backend), service_type, std::move(callback)));
}

}  // namespace pulp::events
