// Tests for ScriptInspectorBridge — the thread-marshaling seam that lets an
// off-thread inspector evaluate against the single-threaded scripted-UI engine.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/script_inspector_bridge.hpp>
#include <choc/text/choc_JSON.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

using namespace pulp::view;
using namespace std::chrono_literals;

namespace pulp::view {
struct ScriptInspectorBridgeTestAccess {
    static bool wait_until_queued(ScriptInspectorBridge& bridge) {
        std::unique_lock<std::mutex> lock(bridge.mutex_);
        return bridge.state_cv_.wait_for(lock, 3s, [&] {
            return bridge.pending_ != nullptr;
        });
    }

    static bool wait_until_running(ScriptInspectorBridge& bridge) {
        std::unique_lock<std::mutex> lock(bridge.mutex_);
        return bridge.state_cv_.wait_for(lock, 3s, [&] {
            return bridge.running_ != nullptr;
        });
    }
};
}  // namespace pulp::view

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

TEST_CASE("A completed evaluation owes its realm reset to the next drain",
          "[view][script][inspector][reset]") {
    ScriptEngine original;
    ScriptEngine replacement;
    ScriptInspectorBridge bridge;
    bridge.attach(&original);

    int resets = 0;
    bridge.set_post_evaluation_reset([&](auto) {
        ++resets;
        bridge.detach();
        bridge.attach(&replacement);
        return std::string{};
    });

    const auto result = bridge.evaluate("40 + 2");
    REQUIRE(result.ok);
    REQUIRE(result.json == "42");
    // Rebuilding inside the request would replace the realm under a caller
    // still holding widget pointers from before it. The debt is recorded and
    // the host discharges it at its frame boundary.
    REQUIRE(resets == 0);
    REQUIRE(bridge.post_evaluation_reset_pending());

    REQUIRE(bridge.run_pending_post_evaluation_reset().empty());
    REQUIRE(resets == 1);
    REQUIRE_FALSE(bridge.post_evaluation_reset_pending());
    REQUIRE(bridge.capabilities().engine == "QuickJS");

    // The debt is owed once however many drains follow it.
    REQUIRE(bridge.run_pending_post_evaluation_reset().empty());
    REQUIRE(resets == 1);
}

TEST_CASE("Evaluations sharing a realm collapse into one reset",
          "[view][script][inspector][reset]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    int resets = 0;
    bridge.set_post_evaluation_reset([&](auto) {
        ++resets;
        return std::string{};
    });

    REQUIRE(bridge.evaluate("1").ok);
    REQUIRE(bridge.evaluate("2").ok);
    REQUIRE(bridge.evaluate("3").ok);
    // All three ran in the same realm, so one reconstruction from last-good
    // source discards everything any of them left behind.
    REQUIRE(bridge.run_pending_post_evaluation_reset().empty());
    REQUIRE(resets == 1);
}

TEST_CASE("A realm reset serves the request queued while it ran",
          "[view][script][inspector][reset]") {
    ScriptEngine original;
    ScriptEngine replacement;
    ScriptInspectorBridge bridge;
    bridge.attach(&original);

    // Deferring the reset means the whole gap between one evaluation's response
    // and the next frame is time in which a client can queue. The engine swap
    // inside the reset must not fail a request that has not run yet.
    bridge.set_post_evaluation_reset([&](auto) {
        bridge.detach();
        bridge.attach(&replacement);
        return std::string{};
    });
    REQUIRE(bridge.evaluate("1").ok);

    ScriptInspectorBridge::EvalResult queued;
    std::thread client([&] { queued = bridge.evaluate("40 + 2", 3s); });
    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));

    REQUIRE(bridge.run_pending_post_evaluation_reset().empty());
    REQUIRE(bridge.pump());
    client.join();

    REQUIRE(queued.ok);
    REQUIRE(queued.json == "42");
    REQUIRE_FALSE(queued.detached);
}

TEST_CASE("A realm reset that leaves no engine strands the request it held",
          "[view][script][inspector][reset]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    // Fail-closed: the reset detaches without a replacement, so the queued
    // request must be failed at the drain rather than left to wait out its own
    // deadline holding the single-flight slot.
    bridge.set_post_evaluation_reset([&](auto) {
        bridge.detach();
        return std::string{"reload failed"};
    });
    REQUIRE(bridge.evaluate("1").ok);

    ScriptInspectorBridge::EvalResult queued;
    std::thread client([&] { queued = bridge.evaluate("40 + 2", 30s); });
    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));

    REQUIRE(bridge.run_pending_post_evaluation_reset() == "reload failed");
    client.join();

    REQUIRE(queued.detached);
    REQUIRE_FALSE(bridge.is_busy());
}

TEST_CASE("Realm reset failure is reported by the drain that ran it",
          "[view][script][inspector][reset]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    bridge.set_post_evaluation_reset(
        [](auto) { return std::string{"reload failed"}; });

    const auto result = bridge.evaluate("40 + 2");
    REQUIRE(result.ok);
    REQUIRE_FALSE(bridge.is_busy());
    // Deferring the reset moved its failure report off the evaluate response
    // and onto the drain. The host turns this into poll()'s "evaluated realm
    // reset failed: …" and runs the same fail-closed quarantine.
    REQUIRE(bridge.run_pending_post_evaluation_reset() == "reload failed");
    REQUIRE_FALSE(bridge.post_evaluation_reset_pending());

    bridge.set_post_evaluation_reset([](auto) -> std::string {
        throw std::runtime_error("reset threw");
    });
    REQUIRE(bridge.evaluate("1").ok);
    REQUIRE(bridge.run_pending_post_evaluation_reset() == "reset threw");
    REQUIRE_FALSE(bridge.post_evaluation_reset_pending());
}

TEST_CASE("External detach cannot cross an in-progress realm reset",
          "[view][script][inspector][reset][lifetime]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    std::mutex gate;
    std::condition_variable gate_cv;
    bool reset_started = false;
    bool reset_saw_detacher = false;
    bool detacher_saw_reset = false;
    bool detach_started = false;
    bool detach_returned = false;
    bool detach_crossed_reset = false;
    bridge.set_post_evaluation_reset([&](auto) {
        std::unique_lock<std::mutex> lock(gate);
        reset_started = true;
        gate_cv.notify_all();
        reset_saw_detacher = gate_cv.wait_for(
            lock, 3s, [&] { return detach_started; });
        detach_crossed_reset = gate_cv.wait_for(
            lock, 100ms, [&] { return detach_returned; });
        return std::string{};
    });

    const auto result = bridge.evaluate("6 * 7", 3s);
    REQUIRE(result.ok);
    REQUIRE(bridge.post_evaluation_reset_pending());

    std::thread detacher([&] {
        {
            std::unique_lock<std::mutex> lock(gate);
            detacher_saw_reset = gate_cv.wait_for(
                lock, 3s, [&] { return reset_started; });
            detach_started = true;
            gate_cv.notify_all();
        }
        bridge.detach();
        {
            std::lock_guard<std::mutex> lock(gate);
            detach_returned = true;
        }
        gate_cv.notify_all();
    });

    REQUIRE(bridge.run_pending_post_evaluation_reset().empty());
    detacher.join();

    // A reset now runs outside any request, so engine quiescence has to cover
    // it too: a detacher must not return while a realm reconstruction is still
    // touching the engine it is about to let the caller destroy.
    REQUIRE_FALSE(detach_crossed_reset);
    REQUIRE(reset_saw_detacher);
    REQUIRE(detacher_saw_reset);
    REQUIRE(detach_returned);
    REQUIRE_FALSE(bridge.is_busy());
}

TEST_CASE("A late engine interrupt is cleared before the next evaluation",
          "[view][script][inspector][interrupt]") {
    ScriptEngine engine;
    engine.request_interrupt();
    REQUIRE(engine.clear_pending_interrupt());
    REQUIRE_FALSE(engine.clear_pending_interrupt());

    const auto value = engine.evaluate(
        "let total = 0;"
        "for (let i = 0; i < 100000; ++i) total += i;"
        "total");
    REQUIRE(value.getWithDefault<std::int64_t>(0) == 4999950000);
}

TEST_CASE("Interrupt after evaluation completion reports no abort",
          "[view][script][inspector][interrupt]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    const auto completed = bridge.evaluate("6 * 7");
    REQUIRE(completed.ok);
    REQUIRE(completed.json == "42");
    REQUIRE_FALSE(bridge.interrupt());

    const auto next = bridge.evaluate("7 * 7");
    REQUIRE(next.ok);
    REQUIRE(next.json == "49");
}

TEST_CASE("Owner-thread evaluate is single-flight and explicitly interruptible",
          "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult contender_result;
    bool observed_running = false;
    bool interrupted = false;
    std::thread contender([&] {
        observed_running = ScriptInspectorBridgeTestAccess::wait_until_running(bridge);
        contender_result = bridge.evaluate("6 * 7", 3s);
        interrupted = bridge.interrupt();
    });

    const auto running_result = bridge.evaluate("while (true) {}", 3s);
    contender.join();

    REQUIRE(observed_running);
    REQUIRE(contender_result.busy);
    REQUIRE(interrupted);
    REQUIRE_FALSE(running_result.ok);
    REQUIRE(running_result.error == "evaluation interrupted");

    const auto recovered = bridge.evaluate("6 * 7");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}

TEST_CASE("Owner-thread runaway evaluation has a deterministic deadline",
          "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    const auto timed_out = bridge.evaluate("while (true) {}", 50ms);
    REQUIRE(timed_out.timed_out);
    REQUIRE_FALSE(timed_out.ok);
    REQUIRE(timed_out.error == "evaluation timed out");
    REQUIRE_FALSE(bridge.is_busy());

    const auto recovered = bridge.evaluate("21 * 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
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
    std::thread client([&] {
        result = bridge.evaluate("'a' + 'b' + 'c'", 3000ms);
    });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    REQUIRE(bridge.pump());
    client.join();

    REQUIRE(result.ok);
    REQUIRE(result.json == "\"abc\"");
}

TEST_CASE("Cross-thread timeout interrupts a runaway evaluation", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::thread client([&] {
        result = bridge.evaluate("while (true) {}", 50ms);
    });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    REQUIRE(bridge.pump());
    client.join();

    REQUIRE(result.timed_out);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "evaluation timed out");
    REQUIRE_FALSE(bridge.is_busy());

    // The engine recovers after the abort.
    auto ok = bridge.evaluate("7 * 7");
    REQUIRE(ok.ok);
    REQUIRE(ok.json == "49");
}

TEST_CASE("A queued timeout cancels the request without poisoning the engine",
          "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::thread client([&] { result = bridge.evaluate("123", 50ms); });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    client.join();

    REQUIRE(result.timed_out);
    REQUIRE_FALSE(bridge.is_busy());
    REQUIRE_FALSE(bridge.pump());

    const auto recovered = bridge.evaluate("40 + 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}

TEST_CASE("Interrupt cancels a queued request instead of arming the next eval",
          "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::thread client([&] { result = bridge.evaluate("123", 3s); });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    REQUIRE(bridge.interrupt());
    client.join();

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "evaluation interrupted before it ran");
    REQUIRE_FALSE(bridge.is_busy());
    REQUIRE_FALSE(bridge.pump());

    const auto recovered = bridge.evaluate("40 + 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}

TEST_CASE("Detach wakes a blocked cross-thread evaluate", "[view][script][inspector]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);

    ScriptInspectorBridge::EvalResult result;
    std::thread client([&] {
        result = bridge.evaluate("123", 3000ms);
    });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    bridge.detach();
    client.join();

    REQUIRE(result.detached);
    REQUIRE_FALSE(result.ok);
}

TEST_CASE("Detach interrupts a running eval and waits for engine quiescence",
          "[view][script][inspector]") {
    auto engine = std::make_unique<ScriptEngine>();
    ScriptInspectorBridge bridge;
    bridge.attach(engine.get());

    ScriptInspectorBridge::EvalResult result;
    bool observed_running = false;
    std::thread client([&] { result = bridge.evaluate("while (true) {}", 3s); });

    REQUIRE(ScriptInspectorBridgeTestAccess::wait_until_queued(bridge));
    std::thread detacher([&] {
        observed_running = ScriptInspectorBridgeTestAccess::wait_until_running(bridge);
        bridge.detach();
        // detach() is the lifetime fence: destroying the caller-owned engine is
        // safe immediately after it returns, even while pump() began with that
        // engine pointer.
        engine.reset();
    });

    REQUIRE(bridge.pump());
    detacher.join();
    client.join();

    REQUIRE(observed_running);
    REQUIRE(result.detached);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(bridge.is_busy());
    REQUIRE_FALSE(bridge.capabilities().can_evaluate);
    REQUIRE(engine == nullptr);
}
