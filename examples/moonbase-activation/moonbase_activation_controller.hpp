// MoonbaseActivationController — a thin, Pulp-native router over the Moonbase
// licensing SDK. It owns UI screen state, the activation request handle, and an
// audio-thread-safe `std::atomic<bool> licensed`. It deliberately does NOT
// re-implement Moonbase's cross-process store locking, online-validation grace
// period, or stale-write protection: those live in moonbase::licensing /
// moonbase::file_license_store and are called as-is. Keeping this a router (not
// a re-implementation) is what makes the integration low-maintenance — upstream
// owns the hard parts.
//
// Threading: all UI/screen state (screen_, license_, pending_) is touched ONLY on
// the host/UI thread — begin_online_activation/poll_once/deactivate/start*/pump.
// The audio thread only ever reads licensed(). start_async() additionally runs a
// ONE-SHOT background worker that calls the Moonbase SDK's online validation; to
// avoid two threads calling into the shared `licensing_`/store at once, the
// controller refuses UI actions that touch the SDK while a revalidation is in
// flight (revalidating()) — the worker thus has exclusive SDK access for its
// (timeout-bounded) lifetime, and pump() re-opens actions once it has joined.

#pragma once

#include <moonbase/moonbase.hpp>

#include "moonbase_pulp_transport.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace moonbase_pulp {

// The User-Agent client token. Mirrors the Moonbase JUCE module's
// `moonbase-juce/...` so Moonbase can attribute Pulp-originated traffic; the
// GenerousCorp/Pulp identity rides in the parenthetical. This is the single
// source of truth — change it here only.
inline std::string client_info(const std::string& integration_version = "1.0.0")
{
#if defined(__APPLE__)
    constexpr const char* os = "macOS";
#elif defined(_WIN32)
    constexpr const char* os = "Windows";
#elif defined(__linux__)
    constexpr const char* os = "Linux";
#else
    constexpr const char* os = "Unknown";
#endif
    return "moonbase-pulp/" + integration_version + " (GenerousCorp Pulp; " + os + ")";
}

struct ActivationConfig {
    std::string endpoint;     // e.g. "https://your-tenant.moonbase.sh"
    std::string product_id;   // your Moonbase product id
    std::string public_key;   // your product's RS256 public key (PEM)
    std::string license_path; // where to persist the validated license
    std::string integration_version = "1.0.0";
};

// Build licensing_options with the moonbase-pulp User-Agent contract applied.
inline moonbase::licensing_options make_options(const ActivationConfig& config)
{
    moonbase::licensing_options options;
    options.endpoint = config.endpoint;
    options.product_id = config.product_id;
    options.public_key = config.public_key;
    options.client_info = client_info(config.integration_version);
    return options;
}

class MoonbaseActivationController {
public:
    enum class Screen {
        Loading,      // validating any stored license at startup
        Welcome,      // not activated — offer online activation
        BrowserWait,  // browser activation in progress, polling
        Success,      // just activated (full license)
        Trial,        // a valid trial license is loaded
        Details,      // a valid full license is loaded
        Expired,      // a trial license that has ended; plugin locked
        Error         // an operation failed; status_message() has detail
    };

    // Production path: builds the real dependencies (Pulp HTTP transport over
    // cpp-httplib, on-disk license store, native fingerprint provider).
    explicit MoonbaseActivationController(ActivationConfig config)
        : config_(std::move(config))
    {
        auto store = std::make_shared<moonbase::file_license_store>(config_.license_path);
        auto transport = std::make_shared<PulpMoonbaseHttpTransport>();
        licensing_ = std::make_shared<moonbase::licensing>(
            make_options(config_), std::move(store), /*fingerprints*/ nullptr,
            std::move(transport));
    }

    // Test seam: drive the state machine against an injected licensing built
    // with a fake transport / store / fingerprint.
    MoonbaseActivationController(ActivationConfig config,
                                std::shared_ptr<moonbase::licensing> licensing)
        : config_(std::move(config)), licensing_(std::move(licensing))
    {
    }

    // Joins any in-flight online-revalidation worker so it never outlives the
    // controller (and the licensing it borrows).
    ~MoonbaseActivationController() { join_revalidate(); }

    MoonbaseActivationController(const MoonbaseActivationController&) = delete;
    MoonbaseActivationController& operator=(const MoonbaseActivationController&) = delete;

    // Called when the host opens the editor: validate any stored license and
    // route to the right screen. Network failures inside Moonbase's grace
    // period keep a previously-valid license usable (upstream semantics).
    void start()
    {
        screen_ = Screen::Loading;
        try {
            if (auto stored = licensing_->store().load_local_license()) {
                try {
                    apply_license(licensing_->validate_token_online(stored->token));
                    return;
                } catch (const std::exception&) {
                    // Couldn't refresh online — fall back to the local view of
                    // the stored token so an offline user isn't locked out.
                    apply_license(*stored);
                    return;
                }
            }
        } catch (const std::exception& ex) {
            set_error(ex.what());
            return;
        }
        screen_ = Screen::Welcome;
    }

    // Non-blocking startup (the recommended editor path). Applies any stored
    // license SYNCHRONOUSLY from its last-validated local view — so the editor
    // opens unlocked instantly without a network round-trip — then re-validates
    // against the Moonbase API on a one-shot background thread. The fresh result
    // is applied on the UI thread by pump() (call it from the editor's frame
    // tick). The audio thread only ever sees the licensed() atomic. This mirrors
    // the synchronous-local-then-async-online startup of Moonbase's own JUCE
    // reference flow, without blocking the editor's open.
    void start_async()
    {
        join_revalidate();
        revalidate_ready_.store(false, std::memory_order_relaxed);
        screen_ = Screen::Loading;
        std::optional<moonbase::license> stored;
        try {
            stored = licensing_->store().load_local_license();
        } catch (const std::exception& ex) {
            set_error(ex.what());
            return;
        }
        if (!stored) {
            screen_ = Screen::Welcome;
            return;
        }
        apply_license(*stored);   // instant, local — UI thread

        const std::string token = stored->token;
        auto licensing = licensing_;  // keep alive for the worker
        // Refuse SDK-touching UI actions until pump() joins the worker, so the
        // worker has exclusive access to `licensing_`/store. Set BEFORE spawning
        // so no action can slip through between spawn and the store.
        revalidating_.store(true, std::memory_order_release);
        revalidate_thread_ = std::thread([this, licensing, token]() {
            std::optional<moonbase::license> fresh;
            bool ok = false;
            try {
                fresh = licensing->validate_token_online(token);
                ok = true;
            } catch (const std::exception&) {
                ok = false;  // offline / grace — pump() keeps the local license
            }
            {
                std::lock_guard<std::mutex> g(result_mutex_);
                revalidate_license_ = std::move(fresh);
                revalidate_ok_ = ok;
            }
            revalidate_ready_.store(true, std::memory_order_release);
        });
    }

    // Apply a completed background re-validation, if any. UI thread only — call
    // from the editor's frame tick alongside poll_once(). No-op until the worker
    // finishes; a network failure leaves the synchronously-applied local license
    // in place (upstream grace semantics).
    void pump()
    {
        if (!revalidate_ready_.load(std::memory_order_acquire)) return;
        revalidate_ready_.store(false, std::memory_order_relaxed);
        std::optional<moonbase::license> fresh;
        bool ok = false;
        {
            std::lock_guard<std::mutex> g(result_mutex_);
            fresh = std::move(revalidate_license_);
            ok = revalidate_ok_;
        }
        join_revalidate();
        if (ok && fresh) apply_license(*fresh);
        revalidating_.store(false, std::memory_order_relaxed);  // re-open UI actions
    }

    // True while a background re-validation holds the SDK; SDK-touching UI
    // actions are refused during this (timeout-bounded) window.
    [[nodiscard]] bool revalidating() const noexcept
    {
        return revalidating_.load(std::memory_order_relaxed);
    }

    // Request an activation and route the user to the browser. The returned
    // browser URL is surfaced via on_open_url (wire it to the platform opener)
    // and browser_url(); the host then polls poll_once().
    void begin_online_activation()
    {
        if (revalidating()) return;  // a background re-validation holds the SDK
        try {
            pending_ = licensing_->request_activation();
            browser_url_ = pending_->browser_url;
            screen_ = Screen::BrowserWait;
            status_message_.clear();
            if (on_open_url) on_open_url(browser_url_);
        } catch (const std::exception& ex) {
            set_error(ex.what());
        }
    }

    // Poll once for a fulfilled activation. Returns true once a license has been
    // obtained and applied. Safe no-op unless we're in BrowserWait.
    bool poll_once()
    {
        if (screen_ != Screen::BrowserWait || !pending_) return false;
        try {
            if (auto license = licensing_->get_requested_activation(*pending_)) {
                apply_license(*license);
                return true;
            }
        } catch (const std::exception& ex) {
            set_error(ex.what());
        }
        return false;
    }

    void cancel_activation()
    {
        pending_.reset();
        screen_ = license_ ? screen_for_license(*license_) : Screen::Welcome;
    }

    // Server-side revoke (best-effort), then forget locally.
    void deactivate()
    {
        if (revalidating()) return;  // a background re-validation holds the SDK
        if (license_) {
            try {
                licensing_->revoke_activation(license_->token);
            } catch (const std::exception&) {
                // Best-effort: a revoke failure must not strand the local UI.
            }
        }
        license_.reset();
        licensed_.store(false, std::memory_order_relaxed);
        screen_ = Screen::Welcome;
    }

    // Read an offline-activated license token (machine-file flow).
    void apply_offline_token(const std::string& token)
    {
        if (revalidating()) return;  // a background re-validation holds the SDK
        try {
            apply_license(licensing_->read_offline_license(token));
        } catch (const std::exception& ex) {
            set_error(ex.what());
        }
    }

    // Test / preview seam: force a screen (and optional license) with no
    // network — for unit tests, UI snapshots, and design iteration.
    void set_preview_state(Screen screen, std::optional<moonbase::license> license = std::nullopt)
    {
        screen_ = screen;
        if (license) {
            license_ = std::move(license);
            licensed_.store(true, std::memory_order_relaxed);
        }
    }

    // ── Accessors ────────────────────────────────────────────────────────────
    [[nodiscard]] Screen screen() const noexcept { return screen_; }
    [[nodiscard]] const std::optional<moonbase::license>& license() const noexcept { return license_; }
    // Audio-thread-safe gate. Read from process() without a lock.
    [[nodiscard]] const std::atomic<bool>& licensed() const noexcept { return licensed_; }
    [[nodiscard]] const std::string& browser_url() const noexcept { return browser_url_; }
    [[nodiscard]] const std::string& status_message() const noexcept { return status_message_; }
    [[nodiscard]] const ActivationConfig& config() const noexcept { return config_; }

    // Wire this to the platform browser opener (e.g. pulp platform URL open).
    std::function<void(const std::string& url)> on_open_url;

private:
    void apply_license(moonbase::license license)
    {
        try {
            licensing_->store().store_local_license(license);
        } catch (const std::exception&) {
            // Persistence is best-effort; we still hold a valid in-memory license.
        }
        screen_ = screen_for_license(license);
        license_ = std::move(license);
        licensed_.store(true, std::memory_order_relaxed);
        status_message_.clear();
    }

    static Screen screen_for_license(const moonbase::license& license)
    {
        return license.trial ? Screen::Trial : Screen::Details;
    }

    void set_error(std::string message)
    {
        status_message_ = std::move(message);
        screen_ = Screen::Error;
    }

    void join_revalidate()
    {
        if (revalidate_thread_.joinable()) revalidate_thread_.join();
    }

    ActivationConfig config_;
    std::shared_ptr<moonbase::licensing> licensing_;
    Screen screen_ = Screen::Loading;
    std::optional<moonbase::license> license_;
    std::atomic<bool> licensed_{false};
    std::optional<moonbase::activation_request> pending_;
    std::string browser_url_;
    std::string status_message_;

    // One-shot online re-validation worker (start_async → pump). The worker only
    // writes the guarded result slot + the ready flag; screen_/license_ are
    // touched solely on the UI thread (start_async/pump), so there is no data
    // race on UI state and the audio thread still only reads licensed_.
    std::thread revalidate_thread_;
    std::mutex result_mutex_;
    std::atomic<bool> revalidate_ready_{false};
    std::optional<moonbase::license> revalidate_license_;  // guarded by result_mutex_
    bool revalidate_ok_ = false;                           // guarded by result_mutex_
    std::atomic<bool> revalidating_{false};                // worker holds the SDK
};

} // namespace moonbase_pulp
