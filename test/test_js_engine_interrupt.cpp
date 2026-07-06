// Cooperative-interrupt tests for the QuickJS backend. A runaway evaluation
// started on the engine thread must be abortable from another thread via
// request_interrupt(), and the engine must remain usable afterward.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/script_engine.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::view;

TEST_CASE("QuickJS reports interrupt support", "[view][script][interrupt]") {
    ScriptEngine engine;
    REQUIRE(static_cast<bool>(engine));
    REQUIRE(engine.engine_type() == JsEngineType::quickjs);
    REQUIRE(engine.supports_interrupt());
}

TEST_CASE("request_interrupt aborts a runaway evaluation", "[view][script][interrupt]") {
    ScriptEngine engine;

    std::atomic<bool> eval_running{false};
    std::thread interrupter([&] {
        // Wait until the evaluation is in flight, then abort it.
        while (!eval_running.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        engine.request_interrupt();
    });

    eval_running.store(true, std::memory_order_release);
    bool threw = false;
    try {
        // Tight infinite loop — only an interrupt can end it.
        engine.evaluate("var x = 0; while (true) { x = x + 1; }");
    } catch (...) {
        threw = true;
    }
    interrupter.join();

    REQUIRE(threw);

    // The cancel flag is consumed by the abort, so the engine keeps working.
    auto result = engine.evaluate("6 * 7");
    REQUIRE(result.getWithDefault<int>(0) == 42);
}

TEST_CASE("request_interrupt while idle does not corrupt the engine", "[view][script][interrupt]") {
    ScriptEngine engine;
    // Arming with nothing running may abort the *next* evaluation (QuickJS
    // clears the flag on the next interrupt check). Either the next eval throws
    // or it succeeds, but the engine must not crash and must recover.
    engine.request_interrupt();
    try {
        engine.evaluate("1 + 1");
    } catch (...) {
        // acceptable — the armed flag aborted this one
    }
    auto result = engine.evaluate("2 + 2");
    REQUIRE(result.getWithDefault<int>(0) == 4);
}
