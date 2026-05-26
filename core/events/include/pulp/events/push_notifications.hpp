#pragma once

// pulp::events::PushNotifications — cross-platform local notification surface.
//
// Scope (this slice):
//   * Local notifications: `post_local_notification(title, body, category)`
//     posts a notification immediately through the host platform's user-
//     notification surface.
//   * Authorization: `request_authorization(callback)` asks the OS for
//     permission to post notifications and reports the user's decision.
//   * Tap handler: `set_handler(callback)` registers a callback that fires
//     when the user activates a delivered notification.
//
// Deferred (follow-up work, tracked in the gap doc):
//   * Remote / push notifications via APNs (Apple) and FCM (Android).
//   * Notification categories with custom action buttons.
//   * Scheduled / time-triggered notifications.
//   * Per-notification badge / sound / attachment customization.
//
// Backends (runtime-detected; build never hard-fails on a missing SDK):
//   * macOS / iOS: `UNUserNotificationCenter` from `UserNotifications.framework`.
//   * Linux: `libnotify.so.4` via runtime-dlopen.
//   * Windows: `Windows.UI.Notifications.ToastNotificationManager` via WinRT
//     activation (runtime-LoadLibrary `combase.dll`).
//   * Other platforms / build configurations: `is_available()` returns false
//     and `post_local_notification` reports a posted notification id of 0.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::events {

/// Outcome of `request_authorization()`.
enum class NotificationAuthorization {
    NotDetermined, ///< The OS has never been asked.
    Denied,        ///< The user (or policy) declined.
    Granted,       ///< The app may post notifications.
    Unsupported,   ///< No notification backend on this build.
};

/// Posted notification descriptor.
struct LocalNotification {
    std::string title;    ///< Bold first line.
    std::string body;     ///< Optional secondary message.
    std::string category; ///< Optional grouping / action category id.
    std::string id;       ///< Optional caller-supplied id; auto-assigned when empty.
};

/// Delivered to `set_handler()` when the user activates a notification.
struct NotificationActivation {
    std::string id;          ///< Matches `LocalNotification::id`.
    std::string category;    ///< Matches `LocalNotification::category`.
    std::string action_id;   ///< Empty for default activation.
};

using AuthorizationCallback = std::function<void(NotificationAuthorization)>;
using NotificationHandler = std::function<void(const NotificationActivation&)>;

/// Cross-platform local notification surface.
///
/// Use the singleton via `PushNotifications::instance()`. The implementation is
/// thread-safe: any thread may call `post_local_notification`, but the
/// `NotificationHandler` is delivered on a platform-defined dispatch thread —
/// callers must hop back to their own UI thread if the handler touches view state.
class PushNotifications {
public:
    static PushNotifications& instance();

    virtual ~PushNotifications() = default;

    /// Returns true when a real notification backend is wired up on this build.
    /// On `false`, `request_authorization` reports `Unsupported` and
    /// `post_local_notification` returns 0 without posting.
    virtual bool is_available() const = 0;

    /// Short identifier of the active backend ("user-notifications", "libnotify",
    /// "winrt-toast", or "none"). Useful for diagnostics + the gap-doc audit.
    virtual std::string backend_id() const = 0;

    /// Ask the OS for permission to post notifications. The callback fires once
    /// the platform has resolved the request (synchronously on backends that
    /// don't require an OS prompt, e.g. libnotify).
    virtual void request_authorization(AuthorizationCallback callback) = 0;

    /// Synchronous query for the current authorization state.
    virtual NotificationAuthorization current_authorization() const = 0;

    /// Post a notification. Returns the assigned id (`notification.id` if
    /// non-empty, otherwise an auto-generated id) or 0 on failure / unavailable.
    /// The id is also surfaced to `NotificationHandler` if the user activates it.
    virtual uint64_t post_local_notification(const LocalNotification& notification) = 0;

    /// Register a callback that fires when the user activates a posted
    /// notification. Pass `{}` to clear.
    virtual void set_handler(NotificationHandler handler) = 0;

    /// Cancel a previously-posted notification by id (no-op if already
    /// dismissed). Returns true if the backend reported the cancellation.
    virtual bool cancel_notification(const std::string& id) = 0;

    /// Cancel every notification this process has posted.
    virtual void cancel_all() = 0;

protected:
    PushNotifications() = default;
    PushNotifications(const PushNotifications&) = delete;
    PushNotifications& operator=(const PushNotifications&) = delete;
};

} // namespace pulp::events
