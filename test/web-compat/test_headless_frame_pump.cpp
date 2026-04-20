// Regression test for #542 — the native Three.js demo (and anything else
// that follows the same pattern) hung in headless `--capture` mode because
// the load loop only pumped JS microtasks via `pump_message_loop()` and
// never flushed pending `requestAnimationFrame` callbacks. In windowed
// mode the NSTimer-driven frame clock calls
// WidgetBridge::service_frame_callbacks which both pumps microtasks AND
// invokes __flushFrames__, so the hang was invisible there.
//
// This test reproduces the core invariant in a backend-agnostic way
// (QuickJS is always available) so every CI lane catches future
// regressions even when V8 isn't linked in.
//
// The test is deliberately independent of three.webgpu.js: we just need a
// JS fragment that reaches "ready" only once a requestAnimationFrame
// callback has run, which is the exact shape of the demo's module
// bootstrap.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <string>

using namespace pulp::view;
using namespace pulp;

namespace {

struct Env {
    View root;
    state::StateStore store;
    ScriptEngine engine;
    std::unique_ptr<WidgetBridge> bridge;
    Env() {
        root.set_bounds({0, 0, 256, 256});
        root.set_theme(Theme::dark());
        bridge = std::make_unique<WidgetBridge>(engine, root, store);
        // Prime the web-compat preamble (installs requestAnimationFrame /
        // __requestFrame__ / __flushFrames__).
        bridge->load_script("");
    }
};

std::string eval_string(ScriptEngine& engine, const std::string& code) {
    return std::string(engine.evaluate(code).getWithDefault<std::string_view>(""));
}

} // namespace

// Issue-NNN tag style keeps us compatible with Catch2's reserved-character
// rules (see CLAUDE.md — "Catch2 tag names cannot contain #").
TEST_CASE("headless load loop flushes requestAnimationFrame callbacks",
          "[view][widget-bridge][issue-542]") {
    Env env;

    // The JS under test mirrors the exact pattern that hung in #542:
    //   1. Set status = 'starting' synchronously.
    //   2. Register a requestAnimationFrame callback that flips the status
    //      to 'ready'. In the real demo this corresponds to the post-init
    //      frame that three.webgpu.js schedules during
    //      `await renderer.init()`.
    // Use engine.evaluate() directly (not load_script) because load_script
    // itself calls __flushFrames__ as part of its contract, which would
    // mask the bug we are reproducing. The pre-fix `load_demo` loop only
    // invoked pump_message_loop() after the initial load_script, and that
    // is what this test exercises.
    env.engine.evaluate(
        "globalThis.__issue542State = 'starting';"
        "window.requestAnimationFrame(function () {"
        "    globalThis.__issue542State = 'ready';"
        "});"
        "void 0"
    );

    REQUIRE(eval_string(env.engine,
                        "String(globalThis.__issue542State)") == "starting");

    // Microtask pumps alone must NOT advance the state — this is what the
    // pre-fix headless loop did, and is the exact bug we are guarding.
    for (int i = 0; i < 16; ++i) {
        env.engine.pump_message_loop();
    }
    REQUIRE(eval_string(env.engine,
                        "String(globalThis.__issue542State)") == "starting");

    // Servicing frame callbacks drains the pending rAF queue, which is the
    // behaviour load_demo() in examples/threejs-native-demo/main.cpp now
    // invokes inside its bounded pump loop.
    bool advanced = false;
    for (int i = 0; i < 16 && !advanced; ++i) {
        env.bridge->service_frame_callbacks();
        if (eval_string(env.engine,
                        "String(globalThis.__issue542State)") == "ready") {
            advanced = true;
        }
    }

    REQUIRE(advanced);
    REQUIRE(eval_string(env.engine,
                        "String(globalThis.__issue542State)") == "ready");
}

TEST_CASE("headless load loop repeatedly services chained rAF callbacks",
          "[view][widget-bridge][issue-542]") {
    // The demo's tick() function re-arms itself via requestAnimationFrame at
    // the end of each frame. Ensuring service_frame_callbacks() can drive a
    // chained animation loop for multiple frames protects against a fix
    // that only works for the very first rAF tick.
    Env env;

    env.engine.evaluate(
        "globalThis.__issue542Frames = 0;"
        "function __issue542Tick() {"
        "    globalThis.__issue542Frames += 1;"
        "    if (globalThis.__issue542Frames < 4) {"
        "        window.requestAnimationFrame(__issue542Tick);"
        "    }"
        "}"
        "window.requestAnimationFrame(__issue542Tick);"
        "void 0"
    );

    // Each service_frame_callbacks() call invokes one batch of pending
    // frames; the callback may re-arm the next frame via rAF.
    for (int i = 0; i < 16; ++i) {
        env.bridge->service_frame_callbacks();
        if (env.engine.evaluate("globalThis.__issue542Frames")
                .getWithDefault<int32_t>(0) >= 4) {
            break;
        }
    }

    REQUIRE(env.engine.evaluate("globalThis.__issue542Frames")
                .getWithDefault<int32_t>(0) == 4);
}
