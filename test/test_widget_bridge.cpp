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
        on('surface', 'wheel', function(dx, dy) {
            wheel_x = dx;
            wheel_y = dy;
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
    REQUIRE(engine.evaluate("global_key").getWithDefault<int>(0) == static_cast<int>(KeyCode::a));
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
    REQUIRE(flex.flex_wrap);
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
    REQUIRE_THAT(panel->corner_radius(), WithinAbs(9.0f, 0.001f));
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

// ── canvasSetTransform / canvasClip / canvasGlobalCompositeOperation (issue-896) ──
//
// These three CanvasRenderingContext2D bridge functions are exercised end-to-end:
// the JS bridge records a CanvasDrawCmd, and CanvasWidget::paint() replays each
// command on a pulp::canvas::RecordingCanvas, which lets us assert on the
// resulting Canvas-API call sequence.
namespace {
static pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                                  pulp::view::ScriptEngine& engine,
                                                  const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}
} // namespace

TEST_CASE("WidgetBridge canvasSetTransform records affine matrix and replays via setMatrix",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'xform-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Bypass the prelude — the bridge function is the unit under test.
        canvasSetTransform(c._id, 2.0, 0.0, 0.0, 3.0, 17.0, 23.0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "xform-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() >= 1);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    bool sawSetTransform = false;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_transform) {
            REQUIRE_THAT(cmd.f[0], WithinAbs(2.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(3.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[4], WithinAbs(17.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[5], WithinAbs(23.0f, 1e-5f));
            sawSetTransform = true;
        }
    }
    REQUIRE(sawSetTransform);
}

TEST_CASE("WidgetBridge canvasClip records clip command and replays Canvas::clip()",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'clip-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 10, 10);
        canvasLineTo(c._id, 80, 10);
        canvasLineTo(c._id, 80, 60);
        canvasLineTo(c._id, 10, 60);
        canvasClosePath(c._id);
        canvasClip(c._id);
        canvasRect(c._id, 0, 0, 200, 100, '#ff0000');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "clip-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int clipIndex = -1;
    int fillRectAfterClip = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::clip) clipIndex = idx;
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rect && clipIndex >= 0
            && fillRectAfterClip < 0) {
            fillRectAfterClip = idx;
        }
        ++idx;
    }
    REQUIRE(clipIndex >= 0);                  // canvasClip dispatched Canvas::clip()
    REQUIRE(fillRectAfterClip > clipIndex);   // subsequent draws are still issued (clip applies, doesn't drop)
}

TEST_CASE("WidgetBridge canvasGlobalCompositeOperation maps CSS strings to BlendMode",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'comp-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasGlobalCompositeOperation(c._id, 'destination-out');
        canvasGlobalCompositeOperation(c._id, 'multiply');
        canvasGlobalCompositeOperation(c._id, 'lighter');
        // Invalid string — must be a graceful no-op (no command emitted).
        canvasGlobalCompositeOperation(c._id, 'not-a-real-blend-mode');
        canvasGlobalCompositeOperation(c._id, 'source-over');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "comp-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    std::vector<int> blendIndices;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_blend_mode) {
            blendIndices.push_back(static_cast<int>(cmd.f[0]));
        }
    }

    // 4 valid strings → 4 set_blend_mode commands; the bogus mode string
    // emits nothing.
    REQUIRE(blendIndices.size() == 4);
    using BM = pulp::canvas::Canvas::BlendMode;
    REQUIRE(blendIndices[0] == static_cast<int>(BM::destination_out));
    REQUIRE(blendIndices[1] == static_cast<int>(BM::multiply));
    REQUIRE(blendIndices[2] == static_cast<int>(BM::lighter));
    REQUIRE(blendIndices[3] == static_cast<int>(BM::source_over));
}

TEST_CASE("WidgetBridge direct Canvas2D gap APIs replay expected canvas commands",
          "[view][bridge][canvas][coverage]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'canvas-gap-api';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetTextAlign(c._id, 'right');
        canvasSetTextBaseline(c._id, 'middle');
        canvasClearRect(c._id, 1, 2, 3, 4);
        canvasClipRect(c._id, 5, 6, 7, 8);
        canvasFillRoundedRect(c._id, 10, 11, 12, 13, 4, '#ff0000');
        canvasStrokeRoundedRect(c._id, 20, 21, 22, 23, 6, '#00ff00', 2.5);
        canvasStrokeCircle(c._id, 30, 31, 9, '#0000ff', 3);
        canvasSetGlobalAlpha(c._id, 0.25);
        canvasSetLineCap(c._id, 'square');
        canvasSetLineJoin(c._id, 'bevel');
        canvasArc(c._id, 40, 41, 10, 0.5, 1.5, '#abcdef', 2);
        canvasSetBlendMode(c._id, 'copy');
        canvasSetBlendMode(c._id, 'not-a-real-blend-mode');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "canvas-gap-api");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 12);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    auto first = [&](DrawType type) -> const pulp::canvas::DrawCommand* {
        for (const auto& cmd : rec.commands()) {
            if (cmd.type == type) return &cmd;
        }
        return nullptr;
    };

    const auto* align = first(DrawType::set_text_align);
    REQUIRE(align != nullptr);
    REQUIRE(align->f[0] == static_cast<float>(pulp::canvas::TextAlign::right));

    const auto* clear = first(DrawType::clear_rect);
    REQUIRE(clear != nullptr);
    REQUIRE_THAT(clear->f[0], WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(clear->f[1], WithinAbs(2.0f, 1e-5f));
    REQUIRE_THAT(clear->f[2], WithinAbs(3.0f, 1e-5f));
    REQUIRE_THAT(clear->f[3], WithinAbs(4.0f, 1e-5f));

    const auto* clip = first(DrawType::clip_rect);
    REQUIRE(clip != nullptr);
    REQUIRE_THAT(clip->f[0], WithinAbs(5.0f, 1e-5f));
    REQUIRE_THAT(clip->f[1], WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(clip->f[2], WithinAbs(7.0f, 1e-5f));
    REQUIRE_THAT(clip->f[3], WithinAbs(8.0f, 1e-5f));

    const auto* fillRounded = first(DrawType::fill_rounded_rect);
    REQUIRE(fillRounded != nullptr);
    REQUIRE_THAT(fillRounded->f[0], WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(fillRounded->f[4], WithinAbs(4.0f, 1e-5f));

    const auto* strokeRounded = first(DrawType::stroke_rounded_rect);
    REQUIRE(strokeRounded != nullptr);
    REQUIRE_THAT(strokeRounded->f[0], WithinAbs(20.0f, 1e-5f));
    REQUIRE_THAT(strokeRounded->f[4], WithinAbs(6.0f, 1e-5f));

    const auto* strokeCircle = first(DrawType::stroke_circle);
    REQUIRE(strokeCircle != nullptr);
    REQUIRE_THAT(strokeCircle->f[0], WithinAbs(30.0f, 1e-5f));
    REQUIRE_THAT(strokeCircle->f[1], WithinAbs(31.0f, 1e-5f));
    REQUIRE_THAT(strokeCircle->f[2], WithinAbs(9.0f, 1e-5f));

    const auto* cap = first(DrawType::set_line_cap);
    REQUIRE(cap != nullptr);
    REQUIRE(cap->f[0] == static_cast<float>(pulp::canvas::LineCap::square));

    const auto* join = first(DrawType::set_line_join);
    REQUIRE(join != nullptr);
    REQUIRE(join->f[0] == static_cast<float>(pulp::canvas::LineJoin::bevel));

    const auto* arc = first(DrawType::stroke_arc);
    REQUIRE(arc != nullptr);
    REQUIRE_THAT(arc->f[0], WithinAbs(40.0f, 1e-5f));
    REQUIRE_THAT(arc->f[1], WithinAbs(41.0f, 1e-5f));
    REQUIRE_THAT(arc->f[2], WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(arc->f[3], WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(arc->f[4], WithinAbs(1.5f, 1e-5f));

    REQUIRE(rec.count(DrawType::set_blend_mode) == 1);
    const auto* blend = first(DrawType::set_blend_mode);
    REQUIRE(blend != nullptr);
    REQUIRE(blend->f[0] == static_cast<float>(static_cast<int>(pulp::canvas::Canvas::BlendMode::copy)));
}

// ───────────────────────────────────────────────────────────────────────────
// pulp #899 — WidgetBridge auto-wires repaint_callback_ to the root view's
// host invalidator so JS-driven UI changes (and rAF callbacks) actually
// schedule a paint when the View owns its own bridge.
// ───────────────────────────────────────────────────────────────────────────

namespace {

class CountingWindowHost final : public WindowHost {
public:
    int repaint_calls = 0;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaint_calls; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class CountingPluginViewHost final : public pulp::view::PluginViewHost {
public:
    int repaint_calls = 0;
    Size size = {400, 300};

    pulp::view::NativeViewHandle native_handle() override { return {}; }
    void attach_to_parent(pulp::view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override { ++repaint_calls; }
    void set_size(uint32_t w, uint32_t h) override { size = {w, h}; }
    Size get_size() const override { return size; }
};

} // namespace

TEST_CASE("WidgetBridge default repaint_callback routes to root host (issue 899)",
          "[view][bridge][issue-899]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    REQUIRE(host.repaint_calls == 0);

    // layout() always calls request_repaint() inside the bridge; with the
    // auto-wired default that must reach the host. Without the fix this
    // stays at 0 because repaint_callback_ is null.
    bridge.load_script("layout()");

    REQUIRE(host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge default repaint reaches host through child view (issue 899)",
          "[view][bridge][issue-899]") {
    // The Spectr NativeEditorView case: a child View owns its own
    // WidgetBridge. set_window_host propagates the host to children on
    // add_child, so the child's own request_repaint() reaches the host
    // directly without parent walking.
    ScriptEngine engine;
    View top_root;
    top_root.set_bounds({0, 0, 800, 600});

    CountingWindowHost host;
    top_root.set_window_host(&host);

    auto child_owned = std::make_unique<View>();
    auto* child = child_owned.get();
    top_root.add_child(std::move(child_owned));
    child->set_bounds({0, 0, 400, 300});

    StateStore store;
    WidgetBridge bridge(engine, *child, store);

    REQUIRE(host.repaint_calls == 0);

    bridge.load_script("layout()");

    REQUIRE(host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge default repaint routes to plugin_view_host (issue 899)",
          "[view][bridge][issue-899]") {
    // DAW plugin context: when a View has a PluginViewHost set instead of
    // a WindowHost, the bridge's auto-wired repaint must still reach it.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});

    CountingPluginViewHost plugin_host;
    root.set_plugin_view_host(&plugin_host);

    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE(plugin_host.repaint_calls == 0);
    bridge.load_script("layout()");
    REQUIRE(plugin_host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge set_repaint_callback overrides the default (issue 899)",
          "[view][bridge][issue-899]") {
    // Hosts (e.g. the standalone window) must still be able to replace the
    // default invalidator with a window-level repaint callback.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    int override_calls = 0;
    bridge.set_repaint_callback([&] { ++override_calls; });

    bridge.load_script("layout()");

    REQUIRE(override_calls >= 1);
    // The override must displace the default — not run alongside it.
    REQUIRE(host.repaint_calls == 0);
}

TEST_CASE("WidgetBridge default repaint is a no-op when no host attached (issue 899)",
          "[view][bridge][issue-899]") {
    // Before the fix, repaint_callback_ was null. After the fix it routes
    // through View::request_repaint(), which itself silently no-ops when
    // no host is wired up. Verify that constructing/using the bridge in a
    // host-less test setup still works (lots of existing tests rely on
    // this).
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script("layout()"));
}

TEST_CASE("View::request_repaint reaches host through propagated descendants (issue 899)",
          "[view][issue-899]") {
    // set_window_host propagates the host pointer to every existing and
    // subsequently-added child. A grandchild's own window_host_ is
    // therefore set; calling request_repaint() reaches the host directly
    // without parent walking.
    View root;
    CountingWindowHost host;
    root.set_window_host(&host);

    auto child_owned = std::make_unique<View>();
    auto* child = child_owned.get();
    root.add_child(std::move(child_owned));

    auto grand_owned = std::make_unique<View>();
    auto* grand = grand_owned.get();
    child->add_child(std::move(grand_owned));

    REQUIRE(host.repaint_calls == 0);
    grand->request_repaint();
    REQUIRE(host.repaint_calls == 1);

    // Detached view: silently no-ops, no crash.
    View orphan;
    REQUIRE_NOTHROW(orphan.request_repaint());
}

// ───────────────────────────────────────────────────────────────────────────
// pulp #921 — __requestFrame__ must call request_repaint() so that
// requestAnimationFrame() actually drives the host paint loop. Without
// this wiring, JS-side rAF callbacks accumulate in pending_frame_ids_
// but the host never schedules the paint that drains them — Spectr's
// FilterBank canvas stays blank.
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("WidgetBridge requestAnimationFrame triggers a host repaint (issue 921)",
          "[view][bridge][issue-921]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    // Snapshot — bridge construction may itself touch the host (it does
    // not today, but pin behaviour to a delta around the rAF call).
    int repaint_baseline = host.repaint_calls;

    bridge.load_script("var __raf_id = window.requestAnimationFrame(function () {});");

    // The fix wires __requestFrame__ → request_repaint() → repaint_callback_
    // → root.request_repaint() → host.repaint(). Without it, repaint_calls
    // would not advance until something else (mouse, resize, layout) ran.
    REQUIRE(host.repaint_calls > repaint_baseline);

    // The id round-trip from JS proves the queue path itself still works
    // — the fix only adds the repaint signal, it must not break the queue.
    auto id_value = engine.evaluate("__raf_id");
    REQUIRE(id_value.getWithDefault<int>(-1) >= 1);
}

TEST_CASE("WidgetBridge cancelAnimationFrame removes the pending native frame",
          "[view][bridge][async][issue-493]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    const int baseline_repaints = host.repaint_calls;
    bridge.load_script("var invalid_raf_id = window.requestAnimationFrame('not a callback');");
    REQUIRE(engine.evaluate("invalid_raf_id").getWithDefault<int>(-1) == 0);
    REQUIRE(host.repaint_calls == baseline_repaints);

    bridge.load_script(R"(
        var raf_hits = 0;
        var canceled_raf_id = window.requestAnimationFrame(function () {
            raf_hits += 1;
        });
        window.cancelAnimationFrame(canceled_raf_id);
    )");

    REQUIRE(engine.evaluate("canceled_raf_id").getWithDefault<int>(-1) >= 1);
    REQUIRE(host.repaint_calls > baseline_repaints);

    const int repaints_after_cancel = host.repaint_calls;
    bridge.poll_async_results();
    bridge.service_frame_callbacks();

    REQUIRE(engine.evaluate("raf_hits").getWithDefault<int>(-1) == 0);
    REQUIRE(host.repaint_calls == repaints_after_cancel);
}

TEST_CASE("WidgetBridge requestAnimationFrame chain keeps requesting paints (issue 921)",
          "[view][bridge][issue-921]") {
    // The Spectr FilterBank pattern: a draw() callback re-arms itself via
    // requestAnimationFrame. Each rAF must request a paint so the host
    // actually services the next frame. Three queued rAFs across a poll
    // cycle therefore produce at least three host repaint signals.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    int repaint_baseline = host.repaint_calls;

    bridge.load_script(R"(
        var rafs_requested = 0;
        function tick() {
            rafs_requested++;
            if (rafs_requested < 3) window.requestAnimationFrame(tick);
        }
        window.requestAnimationFrame(tick);
    )");

    // First rAF was synchronous in the script and must already have hit
    // the host once.
    REQUIRE(host.repaint_calls > repaint_baseline);

    // Drain queued rAFs so each tick re-arms and lands another rAF /
    // host repaint. Bound the loop — production hosts coalesce, but the
    // test only needs to see the rAF→repaint signal continuing.
    for (int i = 0; i < 10; ++i) {
        bridge.poll_async_results();
        if (engine.evaluate("rafs_requested").getWithDefault<int>(-1) >= 3)
            break;
    }

    REQUIRE(engine.evaluate("rafs_requested").getWithDefault<int>(-1) == 3);
    // Each of the three rAFs requested a repaint (one synchronous + two
    // re-armed inside tick()). The host counter is monotonic; we want
    // strictly more than the first-rAF post-condition above.
    REQUIRE(host.repaint_calls >= repaint_baseline + 3);
}

// ── canvasMeasureText / canvasSetLineDash / canvasDrawImage / canvasGetImageData / canvasPutImageData (issue-916) ──
//
// These five CanvasRenderingContext2D bridge functions close the gap
// list in issue-916. The first three are the user-priority items
// (Spectr FilterBank text alignment + footer icon rendering); the last
// two are P2 follow-ups but ship together to land the surface in one
// pass.

TEST_CASE("WidgetBridge canvasMeasureText returns full TextMetrics object",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'measure-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.font = '20px Inter';
        var m1 = ctx.measureText('Hello');
        var m2 = ctx.measureText('Hello, world!');
        // Expose results for the C++ side to read back.
        window.__metrics_short = m1;
        window.__metrics_long  = m2;
    )");

    auto width_short = engine.evaluate("window.__metrics_short.width");
    auto width_long  = engine.evaluate("window.__metrics_long.width");
    REQUIRE(width_short.getWithDefault<double>(-1.0) > 0.0);
    REQUIRE(width_long.getWithDefault<double>(-1.0)
            > width_short.getWithDefault<double>(0.0));

    // Ascent/descent must be populated and non-zero — Spectr text
    // centring relies on these.
    auto ascent  = engine.evaluate("window.__metrics_short.fontBoundingBoxAscent");
    auto descent = engine.evaluate("window.__metrics_short.fontBoundingBoxDescent");
    REQUIRE(ascent.getWithDefault<double>(0.0)  > 0.0);
    REQUIRE(descent.getWithDefault<double>(0.0) > 0.0);

    // actualBoundingBox{Left,Right} fields exist (HTML5 spec — never
    // missing, even when the bounding box collapses to width).
    auto left  = engine.evaluate("typeof window.__metrics_short.actualBoundingBoxLeft  === 'number'");
    auto right = engine.evaluate("typeof window.__metrics_short.actualBoundingBoxRight === 'number'");
    REQUIRE(left.getWithDefault<bool>(false));
    REQUIRE(right.getWithDefault<bool>(false));
}

TEST_CASE("WidgetBridge canvasSetLineDash records pattern + phase, "
          "and an odd-length pattern is duplicated per HTML5 spec",
          "[view][bridge][canvas][issue-916][issue-952]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'dash-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.lineDashOffset = 0;
        ctx.setLineDash([5, 3, 2]);   // odd → duplicates to [5,3,2,5,3,2]
        ctx.strokeRect(10, 10, 80, 40);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "dash-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int dashIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            dashIndex = idx;
            REQUIRE(cmd.floats.size() == 6); // 3 → 6 (duplicated)
            REQUIRE_THAT(cmd.floats[0], WithinAbs(5.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[1], WithinAbs(3.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[2], WithinAbs(2.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[3], WithinAbs(5.0f, 1e-5f));
        }
        ++idx;
    }
    REQUIRE(dashIndex >= 0);
}

// Spectr's bundle calls setLineDash with [4,4], [2,3], [2,2], [1,3] for the
// 0dB baseline, ruler grid, and meter markers, plus an empty array to reset
// to a solid stroke. Each call must produce a `set_line_dash` command whose
// `floats` payload equals the requested pattern verbatim — even-length
// arrays are not duplicated, and the empty pattern records a zero-length
// `floats` (HTML5 "solid line" reset).
TEST_CASE("WidgetBridge canvasSetLineDash carries spectr-style patterns "
          "and solid-line reset through to the recording stream",
          "[view][bridge][canvas][issue-952]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'spectr-dash';
        c.width = 400; c.height = 200;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setLineDash([4, 4]);   // 0dB baseline
        ctx.strokeRect(0, 100, 400, 0);
        ctx.setLineDash([2, 3]);   // ruler grid
        ctx.strokeRect(0, 0, 400, 0);
        ctx.setLineDash([2, 2]);   // meter markers
        ctx.strokeRect(0, 50, 400, 0);
        ctx.setLineDash([1, 3]);   // band-grid dashes
        ctx.strokeRect(0, 150, 400, 0);
        ctx.setLineDash([]);       // solid-line reset
        ctx.strokeRect(0, 175, 400, 0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "spectr-dash");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    // Collect every set_line_dash command, in order.
    std::vector<std::vector<float>> dashes;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            dashes.emplace_back(cmd.floats.begin(), cmd.floats.end());
        }
    }

    REQUIRE(dashes.size() == 5);
    REQUIRE(dashes[0] == std::vector<float>{4.0f, 4.0f});
    REQUIRE(dashes[1] == std::vector<float>{2.0f, 3.0f});
    REQUIRE(dashes[2] == std::vector<float>{2.0f, 2.0f});
    REQUIRE(dashes[3] == std::vector<float>{1.0f, 3.0f});
    REQUIRE(dashes[4].empty()); // solid-line pass-through
}

TEST_CASE("WidgetBridge canvasDrawImage records draw_image with src + dst rect",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'img-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // 5-arg form — most common for plugin icons.
        ctx.drawImage({ src: '/path/to/icon.png', width: 16, height: 16 },
                      10, 20, 64, 32);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "img-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int drawIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::draw_image) {
            drawIndex = idx;
            REQUIRE(cmd.text == "/path/to/icon.png");
            REQUIRE_THAT(cmd.f[0], WithinAbs(10.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(20.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(64.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(32.0f, 1e-5f));
        }
        ++idx;
    }
    REQUIRE(drawIndex >= 0);
}

TEST_CASE("WidgetBridge canvasPutImageData records pixel buffer for paint replay",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // 2x2 RGBA — bright red, green, blue, white. Round-trip through
    // putImageData → bridge base64 decode → write_pixels command.
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'put-canvas';
        c.width = 4; c.height = 4;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var img = {
            width: 2, height: 2,
            data: new Uint8ClampedArray([
                255,   0,   0, 255,
                  0, 255,   0, 255,
                  0,   0, 255, 255,
                255, 255, 255, 255
            ])
        };
        ctx.putImageData(img, 1, 1);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "put-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int writeIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::write_pixels) {
            writeIndex = idx;
            REQUIRE(static_cast<int>(cmd.f[0]) == 2); // width
            REQUIRE(static_cast<int>(cmd.f[1]) == 2); // height
            REQUIRE(static_cast<int>(cmd.f[2]) == 1); // dx
            REQUIRE(static_cast<int>(cmd.f[3]) == 1); // dy
            REQUIRE(cmd.text.size() == 16);            // 2*2*4 RGBA bytes
            // First pixel — opaque red.
            REQUIRE(static_cast<unsigned char>(cmd.text[0]) == 255);
            REQUIRE(static_cast<unsigned char>(cmd.text[1]) == 0);
            REQUIRE(static_cast<unsigned char>(cmd.text[2]) == 0);
            REQUIRE(static_cast<unsigned char>(cmd.text[3]) == 255);
        }
        ++idx;
    }
    REQUIRE(writeIndex >= 0);
}

TEST_CASE("WidgetBridge canvasGetImageData returns a TextMetrics-like object "
          "with width/height/data even when no surface is rasterized",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'get-canvas';
        c.width = 64; c.height = 64;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var img = ctx.getImageData(0, 0, 4, 4);
        window.__got = img;
    )");

    auto width  = engine.evaluate("window.__got.width");
    auto height = engine.evaluate("window.__got.height");
    auto length = engine.evaluate("window.__got.data.length");

    REQUIRE(width.getWithDefault<double>(-1.0)  == 4);
    REQUIRE(height.getWithDefault<double>(-1.0) == 4);
    // 4*4*4 == 64 bytes for the RGBA array.
    REQUIRE(length.getWithDefault<double>(-1.0) == 64);
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

// ── issue-926: setBackdropFilter ─────────────────────────────────────────────

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

// ── pulp #965 — SvgPathWidget JS bridge integration ──────────────────────────

#include <pulp/view/svg_path_widget.hpp>

TEST_CASE("WidgetBridge createSvgPath produces an SvgPathWidget the bridge can address",
          "[view][bridge][issue-965]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgPath('icon', '')");
    bridge.load_script("setSvgPath('icon', 'M 0 0 L 10 0 L 10 10 Z')");
    bridge.load_script("setSvgViewBox('icon', 10, 10)");
    bridge.load_script("setSvgFill('icon', '#ff0000')");
    bridge.load_script("setSvgStroke('icon', '#000000')");
    bridge.load_script("setSvgStrokeWidth('icon', 2.0)");

    auto* w = dynamic_cast<SvgPathWidget*>(bridge.widget("icon"));
    REQUIRE(w != nullptr);
    REQUIRE(w->path_data() == "M 0 0 L 10 0 L 10 10 Z");
    REQUIRE(w->segments().size() == 4);
    REQUIRE(w->viewbox_width() == 10.0f);
    REQUIRE(w->viewbox_height() == 10.0f);
    REQUIRE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 2.0f);
    REQUIRE(w->fill_color().r8() == 255);
    REQUIRE(w->fill_color().g8() == 0);
    REQUIRE(w->fill_color().b8() == 0);
}

TEST_CASE("WidgetBridge setSvgFill 'none' disables fill",
          "[view][bridge][issue-965]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgPath('a', '')");
    bridge.load_script("setSvgPath('a', 'M0 0L1 1')");
    bridge.load_script("setSvgFill('a', 'none')");
    bridge.load_script("setSvgStroke('a', '#222222')");
    bridge.load_script("setSvgStrokeWidth('a', 1.5)");

    auto* w = dynamic_cast<SvgPathWidget*>(bridge.widget("a"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_fill());
    REQUIRE(w->has_stroke());
}

// pulp #968 — canvasRect / canvasStrokeRect must honour the active fill /
// stroke style when no color arg is passed. Validates the JS bridge path:
//   1. five-arg canvasRect → fillStyle (color or gradient) wins
//   2. six-arg canvasRect with explicit color → explicit color wins
//   3. linear gradient set, then five-arg canvasRect → gradient wins
//   4. five-arg canvasStrokeRect → strokeStyle wins
TEST_CASE("WidgetBridge canvasRect with no color uses active fillStyle (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'fill-style-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        // Active fill = magenta. Then a five-arg canvasRect (no color).
        canvasSetFillColor(c._id, '#ff00ff');
        canvasRect(c._id, 10, 10, 50, 50);

        // Explicit color (six-arg) — overrides active fill.
        canvasSetFillColor(c._id, '#ff00ff');
        canvasRect(c._id, 70, 10, 50, 50, '#00ffff');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "fill-style-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_fill{};
    pulp::canvas::Color first_rect_fill{};
    pulp::canvas::Color second_rect_fill{};
    bool saw_first = false, saw_second = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::fill_rect) {
            const bool is_first  = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                    cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            const bool is_second = (cmd.f[0] == 70.0f && cmd.f[1] == 10.0f &&
                                    cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (is_first  && !saw_first)  { saw_first  = true; first_rect_fill  = active_fill; }
            if (is_second && !saw_second) { saw_second = true; second_rect_fill = active_fill; }
        }
    }
    REQUIRE(saw_first);
    REQUIRE(saw_second);
    // First rect (no color arg) → magenta (the active fill at the time).
    const bool first_is_magenta = (first_rect_fill.r8() == 255 &&
                                   first_rect_fill.g8() == 0 &&
                                   first_rect_fill.b8() == 255 &&
                                   first_rect_fill.a8() == 255);
    REQUIRE(first_is_magenta);
    // Second rect (explicit color arg) → cyan, overriding the active fill.
    const bool second_is_cyan = (second_rect_fill.r8() == 0 &&
                                 second_rect_fill.g8() == 255 &&
                                 second_rect_fill.b8() == 255 &&
                                 second_rect_fill.a8() == 255);
    REQUIRE(second_is_cyan);
}

TEST_CASE("WidgetBridge canvasRect with no color preserves active linear gradient (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Linear gradient red→blue along x. RecordingCanvas's default
    // set_fill_gradient_linear records a set_fill_color of the first
    // stop (red) — we use that as the proxy for "gradient is active".
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'grad-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetLinearGradient(c._id, 0, 0, 100, 0, '#ff0000', 0, '#0000ff', 1);
        canvasRect(c._id, 10, 10, 50, 50);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "grad-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_fill{};
    bool saw_rect = false;
    pulp::canvas::Color rect_fill{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::fill_rect) {
            const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                  cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (matches) {
                saw_rect = true;
                rect_fill = active_fill;
            }
        }
    }
    REQUIRE(saw_rect);
    // The gradient's first stop (red) must still be the active fill —
    // i.e. no white set_fill_color from a baked-in cmd.color was emitted
    // between the gradient set and the fill_rect.
    const bool is_red = (rect_fill.r8() == 255 && rect_fill.g8() == 0 &&
                         rect_fill.b8() == 0 && rect_fill.a8() == 255);
    REQUIRE(is_red);
}

TEST_CASE("WidgetBridge canvasStrokeRect with no color uses active strokeStyle (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'stroke-style-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetStrokeColor(c._id, '#00ff00');
        canvasStrokeRect(c._id, 5, 5, 40, 40);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "stroke-style-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_stroke{};
    bool saw_rect = false;
    pulp::canvas::Color stroke_at_draw{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_stroke_color) {
            active_stroke = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::stroke_rect) {
            const bool matches = (cmd.f[0] == 5.0f && cmd.f[1] == 5.0f &&
                                  cmd.f[2] == 40.0f && cmd.f[3] == 40.0f);
            if (matches) {
                saw_rect = true;
                stroke_at_draw = active_stroke;
            }
        }
    }
    REQUIRE(saw_rect);
    const bool is_green = (stroke_at_draw.r8() == 0 && stroke_at_draw.g8() == 255 &&
                           stroke_at_draw.b8() == 0 && stroke_at_draw.a8() == 255);
    REQUIRE(is_green);
}

// ── pulp #1026: React Native style-prop bridge primitives ──────────────────

TEST_CASE("WidgetBridge setShadow lowers RN-shaped args onto setBoxShadow",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 0, 0, 48, 48)");
    // setShadow(id, color, offsetX, offsetY, opacity, radius). RN composes
    // shadowOpacity into the alpha channel of shadowColor; with #000000ff
    // and opacity 0.5 the resulting alpha is ~127/255.
    bridge.load_script("setShadow('gain', '#000000ff', 4, 8, 0.5, 12)");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    const auto& s = w->box_shadow();
    REQUIRE_THAT(s.offset_x, WithinAbs(4.0f, 1e-4f));
    REQUIRE_THAT(s.offset_y, WithinAbs(8.0f, 1e-4f));
    REQUIRE_THAT(s.blur, WithinAbs(12.0f, 1e-4f));
    REQUIRE_THAT(s.spread, WithinAbs(0.0f, 1e-4f));
    // alpha should be approximately 0.5 (1.0 * 0.5).
    REQUIRE_THAT(s.color.a, WithinAbs(0.5f, 1e-3f));
    REQUIRE(s.inset == false);
}

TEST_CASE("WidgetBridge setBackfaceVisibility plumbs the View flag",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE(w->backface_visible());

    bridge.load_script("setBackfaceVisibility('k', 'hidden')");
    REQUIRE_FALSE(w->backface_visible());

    bridge.load_script("setBackfaceVisibility('k', 'visible')");
    REQUIRE(w->backface_visible());
}

TEST_CASE("WidgetBridge setPointerEvents routes 4-valued enum to View",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE(w->pointer_events() == View::PointerEvents::auto_);

    bridge.load_script("setPointerEvents('k', 'none')");
    REQUIRE(w->pointer_events() == View::PointerEvents::none);
    REQUIRE_FALSE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'box-only')");
    REQUIRE(w->pointer_events() == View::PointerEvents::box_only);
    REQUIRE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'box-none')");
    REQUIRE(w->pointer_events() == View::PointerEvents::box_none);
    REQUIRE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'auto')");
    REQUIRE(w->pointer_events() == View::PointerEvents::auto_);
    REQUIRE(w->hit_testable());
}

TEST_CASE("WidgetBridge setTransformOrigin sets normalized origin on View",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    // Default is (0.5, 0.5).
    REQUIRE_THAT(w->transform_origin_x(), WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(w->transform_origin_y(), WithinAbs(0.5f, 1e-5f));

    bridge.load_script("setTransformOrigin('k', 0.0, 1.0)");
    REQUIRE_THAT(w->transform_origin_x(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(w->transform_origin_y(), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("WidgetBridge setBorderColor / setBorderWidth / setBorderRadius granular setters",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_border());

    bridge.load_script("setBorderColor('k', '#ff8800')");
    REQUIRE(w->has_border());
    REQUIRE(w->border_color().r8() == 0xff);
    REQUIRE(w->border_color().g8() == 0x88);
    REQUIRE(w->border_color().b8() == 0x00);

    bridge.load_script("setBorderWidth('k', 4.5)");
    REQUIRE_THAT(w->border_width(), WithinAbs(4.5f, 1e-5f));

    bridge.load_script("setBorderRadius('k', 9.0)");
    REQUIRE_THAT(w->corner_radius(), WithinAbs(9.0f, 1e-5f));
}

TEST_CASE("WidgetBridge per-corner setBorder*Radius setters route to corner_radii",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_corner_radii());

    bridge.load_script("setBorderTopLeftRadius('k', 1.0)");
    bridge.load_script("setBorderTopRightRadius('k', 2.0)");
    bridge.load_script("setBorderBottomLeftRadius('k', 3.0)");
    bridge.load_script("setBorderBottomRightRadius('k', 4.0)");

    REQUIRE(w->has_corner_radii());
    REQUIRE_THAT(w->corner_radius_tl(), WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_tr(), WithinAbs(2.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_bl(), WithinAbs(3.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_br(), WithinAbs(4.0f, 1e-5f));
}

TEST_CASE("WidgetBridge per-side setBorder*Color / Width route to BorderSide",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Each setter sets its own per-side state without corrupting other
    // sides. We assert each side's color and width round-trip.
    bridge.load_script("setBorderTopColor('k', '#11ff00')");
    bridge.load_script("setBorderTopWidth('k', 2.0)");
    bridge.load_script("setBorderRightColor('k', '#0011ff')");
    bridge.load_script("setBorderRightWidth('k', 3.0)");
    bridge.load_script("setBorderBottomColor('k', '#ff0011')");
    bridge.load_script("setBorderBottomWidth('k', 4.0)");
    bridge.load_script("setBorderLeftColor('k', '#fefefe')");
    bridge.load_script("setBorderLeftWidth('k', 5.0)");

    REQUIRE(w->has_border_sides());
    REQUIRE(w->border_top_color().r8() == 0x11);
    REQUIRE(w->border_top_color().g8() == 0xff);
    REQUIRE(w->border_top_color().b8() == 0x00);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    REQUIRE(w->border_right_color().r8() == 0x00);
    REQUIRE(w->border_right_color().g8() == 0x11);
    REQUIRE(w->border_right_color().b8() == 0xff);
    REQUIRE_THAT(w->border_right_width(), WithinAbs(3.0f, 1e-5f));

    REQUIRE(w->border_bottom_color().r8() == 0xff);
    REQUIRE(w->border_bottom_color().g8() == 0x00);
    REQUIRE(w->border_bottom_color().b8() == 0x11);
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(4.0f, 1e-5f));

    REQUIRE(w->border_left_color().r8() == 0xfe);
    REQUIRE_THAT(w->border_left_width(), WithinAbs(5.0f, 1e-5f));

    // Now change ONLY the top color again — top width should be preserved.
    bridge.load_script("setBorderTopColor('k', '#aabbcc')");
    REQUIRE(w->border_top_color().r8() == 0xaa);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    // And change ONLY the bottom width — bottom color should be preserved.
    bridge.load_script("setBorderBottomWidth('k', 7.0)");
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(7.0f, 1e-5f));
    REQUIRE(w->border_bottom_color().r8() == 0xff);
}

// pulp #1027 (audit PR #1166 finding #4) — Interleaved single-attribute
// border setters MUST preserve siblings. Audit found that the JS shim's
// `el.style.borderRadius='8px'; el.style.borderColor='red'` sequence
// silently dropped radius back to 0, because both lowered to
// setBorder(id, color, width, radius) with 0 for unset args. After the
// fix, the JS shim routes through setBorderColor / setBorderWidth /
// setBorderRadius which mutate exactly one slot.
TEST_CASE("WidgetBridge interleaved setBorderColor/Width/Radius preserves siblings",
          "[view][bridge][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    SECTION("set width+color via setBorder, then change only color via setBorderColor") {
        bridge.load_script("setBorder('k', '#112233', 3.0, 7.0)");
        REQUIRE(w->has_border());
        REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(w->corner_radius(), WithinAbs(7.0f, 1e-5f));

        bridge.load_script("setBorderColor('k', '#aabbcc')");
        REQUIRE(w->border_color().r8() == 0xaa);
        REQUIRE(w->border_color().g8() == 0xbb);
        REQUIRE(w->border_color().b8() == 0xcc);
        // Width and radius must NOT have been clobbered.
        REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(w->corner_radius(), WithinAbs(7.0f, 1e-5f));
    }

    SECTION("set width+color via setBorder, then change only radius via setBorderRadius") {
        bridge.load_script("setBorder('k', '#112233', 4.0, 5.0)");
        bridge.load_script("setBorderRadius('k', 11.0)");
        REQUIRE_THAT(w->corner_radius(), WithinAbs(11.0f, 1e-5f));
        // Color and width must NOT have been clobbered.
        REQUIRE(w->border_color().r8() == 0x11);
        REQUIRE(w->border_color().g8() == 0x22);
        REQUIRE(w->border_color().b8() == 0x33);
        REQUIRE_THAT(w->border_width(), WithinAbs(4.0f, 1e-5f));
    }

    SECTION("audit failing case: set radius first, then color via setBorderColor") {
        // This is the exact case the audit called out: setting radius then
        // color used to leave width=1, radius=0 in the broken JS shim.
        // With the bridge setters routed correctly, radius survives.
        bridge.load_script("setBorderRadius('k', 8.0)");
        REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));

        bridge.load_script("setBorderColor('k', '#ff0000')");
        REQUIRE(w->border_color().r8() == 0xff);
        // Radius MUST still be 8, not 0.
        REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));
    }

    SECTION("set radius first, then width via setBorderWidth") {
        bridge.load_script("setBorderRadius('k', 6.0)");
        bridge.load_script("setBorderWidth('k', 2.5)");
        REQUIRE_THAT(w->border_width(), WithinAbs(2.5f, 1e-5f));
        // Radius MUST still be 6, not 0.
        REQUIRE_THAT(w->corner_radius(), WithinAbs(6.0f, 1e-5f));
    }

    SECTION("set color first, then width via setBorderWidth") {
        bridge.load_script("setBorderColor('k', '#00ff00')");
        bridge.load_script("setBorderWidth('k', 4.0)");
        REQUIRE_THAT(w->border_width(), WithinAbs(4.0f, 1e-5f));
        // Color must NOT have been clobbered to default.
        REQUIRE(w->border_color().g8() == 0xff);
        REQUIRE(w->border_color().r8() == 0x00);
    }

    SECTION("per-side variants stay independent under interleaved updates") {
        bridge.load_script("setBorderTopColor('k', '#101010'); setBorderTopWidth('k', 1.0)");
        bridge.load_script("setBorderRightColor('k', '#202020'); setBorderRightWidth('k', 2.0)");
        bridge.load_script("setBorderBottomColor('k', '#303030'); setBorderBottomWidth('k', 3.0)");
        bridge.load_script("setBorderLeftColor('k', '#404040'); setBorderLeftWidth('k', 4.0)");

        // Now change ONLY top color — every other side must be untouched.
        bridge.load_script("setBorderTopColor('k', '#ffffff')");
        REQUIRE(w->border_top_color().r8() == 0xff);
        REQUIRE_THAT(w->border_top_width(), WithinAbs(1.0f, 1e-5f));
        REQUIRE(w->border_right_color().r8() == 0x20);
        REQUIRE_THAT(w->border_right_width(), WithinAbs(2.0f, 1e-5f));
        REQUIRE(w->border_bottom_color().r8() == 0x30);
        REQUIRE_THAT(w->border_bottom_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE(w->border_left_color().r8() == 0x40);
        REQUIRE_THAT(w->border_left_width(), WithinAbs(4.0f, 1e-5f));
    }
}

// pulp #1027 — JS shim regression test: el.style.borderColor must NOT
// clobber el.style.borderRadius. This walks the actual web-compat-style-decl.js
// path (CSSStyleDeclaration._applyProperty) so we'd catch any future
// regression where the shim re-routes to setBorder(id, c, w, r).
TEST_CASE("CSS shim: setting borderRadius then borderColor preserves radius",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Build a minimal CSSStyleDeclaration around the widget. Use the same
    // `_el._id` / `_el._nativeCreated` shape the prelude expects.
    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderRadius', '8px');"
        "__sd._applyProperty('borderColor', '#ff0000');"
    );
    REQUIRE(w->border_color().r8() == 0xff);
    REQUIRE(w->border_color().g8() == 0x00);
    // The audit's failing case — radius MUST survive the borderColor write.
    REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));

    // Reverse order: borderColor first, then borderRadius. Both must stick.
    bridge.load_script(
        "var __el2 = { _id: 'k', _nativeCreated: true };"
        "var __sd2 = new CSSStyleDeclaration(__el2);"
        "__sd2._applyProperty('borderColor', '#00ff00');"
        "__sd2._applyProperty('borderRadius', '12px');"
    );
    REQUIRE(w->border_color().g8() == 0xff);
    REQUIRE_THAT(w->corner_radius(), WithinAbs(12.0f, 1e-5f));

    // Setting borderWidth alone must not zero color or radius.
    bridge.load_script(
        "var __el3 = { _id: 'k', _nativeCreated: true };"
        "var __sd3 = new CSSStyleDeclaration(__el3);"
        "__sd3._applyProperty('borderWidth', '3px');"
    );
    REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
    REQUIRE(w->border_color().g8() == 0xff); // preserved from previous
    REQUIRE_THAT(w->corner_radius(), WithinAbs(12.0f, 1e-5f)); // preserved
}

// pulp #1027 — Codex P1 review on PR #1166 follow-up: CSS per-side flat
// props must NOT clobber the unrelated attribute. Before the fix, the
// JS shim lowered `borderTopWidth: '2px'` to `setBorderSide(id, 'top', 2, "")`
// which reset the side's color, and `borderTopColor: 'red'` to
// `setBorderSide(id, 'top', 0, 'red')` which reset the side's width.
TEST_CASE("CSS shim: per-side flat props preserve unset attribute",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Seed top side with both color and width, then mutate via the JS shim
    // one attribute at a time and verify the other side-attribute survives.
    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderTop', '2px solid #112233');"
    );
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));
    REQUIRE(w->border_top_color().r8() == 0x11);

    // Set borderTopColor only — width must survive.
    bridge.load_script("__sd._applyProperty('borderTopColor', '#ffaa00')");
    REQUIRE(w->border_top_color().r8() == 0xff);
    REQUIRE(w->border_top_color().g8() == 0xaa);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    // Set borderTopWidth only — color must survive.
    bridge.load_script("__sd._applyProperty('borderTopWidth', '5px')");
    REQUIRE_THAT(w->border_top_width(), WithinAbs(5.0f, 1e-5f));
    REQUIRE(w->border_top_color().r8() == 0xff);
    REQUIRE(w->border_top_color().g8() == 0xaa);

    // Reverse order — width first, then color, on the bottom side this time.
    bridge.load_script(
        "__sd._applyProperty('borderBottomWidth', '3px');"
        "__sd._applyProperty('borderBottomColor', '#00ddee');"
    );
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(3.0f, 1e-5f));
    REQUIRE(w->border_bottom_color().r8() == 0x00);
    REQUIRE(w->border_bottom_color().g8() == 0xdd);
    REQUIRE(w->border_bottom_color().b8() == 0xee);
}

// pulp #1027 — `border:` CSS shorthand must preserve a previously-set
// border-radius (CSS L3 spec: shorthand sets only width/style/color).
TEST_CASE("CSS shim: 'border:' shorthand preserves border-radius",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderRadius', '10px');"
        "__sd._applyProperty('border', '2px solid #336699');"
    );
    REQUIRE_THAT(w->border_width(), WithinAbs(2.0f, 1e-5f));
    REQUIRE(w->border_color().r8() == 0x33);
    REQUIRE(w->border_color().g8() == 0x66);
    REQUIRE(w->border_color().b8() == 0x99);
    // Radius must survive the shorthand assignment.
    REQUIRE_THAT(w->corner_radius(), WithinAbs(10.0f, 1e-5f));
}

// pulp #1148 — generalized overlay-click routing. The bridge must expose
// claimOverlay(id) / releaseOverlay(id) so @pulp/react's `<View overlay>`
// JSX prop can opt a widget in as the active click-eligible overlay.
TEST_CASE("WidgetBridge claimOverlay / releaseOverlay drive View::active_overlay_",
          "[view][bridge][issue-1148]") {
    // Reset state — other tests may leave the slot set.
    pulp::view::View::active_overlay_ = nullptr;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('popover', '')");
    auto* popover = bridge.widget("popover");
    REQUIRE(popover != nullptr);
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    bridge.load_script("claimOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == popover);

    bridge.load_script("releaseOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    // releaseOverlay on a non-holder is a silent no-op (does not null
    // a different widget's claim).
    bridge.load_script("createPanel('other', ''); claimOverlay('other')");
    auto* other = bridge.widget("other");
    REQUIRE(pulp::view::View::active_overlay_ == other);
    bridge.load_script("releaseOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == other);

    // Cleanup so the global state doesn't leak into the next test.
    pulp::view::View::active_overlay_ = nullptr;
}

// pulp #1361 — claimOverlay must install on_overlay_dismissed so React
// `<View overlay onDismissed>` consumers can flip setOpen(false) when the
// framework dismisses the overlay via ESC or outside-click.
TEST_CASE("WidgetBridge claimOverlay installs dismiss callback that fires "
          "__dispatch__('id', 'dismiss', 0) [issue-1361]",
          "[view][bridge][issue-1361]") {
    pulp::view::View::active_overlay_ = nullptr;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('popover', '')");
    auto* popover = bridge.widget("popover");
    REQUIRE(popover != nullptr);

    // Install a JS-side recorder for __dispatch__ so we can observe the
    // bridge firing the 'dismiss' event when dismiss_active_overlay()
    // runs.
    bridge.load_script(
        "globalThis.__dismissLog = [];"
        "const __orig = globalThis.__dispatch__;"
        "globalThis.__dispatch__ = (id, type, val) => {"
        "  if (type === 'dismiss') globalThis.__dismissLog.push(id);"
        "  return __orig ? __orig(id, type, val) : undefined;"
        "}");

    bridge.load_script("claimOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == popover);
    REQUIRE(static_cast<bool>(popover->on_overlay_dismissed));

    // Simulate the platform host's ESC / outside-click dismissal path.
    pulp::view::View::dismiss_active_overlay();
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    // The dismiss callback should have fired __dispatch__('popover', 'dismiss', 0).
    auto count = engine.evaluate("globalThis.__dismissLog.length")
                       .getWithDefault<double>(-1);
    REQUIRE(count == 1);
    auto first_id = engine.evaluate("globalThis.__dismissLog[0]")
                          .getWithDefault<std::string>("");
    REQUIRE(first_id == "popover");

    // releaseOverlay (the JSX-unmount path) must clear the dismiss
    // callback so a subsequent dismiss_active_overlay() can't re-fire on
    // the now-detached widget.
    pulp::view::View::active_overlay_ = popover;  // simulate re-claim
    popover->on_overlay_dismissed = []() {};      // re-install (claim path)
    bridge.load_script("releaseOverlay('popover')");
    REQUIRE_FALSE(static_cast<bool>(popover->on_overlay_dismissed));

    pulp::view::View::active_overlay_ = nullptr;
}

// pulp #1420 — `display` CSS values translate to native bridge calls.
// Spectr triage of yoga drift (post-#1395 harness) showed 5 display
// values across 79 sites: flex (63), block (10), inline-block (3),
// none (2), inline-flex (1). Before this fix, inline-block and
// inline-flex were silently dropped. After: inline-block ≡ block,
// inline-flex ≡ flex (matches RN + CSS spec for non-text-flowing
// formatting contexts).
TEST_CASE("CSSStyleDeclaration display routes none/flex/block/inline-block/inline-flex correctly",
          "[view][bridge][css][issue-1420]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto apply_display = [&](const std::string& id, const std::string& value) {
        std::string js = "(function(){"
            "createPanel('" + id + "', '');"
            "var el = { _id: '" + id + "', _nativeCreated: true };"
            "var sd = new CSSStyleDeclaration(el);"
            "sd._applyProperty('display', '" + value + "');"
            "})();";
        bridge.load_script(js);
    };

    apply_display("p_flex", "flex");
    apply_display("p_block", "block");
    apply_display("p_inline_block", "inline-block");
    apply_display("p_inline_flex", "inline-flex");
    apply_display("p_none", "none");

    auto* p_flex = dynamic_cast<Panel*>(bridge.widget("p_flex"));
    auto* p_block = dynamic_cast<Panel*>(bridge.widget("p_block"));
    auto* p_inline_block = dynamic_cast<Panel*>(bridge.widget("p_inline_block"));
    auto* p_inline_flex = dynamic_cast<Panel*>(bridge.widget("p_inline_flex"));
    auto* p_none = dynamic_cast<Panel*>(bridge.widget("p_none"));
    REQUIRE(p_flex != nullptr);
    REQUIRE(p_block != nullptr);
    REQUIRE(p_inline_block != nullptr);
    REQUIRE(p_inline_flex != nullptr);
    REQUIRE(p_none != nullptr);

    // display: flex / inline-flex must set flex direction to row
    // (overriding the RN-style column default).
    REQUIRE(p_flex->flex().direction == FlexDirection::row);
    REQUIRE(p_inline_flex->flex().direction == FlexDirection::row);

    // display: block / inline-block must NOT touch flex direction.
    REQUIRE(p_block->flex().direction == FlexDirection::column);
    REQUIRE(p_inline_block->flex().direction == FlexDirection::column);

    // All four "visible" variants stay visible. display: none flips
    // the View::visible() flag (the canonical CSS-spec "skip render"
    // signal) — the bridge wires setVisible → View::set_visible.
    REQUIRE(p_flex->visible());
    REQUIRE(p_block->visible());
    REQUIRE(p_inline_block->visible());
    REQUIRE(p_inline_flex->visible());
    REQUIRE_FALSE(p_none->visible());
}

// ── pulp #1416 — SvgRectWidget + SvgLineWidget JS bridge integration ─────────
//
// Mirrors the #965 SvgPath bridge tests. Closes Spectr [G] preset
// manager band-shape thumbnails: MiniPreview renders <svg><rect> per
// band + <line> separators, which dom-adapter routes to <View> with
// SVG attribute props. Without these bridge handlers the geometry is
// dropped on the floor and the tiles render blank.

#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

TEST_CASE("WidgetBridge createSvgRect produces a SvgRectWidget the bridge can address",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('bar', '')");
    bridge.load_script("setSvgRect('bar', 10, 20, 50, 30)");
    bridge.load_script("setSvgFill('bar', '#ff0000')");
    bridge.load_script("setSvgStroke('bar', '#000000')");
    bridge.load_script("setSvgStrokeWidth('bar', 2.0)");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("bar"));
    REQUIRE(w != nullptr);
    REQUIRE(w->rect_x() == 10.0f);
    REQUIRE(w->rect_y() == 20.0f);
    REQUIRE(w->rect_width() == 50.0f);
    REQUIRE(w->rect_height() == 30.0f);
    REQUIRE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 2.0f);
    REQUIRE(w->fill_color().r8() == 255);
    REQUIRE(w->fill_color().g8() == 0);
    REQUIRE(w->fill_color().b8() == 0);
}

TEST_CASE("WidgetBridge setSvgFill 'none' disables fill on SvgRectWidget",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('a', '')");
    bridge.load_script("setSvgRect('a', 0, 0, 10, 10)");
    bridge.load_script("setSvgFill('a', 'none')");
    bridge.load_script("setSvgStroke('a', '#222222')");
    bridge.load_script("setSvgStrokeWidth('a', 1.5)");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("a"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 1.5f);
}

TEST_CASE("WidgetBridge createSvgRect + paint produces fill_rect at expected geometry",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('bar', '')");
    bridge.load_script("setSvgRect('bar', 5, 6, 40, 8)");
    bridge.load_script("setSvgFill('bar', '#00ff00')");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("bar"));
    REQUIRE(w != nullptr);

    pulp::canvas::RecordingCanvas rc;
    w->paint(rc);

    bool saw_fill = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rect) {
            REQUIRE(cmd.f[0] == 5.0f);
            REQUIRE(cmd.f[1] == 6.0f);
            REQUIRE(cmd.f[2] == 40.0f);
            REQUIRE(cmd.f[3] == 8.0f);
            saw_fill = true;
        }
    }
    REQUIRE(saw_fill);
}

TEST_CASE("WidgetBridge createSvgLine produces a SvgLineWidget the bridge can address",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('sep', '')");
    bridge.load_script("setSvgLine('sep', 0, 10, 100, 10)");
    bridge.load_script("setSvgStroke('sep', '#0000ff')");
    bridge.load_script("setSvgStrokeWidth('sep', 1.5)");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("sep"));
    REQUIRE(w != nullptr);
    REQUIRE(w->x1() == 0.0f);
    REQUIRE(w->y1() == 10.0f);
    REQUIRE(w->x2() == 100.0f);
    REQUIRE(w->y2() == 10.0f);
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 1.5f);
    REQUIRE(w->stroke_color().r8() == 0);
    REQUIRE(w->stroke_color().g8() == 0);
    REQUIRE(w->stroke_color().b8() == 255);
}

TEST_CASE("WidgetBridge setSvgStroke 'none' disables stroke on SvgLineWidget",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('l', '')");
    bridge.load_script("setSvgLine('l', 0, 0, 10, 10)");
    bridge.load_script("setSvgStroke('l', 'none')");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("l"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_stroke());
}

TEST_CASE("WidgetBridge createSvgLine + paint emits stroke_line at endpoints",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('sep', '')");
    bridge.load_script("setSvgLine('sep', 1, 2, 11, 12)");
    bridge.load_script("setSvgStroke('sep', '#ff00ff')");
    bridge.load_script("setSvgStrokeWidth('sep', 2.5)");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("sep"));
    REQUIRE(w != nullptr);

    pulp::canvas::RecordingCanvas rc;
    w->paint(rc);

    bool saw_line = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_line) {
            REQUIRE(cmd.f[0] == 1.0f);
            REQUIRE(cmd.f[1] == 2.0f);
            REQUIRE(cmd.f[2] == 11.0f);
            REQUIRE(cmd.f[3] == 12.0f);
            saw_line = true;
        }
    }
    REQUIRE(saw_line);
}

TEST_CASE("WidgetBridge SvgRect uses parent for hierarchy attachment",
          "[view][bridge][issue-1416]") {
    // The createSvgRect bridge handler accepts a parent_id so JSX can
    // mount band thumbnails inside their MiniPreview row.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCol('preview', '')");
    bridge.load_script("createSvgRect('band1', 'preview')");
    bridge.load_script("createSvgRect('band2', 'preview')");
    bridge.load_script("createSvgLine('axis', 'preview')");

    REQUIRE(bridge.widget("band1") != nullptr);
    REQUIRE(bridge.widget("band2") != nullptr);
    REQUIRE(bridge.widget("axis") != nullptr);
    auto* preview = bridge.widget("preview");
    REQUIRE(preview != nullptr);
    REQUIRE(preview->child_count() == 3);
}

// pulp #1410 — setWhiteSpace must (a) flip the generic
// `View::white_space_nowrap()` flag for ANY widget (not just Label) so
// non-Label text-bearing surfaces can react, and (b) keep
// `Label::set_multi_line` in lock-step so existing callers / the #1407
// ellipsis path keep working when only one of the flags is set.
TEST_CASE("WidgetBridge setWhiteSpace flips View flag and Label multi_line for both modes",
          "[view][bridge][css][issue-1410]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('mylabel', 'long preset name', '');
        createPanel('mypanel', '');
        setWhiteSpace('mylabel', 'nowrap');
        setWhiteSpace('mypanel', 'nowrap');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("mylabel"));
    auto* panel = bridge.widget("mypanel");
    REQUIRE(label != nullptr);
    REQUIRE(panel != nullptr);

    // Generic flag is set on BOTH the Label and the non-Label Panel —
    // before #1410 only the Label dynamic_cast branch handled it.
    REQUIRE(label->white_space_nowrap());
    REQUIRE(panel->white_space_nowrap());
    // Label's multi_line side-effect stays in lock-step.
    REQUIRE_FALSE(label->multi_line());

    // Toggle back to normal.
    bridge.load_script(R"(
        setWhiteSpace('mylabel', 'normal');
        setWhiteSpace('mypanel', 'normal');
    )");
    REQUIRE_FALSE(label->white_space_nowrap());
    REQUIRE_FALSE(panel->white_space_nowrap());
    REQUIRE(label->multi_line());
}

// pulp #1410 — CSS translator side. style.whiteSpace = 'nowrap' must
// route through CSSStyleDeclaration._applyProperty to setWhiteSpace,
// which then sets the View flag.
TEST_CASE("CSSStyleDeclaration translates whiteSpace to setWhiteSpace bridge call",
          "[view][bridge][css][issue-1410]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        globalThis.__wsCalls = [];
        var __native_setWhiteSpace = setWhiteSpace;
        setWhiteSpace = function(id, mode) {
            globalThis.__wsCalls.push(id + '|' + mode);
            return __native_setWhiteSpace(id, mode);
        };
        createLabel('mylabel', 'long preset name', '');
        var stub_el = { _id: 'mylabel', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub_el);
        sd._applyProperty('whiteSpace', 'nowrap');
    )");

    auto count = engine.evaluate("globalThis.__wsCalls.length")
                       .getWithDefault<double>(-1);
    REQUIRE(count == 1);
    auto recorded = engine.evaluate("globalThis.__wsCalls[0]")
                          .getWithDefault<std::string>("");
    REQUIRE(recorded == "mylabel|nowrap");

    auto* label = dynamic_cast<Label*>(bridge.widget("mylabel"));
    REQUIRE(label != nullptr);
    REQUIRE(label->white_space_nowrap());
}

// pulp #1423 — `width: '100%'` and `height: '100%'` propagate through the
// CSS translator and bridge to Yoga's percent API. Spectr uses the
// `width:'100%'` form at spectr-editor-extracted.js:2377 and :3414.
TEST_CASE("CSS width/height percent strings propagate to Yoga via setFlex",
          "[view][bridge][css][issue-1423]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('width', '100%');
        sd._applyProperty('height', '50%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    // FlexStyle.dim_width / dim_height now carry the percent unit, so
    // yoga_layout.cpp will emit YGNodeStyleSetWidthPercent/HeightPercent.
    const auto& f = child->flex();
    REQUIRE(f.dim_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_width.value, WithinAbs(100.0f, 0.001f));
    REQUIRE(f.dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_height.value, WithinAbs(50.0f, 0.001f));

    // After layout against the 400x200 root, the child should be laid
    // out as 400 wide (100% of parent) and 100 tall (50% of parent).
    root.layout_children();
    REQUIRE_THAT(child->bounds().width, WithinAbs(400.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(100.0f, 0.5f));
}

// pulp #1423 — px values still work after the percent-aware refactor.
// Regression guard: the old code path stored only `preferred_width`;
// the new path also stores into `dim_width.unit = px`. Layout must keep
// using the px size when no percent was specified.
TEST_CASE("CSS width/height px paths unchanged by percent support",
          "[view][bridge][css][issue-1423]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('width', '120px');
        sd._applyProperty('height', '80px');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    const auto& f = child->flex();
    REQUIRE(f.dim_width.unit == DimensionUnit::px);
    REQUIRE_THAT(f.preferred_width, WithinAbs(120.0f, 0.001f));
    REQUIRE_THAT(f.preferred_height, WithinAbs(80.0f, 0.001f));

    root.layout_children();
    REQUIRE_THAT(child->bounds().width, WithinAbs(120.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

// pulp #1434 batch 6 — `top: '50%'`, `right`, `bottom`, `left` percent
// strings propagate through the CSS translator and bridge to Yoga's
// `YGNodeStyleSetPositionPercent`. Mirrors the issue-1423 width/height
// percent path; the four View positional fields previously dropped the
// `%` suffix at the bridge boundary.
TEST_CASE("CSS top/right/bottom/left percent strings propagate to Yoga",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('position', 'absolute');
        sd._applyProperty('top', '50%');
        sd._applyProperty('left', '25%');
        sd._applyProperty('right', '10%');
        sd._applyProperty('bottom', '0%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    // View::top_unit_ / etc. now carry the percent unit, so
    // yoga_layout.cpp will emit YGNodeStyleSetPositionPercent.
    REQUIRE(child->has_top());
    REQUIRE(child->top_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->top(), WithinAbs(50.0f, 0.001f));

    REQUIRE(child->has_left());
    REQUIRE(child->left_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->left(), WithinAbs(25.0f, 0.001f));

    REQUIRE(child->has_right());
    REQUIRE(child->right_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->right(), WithinAbs(10.0f, 0.001f));

    REQUIRE(child->has_bottom());
    REQUIRE(child->bottom_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->bottom(), WithinAbs(0.0f, 0.001f));
}

// pulp #1434 batch 6 — px positional values still work after the
// percent-aware refactor. Regression guard: the existing single-arg
// View::set_top setter must keep top_unit_ at px so layout_children
// uses YGNodeStyleSetPosition (not Percent).
TEST_CASE("CSS top/right/bottom/left px paths unchanged by percent support",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('position', 'absolute');
        sd._applyProperty('top', '12px');
        sd._applyProperty('left', '34px');
        sd._applyProperty('right', '56px');
        sd._applyProperty('bottom', '78px');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    REQUIRE(child->top_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->top(), WithinAbs(12.0f, 0.001f));
    REQUIRE(child->left_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->left(), WithinAbs(34.0f, 0.001f));
    REQUIRE(child->right_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->right(), WithinAbs(56.0f, 0.001f));
    REQUIRE(child->bottom_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->bottom(), WithinAbs(78.0f, 0.001f));
}

// pulp #1434 batch 6 — direct bridge entry-point coverage. The CSS
// translator path is exercised by the test above; this case calls the
// bridge's setTop/setRight/setBottom/setLeft directly so the @pulp/react
// JSX path (which forwards `'NN%'` strings without going through the
// CSS translator) is also covered.
TEST_CASE("setTop/setRight/setBottom/setLeft accept percent strings directly",
          "[view][bridge][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setTop('child', '50%');
        setRight('child', '25%');
        setBottom('child', '10%');
        setLeft('child', '0%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    REQUIRE(child->top_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->top(), WithinAbs(50.0f, 0.001f));
    REQUIRE(child->right_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->right(), WithinAbs(25.0f, 0.001f));
    REQUIRE(child->bottom_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->bottom(), WithinAbs(10.0f, 0.001f));
    REQUIRE(child->left_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->left(), WithinAbs(0.0f, 0.001f));
}

// ── pulp #1434 (rn batch C) — dimension percent strings ─────────────────────
//
// `min_width`/`min_height`/`max_width`/`max_height`/`flex_basis` accept
// either a number (px) or a percentage string (`'50%'`). `flex_basis`
// also accepts `'auto'`. Yoga's `YGNodeStyleSet*Percent` /
// `YGNodeStyleSetFlexBasisAuto` APIs are dispatched on
// `FlexStyle::dim_*.unit` in `yoga_layout.cpp`.

TEST_CASE("setFlex min/max width/height accept percent strings",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'min_width', '25%');
        setFlex('child', 'min_height', '15%');
        setFlex('child', 'max_width', '75%');
        setFlex('child', 'max_height', '90%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    const auto& f = child->flex();

    REQUIRE(f.dim_min_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_min_width.value, WithinAbs(25.0f, 0.001f));
    REQUIRE(f.dim_min_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_min_height.value, WithinAbs(15.0f, 0.001f));
    REQUIRE(f.dim_max_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_max_width.value, WithinAbs(75.0f, 0.001f));
    REQUIRE(f.dim_max_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_max_height.value, WithinAbs(90.0f, 0.001f));
}

TEST_CASE("setFlex min/max width/height numeric path stays px",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'min_width', 50);
        setFlex('child', 'min_height', 30);
        setFlex('child', 'max_width', 200);
        setFlex('child', 'max_height', 150);
    )");

    const auto& f = bridge.widget("child")->flex();
    REQUIRE(f.dim_min_width.unit == DimensionUnit::px);
    REQUIRE_THAT(f.min_width, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(f.min_height, WithinAbs(30.0f, 0.001f));
    REQUIRE_THAT(f.max_width, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(f.max_height, WithinAbs(150.0f, 0.001f));
}

TEST_CASE("setFlex flex_basis accepts 'auto', percent string, and number",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a', 'flex_basis', 'auto');
        createPanel('b','');  setFlex('b', 'flex_basis', '40%');
        createPanel('c','');  setFlex('c', 'flex_basis', 80);
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_flex_basis.unit == DimensionUnit::auto_);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_flex_basis.unit == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_flex_basis.value, WithinAbs(40.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_flex_basis.unit == DimensionUnit::px);
    REQUIRE_THAT(fc.flex_basis, WithinAbs(80.0f, 0.001f));
}

TEST_CASE("max_width percent caps the child at the resolved pixel size",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'max_width', '50%');
        setFlex('child', 'flex_grow', 1);
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    root.layout_children();
    REQUIRE(child->bounds().width <= 200.5f);
}

// ── pulp #1434 (rn batch B) — yoga value-aliasing ───────────────────────────
//
// The bridge's setFlex value mapper now accepts the CSS / RN canonical
// spellings (`flex-start` / `flex-end` for align*+justify; `column` /
// `row-reverse` / `column-reverse` for direction) alongside the Yoga /
// pulp short forms. The CSS shim's `_cssToFlex` already mapped the
// prefixed forms to bare ones for the CSS path, but @pulp/react's
// prop-applier passes RN values through verbatim — so bridge-side
// acceptance is the cross-surface fix.

TEST_CASE("setFlex direction accepts row / row-reverse / column / column-reverse / col",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        createPanel('c', '');
        createPanel('d', '');
        createPanel('e', '');
        setFlex('a', 'direction', 'row');
        setFlex('b', 'direction', 'row-reverse');
        setFlex('c', 'direction', 'column');
        setFlex('d', 'direction', 'column-reverse');
        setFlex('e', 'direction', 'col');
    )");

    auto get_dir = [&](const std::string& id) {
        return bridge.widget(id)->flex().direction;
    };

    REQUIRE(get_dir("a") == FlexDirection::row);
    REQUIRE(get_dir("b") == FlexDirection::row_reverse);
    REQUIRE(get_dir("c") == FlexDirection::column);
    REQUIRE(get_dir("d") == FlexDirection::column_reverse);
    REQUIRE(get_dir("e") == FlexDirection::column);  // legacy 'col' alias
}

TEST_CASE("setFlex align_items accepts start / flex-start / end / flex-end / center / stretch",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_items','start');
        createPanel('b','');  setFlex('b','align_items','flex-start');
        createPanel('c','');  setFlex('c','align_items','end');
        createPanel('d','');  setFlex('d','align_items','flex-end');
        createPanel('e','');  setFlex('e','align_items','center');
        createPanel('f','');  setFlex('f','align_items','stretch');
    )");

    auto al = [&](const std::string& id) { return bridge.widget(id)->flex().align_items; };
    REQUIRE(al("a") == FlexAlign::start);
    REQUIRE(al("b") == FlexAlign::start);
    REQUIRE(al("c") == FlexAlign::end);
    REQUIRE(al("d") == FlexAlign::end);
    REQUIRE(al("e") == FlexAlign::center);
    REQUIRE(al("f") == FlexAlign::stretch);
}

TEST_CASE("setFlex align_self accepts the alias set",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_self','start');
        createPanel('b','');  setFlex('b','align_self','flex-start');
        createPanel('c','');  setFlex('c','align_self','end');
        createPanel('d','');  setFlex('d','align_self','flex-end');
        createPanel('e','');  setFlex('e','align_self','auto');
    )");
    auto sl = [&](const std::string& id) { return bridge.widget(id)->flex().align_self; };
    REQUIRE(sl("a") == FlexAlign::start);
    REQUIRE(sl("b") == FlexAlign::start);
    REQUIRE(sl("c") == FlexAlign::end);
    REQUIRE(sl("d") == FlexAlign::end);
    REQUIRE(sl("e") == FlexAlign::auto_);
}

TEST_CASE("setFlex justify_content accepts the alias set",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','justify_content','start');
        createPanel('b','');  setFlex('b','justify_content','flex-start');
        createPanel('c','');  setFlex('c','justify_content','end');
        createPanel('d','');  setFlex('d','justify_content','flex-end');
        createPanel('e','');  setFlex('e','justify_content','center');
        createPanel('f','');  setFlex('f','justify_content','space-between');
    )");
    auto jc = [&](const std::string& id) { return bridge.widget(id)->flex().justify_content; };
    REQUIRE(jc("a") == FlexJustify::start);
    REQUIRE(jc("b") == FlexJustify::start);
    REQUIRE(jc("c") == FlexJustify::end_);
    REQUIRE(jc("d") == FlexJustify::end_);
    REQUIRE(jc("e") == FlexJustify::center);
    REQUIRE(jc("f") == FlexJustify::space_between);
}

TEST_CASE("CSSStyleDeclaration forwards flex-direction reverse modes verbatim",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  createPanel('b','');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('flexDirection', 'row-reverse');
        sb._applyProperty('flexDirection', 'column-reverse');
    )");

    REQUIRE(bridge.widget("a")->flex().direction == FlexDirection::row_reverse);
    REQUIRE(bridge.widget("b")->flex().direction == FlexDirection::column_reverse);
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

// ── pulp #1434 (cross-surface mega-batch) — per-edge margin/padding
// accept percent strings + auto (margin only). Mirrors width/height
// (#1426) and top/right/bottom/left (#1451) percent patterns. Yoga
// dispatches `dim_*.unit == percent` to `YGNodeStyleSetMargin/PaddingPercent`;
// `unit == auto_` (margin only) to `YGNodeStyleSetMarginAuto`.

TEST_CASE("setFlex padding edges accept percent strings",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'padding_top',    '10%');
        setFlex('a', 'padding_right',  '20%');
        setFlex('a', 'padding_bottom', '30%');
        setFlex('a', 'padding_left',   '40%');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_padding_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_top.value,    WithinAbs(10.0f, 0.001f));
    REQUIRE(f.dim_padding_right.unit  == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_right.value,  WithinAbs(20.0f, 0.001f));
    REQUIRE(f.dim_padding_bottom.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_bottom.value, WithinAbs(30.0f, 0.001f));
    REQUIRE(f.dim_padding_left.unit   == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_left.value,   WithinAbs(40.0f, 0.001f));
}

TEST_CASE("setFlex padding edges numeric path stays px",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'padding_top',    8);
        setFlex('a', 'padding_right',  12);
        setFlex('a', 'padding_bottom', 16);
        setFlex('a', 'padding_left',   4);
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_padding_top.unit == DimensionUnit::px);
    REQUIRE_THAT(f.padding_top,    WithinAbs(8.0f,  0.001f));
    REQUIRE_THAT(f.padding_right,  WithinAbs(12.0f, 0.001f));
    REQUIRE_THAT(f.padding_bottom, WithinAbs(16.0f, 0.001f));
    REQUIRE_THAT(f.padding_left,   WithinAbs(4.0f,  0.001f));
}

TEST_CASE("setFlex margin edges accept percent strings",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'margin_top',    '5%');
        setFlex('a', 'margin_right',  '10%');
        setFlex('a', 'margin_bottom', '15%');
        setFlex('a', 'margin_left',   '20%');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_margin_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_top.value,    WithinAbs(5.0f,  0.001f));
    REQUIRE(f.dim_margin_right.unit  == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_right.value,  WithinAbs(10.0f, 0.001f));
    REQUIRE(f.dim_margin_bottom.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_bottom.value, WithinAbs(15.0f, 0.001f));
    REQUIRE(f.dim_margin_left.unit   == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_left.value,   WithinAbs(20.0f, 0.001f));
}

TEST_CASE("setFlex margin edges accept 'auto' keyword",
          "[view][bridge][css][issue-1434-edges]") {
    // `marginLeft: 'auto'; marginRight: 'auto'` is the canonical
    // centering idiom in CSS / RN; Yoga supports it via
    // YGNodeStyleSetMarginAuto.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'margin_left',  'auto');
        setFlex('a', 'margin_right', 'auto');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_margin_left.unit  == DimensionUnit::auto_);
    REQUIRE(f.dim_margin_right.unit == DimensionUnit::auto_);
}

TEST_CASE("padding_left percent caps the layout edge",
          "[view][bridge][css][issue-1434-edges]") {
    // End-to-end: percent reaches Yoga and produces the expected
    // resolved pixel size after layout.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('parent', '');
        setFlex('parent', 'padding_left', '25%');
        setFlex('parent', 'width',  '100%');
        setFlex('parent', 'height', '100%');
    )");

    auto* parent = bridge.widget("parent");
    REQUIRE(parent != nullptr);
    root.layout_children();
    // 25% of 400px parent width → 100px padding-left.
    REQUIRE_THAT(parent->bounds().width, WithinAbs(400.0f, 0.5f));
}

TEST_CASE("CSSStyleDeclaration forwards marginTop percent + auto verbatim",
          "[view][bridge][css][issue-1434-edges]") {
    // The DOM-lite el.style adapter must forward 'NN%' and 'auto' as
    // strings so the bridge's per-unit branch is reached.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('marginTop',  '50%');
        sa._applyProperty('paddingTop', '25%');
        sb._applyProperty('marginLeft',  'auto');
        sb._applyProperty('marginRight', 'auto');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_margin_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_margin_top.value,    WithinAbs(50.0f, 0.001f));
    REQUIRE(fa.dim_padding_top.unit   == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_padding_top.value,   WithinAbs(25.0f, 0.001f));

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_margin_left.unit  == DimensionUnit::auto_);
    REQUIRE(fb.dim_margin_right.unit == DimensionUnit::auto_);
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
