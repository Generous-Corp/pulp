#pragma once

// pulp::events::IapClient — cross-platform in-app purchase surface.
//
// Scope (this slice):
//   * Product lookup: `request_products(skus, callback)` resolves an SKU list
//     into `Product` records (title / description / localized price).
//   * Purchase: `purchase(sku, callback)` initiates a purchase flow and
//     reports the resulting `Purchase` (state + transaction id + receipt).
//   * Restore: `restore(callback)` re-issues previously-completed purchases
//     so the user can recover entitlements on a new device.
//   * Observer: `set_observer(callback)` registers a single sink that fires
//     whenever the backend resolves a purchase asynchronously (out-of-band
//     transactions, subscription renewals, family-sharing grants).
//
// Deferred (follow-up work, tracked in the gap doc):
//   * Real StoreKit2 wiring on Apple platforms (sandbox round-trip).
//   * Microsoft Store SDK wiring on Windows (currently a runtime-dlopen
//     scaffold that reports `Unavailable`).
//   * Google Play Billing on Android.
//   * Server-side receipt validation helpers.
//   * Subscription-specific surfaces (auto-renew status, grace periods,
//     promotional offers).
//
// Backends (runtime-detected; build never hard-fails on a missing SDK):
//   * macOS / iOS: `StoreKit` (StoreKit 2 when available, StoreKit 1 fallback).
//     This slice ships the interface + a stub that returns `Unavailable`;
//     the real wiring lives in a follow-up so the build stays MIT-clean and
//     doesn't pull in any non-public Apple framework headers at configure
//     time.
//   * Linux: no IAP surface; `is_available()` returns false.
//   * Windows: `Windows.Services.Store.StoreContext` via WinRT activation
//     (runtime-LoadLibrary `combase.dll`). Scaffolded only — a real
//     purchase flow requires MSIX packaging or a transient activator.
//   * Other platforms / build configurations: `is_available()` returns
//     false and every call reports `Unavailable` / empty result.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::events {

/// Lifecycle state of a `Purchase` returned by the backend.
enum class PurchaseState {
    Unknown,        ///< Backend hasn't resolved the transaction yet.
    Purchasing,     ///< User has been prompted; payment in flight.
    Purchased,      ///< Payment cleared; entitlement should be granted.
    Restored,       ///< Re-issued via `restore()`; entitlement should be granted.
    Failed,         ///< Backend reported an error (see `Purchase::error`).
    Cancelled,      ///< User cancelled the purchase prompt.
    Deferred,       ///< Awaiting external action (Ask-to-Buy / parental approval).
    Unavailable,    ///< No IAP backend on this build / SKU not configured.
};

/// Outcome of a product lookup.
enum class ProductLookupStatus {
    Ok,             ///< All requested SKUs resolved.
    PartiallyOk,    ///< Some SKUs resolved; `unknown_skus` lists the misses.
    Failed,         ///< Backend reported an error before any lookup.
    Unavailable,    ///< No IAP backend on this build.
};

/// Resolved product metadata.
struct Product {
    std::string sku;                 ///< Caller-supplied identifier.
    std::string title;               ///< Localized product title.
    std::string description;         ///< Localized long description.
    std::string price_formatted;     ///< Localized price string ("$0.99", "€0,99").
    std::string price_currency_code; ///< ISO 4217 (e.g. "USD").
    double price_amount = 0.0;       ///< Numeric price in `price_currency_code` units.
    bool is_subscription = false;    ///< True for auto-renewing subscriptions.
};

/// Outcome of a single product lookup invocation.
struct ProductLookupResult {
    ProductLookupStatus status = ProductLookupStatus::Unavailable;
    std::vector<Product> products;
    std::vector<std::string> unknown_skus; ///< SKUs the backend didn't recognize.
    std::string error;                     ///< Empty on Ok / PartiallyOk.
};

/// Single transaction record.
struct Purchase {
    std::string sku;            ///< Matches the SKU passed to `purchase()`.
    PurchaseState state = PurchaseState::Unknown;
    std::string transaction_id; ///< Backend-assigned transaction identifier.
    std::string receipt;        ///< Opaque receipt bytes (StoreKit / WinRT / etc.).
    std::string error;          ///< Empty unless `state == Failed`.
};

using ProductLookupCallback = std::function<void(const ProductLookupResult&)>;
using PurchaseCallback = std::function<void(const Purchase&)>;
using RestoreCallback = std::function<void(const std::vector<Purchase>&)>;
using PurchaseObserver = std::function<void(const Purchase&)>;

/// Cross-platform in-app purchase surface.
///
/// Use the singleton via `IapClient::instance()`. The implementation is
/// thread-safe: any thread may invoke any method, but callbacks are
/// dispatched on a platform-defined thread (typically a StoreKit /
/// WinRT delegate queue) — callers must hop back to their own UI
/// thread if the callback touches view state.
class IapClient {
public:
    static IapClient& instance();

    virtual ~IapClient() = default;

    /// Returns true when a real IAP backend is wired up on this build.
    /// On `false`, every method reports `Unavailable` and never charges.
    virtual bool is_available() const = 0;

    /// Short identifier of the active backend ("storekit2", "winrt-store",
    /// "test-mock", or "none"). Useful for diagnostics + the gap-doc audit.
    virtual std::string backend_id() const = 0;

    /// Ask the backend for metadata + localized pricing of the given SKUs.
    /// The callback fires once the backend has resolved the request (or
    /// synchronously on the stub when no backend is wired).
    virtual void request_products(const std::vector<std::string>& skus,
                                  ProductLookupCallback callback) = 0;

    /// Initiate a purchase flow for `sku`. The callback fires once the
    /// transaction resolves; the same `Purchase` is also reported to the
    /// observer (if one is installed) so receipt-validation code can live
    /// in a single place regardless of which entry point started the flow.
    virtual void purchase(const std::string& sku, PurchaseCallback callback) = 0;

    /// Ask the backend to re-issue every previously-completed purchase.
    /// Used on a fresh install / new device to recover entitlements.
    virtual void restore(RestoreCallback callback) = 0;

    /// Register a callback that fires for any backend-resolved purchase
    /// (including out-of-band transactions and subscription renewals).
    /// Pass `{}` to clear. Single-slot — last writer wins, matching the
    /// `PushNotifications::set_handler` contract.
    virtual void set_observer(PurchaseObserver observer) = 0;

    /// Acknowledge / finish a purchase so the backend stops re-delivering
    /// it. Required by StoreKit and Play Billing once entitlement has been
    /// granted. Returns true if the backend recognized the transaction id.
    virtual bool finish_transaction(const std::string& transaction_id) = 0;

protected:
    IapClient() = default;
    IapClient(const IapClient&) = delete;
    IapClient& operator=(const IapClient&) = delete;
};

} // namespace pulp::events
