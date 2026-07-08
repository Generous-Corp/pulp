// End-to-end test for the queryIndex* JS bridge API (R7). Drives a real
// ScriptEngine + WidgetBridge: JS builds an index and searches it, the work runs
// on the service's worker thread, and poll_async_results() delivers the result
// back into the JS context via __dispatch__(callbackId, 'result', payload) — the
// same UI-thread pump execAsync uses.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <functional>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;

namespace {

bool wait_for(WidgetBridge& bridge, const std::function<bool()>& done, int attempts = 400) {
    for (int i = 0; i < attempts; ++i) {
        bridge.poll_async_results();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bridge.poll_async_results();
    return done();
}

}  // namespace

TEST_CASE("queryIndexSearch delivers ranked indices into a JS callback",
          "[view][bridge][query]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        globalThis.__q = null;
        __callbacks__['q1:result'] = function(payload) { globalThis.__q = payload; };
        queryIndexBuild('lib', ['kick', 'snare', 'hat', 'kick_deep', 'clap']);
        queryIndexSearch('lib', 'kick', 'q1', 10);
    )JS");

    REQUIRE(wait_for(bridge, [&] {
        return engine.evaluate("globalThis.__q !== null").getWithDefault<bool>(false);
    }));

    // Payload arrives as a JSON string; parse it back on the JS side.
    const auto len = engine.evaluate("JSON.parse(globalThis.__q).length").getWithDefault<double>(-1);
    REQUIRE(len == 2);
    REQUIRE(engine.evaluate("JSON.parse(globalThis.__q)[0]").getWithDefault<double>(-1) == 0);
    REQUIRE(engine.evaluate("JSON.parse(globalThis.__q)[1]").getWithDefault<double>(-1) == 3);
}

TEST_CASE("queryIndexRelease drops the dataset so later searches are empty",
          "[view][bridge][query]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        globalThis.__before = null;
        globalThis.__after = null;
        __callbacks__['before:result'] = function(p) { globalThis.__before = p; };
        __callbacks__['after:result'] = function(p) { globalThis.__after = p; };
        queryIndexBuild('lib', ['kick', 'snare']);
        queryIndexSearch('lib', 'kick', 'before');
    )JS");
    REQUIRE(wait_for(bridge, [&] {
        return engine.evaluate("globalThis.__before !== null").getWithDefault<bool>(false);
    }));
    REQUIRE(engine.evaluate("globalThis.__before").getString() == "[0]");

    engine.evaluate("queryIndexRelease('lib'); queryIndexSearch('lib', 'kick', 'after');");
    REQUIRE(wait_for(bridge, [&] {
        return engine.evaluate("globalThis.__after !== null").getWithDefault<bool>(false);
    }));
    REQUIRE(engine.evaluate("globalThis.__after").getString() == "[]");
}

TEST_CASE("queryIndexBuild reports readiness with the item count",
          "[view][bridge][query]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        globalThis.__count = null;
        __callbacks__['ready:result'] = function(payload) { globalThis.__count = payload; };
        queryIndexBuild('lib', ['a', 'b', 'c'], 'ready');
    )JS");

    REQUIRE(wait_for(bridge, [&] {
        return engine.evaluate("globalThis.__count !== null").getWithDefault<bool>(false);
    }));
    REQUIRE(engine.evaluate("Number(globalThis.__count)").getWithDefault<double>(-1) == 3);
}
