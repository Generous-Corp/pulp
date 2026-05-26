// PushNotifications cross-platform smoke + headless-mock coverage.
//
// What's verified here (works on every CI platform without poking the
// host OS for real notification permissions):
//   * Singleton returns a usable backend whose `backend_id()` matches
//     one of the documented identifiers.
//   * The default stub returns false from `is_available()` on builds
//     without a real backend, and `request_authorization` reports
//     `Unsupported` synchronously.
//   * A test-double backend installed through the same registration
//     hook real backends use can intercept posts, cancellations,
//     handler installation, and authorization flow end-to-end.

#include <pulp/events/push_notifications.hpp>

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
// Defined in src/push_notifications.cpp — exposed here so tests can
// install a mock backend in place of the stub / real platform backend.
void install_push_notifications_backend(std::unique_ptr<PushNotifications>);
} // namespace pulp::events

namespace {

using namespace pulp::events;

class MockBackend final : public PushNotifications {
public:
    bool is_available() const override { return true; }

    std::string backend_id() const override { return "test-mock"; }

    void request_authorization(AuthorizationCallback callback) override {
        authorization_ = NotificationAuthorization::Granted;
        if (callback) callback(authorization_);
    }

    NotificationAuthorization current_authorization() const override {
        return authorization_;
    }

    uint64_t post_local_notification(const LocalNotification& notification) override {
        std::lock_guard lock(mutex_);
        const auto id = next_id_++;
        const auto string_id = notification.id.empty()
                                   ? "mock-" + std::to_string(id)
                                   : notification.id;
        posts_[string_id] = notification;
        last_id_ = string_id;
        return id;
    }

    void set_handler(NotificationHandler handler) override {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    bool cancel_notification(const std::string& id) override {
        std::lock_guard lock(mutex_);
        return posts_.erase(id) > 0;
    }

    void cancel_all() override {
        std::lock_guard lock(mutex_);
        posts_.clear();
    }

    void simulate_activation(const NotificationActivation& a) {
        NotificationHandler handler;
        {
            std::lock_guard lock(mutex_);
            handler = handler_;
        }
        if (handler) handler(a);
    }

    std::size_t posted_count() const {
        std::lock_guard lock(mutex_);
        return posts_.size();
    }

    std::optional<LocalNotification> last_post() const {
        std::lock_guard lock(mutex_);
        if (last_id_.empty()) return std::nullopt;
        auto it = posts_.find(last_id_);
        if (it == posts_.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LocalNotification> posts_;
    NotificationHandler handler_;
    NotificationAuthorization authorization_{NotificationAuthorization::NotDetermined};
    uint64_t next_id_{1};
    std::string last_id_;
};

// Restore the default backend after each test so we don't leak state
// between cases. The stub is the safe default.
struct ScopedDefaultBackend {
    ~ScopedDefaultBackend() {
        install_push_notifications_backend(nullptr);
    }
};

} // namespace

TEST_CASE("PushNotifications singleton is always available", "[push-notifications]") {
    auto& notifications = PushNotifications::instance();
    REQUIRE_FALSE(notifications.backend_id().empty());

    // Documented backend identifiers — keep this list synced with the
    // header comment in push_notifications.hpp.
    const std::vector<std::string> known = {
        "none",
        "user-notifications",
        "libnotify",
        "winrt-toast-scaffold",
        "test-mock",
    };
    bool matched = false;
    for (const auto& id : known) {
        if (notifications.backend_id() == id) {
            matched = true;
            break;
        }
    }
    REQUIRE(matched);
}

TEST_CASE("PushNotifications stub backend reports unavailable", "[push-notifications]") {
    ScopedDefaultBackend scope;
    install_push_notifications_backend(nullptr); // force re-init to stub

    auto& notifications = PushNotifications::instance();
    if (notifications.backend_id() != "none") {
        // Real platform backend got installed via static initializer —
        // skip the stub-specific assertions. The mock-backend case below
        // still covers the API contract end-to-end.
        SUCCEED("Platform backend installed; stub path not exercised here.");
        return;
    }

    REQUIRE_FALSE(notifications.is_available());

    NotificationAuthorization observed = NotificationAuthorization::Granted;
    notifications.request_authorization([&](NotificationAuthorization a) { observed = a; });
    REQUIRE(observed == NotificationAuthorization::Unsupported);
    REQUIRE(notifications.current_authorization() == NotificationAuthorization::Unsupported);

    LocalNotification notif{"title", "body", "general", "stub-1"};
    REQUIRE(notifications.post_local_notification(notif) == 0);
    REQUIRE_FALSE(notifications.cancel_notification("stub-1"));
}

TEST_CASE("PushNotifications mock backend end-to-end", "[push-notifications]") {
    ScopedDefaultBackend scope;
    auto mock = std::make_unique<MockBackend>();
    MockBackend* mock_ptr = mock.get();
    install_push_notifications_backend(std::move(mock));

    auto& notifications = PushNotifications::instance();
    REQUIRE(notifications.is_available());
    REQUIRE(notifications.backend_id() == "test-mock");

    SECTION("authorization flow") {
        bool fired = false;
        NotificationAuthorization seen = NotificationAuthorization::NotDetermined;
        notifications.request_authorization([&](NotificationAuthorization a) {
            fired = true;
            seen = a;
        });
        REQUIRE(fired);
        REQUIRE(seen == NotificationAuthorization::Granted);
        REQUIRE(notifications.current_authorization() == NotificationAuthorization::Granted);
    }

    SECTION("post + cancel round-trip") {
        const auto id = notifications.post_local_notification({"hi", "world", "general", "abc"});
        REQUIRE(id > 0);
        REQUIRE(mock_ptr->posted_count() == 1);
        REQUIRE(mock_ptr->last_post()->title == "hi");
        REQUIRE(mock_ptr->last_post()->body == "world");
        REQUIRE(notifications.cancel_notification("abc"));
        REQUIRE(mock_ptr->posted_count() == 0);
    }

    SECTION("auto-generated id when caller omits one") {
        const auto id = notifications.post_local_notification({"a", "b", "", ""});
        REQUIRE(id > 0);
        REQUIRE(mock_ptr->posted_count() == 1);
    }

    SECTION("handler fires on simulated activation") {
        std::atomic<bool> fired{false};
        NotificationActivation seen;
        notifications.set_handler([&](const NotificationActivation& a) {
            fired = true;
            seen = a;
        });

        mock_ptr->simulate_activation({"tap-1", "general", ""});
        REQUIRE(fired.load());
        REQUIRE(seen.id == "tap-1");
        REQUIRE(seen.category == "general");
    }

    SECTION("cancel_all drains backend storage") {
        notifications.post_local_notification({"x", "", "", "x1"});
        notifications.post_local_notification({"y", "", "", "x2"});
        REQUIRE(mock_ptr->posted_count() == 2);
        notifications.cancel_all();
        REQUIRE(mock_ptr->posted_count() == 0);
    }
}

TEST_CASE("PushNotifications nullptr install restores stub", "[push-notifications]") {
    install_push_notifications_backend(std::make_unique<MockBackend>());
    REQUIRE(PushNotifications::instance().backend_id() == "test-mock");
    install_push_notifications_backend(nullptr);
    // Next access re-initializes a fresh stub (or whatever the platform
    // backend bootstrap would install, but during this test run only the
    // stub fallback is exercised because we cleared the slot).
    REQUIRE(PushNotifications::instance().backend_id() == "none");
}
