#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/runtime_eval_component.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/script_inspector_bridge.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;
using namespace pulp::view;

namespace {
bool wait_until_busy(ScriptInspectorBridge& bridge) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!bridge.is_busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return bridge.is_busy();
}
} // namespace

TEST_CASE("Runtime eval component exposes the live engine and retained marker",
          "[inspect][runtime-eval][component]") {
    STATIC_REQUIRE(kRuntimeEvalDeadline == 2000ms);
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);

    const auto caps = evaluator->capabilities();
    REQUIRE(caps.engine == "QuickJS");
    REQUIRE(caps.can_evaluate);
    REQUIRE(caps.can_interrupt);
    REQUIRE(evaluator->binary_marker() ==
            "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1");
    const auto result = evaluator->evaluate("6 * 7");
    REQUIRE(result.ok);
    REQUIRE(result.json == "42");
}

TEST_CASE("Runtime eval component caps serialized results",
          "[inspect][runtime-eval][component][negative][limits]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);

    const auto result = evaluator->evaluate(
        "'x'.repeat(1048577)");
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.json.empty());
    REQUIRE(result.error ==
            "Runtime.evaluate result exceeds the 1048576-byte limit");

    const auto recovered = evaluator->evaluate("21 * 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}

TEST_CASE("Runtime eval component rejects cyclic and deeply nested results",
          "[inspect][runtime-eval][component][negative][limits]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);

    const auto cyclic = evaluator->evaluate(
        "(() => { const value = {}; value.self = value; return value; })()");
    REQUIRE_FALSE(cyclic.ok);
    REQUIRE(cyclic.error ==
            "Runtime.evaluate result contains a cycle");

    const auto deep = evaluator->evaluate(
        "(() => { let root = {}, value = root;"
        " for (let i = 0; i < 40; ++i) value = value.child = {};"
        " return root; })()");
    REQUIRE_FALSE(deep.ok);
    REQUIRE(deep.error ==
            "Runtime.evaluate result exceeds the 32-level depth limit");

    const auto recovered = evaluator->evaluate("({ answer: 6 * 7 })");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "{\"answer\":42}");
}

TEST_CASE("Runtime eval component bounds property enumeration before allocation",
          "[inspect][runtime-eval][component][negative][limits]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);

    const auto too_many_properties = bridge.evaluate(
        "(() => { const value = {};"
        " for (let i = 0; i < 20; ++i) value['p' + i] = 0;"
        " return value; })()",
        2s, 64);
    REQUIRE_FALSE(too_many_properties.ok);
    REQUIRE(too_many_properties.error ==
            "Runtime.evaluate result exceeds the 64-byte limit");

    const auto exotic = evaluator->evaluate("new Proxy({}, { ownKeys() { for (;;) {} } })");
    REQUIRE_FALSE(exotic.ok);
    REQUIRE(exotic.error ==
            "Runtime.evaluate result contains an unsupported exotic object");

    const auto recovered = evaluator->evaluate("({ answer: 6 * 7 })");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "{\"answer\":42}");
}

TEST_CASE("Runtime eval component independently rejects oversized code",
          "[inspect][runtime-eval][component][negative][limits]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);

    const auto oversized = evaluator->evaluate(
        std::string(kRuntimeEvalMaxCodeBytes + 1, 'a'));
    REQUIRE_FALSE(oversized.ok);
    REQUIRE(oversized.error ==
            "Runtime.evaluate code exceeds the 65536-byte limit");
    const std::string embedded_nul{"a\0b", 3};
    const auto nul = evaluator->evaluate(embedded_nul);
    REQUIRE_FALSE(nul.ok);
    REQUIRE(nul.error == "Runtime.evaluate code contains a NUL byte");
}

TEST_CASE("Runtime eval component interrupts a hung evaluation and recovers",
          "[inspect][runtime-eval][component][deadline][teardown]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);
    RuntimeEvaluationResult hung;
    std::atomic<bool> audio_running{true};
    std::atomic<std::uint64_t> audio_ticks{0};
    std::thread audio([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (audio_running.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            audio_ticks.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread client([&] { hung = evaluator->evaluate("while (true) {}"); });

    const bool observed_busy = wait_until_busy(bridge);
    const bool pumped = observed_busy && bridge.pump();
    if (!observed_busy)
        bridge.detach();
    client.join();
    audio_running.store(false, std::memory_order_release);
    audio.join();

    REQUIRE(observed_busy);
    REQUIRE(pumped);
    REQUIRE(hung.timed_out);
    REQUIRE_FALSE(hung.ok);
    REQUIRE(audio_ticks.load(std::memory_order_relaxed) > 0);
    REQUIRE_FALSE(bridge.is_busy());
    const auto recovered = evaluator->evaluate("7 * 6");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}

TEST_CASE("Runtime eval component queued request is released by teardown",
          "[inspect][runtime-eval][component][teardown]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);
    RuntimeEvaluationResult result;
    std::thread client([&] { result = evaluator->evaluate("123"); });

    const bool observed_busy = wait_until_busy(bridge);
    bridge.detach();
    client.join();
    REQUIRE(observed_busy);
    REQUIRE(result.detached);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(bridge.is_busy());
}

TEST_CASE("Runtime eval component interrupts queued work without poisoning recovery",
          "[inspect][runtime-eval][component][interrupt]") {
    ScriptEngine engine;
    ScriptInspectorBridge bridge;
    bridge.attach(&engine);
    auto evaluator = make_script_runtime_evaluator(&bridge);
    RuntimeEvaluationResult result;
    std::thread client([&] { result = evaluator->evaluate("123"); });

    const bool observed_busy = wait_until_busy(bridge);
    const bool interrupted = observed_busy && evaluator->interrupt();
    if (!observed_busy)
        bridge.detach();
    client.join();

    REQUIRE(observed_busy);
    REQUIRE(interrupted);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "evaluation interrupted before it ran");
    REQUIRE_FALSE(bridge.is_busy());
    const auto recovered = evaluator->evaluate("40 + 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");
}
