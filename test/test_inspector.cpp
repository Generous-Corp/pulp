#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>

#include <string_view>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

template <typename T>
void collect_views_of_type(View& root, std::vector<T*>& out) {
    if (auto* match = dynamic_cast<T*>(&root))
        out.push_back(match);
    for (size_t i = 0; i < root.child_count(); ++i)
        collect_views_of_type(*root.child_at(i), out);
}

template <typename T>
std::vector<T*> collect_views_of_type(View& root) {
    std::vector<T*> out;
    collect_views_of_type(root, out);
    return out;
}

template <typename T>
T* first_view_of_type(View& root) {
    auto matches = collect_views_of_type<T>(root);
    return matches.empty() ? nullptr : matches.front();
}

bool has_label_containing(View& root, std::string_view text) {
    auto labels = collect_views_of_type<Label>(root);
    for (auto* label : labels) {
        if (label->text().find(text) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("ViewInspector type_name", "[view][inspector]") {
    View v;
    Knob k;
    Fader f;
    Toggle t;
    Label l;
    Meter m;
    XYPad xy;
    WaveformView wf;
    SpectrumView sp;

    REQUIRE(ViewInspector::type_name(v) == "View");
    REQUIRE(ViewInspector::type_name(k) == "Knob");
    REQUIRE(ViewInspector::type_name(f) == "Fader");
    REQUIRE(ViewInspector::type_name(t) == "Toggle");
    REQUIRE(ViewInspector::type_name(l) == "Label");
    REQUIRE(ViewInspector::type_name(m) == "Meter");
    REQUIRE(ViewInspector::type_name(xy) == "XYPad");
    REQUIRE(ViewInspector::type_name(wf) == "WaveformView");
    REQUIRE(ViewInspector::type_name(sp) == "SpectrumView");
}

TEST_CASE("ViewInspector count_views", "[view][inspector]") {
    View root;
    root.add_child(std::make_unique<Knob>());
    root.add_child(std::make_unique<Fader>());

    auto panel = std::make_unique<View>();
    panel->add_child(std::make_unique<Label>("test"));
    root.add_child(std::move(panel));

    REQUIRE(ViewInspector::count_views(root) == 5); // root + 2 + panel + label
}

TEST_CASE("ViewInspector find_by_id", "[view][inspector]") {
    View root;
    root.set_id("root");

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    auto fader = std::make_unique<Fader>();
    fader->set_id("volume");
    root.add_child(std::move(fader));

    REQUIRE(ViewInspector::find_by_id(root, "root") == &root);
    REQUIRE(ViewInspector::find_by_id(root, "gain") == knob_ptr);
    REQUIRE(ViewInspector::find_by_id(root, "nonexistent") == nullptr);
}

TEST_CASE("ViewInspector to_json", "[view][inspector]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 400, 300});

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({10, 10, 48, 48});
    knob->set_value(0.75f);
    knob->set_label("Gain");
    root.add_child(std::move(knob));

    auto toggle = std::make_unique<Toggle>();
    toggle->set_id("bypass");
    toggle->set_on(true);
    root.add_child(std::move(toggle));

    auto json = ViewInspector::to_json(root);
    REQUIRE_FALSE(json.empty());

    // Verify key elements are present in the JSON
    REQUIRE(json.find("\"root\"") != std::string::npos);
    REQUIRE(json.find("\"Knob\"") != std::string::npos);
    REQUIRE(json.find("\"gain\"") != std::string::npos);
    REQUIRE(json.find("\"Toggle\"") != std::string::npos);
    REQUIRE(json.find("\"bypass\"") != std::string::npos);
    REQUIRE(json.find("children") != std::string::npos);
}

TEST_CASE("ViewInspector JSON includes widget values", "[view][inspector]") {
    View root;

    auto fader = std::make_unique<Fader>();
    fader->set_value(0.6f);
    fader->set_orientation(Fader::Orientation::horizontal);
    root.add_child(std::move(fader));

    auto label = std::make_unique<Label>("Hello");
    root.add_child(std::move(label));

    auto json = ViewInspector::to_json(root);
    REQUIRE(json.find("\"horizontal\"") != std::string::npos);
    REQUIRE(json.find("\"Hello\"") != std::string::npos);
}

// ── Protocol encode/decode ──────────────────────────────────────────────────

#include <pulp/inspect/protocol.hpp>

TEST_CASE("Protocol: encode request") {
    auto msg = make_request(1, "DOM.getDocument", R"({"depth":1})");
    auto json = encode_message(msg);
    REQUIRE(json.find("\"id\"") != std::string::npos);
    REQUIRE(json.find("DOM.getDocument") != std::string::npos);
}

TEST_CASE("Protocol: encode response") {
    auto msg = make_response(1, R"({"root":{}})");
    auto json = encode_message(msg);
    REQUIRE(json.find("\"id\"") != std::string::npos);
    REQUIRE(json.find("root") != std::string::npos);
}

TEST_CASE("Protocol: encode error") {
    auto msg = make_error(1, "View not found");
    auto json = encode_message(msg);
    REQUIRE(json.find("error") != std::string::npos);
    REQUIRE(json.find("View not found") != std::string::npos);
}

TEST_CASE("Protocol: encode event has no id") {
    auto msg = make_event("DOM.documentUpdated");
    auto json = encode_message(msg);
    REQUIRE(json.find("DOM.documentUpdated") != std::string::npos);
}

TEST_CASE("Protocol: decode roundtrip") {
    auto original = make_request(42, "State.getParameters", R"({"filter":"gain"})");
    auto json = encode_message(original);

    InspectorMessage decoded;
    REQUIRE(decode_message(json, decoded));
    REQUIRE(decoded.id == 42);
    REQUIRE(decoded.method == "State.getParameters");
}

TEST_CASE("Protocol: decode invalid JSON") {
    InspectorMessage msg;
    REQUIRE_FALSE(decode_message("not json", msg));
}

// ── InspectorOverlay ────────────────────────────────────────────────────────

#include <pulp/inspect/inspector_overlay.hpp>

TEST_CASE("InspectorOverlay: toggle") {
    View root;
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.is_active());
    overlay.set_active(true);
    REQUIRE(overlay.is_active());
    overlay.toggle();
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: Cmd+I toggles") {
    View root;
    InspectorOverlay overlay(root);
    KeyEvent ke;
    ke.key = KeyCode::i;
#ifdef __APPLE__
    ke.modifiers = kModCmd;
#else
    ke.modifiers = kModCtrl;  // Ctrl+I on Linux/Windows
#endif
    ke.is_down = true;
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE(overlay.is_active());
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: Escape dismisses") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    KeyEvent ke;
    ke.key = KeyCode::escape;
    ke.is_down = true;
    REQUIRE(overlay.handle_key_event(ke));
    REQUIRE_FALSE(overlay.is_active());
}

TEST_CASE("InspectorOverlay: inactive ignores events") {
    View root;
    InspectorOverlay overlay(root);
    KeyEvent ke;
    ke.key = KeyCode::escape;
    ke.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(ke));
    MouseEvent me;
    me.position = {10, 10};
    me.is_down = true;
    REQUIRE_FALSE(overlay.handle_mouse_event(me));
}

TEST_CASE("InspectorOverlay: mouse selection and panel tree interactions", "[inspect][overlay][issue-641]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 500, 300});

    auto first = std::make_unique<View>();
    first->set_id("first");
    first->set_bounds({10, 10, 80, 40});
    first->flex().padding = 3;
    first->flex().margin = 4;
    auto* first_ptr = first.get();
    root.add_child(std::move(first));

    auto second = std::make_unique<View>();
    second->set_id("second");
    second->set_bounds({120, 10, 80, 40});
    auto* second_ptr = second.get();
    root.add_child(std::move(second));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    MouseEvent hover;
    hover.position = {20, 20};
    hover.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover));
    REQUIRE(overlay.hovered_view() == first_ptr);

    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == first_ptr);

    canvas.clear();
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    MouseEvent panel_select;
    panel_select.position = {250, 25};
    panel_select.is_down = true;
    REQUIRE(overlay.handle_mouse_event(panel_select));
    REQUIRE(overlay.selected_view() == first_ptr);

    MouseEvent panel_collapse;
    panel_collapse.position = {205, 5};
    panel_collapse.is_down = true;
    REQUIRE(overlay.handle_mouse_event(panel_collapse));

    canvas.clear();
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    overlay.set_active(false);
    REQUIRE(overlay.selected_view() == nullptr);
    REQUIRE(overlay.hovered_view() == nullptr);

    overlay.set_active(true);
    MouseEvent alt_first;
    alt_first.position = {20, 20};
    alt_first.modifiers = kModAlt;
    alt_first.is_down = true;
    REQUIRE(overlay.handle_mouse_event(alt_first));

    MouseEvent alt_second;
    alt_second.position = {130, 20};
    alt_second.modifiers = kModAlt;
    alt_second.is_down = true;
    REQUIRE(overlay.handle_mouse_event(alt_second));
    REQUIRE(overlay.selected_view() == second_ptr);
}

// Phase 3f — Alt-hover sibling distance (Figma-style spacing reveal).
// Click to select a node, then HOLD Alt while hovering another → overlay
// renders a live distance line from selection to hovered target. Released
// Alt clears the target. Distinct from the Alt+CLICK sticky-anchor mode
// covered above.
TEST_CASE("InspectorOverlay: Alt-hover reveals sibling distance line",
          "[inspect][overlay][phase3f]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 500, 300});

    auto a = std::make_unique<View>();
    a->set_id("a");
    a->set_bounds({10, 10, 60, 60});
    auto* a_ptr = a.get();
    root.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_id("b");
    b->set_bounds({100, 10, 60, 60});
    auto* b_ptr = b.get();
    root.add_child(std::move(b));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select a via click (no Alt).
    MouseEvent click_a;
    click_a.position = {20, 20};
    click_a.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click_a));
    REQUIRE(overlay.selected_view() == a_ptr);

    // Hover over b WITHOUT Alt — no distance reveal; selection unchanged.
    MouseEvent hover_b;
    hover_b.position = {130, 30};
    hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == a_ptr);

    // Paint with no Alt — single highlight, no Alt-hover line.
    pulp::canvas::RecordingCanvas no_alt_canvas;
    overlay.paint(no_alt_canvas);
    auto baseline_cmd_count = no_alt_canvas.command_count();
    REQUIRE(baseline_cmd_count > 0);

    // Now hover over b WITH Alt held → alt-hover target should be b.
    MouseEvent alt_hover_b;
    alt_hover_b.position = {130, 30};
    alt_hover_b.modifiers = kModAlt;
    alt_hover_b.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(alt_hover_b));
    REQUIRE(overlay.hovered_view() == b_ptr);
    REQUIRE(overlay.selected_view() == a_ptr);  // selection unchanged by hover

    pulp::canvas::RecordingCanvas alt_canvas;
    overlay.paint(alt_canvas);
    // With Alt held + selection + hover, paint_distance_lines() draws an
    // extra line + label between selected and hovered. The exact command
    // count depends on canvas internals, but it MUST be greater than the
    // no-Alt baseline.
    REQUIRE(alt_canvas.command_count() > baseline_cmd_count);

    // Release Alt — alt_hover_target_ clears; render returns to baseline.
    MouseEvent release_alt;
    release_alt.position = {130, 30};
    release_alt.modifiers = 0;  // Alt released
    release_alt.is_down = false;
    REQUIRE_FALSE(overlay.handle_mouse_event(release_alt));

    pulp::canvas::RecordingCanvas after_release_canvas;
    overlay.paint(after_release_canvas);
    REQUIRE(after_release_canvas.command_count() == baseline_cmd_count);
}

TEST_CASE("InspectorOverlay: Alt-hover does nothing without a selection",
          "[inspect][overlay][phase3f]") {
    // Edge case: holding Alt while hovering with no selected_ should
    // NOT set alt_hover_target_. Otherwise we'd render a line from
    // nullptr → confused diagnostics.
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 60, 60});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas baseline;
    overlay.paint(baseline);
    auto baseline_count = baseline.command_count();

    MouseEvent alt_hover_no_sel;
    alt_hover_no_sel.position = {30, 30};
    alt_hover_no_sel.modifiers = kModAlt;
    alt_hover_no_sel.is_down = false;
    overlay.handle_mouse_event(alt_hover_no_sel);

    pulp::canvas::RecordingCanvas after_alt_hover;
    overlay.paint(after_alt_hover);
    // No selection → no distance line should be added → command count
    // matches baseline.
    REQUIRE(after_alt_hover.command_count() == baseline_count);
}

// ── InspectorWindow ────────────────────────────────────────────────────────

TEST_CASE("CollapsableSection toggles content from header clicks", "[inspect][window][issue-641]") {
    CollapsableSection section("Layout");
    REQUIRE(section.is_expanded());
    REQUIRE(section.content() != nullptr);
    REQUIRE(section.content()->visible());

    MouseEvent up;
    up.position = {4, 4};
    up.is_down = false;
    section.on_mouse_event(up);
    REQUIRE(section.is_expanded());

    MouseEvent body_click;
    body_click.position = {4, 30};
    body_click.is_down = true;
    section.on_mouse_event(body_click);
    REQUIRE(section.is_expanded());

    MouseEvent header_click;
    header_click.position = {4, 6};
    header_click.is_down = true;
    section.on_mouse_event(header_click);
    REQUIRE_FALSE(section.is_expanded());
    REQUIRE_FALSE(section.content()->visible());

    section.set_expanded(true);
    REQUIRE(section.is_expanded());
    REQUIRE(section.content()->visible());
}

TEST_CASE("InspectorWindow builds tabs and updates element properties", "[inspect][window][issue-641]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.set_bounds({0, 0, 480, 320});

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(10, 20, 30);
    theme.colors["bg.surface"] = Color::rgba8(40, 50, 60);
    inspected_root.set_theme(theme);

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({12, 18, 48, 48});
    knob->flex().direction = FlexDirection::row;
    knob->flex().gap = 3;
    knob->flex().padding = 4;
    knob->flex().margin = 2;
    knob->flex().flex_grow = 1.0f;
    knob->flex().flex_shrink = 0.5f;
    knob->set_background_color(Color::rgba8(30, 40, 50, 200));
    knob->set_border(Color::rgba8(80, 90, 100), 2, 5);
    knob->set_opacity(0.75f);
    knob->set_value(0.625f);
    knob->set_label("Gain");
    auto* knob_ptr = knob.get();
    inspected_root.add_child(std::move(knob));

    InspectorWindow window(inspected_root);
    REQUIRE(window.child_count() == 1);

    auto* tabs = dynamic_cast<TabPanel*>(window.child_at(0));
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->tab_count() == 4);
    REQUIRE(tabs->active_tab() == 0);
    REQUIRE(tabs->child_at(0)->visible());
    REQUIRE_FALSE(tabs->child_at(1)->visible());

    auto* tree = first_view_of_type<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    auto* knob_node = tree->find_node_by_user_data(knob_ptr);
    REQUIRE(knob_node != nullptr);
    REQUIRE(knob_node->label.find("Knob #gain") != std::string::npos);

    bool callback_seen = false;
    window.on_view_selected = [&](View* view) {
        callback_seen = (view == knob_ptr);
    };
    window.select_view(knob_ptr);
    REQUIRE(callback_seen);
    REQUIRE(has_label_containing(window, "Type: Knob"));
    REQUIRE(has_label_containing(window, "ID: gain"));
    REQUIRE(has_label_containing(window, "Bounds: 12, 18  48 x 48"));
    REQUIRE(has_label_containing(window, "Direction: row"));
    REQUIRE(has_label_containing(window, "Gap: 3.0"));
    REQUIRE(has_label_containing(window, "Padding: 4.0"));
    REQUIRE(has_label_containing(window, "Margin: 2.0"));
    REQUIRE(has_label_containing(window, "Grow: 1.0 / Shrink: 0.5"));
    REQUIRE(has_label_containing(window, "Opacity: 0.75"));
    REQUIRE(has_label_containing(window, "Border: 2.0px"));
    REQUIRE(has_label_containing(window, "Corner radius: 5.0"));
    REQUIRE(has_label_containing(window, "Value: 0.625  Label: Gain"));

    Fader fader;
    fader.set_value(0.2f);
    fader.set_label("Mix");
    window.select_view(&fader);
    REQUIRE(has_label_containing(window, "Value: 0.200  Label: Mix"));

    Toggle toggle;
    toggle.set_on(true);
    toggle.set_label("Bypass");
    window.select_view(&toggle);
    REQUIRE(has_label_containing(window, "On: true  Label: Bypass"));

    Label label("Ready");
    window.select_view(&label);
    REQUIRE(has_label_containing(window, "Text: Ready"));

    Meter meter;
    meter.set_level(0.125f, 0.5f);
    window.select_view(&meter);
    REQUIRE(has_label_containing(window, "RMS: 0.125  Peak: 0.500"));

    window.refresh();
    REQUIRE(tree->find_node_by_user_data(knob_ptr) != nullptr);
}

TEST_CASE("InspectorWindow refreshes console performance and state tabs", "[inspect][window][issue-641]") {
    View inspected_root;
    inspected_root.set_id("root");
    inspected_root.add_child(std::make_unique<Label>("child"));

    InspectorWindow window(inspected_root);
    auto* tabs = dynamic_cast<TabPanel*>(window.child_at(0));
    REQUIRE(tabs != nullptr);

    ConsoleCapture capture;
    auto console = capture.callback();
    console("warn", "careful");
    console("debug", "trace");
    window.set_console_capture(&capture);
    tabs->set_active_tab(1);
    window.refresh();
    REQUIRE(tabs->active_tab() == 1);
    REQUIRE(tabs->child_at(1)->visible());
    REQUIRE(collect_views_of_type<ConsoleEntryView>(*tabs->child_at(1)).size() == 2);

    pulp::render::RenderPassManager rpm;
    rpm.set_budget(1.0f);
    rpm.begin_frame();
    rpm.begin_pass(pulp::render::RenderPassType::background);
    rpm.end_pass(0.75f, 3);
    rpm.begin_pass(pulp::render::RenderPassType::overlay);
    rpm.end_pass(1.75f, 7);
    rpm.end_frame();

    window.set_render_pass_manager(&rpm);
    tabs->set_active_tab(2);
    window.refresh();
    REQUIRE(has_label_containing(*tabs->child_at(2), "FPS: 400.0"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "Frame time: 2.50 ms"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "Budget: 1.0 ms  [OVER BUDGET]"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "View count: 2"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "background: 0.75 ms, 3 draws"));
    REQUIRE(has_label_containing(*tabs->child_at(2), "overlay: 1.75 ms, 7 draws"));

    InspectorWindow missing_perf_window(inspected_root);
    auto* missing_perf_tabs = dynamic_cast<TabPanel*>(missing_perf_window.child_at(0));
    REQUIRE(missing_perf_tabs != nullptr);
    missing_perf_tabs->set_active_tab(2);
    missing_perf_window.refresh();
    REQUIRE(has_label_containing(*missing_perf_tabs->child_at(2), "FPS: (no data)"));

    StateStore store;
    ParamInfo gain;
    gain.id = 7;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-60.0f, 12.0f, -12.0f, 0.5f};
    gain.to_string = [](float value) {
        return std::to_string(static_cast<int>(value)) + " dB";
    };
    store.add_parameter(gain);
    store.set_value(7, -6.0f);

    StateInspector state_inspector(store);
    window.set_state_inspector(&state_inspector);
    tabs->set_active_tab(3);
    window.refresh();
    REQUIRE(has_label_containing(*tabs->child_at(3), "Gain: -6.000 (-6 dB)"));
    REQUIRE(has_label_containing(*tabs->child_at(3), "Range: [-60.00, 12.00] step=0.500  Unit: dB"));
}

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
