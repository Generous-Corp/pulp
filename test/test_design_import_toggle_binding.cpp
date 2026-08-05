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

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

namespace {

/// One lowered switch on a panel, shaped exactly as the browser capture emits
/// it: an absolutely positioned frame carrying the audio widget kind, the
/// parameter under `pulpParamKey`, and the route id plus stable anchor the
/// native binder resolves the widget by.
DesignIR panel_with_one_switch(const std::string& param) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 240.0f;
    ir.root.style.height = 120.0f;

    IRNode control;
    control.type = "frame";
    control.name = "Sync";
    control.audio_widget = AudioWidgetType::toggle;
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
    const auto ir = panel_with_one_switch("sync");

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
    const auto ir = panel_with_one_switch("sync");

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
