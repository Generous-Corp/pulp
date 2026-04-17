#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/permissions.hpp>

#include <atomic>

using pulp::platform::Permission;
using pulp::platform::PermissionState;
using pulp::platform::PermissionsOverride;
using pulp::platform::has_platform_backend;
using pulp::platform::query;
using pulp::platform::request;

TEST_CASE("query returns a concrete state for every permission class",
          "[platform][permissions]") {
    for (auto p : {
             Permission::Microphone,
             Permission::Camera,
             Permission::BluetoothMidi,
             Permission::LocalNetwork,
             Permission::Notifications,
             Permission::BackgroundAudio,
             Permission::ForegroundService,
         }) {
        auto state = query(p);
        // Never NotDetermined on the desktop no-op backend — callers
        // must be able to make a decision without prompting.
        if (!has_platform_backend()) {
            REQUIRE(state != PermissionState::NotDetermined);
        }
    }
}

TEST_CASE("desktop backend reports mobile-only classes as Restricted",
          "[platform][permissions]") {
    if (has_platform_backend()) {
        SUCCEED("platform backend active; skipping desktop default assertions");
        return;
    }
    REQUIRE(query(Permission::BackgroundAudio) == PermissionState::Restricted);
    REQUIRE(query(Permission::ForegroundService) == PermissionState::Restricted);
}

TEST_CASE("desktop backend reports generic classes as Granted",
          "[platform][permissions]") {
    if (has_platform_backend()) {
        SUCCEED("platform backend active; skipping desktop default assertions");
        return;
    }
    REQUIRE(query(Permission::Microphone) == PermissionState::Granted);
    REQUIRE(query(Permission::Camera) == PermissionState::Granted);
    REQUIRE(query(Permission::BluetoothMidi) == PermissionState::Granted);
    REQUIRE(query(Permission::LocalNetwork) == PermissionState::Granted);
    REQUIRE(query(Permission::Notifications) == PermissionState::Granted);
}

TEST_CASE("request() invokes the callback exactly once and forwards query state",
          "[platform][permissions]") {
    std::atomic<int> calls{0};
    PermissionState seen = PermissionState::NotDetermined;
    request(Permission::Microphone, [&](PermissionState s) {
        ++calls;
        seen = s;
    });
    REQUIRE(calls.load() == 1);
    REQUIRE(seen == query(Permission::Microphone));
}

TEST_CASE("request() tolerates an empty callback",
          "[platform][permissions]") {
    // Must not crash or invoke UB when the caller doesn't need a result.
    request(Permission::Microphone, {});
    SUCCEED();
}

TEST_CASE("PermissionsOverride redirects query and request",
          "[platform][permissions][override]") {
    PermissionsOverride guard;
    guard.set(Permission::Microphone, PermissionState::Denied);
    guard.set(Permission::Camera, PermissionState::NotDetermined);

    REQUIRE(query(Permission::Microphone) == PermissionState::Denied);
    REQUIRE(query(Permission::Camera) == PermissionState::NotDetermined);

    PermissionState seen_mic = PermissionState::Granted;
    request(Permission::Microphone, [&](PermissionState s) { seen_mic = s; });
    REQUIRE(seen_mic == PermissionState::Denied);

    // Un-overridden permissions keep falling through to the real backend.
    if (!has_platform_backend()) {
        REQUIRE(query(Permission::Notifications) == PermissionState::Granted);
    }
}

TEST_CASE("PermissionsOverride::clear removes one entry without tearing down others",
          "[platform][permissions][override]") {
    PermissionsOverride guard;
    guard.set(Permission::Microphone, PermissionState::Denied);
    guard.set(Permission::Camera, PermissionState::Denied);

    guard.clear(Permission::Microphone);
    if (!has_platform_backend()) {
        REQUIRE(query(Permission::Microphone) == PermissionState::Granted);
    }
    REQUIRE(query(Permission::Camera) == PermissionState::Denied);
}

TEST_CASE("PermissionsOverride unwinds state when guard leaves scope",
          "[platform][permissions][override]") {
    {
        PermissionsOverride guard;
        guard.set(Permission::Microphone, PermissionState::Denied);
        REQUIRE(query(Permission::Microphone) == PermissionState::Denied);
    }
    if (!has_platform_backend()) {
        REQUIRE(query(Permission::Microphone) == PermissionState::Granted);
    }
}

TEST_CASE("PermissionsOverride guards nest — outer state restores after inner exits",
          "[platform][permissions][override]") {
    PermissionsOverride outer;
    outer.set(Permission::Microphone, PermissionState::Denied);
    REQUIRE(query(Permission::Microphone) == PermissionState::Denied);
    {
        PermissionsOverride inner;
        inner.set(Permission::Microphone, PermissionState::Granted);
        REQUIRE(query(Permission::Microphone) == PermissionState::Granted);
    }
    // Inner guard popped — outer "Denied" must still hold.
    REQUIRE(query(Permission::Microphone) == PermissionState::Denied);
}

TEST_CASE("has_platform_backend reports truthfully for the current target",
          "[platform][permissions]") {
#if defined(PULP_PERMISSIONS_HAS_BACKEND)
    REQUIRE(has_platform_backend());
#else
    REQUIRE_FALSE(has_platform_backend());
#endif
}
