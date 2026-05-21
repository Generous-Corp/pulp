// JSON-RPC domain-handler + console/audio/state/perf/live-constant tests, split from test_inspector.cpp (P11-5, #2647).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/render/dirty_tracker.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/live_constant_editor.hpp>
#include <pulp/state/store.hpp>
#include <choc/containers/choc_Value.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

// ── ConsoleCapture ──────────────────────────────────────────────────────────

#include <pulp/inspect/console_capture.hpp>

TEST_CASE("ConsoleCapture: captures log entries") {
    ConsoleCapture capture;
    auto cb = capture.callback();
    cb("log", "hello");
    cb("warn", "caution");
    cb("error", "fail");
    auto entries = capture.entries();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].level == "log");
    REQUIRE(entries[0].message == "hello");
    REQUIRE(entries[2].level == "error");
}

TEST_CASE("ConsoleCapture: chains previous callback") {
    std::string captured;
    auto previous = [&](std::string_view level, std::string_view msg) {
        captured = std::string(level) + ":" + std::string(msg);
    };
    ConsoleCapture capture;
    auto cb = capture.callback(previous);
    cb("log", "test");
    REQUIRE(captured == "log:test");
    REQUIRE(capture.entries().size() == 1);
}

TEST_CASE("ConsoleCapture: clear") {
    ConsoleCapture capture;
    auto cb = capture.callback();
    cb("log", "a");
    cb("log", "b");
    capture.clear();
    REQUIRE(capture.entries().empty());
}

TEST_CASE("ConsoleCapture: retains the newest ring buffer entries", "[inspect][console][issue-641]") {
    ConsoleCapture capture;
    auto cb = capture.callback();

    for (int i = 0; i < 205; ++i)
        cb("log", "entry-" + std::to_string(i));

    auto entries = capture.entries();
    REQUIRE(entries.size() == 200);
    REQUIRE(entries.front().message == "entry-5");
    REQUIRE(entries.back().message == "entry-204");

    capture.clear();
    REQUIRE(capture.entries().empty());
}

// ── AudioInspector ──────────────────────────────────────────────────────────

#include <pulp/inspect/audio_inspector.hpp>

TEST_CASE("AudioInspector: config roundtrip") {
    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 256;
    cfg.output_channels = 2;
    audio.set_config(cfg);
    auto read = audio.config();
    REQUIRE(read.sample_rate == 48000);
    REQUIRE(read.buffer_size == 256);
}

TEST_CASE("AudioInspector: MIDI logging") {
    AudioInspector audio;
    audio.log_midi(0x90, 60, 100, "Note On C4");
    audio.log_midi(0x80, 60, 0, "Note Off C4");
    auto events = audio.recent_midi();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].status == 0x90);
    REQUIRE(events[0].description == "Note On C4");
}

TEST_CASE("AudioInspector: metering gates level snapshots") {
    AudioInspector audio;

    audio.report_levels({{0.5f, 0.25f}});
    REQUIRE(audio.latest_levels().empty());

    audio.set_metering_enabled(true);
    audio.report_levels({{0.8f, 0.4f}, {0.25f, 0.1f}});

    auto levels = audio.latest_levels();
    REQUIRE(levels.size() == 2);
    REQUIRE(levels[0].peak == 0.8f);
    REQUIRE(levels[1].rms == 0.1f);
}

// ── DomainHandler ───────────────────────────────────────────────────────────

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/state/store.hpp>

TEST_CASE("DomainHandler: unknown domain") {
    DomainHandler handler;
    auto resp = handler.handle(make_request(1, "Bogus.method"));
    REQUIRE(resp.is_error);
}

TEST_CASE("DomainHandler: rejects malformed dispatch and missing inspect roots", "[inspect][domain][issue-641]") {
    DomainHandler handler;

    auto invalid = handler.handle(make_request(1, "DOMGetDocument"));
    REQUIRE(invalid.is_error);
    REQUIRE(invalid.params_json == "Invalid method: DOMGetDocument");

    auto unknown = handler.handle(make_request(2, "Bogus.method"));
    REQUIRE(unknown.is_error);
    REQUIRE(unknown.params_json == "Unknown domain: Bogus");

    auto dom_missing_root = handler.handle(make_request(3, methods::kDOMGetDocument));
    REQUIRE(dom_missing_root.is_error);
    REQUIRE(dom_missing_root.params_json == "No root view attached");

    auto css_missing_root = handler.handle(make_request(4, methods::kCSSGetComputedStyle, R"({"id":"root"})"));
    REQUIRE(css_missing_root.is_error);
    REQUIRE(css_missing_root.params_json == "No root view attached");
}

TEST_CASE("DomainHandler: Inspector.getInfo") {
    View root;
    DomainHandler handler;
    handler.set_root_view(&root);
    auto resp = handler.handle(make_request(1, "Inspector.getInfo"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("Pulp") != std::string::npos);
}

TEST_CASE("DomainHandler: DOM.getDocument") {
    View root;
    auto child = std::make_unique<View>();
    child->set_id("child1");
    root.add_child(std::move(child));
    DomainHandler handler;
    handler.set_root_view(&root);
    auto resp = handler.handle(make_request(1, "DOM.getDocument"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("child1") != std::string::npos);
}

TEST_CASE("DomainHandler: DOM and CSS reject malformed params", "[inspect][domain][issue-641]") {
    View root;
    root.set_id("root");

    DomainHandler handler;
    handler.set_root_view(&root);

    auto dom_bad_json = handler.handle(make_request(1, methods::kDOMGetNodeById, "{"));
    REQUIRE(dom_bad_json.is_error);
    REQUIRE(dom_bad_json.params_json == "Invalid params for DOM.getNodeById");

    auto dom_missing_id = handler.handle(make_request(2, methods::kDOMGetNodeById, R"({"nodeId":"root"})"));
    REQUIRE(dom_missing_id.is_error);
    REQUIRE(dom_missing_id.params_json == "Invalid params for DOM.getNodeById");

    auto search_bad_json = handler.handle(make_request(3, methods::kDOMSearch, "{"));
    REQUIRE(search_bad_json.is_error);
    REQUIRE(search_bad_json.params_json == "Invalid params for DOM.search");

    auto css_bad_json = handler.handle(make_request(4, methods::kCSSGetComputedStyle, "{"));
    REQUIRE(css_bad_json.is_error);
    REQUIRE(css_bad_json.params_json == "Invalid params for CSS.getComputedStyle");

    auto css_missing_id = handler.handle(make_request(5, methods::kCSSGetComputedStyle, R"({"nodeId":"root"})"));
    REQUIRE(css_missing_id.is_error);
    REQUIRE(css_missing_id.params_json == "Invalid params for CSS.getComputedStyle");
}

TEST_CASE("DomainHandler: State.getParameters") {
    StateStore store;
    store.add_parameter({0, "Volume", "dB", {-60.0f, 6.0f, -12.0f}});
    store.set_value(0, -6.0f);
    StateInspector state_inspector(store);
    DomainHandler handler;
    handler.set_state_inspector(&state_inspector);
    auto resp = handler.handle(make_request(1, "State.getParameters"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("Volume") != std::string::npos);
}

TEST_CASE("DomainHandler: Audio domain exposes config and MIDI log") {
    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 128;
    cfg.input_channels = 1;
    cfg.output_channels = 2;
    cfg.latency_samples = 64;
    audio.set_config(cfg);
    audio.log_midi(0x90, 60, 100, "Note On C4");

    DomainHandler handler;
    handler.set_audio_inspector(&audio);

    auto config = handler.handle(make_request(1, methods::kAudioGetConfig));
    REQUIRE_FALSE(config.is_error);
    REQUIRE(config.params_json.find("\"sample_rate\"") != std::string::npos);
    REQUIRE(config.params_json.find("48000") != std::string::npos);
    REQUIRE(config.params_json.find("\"buffer_size\"") != std::string::npos);
    REQUIRE(config.params_json.find("128") != std::string::npos);
    REQUIRE(config.params_json.find("\"latency_samples\"") != std::string::npos);
    REQUIRE(config.params_json.find("64") != std::string::npos);

    REQUIRE_FALSE(audio.metering_enabled());
    auto metering = handler.handle(make_request(2, methods::kAudioEnableMetering));
    REQUIRE_FALSE(metering.is_error);
    REQUIRE(audio.metering_enabled());
    REQUIRE(metering.params_json.find("\"metering\":true") != std::string::npos);

    auto midi = handler.handle(make_request(3, methods::kAudioGetMidiLog));
    REQUIRE_FALSE(midi.is_error);
    REQUIRE(midi.params_json.find("\"status\"") != std::string::npos);
    REQUIRE(midi.params_json.find("144") != std::string::npos);
    REQUIRE(midi.params_json.find("\"data1\"") != std::string::npos);
    REQUIRE(midi.params_json.find("60") != std::string::npos);
    REQUIRE(midi.params_json.find("\"description\"") != std::string::npos);
    REQUIRE(midi.params_json.find("Note On C4") != std::string::npos);

    auto unknown = handler.handle(make_request(4, "Audio.unknown"));
    REQUIRE(unknown.is_error);

    DomainHandler missing_audio;
    auto missing = missing_audio.handle(make_request(5, methods::kAudioGetConfig));
    REQUIRE(missing.is_error);
}

TEST_CASE("DomainHandler: dispatches inspector domain edge paths", "[inspect][domain][issue-641]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 320, 200});
    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(12, 34, 56);
    root.set_theme(theme);

    auto child = std::make_unique<Label>("Child");
    child->set_id("child");
    child->set_bounds({5, 6, 70, 20});
    child->set_opacity(0.5f);
    child->set_visible(false);
    child->flex().direction = FlexDirection::row;
    child->flex().gap = 8;
    child->flex().padding = 3;
    child->flex().margin = 2;
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    ConsoleCapture capture;
    auto console = capture.callback();
    console("info", "hello");

    StateStore store;
    ParamInfo gain;
    gain.id = 9;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-60.0f, 12.0f, 0.0f, 0.5f};
    store.add_parameter(gain);
    StateInspector state(store);

    AudioInspector audio;
    AudioConfig cfg;
    cfg.sample_rate = 44100;
    cfg.buffer_size = 256;
    audio.set_config(cfg);

    pulp::render::RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(pulp::render::RenderPassType::content);
    rpm.end_pass(4.0f, 11);
    rpm.end_frame();

    DomainHandler handler;
    handler.set_root_view(&root);
    handler.set_overlay(&overlay);
    handler.set_console_capture(&capture);
    handler.set_state_inspector(&state);
    handler.set_audio_inspector(&audio);
    handler.set_render_pass_manager(&rpm);

    auto invalid = handler.handle(make_request(1, "InvalidMethod"));
    REQUIRE(invalid.is_error);
    REQUIRE(invalid.params_json.find("Invalid method") != std::string::npos);

    auto enabled = handler.handle(make_request(2, methods::kInspectorEnable));
    REQUIRE_FALSE(enabled.is_error);
    REQUIRE(overlay.is_active());

    auto disabled = handler.handle(make_request(3, methods::kInspectorDisable));
    REQUIRE_FALSE(disabled.is_error);
    REQUIRE_FALSE(overlay.is_active());

    auto unknown_inspector = handler.handle(make_request(4, "Inspector.nope"));
    REQUIRE(unknown_inspector.is_error);

    auto node = handler.handle(make_request(5, methods::kDOMGetNodeById, R"({"id":"child"})"));
    REQUIRE_FALSE(node.is_error);
    REQUIRE(node.params_json.find("\"id\"") != std::string::npos);
    REQUIRE(node.params_json.find("child") != std::string::npos);
    REQUIRE(node.params_json.find("child_count") != std::string::npos);

    auto missing_node = handler.handle(make_request(6, methods::kDOMGetNodeById, R"({"id":"missing"})"));
    REQUIRE(missing_node.is_error);

    auto bad_node_params = handler.handle(make_request(7, methods::kDOMGetNodeById, "not json"));
    REQUIRE(bad_node_params.is_error);

    auto highlight = handler.handle(make_request(8, methods::kDOMHighlightNode));
    REQUIRE_FALSE(highlight.is_error);
    auto clear = handler.handle(make_request(9, methods::kDOMClearHighlight));
    REQUIRE_FALSE(clear.is_error);

    auto search = handler.handle(make_request(10, methods::kDOMSearch, R"({"query":"child"})"));
    REQUIRE_FALSE(search.is_error);
    REQUIRE(search.params_json.find("child") != std::string::npos);

    auto bad_search = handler.handle(make_request(11, methods::kDOMSearch, "not json"));
    REQUIRE(bad_search.is_error);

    auto unknown_dom = handler.handle(make_request(12, "DOM.nope"));
    REQUIRE(unknown_dom.is_error);

    auto style = handler.handle(make_request(13, methods::kCSSGetComputedStyle, R"({"id":"child"})"));
    REQUIRE_FALSE(style.is_error);
    REQUIRE(style.params_json.find("direction") != std::string::npos);
    REQUIRE(style.params_json.find("row") != std::string::npos);
    REQUIRE(style.params_json.find("visible") != std::string::npos);
    REQUIRE(style.params_json.find("false") != std::string::npos);

    auto missing_style = handler.handle(make_request(14, methods::kCSSGetComputedStyle, R"({"id":"missing"})"));
    REQUIRE(missing_style.is_error);

    auto bad_style = handler.handle(make_request(15, methods::kCSSGetComputedStyle, "not json"));
    REQUIRE(bad_style.is_error);

    auto theme_resp = handler.handle(make_request(16, methods::kCSSGetTheme));
    REQUIRE_FALSE(theme_resp.is_error);
    REQUIRE(theme_resp.params_json.find("accent.primary") != std::string::npos);

    auto unknown_css = handler.handle(make_request(17, "CSS.nope"));
    REQUIRE(unknown_css.is_error);

    auto perf = handler.handle(make_request(18, methods::kPerfGetMetrics));
    REQUIRE_FALSE(perf.is_error);
    REQUIRE(perf.params_json.find("total_time_ms") != std::string::npos);
    REQUIRE(perf.params_json.find("draw_calls") != std::string::npos);
    REQUIRE(perf.params_json.find("11") != std::string::npos);

    auto tracking = handler.handle(make_request(19, methods::kPerfEnableTracking));
    REQUIRE_FALSE(tracking.is_error);

    auto unknown_perf = handler.handle(make_request(20, "Performance.nope"));
    REQUIRE(unknown_perf.is_error);

    auto set_param = handler.handle(make_request(21, methods::kStateSetParameter, R"({"id":9,"value":-12.5})"));
    REQUIRE_FALSE(set_param.is_error);
    REQUIRE(store.get_value(9) == -12.5f);

    auto bad_set_param = handler.handle(make_request(22, methods::kStateSetParameter, "not json"));
    REQUIRE(bad_set_param.is_error);

    auto unknown_state = handler.handle(make_request(23, "State.nope"));
    REQUIRE(unknown_state.is_error);

    auto console_entries = handler.handle(make_request(24, methods::kConsoleEnable));
    REQUIRE_FALSE(console_entries.is_error);
    REQUIRE(console_entries.params_json.find("hello") != std::string::npos);

    auto unknown_console = handler.handle(make_request(25, "Console.nope"));
    REQUIRE(unknown_console.is_error);

    auto runtime_eval = handler.handle(make_request(26, methods::kRuntimeEvaluate));
    REQUIRE(runtime_eval.is_error);

    auto hot_reload = handler.handle(make_request(27, methods::kRuntimeGetHotReloadStatus));
    REQUIRE_FALSE(hot_reload.is_error);
    REQUIRE(hot_reload.params_json.find("available") != std::string::npos);
    REQUIRE(hot_reload.params_json.find("false") != std::string::npos);

    auto unknown_runtime = handler.handle(make_request(28, "Runtime.nope"));
    REQUIRE(unknown_runtime.is_error);

    auto screenshot = handler.handle(make_request(29, methods::kCaptureScreenshot));
    REQUIRE(screenshot.is_error);
    auto screenshot_node = handler.handle(make_request(30, methods::kCaptureScreenshotNode));
    REQUIRE(screenshot_node.is_error);
    auto unknown_capture = handler.handle(make_request(31, "Capture.nope"));
    REQUIRE(unknown_capture.is_error);

    DomainHandler missing_sources;
    REQUIRE(missing_sources.handle(make_request(32, methods::kDOMGetDocument)).is_error);
    REQUIRE(missing_sources.handle(make_request(33, methods::kCSSGetTheme)).is_error);
    REQUIRE(missing_sources.handle(make_request(34, methods::kStateGetParameters)).is_error);
    REQUIRE(missing_sources.handle(make_request(35, methods::kAudioGetConfig)).is_error);

    auto no_console = missing_sources.handle(make_request(36, methods::kConsoleEnable));
    REQUIRE_FALSE(no_console.is_error);
    REQUIRE(no_console.params_json == "[]");

    auto no_perf = missing_sources.handle(make_request(37, methods::kPerfGetMetrics));
    REQUIRE_FALSE(no_perf.is_error);
    REQUIRE(no_perf.params_json.find("available") != std::string::npos);
    REQUIRE(no_perf.params_json.find("false") != std::string::npos);
}

// ─── StateInspector ListenerToken migration (Slice 3) ───────────────────────

TEST_CASE("StateInspector records parameter changes after subscribing",
          "[inspect][state][listener]") {
    StateStore store;
    ParamInfo info;
    info.id = 42;
    info.name = "Cutoff";
    info.unit = "Hz";
    info.range = {20.0f, 20000.0f, 1000.0f};
    store.add_parameter(info);

    StateInspector inspector(store);

    REQUIRE(inspector.recent_changes().empty());

    store.set_value(42, 2400.0f);
    store.set_value(42, 4800.0f);

    auto changes = inspector.recent_changes();
    REQUIRE(changes.size() == 2);
    REQUIRE(changes[0].id == 42);
    REQUIRE(changes[1].value > changes[0].value);
}

TEST_CASE("Destroying StateInspector removes its listener (no alive-guard)",
          "[inspect][state][listener]") {
    StateStore store;
    ParamInfo info;
    info.id = 1;
    info.name = "Gain";
    info.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(info);

    {
        StateInspector inspector(store);
        store.set_value(1, 0.25f);
        REQUIRE(inspector.recent_changes().size() == 1);
    } // ~StateInspector() — ListenerToken dtor unregisters

    // The store no longer has a live listener pointing at the
    // destroyed inspector. With the legacy alive-guard pattern, an
    // entry was still in the listener list (just no-op-checking
    // alive). With ListenerToken it's actually removed, so this
    // set_value is a pure atomic store + a notify() that iterates
    // an empty snapshot. No use-after-free, no leak.
    store.set_value(1, 0.75f);
    REQUIRE_THAT(store.get_value(1), Catch::Matchers::WithinAbs(0.75, 0.001));
}

// ─── Performance.setRepaintFlash (Tier A Slice 6) ───────────────────────────

#include <pulp/render/dirty_tracker.hpp>

TEST_CASE("Performance.setRepaintFlash toggles DirtyTracker::debug_overlay",
          "[inspect][perf][repaint-flash]") {
    pulp::render::DirtyTracker dirty;
    REQUIRE_FALSE(dirty.debug_overlay());

    DomainHandler handler;
    handler.set_dirty_tracker(&dirty);

    auto enable_req = make_request(1, methods::kPerfSetRepaintFlash,
                                   R"({"enabled":true})");
    auto enable_resp = handler.handle(enable_req);
    REQUIRE_FALSE(enable_resp.is_error);
    REQUIRE(dirty.debug_overlay());

    auto get_req = make_request(2, methods::kPerfGetRepaintFlash);
    auto get_resp = handler.handle(get_req);
    REQUIRE_FALSE(get_resp.is_error);
    REQUIRE(get_resp.params_json.find("\"enabled\": true")
            != std::string::npos);
    REQUIRE(get_resp.params_json.find("\"available\": true")
            != std::string::npos);

    auto disable_req = make_request(3, methods::kPerfSetRepaintFlash,
                                    R"({"enabled":false})");
    auto disable_resp = handler.handle(disable_req);
    REQUIRE_FALSE(disable_resp.is_error);
    REQUIRE_FALSE(dirty.debug_overlay());
}

TEST_CASE("Performance.setRepaintFlash without a tracker reports unavailable",
          "[inspect][perf][repaint-flash]") {
    DomainHandler handler;
    // Deliberately not calling set_dirty_tracker — the inspector
    // grew the toggle, but the host process may not have wired one
    // yet. Behavior: get reports available=false; set returns an
    // error so the UI can grey out the toggle.
    auto get_resp = handler.handle(
        make_request(1, methods::kPerfGetRepaintFlash));
    REQUIRE_FALSE(get_resp.is_error);
    REQUIRE(get_resp.params_json.find("\"available\": false")
            != std::string::npos);

    auto set_resp = handler.handle(
        make_request(2, methods::kPerfSetRepaintFlash,
                     R"({"enabled":true})"));
    REQUIRE(set_resp.is_error);
}

// ─── LiveConstant RPC (Tier A Slice 13) ─────────────────────────────────────

#include <pulp/view/live_constant_editor.hpp>

TEST_CASE("LiveConstant.list returns the registry contents",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();

    // Seed the registry. PULP_LIVE_CONSTANT macros do this implicitly,
    // but we call register_constant directly so the test doesn't have
    // to compile in a TU that already has them.
    [[maybe_unused]] auto& cutoff =
        registry.register_constant("test_cutoff", __FILE__, __LINE__,
                                   /*default*/ 440.0f,
                                   /*min*/ 20.0f, /*max*/ 20000.0f);
    [[maybe_unused]] auto& gain =
        registry.register_constant("test_gain", __FILE__, __LINE__,
                                   0.0f, -60.0f, 12.0f);

    DomainHandler handler;
    auto resp = handler.handle(make_request(1, methods::kLiveConstList));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.params_json.find("test_cutoff") != std::string::npos);
    REQUIRE(resp.params_json.find("test_gain") != std::string::npos);
    REQUIRE(resp.params_json.find("\"constants\"") != std::string::npos);
}

TEST_CASE("LiveConstant.set mutates the registry value",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();
    [[maybe_unused]] auto& v =
        registry.register_constant("test_setter", __FILE__, __LINE__,
                                   1.0f, 0.0f, 10.0f);
    registry.reset("test_setter");

    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstSet,
        R"({"name":"test_setter","value":4.5})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE_THAT(registry.get("test_setter"),
                 Catch::Matchers::WithinAbs(4.5, 0.001));
}

TEST_CASE("LiveConstant.set without a name returns an error",
          "[inspect][live-constant]") {
    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstSet, R"({"value":1.0})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("LiveConstant.reset rolls a value back to its default",
          "[inspect][live-constant]") {
    auto& registry = pulp::view::LiveConstantRegistry::instance();
    [[maybe_unused]] auto& v =
        registry.register_constant("test_reset", __FILE__, __LINE__,
                                   2.0f, 0.0f, 10.0f);
    registry.set("test_reset", 7.5f);
    REQUIRE_THAT(registry.get("test_reset"),
                 Catch::Matchers::WithinAbs(7.5, 0.001));

    DomainHandler handler;
    auto resp = handler.handle(make_request(
        1, methods::kLiveConstReset, R"({"name":"test_reset"})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE_THAT(registry.get("test_reset"),
                 Catch::Matchers::WithinAbs(2.0, 0.001));
}
