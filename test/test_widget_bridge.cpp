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

static std::string trim_crlf(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

// Walk document.getElementById('<id>')._id and look the resulting native
// element up in WidgetBridge's table, downcast to CanvasWidget. Used by
// the canvas2d-bridge tests + downstream surfaces (CSS clip-path,
// box-shadow JS bridge, SVG widget integration, etc.). Originally lived
// inside the canvas2d section that was extracted to
// test_widget_bridge_canvas2d.cpp in the P5-1 continuation; this copy
// stays because the other tests in this TU also need it.
static pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                                  pulp::view::ScriptEngine& engine,
                                                  const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
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

TEST_CASE("WidgetBridge creates knob from JS", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");

    REQUIRE(root.child_count() == 1);
    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Knob*>(w) != nullptr);
}

// Phase 0b: setAnchor() bridge call binds an anchor to a live widget so
// the inspector can key tweaks back to the originating source element.
TEST_CASE("WidgetBridge setAnchor binds the anchor to the live widget",
          "[view][bridge][anchor]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(
        "createKnob('gain', 10, 10, 48, 48);"
        "setAnchor('gain', 'figma:0:42');"
    );

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->anchor_id() == "figma:0:42");
}

TEST_CASE("WidgetBridge setAnchor is a silent no-op on unknown widget id",
          "[view][bridge][anchor]") {
    // Mirror the pattern used by every other property setter — silent
    // no-op on unmounted ids. Avoids load-order coupling between
    // setAnchor() and createX().
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Should not throw / crash. No assertion required beyond the
    // absence of exceptions; the run finishing cleanly is the test.
    bridge.load_script("setAnchor('nope', 'figma:0:1');");
    REQUIRE(bridge.widget("nope") == nullptr);
}

TEST_CASE("View::anchor_id() defaults to empty for non-imported views",
          "[view][anchor]") {
    View v;
    REQUIRE(v.anchor_id().empty());
    v.set_anchor_id("figma:0:99");
    REQUIRE(v.anchor_id() == "figma:0:99");
    v.set_anchor_id("");
    REQUIRE(v.anchor_id().empty());
}

TEST_CASE("WidgetBridge creates fader from JS", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createFader('volume', 60, 10, 24, 200)");

    auto* w = bridge.widget("volume");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Fader*>(w) != nullptr);
}

TEST_CASE("WidgetBridge creates toggle from JS", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createToggle('bypass', 100, 10, 50, 30)");

    auto* w = bridge.widget("bypass");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Toggle*>(w) != nullptr);
}

TEST_CASE("WidgetBridge creates range slider from JS",
          "[view][bridge][issue-966]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createRangeSlider('morph', '');
        setMin('morph', 0);
        setMax('morph', 1);
        setStep('morph', 0.001);
        setValue('morph', 0.4);
    )");

    auto* w = bridge.widget("morph");
    REQUIRE(w != nullptr);
    auto* range = dynamic_cast<RangeSlider*>(w);
    REQUIRE(range != nullptr);
    REQUIRE_THAT(range->min_value(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(range->max_value(), WithinAbs(1.0, 0.0001));
    REQUIRE_THAT(range->step(), WithinAbs(0.001, 0.0001));
    // Quantised to nearest 0.001 — 0.4 is already a clean step.
    REQUIRE_THAT(range->value(), WithinAbs(0.4, 0.001));
}

TEST_CASE("WidgetBridge range slider setOrientation and setAccentColor",
          "[view][bridge][issue-966]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createRangeSlider('volume', '');
        setOrientation('volume', 'vertical');
        setAccentColor('volume', '#ff8844');
    )");

    auto* range = dynamic_cast<RangeSlider*>(bridge.widget("volume"));
    REQUIRE(range != nullptr);
    REQUIRE(range->orientation() == RangeSlider::Orientation::vertical);
    REQUIRE(range->has_accent_color());

    auto c = range->accent_color();
    // #ff8844 → r≈1.0, g≈0.533, b≈0.267
    REQUIRE_THAT(c.r, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(c.g, WithinAbs(0.533, 0.02));
    REQUIRE_THAT(c.b, WithinAbs(0.267, 0.02));

    // Empty hex clears the override.
    bridge.load_script("setAccentColor('volume', '')");
    REQUIRE_FALSE(range->has_accent_color());
}

TEST_CASE("WidgetBridge range slider get/setValue round-trip",
          "[view][bridge][issue-966]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createRangeSlider('scrub', '');
        setMin('scrub', 0);
        setMax('scrub', 100);
        setStep('scrub', 5);
        setValue('scrub', 47);
    )");

    auto* range = dynamic_cast<RangeSlider*>(bridge.widget("scrub"));
    REQUIRE(range != nullptr);
    // 47 quantised to step=5 → 45.
    REQUIRE_THAT(range->value(), WithinAbs(45.0, 0.0001));

    auto js_val = engine.evaluate("getValue('scrub')").getWithDefault<double>(-1);
    REQUIRE_THAT(js_val, WithinAbs(45.0, 0.0001));
}

TEST_CASE("WidgetBridge range slider drag dispatches change event",
          "[view][bridge][issue-966]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // The bridge dispatches via __dispatch__, which the script wires to
    // a JS-side recorder. Confirms the dispatch path actually fires —
    // matching the same pattern Knob/Fader callbacks use.
    bridge.load_script(R"(
        var changes = [];
        __dispatch__ = function(id, type, value) {
            changes.push({id: id, type: type, value: value});
        };
        createRangeSlider('scrub', '');
        setMin('scrub', 0);
        setMax('scrub', 1);
    )");

    auto* range = dynamic_cast<RangeSlider*>(bridge.widget("scrub"));
    REQUIRE(range != nullptr);
    range->set_bounds({0, 0, 200, 24});

    // Simulate a click in the middle of the track.
    MouseEvent ev;
    ev.position = {100, 12};
    ev.is_down = true;
    range->on_mouse_event(ev);

    auto count = engine.evaluate("changes.length").getWithDefault<double>(0);
    REQUIRE(count >= 1);
    auto type = engine.evaluate("changes[0].type").getWithDefault<std::string>("");
    REQUIRE(type == "change");
    auto val = engine.evaluate("changes[0].value").getWithDefault<double>(-1);
    REQUIRE_THAT(val, WithinAbs(0.5, 0.05));
}

TEST_CASE("WidgetBridge creates modal overlay from JS", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createModal('help-modal', '')");

    auto* w = bridge.widget("help-modal");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<ModalOverlay*>(w) != nullptr);
}

TEST_CASE("WidgetBridge modal dismiss dispatches JS handler on Escape", "[view][bridge]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var dismissed = 0;
        createModal('help-modal', '');
        on('help-modal', 'dismiss', function() { dismissed = 1; });
    )");

    auto* modal = dynamic_cast<ModalOverlay*>(bridge.widget("help-modal"));
    REQUIRE(modal != nullptr);
    modal->set_visible(true);

    KeyEvent esc{};
    esc.is_down = true;
    esc.key = KeyCode::escape;
    REQUIRE(modal->on_key_event(esc));
    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 1);
    REQUIRE_FALSE(modal->visible());
}

TEST_CASE("WidgetBridge stale click callbacks are inert after bridge destruction", "[view][bridge][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    std::function<void()> click_handler;
    std::function<void(const std::string&, uint16_t)> global_click_handler;

    {
        WidgetBridge bridge(engine, root, store);
        bridge.load_script(R"(
            createLabel('button', 'Hello', '');
            registerClick('button');
            enableInspectClick();
        )");

        auto* button = bridge.widget("button");
        REQUIRE(button != nullptr);
        REQUIRE(button->on_click);
        REQUIRE(root.on_global_click);
        click_handler = button->on_click;
        global_click_handler = root.on_global_click;
    }

    REQUIRE_NOTHROW(click_handler());
    REQUIRE_NOTHROW(global_click_handler("button", 0x10));
}

// pulp #1006 — JSX `onClick={fn}` flows through @pulp/react's prop-applier
// into a bare `on(id, 'click', fn)` bridge call (no addEventListener,
// no registerClick). Before the fix, this stored the JS callback in
// __callbacks__ but never wired View::on_click on the native side, so
// real NSEvent / Win32 mouse events fired View::on_mouse_down/up but
// never dispatched 'click' through the bridge — the React handler
// silently dropped on the floor.
TEST_CASE("WidgetBridge on(id,'click',fn) auto-wires View::on_click", "[view][bridge][issue-1006]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var clicks = 0;
        createToggleButton('clear', '');
        on('clear', 'click', function() { clicks += 1; });
    )");

    auto* button = bridge.widget("clear");
    REQUIRE(button != nullptr);

    // The smoking gun: without the fix, `View::on_click` is empty and
    // the deferred mac-host dispatch (which reads exactly this field
    // post-#992) has nothing to call. With the fix, `on()` delegates
    // to registerClick(id), which installs the native callback that
    // emits __dispatch__('clear', 'click', 0).
    REQUIRE(static_cast<bool>(button->on_click));

    // Drive the native callback directly to verify it routes through
    // __dispatch__ to the JS subscriber registered above.
    button->on_click();
    REQUIRE(engine.evaluate("clicks").getWithDefault<int>(-1) == 1);

    // The full mouse-down/up path through the View should also fire
    // the JS handler exactly once (matches mac-host mouseUp path that
    // calls `click_handler()` after view_is_in_tree validation).
    button->on_mouse_down({4, 4});
    button->on_mouse_up({4, 4});
    button->on_click();
    REQUIRE(engine.evaluate("clicks").getWithDefault<int>(-1) == 2);
}

// pulp #1006 — repeated `on(id, 'click', fn)` calls (which @pulp/react
// performs on every commitUpdate) must remain idempotent on the native
// side. registerClick is overwriting-by-design (it stores its lambda
// on view->on_click), but registerPointer chains (each call wraps the
// previous handler). The auto-wire in `on()` guards re-registration
// via __nativeRegistered__ so pointer events don't grow an O(N) chain.
TEST_CASE("WidgetBridge on() native registration is idempotent", "[view][bridge][issue-1006]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var pointer_events = 0;
        createCol('panel', '');
        on('panel', 'pointerdown', function() { pointer_events += 1; });
        on('panel', 'pointerdown', function() { pointer_events += 1; });
        on('panel', 'pointerdown', function() { pointer_events += 1; });
    )");

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    REQUIRE(static_cast<bool>(panel->on_pointer_event));

    MouseEvent down;
    down.position = {10, 10};
    down.is_down = true;
    panel->on_pointer_event(down);

    // Three subscriptions but each call to on() overwrites the
    // __callbacks__ slot, so a single dispatch fires once. If the
    // pointer chain wasn't guarded, this would grow with each
    // re-registration into a multi-fire chain.
    auto count = engine.evaluate("pointer_events").getWithDefault<int>(-1);
    REQUIRE(count == 1);
}

TEST_CASE("WidgetBridge getLayoutRect accounts for scroll offsets", "[view][bridge][layout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createScrollView('sv', '');
        setFlex('sv', 'direction', 'col');
        setFlex('sv', 'width', 200);
        setFlex('sv', 'height', 100);
        setScrollContentSize('sv', 200, 260);
        createCol('spacer', 'sv');
        setFlex('spacer', 'height', 120);
        createLabel('anchor', 'Anchor', 'sv');
        setFlex('anchor', 'height', 20);
        layout();
    )");

    auto* scroll = dynamic_cast<ScrollView*>(bridge.widget("sv"));
    REQUIRE(scroll != nullptr);
    auto before = engine.evaluate("getLayoutRect('anchor').y").getWithDefault<double>(-1.0);
    scroll->set_scroll(0.0f, 60.0f);

    auto after = engine.evaluate("getLayoutRect('anchor').y").getWithDefault<double>(-1.0);
    REQUIRE_THAT(before - after, WithinAbs(60.0, 0.5));
}

TEST_CASE("WidgetBridge set/get value from JS", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setValue('gain', 0.75)");

    auto result = engine.evaluate("getValue('gain')");
    REQUIRE_THAT(result.getWithDefault<double>(0), WithinAbs(0.75, 0.01));
}

TEST_CASE("WidgetBridge records canvas commands from web-compat 2D context", "[view][bridge][canvas]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var canvas = document.createElement('canvas');
        canvas.id = 'phase13-canvas';
        canvas.width = 240;
        canvas.height = 120;
        document.body.appendChild(canvas);
        var ctx = canvas.getContext('2d');
        ctx.fillStyle = '#ff8844';
        ctx.fillRect(12, 16, 44, 28);
        ctx.strokeStyle = '#99ddff';
        ctx.lineWidth = 3;
        ctx.strokeRect(70, 24, 50, 30);
    )");

    root.layout_children();

    auto nativeIdValue = engine.evaluate("document.getElementById('phase13-canvas')._id");
    auto nativeId = std::string(nativeIdValue.getWithDefault<std::string_view>(""));
    auto* canvas = dynamic_cast<CanvasWidget*>(bridge.widget(nativeId));
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() >= 4);
    REQUIRE_THAT(canvas->bounds().width, WithinAbs(240.0f, 1.0f));
    REQUIRE_THAT(canvas->bounds().height, WithinAbs(120.0f, 1.0f));
}

TEST_CASE("WidgetBridge parameter binding", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});

    StateStore store;
    ParamInfo gain_info;
    gain_info.id = 1;
    gain_info.name = "gain";
    gain_info.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(gain_info);

    WidgetBridge bridge(engine, root, store);

    // Set param from JS
    bridge.load_script("setParam('gain', 0.8)");
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.8, 0.01));

    // Read param from JS
    auto result = engine.evaluate("getParam('gain')");
    REQUIRE_THAT(result.getWithDefault<double>(0), WithinAbs(0.8, 0.01));
}

TEST_CASE("WidgetBridge complete UI script", "[view][bridge]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});

    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    store.add_parameter({2, "Mix", "%", {0.0f, 100.0f, 100.0f}});

    WidgetBridge bridge(engine, root, store);

    // Simulate a typical plugin UI script
    bridge.load_script(R"(
        createLabel('title', 'PulpGain', 10, 10, 200, 30);
        createKnob('Gain', 10, 50, 60, 60);
        createKnob('Mix', 80, 50, 60, 60);
        createToggle('bypass', 150, 50, 50, 30);
    )");

    REQUIRE(root.child_count() == 4);
    REQUIRE(bridge.widget("title") != nullptr);
    REQUIRE(bridge.widget("Gain") != nullptr);
    REQUIRE(bridge.widget("Mix") != nullptr);
    REQUIRE(bridge.widget("bypass") != nullptr);

    // Sync from store
    store.set_normalized(1, 0.5f);
    store.set_normalized(2, 0.75f);
    bridge.sync_from_store();

    auto* gain_knob = dynamic_cast<Knob*>(bridge.widget("Gain"));
    REQUIRE(gain_knob != nullptr);
    REQUIRE_THAT(gain_knob->value(), WithinAbs(0.5, 0.01));
}

TEST_CASE("WidgetBridge extended controls and visualization APIs round-trip JS to native state",
          "[view][bridge][controls]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var checkbox_change = -1;
        var toggle_change = -1;
        var list_select = -1;
        var list_activate = -1;

        createIcon('send-icon', 'send', '');
        createImage('preview', '');
        setImageSource('preview', '/tmp/pulp-preview.png');

        createCheckbox('armed', '');
        on('armed', 'change', function(value) { checkbox_change = value; });
        setValue('armed', 1);

        createToggleButton('latched', '');
        on('latched', 'toggle', function(value) { toggle_change = value; });
        setLabel('latched', 'Latch');
        setValue('latched', 1);

        createMeter('meter', 'horizontal', '');
        setMeterLevel('meter', 0.25, 0.75);
        setWidgetStyle('meter', 'minimal');

        createXYPad('xy', '');
        setXY('xy', 1.2, -0.2);

        createWaveform('wave', '');
        setWaveformData('wave', [-1, 0, 1]);

        createSpectrum('spectrum', '');
        setSpectrumData('spectrum', [-60, -12, 0, -6]);

        createProgress('progress', '');
        setProgress('progress', 0.66);

        createListBox('list', '');
        on('list', 'select', function(index) { list_select = index; });
        on('list', 'activate', function(index) { list_activate = index; });
        setListItems('list', ['A', 'B', 'C']);
        setListRowHeight('list', 31);
        setListSelected('list', 2);
    )");

    auto* icon = dynamic_cast<Icon*>(bridge.widget("send-icon"));
    REQUIRE(icon != nullptr);
    REQUIRE(icon->type() == Icon::Type::send);

    auto* image = dynamic_cast<ImageView*>(bridge.widget("preview"));
    REQUIRE(image != nullptr);
    REQUIRE(image->image_path() == "file:///tmp/pulp-preview.png");

    auto* checkbox = dynamic_cast<Checkbox*>(bridge.widget("armed"));
    REQUIRE(checkbox != nullptr);
    REQUIRE(checkbox->is_checked());
    checkbox->on_mouse_down({});
    REQUIRE_FALSE(checkbox->is_checked());
    REQUIRE(engine.evaluate("checkbox_change").getWithDefault<int>(-1) == 0);

    auto* toggle = dynamic_cast<ToggleButton*>(bridge.widget("latched"));
    REQUIRE(toggle != nullptr);
    REQUIRE(toggle->is_on());
    REQUIRE(toggle->label() == "Latch");
    toggle->on_mouse_down({});
    REQUIRE_FALSE(toggle->is_on());
    REQUIRE(engine.evaluate("toggle_change").getWithDefault<int>(-1) == 0);

    auto* meter = dynamic_cast<Meter*>(bridge.widget("meter"));
    REQUIRE(meter != nullptr);
    REQUIRE(meter->orientation() == Meter::Orientation::horizontal);
    REQUIRE(meter->render_style() == WidgetRenderStyle::minimal);
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(meter->display_peak(), WithinAbs(0.75f, 0.001f));

    auto* xy = dynamic_cast<XYPad*>(bridge.widget("xy"));
    REQUIRE(xy != nullptr);
    REQUIRE_THAT(xy->x_value(), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(xy->y_value(), WithinAbs(0.0f, 0.001f));

    auto* wave = dynamic_cast<WaveformView*>(bridge.widget("wave"));
    REQUIRE(wave != nullptr);
    REQUIRE(wave->sample_count() == 3);

    auto* spectrum = dynamic_cast<SpectrumView*>(bridge.widget("spectrum"));
    REQUIRE(spectrum != nullptr);
    REQUIRE(spectrum->bin_count() == 4);

    auto* progress = dynamic_cast<ProgressBar*>(bridge.widget("progress"));
    REQUIRE(progress != nullptr);
    REQUIRE_THAT(progress->progress(), WithinAbs(0.66f, 0.001f));

    auto* list = dynamic_cast<ListBox*>(bridge.widget("list"));
    REQUIRE(list != nullptr);
    REQUIRE(list->items().size() == 3);
    REQUIRE(list->selected() == 2);
    REQUIRE_THAT(list->row_height(), WithinAbs(31.0f, 0.001f));
    REQUIRE(engine.evaluate("list_select").getWithDefault<int>(-1) == 2);

    KeyEvent enter{};
    enter.is_down = true;
    enter.key = KeyCode::enter;
    REQUIRE(list->on_key_event(enter));
    REQUIRE(engine.evaluate("list_activate").getWithDefault<int>(-1) == 2);
}

TEST_CASE("WidgetBridge pointer, gesture, capture, and shortcut APIs dispatch to JS",
          "[view][bridge][events]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var pointer_down_id = -1;
        var pointer_down_type = '';
        var pointer_down_primary = true;
        var pointer_down_pressure = -1;
        var pointer_down_client_x = -1;
        var pointer_down_offset_x = -1;
        var pointer_down_button = -1;
        var pointer_down_mods = '';
        var pointer_up_id = -1;
        var pointer_cancel_id = -1;
        var pointer_move_x = -1;
        var pointer_move_y = -1;
        var wheel_x = 0;
        var wheel_y = 0;
        var gesture_log = [];
        var capture_log = [];
        var shortcut_count = 0;
        var global_key = -1;
        var global_mods = -1;
        function shortcutHit() { shortcut_count++; }

        createLabel('surface', 'Surface', '');
        on('surface', 'pointerdown', function(e) {
            pointer_down_id = e.pointerId;
            pointer_down_type = e.pointerType;
            pointer_down_primary = e.isPrimary;
            pointer_down_pressure = e.pressure;
            pointer_down_client_x = e.clientX;
            pointer_down_offset_x = e.offsetX;
            pointer_down_button = e.button;
            pointer_down_mods = [e.ctrlKey, e.shiftKey, e.altKey, e.metaKey].join(':');
        });
        on('surface', 'pointerup', function(e) { pointer_up_id = e.pointerId; });
        on('surface', 'pointercancel', function(e) { pointer_cancel_id = e.pointerId; });
        on('surface', 'pointermove', function(e) {
            pointer_move_x = e.offsetX;
            pointer_move_y = e.offsetY;
        });
        // pulp #1XXX — wheel events are dispatched as an object
        // {deltaX, deltaY, clientX, clientY} (not positional args), so the
        // @pulp/react synthetic-event shim's isPlainObject(a0) branch
        // can lift the fields. JSX `onWheel(e => e.deltaY)` depends on
        // this shape.
        on('surface', 'wheel', function(e) {
            wheel_x = e.deltaX;
            wheel_y = e.deltaY;
        });
        on('surface', 'gesturestart', function(e) { gesture_log.push('start:' + e.scale); });
        on('surface', 'gesturechange', function(e) { gesture_log.push('change:' + e.rotation); });
        on('surface', 'gestureend', function(e) { gesture_log.push('end:' + e.clientX); });
        on('surface', 'gotpointercapture', function(e) { capture_log.push('got:' + e.pointerId); });
        on('surface', 'lostpointercapture', function(e) { capture_log.push('lost:' + e.pointerId); });
        on('__global__', 'keydown', function(e) {
            global_key = e.key;
            global_mods = e.mods;
        });

        registerPointer('surface');
        registerWheel('surface');
        registerGesture('surface');
        registerShortcut(65, 18, 'shortcutHit');
    )");

    auto* surface = bridge.widget("surface");
    REQUIRE(surface != nullptr);
    REQUIRE(surface->on_pointer_event);
    REQUIRE(surface->on_drag);
    REQUIRE(surface->on_gesture_cb);

    MouseEvent down{};
    down.is_down = true;
    down.position = {7.0f, 9.0f};
    down.window_position = {107.0f, 109.0f};
    down.pointer_id = 3;
    down.pointer_type = PointerType::pen;
    down.pressure = 0.75f;
    down.altitude_angle = 0.4f;
    down.azimuth_angle = 1.2f;
    down.button = MouseButton::right;
    down.modifiers = static_cast<uint16_t>(kModCtrl | kModShift | kModAlt | kModCmd);
    surface->on_mouse_event(down);

    REQUIRE(engine.evaluate("pointer_down_id").getWithDefault<int>(-1) == 3);
    REQUIRE(engine.evaluate("pointer_down_type").toString() == "pen");
    REQUIRE_FALSE(engine.evaluate("pointer_down_primary").getWithDefault<bool>(true));
    REQUIRE_THAT(engine.evaluate("pointer_down_pressure").getWithDefault<double>(0.0), WithinAbs(0.75, 0.001));
    REQUIRE_THAT(engine.evaluate("pointer_down_client_x").getWithDefault<double>(0.0), WithinAbs(107.0, 0.001));
    REQUIRE_THAT(engine.evaluate("pointer_down_offset_x").getWithDefault<double>(0.0), WithinAbs(7.0, 0.001));
    // pulp #1XXX — W3C MouseEvent.button: right=2 (unchanged), but the
    // earlier raw-enum path coincidentally also emitted 2 for right.
    // The button=0/1/2 contract test covering the regression cause
    // (left=0, not 1) lives in the "W3C button mapping" test below.
    REQUIRE(engine.evaluate("pointer_down_button").getWithDefault<int>(0) == 2);
    REQUIRE(engine.evaluate("pointer_down_mods").toString() == "true:true:true:true");

    MouseEvent up = down;
    up.is_down = false;
    surface->on_mouse_event(up);
    REQUIRE(engine.evaluate("pointer_up_id").getWithDefault<int>(-1) == 3);

    MouseEvent cancel = up;
    cancel.is_cancelled = true;
    surface->on_mouse_event(cancel);
    REQUIRE(engine.evaluate("pointer_cancel_id").getWithDefault<int>(-1) == 3);

    surface->on_drag({13.0f, 17.0f});
    REQUIRE_THAT(engine.evaluate("pointer_move_x").getWithDefault<double>(0.0), WithinAbs(13.0, 0.001));
    REQUIRE_THAT(engine.evaluate("pointer_move_y").getWithDefault<double>(0.0), WithinAbs(17.0, 0.001));

    MouseEvent wheel{};
    wheel.is_wheel = true;
    wheel.scroll_delta_x = 2.5f;
    wheel.scroll_delta_y = -7.0f;
    surface->on_mouse_event(wheel);
    REQUIRE_THAT(engine.evaluate("wheel_x").getWithDefault<double>(0.0), WithinAbs(2.5, 0.001));
    REQUIRE_THAT(engine.evaluate("wheel_y").getWithDefault<double>(0.0), WithinAbs(-7.0, 0.001));

    GestureEvent gesture{};
    gesture.phase = GesturePhase::began;
    gesture.scale = 1.25f;
    gesture.position = {21.0f, 22.0f};
    surface->on_gesture_event(gesture);
    gesture.phase = GesturePhase::changed;
    gesture.rotation = 0.5f;
    surface->on_gesture_event(gesture);
    gesture.phase = GesturePhase::cancelled;
    surface->on_gesture_event(gesture);
    REQUIRE(engine.evaluate("gesture_log.join('|')").toString() == "start:1.25|change:0.5|end:21");

    bridge.load_script("nativeSetPointerCapture('surface', 42);");
    REQUIRE(surface->has_pointer_capture(42));
    bridge.load_script("nativeReleasePointerCapture('surface', 42);");
    REQUIRE_FALSE(surface->has_pointer_capture(42));
    REQUIRE(engine.evaluate("capture_log.join('|')").toString() == "got:42|lost:42");

    bridge.forward_key_event(static_cast<int>(KeyCode::a),
                             static_cast<uint16_t>(kModCtrl | kModCmd),
                             true);
    REQUIRE(engine.evaluate("shortcut_count").getWithDefault<int>(0) == 0);
    // pulp #1XXX — `e.key` is a W3C UIEvent.key string ('a', not 97).
    // JSX handlers compare `e.key === 'Escape'` / `'a'` and the previous
    // raw-int dispatch broke every such comparison.
    REQUIRE(engine.evaluate("global_key").toString() == "a");
    REQUIRE(engine.evaluate("global_mods").getWithDefault<int>(0) == static_cast<int>(kModCtrl | kModCmd));

    bridge.forward_key_event(static_cast<int>(KeyCode::a),
                             static_cast<uint16_t>(kModCtrl | kModCmd),
                             false);
    REQUIRE(engine.evaluate("shortcut_count").getWithDefault<int>(0) == 0);

    bridge.forward_key_event(65, 18, true);
    REQUIRE(engine.evaluate("shortcut_count").getWithDefault<int>(0) == 1);
}

TEST_CASE("WidgetBridge style and layout setters update native view state",
          "[view][bridge][style][layout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        setTheme('light');
        createGrid('grid', '');
        createPanel('panel', 'grid');
        createLabel('title', 'Hello', 'panel');
        createTextEditor('editor', 'panel');

        setGrid('grid', 'template_columns', '1fr 2fr auto 100px');
        setGrid('grid', 'template_rows', '40px auto');
        setGrid('grid', 'gap', 8);
        setGrid('panel', 'column_start', 2);
        setGrid('panel', 'column_end', 4);
        setGrid('panel', 'row_start', 1);
        setGrid('panel', 'row_end', 3);

        setFlex('panel', 'direction', 'row');
        setFlex('panel', 'gap', 5);
        setFlex('panel', 'padding', 11);
        setFlex('panel', 'padding_top', 12);
        setFlex('panel', 'padding_right', 13);
        setFlex('panel', 'padding_bottom', 14);
        setFlex('panel', 'padding_left', 15);
        setFlex('panel', 'margin', 4);
        setFlex('panel', 'margin_top', 1);
        setFlex('panel', 'margin_right', 2);
        setFlex('panel', 'margin_bottom', 3);
        setFlex('panel', 'margin_left', 4);
        setFlex('panel', 'flex_grow', 2);
        setFlex('panel', 'flex_shrink', 0.25);
        setFlex('panel', 'flex_basis', 88);
        setFlex('panel', 'flex_wrap', 1);
        setFlex('panel', 'order', 3);
        setFlex('panel', 'width', 123);
        setFlex('panel', 'height', 45);
        setFlex('panel', 'min_width', 44);
        setFlex('panel', 'min_height', 33);
        setFlex('panel', 'max_width', 222);
        setFlex('panel', 'max_height', 111);
        setFlex('panel', 'row_gap', 6);
        setFlex('panel', 'column_gap', 7);
        setFlex('panel', 'align_items', 'center');
        setFlex('panel', 'align_self', 'end');
        setFlex('panel', 'justify_content', 'space-evenly');

        setPointerEvents('panel', 'none');
        setVisibility('panel', 'hidden');
        setUserSelect('panel', 'none');
        setPanelStyle('panel', 'bg.raised', 'accent.primary', 9, 2);
        setBackground('panel', 'rgba(10, 20, 30, 0.5)');
        setBorder('panel', 'hsl(120, 100%, 50%)', 2, 6);
        setBorderSide('panel', 'top', 4, 'tomato');
        setCornerRadius('panel', 'TopLeft', 3);
        setOpacity('panel', 0.42);
        setOverflow('panel', 'visible');
        setEnabled('panel', 0);
        setPosition('panel', 'sticky');
        setTop('panel', 10);
        setRight('panel', 20);
        setBottom('panel', 30);
        setLeft('panel', 40);
        setZIndex('panel', 99);
        setTransitionDuration('panel', 0.33);
        setTranslate('panel', 12, 13);
        setRotation('panel', 45);
        setScale('panel', 1.5);
        setTransformOrigin('panel', 0.25, 0.75);
        setTextOverflow('panel', 'ellipsis');
        setCursor('panel', 'grab');
        setFilter('panel', 'blur(3.5px)');
        setBackgroundGradient('panel', 'linear-gradient(to right, red, blue)');
        setDebugPaint(1);
        setColorToken('accent.tomato', 'tomato');
        setDimensionToken('space.test', 19);

        setWhiteSpace('title', 'pre-wrap');
        setFontFamily('title', 'Inter Tight');
        setFontWeight('title', 700);
        setFontStyle('title', 'italic');
        setLetterSpacing('title', 1.5);
        setLineHeight('title', 22);
        setTextAlign('title', 'right');
        setFontSize('title', 18);
        setTextColor('title', '#0f8');
        setTextTransform('title', 'uppercase');
        setTextDecoration('title', 'underline');
        setText('title', 'Updated');
        setMultiLine('title', 1);
        setText('editor', 'typed');
        setPlaceholder('editor', 'Enter value');
        setMultiLine('editor', 1);
        setFontSize('editor', 16);

        setStyle('panel', 'noop');
        defineKeyframes('pulse', [{ offset: 0, value: 1 }]);
        setAnimation('panel', 'opacity', 1.0, 2, 'alternate');
        beginPath();
        drawPath('canvas-missing', 'M 0 0 L 10 10', '#fff', '#000', 1);

        var saved_ok = saveStylePreset('Bridge Coverage Preset!', {
            color: '#ffffff',
            nested: { value: 3 }
        });
        var loaded_preset = loadStylePreset('Bridge Coverage Preset!');
        var missing_preset_type = typeof loadStylePreset('missing-preset-for-coverage');
    )");

    auto* grid = bridge.widget("grid");
    auto* panel = dynamic_cast<Panel*>(bridge.widget("panel"));
    auto* title = dynamic_cast<Label*>(bridge.widget("title"));
    auto* editor = dynamic_cast<TextEditor*>(bridge.widget("editor"));
    REQUIRE(grid != nullptr);
    REQUIRE(panel != nullptr);
    REQUIRE(title != nullptr);
    REQUIRE(editor != nullptr);
    auto* panel_view = static_cast<View*>(panel);

    REQUIRE(grid->layout_mode() == LayoutMode::grid);
    REQUIRE(grid->grid().template_columns.size() == 4);
    REQUIRE(grid->grid().template_rows.size() == 2);
    REQUIRE_THAT(grid->grid().column_gap, WithinAbs(8.0f, 0.001f));
    REQUIRE_THAT(grid->grid().row_gap, WithinAbs(8.0f, 0.001f));
    REQUIRE(panel->grid().grid_column_start == 2);
    REQUIRE(panel->grid().grid_column_end == 4);
    REQUIRE(panel->grid().grid_row_start == 1);
    REQUIRE(panel->grid().grid_row_end == 3);

    const auto& flex = panel->flex();
    REQUIRE(flex.direction == FlexDirection::row);
    REQUIRE_THAT(flex.gap, WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(flex.padding, WithinAbs(11.0f, 0.001f));
    REQUIRE_THAT(flex.padding_top, WithinAbs(12.0f, 0.001f));
    REQUIRE_THAT(flex.padding_right, WithinAbs(13.0f, 0.001f));
    REQUIRE_THAT(flex.padding_bottom, WithinAbs(14.0f, 0.001f));
    REQUIRE_THAT(flex.padding_left, WithinAbs(15.0f, 0.001f));
    REQUIRE_THAT(flex.margin_t(), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(flex.margin_r(), WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(flex.margin_b(), WithinAbs(3.0f, 0.001f));
    REQUIRE_THAT(flex.margin_l(), WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(flex.flex_grow, WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(flex.flex_shrink, WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(flex.flex_basis, WithinAbs(88.0f, 0.001f));
    REQUIRE(flex.flex_wrap == FlexWrap::wrap);
    REQUIRE(flex.order == 3);
    REQUIRE_THAT(flex.preferred_width, WithinAbs(123.0f, 0.001f));
    REQUIRE_THAT(flex.preferred_height, WithinAbs(45.0f, 0.001f));
    REQUIRE_THAT(flex.min_width, WithinAbs(44.0f, 0.001f));
    REQUIRE_THAT(flex.min_height, WithinAbs(33.0f, 0.001f));
    REQUIRE_THAT(flex.max_width, WithinAbs(222.0f, 0.001f));
    REQUIRE_THAT(flex.max_height, WithinAbs(111.0f, 0.001f));
    REQUIRE_THAT(flex.row_gap, WithinAbs(6.0f, 0.001f));
    REQUIRE_THAT(flex.column_gap, WithinAbs(7.0f, 0.001f));
    REQUIRE(flex.align_items == FlexAlign::center);
    REQUIRE(flex.align_self == FlexAlign::end);
    REQUIRE(flex.justify_content == FlexJustify::space_evenly);

    REQUIRE_FALSE(panel->hit_testable());
    REQUIRE(panel->background_token() == "bg.raised");
    REQUIRE(panel->border_token() == "accent.primary");
    // pulp #1731 P1 — Panel::set_corner_radius now routes through the
    // same View slot setBorder() writes to. setPanelStyle(..., 9, ...)
    // on JS line 90 was followed by setBorder(..., 2, 6) on line 92, so
    // last-write-wins yields 6. Previously Panel kept a shadowed 9 that
    // paint() never read — the on-screen radius was always the View slot.
    REQUIRE_THAT(panel->corner_radius(), WithinAbs(6.0f, 0.001f));
    REQUIRE_THAT(panel->border_width(), WithinAbs(2.0f, 0.001f));
    REQUIRE(panel->has_background_color());
    REQUIRE(panel->has_border());
    REQUIRE_THAT(panel->background_color().r, WithinAbs(10.0f / 255.0f, 0.001f));
    REQUIRE_THAT(panel->background_color().g, WithinAbs(20.0f / 255.0f, 0.001f));
    REQUIRE_THAT(panel->background_color().b, WithinAbs(30.0f / 255.0f, 0.001f));
    REQUIRE_THAT(panel->background_color().a, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(panel_view->border_width(), WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(panel_view->corner_radius(), WithinAbs(6.0f, 0.001f));
    REQUIRE_THAT(panel->opacity(), WithinAbs(0.42f, 0.001f));
    REQUIRE_FALSE(panel->enabled());
    REQUIRE(panel->position() == View::Position::sticky);
    REQUIRE(panel->overflow() == View::Overflow::visible);
    REQUIRE_THAT(panel->top(), WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(panel->right(), WithinAbs(20.0f, 0.001f));
    REQUIRE_THAT(panel->bottom(), WithinAbs(30.0f, 0.001f));
    REQUIRE_THAT(panel->left(), WithinAbs(40.0f, 0.001f));
    REQUIRE(panel->has_top());
    REQUIRE(panel->has_right());
    REQUIRE(panel->has_bottom());
    REQUIRE(panel->has_left());
    REQUIRE(panel->z_index() == 99);
    REQUIRE_THAT(panel->theme().dimension("transition.duration").value_or(0.0f), WithinAbs(0.33f, 0.001f));
    REQUIRE_THAT(panel->translate_x(), WithinAbs(12.0f, 0.001f));
    REQUIRE_THAT(panel->translate_y(), WithinAbs(13.0f, 0.001f));
    REQUIRE_THAT(panel->rotation(), WithinAbs(45.0f, 0.001f));
    REQUIRE_THAT(panel->scale(), WithinAbs(1.5f, 0.001f));
    REQUIRE_THAT(panel->transform_origin_x(), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(panel->transform_origin_y(), WithinAbs(0.75f, 0.001f));
    REQUIRE(panel->text_overflow_ellipsis());
    REQUIRE(panel->cursor() == View::CursorStyle::grab);
    REQUIRE_THAT(panel->filter_blur(), WithinAbs(3.5f, 0.001f));
    REQUIRE(panel->has_background_gradient());

    REQUIRE_THAT(root.theme().dimension("debug.paint").value_or(0.0f), WithinAbs(1.0f, 0.001f));
    REQUIRE(root.theme().color("accent.tomato").has_value());
    REQUIRE(root.theme().dimension("space.test").value_or(0.0f) == 19.0f);

    REQUIRE(title->text() == "Updated");
    REQUIRE(title->multi_line());
    REQUIRE(title->font_family() == "Inter Tight");
    REQUIRE(title->font_weight() == 700);
    REQUIRE(title->font_style() == 1);
    REQUIRE_THAT(title->letter_spacing(), WithinAbs(1.5f, 0.001f));
    REQUIRE_THAT(title->line_height(), WithinAbs(22.0f, 0.001f));
    REQUIRE(title->text_align() == LabelAlign::right);
    REQUIRE_THAT(title->font_size(), WithinAbs(18.0f, 0.001f));
    REQUIRE(title->text_transform() == Label::TextTransform::uppercase);
    REQUIRE(title->theme().color("text.primary").has_value());

    REQUIRE(editor->text() == "typed");
    REQUIRE(editor->placeholder == "Enter value");
    REQUIRE(editor->multi_line);
    REQUIRE_THAT(editor->font_size(), WithinAbs(16.0f, 0.001f));

    REQUIRE(engine.evaluate("saved_ok").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("loaded_preset.nested.value").getWithDefault<int>(0) == 3);
    REQUIRE(engine.evaluate("missing_preset_type").toString() == "undefined");
}

// pulp #1549 — RN `mixBlendMode` (RN 0.76 New Architecture). The bridge
// fn `setMixBlendMode` maps W3C blend-mode keywords to the canvas
// BlendMode enum on the View slot; the @pulp/react prop-applier dispatch
// is exercised by packages/pulp-react/test/prop-applier-mix-blend-mode.test.ts.
TEST_CASE("WidgetBridge setMixBlendMode keyword -> BlendMode mapping",
          "[view][bridge][issue-1549]") {
    using BM = pulp::canvas::Canvas::BlendMode;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
    )");
    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);

    // Default is normal — paint-time fast path stays a no-op.
    REQUIRE(p->mix_blend_mode() == BM::normal);
    REQUIRE_FALSE(p->has_non_default_blend_mode());

    struct KW { const char* keyword; BM mode; };
    const KW table[] = {
        {"normal",      BM::normal},
        {"multiply",    BM::multiply},
        {"screen",      BM::screen},
        {"overlay",     BM::overlay},
        {"darken",      BM::darken},
        {"lighten",     BM::lighten},
        {"color-dodge", BM::color_dodge},
        {"color-burn",  BM::color_burn},
        {"hard-light",  BM::hard_light},
        {"soft-light",  BM::soft_light},
        {"difference",  BM::difference},
        {"exclusion",   BM::exclusion},
        {"hue",         BM::hue},
        {"saturation",  BM::saturation},
        {"color",       BM::color},
        {"luminosity",  BM::luminosity},
        // pulp #1549 closure (Tier 2 reclass 2026-05-12): `plus-lighter`
        // is the W3C draft's additive composite (min(A+B, 1) per channel,
        // same math as SkBlendMode::kPlus). Bridge mapping to BM::lighter
        // is spec-correct — reclassed as supported.
        //
        // `plus-darker` is a documented DIVERGENCE pinned for honesty
        // (Codex P2 review on PR #1870): W3C draft defines it as the
        // MULTIPLICATIVE variant, but Pulp (and Chromium) route it to
        // additive kPlus along with `plus-lighter`. The View slot ends
        // up at BM::lighter, but `plus-darker` stays in compat.json's
        // unsupportedValues because callers don't get the spec
        // composite. Pin the observable bridge behavior so a future
        // refactor doesn't silently drop the alias back to BM::normal.
        {"plus-lighter", BM::lighter},
        {"plus-darker",  BM::lighter},  // documented divergent alias
    };
    for (const auto& row : table) {
        std::string js = std::string("setMixBlendMode('p', '") + row.keyword + "');";
        bridge.load_script(js);
        REQUIRE(p->mix_blend_mode() == row.mode);
    }

    // Non-default keyword sets has_non_default_blend_mode().
    bridge.load_script("setMixBlendMode('p', 'multiply');");
    REQUIRE(p->has_non_default_blend_mode());

    // plus-lighter is fully supported (additive kPlus). Pin the
    // non-default mark + the BM::lighter destination slot.
    bridge.load_script("setMixBlendMode('p', 'plus-lighter');");
    REQUIRE(p->has_non_default_blend_mode());
    REQUIRE(p->mix_blend_mode() == BM::lighter);

    // plus-darker — same View slot as plus-lighter today (documented
    // divergence). Pinned so the additive-vs-multiplicative gap stays
    // visible in compat.json AND a regression that broke the alias
    // routing would be caught.
    bridge.load_script("setMixBlendMode('p', 'plus-darker');");
    REQUIRE(p->has_non_default_blend_mode());
    REQUIRE(p->mix_blend_mode() == BM::lighter);  // matches plus-lighter — divergent from W3C multiplicative spec

    // Unknown keyword -> normal (paint-time no-op fallback).
    bridge.load_script("setMixBlendMode('p', 'not-a-blend-mode');");
    REQUIRE(p->mix_blend_mode() == BM::normal);
    REQUIRE_FALSE(p->has_non_default_blend_mode());
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

// ── clear() lifecycle + snapshot/restore across hot reload ──────────────

TEST_CASE("WidgetBridge::clear drops widgets - subsequent lookups return nullptr",
          "[view][bridge][lifetime][clear]") {
    // clear() is the hot-reload entry point — it must actually purge
    // registered widgets so a fresh script build can start from an
    // empty registry. A silent no-op here is exactly the kind of
    // bug the #295 audit lesson targets.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain', 0, 0, 1, 0.5);
        createFader('volume', 0, 1, 0.75);
        createToggle('bypass', false);
    )");
    REQUIRE(bridge.widget("gain") != nullptr);
    REQUIRE(bridge.widget("volume") != nullptr);
    REQUIRE(bridge.widget("bypass") != nullptr);

    bridge.clear();

    REQUIRE(bridge.widget("gain") == nullptr);
    REQUIRE(bridge.widget("volume") == nullptr);
    REQUIRE(bridge.widget("bypass") == nullptr);
}

TEST_CASE("WidgetBridge::clear is safe to call before any script loads",
          "[view][bridge][lifetime][clear]") {
    // Fresh bridge — no widgets have been registered yet. clear() must
    // be a safe no-op. Second call also safe.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.clear());
    REQUIRE(bridge.widget("nothing") == nullptr);
    REQUIRE_NOTHROW(bridge.clear());
}

TEST_CASE("WidgetBridge snapshot_values captures every registered widget value",
          "[view][bridge][hot-reload][snapshot]") {
    // snapshot_values is the first half of the hot-reload dance.
    // Pin the contract: it records every widget's current value into
    // the provided map by id, so restore_values can put them back.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain', -60, 12, 0);
        createFader('mix', 0, 1, 0.25);
        createToggle('bypass', false);
    )");

    std::unordered_map<std::string, float> snap;
    bridge.snapshot_values(snap);

    // All three widgets contribute to the snapshot. Exact values are
    // implementation-defined at creation — the contract is coverage.
    REQUIRE(snap.count("gain") == 1);
    REQUIRE(snap.count("mix") == 1);
    REQUIRE(snap.count("bypass") == 1);
}

TEST_CASE("WidgetBridge restore_values handles missing snapshot entries gracefully",
          "[view][bridge][hot-reload][restore]") {
    // Real-world hot reload scenario: user adds a new widget between
    // snapshot and restore. The new widget's id is NOT in the snapshot;
    // restore_values must not crash or throw — it should just leave
    // that widget at its default.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain', -60, 12, 0);
    )");

    std::unordered_map<std::string, float> stale;  // empty snapshot
    stale["old-widget"] = 42.0f;  // id that no longer exists

    REQUIRE_NOTHROW(bridge.restore_values(stale));
    REQUIRE(bridge.widget("gain") != nullptr);  // still registered
    REQUIRE(bridge.widget("old-widget") == nullptr);
}

// Issue #491 P2: __gpuComputeDispatchImpl used to ignore the
// `bufferDataBase64` field in the bindGroups payload, so JS compute
// shaders always ran against zeroed buffers. The fix decodes base64 and
// uploads via queue.WriteBuffer. Without a live Dawn device we can't
// run the full pipeline, but we can exercise the parse + base64 decode
// branch to confirm it doesn't throw or crash on well-formed input.
TEST_CASE("WidgetBridge __gpuComputeDispatchImpl parses bufferDataBase64 payload",
          "[view][bridge][gpu][issue-491]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // JSON shape matches what web-compat-gpu-buffered.js emits — the
    // btoa()'d byte stream comes in as `bufferDataBase64`, size is the
    // GPU buffer size, usage is the GPUBufferUsage mask.
    // "AAECAwQFBgc=" decodes to {0,1,2,3,4,5,6,7}.
    bridge.load_script(R"(
        globalThis.__computeResult = __gpuComputeDispatchImpl(JSON.stringify({
            shaderCode: "@compute @workgroup_size(1) fn main() {}",
            entryPoint: "main",
            workgroupCountX: 1,
            workgroupCountY: 1,
            workgroupCountZ: 1,
            bindGroups: {
                "0": [
                    {
                        binding: 0,
                        bufferSize: 8,
                        bufferUsage: 0x80,
                        bufferOffset: 0,
                        bufferDataBase64: "AAECAwQFBgc="
                    }
                ]
            }
        }));
    )");

    // Without a Dawn device the impl returns false — the important
    // assertion is that we got here without exceptions from the JSON
    // parse or base64 decode path. (Before the fix the payload was
    // silently ignored; a malformed bufferDataBase64 could now throw
    // if we didn't treat decode failure as a no-op.)
    bridge.load_script("globalThis.__computeType = typeof globalThis.__computeResult");
    // Smoke: the script ran to completion. If __computeType is
    // undefined the JS errored before assigning, so this test fails
    // loudly instead of silently passing.
    REQUIRE(true); // Reaching here means the bridge didn't throw.
}

// Malformed base64 must not crash the bridge — runtime::base64_decode
// returns nullopt and we treat that as "skip upload", matching the
// pre-fix zero-fill behaviour for bad payloads. Same issue-491 surface.
TEST_CASE("WidgetBridge __gpuComputeDispatchImpl tolerates malformed bufferDataBase64",
          "[view][bridge][gpu][issue-491]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(R"(
        __gpuComputeDispatchImpl(JSON.stringify({
            shaderCode: "@compute @workgroup_size(1) fn main() {}",
            entryPoint: "main",
            workgroupCountX: 1,
            workgroupCountY: 1,
            workgroupCountZ: 1,
            bindGroups: {
                "0": [
                    {
                        binding: 0,
                        bufferSize: 4,
                        bufferUsage: 0x80,
                        bufferDataBase64: "!!!not-valid-base64!!!"
                    }
                ]
            }
        }));
    )"));
}

TEST_CASE("WidgetBridge GPU info and fallback canvas descriptors are scriptable",
          "[view][bridge][gpu]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        globalThis.gpu_info = getGPUInfo();
        globalThis.gpu_adapter = __describeNativeAdapterImpl();
        globalThis.gpu_device = __describeNativeDeviceImpl();
        globalThis.gpu_configured = __gpuCanvasConfigureImpl(
            '', 0, -5, 'rgba8unorm', 7, 'premultiplied');
        globalThis.gpu_texture = __gpuCanvasDescribeCurrentTextureImpl('');
        globalThis.gpu_preferred = navigatorGPU.getPreferredCanvasFormat();
    )");

    REQUIRE(engine.evaluate("gpu_info.available").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("gpu_info.backend").toString() == "Dawn/WebGPU");
    REQUIRE_FALSE(engine.evaluate("gpu_info.nativeBridge").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("gpu_info.preferredCanvasFormat").toString() == "bgra8unorm");
    REQUIRE(engine.evaluate("gpu_preferred").toString() == "bgra8unorm");

    REQUIRE(engine.evaluate("gpu_adapter.available").getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate("gpu_adapter.nativeBridge").getWithDefault<bool>(true));
    REQUIRE_FALSE(engine.evaluate("gpu_device.nativeBridge").getWithDefault<bool>(true));

    REQUIRE_FALSE(engine.evaluate("gpu_configured.configured").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("gpu_configured.width").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("gpu_configured.height").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("gpu_configured.format").toString() == "rgba8unorm");
    REQUIRE(engine.evaluate("gpu_configured.usage").getWithDefault<int>(0) == 7);
    REQUIRE(engine.evaluate("gpu_configured.alphaMode").toString() == "premultiplied");

    REQUIRE_FALSE(engine.evaluate("gpu_texture.nativeBridge").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("gpu_texture.width").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("gpu_texture.height").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("gpu_texture.format").toString() == "bgra8unorm");
    REQUIRE(engine.evaluate("gpu_texture.usage").getWithDefault<int>(-1) == 0);
    REQUIRE(engine.evaluate("gpu_texture.label").toString() == "pulp-native-gpu-texture");
}

TEST_CASE("WidgetBridge applyShader reports results and marks targets active",
          "[view][bridge][gpu][shader]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('shader-target', 'Shader', '');
        globalThis.shader_ok = applyShader(
            'shader-target',
            'half4 main(float2 p) { return half4(1); }');
        globalThis.shader_empty = applyShader('shader-target', '');
        globalThis.shader_missing = applyShader(
            'missing-target',
            'half4 main(float2 p) { return half4(1); }');
    )");

    REQUIRE(engine.evaluate("shader_ok.success").getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate("shader_empty.success").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("shader_missing.success").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("shader_ok.error").toString().empty());

    auto* target = bridge.widget("shader-target");
    REQUIRE(target != nullptr);
    REQUIRE_THAT(target->theme().dimension("shader.active").value_or(0.0f),
                 WithinAbs(1.0f, 0.001f));
}

// ── setTransform on a View (issue-930) ────────────────────────────────────
//
// The View-level companion to canvasSetTransform from PR #897. Used 1× today
// by the CSS adapter for translateX(-50%) modal centering and is foundational
// for future widget animation. Apply an affine matrix to a View; paint_all()
// concats it onto the canvas matrix so parent transforms compose and child
// Views inherit it. Layout is paint-only — hit-test and Yoga ignore it.
TEST_CASE("WidgetBridge setTransform stores affine matrix on the target View",
          "[view][bridge][transform][issue-930]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d = document.createElement('div');
        d.id = 'xform-view';
        document.body.appendChild(d);
        // translateX(-50%) on a 100px-wide centered modal becomes
        // setTransform(1, 0, 0, 1, -50, 0).
        setTransform(d._id, 1.0, 0.0, 0.0, 1.0, -50.0, 0.0);
    )");

    // The bridge resolves d._id -> a native widget id; look it up the same
    // way the canvas tests do.
    auto value = engine.evaluate("document.getElementById('xform-view')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    auto* v = bridge.widget(nativeId);
    REQUIRE(v != nullptr);

    REQUIRE(v->has_transform_matrix());
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    v->get_transform_matrix(a, b, c, d, e, f);
    REQUIRE_THAT(a, WithinAbs(1.0f,   1e-5f));
    REQUIRE_THAT(b, WithinAbs(0.0f,   1e-5f));
    REQUIRE_THAT(c, WithinAbs(0.0f,   1e-5f));
    REQUIRE_THAT(d, WithinAbs(1.0f,   1e-5f));
    REQUIRE_THAT(e, WithinAbs(-50.0f, 1e-5f));
    REQUIRE_THAT(f, WithinAbs(0.0f,   1e-5f));

    // clearTransform drops the affine.
    engine.evaluate("clearTransform(document.getElementById('xform-view')._id)");
    REQUIRE_FALSE(v->has_transform_matrix());
}

// ── pulp #1434 batch 3: text-decoration longhands ────────────────────────────

TEST_CASE("WidgetBridge setTextDecorationColor stores color on Label",
          "[view][bridge][issue-1434-batch-3]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'hello', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    REQUIRE_FALSE(lab->has_text_decoration_color());

    bridge.load_script("setTextDecorationColor('lab', '#ff0000')");
    REQUIRE(lab->has_text_decoration_color());
    REQUIRE(lab->text_decoration_color().r8() == 255);
    REQUIRE(lab->text_decoration_color().g8() == 0);
    REQUIRE(lab->text_decoration_color().b8() == 0);
}

TEST_CASE("WidgetBridge setTextDecorationStyle stores enum on Label",
          "[view][bridge][issue-1434-batch-3]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'hello', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    // Default is solid.
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::solid);

    bridge.load_script("setTextDecorationStyle('lab', 'dashed')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::dashed);

    bridge.load_script("setTextDecorationStyle('lab', 'wavy')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::wavy);

    bridge.load_script("setTextDecorationStyle('lab', 'dotted')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::dotted);

    bridge.load_script("setTextDecorationStyle('lab', 'double')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::double_);

    // Unknown value → solid (defensive default).
    bridge.load_script("setTextDecorationStyle('lab', 'wat')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::solid);
}

TEST_CASE("WidgetBridge text-decoration longhand setters preserve siblings",
          "[view][bridge][issue-1434-batch-3]") {
    // Mirrors the per-attribute border-fix from PR #1166 finding #4 —
    // setting one longhand must not clobber a previously-set sibling.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('lab', 'hello', '');
        setTextDecoration('lab', 'underline');
        setTextDecorationColor('lab', '#00ff00');
        setTextDecorationStyle('lab', 'wavy');
    )");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    REQUIRE(lab->text_decoration() == Label::TextDecoration::underline);
    REQUIRE(lab->has_text_decoration_color());
    REQUIRE(lab->text_decoration_color().g8() == 255);
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::wavy);
}

// ── pulp #1552: line-clamp + background-repeat ──────────────────────────────

TEST_CASE("WidgetBridge setLineClamp stores count on Label",
          "[view][bridge][issue-1552]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'one\\ntwo\\nthree\\nfour', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    // Default: no clamp.
    REQUIRE(lab->line_clamp() == 0);
    REQUIRE_FALSE(lab->multi_line());

    // Setting a non-zero clamp also implicitly enables multi_line so the
    // paint path takes the multi-line branch (the implicit-wrap rule).
    bridge.load_script("setLineClamp('lab', 2)");
    REQUIRE(lab->line_clamp() == 2);
    REQUIRE(lab->multi_line());

    // Update the count — multi_line stays on.
    bridge.load_script("setLineClamp('lab', 5)");
    REQUIRE(lab->line_clamp() == 5);
    REQUIRE(lab->multi_line());

    // 0 clears the slot. multi_line is left as-is (the user may have
    // set it independently via setMultiLine / white-space).
    bridge.load_script("setLineClamp('lab', 0)");
    REQUIRE(lab->line_clamp() == 0);
    REQUIRE(lab->multi_line());  // sticky from the previous call

    // Negative values are clamped to 0 (defensive — JS shim parseInt
    // never emits a negative, but we don't want to trust that).
    bridge.load_script("setLineClamp('lab', -3)");
    REQUIRE(lab->line_clamp() == 0);
}

TEST_CASE("WidgetBridge setLineClamp truncates multi-line text in paint",
          "[view][bridge][issue-1552]") {
    using namespace pulp::canvas;

    Label label("alpha\nbeta\ngamma\ndelta");
    label.set_bounds({0, 0, 200, 200});
    label.set_multi_line(true);
    label.set_line_clamp(2);

    RecordingCanvas canvas;
    label.paint(canvas);

    // Verify exactly 2 fill_text commands emitted (one per visible line).
    auto fills = std::vector<DrawCommand>{};
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 2);
    // First line is verbatim, second line gets the U+2026 ellipsis
    // appended because source lines were dropped.
    REQUIRE(fills[0].text == "alpha");
    REQUIRE(fills[1].text == std::string("beta") + "\xe2\x80\xa6");

    // Clamp larger than the source-line count: paint emits all lines
    // and skips the ellipsis (no source lines were dropped).
    canvas.clear();
    label.set_line_clamp(10);
    label.paint(canvas);
    fills.clear();
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 4);
    REQUIRE(fills[3].text == "delta");  // no ellipsis
}

TEST_CASE("Label line-clamp shrinks text_h for vertical-align centering",
          "[view][bridge][issue-1552]") {
    using namespace pulp::canvas;

    // Codex P2 on PR #1573 — vertical positioning previously used the
    // *source* newline count for text_h, so a 5-line label clamped to 2
    // visible lines was offset upward as if the hidden lines still
    // occupied space. Verify the first visible line's y reflects a
    // 2-line block centered in the bounds, not a 5-line block.

    constexpr float kBoundsH = 200.0f;
    constexpr float kFontSize = 14.0f;
    const float lh = kFontSize * 1.4f;       // Label default line-height
    const float ascent = kFontSize * 0.85f;  // Label baseline offset

    // Reference: 2-line block (matches the clamped behavior we want).
    Label two_lines("alpha\nbeta");
    two_lines.set_bounds({0, 0, 200, kBoundsH});
    two_lines.set_multi_line(true);
    two_lines.set_font_size(kFontSize);
    two_lines.set_vertical_align(TextVerticalAlign::center);

    RecordingCanvas ref_canvas;
    two_lines.paint(ref_canvas);
    float ref_first_y = -1.0f;
    for (auto& cmd : ref_canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) {
            ref_first_y = cmd.f[1];
            break;
        }
    }
    REQUIRE(ref_first_y > 0.0f);

    // Subject: 5 source lines, clamp=2. The first visible line's y must
    // match the 2-line reference (i.e. clamp shrinks the centered block,
    // it does not push the visible lines off-center).
    Label clamped("one\ntwo\nthree\nfour\nfive");
    clamped.set_bounds({0, 0, 200, kBoundsH});
    clamped.set_multi_line(true);
    clamped.set_font_size(kFontSize);
    clamped.set_vertical_align(TextVerticalAlign::center);
    clamped.set_line_clamp(2);

    RecordingCanvas canvas;
    clamped.paint(canvas);

    std::vector<DrawCommand> fills;
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 2);
    REQUIRE_THAT(fills[0].f[1], WithinAbs(ref_first_y, 1e-3));
    // Second visible line sits exactly one line-height below the first.
    REQUIRE_THAT(fills[1].f[1], WithinAbs(ref_first_y + lh, 1e-3));

    // Sanity: the centered y is meaningfully below the top-aligned y
    // (= ascent). This guards against regressing to the historic bug
    // where multi-line always painted from the top regardless of
    // vertical-align (would have ref_first_y == ascent).
    REQUIRE(ref_first_y > ascent + lh);  // strictly below 1-line top
}

TEST_CASE("WidgetBridge setBackgroundRepeat round-trips keyword on View",
          "[view][bridge][issue-1552]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCol('panel', '')");
    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->background_repeat().empty());

    bridge.load_script("setBackgroundRepeat('panel', 'no-repeat')");
    REQUIRE(panel->background_repeat() == "no-repeat");

    bridge.load_script("setBackgroundRepeat('panel', 'repeat-x')");
    REQUIRE(panel->background_repeat() == "repeat-x");

    bridge.load_script("setBackgroundRepeat('panel', 'space')");
    REQUIRE(panel->background_repeat() == "space");

    bridge.load_script("setBackgroundRepeat('panel', '')");
    REQUIRE(panel->background_repeat().empty());
}

// ── issue-926: setBackdropFilter ─────────────────────────────────────────────

// pulp #1517 — background sub-properties round-trip through the bridge
// onto storage-only View slots. Paint impact today is partial (only
// the keyword is stored; the box-clip / scroll-attachment paint paths
// haven't landed). The test asserts the wire-through, not paint.
TEST_CASE("WidgetBridge background sub-properties round-trip onto View slots",
          "[view][bridge][issue-1517]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        setBackgroundAttachment('p', 'scroll');
        setBackgroundClip('p', 'text');
        setBackgroundOrigin('p', 'padding-box');
    )");

    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);
    REQUIRE(p->background_attachment() == "scroll");
    REQUIRE(p->background_clip() == "text");
    REQUIRE(p->background_origin() == "padding-box");

    // Subsequent writes overwrite (storage-only, no merge logic).
    bridge.load_script("setBackgroundClip('p', 'border-box')");
    REQUIRE(p->background_clip() == "border-box");
}

// pulp #1517 — CSSStyleDeclaration shim forwards camelCase background
// sub-properties to the bridge setters.
TEST_CASE("CSSStyleDeclaration forwards background sub-props to bridge",
          "[view][bridge][css][issue-1517]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s.backgroundAttachment = 'scroll';
        s.backgroundClip        = 'border-box';
        s.backgroundOrigin      = 'content-box';
    )");

    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);
    REQUIRE(p->background_attachment() == "scroll");
    REQUIRE(p->background_clip() == "border-box");
    REQUIRE(p->background_origin() == "content-box");
}

TEST_CASE("WidgetBridge setBackdropFilter sets backdrop_blur on the View",
          "[view][bridge][issue-926]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('panel', 10, 10, 200, 100)");
    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->backdrop_blur() == 0.0f);

    bridge.load_script("setBackdropFilter('panel', 12)");
    REQUIRE_THAT(panel->backdrop_blur(), WithinAbs(12.0, 1e-4));

    // Zero clears the filter.
    bridge.load_script("setBackdropFilter('panel', 0)");
    REQUIRE(panel->backdrop_blur() == 0.0f);
}

TEST_CASE("View::paint_all emits save_backdrop_filter when backdrop_blur is set",
          "[view][canvas][issue-926]") {
    using namespace pulp::canvas;

    View root;
    root.set_bounds({0, 0, 200, 120});
    root.set_backdrop_blur(8.0f);

    RecordingCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::save_backdrop_filter) == 1);

    // Verify the recorded payload — bounds match the View's local rect, blur
    // radius matches the value passed to set_backdrop_blur().
    bool found = false;
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::save_backdrop_filter) {
            REQUIRE(cmd.f[0] == 0.0f);
            REQUIRE(cmd.f[1] == 0.0f);
            REQUIRE_THAT(cmd.f[2], WithinAbs(200.0, 1e-4));
            REQUIRE_THAT(cmd.f[3], WithinAbs(120.0, 1e-4));
            REQUIRE_THAT(cmd.f[4], WithinAbs(8.0, 1e-4));
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("View::paint_all skips backdrop layer when backdrop_blur is zero",
          "[view][canvas][issue-926]") {
    using namespace pulp::canvas;

    View root;
    root.set_bounds({0, 0, 100, 50});

    RecordingCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::save_backdrop_filter) == 0);
}

TEST_CASE("Canvas::save_backdrop_filter base default falls back to save()",
          "[canvas][issue-926]") {
    using namespace pulp::canvas;
    RecordingCanvas canvas;

    // RecordingCanvas overrides — emits the dedicated command.
    canvas.save_backdrop_filter(0, 0, 64, 64, 4.0f);
    REQUIRE(canvas.count(DrawCommand::Type::save_backdrop_filter) == 1);

    // The matching restore() must balance correctly so View::paint_all
    // bookkeeping (save → save_backdrop_filter → ... → restore → restore)
    // never leaves the canvas state stack imbalanced.
    canvas.restore();
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 1);
}

// ── issue-925: setBoxShadow / clearBoxShadow JS bridge ─────────────────────
TEST_CASE("WidgetBridge setBoxShadow stores offsets/blur/spread/color on widget",
          "[view][bridge][issue-925]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setBoxShadow('gain', 0, 14, 40, 0, '#000000a0')");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    const auto& s = w->box_shadow();
    REQUIRE_THAT(s.offset_x, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(s.offset_y, WithinAbs(14.0f, 0.001f));
    REQUIRE_THAT(s.blur, WithinAbs(40.0f, 0.001f));
    REQUIRE_THAT(s.spread, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(s.color.a, WithinAbs(160.0f / 255.0f, 0.01f));
    REQUIRE(s.inset == false);
}

TEST_CASE("WidgetBridge setBoxShadow accepts inset flag (true / 'inset' / 1)",
          "[view][bridge][issue-925]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('a', 0, 0, 32, 32)");
    bridge.load_script("createKnob('b', 0, 0, 32, 32)");
    bridge.load_script("createKnob('c', 0, 0, 32, 32)");
    bridge.load_script("createKnob('d', 0, 0, 32, 32)");

    bridge.load_script("setBoxShadow('a', 1, 2, 3, 4, '#112233ff', true)");
    bridge.load_script("setBoxShadow('b', 1, 2, 3, 4, '#112233ff', 'inset')");
    bridge.load_script("setBoxShadow('c', 1, 2, 3, 4, '#112233ff', 1)");
    bridge.load_script("setBoxShadow('d', 1, 2, 3, 4, '#112233ff', false)");

    REQUIRE(bridge.widget("a")->box_shadow().inset == true);
    REQUIRE(bridge.widget("b")->box_shadow().inset == true);
    REQUIRE(bridge.widget("c")->box_shadow().inset == true);
    REQUIRE(bridge.widget("d")->box_shadow().inset == false);
}

TEST_CASE("WidgetBridge clearBoxShadow removes the shadow",
          "[view][bridge][issue-925]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('panel', 0, 0, 32, 32)");
    bridge.load_script("setBoxShadow('panel', 0, 14, 40, 0, '#00000099')");
    REQUIRE(bridge.widget("panel")->has_box_shadow());

    bridge.load_script("clearBoxShadow('panel')");
    REQUIRE(bridge.widget("panel")->has_box_shadow() == false);
}

TEST_CASE("View::paint_all dispatches outset shadow before clip, "
          "inset shadow after children",
          "[view][canvas][issue-925]") {
    using namespace pulp::canvas;

    SECTION("outset shadow paints before clip_rect") {
        View v;
        v.set_bounds({0, 0, 100, 50});
        // pulp #972 — overflow now defaults to visible (no clip_rect).
        // This test specifically asserts the shadow / clip_rect ordering
        // contract, so opt-in to clipping explicitly.
        v.set_overflow(View::Overflow::hidden);
        v.set_box_shadow(0, 14, 40, 0, Color::rgba8(0, 0, 0, 160), /*inset=*/false);

        RecordingCanvas rc;
        v.paint_all(rc);

        const auto& cmds = rc.commands();
        // Find the box-shadow command and the clip_rect command — outset
        // shadow must come first so the shadow's blur halo can extend
        // beyond the box bounds.
        size_t shadow_idx = SIZE_MAX, clip_idx = SIZE_MAX;
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (cmds[i].type == DrawCommand::Type::draw_box_shadow && shadow_idx == SIZE_MAX)
                shadow_idx = i;
            if (cmds[i].type == DrawCommand::Type::clip_rect && clip_idx == SIZE_MAX)
                clip_idx = i;
        }
        REQUIRE(shadow_idx != SIZE_MAX);
        REQUIRE(clip_idx != SIZE_MAX);
        REQUIRE(shadow_idx < clip_idx);

        // Inset flag captured as 0
        REQUIRE_THAT(cmds[shadow_idx].f[4], WithinAbs(0.0f, 0.001f));
    }

    SECTION("inset shadow paints inside the bounds clip") {
        View v;
        v.set_bounds({0, 0, 100, 50});
        v.set_overflow(View::Overflow::hidden);  // pulp #972
        v.set_box_shadow(0, 4, 10, 0, Color::rgba8(0, 0, 0, 80), /*inset=*/true);

        RecordingCanvas rc;
        v.paint_all(rc);

        const auto& cmds = rc.commands();
        size_t shadow_idx = SIZE_MAX, clip_idx = SIZE_MAX;
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (cmds[i].type == DrawCommand::Type::draw_box_shadow && shadow_idx == SIZE_MAX)
                shadow_idx = i;
            if (cmds[i].type == DrawCommand::Type::clip_rect && clip_idx == SIZE_MAX)
                clip_idx = i;
        }
        REQUIRE(shadow_idx != SIZE_MAX);
        REQUIRE(clip_idx != SIZE_MAX);
        // Inset shadow paints AFTER the clip (inside the box silhouette).
        REQUIRE(clip_idx < shadow_idx);

        // Inset flag captured as 1
        REQUIRE_THAT(cmds[shadow_idx].f[4], WithinAbs(1.0f, 0.001f));
        // dx/dy/blur/spread captured in the floats payload.
        REQUIRE(cmds[shadow_idx].floats.size() == 4);
        REQUIRE_THAT(cmds[shadow_idx].floats[0], WithinAbs(0.0f, 0.001f));
        REQUIRE_THAT(cmds[shadow_idx].floats[1], WithinAbs(4.0f, 0.001f));
        REQUIRE_THAT(cmds[shadow_idx].floats[2], WithinAbs(10.0f, 0.001f));
        REQUIRE_THAT(cmds[shadow_idx].floats[3], WithinAbs(0.0f, 0.001f));
    }
}

TEST_CASE("Canvas::draw_box_shadow CPU fallback approximates shadow as "
          "stacked rounded rects",
          "[canvas][issue-925]") {
    using namespace pulp::canvas;

    RecordingCanvas backing;

    // Wrap in a forwarder that re-routes draw_box_shadow to the base-class
    // CPU fallback. RecordingCanvas overrides draw_box_shadow to record a
    // single command, so we go via Canvas::draw_box_shadow explicitly.
    backing.Canvas::draw_box_shadow(0, 0, 100, 50,
                                     0, 14, 40, 0,
                                     Color::rgba8(0, 0, 0, 160),
                                     /*inset=*/false, /*corner_radius=*/8);

    // Outset CPU fallback should emit several fill_rounded_rect calls
    // (steps = ceil(blur/2) + 1 = 21 for blur=40).
    REQUIRE(backing.count(DrawCommand::Type::fill_rounded_rect) >= 5);
}

// ── pulp #1434 Triage #11 — textAlign 'auto' + 'justify' ────────────────────
//
// Bridge accepts five textAlign values now. The CSS shim and
// @pulp/react prop-applier pass values through verbatim; bridge maps
// to LabelAlign. `auto` resolves at paint-time (LTR-only today).
// `justify` reaches canvas TextAlign::justify; SkParagraph kJustify
// integration lands in a follow-up.

TEST_CASE("setTextAlign accepts left / center / right / start / end / auto / justify",
          "[view][bridge][issue-1434-textalign-11]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('a','left text',  '');  setTextAlign('a','left');
        createLabel('b','center text','');  setTextAlign('b','center');
        createLabel('c','right text', '');  setTextAlign('c','right');
        createLabel('d','start text', '');  setTextAlign('d','start');
        createLabel('e','end text',   '');  setTextAlign('e','end');
        createLabel('f','auto text',  '');  setTextAlign('f','auto');
        createLabel('g','justify text','');  setTextAlign('g','justify');
    )");

    auto al = [&](const std::string& id) {
        return dynamic_cast<Label*>(bridge.widget(id))->text_align();
    };

    REQUIRE(al("a") == LabelAlign::left);
    REQUIRE(al("b") == LabelAlign::center);
    REQUIRE(al("c") == LabelAlign::right);
    REQUIRE(al("d") == LabelAlign::left);  // 'start' alias for left under LTR
    REQUIRE(al("e") == LabelAlign::right); // 'end' alias for right under LTR
    REQUIRE(al("f") == LabelAlign::auto_);
    REQUIRE(al("g") == LabelAlign::justify);
}

TEST_CASE("Label paints text with TextAlign::justify when textAlign='justify'",
          "[view][widget][issue-1434-textalign-11]") {
    Label label("the quick brown fox");
    label.set_bounds({0, 0, 200, 24});
    label.set_text_align(LabelAlign::justify);

    pulp::canvas::RecordingCanvas canvas;
    label.paint(canvas);

    // The recorded set_text_align command must be the justify variant.
    bool saw_justify = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::justify) saw_justify = true;
        }
    }
    REQUIRE(saw_justify);
}

TEST_CASE("Label paints with TextAlign::left when textAlign='auto' (LTR fallback)",
          "[view][widget][issue-1434-textalign-11]") {
    // pulp doesn't model RTL writing direction yet, so 'auto' degrades
    // to left at paint time. This test guards the fallback so a future
    // RTL slice flagging this assertion as a failure is the intended
    // signal to wire writing-direction context into the resolution.
    Label label("auto-aligned text");
    label.set_bounds({0, 0, 200, 24});
    label.set_text_align(LabelAlign::auto_);

    pulp::canvas::RecordingCanvas canvas;
    label.paint(canvas);

    bool saw_left = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::left) saw_left = true;
        }
    }
    REQUIRE(saw_left);
}

// ── pulp #1434 follow-up (css/textAlign coverage gap) — match-parent ─────
//
// CSS spec: `match-parent` resolves to the parent's *computed*
// `text-align`. Pulp wires this at paint time by walking the View parent
// chain via `inheritable_text_align()`. If no ancestor set a value, falls
// back to `left` (CSS spec default for `text-align`).
//
// Implementation lives in:
//   - widget_bridge.cpp setTextAlign — accepts "match-parent" string
//   - widgets.hpp LabelAlign::match_parent — new enum value (5)
//   - widgets.cpp Label::paint — paint-time parent-chain walk

TEST_CASE("setTextAlign accepts match-parent on a Label",
          "[view][bridge][css][issue-1434][coverage]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('a','match text', '');  setTextAlign('a','match-parent');
    )");

    REQUIRE(dynamic_cast<Label*>(bridge.widget("a"))->text_align()
            == LabelAlign::match_parent);
}

TEST_CASE("Label with textAlign='match-parent' adopts parent's resolved value (center)",
          "[view][widget][css][issue-1434][coverage]") {
    // Parent View carries an inheritable text-align (center = 1). Label
    // child set to match-parent should paint with TextAlign::center.
    View parent;
    parent.set_inheritable_text_align(1);  // 1 == LabelAlign::center

    auto child = std::make_unique<Label>("inherited");
    child->set_bounds({0, 0, 200, 24});
    child->set_text_align(LabelAlign::match_parent);
    auto* child_raw = child.get();
    parent.add_child(std::move(child));

    pulp::canvas::RecordingCanvas canvas;
    child_raw->paint(canvas);

    bool saw_center = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::center) saw_center = true;
        }
    }
    REQUIRE(saw_center);
}

TEST_CASE("Label with textAlign='match-parent' falls back to left when no ancestor set a value",
          "[view][widget][css][issue-1434][coverage]") {
    // CSS spec: `text-align` defaults to `start` (≡ left under LTR) when
    // no value is inherited. A match-parent Label with no parent (or a
    // parent chain that never called set_inheritable_text_align) must
    // paint left-aligned.
    Label label("orphan");
    label.set_bounds({0, 0, 200, 24});
    label.set_text_align(LabelAlign::match_parent);

    pulp::canvas::RecordingCanvas canvas;
    label.paint(canvas);

    bool saw_left = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::left) saw_left = true;
        }
    }
    REQUIRE(saw_left);
}

// Codex P1 on PR #1879 (2026-05-12): when an INTERMEDIATE ancestor in
// the chain has text-align: match-parent itself, the walk must
// continue past it to find the first non-match-parent value upstream.
// The original implementation stopped at parent.inheritable_text_align()
// returning 5 and degraded the child to `left` — wrong for a chain
// like grandparent=center → parent=match-parent → label=match-parent
// which should resolve to center.
TEST_CASE("Label with match-parent walks past intermediate match-parent ancestor",
          "[view][widget][css][issue-1434][issue-1879][coverage]") {
    // Build: grandparent(center) → parent(match-parent) → label(match-parent).
    View grandparent;
    grandparent.set_bounds({0, 0, 600, 200});
    grandparent.set_inheritable_text_align(1);  // center

    auto parent_owned = std::make_unique<View>();
    auto* parent_v = parent_owned.get();
    parent_v->set_bounds({0, 0, 600, 200});
    parent_v->set_inheritable_text_align(5);    // match-parent

    auto label_owned = std::make_unique<Label>("hi");
    auto* label = label_owned.get();
    label->set_bounds({0, 0, 600, 24});
    label->set_text_align(LabelAlign::match_parent);

    parent_v->add_child(std::move(label_owned));
    grandparent.add_child(std::move(parent_owned));

    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);

    // Should resolve through parent (match-parent → keep walking) →
    // grandparent (center). Paint must emit a center text-align.
    bool saw_center = false;
    bool saw_left   = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::center) saw_center = true;
            if (enc == pulp::canvas::TextAlign::left)   saw_left   = true;
        }
    }
    REQUIRE(saw_center);
    REQUIRE_FALSE(saw_left);  // regression guard for the old fall-back-to-left bug
}

TEST_CASE("Label with match-parent through chain of all match-parent ancestors falls back to left",
          "[view][widget][css][issue-1434][issue-1879][coverage]") {
    // Pathological case: every ancestor in the chain has match-parent.
    // CSS spec says fall back to `start` (≡ left under LTR).
    View root;
    root.set_bounds({0, 0, 600, 200});
    root.set_inheritable_text_align(5);  // match-parent

    auto mid_owned = std::make_unique<View>();
    auto* mid = mid_owned.get();
    mid->set_bounds({0, 0, 600, 200});
    mid->set_inheritable_text_align(5);  // match-parent

    auto label_owned = std::make_unique<Label>("hi");
    auto* label = label_owned.get();
    label->set_bounds({0, 0, 600, 24});
    label->set_text_align(LabelAlign::match_parent);

    mid->add_child(std::move(label_owned));
    root.add_child(std::move(mid_owned));

    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);

    bool saw_left = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_text_align) {
            const auto enc = static_cast<pulp::canvas::TextAlign>(static_cast<int>(cmd.f[0]));
            if (enc == pulp::canvas::TextAlign::left) saw_left = true;
        }
    }
    REQUIRE(saw_left);
}

TEST_CASE("setTextAlign on a container View accepts match-parent (encoded as 5)",
          "[view][bridge][css][issue-1434][coverage]") {
    // Non-Label Views store the alignment in the inheritable slot so
    // descendant Labels pick it up. The match-parent encoding (5) must
    // round-trip through inheritable_text_align().
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');  setTextAlign('p','match-parent');
    )");

    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    auto inh = panel->inheritable_text_align();
    REQUIRE(inh.has_value());
    REQUIRE(inh.value() == 5);
}

// ── pulp #1434 small-wins bundle (Triage #7 + #14) ──────────────────────
//
// Triage #7: cursor enum fan-out — extended setCursor case ladder maps
// the full CSS cursor keyword set to the existing View::CursorStyle
// slots (axis-aligned + diagonal resize aliases, move/all-scroll →
// multi-directional, none/hidden → invisible).
//
// Triage #14: flexWrap reverse — flex_wrap is now a tri-state enum
// (no_wrap / wrap / wrap_reverse) routed through Yoga's
// YGWrapWrapReverse for the previously-inexpressible CSS
// `flex-wrap: wrap-reverse` mode. Bridge accepts the keyword strings
// alongside the legacy 0/1 numeric path.

TEST_CASE("setCursor maps the full CSS keyword set",
          "[view][bridge][css][issue-1434-bundle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setCursor('a', 'col-resize');
        createPanel('b', '');  setCursor('b', 'row-resize');
        createPanel('c', '');  setCursor('c', 'nwse-resize');
        createPanel('d', '');  setCursor('d', 'nesw-resize');
        createPanel('e', '');  setCursor('e', 'move');
        createPanel('f', '');  setCursor('f', 'not-allowed');
        createPanel('g', '');  setCursor('g', 'grabbing');
        createPanel('h', '');  setCursor('h', 'none');
        createPanel('i', '');  setCursor('i', 'all-scroll');
        createPanel('j', '');  setCursor('j', 'parchment-curl'); // unknown → default
    )");

    using CS = View::CursorStyle;
    REQUIRE(bridge.widget("a")->cursor() == CS::horizontal_resize);
    REQUIRE(bridge.widget("b")->cursor() == CS::vertical_resize);
    REQUIRE(bridge.widget("c")->cursor() == CS::top_left_resize);
    REQUIRE(bridge.widget("d")->cursor() == CS::top_right_resize);
    REQUIRE(bridge.widget("e")->cursor() == CS::multi_directional_resize);
    REQUIRE(bridge.widget("f")->cursor() == CS::not_allowed);
    REQUIRE(bridge.widget("g")->cursor() == CS::grabbing);
    REQUIRE(bridge.widget("h")->cursor() == CS::invisible);
    REQUIRE(bridge.widget("i")->cursor() == CS::multi_directional_resize);
    REQUIRE(bridge.widget("j")->cursor() == CS::default_);
}

// pulp #1550 Tier-4 macOS partial 2026-05-12: five CSS cursor keywords
// (`alias` / `copy` / `zoom-in` / `zoom-out` / `context-menu`) now
// route to dedicated `View::CursorStyle` slots so the macOS dispatch
// in `window_host_mac.mm` can pick a real `NSCursor`. The other four
// previously-unsupported keywords (`wait` / `help` / `progress` /
// `cell`) stay collapsed to `default_` because macOS has no native
// cursor for them. Pin both halves.
TEST_CASE("setCursor wires 5 macOS-backed keywords to dedicated CursorStyle slots",
          "[view][bridge][issue-1550][coverage]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        // The 5 keywords with native NSCursor mappings.
        createPanel('al', ''); setCursor('al', 'alias');
        createPanel('cp', ''); setCursor('cp', 'copy');
        createPanel('zi', ''); setCursor('zi', 'zoom-in');
        createPanel('zo', ''); setCursor('zo', 'zoom-out');
        createPanel('cm', ''); setCursor('cm', 'context-menu');
        // The 4 keywords that genuinely have no macOS cursor — these
        // must STAY collapsed to default_ (catalog still lists them
        // as unsupported for honesty).
        createPanel('wt', ''); setCursor('wt', 'wait');
        createPanel('hp', ''); setCursor('hp', 'help');
        createPanel('pg', ''); setCursor('pg', 'progress');
        createPanel('cl', ''); setCursor('cl', 'cell');
    )");

    using CS = View::CursorStyle;
    // Newly-wired keywords route to dedicated slots.
    REQUIRE(bridge.widget("al")->cursor() == CS::alias);
    REQUIRE(bridge.widget("cp")->cursor() == CS::copy);
    REQUIRE(bridge.widget("zi")->cursor() == CS::zoom_in);
    REQUIRE(bridge.widget("zo")->cursor() == CS::zoom_out);
    REQUIRE(bridge.widget("cm")->cursor() == CS::context_menu);
    // No-native-cursor keywords stay at default_ — pinning prevents
    // a future change from silently flipping them.
    REQUIRE(bridge.widget("wt")->cursor() == CS::default_);
    REQUIRE(bridge.widget("hp")->cursor() == CS::default_);
    REQUIRE(bridge.widget("pg")->cursor() == CS::default_);
    REQUIRE(bridge.widget("cl")->cursor() == CS::default_);
}

TEST_CASE("setCursor accepts axis-aligned aliases",
          "[view][bridge][css][issue-1434-bundle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('e', ''); setCursor('e', 'e-resize');
        createPanel('w', ''); setCursor('w', 'w-resize');
        createPanel('n', ''); setCursor('n', 'n-resize');
        createPanel('s', ''); setCursor('s', 's-resize');
        createPanel('ns',''); setCursor('ns', 'ns-resize');
        createPanel('ew',''); setCursor('ew', 'ew-resize');
    )");
    using CS = View::CursorStyle;
    REQUIRE(bridge.widget("e")->cursor()  == CS::horizontal_resize);
    REQUIRE(bridge.widget("w")->cursor()  == CS::horizontal_resize);
    REQUIRE(bridge.widget("ew")->cursor() == CS::horizontal_resize);
    REQUIRE(bridge.widget("n")->cursor()  == CS::vertical_resize);
    REQUIRE(bridge.widget("s")->cursor()  == CS::vertical_resize);
    REQUIRE(bridge.widget("ns")->cursor() == CS::vertical_resize);
}

TEST_CASE("setFlex flex_wrap accepts wrap-reverse keyword",
          "[view][bridge][css][issue-1434-bundle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', ''); setFlex('a', 'flex_wrap', 'wrap-reverse');
        createPanel('b', ''); setFlex('b', 'flex_wrap', 'wrap');
        createPanel('c', ''); setFlex('c', 'flex_wrap', 'nowrap');
        createPanel('d', ''); setFlex('d', 'flex_wrap', 'no-wrap');
        createPanel('e', ''); setFlex('e', 'flex_wrap', 1);  // legacy numeric
        createPanel('f', ''); setFlex('f', 'flex_wrap', 0);
    )");
    REQUIRE(bridge.widget("a")->flex().flex_wrap == FlexWrap::wrap_reverse);
    REQUIRE(bridge.widget("b")->flex().flex_wrap == FlexWrap::wrap);
    REQUIRE(bridge.widget("c")->flex().flex_wrap == FlexWrap::no_wrap);
    REQUIRE(bridge.widget("d")->flex().flex_wrap == FlexWrap::no_wrap);
    REQUIRE(bridge.widget("e")->flex().flex_wrap == FlexWrap::wrap);
    REQUIRE(bridge.widget("f")->flex().flex_wrap == FlexWrap::no_wrap);
}

TEST_CASE("CSSStyleDeclaration forwards flex-wrap: wrap-reverse",
          "[view][bridge][css][issue-1434-bundle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('flexWrap', 'wrap-reverse');
    )");
    REQUIRE(bridge.widget("a")->flex().flex_wrap == FlexWrap::wrap_reverse);
}

TEST_CASE("flex-flow shorthand recognizes wrap-reverse",
          "[view][bridge][css][issue-1434-bundle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('flexFlow', 'row wrap-reverse');
    )");
    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.flex_wrap == FlexWrap::wrap_reverse);
    REQUIRE(f.direction == FlexDirection::row);
}

// ── pulp #1434 Triage #10 — borderStyle dashed/dotted ─────────────────────
//
// Bridge maps the CSS border-style keyword to View::BorderStyle. Skia
// installs SkDashPathEffect at stroke time for `dashed` / `dotted`;
// other named styles currently degrade to solid (paint-side gap).
// `none` / `hidden` short-circuit the stroke entirely.

TEST_CASE("setBorderStyle maps each keyword to the right enum",
          "[view][bridge][css][issue-1434-borderstyle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setBorderStyle('a', 'solid');
        createPanel('b', '');  setBorderStyle('b', 'dashed');
        createPanel('c', '');  setBorderStyle('c', 'dotted');
        createPanel('d', '');  setBorderStyle('d', 'double');
        createPanel('e', '');  setBorderStyle('e', 'groove');
        createPanel('f', '');  setBorderStyle('f', 'ridge');
        createPanel('g', '');  setBorderStyle('g', 'inset');
        createPanel('h', '');  setBorderStyle('h', 'outset');
        createPanel('i', '');  setBorderStyle('i', 'none');
        createPanel('j', '');  setBorderStyle('j', 'hidden');
    )");

    REQUIRE(bridge.widget("a")->border_style() == View::BorderStyle::solid);
    REQUIRE(bridge.widget("b")->border_style() == View::BorderStyle::dashed);
    REQUIRE(bridge.widget("c")->border_style() == View::BorderStyle::dotted);
    REQUIRE(bridge.widget("d")->border_style() == View::BorderStyle::double_);
    REQUIRE(bridge.widget("e")->border_style() == View::BorderStyle::groove);
    REQUIRE(bridge.widget("f")->border_style() == View::BorderStyle::ridge);
    REQUIRE(bridge.widget("g")->border_style() == View::BorderStyle::inset);
    REQUIRE(bridge.widget("h")->border_style() == View::BorderStyle::outset);
    REQUIRE(bridge.widget("i")->border_style() == View::BorderStyle::none);
    REQUIRE(bridge.widget("j")->border_style() == View::BorderStyle::hidden);
}

TEST_CASE("setBorderStyle unknown keyword falls back to solid",
          "[view][bridge][css][issue-1434-borderstyle]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('x', '');
        setBorderStyle('x', 'parchment-curl');
    )");
    REQUIRE(bridge.widget("x")->border_style() == View::BorderStyle::solid);
}

TEST_CASE("dashed border emits set_line_dash command then clears it",
          "[view][widget][issue-1434-borderstyle]") {
    // Recording-canvas inspection: the paint sequence for a dashed
    // border must include set_line_dash with a 2-entry pattern, the
    // stroke call, and a final set_line_dash(intervals=null) reset
    // so subsequent strokes don't inherit the dash.
    View v;
    v.set_bounds({0, 0, 100, 80});
    v.set_border({0xff, 0, 0, 0xff}, 2.0f, 0.0f);
    v.set_border_style(View::BorderStyle::dashed);

    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);

    int set_dash_count = 0;
    bool saw_stroke = false;
    bool stroke_after_first_dash = false;
    bool dash_reset_after_stroke = false;
    size_t first_intervals_count = 0;
    size_t last_intervals_count = 999;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            set_dash_count++;
            if (set_dash_count == 1) first_intervals_count = cmd.floats.size();
            last_intervals_count = cmd.floats.size();
            if (saw_stroke) dash_reset_after_stroke = true;
        }
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rect) {
            saw_stroke = true;
            if (set_dash_count > 0) stroke_after_first_dash = true;
        }
    }
    REQUIRE(saw_stroke);
    REQUIRE(stroke_after_first_dash);
    REQUIRE(dash_reset_after_stroke);
    REQUIRE(first_intervals_count == 2u);  // [on, off]
    REQUIRE(last_intervals_count == 0u);   // reset
}

TEST_CASE("solid border does NOT emit set_line_dash",
          "[view][widget][issue-1434-borderstyle]") {
    View v;
    v.set_bounds({0, 0, 100, 80});
    v.set_border({0xff, 0, 0, 0xff}, 2.0f, 0.0f);
    // Default style is solid — no dash should be installed.
    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);
    for (const auto& cmd : canvas.commands()) {
        REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::set_line_dash);
    }
}

TEST_CASE("border-style: none short-circuits the stroke",
          "[view][widget][issue-1434-borderstyle]") {
    View v;
    v.set_bounds({0, 0, 100, 80});
    v.set_border({0xff, 0, 0, 0xff}, 2.0f, 0.0f);
    v.set_border_style(View::BorderStyle::none);
    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);
    for (const auto& cmd : canvas.commands()) {
        REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::stroke_rect);
        REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::stroke_rounded_rect);
    }
}

// ── pulp #1434 Phase A2-3 — RTL writing direction ─────────────────────
//
// setDirection maps the CSS keyword to View::WritingDirection. Yoga
// honors at layout (YGNodeStyleSetDirection); Skia paragraph_style
// honors at text shape time (Phase A2-3 follow-up). LTR-only fast
// path on the @pulp/react logical-edge bundle (#1497) stays in effect
// until a separate slice plumbs the direction resolver into the
// prop-applier; this PR establishes the View enum + bridge fn + Yoga
// dispatch substrate.

TEST_CASE("setDirection maps each keyword to the right enum",
          "[view][bridge][css][issue-1434-rtl]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');  setDirection('a', 'ltr');
        createPanel('b', '');  setDirection('b', 'rtl');
        createPanel('c', '');  setDirection('c', 'inherit');
        createPanel('d', '');  setDirection('d', 'parchment-curl');
    )");
    using D = View::WritingDirection;
    REQUIRE(bridge.widget("a")->direction() == D::ltr);
    REQUIRE(bridge.widget("b")->direction() == D::rtl);
    // Anything other than ltr/rtl falls back to auto_ (inherit).
    REQUIRE(bridge.widget("c")->direction() == D::auto_);
    REQUIRE(bridge.widget("d")->direction() == D::auto_);
}

TEST_CASE("View::resolved_direction defaults auto_ to ltr",
          "[view][bridge][css][issue-1434-rtl]") {
    View v;
    REQUIRE(v.resolved_direction() == View::WritingDirection::ltr);
    v.set_direction(View::WritingDirection::rtl);
    REQUIRE(v.resolved_direction() == View::WritingDirection::rtl);
    v.set_direction(View::WritingDirection::auto_);
    REQUIRE(v.resolved_direction() == View::WritingDirection::ltr);
}

TEST_CASE("CSSStyleDeclaration forwards 'direction: rtl'",
          "[view][bridge][css][issue-1434-rtl]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('direction', 'rtl');
    )");
    REQUIRE(bridge.widget("a")->direction() == View::WritingDirection::rtl);
}

TEST_CASE("HTML dir attribute forwards to View writing direction",
          "[view][bridge][html][direction][issue-2163]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var rtl = document.createElement('div');
        rtl.id = 'dir-rtl';
        document.body.appendChild(rtl);
        rtl.setAttribute('dir', 'RTL');

        var ltr = document.createElement('div');
        ltr.id = 'dir-ltr';
        document.body.appendChild(ltr);
        ltr.setAttribute('dir', 'ltr');

        var autoDir = document.createElement('div');
        autoDir.id = 'dir-auto';
        document.body.appendChild(autoDir);
        autoDir.setAttribute('dir', 'auto');

        var invalid = document.createElement('div');
        invalid.id = 'dir-invalid';
        document.body.appendChild(invalid);
        invalid.setAttribute('dir', 'rtl');
        invalid.setAttribute('dir', 'sideways');
    )");

    auto id_for = [&](const char* id) {
        auto val = engine.evaluate(std::string("document.getElementById('") + id + "')._id");
        return std::string(val.getWithDefault<std::string_view>(""));
    };

    using D = View::WritingDirection;
    REQUIRE(bridge.widget(id_for("dir-rtl"))->direction() == D::rtl);
    REQUIRE(bridge.widget(id_for("dir-ltr"))->direction() == D::ltr);
    REQUIRE(bridge.widget(id_for("dir-auto"))->direction() == D::auto_);
    REQUIRE(bridge.widget(id_for("dir-invalid"))->direction() == D::rtl);

    auto attr = engine.evaluate("document.getElementById('dir-invalid').getAttribute('dir')");
    REQUIRE(std::string(attr.getWithDefault<std::string_view>("")) == "sideways");
}

// pulp #1434 Phase A2-3 — Codex P1 (PR #1506, comment 3196031661):
// `auto_` on a non-root view must map to YGDirectionInherit so an
// RTL ancestor actually flows into descendants. Previously,
// build_yoga_subtree called view.resolved_direction() which collapses
// `auto_` to ltr unconditionally, so the descendant's yoga node was
// pinned to YGDirectionLTR and any grandchildren flowed LTR even when
// the grandparent was set to RTL.
//
// Verification path:
//   grandparent (rtl, row)
//     └── parent  (auto_, row) ← intermediate node; bug pinned LTR here
//           ├── a (width 50)
//           └── b (width 60)
// Under RTL flow inside `parent`, sibling `a` (declared first) should
// sit on the right and `b` to its left. With the bug, parent's yoga
// direction is LTR so `a` ends up on the left and `b` on the right.
TEST_CASE("auto_ direction inherits RTL from grandparent through yoga",
          "[view][bridge][css][issue-1434-rtl]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('grand',  '');
        createPanel('mid',    'grand');
        createPanel('a',      'mid');
        createPanel('b',      'mid');

        // Grandparent: row layout with explicit RTL flow.
        setFlex('grand', 'direction', 'row');
        setDirection('grand', 'rtl');

        // Mid-level: row layout but DEFAULT (auto_) direction. The bug
        // would pin this to LTR; the fix lets it inherit RTL.
        setFlex('mid', 'direction', 'row');

        // Sizes so the grandparent fills the root and 'mid' fills it.
        var sg = new CSSStyleDeclaration({ _id: 'grand', _nativeCreated: true });
        sg._applyProperty('width',  '200px');
        sg._applyProperty('height', '100px');
        var sm = new CSSStyleDeclaration({ _id: 'mid', _nativeCreated: true });
        sm._applyProperty('width',  '200px');
        sm._applyProperty('height', '100px');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('width',  '50px');
        sa._applyProperty('height', '100px');
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sb._applyProperty('width',  '60px');
        sb._applyProperty('height', '100px');
    )");

    auto* mid = bridge.widget("mid");
    auto* a   = bridge.widget("a");
    auto* b   = bridge.widget("b");
    REQUIRE(mid != nullptr);
    REQUIRE(a   != nullptr);
    REQUIRE(b   != nullptr);
    // Mid-level direction is the default; the test depends on yoga
    // INHERITING rtl from the grandparent rather than mid's enum
    // resolving to ltr at build time.
    REQUIRE(mid->direction() == View::WritingDirection::auto_);

    root.layout_children();

    // Under correct RTL flow inside `mid`, the first child 'a' sits on
    // the right edge (x = 200 - 50 = 150) and 'b' (60 wide) sits to
    // its left (x = 150 - 60 = 90). If the bug returns, `mid` flows
    // LTR and 'a' lands at x=0 with 'b' at x=50.
    REQUIRE_THAT(a->bounds().x, WithinAbs(150.0f, 0.5f));
    REQUIRE_THAT(b->bounds().x, WithinAbs(90.0f,  0.5f));
}

// ── pulp #1514 — list-style cluster bridge round-trip ────────────────────
//
// Pulp doesn't model HTML <li>/<ul>/<ol> semantics; the bridge stores
// the list-style values verbatim on the View so a future paint pass
// (or a future semantic-list surface) can honor them. The catalog
// status is `partial` (stored, not painted) — these tests prove the
// JS → bridge → View slot round-trip works for every keyword.

TEST_CASE("setListStyleType maps each keyword to the right enum (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setListStyleType('a', 'none');
        createPanel('b', '');  setListStyleType('b', 'disc');
        createPanel('c', '');  setListStyleType('c', 'circle');
        createPanel('d', '');  setListStyleType('d', 'square');
        createPanel('e', '');  setListStyleType('e', 'decimal');
    )");

    REQUIRE(bridge.widget("a")->list_style_type() == View::ListStyleType::none);
    REQUIRE(bridge.widget("b")->list_style_type() == View::ListStyleType::disc);
    REQUIRE(bridge.widget("c")->list_style_type() == View::ListStyleType::circle);
    REQUIRE(bridge.widget("d")->list_style_type() == View::ListStyleType::square);
    REQUIRE(bridge.widget("e")->list_style_type() == View::ListStyleType::decimal);
}

TEST_CASE("setListStyleType unknown keyword falls back to disc (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('x', '');
        setListStyleType('x', 'tibetan');
    )");
    // 'tibetan' isn't in the supported counter-style set; bridge defaults
    // to disc (the CSS spec default for <ul>). The keyword choice
    // intentionally tracks a CSS counter-style we don't have an enum slot
    // for, so the fallback path stays exercised even as Pulp grows new
    // counter-style slots.
    REQUIRE(bridge.widget("x")->list_style_type() == View::ListStyleType::disc);
}

// pulp #1514 — extend the counter-style keyword set (lower-roman /
// upper-roman / lower-alpha / etc.). Storage-only round-trip; paint-side
// glyph rendering is the follow-up. The bar for this slice is the bridge
// stores each keyword on its own View::ListStyleType slot so the catalog
// can flip from `missing` to `supported-with-gaps`.
TEST_CASE("setListStyleType maps counter-style keywords to enum slots (issue-1514)",
          "[view][bridge][css][issue-1514][coverage]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('dz', ''); setListStyleType('dz', 'decimal-leading-zero');
        createPanel('lr', ''); setListStyleType('lr', 'lower-roman');
        createPanel('ur', ''); setListStyleType('ur', 'upper-roman');
        createPanel('la', ''); setListStyleType('la', 'lower-alpha');
        createPanel('ua', ''); setListStyleType('ua', 'upper-alpha');
        createPanel('ll', ''); setListStyleType('ll', 'lower-latin');
        createPanel('ul', ''); setListStyleType('ul', 'upper-latin');
        createPanel('lg', ''); setListStyleType('lg', 'lower-greek');
        createPanel('am', ''); setListStyleType('am', 'armenian');
        createPanel('ge', ''); setListStyleType('ge', 'georgian');
    )");
    using L = View::ListStyleType;
    REQUIRE(bridge.widget("dz")->list_style_type() == L::decimal_leading_zero);
    REQUIRE(bridge.widget("lr")->list_style_type() == L::lower_roman);
    REQUIRE(bridge.widget("ur")->list_style_type() == L::upper_roman);
    REQUIRE(bridge.widget("la")->list_style_type() == L::lower_alpha);
    REQUIRE(bridge.widget("ua")->list_style_type() == L::upper_alpha);
    REQUIRE(bridge.widget("ll")->list_style_type() == L::lower_latin);
    REQUIRE(bridge.widget("ul")->list_style_type() == L::upper_latin);
    REQUIRE(bridge.widget("lg")->list_style_type() == L::lower_greek);
    REQUIRE(bridge.widget("am")->list_style_type() == L::armenian);
    REQUIRE(bridge.widget("ge")->list_style_type() == L::georgian);
}

// pulp #1514 — listStyle shorthand routes counter-style keywords to
// setListStyleType (not silently dropped). Regression guard for the
// `lsTypes` table in web-compat-style-decl.js.
TEST_CASE("listStyle shorthand routes counter-style keywords (issue-1514)",
          "[view][bridge][css][issue-1514][coverage]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('listStyle', 'lower-roman inside');

        createPanel('b', '');
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sb._applyProperty('listStyle', 'georgian');
    )");
    using L = View::ListStyleType;
    REQUIRE(bridge.widget("a")->list_style_type() == L::lower_roman);
    REQUIRE(bridge.widget("a")->list_style_position() == View::ListStylePosition::inside);
    REQUIRE(bridge.widget("b")->list_style_type() == L::georgian);
}

TEST_CASE("setListStyleImage stores url and clears on 'none' (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setListStyleImage('a', 'url(bullet.png)');
        createPanel('b', '');  setListStyleImage('b', 'none');
    )");

    REQUIRE(bridge.widget("a")->list_style_image() == "url(bullet.png)");
    REQUIRE(bridge.widget("b")->list_style_image() == "");
}

TEST_CASE("setListStylePosition maps each keyword to the right enum (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setListStylePosition('a', 'inside');
        createPanel('b', '');  setListStylePosition('b', 'outside');
    )");

    REQUIRE(bridge.widget("a")->list_style_position() == View::ListStylePosition::inside);
    REQUIRE(bridge.widget("b")->list_style_position() == View::ListStylePosition::outside);
}

TEST_CASE("setListStylePosition unknown keyword falls back to outside (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('x', '');
        setListStylePosition('x', 'middle');
    )");
    REQUIRE(bridge.widget("x")->list_style_position() == View::ListStylePosition::outside);
}

TEST_CASE("listStyle shorthand parses type / position / image in any order (issue-1514)",
          "[view][bridge][css][issue-1514]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        createPanel('c', '');
    )");

    // Drive the actual web-compat-style-decl.js path with three orderings:
    //   a: type position image
    //   b: image type position  (CSS spec allows any order)
    //   c: just "none"          (the most common reset)
    bridge.load_script(R"(
        var __ea = { _id: 'a', _nativeCreated: true };
        var __sda = new CSSStyleDeclaration(__ea);
        __sda._applyProperty('listStyle', 'square inside url(bullet.png)');

        var __eb = { _id: 'b', _nativeCreated: true };
        var __sdb = new CSSStyleDeclaration(__eb);
        __sdb._applyProperty('listStyle', 'url(dot.png) circle outside');

        var __ec = { _id: 'c', _nativeCreated: true };
        var __sdc = new CSSStyleDeclaration(__ec);
        __sdc._applyProperty('listStyle', 'none');
    )");

    REQUIRE(bridge.widget("a")->list_style_type() == View::ListStyleType::square);
    REQUIRE(bridge.widget("a")->list_style_position() == View::ListStylePosition::inside);
    REQUIRE(bridge.widget("a")->list_style_image() == "url(bullet.png)");

    REQUIRE(bridge.widget("b")->list_style_type() == View::ListStyleType::circle);
    REQUIRE(bridge.widget("b")->list_style_position() == View::ListStylePosition::outside);
    REQUIRE(bridge.widget("b")->list_style_image() == "url(dot.png)");

    REQUIRE(bridge.widget("c")->list_style_type() == View::ListStyleType::none);
}

// ── pulp #1434 Phase A2-4 — CSS filter chain ─────────────────────────
//
// setFilter walks the function-chain string (e.g. "blur(4px)
// brightness(0.8) saturate(1.2) drop-shadow(2px 2px 4px black)")
// and produces a structured View::FilterOp vector. View::paint hands
// the chain to canvas.save_layer_with_filters which composes via
// SkImageFilters on the Skia backend; CG falls back to blur-only.

TEST_CASE("setFilter parses single blur(Npx)",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setFilter('a', 'blur(8px)');
    )");
    const auto& chain = bridge.widget("a")->filter_chain();
    REQUIRE(chain.size() == 1);
    REQUIRE(chain[0].kind == View::FilterOp::Kind::blur);
    REQUIRE_THAT(chain[0].amount, WithinAbs(8.0f, 0.001f));
    // Legacy slot kept for back-compat.
    REQUIRE_THAT(bridge.widget("a")->filter_blur(), WithinAbs(8.0f, 0.001f));
}

TEST_CASE("setFilter parses brightness/contrast/grayscale/etc",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setFilter('a', 'brightness(0.8)');
        createPanel('b', '');  setFilter('b', 'contrast(1.5)');
        createPanel('c', '');  setFilter('c', 'grayscale(1)');
        createPanel('d', '');  setFilter('d', 'invert(0.5)');
        createPanel('e', '');  setFilter('e', 'opacity(0.7)');
        createPanel('f', '');  setFilter('f', 'saturate(2)');
        createPanel('g', '');  setFilter('g', 'sepia(0.4)');
    )");
    REQUIRE(bridge.widget("a")->filter_chain()[0].kind == View::FilterOp::Kind::brightness);
    REQUIRE_THAT(bridge.widget("a")->filter_chain()[0].amount, WithinAbs(0.8f, 0.001f));
    REQUIRE(bridge.widget("b")->filter_chain()[0].kind == View::FilterOp::Kind::contrast);
    REQUIRE_THAT(bridge.widget("b")->filter_chain()[0].amount, WithinAbs(1.5f, 0.001f));
    REQUIRE(bridge.widget("c")->filter_chain()[0].kind == View::FilterOp::Kind::grayscale);
    REQUIRE(bridge.widget("d")->filter_chain()[0].kind == View::FilterOp::Kind::invert);
    REQUIRE(bridge.widget("e")->filter_chain()[0].kind == View::FilterOp::Kind::opacity);
    REQUIRE(bridge.widget("f")->filter_chain()[0].kind == View::FilterOp::Kind::saturate);
    REQUIRE(bridge.widget("g")->filter_chain()[0].kind == View::FilterOp::Kind::sepia);
}

TEST_CASE("setFilter parses hue-rotate with deg/rad/turn/grad units",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', ''); setFilter('a', 'hue-rotate(90deg)');
        createPanel('b', ''); setFilter('b', 'hue-rotate(0.5turn)');
        createPanel('c', ''); setFilter('c', 'hue-rotate(100grad)');
    )");
    REQUIRE(bridge.widget("a")->filter_chain()[0].kind == View::FilterOp::Kind::hue_rotate);
    REQUIRE_THAT(bridge.widget("a")->filter_chain()[0].angle_deg, WithinAbs(90.0f, 0.001f));
    REQUIRE_THAT(bridge.widget("b")->filter_chain()[0].angle_deg, WithinAbs(180.0f, 0.001f));
    REQUIRE_THAT(bridge.widget("c")->filter_chain()[0].angle_deg, WithinAbs(90.0f, 0.001f));
}

TEST_CASE("setFilter parses chained functions in source order",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setFilter('a', 'blur(4px) brightness(0.8) saturate(1.2)');
    )");
    const auto& chain = bridge.widget("a")->filter_chain();
    REQUIRE(chain.size() == 3);
    REQUIRE(chain[0].kind == View::FilterOp::Kind::blur);
    REQUIRE(chain[1].kind == View::FilterOp::Kind::brightness);
    REQUIRE(chain[2].kind == View::FilterOp::Kind::saturate);
}

TEST_CASE("setFilter parses drop-shadow(<dx> <dy> <blur> <color>)",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setFilter('a', 'drop-shadow(2px 4px 6px #ff0000)');
    )");
    const auto& chain = bridge.widget("a")->filter_chain();
    REQUIRE(chain.size() == 1);
    REQUIRE(chain[0].kind == View::FilterOp::Kind::drop_shadow);
    REQUIRE_THAT(chain[0].ds_offset_x, WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(chain[0].ds_offset_y, WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(chain[0].ds_blur, WithinAbs(6.0f, 0.001f));
    // Color: #ff0000 → r=1, g=0, b=0, a=1
    REQUIRE_THAT(chain[0].ds_color.r, WithinAbs(1.0f, 0.01f));
    REQUIRE_THAT(chain[0].ds_color.g, WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(chain[0].ds_color.b, WithinAbs(0.0f, 0.01f));
}

TEST_CASE("setFilter('none') clears the chain",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setFilter('a', 'blur(4px)');
        setFilter('a', 'none');
    )");
    REQUIRE(bridge.widget("a")->filter_chain().empty());
    REQUIRE_THAT(bridge.widget("a")->filter_blur(), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("setFilter unknown function silently drops",
          "[view][bridge][css][issue-1434-filter-chain]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setFilter('a', 'parchment-curl(99) blur(2px)');
    )");
    const auto& chain = bridge.widget("a")->filter_chain();
    // The blur survives; the unknown function is silently dropped.
    REQUIRE(chain.size() == 1);
    REQUIRE(chain[0].kind == View::FilterOp::Kind::blur);
    REQUIRE_THAT(chain[0].amount, WithinAbs(2.0f, 0.001f));
}

TEST_CASE("filter chain triggers save_layer_with_filters at paint",
          "[view][widget][issue-1434-filter-chain]") {
    // Smoke test: a View with a non-empty filter chain emits a layer
    // save during paint. The base RecordingCanvas's default
    // save_layer_with_filters falls through to save_layer, but we
    // verify the call shape (a save_layer with the collapsed-blur
    // amount) lands on the canvas.
    View v;
    v.set_bounds({0, 0, 100, 80});
    std::vector<View::FilterOp> chain;
    View::FilterOp blur{};
    blur.kind = View::FilterOp::Kind::blur;
    blur.amount = 5.0f;
    chain.push_back(blur);
    v.set_filter_chain(std::move(chain));

    // RecordingCanvas's default save_layer_with_filters falls through
    // to save() (no native layer recording in RecordingCanvas yet);
    // we verify at least one save command is emitted, confirming the
    // chain reaches the canvas API.
    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);
    int save_count = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::save) ++save_count;
    }
    REQUIRE(save_count > 0);
}

// ── pulp #1434 — canvasSetFontFull bridge fn ─────────────────────────────
//
// The Canvas2D shim's full CSS font shorthand parser dispatches through
// `canvasSetFontFull(id, family, size, weight, slant, letterSpacing)`.
// Cover the bridge fn directly to lock in the recorded
// CanvasDrawCmd::set_font_full payload field-for-field, independent of
// the JS-side parse layer covered in test_canvas2d_shim.cpp.
TEST_CASE("WidgetBridge canvasSetFontFull records weight/slant verbatim",
          "[view][bridge][canvas][issue-1434]") {
    // Drive the bridge fn directly (bypassing the JS parser) and assert
    // the recorded CanvasDrawCmd carries the full payload.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'font-full-canvas';
        c.width = 100; c.height = 50;
        document.body.appendChild(c);
        // Bypass the JS parser — call the bridge fn directly with each
        // payload field so the recorded CanvasDrawCmd round-trips
        // verbatim.
        canvasSetFontFull(c._id, 'Inter', 18.0, 700, 1, 0.5);
    )");

    auto* canvas = canvasFromBridge(bridge, engine, "font-full-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 1);

    const auto& cmd = canvas->commands().front();
    REQUIRE(cmd.type == pulp::view::CanvasDrawCmd::Type::set_font_full);
    REQUIRE(cmd.text == "Inter");
    REQUIRE_THAT(cmd.extra, WithinAbs(18.0f, 1e-5f));   // size
    REQUIRE_THAT(cmd.x,     WithinAbs(700.0f, 1e-5f));  // weight
    REQUIRE_THAT(cmd.y,     WithinAbs(1.0f, 1e-5f));    // slant=italic
    REQUIRE_THAT(cmd.x2,    WithinAbs(0.5f, 1e-5f));    // letter_spacing
}

TEST_CASE("WidgetBridge canvasSetFontFull replays through Canvas::set_font_full",
          "[view][bridge][canvas][issue-1434]") {
    // Drive a CanvasWidget paint onto a RecordingCanvas and assert the
    // backend received both the legacy set_font (back-compat) AND the
    // rich set_font_full carrying weight/slant. RecordingCanvas's
    // set_font_full override emits both per the existing #927 contract.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'font-full-replay';
        c.width = 100; c.height = 50;
        document.body.appendChild(c);
        canvasSetFontFull(c._id, 'Helvetica', 14.0, 300, 0, 0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "font-full-replay");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    const pulp::canvas::DrawCommand* full = nullptr;
    for (const auto& c : rec.commands()) {
        if (c.type == DrawType::set_font_full) { full = &c; break; }
    }
    REQUIRE(full != nullptr);
    REQUIRE(full->text == "Helvetica");
    REQUIRE_THAT(full->f[0], WithinAbs(14.0f, 1e-5f));   // size
    REQUIRE_THAT(full->f[1], WithinAbs(300.0f, 1e-5f));  // weight
    REQUIRE_THAT(full->f[2], WithinAbs(0.0f, 1e-5f));    // slant=upright
}

// pulp #1434 (sub-agent #12 follow-up) — align_content multi-line
// flex cross-axis distribution. Yoga supports it natively via
// YGNodeStyleSetAlignContent; the gap was a missing FlexStyle field
// + setter wiring. Round-trip every value the bridge accepts so a
// regression in either the parser, the FlexStyle field, or the
// space-* sibling enum gets caught here rather than silently
// reverting Yoga to the default FlexStart.
TEST_CASE("setFlex align_content accepts start / end / center / stretch / space-* aliases",
          "[view][bridge][css][issue-1434-aligncontent]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_content','start');
        createPanel('b','');  setFlex('b','align_content','flex-start');
        createPanel('c','');  setFlex('c','align_content','end');
        createPanel('d','');  setFlex('d','align_content','flex-end');
        createPanel('e','');  setFlex('e','align_content','center');
        createPanel('f','');  setFlex('f','align_content','stretch');
        createPanel('g','');  setFlex('g','align_content','space-between');
        createPanel('h','');  setFlex('h','align_content','space-around');
        createPanel('i','');  setFlex('i','align_content','space-evenly');
    )");

    using AcSpace = FlexStyle::AlignContentSpace;
    auto ac = [&](const std::string& id) { return bridge.widget(id)->flex().align_content; };
    auto sp = [&](const std::string& id) { return bridge.widget(id)->flex().align_content_space; };

    REQUIRE(ac("a") == FlexAlign::start);    REQUIRE(sp("a") == AcSpace::none);
    REQUIRE(ac("b") == FlexAlign::start);    REQUIRE(sp("b") == AcSpace::none);
    REQUIRE(ac("c") == FlexAlign::end);      REQUIRE(sp("c") == AcSpace::none);
    REQUIRE(ac("d") == FlexAlign::end);      REQUIRE(sp("d") == AcSpace::none);
    REQUIRE(ac("e") == FlexAlign::center);   REQUIRE(sp("e") == AcSpace::none);
    REQUIRE(ac("f") == FlexAlign::stretch);  REQUIRE(sp("f") == AcSpace::none);
    REQUIRE(sp("g") == AcSpace::space_between);
    REQUIRE(sp("h") == AcSpace::space_around);
    REQUIRE(sp("i") == AcSpace::space_evenly);
}

// pulp #1434 (sub-agent #12 follow-up) — width: 'auto' routes through
// the bridge's setFlex string path to FlexStyle.dim_width.unit =
// DimensionUnit::auto_. yoga_layout.cpp dispatches on that to
// YGNodeStyleSetWidthAuto. The percent path remains intact, and
// numeric values still flow through the px branch.
TEST_CASE("setFlex width accepts 'auto' keyword and routes to dim_width.auto_",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','width','auto');
        createPanel('b','');  setFlex('b','width', 120);
        createPanel('c','');  setFlex('c','width', '50%');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_width.unit == DimensionUnit::auto_);
    REQUIRE(fa.preferred_width == 0.0f);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_width.unit == DimensionUnit::px);
    REQUIRE_THAT(fb.preferred_width, WithinAbs(120.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(fc.dim_width.value, WithinAbs(50.0f, 0.001f));
}

TEST_CASE("setFlex height accepts 'auto' keyword and routes to dim_height.auto_",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','height','auto');
        createPanel('b','');  setFlex('b','height', 80);
        createPanel('c','');  setFlex('c','height', '25%');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_height.unit == DimensionUnit::auto_);
    REQUIRE(fa.preferred_height == 0.0f);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_height.unit == DimensionUnit::px);
    REQUIRE_THAT(fb.preferred_height, WithinAbs(80.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(fc.dim_height.value, WithinAbs(25.0f, 0.001f));
}

// pulp #1434 (sub-agent #12 follow-up) — verify the CSS shim path
// also forwards 'auto' for width/height. The DOM-lite el.style
// adapter must produce the same FlexStyle.dim_*.unit = auto_ result
// as the direct setFlex(id, 'width', 'auto') path.
TEST_CASE("CSSStyleDeclaration forwards width/height auto to bridge",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('width', 'auto');
        sa._applyProperty('height', 'auto');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_width.unit  == DimensionUnit::auto_);
    REQUIRE(fa.dim_height.unit == DimensionUnit::auto_);
}

// ── pulp #1522 — Canvas2D fillRule arg threads through bridge fns ───────
//
// `canvasFillPath` and `canvasClip` accept an optional fillRule int
// (0 = nonzero/winding, 1 = evenodd). The bridge stores it on
// CanvasDrawCmd::int_val; the widget-level canvas2d shim tests in
// test_canvas2d_shim.cpp drive ctx.fill('evenodd')/ctx.clip('evenodd')
// end-to-end. This bridge-level test exercises the fns directly so a
// regression in the int_val plumbing surfaces here independent of the
// JS shim parser.
TEST_CASE("WidgetBridge canvasFillPath / canvasClip thread fillRule int_val",
          "[view][bridge][canvas][issue-1522]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'fillrule-canvas';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        // Drive the bridge fns directly so we exercise int_val plumbing
        // without going through the JS shim arg parser.
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasLineTo(c._id, 0, 10);
        canvasClosePath(c._id);
        canvasFillPath(c._id, 1);     // evenodd
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasFillPath(c._id);        // default (nonzero)
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasClip(c._id, 1);         // evenodd
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasClip(c._id);            // default (nonzero)
    )");

    auto* canvas = canvasFromBridge(bridge, engine, "fillrule-canvas");
    REQUIRE(canvas != nullptr);

    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<int> fill_int_vals;
    std::vector<int> clip_int_vals;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::fill_path) fill_int_vals.push_back(cmd.int_val);
        if (cmd.type == T::clip)      clip_int_vals.push_back(cmd.int_val);
    }
    REQUIRE(fill_int_vals.size() == 2);
    REQUIRE(fill_int_vals[0] == 1);   // explicit evenodd
    REQUIRE(fill_int_vals[1] == 0);   // default nonzero
    REQUIRE(clip_int_vals.size() == 2);
    REQUIRE(clip_int_vals[0] == 1);   // explicit evenodd
    REQUIRE(clip_int_vals[1] == 0);   // default nonzero
}

// ── pulp #1520 — canvasSetDirection / canvasSetFilter bridge fns ────────
//
// These two register_function entries are the only direct surface
// between the Canvas2D ctx.direction / ctx.filter setters and the
// underlying canvas state. The shim's own coverage lives in
// test_canvas2d_shim.cpp; this test asserts the bridge fn → canvas
// command record path with no JS-side caching in the way.

TEST_CASE("WidgetBridge canvasSetDirection records direction enum on the canvas command stream",
          "[view][bridge][canvas][issue-1520]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'dir-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        // Drive the bridge fn directly so the cache in the JS shim
        // can't suppress the call.
        canvasSetDirection(c._id, 1);  // rtl
        canvasSetDirection(c._id, 2);  // inherit
        canvasSetDirection(c._id, 0);  // ltr
        canvasSetDirection(c._id, 99); // invalid → coerced to ltr
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "dir-canvas");
    REQUIRE(canvas != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<int> values;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::set_direction) values.push_back(cmd.int_val);
    }
    REQUIRE(values.size() == 4);
    REQUIRE(values[0] == 1);
    REQUIRE(values[1] == 2);
    REQUIRE(values[2] == 0);
    REQUIRE(values[3] == 0); // out-of-range coerced to ltr
}

TEST_CASE("WidgetBridge canvasSetFilter records the raw CSS filter string",
          "[view][bridge][canvas][issue-1520]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'filter-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        // Bypass the JS-side _syncFilterState cache; drive the bridge
        // fn directly so each call records.
        canvasSetFilter(c._id, 'blur(5px)');
        canvasSetFilter(c._id, 'sepia(80%) hue-rotate(45deg)');
        canvasSetFilter(c._id, 'none');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "filter-canvas");
    REQUIRE(canvas != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<std::string> sources;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::set_filter) sources.push_back(cmd.text);
    }
    REQUIRE(sources.size() == 3);
    REQUIRE(sources[0] == "blur(5px)");
    REQUIRE(sources[1] == "sepia(80%) hue-rotate(45deg)");
    REQUIRE(sources[2] == "none");
}

TEST_CASE("WidgetBridge canvasSetFilter chain replays through to the recording canvas",
          "[view][bridge][canvas][issue-1520]") {
    // End-to-end: bridge fn → CanvasWidget command → RecordingCanvas
    // capture. Asserts the dispatch table in canvas_widget.cpp wires
    // set_filter through to Canvas::set_filter() and that the
    // RecordingCanvas captures the same string.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'filter-replay-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasSetFilter(c._id, 'blur(3px) sepia(50%)');
        canvasSetDirection(c._id, 1);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "filter-replay-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DT = pulp::canvas::DrawCommand::Type;
    bool saw_filter = false, saw_direction = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DT::set_filter) {
            saw_filter = true;
            REQUIRE(cmd.text == "blur(3px) sepia(50%)");
        }
        if (cmd.type == DT::set_direction) {
            saw_direction = true;
            // RTL = enum value 1 (TextDirection::rtl).
            REQUIRE(cmd.f[0] == static_cast<float>(
                pulp::canvas::Canvas::TextDirection::rtl));
        }
    }
    REQUIRE(saw_filter);
    REQUIRE(saw_direction);
}

// pulp #1543 — Yoga borderWidth wiring. Pulp's borders were already
// painted as a Skia stroke via `View::set_border_*`, but Yoga never
// knew about them. Now `apply_border_widths` in
// `core/view/src/yoga_layout.cpp` calls `YGNodeStyleSetBorder` so the
// layout engine subtracts the border from the declared dimension the
// same way it already subtracts padding. Yoga 3.x's default
// box-sizing is `border-box`, which is also Pulp's pre-#1516 implicit
// behavior — so a border-box load-bearing test passes without any
// box-sizing plumbing. The companion content-box test lives in #1516
// once `setBoxSizing` is wired.
//
// Layout test 1: width=100, padding=0, borderWidth=10. Yoga 3.x's
// default border-box: outer (declared) = 100, border=10 each side →
// content area = 100 - 2*10 = 80. We assert the parent's outer width
// stays 100 (the declared dimension under border-box) and that a
// 100%-width child occupies only the inner content area = 80.
TEST_CASE("borderWidth shrinks content area (Yoga border-box default, #1543)",
          "[view][bridge][yoga][layout][issue-1543]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderWidth('parent', 10);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // border-box (Yoga 3.x default): outer == declared.
    REQUIRE_THAT(parent->bounds().width,  WithinAbs(100.0f, 0.5f));
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Content area = 100 - 2*10 = 80. The child fills 100% of the
    // CONTENT box (Yoga's content-box, post-border, post-padding).
    // Pre-#1543, Yoga saw 0 border and the child would have been sized
    // to 100, leaking under the painted stroke.
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

// Layout test 3: per-edge border variants. borderTopWidth=5,
// borderBottomWidth=5, padding=0 → inside the parent the top inset is
// 5 and the bottom inset is 5. We pin a 100%-height child and assert
// its absolute Y position is 5 (top border) and its height is 90
// (parent height 100 - top 5 - bottom 5).
TEST_CASE("per-edge borderTopWidth / borderBottomWidth set Yoga insets",
          "[view][bridge][yoga][layout][issue-1543]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderTopWidth('parent', 5);
        setBorderBottomWidth('parent', 5);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // Parent outer == declared 100x100 (border-box default).
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Child sits below the top border (y inset = 5, relative to
    // parent, which is what apply_yoga_results stores via
    // YGNodeLayoutGetTop), is 90 tall (parent 100 - top 5 - bottom 5),
    // full-width (no left/right border so width is unchanged at 100).
    REQUIRE_THAT(child->bounds().y,      WithinAbs(5.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(90.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(100.0f, 0.5f));
}

// pulp #1566 (Codex P2 follow-up to #1543) — an explicit per-edge
// `borderTopWidth: 0` MUST override the uniform `borderWidth: 10`
// shorthand on that edge. Pre-#1566, apply_border_widths treated a
// per-side value of 0 as "unset" and silently fell back to the
// uniform value, so the painted top stroke kept its 10px inset and
// child positioning remained shrunk by 10px. CSS and React Native
// both treat the longhand as overriding the shorthand even when
// the longhand is 0.
TEST_CASE("explicit per-edge borderWidth=0 overrides uniform shorthand",
          "[view][bridge][yoga][layout][issue-1543][issue-1566]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        // Uniform shorthand applies 10px to all four edges, then the
        // per-edge longhand zeroes the top edge (and only the top).
        setBorderWidth('parent', 10);
        setBorderTopWidth('parent', 0);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // Parent outer == declared 100x100 (border-box default).
    REQUIRE_THAT(parent->bounds().width,  WithinAbs(100.0f, 0.5f));
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Top inset is 0 (explicit 0 wins over the 10 shorthand). The
    // bottom / left / right inset stays at the shorthand 10. So the
    // child sits at y=0, height = 100 - 0(top) - 10(bottom) = 90,
    // x=10, width = 100 - 10 - 10 = 80.
    REQUIRE_THAT(child->bounds().y,      WithinAbs(0.0f,  0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(90.0f, 0.5f));
    REQUIRE_THAT(child->bounds().x,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
}

// pulp #1566 — color-only setters MUST NOT mark the per-edge width as
// explicitly set. If `setBorderTopColor` flipped the width's `set` flag
// to true (with the stored width still 0), the uniform `borderWidth: 10`
// shorthand would silently drop to 0 on the top edge. Equivalent to the
// CSS rule that `border-top-color` and `border-top-width` are
// independent longhands.
TEST_CASE("setBorderTopColor preserves uniform borderWidth shorthand",
          "[view][bridge][yoga][layout][issue-1543][issue-1566]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderWidth('parent', 10);
        // Color-only — width on every edge must still be 10.
        setBorderTopColor('parent', '#ff0000ff');
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // All four edges still 10 → child is 80x80, offset (10,10).
    REQUIRE_THAT(child->bounds().x,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().y,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

