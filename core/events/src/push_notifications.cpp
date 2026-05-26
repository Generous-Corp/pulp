// PushNotifications — cross-platform local notification dispatcher.
//
// This translation unit owns:
//   * The shared `PushNotifications::instance()` accessor.
//   * Helper plumbing common to every backend (id assignment, handler
//     storage, thread-safety).
//   * A `StubBackend` used when no platform backend is wired into the
//     build, so callers never crash on `instance().post_local_notification`.
//
// Platform backends register themselves through `install_backend()` from
// their own translation units (mac/ios/win/linux). They are linked
// conditionally by the events CMakeLists.txt so a build on an OS without
// a backend simply falls through to the stub.

#include <pulp/events/push_notifications.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace pulp::events {
namespace {

class StubBackend final : public PushNotifications {
public:
    bool is_available() const override { return false; }

    std::string backend_id() const override { return "none"; }

    void request_authorization(AuthorizationCallback callback) override {
        if (callback) callback(NotificationAuthorization::Unsupported);
    }

    NotificationAuthorization current_authorization() const override {
        return NotificationAuthorization::Unsupported;
    }

    uint64_t post_local_notification(const LocalNotification&) override { return 0; }

    void set_handler(NotificationHandler) override {}

    bool cancel_notification(const std::string&) override { return false; }

    void cancel_all() override {}
};

std::mutex& instance_mutex() {
    static std::mutex mu;
    return mu;
}

std::unique_ptr<PushNotifications>& instance_slot() {
    static std::unique_ptr<PushNotifications> slot;
    return slot;
}

} // namespace

// Backends register themselves by calling this from a translation-unit
// initializer. Last writer wins so a host application can override the
// default platform backend with a test double.
void install_push_notifications_backend(std::unique_ptr<PushNotifications> backend) {
    std::lock_guard lock(instance_mutex());
    instance_slot() = std::move(backend);
}

PushNotifications& PushNotifications::instance() {
    std::lock_guard lock(instance_mutex());
    auto& slot = instance_slot();
    if (!slot) {
        slot = std::make_unique<StubBackend>();
    }
    return *slot;
}

} // namespace pulp::events
