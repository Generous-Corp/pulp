// test_widget_bridge_audio_meter.cpp — live audio metering + per-frame tick for
// scripted custom draws (the Forge live-visuals enabler).
//
// Covers the JS surface added by widget_bridge/audio_meter_api.cpp end-to-end:
//   * getMeterLevel(ch) / getMeterPeak(ch) / getMeterChannelCount() read the
//     latest MeterData an AudioBridge publishes (the audio→UI TripleBuffer path).
//   * onFrame(fn) registers a persistent per-frame callback that
//     service_frame_callbacks() fires once per host tick, driving a canvas
//     repaint; cancelFrame(id) stops it with zero further overhead.
//
// This is the enabler that lets a generated ui.js canvas animate from real audio
// (meters, VU needles, scopes, glow). The audio thread only writes the
// AudioBridge; these JS getters read it on the UI thread — mirroring the Forge
// shell contract (publish in process(), attach the bridge to the editor session).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

TEST_CASE("getMeter* returns 0 with no attached source", "[view][bridge][audio-meter]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE(bridge.meter_source() == nullptr);
    REQUIRE(engine.evaluate("getMeterChannelCount()").getWithDefault<int64_t>(-1) == 0);
    REQUIRE(engine.evaluate("getMeterLevel(0)").getWithDefault<double>(-1.0) == 0.0);
    REQUIRE(engine.evaluate("getMeterPeak(0)").getWithDefault<double>(-1.0) == 0.0);
}

TEST_CASE("getMeter* reads live peak/rms published through an AudioBridge",
          "[view][bridge][audio-meter]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    AudioBridge audio;
    MeterData data;
    data.num_channels = 2;
    data.peak[0] = 0.8f; data.rms[0] = 0.5f;
    data.peak[1] = 0.3f; data.rms[1] = 0.2f;
    audio.push_meter(data);

    bridge.set_meter_source(&audio);
    REQUIRE(bridge.meter_source() == &audio);

    REQUIRE(engine.evaluate("getMeterChannelCount()").getWithDefault<int64_t>(-1) == 2);
    REQUIRE_THAT(engine.evaluate("getMeterLevel(0)").getWithDefault<double>(-1.0), WithinAbs(0.5, 1e-4));
    REQUIRE_THAT(engine.evaluate("getMeterPeak(0)").getWithDefault<double>(-1.0), WithinAbs(0.8, 1e-4));
    REQUIRE_THAT(engine.evaluate("getMeterLevel(1)").getWithDefault<double>(-1.0), WithinAbs(0.2, 1e-4));
    REQUIRE_THAT(engine.evaluate("getMeterPeak(1)").getWithDefault<double>(-1.0), WithinAbs(0.3, 1e-4));

    // Out-of-range channels are clamped to 0, never a read past num_channels.
    REQUIRE(engine.evaluate("getMeterLevel(5)").getWithDefault<double>(-1.0) == 0.0);
    REQUIRE(engine.evaluate("getMeterPeak(-1)").getWithDefault<double>(-1.0) == 0.0);

    // A later publication is observed on the next read (TripleBuffer latest-value).
    MeterData next;
    next.num_channels = 2;
    next.peak[0] = 0.1f; next.rms[0] = 0.05f;
    audio.push_meter(next);
    REQUIRE_THAT(engine.evaluate("getMeterLevel(0)").getWithDefault<double>(-1.0), WithinAbs(0.05, 1e-4));
}

TEST_CASE("onFrame tick fires each service_frame_callbacks and drives a canvas repaint",
          "[view][bridge][audio-meter]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    AudioBridge audio;
    MeterData data;
    data.num_channels = 1;
    data.peak[0] = 0.9f; data.rms[0] = 0.6f;
    audio.push_meter(data);
    bridge.set_meter_source(&audio);

    // A ui.js-style meter canvas: onFrame reads the live level and paints a bar.
    bridge.load_script(R"(
        createCanvas('meter-canvas', '');
        globalThis.__frames__ = 0;
        globalThis.__lastLevel__ = -1;
        globalThis.__onFrameId__ = onFrame(function() {
            __frames__++;
            __lastLevel__ = getMeterLevel(0);
            canvasFillRect('meter-canvas', 0, 0, 10, __lastLevel__ * 100);
        });
    )");

    REQUIRE(bridge.frame_callback_count() == 1);
    auto* canvas = dynamic_cast<CanvasWidget*>(bridge.widget("meter-canvas"));
    REQUIRE(canvas != nullptr);
    // Registered but not yet ticked: no draw commands, callback hasn't run.
    REQUIRE(canvas->command_count() == 0);
    REQUIRE(engine.evaluate("__frames__").getWithDefault<int64_t>(-1) == 0);

    for (int i = 0; i < 3; ++i) bridge.service_frame_callbacks();

    REQUIRE(engine.evaluate("__frames__").getWithDefault<int64_t>(-1) == 3);
    // The onFrame body read the live meter (0.6 rms) through the JS getter.
    REQUIRE_THAT(engine.evaluate("__lastLevel__").getWithDefault<double>(-1.0), WithinAbs(0.6, 1e-4));
    // Each frame appended one fill_rect: proof the tick drives repaint CONTENT.
    REQUIRE(canvas->command_count() == 3);

    // A changed signal flows through to the next frame's draw.
    MeterData quieter;
    quieter.num_channels = 1;
    quieter.rms[0] = 0.25f;
    audio.push_meter(quieter);
    bridge.service_frame_callbacks();
    REQUIRE_THAT(engine.evaluate("__lastLevel__").getWithDefault<double>(-1.0), WithinAbs(0.25, 1e-4));
}

TEST_CASE("cancelFrame stops the tick with zero further overhead", "[view][bridge][audio-meter]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCanvas('c', '');
        globalThis.__frames__ = 0;
        globalThis.__id__ = onFrame(function() {
            __frames__++;
            canvasFillRect('c', 0, 0, 10, 10);
        });
    )");
    REQUIRE(bridge.frame_callback_count() == 1);

    bridge.service_frame_callbacks();
    auto* canvas = dynamic_cast<CanvasWidget*>(bridge.widget("c"));
    REQUIRE(canvas != nullptr);
    REQUIRE(engine.evaluate("__frames__").getWithDefault<int64_t>(-1) == 1);
    const auto commands_after_one = canvas->command_count();
    REQUIRE(commands_after_one == 1);

    engine.evaluate("cancelFrame(__id__)");
    REQUIRE(bridge.frame_callback_count() == 0);

    // Further ticks are a no-op: the callback never runs and no commands append.
    bridge.service_frame_callbacks();
    bridge.service_frame_callbacks();
    REQUIRE(engine.evaluate("__frames__").getWithDefault<int64_t>(-1) == 1);
    REQUIRE(canvas->command_count() == commands_after_one);
}
