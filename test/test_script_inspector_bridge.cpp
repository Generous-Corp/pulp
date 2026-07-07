// Tests for ScriptInspectorBridge — the thread-marshaling seam that lets an
// off-thread inspector evaluate against the single-threaded scripted-UI engine.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/script_inspector_bridge.hpp>
#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::view;
using namespace std::chrono_literals;

TEST_CASE("Bridge reports detached before an engine is attached", "[view][script][inspector]") {
    ScriptInspectorBridge bridge;
    auto r = bridge.evaluate("1 + 1");
    REQUIRE(r.detached);
    REQUIRE_FALSE(r.ok);

    auto caps = bridge.capabilities();
    REQUIRE(caps.engine.empty());
    REQUIRE_FALSE(caps.can_evaluate);
}

TEST_CASE("Bridge capabilities reflect the QuickJS engine", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    auto caps = bridge.capabilities();
    REQUIRE(caps.engine == "QuickJS");
    REQUIRE(caps.can_evaluate);
    REQUIRE(caps.can_interrupt);
    // Honest: mainline QuickJS has no source-line debug protocol.
    REQUIRE_FALSE(caps.can_break);
    REQUIRE_FALSE(caps.can_step);
    REQUIRE_FALSE(caps.can_inspect_locals);
}

TEST_CASE("Same-thread evaluate runs inline without a pump", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);  // records this thread as the engine thread

    auto r = bridge.evaluate("40 + 2");
    REQUIRE(r.ok);
    REQUIRE(r.json == "42");

    auto err = bridge.evaluate("throw new Error('boom')");
    REQUIRE_FALSE(err.ok);
    REQUIRE(err.error.find("boom") != std::string::npos);
}

TEST_CASE("Evaluate never yields invalid JSON for non-finite numbers", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    // 1/0 → Infinity, which is not representable as a bare JSON number. Whatever
    // the engine yields (QuickJS stringifies it to "Infinity"; the guard would
    // fold a bare token to null), the result must be valid JSON so it can be
    // embedded raw in a response frame. Wrap in an array because a bare scalar
    // is not valid top-level JSON.
    auto r = bridge.evaluate("1/0");
    if (r.ok) {
        REQUIRE_NOTHROW(choc::json::parse("[" + r.json + "]"));
    } else {
        REQUIRE_FALSE(r.error.empty());
    }
}

TEST_CASE("Cross-thread evaluate marshals onto the engine thread via pump", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);  // engine thread == this (main) thread

    ScriptInspectorBridge::EvalResult result;
    std::atomic<bool> done{false};
    std::thread client([&] {
        result = bridge.evaluate("'a' + 'b' + 'c'", 3000ms);
        done.store(true, std::memory_order_release);
    });

    // Drive the engine thread: pump until the client's request is serviced.
    while (!done.load(std::memory_order_acquire)) {
        bridge.pump();
        std::this_thread::sleep_for(1ms);
    }
    client.join();

    REQUIRE(result.ok);
    REQUIRE(result.json == "\"abc\"");
}

TEST_CASE("Cross-thread timeout interrupts a runaway evaluation", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::atomic<bool> done{false};
    std::thread client([&] {
        // Short timeout; the runaway loop can only end via the bridge's
        // timeout-triggered interrupt.
        result = bridge.evaluate("while (true) {}", 400ms);
        done.store(true, std::memory_order_release);
    });

    // The engine thread blocks inside this pump() running the runaway loop
    // until the client's timeout interrupts it. A hang here is a real failure
    // (the test harness will time out), which is the point.
    while (!done.load(std::memory_order_acquire)) {
        bridge.pump();
        std::this_thread::sleep_for(1ms);
    }
    client.join();

    // Either path proves the runaway was aborted: the interrupt landed before
    // the second wait (ok=false with an error) or the wait window elapsed
    // (timed_out). What must NOT happen is a successful result or a hang.
    REQUIRE_FALSE(result.ok);
    REQUIRE((result.timed_out || !result.error.empty()));

    // The engine recovers after the abort.
    auto ok = bridge.evaluate("7 * 7");
    REQUIRE(ok.ok);
    REQUIRE(ok.json == "49");
}

TEST_CASE("Detach wakes a blocked cross-thread evaluate", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::atomic<bool> done{false};
    std::thread client([&] {
        result = bridge.evaluate("123", 3000ms);
        done.store(true, std::memory_order_release);
    });

    // Do NOT pump. Detach should strand the pending request with a clear error.
    // Wait until the request is queued so the detach has something to strand.
    while (!bridge.is_busy())
        std::this_thread::sleep_for(1ms);
    bridge.detach();

    while (!done.load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);
    client.join();

    REQUIRE(result.detached);
    REQUIRE_FALSE(result.ok);
}
