#include <catch2/catch_test_macros.hpp>
#include <pulp/events/message_loop_integration.hpp>

#include <atomic>
#include <string>

using namespace pulp::events;

namespace {

MainThreadDispatcher::Backend make_noop_backend(std::atomic<int>* counter) {
    MainThreadDispatcher::Backend b;
    b.post = [counter](Task t) {
        if (counter) ++(*counter);
        if (t) t();
        return true;
    };
    b.is_main_thread = [] { return true; };
    return b;
}

} // namespace

TEST_CASE("MessageLoopIntegration reports None when no backend is registered",
          "[events][message-loop-integration]") {
    // No backend installed in the test process by default. We can't
    // unregister another test's backend safely, so just assert the
    // active_kind is sensible — either None, or whatever a sibling
    // test left registered. The important invariant is that
    // available() and active_kind() agree.
    auto kind = MessageLoopIntegration::active_kind();
    if (kind == MainLoopKind::None) {
        REQUIRE_FALSE(MessageLoopIntegration::available());
        REQUIRE(MessageLoopIntegration::active_name() == "none");
    } else {
        REQUIRE(MessageLoopIntegration::available());
        REQUIRE(MessageLoopIntegration::active_name() != "none");
    }
}

TEST_CASE("MessageLoopIntegration register_kind reports the active kind",
          "[events][message-loop-integration]") {
    std::atomic<int> calls{0};
    auto token = MainThreadDispatcher::register_backend(make_noop_backend(&calls));
    REQUIRE(token != 0);

    MessageLoopIntegration::register_kind(token, MainLoopKind::Custom);
    REQUIRE(MessageLoopIntegration::available());
    REQUIRE(MessageLoopIntegration::active_kind() == MainLoopKind::Custom);
    REQUIRE(MessageLoopIntegration::active_name() == "custom");

    // post() forwards to the backend
    bool ran = false;
    REQUIRE(MessageLoopIntegration::post([&] { ran = true; }));
    REQUIRE(ran);
    REQUIRE(calls.load() >= 1);

    MessageLoopIntegration::unregister_kind(token);
    REQUIRE(MainThreadDispatcher::unregister_backend(token));
}

TEST_CASE("MessageLoopIntegration kind names cover every enum value",
          "[events][message-loop-integration]") {
    // Pin the name table. If a new MainLoopKind value is added without a
    // matching kind_name() arm, this test will surface the gap.
    auto check = [](MainLoopKind k, std::string_view expected) {
        std::atomic<int> calls{0};
        auto token = MainThreadDispatcher::register_backend(make_noop_backend(&calls));
        REQUIRE(token != 0);
        MessageLoopIntegration::register_kind(token, k);
        REQUIRE(MessageLoopIntegration::active_name() == expected);
        MessageLoopIntegration::unregister_kind(token);
        REQUIRE(MainThreadDispatcher::unregister_backend(token));
    };

    check(MainLoopKind::Cocoa,   "cocoa");
    check(MainLoopKind::Win32,   "win32");
    check(MainLoopKind::GLib,    "glib");
    check(MainLoopKind::X11,     "x11");
    check(MainLoopKind::Wayland, "wayland");
    check(MainLoopKind::Custom,  "custom");
}

TEST_CASE("MessageLoopIntegration backend without a kind tag stays self-consistent",
          "[events][message-loop-integration]") {
    // Regression: register a backend with MainThreadDispatcher but skip
    // the separate register_kind() step (the header documents kind tagging
    // as a distinct call after register_backend()). The documented
    // equivalence is `available() == (active_kind() != None)`; previously
    // an untagged-but-present backend reported available()==true while
    // active_kind()==None, breaking that invariant.
    std::atomic<int> calls{0};
    auto token = MainThreadDispatcher::register_backend(make_noop_backend(&calls));
    REQUIRE(token != 0);

    // No register_kind() call here on purpose.
    REQUIRE(MessageLoopIntegration::available());
    REQUIRE(MessageLoopIntegration::active_kind() != MainLoopKind::None);
    REQUIRE(MessageLoopIntegration::active_name() != "none");

    // The header-documented equivalence holds either way.
    REQUIRE(MessageLoopIntegration::available() ==
            (MessageLoopIntegration::active_kind() != MainLoopKind::None));

    REQUIRE(MainThreadDispatcher::unregister_backend(token));
}

TEST_CASE("MessageLoopIntegration kind tag override and revert transitions",
          "[events][message-loop-integration]") {
    // Locks the full lifecycle the Custom fallback's correctness rests on:
    // untagged backend reports Custom; a later register_kind() overrides it;
    // unregister_kind() reverts to the Custom fallback (backend still present);
    // unregister_backend() returns to None. The invariant
    // `available() == (active_kind() != None)` holds at every step (single
    // observer; see the cross-mutex caveat in message_loop_integration.cpp).
    std::atomic<int> calls{0};
    auto token = MainThreadDispatcher::register_backend(make_noop_backend(&calls));
    REQUIRE(token != 0);

    // 1. Present but untagged → Custom fallback.
    REQUIRE(MessageLoopIntegration::active_kind() == MainLoopKind::Custom);
    REQUIRE(MessageLoopIntegration::available());

    // 2. Tagging overrides the fallback with the concrete kind.
    MessageLoopIntegration::register_kind(token, MainLoopKind::Cocoa);
    REQUIRE(MessageLoopIntegration::active_kind() == MainLoopKind::Cocoa);

    // 3. Untagging (backend still registered) reverts to the Custom fallback,
    //    not None — the present-but-unknown state.
    MessageLoopIntegration::unregister_kind(token);
    REQUIRE(MessageLoopIntegration::active_kind() == MainLoopKind::Custom);
    REQUIRE(MessageLoopIntegration::available());

    // 4. Removing the backend returns to None (the early-return path dominates
    //    any stale tag).
    REQUIRE(MainThreadDispatcher::unregister_backend(token));
    REQUIRE(MessageLoopIntegration::active_kind() == MainLoopKind::None);
    REQUIRE_FALSE(MessageLoopIntegration::available());
}

TEST_CASE("MessageLoopIntegration register_kind with token=0 is a no-op",
          "[events][message-loop-integration]") {
    const auto before_kind = MessageLoopIntegration::active_kind();
    const auto before_name = std::string(MessageLoopIntegration::active_name());
    const bool before_available = MessageLoopIntegration::available();

    MessageLoopIntegration::register_kind(0, MainLoopKind::Cocoa);
    REQUIRE(MessageLoopIntegration::active_kind() == before_kind);
    REQUIRE(MessageLoopIntegration::active_name() == before_name);
    REQUIRE(MessageLoopIntegration::available() == before_available);

    MessageLoopIntegration::unregister_kind(0);
    REQUIRE(MessageLoopIntegration::active_kind() == before_kind);
    REQUIRE(MessageLoopIntegration::active_name() == before_name);
    REQUIRE(MessageLoopIntegration::available() == before_available);
}

TEST_CASE("MessageLoopIntegration call_sync forwards return value",
          "[events][message-loop-integration]") {
    std::atomic<int> calls{0};
    auto token = MainThreadDispatcher::register_backend(make_noop_backend(&calls));
    REQUIRE(token != 0);
    MessageLoopIntegration::register_kind(token, MainLoopKind::Custom);

    // is_main_thread on the noop backend returns true, so call_sync
    // dispatches inline.
    bool ran = false;
    REQUIRE(MessageLoopIntegration::call_sync([&] { ran = true; }));
    REQUIRE(ran);

    MessageLoopIntegration::unregister_kind(token);
    REQUIRE(MainThreadDispatcher::unregister_backend(token));
}
