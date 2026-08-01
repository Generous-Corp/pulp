#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/audit.hpp>
#include <pulp/inspect/session.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

InspectorPolicyConfig test_input_policy(InspectorProfile profile) {
    InspectorPolicyConfig config;
    config.profile = profile;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::TestInput,
    };
    return config;
}

InspectorMessage midi_request(std::int64_t id) {
    return make_request(
        id, std::string(methods::kTestInjectMidi),
        R"({"kind":"note_on","channel":1,"note":60,"velocity":100,"secret":"not-dispatched"})");
}

} // namespace

TEST_CASE("mutating inspector audit is bounded, monotonic, exact, and redacted",
          "[inspect][audit][test-input]") {
    auto audit = std::make_shared<InspectorAuditLog>(2);
    InspectorSession session({"session-audit", "instance-audit", "fixture"},
                             test_input_policy(InspectorProfile::Develop),
                             [](const InspectorMessage& request) {
                                 return make_response(request.id, R"({"accepted":true})");
                             });
    session.set_audit_log(audit);

    auto denied = session.handle("controller-a", midi_request(1));
    REQUIRE(denied.is_error);
    CHECK(denied.error_code == "controller_lease_required");

    REQUIRE_FALSE(session
                      .handle("controller-a",
                              make_request(2, std::string(methods::kSessionAcquireController)))
                      .is_error);
    REQUIRE_FALSE(session.handle("controller-a", midi_request(3)).is_error);
    REQUIRE_FALSE(session.handle("controller-a", midi_request(4)).is_error);

    const auto entries = audit->snapshot();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].sequence + 1 == entries[1].sequence);
    CHECK(entries[0].request_id == 3);
    CHECK(entries[1].request_id == 4);
    for (const auto& entry : entries) {
        CHECK(entry.session_id == "session-audit");
        CHECK(entry.instance_id == "instance-audit");
        CHECK(entry.client_id == "controller-a");
        CHECK(entry.method == methods::kTestInjectMidi);
        CHECK(entry.capability == InspectorCapability::TestInput);
        CHECK(entry.outcome == InspectorAuditOutcome::Applied);
        CHECK(entry.error_code.empty());
        CHECK(entry.method.find("secret") == std::string::npos);
    }
}

TEST_CASE("observe-only test input denial is audited before dispatch", "[inspect][audit][policy]") {
    int dispatches = 0;
    auto audit = std::make_shared<InspectorAuditLog>();
    InspectorSession session({"session-observe", "instance-observe", "fixture"},
                             test_input_policy(InspectorProfile::Observe),
                             [&](const InspectorMessage& request) {
                                 ++dispatches;
                                 return make_response(request.id, R"({"accepted":true})");
                             });
    session.set_audit_log(audit);

    const auto response = session.handle("observer", midi_request(1));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "capability_denied");
    CHECK(dispatches == 0);

    const auto entries = audit->snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].outcome == InspectorAuditOutcome::Denied);
    CHECK(entries[0].error_code == "capability_denied");
    CHECK(entries[0].client_id == "observer");
}

TEST_CASE("test input rejection records the host outcome without payload data",
          "[inspect][audit][errors]") {
    auto audit = std::make_shared<InspectorAuditLog>();
    InspectorSession session(
        {"session-reject", "instance-reject", "fixture"},
        test_input_policy(InspectorProfile::Develop), [](const InspectorMessage& request) {
            return make_error(request.id, "The bounded input queue is full", "input_queue_full");
        });
    session.set_audit_log(audit);
    REQUIRE_FALSE(
        session.handle("writer", make_request(1, std::string(methods::kSessionAcquireController)))
            .is_error);
    const auto response = session.handle("writer", midi_request(2));
    REQUIRE(response.is_error);

    const auto entries = audit->snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].outcome == InspectorAuditOutcome::Rejected);
    CHECK(entries[0].error_code == "input_queue_full");
}

TEST_CASE("controller scope cleanup identifies release, disconnect, expiry, and teardown",
          "[inspect][session][test-input][lifecycle]") {
    auto now = std::chrono::steady_clock::time_point{};
    InspectorSession session(
        {"session-life", "instance-life", "fixture"}, test_input_policy(InspectorProfile::Develop),
        [](const InspectorMessage& request) {
            return make_response(request.id, R"({"accepted":true})");
        },
        100ms, [&] { return now; });
    std::vector<InspectorControllerScopeEnd> ended;
    session.set_controller_scope_end_handler(
        [&](const InspectorControllerScopeEnd& event) { ended.push_back(event); });

    REQUIRE_FALSE(session
                      .handle("release-owner",
                              make_request(1, std::string(methods::kSessionAcquireController)))
                      .is_error);
    REQUIRE_FALSE(session
                      .handle("release-owner",
                              make_request(2, std::string(methods::kSessionReleaseController)))
                      .is_error);

    REQUIRE_FALSE(session
                      .handle("disconnect-owner",
                              make_request(3, std::string(methods::kSessionAcquireController)))
                      .is_error);
    session.disconnect("disconnect-owner");

    REQUIRE_FALSE(session
                      .handle("expired-owner",
                              make_request(4, std::string(methods::kSessionAcquireController)))
                      .is_error);
    now += 101ms;
    const auto expired = session.handle("expired-owner", midi_request(5));
    REQUIRE(expired.is_error);
    CHECK(expired.error_code == "controller_lease_required");

    REQUIRE_FALSE(session
                      .handle("teardown-owner",
                              make_request(6, std::string(methods::kSessionAcquireController)))
                      .is_error);
    session.suspend_dispatches();

    REQUIRE(ended.size() == 4);
    CHECK(ended[0].client_id == "release-owner");
    CHECK(ended[0].reason == TestInputReleaseReason::ControllerReleased);
    CHECK(ended[1].client_id == "disconnect-owner");
    CHECK(ended[1].reason == TestInputReleaseReason::ClientDisconnected);
    CHECK(ended[2].client_id == "expired-owner");
    CHECK(ended[2].reason == TestInputReleaseReason::ControllerExpired);
    CHECK(ended[3].client_id == "teardown-owner");
    CHECK(ended[3].reason == TestInputReleaseReason::SessionTeardown);
    for (const auto& event : ended) {
        CHECK(event.session_id == "session-life");
        CHECK(event.instance_id == "instance-life");
    }
}

TEST_CASE("controller lease expiry wakes cleanup without another request",
          "[inspect][session][test-input][expiry]") {
    InspectorSession session(
        {"session-idle-expiry", "instance-idle-expiry", "fixture"},
        test_input_policy(InspectorProfile::Develop),
        [](const InspectorMessage& request) {
            return make_response(request.id, R"({"accepted":true})");
        },
        20ms);
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<InspectorControllerScopeEnd> ended;
    session.set_controller_scope_end_handler([&](const InspectorControllerScopeEnd& event) {
        {
            std::lock_guard lock(mutex);
            ended.push_back(event);
        }
        cv.notify_all();
    });

    REQUIRE_FALSE(
        session
            .handle("idle-owner", make_request(1, std::string(methods::kSessionAcquireController)))
            .is_error);

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, 1s, [&] { return ended.size() == 1; }));
    CHECK(ended[0].client_id == "idle-owner");
    CHECK(ended[0].reason == TestInputReleaseReason::ControllerExpired);
}

TEST_CASE("controller cleanup callback exceptions are contained",
          "[inspect][session][test-input][lifecycle]") {
    InspectorSession session({"session-callback", "instance-callback", "fixture"},
                             test_input_policy(InspectorProfile::Develop),
                             [](const InspectorMessage& request) {
                                 return make_response(request.id, R"({"accepted":true})");
                             });
    session.set_controller_scope_end_handler([](const InspectorControllerScopeEnd&) {
        throw std::runtime_error("fixture cleanup failure");
    });
    REQUIRE_FALSE(
        session.handle("first", make_request(1, std::string(methods::kSessionAcquireController)))
            .is_error);
    REQUIRE_FALSE(
        session.handle("first", make_request(2, std::string(methods::kSessionReleaseController)))
            .is_error);
    CHECK_FALSE(
        session.handle("second", make_request(3, std::string(methods::kSessionAcquireController)))
            .is_error);
}
