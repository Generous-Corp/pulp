// macOS / iOS PushNotifications backend.
//
// Wraps `UNUserNotificationCenter` from `UserNotifications.framework`.
// Local-only in this slice — remote push (APNs registration tokens,
// `application:didRegisterForRemoteNotificationsWithDeviceToken:`,
// silent push) is deferred until a real APNs integration ships.
//
// The backend is registered through a translation-unit-static
// initializer so plugins that link `pulp::events` automatically pick
// up the macOS path without per-call wiring. Plugin sandboxes that
// cannot use UserNotifications (older AU hosts) still work because
// the framework calls themselves no-op when the host process is not
// authorized; the gap-doc row stays "local-only" until remote shipping.

#include <pulp/events/push_notifications.hpp>

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace pulp::events {

void install_push_notifications_backend(std::unique_ptr<PushNotifications>);

namespace {

NSString* nsstring_or_nil(const std::string& s) {
    if (s.empty()) return nil;
    return [NSString stringWithUTF8String:s.c_str()];
}

class UserNotificationsBackend final : public PushNotifications {
public:
    UserNotificationsBackend() = default;

    bool is_available() const override {
        return [UNUserNotificationCenter currentNotificationCenter] != nil;
    }

    std::string backend_id() const override { return "user-notifications"; }

    void request_authorization(AuthorizationCallback callback) override {
        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center) {
            if (callback) callback(NotificationAuthorization::Unsupported);
            return;
        }

        UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                      UNAuthorizationOptionSound |
                                      UNAuthorizationOptionBadge;

        [center requestAuthorizationWithOptions:opts
                              completionHandler:^(BOOL granted, NSError* /*error*/) {
            const auto outcome = granted ? NotificationAuthorization::Granted
                                         : NotificationAuthorization::Denied;
            cached_authorization_.store(static_cast<int>(outcome),
                                        std::memory_order_release);
            if (callback) callback(outcome);
        }];
    }

    NotificationAuthorization current_authorization() const override {
        const auto raw = cached_authorization_.load(std::memory_order_acquire);
        return static_cast<NotificationAuthorization>(raw);
    }

    uint64_t post_local_notification(const LocalNotification& notification) override {
        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center) return 0;

        UNMutableNotificationContent* content =
            [[UNMutableNotificationContent alloc] init];
        content.title = nsstring_or_nil(notification.title) ?: @"";
        content.body = nsstring_or_nil(notification.body) ?: @"";
        if (!notification.category.empty()) {
            content.categoryIdentifier = nsstring_or_nil(notification.category);
        }

        const auto numeric_id = next_id_.fetch_add(1, std::memory_order_relaxed);
        const auto string_id = notification.id.empty()
                                   ? std::string("pulp-notif-") + std::to_string(numeric_id)
                                   : notification.id;

        UNNotificationRequest* request = [UNNotificationRequest
            requestWithIdentifier:[NSString stringWithUTF8String:string_id.c_str()]
                          content:content
                          trigger:nil];

        {
            std::lock_guard lock(mutex_);
            posted_ids_.insert(string_id);
        }

        [center addNotificationRequest:request withCompletionHandler:nil];
        return numeric_id;
    }

    void set_handler(NotificationHandler handler) override {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    bool cancel_notification(const std::string& id) override {
        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center) return false;

        bool tracked = false;
        {
            std::lock_guard lock(mutex_);
            tracked = posted_ids_.erase(id) > 0;
        }

        NSString* ns_id = [NSString stringWithUTF8String:id.c_str()];
        [center removePendingNotificationRequestsWithIdentifiers:@[ns_id]];
        [center removeDeliveredNotificationsWithIdentifiers:@[ns_id]];
        return tracked;
    }

    void cancel_all() override {
        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center) return;
        [center removeAllPendingNotificationRequests];
        [center removeAllDeliveredNotifications];

        std::lock_guard lock(mutex_);
        posted_ids_.clear();
    }

private:
    std::mutex mutex_;
    NotificationHandler handler_;
    std::unordered_set<std::string> posted_ids_;
    std::atomic<uint64_t> next_id_{1};
    std::atomic<int> cached_authorization_{
        static_cast<int>(NotificationAuthorization::NotDetermined)};
};

struct UserNotificationsBootstrap {
    UserNotificationsBootstrap() {
        install_push_notifications_backend(
            std::make_unique<UserNotificationsBackend>());
    }
};

const UserNotificationsBootstrap kBootstrap;

} // namespace
} // namespace pulp::events

#endif // __APPLE__
