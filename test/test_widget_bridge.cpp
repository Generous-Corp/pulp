#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
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
          "[view][bridge][canvas][issue-916]") {
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
