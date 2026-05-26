// CFRunLoop cooperation in plugin-mode (item 6.4b macOS plan, post-PR-#2825).
//
// PR #2825 added pulp::events::MainThreadDispatcher and registered Cocoa /
// UIKit / SDL backends from standalone WindowHost constructors. In plugin
// mode (AU v3 / VST3 / CLAP loaded inside a DAW), there is no Pulp window
// and historically no backend was installed, so any code that asked
// "please run this on the host's main thread" silently no-oped.
//
// Item 6.4b ships `pulp::events::register_plugin_backend()` and wires it
// into each adapter's init/dealloc path. These tests pin the contract:
//
//   1. The bare contract — register installs a backend, unregister tears it
//      down, and reference-counting works across multiple "plugin instances".
//   2. The cross-platform discoverability — `plugin_backend_available()`
//      reports the right answer on each platform so adapter code can branch
//      on it for plugin-mode-only assertions.
//   3. The dispatch surface — once a backend is registered, `call_async`
//      enqueues work that is observable from another thread. On macOS the
//      work runs on the real main thread; on non-Apple platforms the
//      registration is a no-op and `call_async` returns false.

#include <catch2/catch_test_macros.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/events/plugin_main_thread.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#if defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#endif

using namespace pulp::events;

TEST_CASE("plugin_backend_available reports the expected platform value",
          "[events][main_thread_dispatcher][item-6-4b]") {
#if defined(__APPLE__)
    REQUIRE(plugin_backend_available());
#else
    REQUIRE_FALSE(plugin_backend_available());
#endif
}

TEST_CASE("register_plugin_backend / unregister_plugin_backend is symmetric",
          "[events][main_thread_dispatcher][item-6-4b]") {
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());

    auto token = register_plugin_backend();
#if defined(__APPLE__)
    REQUIRE(token != 0);
    REQUIRE(MainThreadDispatcher::has_backend());
    REQUIRE(unregister_plugin_backend(token));
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());
#else
    // No backend on this platform — the token is the sentinel `0` and we
    // can still call unregister_plugin_backend(0) for symmetric teardown.
    REQUIRE(token == 0);
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());
    REQUIRE(unregister_plugin_backend(token));
#endif
}

TEST_CASE("plugin backend is reference-counted across instances",
          "[events][main_thread_dispatcher][item-6-4b]") {
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());

    auto t1 = register_plugin_backend();
    auto t2 = register_plugin_backend();
    auto t3 = register_plugin_backend();

#if defined(__APPLE__)
    REQUIRE(t1 != 0);
    REQUIRE(t2 == t1);  // refcount-bumps return the same active token
    REQUIRE(t3 == t1);
    REQUIRE(MainThreadDispatcher::has_backend());

    REQUIRE(unregister_plugin_backend(t1));
    REQUIRE(MainThreadDispatcher::has_backend());  // still 2 outstanding

    REQUIRE(unregister_plugin_backend(t2));
    REQUIRE(MainThreadDispatcher::has_backend());  // still 1 outstanding

    REQUIRE(unregister_plugin_backend(t3));
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());  // all released

    // Once released, a stale token cannot remove a backend that is no
    // longer ours.
    REQUIRE_FALSE(unregister_plugin_backend(t1));
#else
    REQUIRE(t1 == 0);
    REQUIRE(t2 == 0);
    REQUIRE(t3 == 0);
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());
    REQUIRE(unregister_plugin_backend(t1));
    REQUIRE(unregister_plugin_backend(t2));
    REQUIRE(unregister_plugin_backend(t3));
#endif
}

#if defined(__APPLE__)
// macOS-only — verify call_async dispatches work that actually runs on the
// real main thread when the plugin backend is the only registered backend.
// This is the "dispatcher actually executes scheduled work on the host's
// main thread" regression the macOS plan asks for under 6.4b.
TEST_CASE("plugin backend marshals call_async onto the main thread",
          "[events][main_thread_dispatcher][item-6-4b][macos]") {
    auto token = register_plugin_backend();
    REQUIRE(token != 0);

    std::atomic<bool> ran{false};
    std::atomic<bool> ran_on_main{false};

    // Post from a worker thread so the dispatch is genuinely cross-thread.
    std::thread worker([&] {
        REQUIRE(MainThreadDispatcher::call_async([&] {
            ran_on_main.store(MainThreadDispatcher::is_main_thread());
            ran.store(true);
        }));
    });
    worker.join();

    // Cocoa's main-queue work runs when CFRunLoop is serviced. The
    // dispatch_get_main_queue() runloop source fires only when the runloop
    // is in its default mode and gets a tick. Spin the runloop here in the
    // same way a DAW's main thread would.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ran.load() && std::chrono::steady_clock::now() < deadline) {
        // Process one runloop iteration (up to 100ms). This drains the
        // main queue's runloop source.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, /*seconds=*/0.1,
                           /*returnAfterSourceHandled=*/true);
    }

    REQUIRE(ran.load());
    REQUIRE(ran_on_main.load());

    REQUIRE(unregister_plugin_backend(token));
}
#endif // __APPLE__

TEST_CASE("call_async without a backend returns false and runs nothing",
          "[events][main_thread_dispatcher][item-6-4b]") {
    REQUIRE_FALSE(MainThreadDispatcher::has_backend());
    std::atomic<int> ran{0};
    REQUIRE_FALSE(MainThreadDispatcher::call_async([&] { ran.fetch_add(1); }));
    REQUIRE(ran.load() == 0);
}
