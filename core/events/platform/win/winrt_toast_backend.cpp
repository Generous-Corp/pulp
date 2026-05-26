// Windows toast notification backend for PushNotifications.
//
// Posture: scaffold-only.
//
// Real Windows toast posting goes through the WinRT
// `Windows.UI.Notifications.ToastNotificationManager` API surface,
// which requires the calling process to either:
//   * Be packaged via MSIX with a manifest declaring a toast-capable
//     activatable class, OR
//   * Register a transient COM activator + AUMID at runtime
//     (`SHGetPropertyStoreForWindow` + `IApplicationActivationManager`).
//
// Both of those impose significant packaging requirements on plugin
// hosts that simply embed Pulp as a library, so the gap-doc scoping
// for this slice is *local notifications only* with the Windows path
// landing as a runtime-detected scaffold:
//
//   * At process start we attempt to LoadLibrary `combase.dll`. If the
//     OS resolves it, we register a backend that reports
//     `is_available() == false` (no real toast surface yet) but with
//     `backend_id() == "winrt-toast-scaffold"` so the gap-doc audit
//     can confirm the platform was probed.
//   * Calling `post_local_notification` returns 0 and the gap-doc row
//     stays "Partial" until the full COM-activator integration ships.
//
// This keeps the Windows build green without lying about delivered
// notifications, and gives the follow-up work a clearly-named
// extension point.

#include <pulp/events/push_notifications.hpp>

#if defined(_WIN32)

#include <pulp/runtime/dynamic_library.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace pulp::events {

void install_push_notifications_backend(std::unique_ptr<PushNotifications>);

namespace {

class WinRTToastScaffoldBackend final : public PushNotifications {
public:
    explicit WinRTToastScaffoldBackend(pulp::runtime::DynamicLibrary combase)
        : combase_(std::move(combase)) {}

    bool is_available() const override {
        // Scaffold-only: combase is loaded so the WinRT activator path
        // CAN be wired up, but the full COM activation + AUMID flow is
        // deferred. Until that ships, report unavailable so callers
        // don't believe they posted a real notification.
        return false;
    }

    std::string backend_id() const override { return "winrt-toast-scaffold"; }

    void request_authorization(AuthorizationCallback callback) override {
        if (callback) callback(NotificationAuthorization::Unsupported);
    }

    NotificationAuthorization current_authorization() const override {
        return NotificationAuthorization::Unsupported;
    }

    uint64_t post_local_notification(const LocalNotification&) override { return 0; }

    void set_handler(NotificationHandler handler) override {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    bool cancel_notification(const std::string&) override { return false; }

    void cancel_all() override {}

private:
    pulp::runtime::DynamicLibrary combase_;
    std::mutex mutex_;
    NotificationHandler handler_;
};

struct WinRTBootstrap {
    WinRTBootstrap() {
        pulp::runtime::DynamicLibrary combase;
        if (!combase.open("combase.dll")) return;
        install_push_notifications_backend(
            std::make_unique<WinRTToastScaffoldBackend>(std::move(combase)));
    }
};

const WinRTBootstrap kBootstrap;

} // namespace
} // namespace pulp::events

#endif // _WIN32
