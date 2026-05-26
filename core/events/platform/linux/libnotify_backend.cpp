// Linux libnotify backend for PushNotifications.
//
// Same posture as Pulp's other runtime-resolved Linux backends (avahi,
// dl_shim wrappers): the build never links libnotify at compile time.
// At process start we try to `dlopen("libnotify.so.4", RTLD_LAZY)` and
// resolve the symbols we need; on success we register a real backend,
// on failure we leave the stub in place so the build remains green on
// minimal images where libnotify is not installed.
//
// libnotify is LGPL-2.1+ — calling it through dlsym is the same
// arms-length boundary every distro package manager already relies on.
// We never copy upstream headers; only the public function signatures
// (uncopyrightable interface facts).

#include <pulp/events/push_notifications.hpp>

#if defined(__linux__)

#include <pulp/runtime/dynamic_library.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::events {

void install_push_notifications_backend(std::unique_ptr<PushNotifications>);

namespace {

// libnotify C API surface (minimal mirror).
struct NotifyNotificationOpaque;
using NotifyInitFn = int (*)(const char* app_name);
using NotifyIsInitedFn = int (*)(void);
using NotifyNotificationNewFn = NotifyNotificationOpaque* (*)(const char* summary,
                                                              const char* body,
                                                              const char* icon);
using NotifyNotificationShowFn = int (*)(NotifyNotificationOpaque*, void**);
using NotifyNotificationCloseFn = int (*)(NotifyNotificationOpaque*, void**);
using GObjectUnrefFn = void (*)(void*);

class LibNotifyBackend final : public PushNotifications {
public:
    LibNotifyBackend(pulp::runtime::DynamicLibrary lib,
                     pulp::runtime::DynamicLibrary gobject,
                     NotifyInitFn init,
                     NotifyNotificationNewFn make,
                     NotifyNotificationShowFn show,
                     NotifyNotificationCloseFn close,
                     GObjectUnrefFn unref)
        : lib_(std::move(lib)),
          gobject_(std::move(gobject)),
          init_(init),
          make_(make),
          show_(show),
          close_(close),
          unref_(unref) {
        init_("pulp");
    }

    ~LibNotifyBackend() override {
        std::lock_guard lock(mutex_);
        for (auto& [id, handle] : live_) {
            close_(handle, nullptr);
            unref_(handle);
        }
        live_.clear();
    }

    bool is_available() const override { return true; }

    std::string backend_id() const override { return "libnotify"; }

    void request_authorization(AuthorizationCallback callback) override {
        // libnotify has no per-process authorization gate — the user's
        // session-level notification settings apply. Report Granted so
        // callers proceed with `post_local_notification`.
        if (callback) callback(NotificationAuthorization::Granted);
    }

    NotificationAuthorization current_authorization() const override {
        return NotificationAuthorization::Granted;
    }

    uint64_t post_local_notification(const LocalNotification& notification) override {
        auto* handle = make_(notification.title.c_str(),
                             notification.body.empty() ? nullptr : notification.body.c_str(),
                             nullptr);
        if (!handle) return 0;

        if (!show_(handle, nullptr)) {
            unref_(handle);
            return 0;
        }

        std::lock_guard lock(mutex_);
        const auto numeric_id = next_id_.fetch_add(1, std::memory_order_relaxed);
        const auto string_id = notification.id.empty()
                                   ? "pulp-notif-" + std::to_string(numeric_id)
                                   : notification.id;
        live_.emplace(string_id, handle);
        return numeric_id;
    }

    void set_handler(NotificationHandler handler) override {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    bool cancel_notification(const std::string& id) override {
        std::lock_guard lock(mutex_);
        auto it = live_.find(id);
        if (it == live_.end()) return false;
        close_(it->second, nullptr);
        unref_(it->second);
        live_.erase(it);
        return true;
    }

    void cancel_all() override {
        std::lock_guard lock(mutex_);
        for (auto& [id, handle] : live_) {
            close_(handle, nullptr);
            unref_(handle);
        }
        live_.clear();
    }

private:
    pulp::runtime::DynamicLibrary lib_;
    pulp::runtime::DynamicLibrary gobject_;
    NotifyInitFn init_;
    NotifyNotificationNewFn make_;
    NotifyNotificationShowFn show_;
    NotifyNotificationCloseFn close_;
    GObjectUnrefFn unref_;
    std::mutex mutex_;
    std::unordered_map<std::string, NotifyNotificationOpaque*> live_;
    std::atomic<uint64_t> next_id_{1};
    NotificationHandler handler_;
};

struct LibNotifyBootstrap {
    LibNotifyBootstrap() {
        pulp::runtime::DynamicLibrary lib;
        if (!lib.open("libnotify.so.4") && !lib.open("libnotify.so")) return;

        pulp::runtime::DynamicLibrary gobject;
        if (!gobject.open("libgobject-2.0.so.0") && !gobject.open("libgobject-2.0.so")) return;

        auto init = reinterpret_cast<NotifyInitFn>(lib.find_symbol("notify_init"));
        auto make = reinterpret_cast<NotifyNotificationNewFn>(lib.find_symbol("notify_notification_new"));
        auto show = reinterpret_cast<NotifyNotificationShowFn>(lib.find_symbol("notify_notification_show"));
        auto close = reinterpret_cast<NotifyNotificationCloseFn>(lib.find_symbol("notify_notification_close"));
        auto unref = reinterpret_cast<GObjectUnrefFn>(gobject.find_symbol("g_object_unref"));
        if (!init || !make || !show || !close || !unref) return;

        install_push_notifications_backend(std::make_unique<LibNotifyBackend>(
            std::move(lib), std::move(gobject), init, make, show, close, unref));
    }
};

const LibNotifyBootstrap kBootstrap;

} // namespace
} // namespace pulp::events

#endif // __linux__
