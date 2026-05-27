// IapClient cross-platform smoke + headless-mock coverage.
//
// What's verified here (works on every CI platform without poking the
// host OS for a real StoreKit / WinRT / Play Billing flow):
//   * Singleton returns a usable backend whose `backend_id()` matches
//     one of the documented identifiers.
//   * The default stub returns false from `is_available()` on builds
//     without a real backend, and every operation reports `Unavailable`
//     synchronously without charging.
//   * A test-double backend installed through the same registration
//     hook real backends use can intercept product lookups, purchases,
//     restore, observer notification, and finish_transaction end-to-end.

#include <pulp/events/in_app_purchase.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::events {
// Defined in src/in_app_purchase.cpp — exposed here so tests can install
// a mock backend in place of the stub / real platform backend.
void install_iap_backend(std::unique_ptr<IapClient>);
} // namespace pulp::events

namespace {

using namespace pulp::events;

class MockBackend final : public IapClient {
public:
    bool is_available() const override { return true; }

    std::string backend_id() const override { return "test-mock"; }

    void seed_product(const Product& product) {
        std::lock_guard lock(mutex_);
        catalog_[product.sku] = product;
    }

    void request_products(const std::vector<std::string>& skus,
                          ProductLookupCallback callback) override {
        ProductLookupResult result;
        {
            std::lock_guard lock(mutex_);
            for (const auto& sku : skus) {
                auto it = catalog_.find(sku);
                if (it == catalog_.end()) {
                    result.unknown_skus.push_back(sku);
                } else {
                    result.products.push_back(it->second);
                }
            }
        }
        if (result.unknown_skus.empty()) {
            result.status = ProductLookupStatus::Ok;
        } else if (!result.products.empty()) {
            result.status = ProductLookupStatus::PartiallyOk;
        } else {
            result.status = ProductLookupStatus::Failed;
            result.error = "No requested SKUs were found in the mock catalog.";
        }
        if (callback) callback(result);
    }

    void purchase(const std::string& sku, PurchaseCallback callback) override {
        Purchase p;
        p.sku = sku;
        p.transaction_id = "mock-tx-" + std::to_string(next_tx_++);
        p.receipt = "mock-receipt-bytes";
        bool known = false;
        {
            std::lock_guard lock(mutex_);
            known = catalog_.find(sku) != catalog_.end();
            if (known) {
                p.state = PurchaseState::Purchased;
                pending_[p.transaction_id] = p;
            } else {
                p.state = PurchaseState::Failed;
                p.error = "SKU not in mock catalog.";
            }
        }
        if (callback) callback(p);
        notify_observer(p);
    }

    void restore(RestoreCallback callback) override {
        std::vector<Purchase> snapshot;
        {
            std::lock_guard lock(mutex_);
            snapshot.reserve(pending_.size());
            for (auto& [tx, purchase] : pending_) {
                Purchase r = purchase;
                r.state = PurchaseState::Restored;
                snapshot.push_back(r);
            }
        }
        for (const auto& r : snapshot) {
            notify_observer(r);
        }
        if (callback) callback(snapshot);
    }

    void set_observer(PurchaseObserver observer) override {
        std::lock_guard lock(mutex_);
        observer_ = std::move(observer);
    }

    bool finish_transaction(const std::string& transaction_id) override {
        std::lock_guard lock(mutex_);
        return pending_.erase(transaction_id) > 0;
    }

    // Push an externally-resolved transaction through the observer (mimics
    // out-of-band StoreKit notifications and subscription auto-renewals).
    void simulate_external_purchase(const Purchase& purchase) {
        notify_observer(purchase);
    }

    std::size_t pending_count() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    void notify_observer(const Purchase& purchase) {
        PurchaseObserver observer;
        {
            std::lock_guard lock(mutex_);
            observer = observer_;
        }
        if (observer) observer(purchase);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Product> catalog_;
    std::unordered_map<std::string, Purchase> pending_;
    PurchaseObserver observer_;
    uint64_t next_tx_{1};
};

// Restore the default backend after each test so we don't leak state
// between cases. The stub is the safe default.
struct ScopedDefaultBackend {
    ~ScopedDefaultBackend() { install_iap_backend(nullptr); }
};

} // namespace

TEST_CASE("IapClient singleton is always available", "[in-app-purchase]") {
    auto& iap = IapClient::instance();
    REQUIRE_FALSE(iap.backend_id().empty());

    // Documented backend identifiers — keep this list synced with the
    // header comment in in_app_purchase.hpp.
    const std::vector<std::string> known = {
        "none",
        "storekit2",
        "winrt-store",
        "test-mock",
    };
    bool matched = false;
    for (const auto& id : known) {
        if (iap.backend_id() == id) {
            matched = true;
            break;
        }
    }
    REQUIRE(matched);
}

TEST_CASE("IapClient stub backend reports unavailable", "[in-app-purchase]") {
    ScopedDefaultBackend scope;
    install_iap_backend(nullptr); // force re-init to stub

    auto& iap = IapClient::instance();
    if (iap.backend_id() != "none") {
        // Real platform backend got installed via static initializer —
        // skip the stub-specific assertions. The mock-backend case below
        // still covers the API contract end-to-end.
        SUCCEED("Platform backend installed; stub path not exercised here.");
        return;
    }

    REQUIRE_FALSE(iap.is_available());

    ProductLookupResult product_result;
    iap.request_products({"sku.a", "sku.b"}, [&](const ProductLookupResult& r) {
        product_result = r;
    });
    REQUIRE(product_result.status == ProductLookupStatus::Unavailable);
    REQUIRE(product_result.products.empty());
    REQUIRE(product_result.unknown_skus.size() == 2);

    Purchase purchase_result;
    iap.purchase("sku.a", [&](const Purchase& p) { purchase_result = p; });
    REQUIRE(purchase_result.sku == "sku.a");
    REQUIRE(purchase_result.state == PurchaseState::Unavailable);
    REQUIRE_FALSE(purchase_result.error.empty());

    std::vector<Purchase> restored = {Purchase{"placeholder", PurchaseState::Purchased, "", "", ""}};
    iap.restore([&](const std::vector<Purchase>& r) { restored = r; });
    REQUIRE(restored.empty());

    REQUIRE_FALSE(iap.finish_transaction("any-tx"));
}

TEST_CASE("IapClient mock backend end-to-end", "[in-app-purchase]") {
    ScopedDefaultBackend scope;
    auto mock = std::make_unique<MockBackend>();
    MockBackend* mock_ptr = mock.get();
    mock_ptr->seed_product(Product{
        /*sku=*/"com.example.tip", /*title=*/"Small Tip",
        /*description=*/"Buy us a coffee.", /*price_formatted=*/"$0.99",
        /*price_currency_code=*/"USD", /*price_amount=*/0.99,
        /*is_subscription=*/false});
    install_iap_backend(std::move(mock));

    auto& iap = IapClient::instance();
    REQUIRE(iap.is_available());
    REQUIRE(iap.backend_id() == "test-mock");

    SECTION("product lookup resolves known SKUs and reports unknowns") {
        ProductLookupResult result;
        iap.request_products({"com.example.tip", "com.example.missing"},
                             [&](const ProductLookupResult& r) { result = r; });
        REQUIRE(result.status == ProductLookupStatus::PartiallyOk);
        REQUIRE(result.products.size() == 1);
        REQUIRE(result.products[0].sku == "com.example.tip");
        REQUIRE(result.products[0].price_currency_code == "USD");
        REQUIRE(result.unknown_skus == std::vector<std::string>{"com.example.missing"});
    }

    SECTION("purchase round-trip + observer fires + finish drops pending") {
        std::atomic<int> observer_calls{0};
        Purchase last_observed;
        iap.set_observer([&](const Purchase& p) {
            observer_calls++;
            last_observed = p;
        });

        Purchase purchase_seen;
        iap.purchase("com.example.tip", [&](const Purchase& p) { purchase_seen = p; });
        REQUIRE(purchase_seen.state == PurchaseState::Purchased);
        REQUIRE_FALSE(purchase_seen.transaction_id.empty());
        REQUIRE(observer_calls.load() == 1);
        REQUIRE(last_observed.transaction_id == purchase_seen.transaction_id);
        REQUIRE(mock_ptr->pending_count() == 1);

        REQUIRE(iap.finish_transaction(purchase_seen.transaction_id));
        REQUIRE(mock_ptr->pending_count() == 0);
        REQUIRE_FALSE(iap.finish_transaction(purchase_seen.transaction_id));
    }

    SECTION("purchase of unknown SKU reports Failed") {
        Purchase result;
        iap.purchase("not.in.catalog", [&](const Purchase& p) { result = p; });
        REQUIRE(result.state == PurchaseState::Failed);
        REQUIRE_FALSE(result.error.empty());
    }

    SECTION("restore replays pending purchases through callback + observer") {
        iap.purchase("com.example.tip", [](const Purchase&) {});

        std::atomic<int> observer_calls{0};
        iap.set_observer([&](const Purchase&) { observer_calls++; });

        std::vector<Purchase> restored;
        iap.restore([&](const std::vector<Purchase>& r) { restored = r; });
        REQUIRE(restored.size() == 1);
        REQUIRE(restored[0].state == PurchaseState::Restored);
        REQUIRE(observer_calls.load() == 1);
    }

    SECTION("simulated external purchase reaches observer") {
        std::atomic<bool> fired{false};
        Purchase seen;
        iap.set_observer([&](const Purchase& p) {
            fired = true;
            seen = p;
        });

        Purchase incoming;
        incoming.sku = "com.example.tip";
        incoming.transaction_id = "external-tx-42";
        incoming.state = PurchaseState::Purchased;
        mock_ptr->simulate_external_purchase(incoming);

        REQUIRE(fired.load());
        REQUIRE(seen.transaction_id == "external-tx-42");
    }
}

TEST_CASE("IapClient nullptr install restores stub", "[in-app-purchase]") {
    install_iap_backend(std::make_unique<MockBackend>());
    REQUIRE(IapClient::instance().backend_id() == "test-mock");
    install_iap_backend(nullptr);
    // Next access re-initializes a fresh stub (or whatever the platform
    // backend bootstrap would install, but during this test run only the
    // stub fallback is exercised because we cleared the slot).
    REQUIRE(IapClient::instance().backend_id() == "none");
}
