// test_widget_bridge_animation.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 P5-1 follow-up refactor.
//
// Bridge animation API tests. Covers the bridge ↔ MotionEngine plumbing
// for setMotionToken, animate(), the Web Animations API surface
// (Element.animate(...) / KeyframeEffect), motion provenance, and the
// pulp-motion-bench harness output. Self-contained cluster from
// test_widget_bridge.cpp — about 800 lines of contiguous animation tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static std::string trim_crlf(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

static std::string js_single_quoted(std::string value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

static bool wait_for_async_result(WidgetBridge& bridge, const std::function<bool()>& done) {
#if defined(_WIN32)
    constexpr int attempts = 300;
#else
    constexpr int attempts = 50;
#endif
    for (int i = 0; i < attempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bridge.poll_async_results();
        if (done()) return true;
    }
    return done();
}

// ── Bridge animation API tests ──────────────────────────────────────────────

TEST_CASE("WidgetBridge setMotionToken from JS", "[view][bridge][animation]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("setMotionToken('motion.duration.fast', 0.5)");

    auto val = root.theme().dimension("motion.duration.fast");
    REQUIRE(val.has_value());
    REQUIRE_THAT(val.value(), WithinAbs(0.5, 0.001));
}

TEST_CASE("WidgetBridge getMotionToken from JS", "[view][bridge][animation]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto result = engine.evaluate("getMotionToken('motion.duration.fast')");
    REQUIRE(result.getWithDefault<double>(0) > 0.0);
}

TEST_CASE("WidgetBridge setVisible from JS", "[view][bridge][animation]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    auto* w = bridge.widget("gain");
    REQUIRE(w->visible());

    bridge.load_script("setVisible('gain', 0)");
    REQUIRE_FALSE(w->visible());

    bridge.load_script("setVisible('gain', 1)");
    REQUIRE(w->visible());
}

TEST_CASE("WidgetBridge removeWidget from JS", "[view][bridge][animation]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    REQUIRE(root.child_count() == 1);
    REQUIRE(bridge.widget("gain") != nullptr);

    bridge.load_script("removeWidget('gain')");
    REQUIRE(root.child_count() == 0);
    REQUIRE(bridge.widget("gain") == nullptr);
}

TEST_CASE("WidgetBridge ComboBox selection survives applyTokenDiff in select handler", "[view][bridge][combo]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCombo('harmony', '');
        setItems('harmony', ['Monochromatic', 'Analogous', 'Complementary']);
        on('harmony', 'select', function(idx) {
            setSelected('harmony', idx);
            applyTokenDiff('{"colors":{"accent.primary":"#ff0000"}}');
        });
    )");

    auto* combo = dynamic_cast<ComboBox*>(bridge.widget("harmony"));
    REQUIRE(combo != nullptr);
    REQUIRE(combo->selected() == 0);
    auto* original_ptr = combo;

    combo->set_bounds({0, 0, 140, 120});

    MouseEvent open_click;
    open_click.position = {70, 14};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);

    MouseEvent select_click;
    select_click.position = {70, 54};
    select_click.is_down = true;
    combo->on_mouse_event(select_click);

    auto* combo_after = dynamic_cast<ComboBox*>(bridge.widget("harmony"));
    REQUIRE(combo_after == original_ptr);
    REQUIRE(combo_after->selected() == 1);
    REQUIRE(combo_after->selected_text() == "Analogous");
}

TEST_CASE("WidgetBridge setSelected updates ComboBox without firing select handler", "[view][bridge][combo]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var select_count = 0;
        createCombo('harmony', '');
        setItems('harmony', ['Monochromatic', 'Analogous', 'Complementary']);
        on('harmony', 'select', function(idx) { select_count++; });
    )");

    auto* combo = dynamic_cast<ComboBox*>(bridge.widget("harmony"));
    REQUIRE(combo != nullptr);
    REQUIRE(combo->selected_text() == "Monochromatic");

    bridge.load_script("setSelected('harmony', 1)");

    REQUIRE(combo->selected() == 1);
    REQUIRE(combo->selected_text() == "Analogous");
    REQUIRE(engine.evaluate("select_count").getWithDefault<int>(-1) == 0);
}

TEST_CASE("WidgetBridge shader and schema APIs apply to knob, fader, and toggle", "[view][bridge][style]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain', 10, 10, 48, 48);
        createFader('volume', 70, 10, 24, 120, 'vertical');
        createToggle('bypass', 110, 10, 50, 30);
        setWidgetShader('gain', 'half4 main(float2 coord) { return half4(1); }');
        setWidgetShader('volume', 'half4 main(float2 coord) { return half4(1); }');
        setWidgetShader('bypass', 'half4 main(float2 coord) { return half4(1); }');
        setWidgetSchema('gain', '{"elements":[{"type":"circle","radius":"20%","color":"accent.primary"}]}');
        setWidgetSchema('volume', '{"elements":[{"type":"rect","cornerRadius":"4","color":"control.fill"}]}');
        setWidgetSchema('bypass', '{"elements":[{"type":"rect","cornerRadius":"10","color":"control.track"}]}');
    )");

    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain"));
    auto* fader = dynamic_cast<Fader*>(bridge.widget("volume"));
    auto* toggle = dynamic_cast<Toggle*>(bridge.widget("bypass"));
    REQUIRE(knob != nullptr);
    REQUIRE(fader != nullptr);
    REQUIRE(toggle != nullptr);

    REQUIRE(knob->has_custom_shader());
    REQUIRE(fader->has_custom_shader());
    REQUIRE(toggle->has_custom_shader());
    REQUIRE_FALSE(knob->widget_schema().empty());
    REQUIRE_FALSE(fader->widget_schema().empty());
    REQUIRE_FALSE(toggle->widget_schema().empty());

    bridge.load_script(R"(
        clearWidgetShader('gain');
        clearWidgetShader('volume');
        clearWidgetShader('bypass');
        clearWidgetSchema('gain');
        clearWidgetSchema('volume');
        clearWidgetSchema('bypass');
    )");

    REQUIRE_FALSE(knob->has_custom_shader());
    REQUIRE_FALSE(fader->has_custom_shader());
    REQUIRE_FALSE(toggle->has_custom_shader());
    REQUIRE(knob->widget_schema().empty());
    REQUIRE(fader->widget_schema().empty());
    REQUIRE(toggle->widget_schema().empty());
}

TEST_CASE("WidgetBridge Lottie APIs store state on knob, fader, and toggle", "[view][bridge][style]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain', 10, 10, 48, 48);
        createFader('volume', 70, 10, 24, 120, 'vertical');
        createToggle('bypass', 110, 10, 50, 30);
        setWidgetLottie('gain', '{"v":"5.5.2"}');
        setWidgetLottie('volume', '{"v":"5.5.2"}');
        setWidgetLottie('bypass', '{"v":"5.5.2"}');
        seekWidgetLottie('gain', 0.25);
        seekWidgetLottie('volume', 0.5);
        seekWidgetLottie('bypass', 0.75);
    )");

    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain"));
    auto* fader = dynamic_cast<Fader*>(bridge.widget("volume"));
    auto* toggle = dynamic_cast<Toggle*>(bridge.widget("bypass"));
    REQUIRE(knob != nullptr);
    REQUIRE(fader != nullptr);
    REQUIRE(toggle != nullptr);

    REQUIRE(knob->lottie_json() == "{\"v\":\"5.5.2\"}");
    REQUIRE(fader->lottie_json() == "{\"v\":\"5.5.2\"}");
    REQUIRE(toggle->lottie_json() == "{\"v\":\"5.5.2\"}");
    REQUIRE_THAT(knob->lottie_time(), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(fader->lottie_time(), WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(toggle->lottie_time(), WithinAbs(0.75f, 0.001f));
}

TEST_CASE("WidgetBridge import/export design tokens and AI CLI are scriptable", "[view][bridge][style]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        importDesignTokens('{"accent":{"primary":{"$value":"#ff0000"}},"spacing":{"md":{"$value":"12"}}}');
        setAICli('echo test-ai');
    )");

    REQUIRE(root.theme().color("accent.primary").has_value());
    REQUIRE(root.theme().color("accent.primary").value() == color_from_hex(0xFF0000));
    REQUIRE(root.theme().dimension("spacing.md").has_value());
    REQUIRE(root.theme().dimension("spacing.md").value() == 12.0f);
    REQUIRE(engine.evaluate("getAICli()").toString() == "echo test-ai");

    auto exported = engine.evaluate("exportDesignTokens()").toString();
    REQUIRE(exported.find("accent") != std::string::npos);
    REQUIRE(exported.find("#ff0000") != std::string::npos);
}

TEST_CASE("WidgetBridge compileShader accepts standard widget-uniform SkSL", "[view][bridge][style]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var shader_compile = compileShader(`uniform float2 resolution;
uniform float value;
layout(color) uniform float4 accentColor;
layout(color) uniform float4 fillColor;
half4 main(float2 coord) {
  float2 uv = coord / resolution;
  float glow = smoothstep(0.9, 0.2, length(uv - float2(0.5)));
  half3 color = mix(fillColor.rgb, accentColor.rgb, half(value));
  return half4(color * half(glow), half(glow));
}`);
    )");

    const auto success = engine.evaluate("shader_compile.success").getWithDefault<bool>(false);
    const auto error = engine.evaluate("shader_compile.error").toString();

#ifdef PULP_HAS_SKIA
    REQUIRE(success);
    REQUIRE(error.empty());
#else
    REQUIRE_FALSE(success);
    REQUIRE(error == "Skia not available — shader compilation requires GPU build");
#endif
}

TEST_CASE("WidgetBridge execAsync returns results without blocking the caller", "[view][bridge][async]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    const std::string async_cmd =
#if defined(_WIN32)
        "powershell -NoProfile -Command \"[Console]::Out.Write('hello async')\"";
#else
        "printf 'hello async'";
#endif

    bridge.load_script(
        "var async_result = '';\n"
        "on('__async-test__', 'result', function(value) { async_result = value; });\n"
        "execAsync('" + js_single_quoted(async_cmd) + "', '__async-test__');\n");

    REQUIRE(wait_for_async_result(bridge, [&] {
        return trim_crlf(engine.evaluate("async_result").toString()) == "hello async";
    }));
    REQUIRE(trim_crlf(engine.evaluate("async_result").toString()) == "hello async");
}

TEST_CASE("WidgetBridge requestAnimationFrame callbacks continue during poll loop", "[view][bridge][async]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var frame_count = 0;
        function tick() {
            frame_count++;
            if (frame_count < 3) window.requestAnimationFrame(tick);
        }
        window.requestAnimationFrame(tick);
    )");

    bridge.poll_async_results();
    REQUIRE(engine.evaluate("frame_count").getWithDefault<int>(-1) >= 1);

    for (int i = 0; i < 10; ++i) {
        bridge.poll_async_results();
        if (engine.evaluate("frame_count").getWithDefault<int>(-1) >= 3)
            break;
    }

    REQUIRE(engine.evaluate("frame_count").getWithDefault<int>(-1) == 3);
}

// pulp #1412 — host idle pump must drain timers, not just rAF + async results.
//
// The platform host idle entry point (Mac CVDisplayLink, iOS CADisplayLink,
// Android AChoreographer) is the only thing that drives the bridge per
// vsync when no input event fires. PRs #1400/#1404/#1405 wired the host
// idle to call poll_async_results(), which only drains async-shell
// results and rAF frame callbacks — NOT setTimeout / setInterval. The
// fix routes the host idle through poll_async_results() AND
// service_frame_callbacks() so timers also fire. These tests exercise
// the combined "host idle pump" pattern directly.

namespace {
// Mirrors what the host idle paths now do per-vsync:
//   ScriptedUiSession::poll() → bridge.poll_async_results()
//                              + bridge.service_frame_callbacks()
//   Android android_render_frame() → same pair.
inline void host_idle_pump(WidgetBridge& bridge) {
    bridge.poll_async_results();
    bridge.service_frame_callbacks();
}
}  // namespace

TEST_CASE("WidgetBridge host idle pump fires setTimeout callbacks",
          "[view][bridge][issue-1412]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var fired = 0;
        setTimeout(function () { fired += 1; }, 50);
    )");

    // Before the deadline, the timer must not fire even after several
    // host idle pumps.
    host_idle_pump(bridge);
    REQUIRE(engine.evaluate("fired").getWithDefault<int>(-1) == 0);

    // Walk past the 50ms deadline with the same per-vsync pump the
    // host idle paths run. With only poll_async_results() this loop
    // would never fire `fired += 1` — that's the #1412 bug.
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        host_idle_pump(bridge);
        if (engine.evaluate("fired").getWithDefault<int>(-1) >= 1)
            break;
    }

    REQUIRE(engine.evaluate("fired").getWithDefault<int>(-1) == 1);
}

TEST_CASE("WidgetBridge host idle pump fires setInterval callbacks repeatedly",
          "[view][bridge][issue-1412]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var hits = 0;
        var id = setInterval(function () {
            hits += 1;
            if (hits >= 3) clearInterval(id);
        }, 50);
    )");

    // Pump on the same cadence the host idle would, walking ~250ms
    // simulated wall time so the interval re-arms ~3 times.
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        host_idle_pump(bridge);
        if (engine.evaluate("hits").getWithDefault<int>(-1) >= 3)
            break;
    }

    REQUIRE(engine.evaluate("hits").getWithDefault<int>(-1) >= 3);
}

TEST_CASE("WidgetBridge host idle pump drains rAF + setTimeout in same call",
          "[view][bridge][issue-1412]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var raf_count = 0;
        var timer_count = 0;
        window.requestAnimationFrame(function () { raf_count += 1; });
        // 0ms timeout: deadline already in the past by the time the
        // host idle pump runs, so it must fire on the first pump.
        setTimeout(function () { timer_count += 1; }, 0);
    )");

    // A single host idle pump must drain BOTH the rAF callback (via
    // poll_async_results → __flushFrames__) AND the expired timer
    // (via service_frame_callbacks → __flushTimers__).
    host_idle_pump(bridge);

    REQUIRE(engine.evaluate("raf_count").getWithDefault<int>(-1) == 1);
    REQUIRE(engine.evaluate("timer_count").getWithDefault<int>(-1) == 1);
}

TEST_CASE("WidgetBridge poll_async_results alone does NOT fire setTimeout (regression guard)",
          "[view][bridge][issue-1412]") {
    // This test is the inverse of the fix: it asserts the historical
    // behavior that was the actual #1412 bug. It documents WHY the
    // host idle pump can't be just poll_async_results().
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var fired = 0;
        setTimeout(function () { fired += 1; }, 0);
    )");

    // Pump only poll_async_results several times across enough wall
    // time that a 0ms timer would have fired if it were drained.
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bridge.poll_async_results();
    }

    REQUIRE(engine.evaluate("fired").getWithDefault<int>(-1) == 0);

    // Now the full host idle pump must drain it.
    host_idle_pump(bridge);
    REQUIRE(engine.evaluate("fired").getWithDefault<int>(-1) == 1);
}

TEST_CASE("WidgetBridge execAsync preserves JSON-heavy results", "[view][bridge][async]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    const std::string async_json_cmd =
#if defined(_WIN32)
        "cmd /c echo {\"message\":\"hello \\\"shader\\\"\",\"shader\":\"line1\\\\nline2\"}";
#else
        "python3 -c \"import json; print(json.dumps({'message':'hello \\\"shader\\\"','shader':'line1\\\\nline2'}))\"";
#endif

    bridge.load_script(
        "var async_json = '';\n"
        "on('__async-json__', 'result', function(value) { async_json = value; });\n"
        "execAsync('" + js_single_quoted(async_json_cmd) + "', '__async-json__');\n");

    REQUIRE(wait_for_async_result(bridge, [&] {
        return engine.evaluate("async_json").toString().find("\"shader\"") != std::string::npos;
    }));
    auto async_json = engine.evaluate("async_json").toString();
    REQUIRE(async_json.find("\"message\"") != std::string::npos);
    REQUIRE(async_json.find("\"shader\"") != std::string::npos);
    REQUIRE(engine.evaluate("JSON.parse(async_json).message").toString() == "hello \"shader\"");
    auto shader = engine.evaluate("JSON.parse(async_json).shader").toString();
    REQUIRE((shader == std::string("line1\nline2") || shader == std::string("line1\\nline2")));
}

TEST_CASE("WidgetBridge execAsync completion is safe after bridge destruction", "[view][bridge][async][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    {
        WidgetBridge bridge(engine, root, store);
        const std::string async_cmd =
#if defined(_WIN32)
            "powershell -NoProfile -Command \"Start-Sleep -Milliseconds 25; [Console]::Out.Write('done')\"";
#else
            "sh -c 'sleep 0.025; printf done'";
#endif
        bridge.load_script(
            "on('__async-destroy__', 'result', function(value) { });\n"
            "execAsync('" + js_single_quoted(async_cmd) + "', '__async-destroy__');\n");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    SUCCEED();
}

TEST_CASE("WidgetBridge timers and storage helpers run through native bridge",
          "[view][bridge][runtime]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var timer_hits = 0;
        var canceled_hits = 0;
        var interval_hits = 0;

        localStorage.removeItem('pulp-widget-bridge-runtime');
        localStorage.setItem('pulp-widget-bridge-runtime', 'stored-value');

        setTimeout(function () { timer_hits += 1; }, -4);
        var canceled = setTimeout(function () { canceled_hits += 1; }, 25);
        clearTimeout(canceled);

        var interval_id = setInterval(function () {
            interval_hits += 1;
            if (interval_hits >= 2)
                clearInterval(interval_id);
        }, 1);
    )");

    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bridge.service_frame_callbacks();
    }

    auto stored = engine.evaluate("localStorage.getItem('pulp-widget-bridge-runtime')");
    REQUIRE(std::string(stored.getWithDefault<std::string_view>("")) == "stored-value");
    REQUIRE(engine.evaluate("timer_hits").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("canceled_hits").getWithDefault<int>(-1) == 0);
    REQUIRE(engine.evaluate("interval_hits").getWithDefault<int>(0) >= 2);

    bridge.load_script("localStorage.removeItem('pulp-widget-bridge-runtime')");
}

TEST_CASE("WidgetBridge loadAssetSync covers embedded file and missing records",
          "[view][bridge][asset]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    static const char kEmbeddedJson[] = "{\"label\":\"pulp\"}";
    AssetManager::instance().register_embedded(
        "coverage/widget-bridge-runtime.json",
        reinterpret_cast<const uint8_t*>(kEmbeddedJson),
        sizeof(kEmbeddedJson) - 1);

    const auto temp_path =
        std::filesystem::temp_directory_path() / "pulp-widget-bridge-runtime-asset.bin";
    {
        std::ofstream out(temp_path, std::ios::binary);
        const char bytes[] = {'\0', '\1', '\2'};
        out.write(bytes, sizeof(bytes));
    }

    const auto file_url = std::string("file://") + js_single_quoted(temp_path.string());
    bridge.load_script(
        "var embedded_asset = __loadAssetSync__('pulp://coverage/widget-bridge-runtime.json');"
        "var file_asset = __loadAssetSync__('" + file_url + "');"
        "var empty_asset = __loadAssetSync__('');"
        "var missing_asset = __loadAssetSync__('pulp://coverage/missing.txt');");

    REQUIRE(engine.evaluate("embedded_asset.ok").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("embedded_asset.status").getWithDefault<int>(0) == 200);
    REQUIRE(std::string(engine.evaluate("embedded_asset.contentType").getWithDefault<std::string_view>("")) ==
            "application/json;charset=utf-8");
    REQUIRE(std::string(engine.evaluate("embedded_asset.text").getWithDefault<std::string_view>("")) ==
            kEmbeddedJson);
    REQUIRE_FALSE(
        std::string(engine.evaluate("embedded_asset.base64").getWithDefault<std::string_view>("")).empty());

    REQUIRE(engine.evaluate("file_asset.ok").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("file_asset.status").getWithDefault<int>(0) == 200);
    REQUIRE(std::string(engine.evaluate("file_asset.contentType").getWithDefault<std::string_view>("")) ==
            "application/octet-stream");
    REQUIRE(std::string(engine.evaluate("file_asset.base64").getWithDefault<std::string_view>("")) == "AAEC");
    REQUIRE(std::string(engine.evaluate("file_asset.text").getWithDefault<std::string_view>("")).empty());

    REQUIRE_FALSE(engine.evaluate("empty_asset.ok").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("empty_asset.status").getWithDefault<int>(0) == 400);
    REQUIRE_FALSE(engine.evaluate("missing_asset.ok").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("missing_asset.status").getWithDefault<int>(0) == 404);

    std::filesystem::remove(temp_path);
}

TEST_CASE("WidgetBridge loadAssetSync covers text mime variants and path normalization",
          "[view][bridge][asset][coverage]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    static const char kEmbeddedCss[] = "body { color: red; }\n";
    static const char kEmbeddedSvg[] = "<svg><path d='M0 0h1v1z'/></svg>";
    AssetManager::instance().register_embedded(
        "coverage/widget-bridge-style.CSS",
        reinterpret_cast<const uint8_t*>(kEmbeddedCss),
        sizeof(kEmbeddedCss) - 1);
    AssetManager::instance().register_embedded(
        "coverage/widget-bridge-icon.svg",
        reinterpret_cast<const uint8_t*>(kEmbeddedSvg),
        sizeof(kEmbeddedSvg) - 1);

    const auto script_path =
        std::filesystem::temp_directory_path() / "pulp-widget-bridge-runtime-asset.mjs";
    {
        std::ofstream out(script_path, std::ios::binary);
        out << "export const value = 3;\n";
    }

    const auto script_url = std::string("file://") + js_single_quoted(script_path.string());
    bridge.load_script(
        "var css_asset = __loadAssetSync__('pulp:////coverage/widget-bridge-style.CSS');"
        "var svg_asset = __loadAssetSync__('coverage/widget-bridge-icon.svg');"
        "var script_asset = __loadAssetSync__('" + script_url + "');");

    REQUIRE(engine.evaluate("css_asset.ok").getWithDefault<bool>(false));
    REQUIRE(std::string(engine.evaluate("css_asset.resolvedPath").getWithDefault<std::string_view>("")) ==
            "coverage/widget-bridge-style.CSS");
    REQUIRE(std::string(engine.evaluate("css_asset.contentType").getWithDefault<std::string_view>("")) ==
            "text/css");
    REQUIRE(std::string(engine.evaluate("css_asset.text").getWithDefault<std::string_view>("")) ==
            kEmbeddedCss);
    REQUIRE_FALSE(
        std::string(engine.evaluate("css_asset.base64").getWithDefault<std::string_view>("")).empty());

    REQUIRE(engine.evaluate("svg_asset.ok").getWithDefault<bool>(false));
    REQUIRE(std::string(engine.evaluate("svg_asset.contentType").getWithDefault<std::string_view>("")) ==
            "image/svg+xml");
    REQUIRE(std::string(engine.evaluate("svg_asset.text").getWithDefault<std::string_view>("")) ==
            kEmbeddedSvg);

    REQUIRE(engine.evaluate("script_asset.ok").getWithDefault<bool>(false));
    REQUIRE(std::string(engine.evaluate("script_asset.contentType").getWithDefault<std::string_view>("")) ==
            "text/javascript");
    REQUIRE(std::string(engine.evaluate("script_asset.text").getWithDefault<std::string_view>("")) ==
            "export const value = 3;\n");

    std::filesystem::remove(script_path);
}

TEST_CASE("WidgetBridge registerDrop dispatches escaped payloads to JS",
          "[view][bridge][dnd][coverage]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var drop_type = '';
        var drop_data = '';
        var drop_x = 0;
        var drop_y = 0;
        function handleDrop(type, data, x, y) {
            drop_type = type;
            drop_data = data;
            drop_x = x;
            drop_y = y;
        }
        createPanel('dropzone', '');
        registerDrop('dropzone', 'handleDrop');
    )");

    auto* dropzone = bridge.widget("dropzone");
    REQUIRE(dropzone != nullptr);
    REQUIRE(static_cast<bool>(dropzone->on_drop));

    dropzone->on_drop("text", "line one\nit's fine", 12.5f, 30.25f);

    REQUIRE(engine.evaluate("drop_type").toString() == "text");
    REQUIRE(engine.evaluate("drop_data").toString() == "line one\nit's fine");
    REQUIRE_THAT(engine.evaluate("drop_x").getWithDefault<double>(0.0),
                 WithinAbs(12.5, 1e-4));
    REQUIRE_THAT(engine.evaluate("drop_y").getWithDefault<double>(0.0),
                 WithinAbs(30.25, 1e-4));

    bridge.load_script("registerDrop('dropzone', '')");
    REQUIRE(static_cast<bool>(dropzone->on_drop));

    bridge.load_script("createPanel('no-drop', ''); registerDrop('no-drop', '')");
    auto* inert = bridge.widget("no-drop");
    REQUIRE(inert != nullptr);
    REQUIRE_FALSE(static_cast<bool>(inert->on_drop));
}

TEST_CASE("WidgetBridge registerContextMenu dispatches native menu position",
          "[view][bridge][context-menu][coverage]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var menu_x = -1;
        var menu_y = -1;
        function handleMenu(x, y) {
            menu_x = x;
            menu_y = y;
        }
        createPanel('menu-target', '');
        registerContextMenu('menu-target', 'handleMenu');
    )");

    auto* target = bridge.widget("menu-target");
    REQUIRE(target != nullptr);
    REQUIRE(static_cast<bool>(target->on_context_menu));

    target->on_context_menu({42.5f, 19.25f});

    REQUIRE_THAT(engine.evaluate("menu_x").getWithDefault<double>(0.0),
                 WithinAbs(42.5, 1e-4));
    REQUIRE_THAT(engine.evaluate("menu_y").getWithDefault<double>(0.0),
                 WithinAbs(19.25, 1e-4));

    bridge.load_script("createPanel('menu-inert', ''); registerContextMenu('menu-inert', '')");
    auto* inert = bridge.widget("menu-inert");
    REQUIRE(inert != nullptr);
    REQUIRE_FALSE(static_cast<bool>(inert->on_context_menu));
}

TEST_CASE("WidgetBridge loadFont reports existing and missing paths",
          "[view][bridge][font][coverage]")
{
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto font_path =
        std::filesystem::temp_directory_path() /
        ("pulp-widget-bridge-font-" + unique + ".ttf");
    {
        std::ofstream out(font_path, std::ios::binary);
        out << "fake-font";
    }

    const auto existing = js_single_quoted(font_path.string());
    const auto missing = js_single_quoted((font_path.string() + ".missing"));
    bridge.load_script(
        "var font_existing = loadFont('" + existing + "');"
        "var font_missing = loadFont('" + missing + "');"
        "var font_empty = loadFont('');");

    REQUIRE(engine.evaluate("font_existing").getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate("font_missing").getWithDefault<bool>(true));
    REQUIRE_FALSE(engine.evaluate("font_empty").getWithDefault<bool>(true));

    std::error_code ec;
    std::filesystem::remove(font_path, ec);
}

TEST_CASE("WidgetBridge text editor escape dispatches JS handler", "[view][bridge][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var escaped = 0;
        createTextEditor('field', '');
        on('field', 'escape', function() { escaped++; });
    )");

    auto* field = dynamic_cast<TextEditor*>(bridge.widget("field"));
    REQUIRE(field != nullptr);

    KeyEvent esc{};
    esc.is_down = true;
    esc.key = KeyCode::escape;
    REQUIRE(field->on_key_event(esc));
    REQUIRE(engine.evaluate("escaped").getWithDefault<int>(0) == 1);
}

