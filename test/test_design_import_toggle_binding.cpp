// An imported on/off drives its parameter — on BOTH consumers of a lowered
// control.
//
// A DesignIR control reaches a running plugin down two independent paths: the
// native materializer builds widgets and installs binder callbacks, and the
// script emitter writes a ui.js the widget bridge executes. A control wired on
// one and not the other is indistinguishable from a working one until someone
// flips it in the host that happens to use the other path, so both are proved
// here, in the same file, by the same assertion: click it and the STORE moved.
//
// Neither case asserts that a widget was constructed or that a binding was
// registered. Both of those are true of a switch that moves nothing.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <string>
#include <type_traits>
#include <vector>

using namespace pulp::view;

namespace {

/// One lowered control on a panel, shaped exactly as the browser capture emits
/// it: an absolutely positioned frame carrying the audio widget kind, the
/// parameter under `pulpParamKey`, and the route id plus stable anchor the
/// native binder resolves the widget by.
DesignIR panel_with_one_control(AudioWidgetType kind, const std::string& param) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 240.0f;
    ir.root.style.height = 120.0f;

    IRNode control;
    control.type = "frame";
    control.name = "Sync";
    control.audio_widget = kind;
    control.audio_min = 0.0f;
    control.audio_max = 1.0f;
    control.audio_default = 0.0f;
    control.style.position = "absolute";
    control.style.left = 20.0f;
    control.style.top = 20.0f;
    control.style.width = 56.0f;
    control.style.height = 28.0f;
    control.stable_anchor_id = "capture:" + param + ":0";
    control.anchor_strategy = "path";
    control.attributes["binding"] = param;
    control.attributes["pulpParamKey"] = param;
    control.attributes["pulpRouteId"] = "capture:" + param + ":0";
    control.attributes["designed_body"] = "underlay";
    ir.root.children.push_back(std::move(control));
    return ir;
}

/// The gesture each kind answers to. A Knob and a Fader take a drag along their
/// travel; a Toggle takes a press. Driving all three the same way would prove
/// nothing about two of them.
void drive(Knob& knob) {
    const auto box = knob.bounds();
    knob.simulate_drag({box.width * 0.5f, box.height - 4.0f},
                       {box.width * 0.5f, 4.0f});
}
void drive(Fader& fader) {
    const auto box = fader.bounds();
    fader.simulate_drag({box.width * 0.5f, box.height - 4.0f},
                        {box.width * 0.5f, 4.0f});
}
void drive(Toggle& toggle) {
    const auto box = toggle.bounds();
    toggle.on_mouse_down({box.width * 0.5f, box.height * 0.5f});
}

template <typename T>
T* find_widget(View& view) {
    if (auto* hit = dynamic_cast<T*>(&view)) return hit;
    for (std::size_t i = 0; i < view.child_count(); ++i)
        if (View* child = view.child_at(i))
            if (T* hit = find_widget<T>(*child)) return hit;
    return nullptr;
}

/// The binding context an embedding host writes: resolve the declared key
/// against this plugin's parameters and install the write-back.
class StoreBindingContext final : public NativeImportBindingContext {
public:
    explicit StoreBindingContext(pulp::state::StateStore& store) : store_(store) {}

    void bind_stepper(Stepper& stepper,
                      const NativeImportStepperBindingDescriptor& d) override {
        const auto id = id_for(d.param_key);
        if (id == 0) return;
        const double min = d.min, max = d.max, step = d.step;
        stepper.set_value(stepper_plain_value(store_.get_value(id), min, max, step));
        stepper.on_change = [this, id, min, max](double plain) {
            store_.set_value(id, stepper_normalized_value(plain, min, max));
        };
    }

    void bind_segmented(SegmentedControl& segmented,
                        const NativeImportSegmentedBindingDescriptor& d) override {
        const auto id = id_for(d.param_key);
        if (id == 0) return;
        const int count = d.segment_count;
        segmented.set_selected_silent(
            selector_segment_index(store_.get_value(id), count));
        segmented.on_change = [this, id, count](int index) {
            store_.set_value(id, selector_segment_value(index, count));
        };
    }

    void bind_toggle_button(ToggleButton& button,
                            const NativeImportBindingDescriptor& d) override {
        const auto id = id_for(d.param_key);
        if (id == 0) return;
        button.set_on(store_.get_value(id) >= 0.5f);
        button.on_toggle = [this, id](bool on) {
            store_.set_value(id, on ? 1.0f : 0.0f);
        };
    }

private:
    pulp::state::ParamID id_for(std::string_view key) const {
        return key == "sync" ? 1 : 0;
    }
    pulp::state::StateStore& store_;
};

void add_sync_param(pulp::state::StateStore& store) {
    store.add_parameter({.id = 1,
                         .name = "sync",
                         .unit = "",
                         .range = {.min = 0.0f, .max = 1.0f}});
}

}  // namespace

TEST_CASE("a lowered switch moves its parameter through the native materializer",
          "[view][import][native-materializer][binding][toggle]") {
    const auto ir = panel_with_one_control(AudioWidgetType::toggle, "sync");

    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root->layout_children();

    auto* button = find_widget<ToggleButton>(*root);
    REQUIRE(button != nullptr);

    pulp::state::StateStore store;
    add_sync_param(store);
    StoreBindingContext ctx{store};
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &diagnostics});
    for (const auto& d : diagnostics) INFO(d.code << ": " << d.message);
    CHECK(diagnostics.empty());

    const float before = store.get_value(1);
    const auto box = button->bounds();
    button->on_mouse_down({box.width * 0.5f, box.height * 0.5f});
    INFO("sync " << before << " -> " << store.get_value(1));
    CHECK(store.get_value(1) != before);
}

TEST_CASE("a lowered switch moves its parameter through the emitted script",
          "[view][import][codegen][binding][toggle]") {
    const auto ir = panel_with_one_control(AudioWidgetType::toggle, "sync");

    // web_compat is the emitter that installs parameter bindings; the
    // bridge_native_js lane prints a control's binding as a caption and wires
    // nothing, for every widget kind including a knob.
    CodeGenOptions options;
    options.mode = CodeGenMode::web_compat;
    options.include_comments = false;
    const auto js = generate_pulp_js(ir, options);

    ScriptEngine engine;
    View root;
    root.set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root.set_theme(Theme::dark());
    pulp::state::StateStore store;
    add_sync_param(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(js);

    root.layout_children();
    auto* toggle = find_widget<Toggle>(root);
    INFO("emitted script:\n" << js);
    REQUIRE(toggle != nullptr);

    // The user's own path: the mouse handler is what fires `on_toggle`, which
    // is what the emitted listener is attached to. Calling set_on() instead
    // would prove the widget can change its own mind and nothing about whether
    // the script is listening.
    const float before = store.get_value(1);
    const auto box = toggle->bounds();
    toggle->on_mouse_down({box.width * 0.5f, box.height * 0.5f});
    INFO("sync " << before << " -> " << store.get_value(1));
    CHECK(store.get_value(1) != before);
}

// The DEFAULT script emitter, which is the one `pulp import-design --emit js`
// produces without `--web-compat`.
//
// Both `bindWidgetToParam` emission sites live in generate_node(), which only
// the web-compat arm of generate_pulp_js() reaches. The native-bridge arm
// creates the widget, sizes it, labels it, and prints the control's binding
// name as a grey caption underneath — so a knob imported through the
// documented default renders, turns under the mouse, and moves no parameter.
// It is the fader defect and the toggle defect again at the scale of an
// entire emitter, and nothing downstream can see it: the script is well
// formed, the widget is real, and the caption even says which parameter it is
// supposed to be driving.
TEMPLATE_TEST_CASE("every drivable kind moves its parameter through the default "
                   "script emitter",
                   "[view][import][codegen][binding]", Knob, Fader, Toggle) {
    const auto kind = std::is_same_v<TestType, Knob>    ? AudioWidgetType::knob
                      : std::is_same_v<TestType, Fader> ? AudioWidgetType::fader
                                                        : AudioWidgetType::toggle;
    const auto ir = panel_with_one_control(kind, "sync");

    CodeGenOptions options;
    options.mode = CodeGenMode::bridge_native_js;  // the CLI default
    options.include_comments = false;
    const auto js = generate_pulp_js(ir, options);

    ScriptEngine engine;
    View root;
    root.set_bounds({0.0f, 0.0f, 240.0f, 160.0f});
    root.set_theme(Theme::dark());
    pulp::state::StateStore store;
    add_sync_param(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(js);
    root.layout_children();

    auto* control = find_widget<TestType>(root);
    INFO("emitted script:\n" << js);
    REQUIRE(control != nullptr);

    const float before = store.get_value(1);
    drive(*control);
    INFO("sync " << before << " -> " << store.get_value(1));
    CHECK(store.get_value(1) != before);
}

namespace {

/// A four-way selector, the shape lattice's DIRECTION control has: one track,
/// four labelled segments, exactly one lit.
DesignIR panel_with_a_selector(const std::string& param) {
    auto ir = panel_with_one_control(AudioWidgetType::selector, param);
    auto& control = ir.root.children.front();
    control.style.width = 200.0f;
    control.style.height = 28.0f;
    control.attributes["pulpChoices"] = "Up|Down|Converge|Random";
    return ir;
}

/// Click the centre of segment `index` of `count` along the control's track.
void click_segment(View& control, int index, int count) {
    const auto box = control.bounds();
    const float width = box.width / static_cast<float>(count);
    MouseEvent event;
    event.position = {width * (static_cast<float>(index) + 0.5f), box.height * 0.5f};
    event.is_down = true;
    control.on_mouse_event(event);
}

}  // namespace

TEST_CASE("a selector's segments are one group, and each writes its own value",
          "[view][import][native-materializer][binding][selector]") {
    const auto ir = panel_with_a_selector("sync");

    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root->layout_children();

    auto* segmented = find_widget<SegmentedControl>(*root);
    REQUIRE(segmented != nullptr);
    // ONE control carrying four segments, not four controls that happen to
    // touch. A row of independent toggles can light two at once; this cannot.
    REQUIRE(segmented->segments().size() == 4);

    pulp::state::StateStore store;
    add_sync_param(store);
    StoreBindingContext ctx{store};
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &diagnostics});
    for (const auto& d : diagnostics) INFO(d.code << ": " << d.message);
    CHECK(diagnostics.empty());

    // Each segment writes ITS value, not merely "something changed".
    click_segment(*segmented, 3, 4);
    CHECK(store.get_value(1) == 1.0f);
    CHECK(segmented->selected() == 3);

    click_segment(*segmented, 1, 4);
    CHECK(store.get_value(1) == selector_segment_value(1, 4));
    // And picking a sibling RELEASES the first — the property that makes this
    // one choice rather than four independent states.
    CHECK(segmented->selected() == 1);
}

TEST_CASE("a selector drives its parameter through both script emitters",
          "[view][import][codegen][binding][selector]") {
    const auto ir = panel_with_a_selector("sync");

    for (const auto mode : {CodeGenMode::web_compat, CodeGenMode::bridge_native_js}) {
        CodeGenOptions options;
        options.mode = mode;
        options.include_comments = false;
        const auto js = generate_pulp_js(ir, options);

        ScriptEngine engine;
        View root;
        root.set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
        root.set_theme(Theme::dark());
        pulp::state::StateStore store;
        add_sync_param(store);
        WidgetBridge bridge(engine, root, store);
        bridge.load_script(js);
        root.layout_children();

        INFO("mode=" << (mode == CodeGenMode::web_compat ? "web_compat" : "bridge_native_js"));
        INFO("children=" << root.child_count());
        auto* segmented = find_widget<SegmentedControl>(root);
        INFO("emitted script:\n" << js);
        REQUIRE(segmented != nullptr);
        REQUIRE(segmented->segments().size() == 4);

        click_segment(*segmented, 3, 4);
        INFO("after segment 3: " << store.get_value(1));
        CHECK(store.get_value(1) == 1.0f);

        click_segment(*segmented, 1, 4);
        INFO("after segment 1: " << store.get_value(1));
        CHECK(store.get_value(1) == selector_segment_value(1, 4));
        CHECK(segmented->selected() == 1);
    }
}

namespace {

/// A voice-count stepper: 1..8 on a grid of 1, which is the control the
/// original request asked for and the one a knob reads worst.
DesignIR panel_with_a_stepper(const std::string& param) {
    auto ir = panel_with_one_control(AudioWidgetType::stepper, param);
    auto& control = ir.root.children.front();
    control.style.width = 80.0f;
    control.style.height = 28.0f;
    control.audio_min = 1.0f;
    control.audio_max = 8.0f;
    control.has_audio_range = true;
    control.attributes["pulpStep"] = "1";
    return ir;
}

}  // namespace

TEST_CASE("a stepper writes the parameter behind the number it shows",
          "[view][import][native-materializer][binding][stepper]") {
    const auto ir = panel_with_a_stepper("sync");

    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root->layout_children();

    auto* stepper = find_widget<Stepper>(*root);
    REQUIRE(stepper != nullptr);
    REQUIRE(stepper->minimum() == 1.0);
    REQUIRE(stepper->maximum() == 8.0);

    pulp::state::StateStore store;
    add_sync_param(store);
    StoreBindingContext ctx{store};
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &diagnostics});
    for (const auto& d : diagnostics) INFO(d.code << ": " << d.message);
    CHECK(diagnostics.empty());

    // The widget shows a COUNT and the parameter behind it is normalized, so
    // the assertion is on the mapping, not merely on movement: 8 voices is the
    // top of the range, 1 voice is the bottom.
    stepper->set_value(8.0);
    CHECK(store.get_value(1) == 1.0f);
    stepper->set_value(1.0);
    CHECK(store.get_value(1) == 0.0f);
    stepper->set_value(5.0);
    CHECK(store.get_value(1) == stepper_normalized_value(5.0, 1.0, 8.0));
}

TEST_CASE("a stepper drives its parameter through both script emitters",
          "[view][import][codegen][binding][stepper]") {
    const auto ir = panel_with_a_stepper("sync");

    for (const auto mode : {CodeGenMode::web_compat, CodeGenMode::bridge_native_js}) {
        CodeGenOptions options;
        options.mode = mode;
        options.include_comments = false;
        const auto js = generate_pulp_js(ir, options);

        ScriptEngine engine;
        View root;
        root.set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
        root.set_theme(Theme::dark());
        pulp::state::StateStore store;
        add_sync_param(store);
        WidgetBridge bridge(engine, root, store);
        bridge.load_script(js);
        root.layout_children();

        INFO("mode=" << (mode == CodeGenMode::web_compat ? "web_compat"
                                                         : "bridge_native_js"));
        INFO("emitted script:\n" << js);
        auto* stepper = find_widget<Stepper>(root);
        REQUIRE(stepper != nullptr);
        // The declared range has to reach the widget, or the number it shows is
        // off its own default grid rather than the patch's.
        REQUIRE(stepper->minimum() == 1.0);
        REQUIRE(stepper->maximum() == 8.0);

        stepper->set_value(8.0);
        INFO("after 8 voices: " << store.get_value(1));
        CHECK(store.get_value(1) == 1.0f);
        stepper->set_value(5.0);
        CHECK(store.get_value(1) == stepper_normalized_value(5.0, 1.0, 8.0));
    }
}
