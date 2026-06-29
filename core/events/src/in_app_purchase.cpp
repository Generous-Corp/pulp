// IapClient — cross-platform in-app purchase dispatcher.
//
// This translation unit owns:
//   * The shared `IapClient::instance()` accessor.
//   * A `StubBackend` used when no host backend is installed
//     so callers never crash on `instance().purchase(...)`.
//
// Host or test backends register themselves through `install_iap_backend()`.
// Pulp intentionally does not link a production StoreKit / Microsoft Store /
// Play Billing backend, so the built-in path falls through to the stub.
//
// The stub deliberately keeps state machinery minimal — it doesn't
// queue products or pretend to grant entitlements. Callers see
// `Unavailable` everywhere so a host application can branch on
// `is_available()` once and skip the IAP UI entirely on unsupported
// builds.

#include <pulp/events/in_app_purchase.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::events {
namespace {

class StubBackend final : public IapClient {
public:
    bool is_available() const override { return false; }

    std::string backend_id() const override { return "none"; }

    void request_products(const std::vector<std::string>& skus,
                          ProductLookupCallback callback) override {
        if (!callback) return;
        ProductLookupResult result;
        result.status = ProductLookupStatus::Unavailable;
        result.unknown_skus = skus;
        result.error = "No IAP backend is wired into this build.";
        callback(result);
    }

    void purchase(const std::string& sku, PurchaseCallback callback) override {
        if (!callback) return;
        Purchase p;
        p.sku = sku;
        p.state = PurchaseState::Unavailable;
        p.error = "No IAP backend is wired into this build.";
        callback(p);
    }

    void restore(RestoreCallback callback) override {
        if (callback) callback({});
    }

    void set_observer(PurchaseObserver) override {}

    bool finish_transaction(const std::string&) override { return false; }
};

std::mutex& instance_mutex() {
    static std::mutex mu;
    return mu;
}

std::unique_ptr<IapClient>& instance_slot() {
    static std::unique_ptr<IapClient> slot;
    return slot;
}

} // namespace

// Backends register themselves by calling this from a translation-unit
// initializer (or a test installs a mock). Last writer wins so a host
// application can override the default stub with a test double.
void install_iap_backend(std::unique_ptr<IapClient> backend) {
    std::lock_guard lock(instance_mutex());
    instance_slot() = std::move(backend);
}

IapClient& IapClient::instance() {
    std::lock_guard lock(instance_mutex());
    auto& slot = instance_slot();
    if (!slot) {
        slot = std::make_unique<StubBackend>();
    }
    return *slot;
}

} // namespace pulp::events
