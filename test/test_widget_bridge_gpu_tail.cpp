#include "widget_bridge_test_support.hpp"
#include "support/unique_temp_dir.hpp"

template <typename Body>
static std::string capture_widget_bridge_stderr(Body&& body) {
    fflush(stderr);
    int saved_fd = pulp_test_dup_fd(pulp_test_fileno(stderr));
    REQUIRE(saved_fd >= 0);
    PulpTestFdGuard saved_fd_guard(saved_fd);
    PulpTestFileGuard tmp_file(std::tmpfile());
    REQUIRE(tmp_file.file != nullptr);
    REQUIRE(pulp_test_dup2_fd(pulp_test_fileno(tmp_file.file), pulp_test_fileno(stderr)) >= 0);
    try { body(); } catch (...) { /* swallow — body asserts on throw */ }
    fflush(stderr);
    REQUIRE(pulp_test_dup2_fd(saved_fd_guard.fd, pulp_test_fileno(stderr)) >= 0);
    std::fflush(tmp_file.file);
    std::fseek(tmp_file.file, 0, SEEK_SET);
    std::stringstream ss;
    char buffer[4096];
    while (std::size_t n = std::fread(buffer, 1, sizeof(buffer), tmp_file.file)) {
        ss.write(buffer, static_cast<std::streamsize>(n));
    }
    return ss.str();
}

TEST_CASE("WidgetBridge teardown reports a quarantine repaint failure without terminating",
          "[view][bridge][lifetime][quarantine]") {
    class ThrowingRepaintHost final : public WindowHost {
    public:
        void show() override {}
        void hide() override {}
        bool is_visible() const override { return true; }
        void repaint() override {
            ++repaint_calls;
            throw std::runtime_error("test repaint failure");
        }
        void set_close_callback(std::function<void()>) override {}
        void run_event_loop() override {}

        int repaint_calls = 0;
    } host;

    ScriptEngine engine;
    View root;
    StateStore store;
    root.set_window_host(&host);
    root.set_visible(true);

    const std::string captured = capture_widget_bridge_stderr([&] {
        auto bridge = std::make_unique<WidgetBridge>(engine, root, store);
        bridge->quarantine_realm();
        CHECK_FALSE(root.visible());
        bridge.reset();
    });

    CHECK(root.visible());
    CHECK(host.repaint_calls == 2);
    CHECK(captured.find("WidgetBridge quarantine repaint error: test repaint failure") !=
          std::string::npos);
}

TEST_CASE("WidgetBridge eval_or_throw logs PULP_EVAL_THROW before rethrowing (#3206)",
          "[view][bridge][issue-3206]") {
    pulp::view::ScriptEngine engine;
    pulp::view::View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    pulp::view::WidgetBridge bridge(engine, root, store);

    // load_script wraps user code in `;void 0` and routes through
    // eval_or_throw with name="user_script". A reference to an undeclared
    // identifier triggers a runtime ReferenceError that bubbles up through
    // one of the three catch branches and the log line should fire before
    // the rethrow.
    std::string captured = capture_widget_bridge_stderr([&]() {
        try {
            bridge.load_script("__definitely_not_defined_3206__.crash_here()");
        } catch (...) {
            // expected — the rethrow is fine, we only care about the log
            // line that fires before it.
        }
    });

    REQUIRE(captured.find("PULP_EVAL_THROW:") != std::string::npos);
    REQUIRE(captured.find("name=user_script") != std::string::npos);
}

TEST_CASE("WidgetBridge creates Ink & Signal design-system widgets from JS",
          "[view][bridge][design-system]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // createBadge(id, text, tone, parentId); createStepper/createPan(id, parentId).
    bridge.load_script(
        "createBadge('fmt', 'VST3', 'info', '');"
        "createStepper('voices', '');"
        "createPan('balance', '');");

    REQUIRE(root.child_count() == 3);

    auto* badge = dynamic_cast<Badge*>(bridge.widget("fmt"));
    REQUIRE(badge != nullptr);
    REQUIRE(badge->text() == "VST3");
    REQUIRE(badge->tone() == Tone::info);

    auto* stepper = dynamic_cast<Stepper*>(bridge.widget("voices"));
    REQUIRE(stepper != nullptr);
    auto* pan = dynamic_cast<PanControl*>(bridge.widget("balance"));
    REQUIRE(pan != nullptr);

    // setValue routes through the shared dynamic_cast chain to the gap widgets.
    bridge.load_script("setValue('voices', 8); setValue('balance', -1); setText('fmt', 'CLAP');");
    REQUIRE(stepper->value() == Catch::Approx(8.0));
    REQUIRE(engine.evaluate("getValue('voices')").getWithDefault<double>(0.0) ==
            Catch::Approx(8.0));
    REQUIRE(pan->value() == Catch::Approx(-1.0f));
    REQUIRE(badge->text() == "CLAP");
}

TEST_CASE("WidgetBridge discrete factories and DOM tags share gesture-safe callbacks",
          "[view][bridge][design-system][discrete][gesture][lifetime]") {
    struct Variant { const char* name; const char* create; int kind; };
    for (const auto variant : {
             Variant{"factory stepper", "createStepper('control', '');", 0},
             Variant{"DOM stepper", "__domAppend('', 'control', 'stepper');", 0},
             Variant{"factory segmented", "createSegmented('control', ''); setSegments('control', ['A','B','C']);", 1},
             Variant{"DOM segmented", "__domAppend('', 'control', 'segmented'); setSegments('control', ['A','B','C']);", 1},
             Variant{"factory combo", "createCombo('control', ''); setItems('control', ['A','B','C']);", 2},
             Variant{"DOM combo", "__domAppend('', 'control', 'select'); setItems('control', ['A','B','C']);", 2}}) {
        DYNAMIC_SECTION(variant.name) {
            ScriptEngine engine;
            View root;
            StateStore store;
            store.add_parameter({
                .id = 1,
                .name = "choice",
                .range = {0.0f, 1.0f, 0.0f, 0.0f},
            });
            int begins = 0;
            int ends = 0;
            store.set_gesture_callbacks(
                [&](pulp::state::ParamID) { ++begins; },
                [&](pulp::state::ParamID) { ++ends; });
            WidgetBridge bridge(engine, root, store);
            bridge.load_script(std::string(variant.create) +
                               "bindWidgetToParam('control', 'choice');");

            if (variant.kind == 1) {
                auto* control = dynamic_cast<SegmentedControl*>(bridge.widget("control"));
                REQUIRE(control != nullptr);
                REQUIRE(control->on_change);
                control->on_change(1);
            } else if (variant.kind == 2) {
                auto* control = dynamic_cast<ComboBox*>(bridge.widget("control"));
                REQUIRE(control != nullptr);
                REQUIRE(control->on_change);
                control->on_change(1);
            } else {
                auto* control = dynamic_cast<Stepper*>(bridge.widget("control"));
                REQUIRE(control != nullptr);
                REQUIRE(control->on_change);
                control->on_change(0.5);
            }
            REQUIRE(begins == 1);
            REQUIRE(ends == 1);
            REQUIRE(store.open_gesture_count() == 0);
        }
    }

    // A JS handler may tear down the bridge during dispatch. The callback must
    // not touch its destroyed `this` while closing the instantaneous gesture.
    for (const auto variant : {
             Variant{"teardown factory stepper", "createStepper('control', '');", 0},
             Variant{"teardown DOM stepper", "__domAppend('', 'control', 'stepper');", 0},
             Variant{"teardown factory segmented", "createSegmented('control', '');", 1},
             Variant{"teardown DOM segmented", "__domAppend('', 'control', 'segmented');", 1},
             Variant{"teardown factory combo", "createCombo('control', '');", 2},
             Variant{"teardown DOM combo", "__domAppend('', 'control', 'select');", 2}}) {
        DYNAMIC_SECTION(variant.name) {
            ScriptEngine engine;
            View root;
            StateStore store;
            std::unique_ptr<WidgetBridge> bridge;
            engine.register_function("__destroyDiscreteBridge",
                [&](const choc::value::Value*, size_t) {
                    bridge.reset();
                    return choc::value::createInt32(1);
                });
            bridge = std::make_unique<WidgetBridge>(engine, root, store);
            bridge->load_script(std::string(variant.create) +
                "on('control', '" + (variant.kind == 0 ? "change" : "select") +
                "', function() { __destroyDiscreteBridge(); });");

            if (variant.kind == 1) {
                auto callback = dynamic_cast<SegmentedControl*>(
                    bridge->widget("control"))->on_change;
                REQUIRE_NOTHROW(callback(1));
            } else if (variant.kind == 2) {
                auto callback = dynamic_cast<ComboBox*>(
                    bridge->widget("control"))->on_change;
                REQUIRE_NOTHROW(callback(1));
            } else {
                auto callback = dynamic_cast<Stepper*>(
                    bridge->widget("control"))->on_change;
                REQUIRE_NOTHROW(callback(0.5));
            }
            REQUIRE(bridge == nullptr);
        }
    }

    // A scheduler or caller may retain a copied widget callback past bridge
    // teardown. The alive token must be checked before the callback's first
    // use of its captured raw `this`, not merely after JS dispatch returns.
    SECTION("retained stepper callback invoked after bridge destruction") {
        ScriptEngine engine;
        View root;
        StateStore store;
        store.add_parameter({.id = 1, .name = "choice",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        int begins = 0;
        int ends = 0;
        store.set_gesture_callbacks([&](auto) { ++begins; },
                                    [&](auto) { ++ends; });
        auto bridge = std::make_unique<WidgetBridge>(engine, root, store);
        bridge->load_script(
            "globalThis.retainedEvents = 0;"
            "createStepper('control', '');"
            "on('control', 'change', function(){ ++globalThis.retainedEvents; });"
            "bindWidgetToParam('control', 'choice');");
        auto callback = dynamic_cast<Stepper*>(bridge->widget("control"))->on_change;
        REQUIRE(callback);
        bridge.reset();
        REQUIRE_NOTHROW(callback(0.5));
        CHECK(begins == 0);
        CHECK(ends == 0);
        CHECK(engine.evaluate("globalThis.retainedEvents")
                  .getWithDefault<int32_t>(-1) == 0);
    }

    SECTION("retained segmented callback invoked after bridge destruction") {
        ScriptEngine engine;
        View root;
        StateStore store;
        store.add_parameter({.id = 1, .name = "choice",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        int begins = 0;
        int ends = 0;
        store.set_gesture_callbacks([&](auto) { ++begins; },
                                    [&](auto) { ++ends; });
        auto bridge = std::make_unique<WidgetBridge>(engine, root, store);
        bridge->load_script(
            "globalThis.retainedEvents = 0;"
            "createSegmented('control', '');"
            "setSegments('control', ['A', 'B']);"
            "on('control', 'select', function(){ ++globalThis.retainedEvents; });"
            "bindWidgetToParam('control', 'choice');");
        auto callback =
            dynamic_cast<SegmentedControl*>(bridge->widget("control"))->on_change;
        REQUIRE(callback);
        bridge.reset();
        REQUIRE_NOTHROW(callback(1));
        CHECK(begins == 0);
        CHECK(ends == 0);
        CHECK(engine.evaluate("globalThis.retainedEvents")
                  .getWithDefault<int32_t>(-1) == 0);
    }

    SECTION("retained toggle callback invoked after bridge destruction") {
        ScriptEngine engine;
        View root;
        StateStore store;
        store.add_parameter({.id = 1, .name = "choice",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        int begins = 0;
        int ends = 0;
        store.set_gesture_callbacks([&](auto) { ++begins; },
                                    [&](auto) { ++ends; });
        auto bridge = std::make_unique<WidgetBridge>(engine, root, store);
        bridge->load_script(
            "globalThis.retainedEvents = 0;"
            "createToggle('control', false);"
            "on('control', 'toggle', function(){ ++globalThis.retainedEvents; });"
            "bindWidgetToParam('control', 'choice');");
        auto callback = dynamic_cast<Toggle*>(bridge->widget("control"))->on_toggle;
        REQUIRE(callback);
        bridge.reset();
        REQUIRE_NOTHROW(callback(true));
        CHECK(begins == 0);
        CHECK(ends == 0);
        CHECK(engine.evaluate("globalThis.retainedEvents")
                  .getWithDefault<int32_t>(-1) == 0);
    }
}

TEST_CASE("WidgetBridge exposes designed overlays for every scripted discrete control",
          "[view][bridge][design-system][discrete][overlay]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createToggle('toggle', ''); setDesignedOverlay('toggle', true);"
        "createStepper('stepper', ''); setDesignedOverlay('stepper', true);"
        "createSegmented('selector', ''); setDesignedOverlay('selector', true);");

    auto* toggle = dynamic_cast<Toggle*>(bridge.widget("toggle"));
    auto* stepper = dynamic_cast<Stepper*>(bridge.widget("stepper"));
    auto* selector = dynamic_cast<SegmentedControl*>(bridge.widget("selector"));
    REQUIRE(toggle != nullptr);
    REQUIRE(stepper != nullptr);
    REQUIRE(selector != nullptr);
    CHECK(toggle->designed_overlay());
    CHECK(stepper->designed_overlay());
    CHECK(selector->designed_overlay());
}

TEST_CASE("WidgetBridge range updates silently clamp Stepper state",
          "[view][bridge][design-system][stepper][range]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createStepper('stepper', '');"
        "setValue('stepper', 8);"
        "setMax('stepper', 4);");

    auto* stepper = dynamic_cast<Stepper*>(bridge.widget("stepper"));
    REQUIRE(stepper != nullptr);
    CHECK(stepper->maximum() == 4.0);
    CHECK(stepper->value() == 4.0);
}

TEST_CASE("WidgetBridge design-system stepper/pan dispatch change events",
          "[view][bridge][design-system]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(
        "globalThis.changes = [];"
        "globalThis.__dispatch__ = function(id, ev, v) { changes.push({id:id, ev:ev, v:v}); };"
        "createStepper('voices', '');");

    auto* stepper = dynamic_cast<Stepper*>(bridge.widget("voices"));
    REQUIRE(stepper != nullptr);
    stepper->set_value(3);            // fires on_change → __dispatch__
    engine.pump_message_loop();
    REQUIRE(engine.evaluate("changes.length").getWithDefault<double>(0) >= 1);
    REQUIRE(engine.evaluate("changes[changes.length-1].ev").getWithDefault<std::string>("") == "change");
}

// ── Custom widget-declared reload state (live-swap item 1.4b) ─────────────────
namespace {
// A custom widget that carries its own state across a scripted-UI reload by
// opting into the View reload-state hook.
class CustomStateWidget : public pulp::view::View {
public:
    int state = 0;
    bool save_reload_state(std::string& out) const override {
        out = "state=" + std::to_string(state);
        return true;
    }
    bool restore_reload_state(std::string_view blob) override {
        constexpr std::string_view prefix = "state=";
        if (blob.substr(0, prefix.size()) != prefix) return false;
        state = std::atoi(std::string(blob.substr(prefix.size())).c_str());
        return true;
    }
};
}  // namespace

TEST_CASE("View reload-state hook round-trips custom widget state (item 1.4b)",
          "[view][reload][1.4b]") {
    CustomStateWidget w;
    w.state = 42;
    std::string blob;
    REQUIRE(w.save_reload_state(blob));
    REQUIRE(blob == "state=42");

    CustomStateWidget restored;
    REQUIRE(restored.restore_reload_state(blob));
    REQUIRE(restored.state == 42);

    // A plain View opts OUT by default — existing widgets are unaffected.
    pulp::view::View plain;
    std::string unused;
    REQUIRE_FALSE(plain.save_reload_state(unused));
    REQUIRE_FALSE(plain.restore_reload_state("state=1"));
}

TEST_CASE("WidgetBridge reload snapshot leaves custom_state empty for built-ins (item 1.4b)",
          "[view][bridge][reload][1.4b]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createKnob('k', 0, 0, 48, 48);");
    REQUIRE(bridge.widget("k") != nullptr);

    WidgetReloadSnapshot snap;
    bridge.snapshot_values(snap);
    REQUIRE(snap.scalar_values.count("k") == 1);   // built-in still snapshotted by type
    REQUIRE(snap.custom_state.empty());            // built-in Knob opts out of custom state
}

// ── Canonical scalar-value access (widget_bridge/value_widget_access.hpp) ─────
//
// try_get_scalar_value / try_set_scalar_value are the single ladder that knows
// which widgets carry a float and how to read/write it. Both snapshot_values
// overloads and both restore_values overloads route through it, so the per-type
// contract is pinned here through the public surface: whatever the snapshot
// captures must restore to the same value, for every type in the ladder.
TEST_CASE("WidgetBridge snapshot/restore round-trips every scalar value widget",
          "[view][bridge][hot-reload][snapshot][parity]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    auto knob = std::make_unique<Knob>();
    knob->set_id("knob");
    knob->set_value(0.25f);
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    auto fader = std::make_unique<Fader>();
    fader->set_id("fader");
    fader->set_value(0.75f);
    auto* fader_ptr = fader.get();
    root.add_child(std::move(fader));

    auto range = std::make_unique<RangeSlider>();
    range->set_id("range");
    range->set_value(0.5f);
    auto* range_ptr = range.get();
    root.add_child(std::move(range));

    auto toggle = std::make_unique<Toggle>();
    toggle->set_id("toggle");
    toggle->set_on(true);
    auto* toggle_ptr = toggle.get();
    root.add_child(std::move(toggle));

    auto checkbox = std::make_unique<Checkbox>();
    checkbox->set_id("checkbox");
    checkbox->set_checked(true);
    auto* checkbox_ptr = checkbox.get();
    root.add_child(std::move(checkbox));

    auto toggle_button = std::make_unique<ToggleButton>();
    toggle_button->set_id("toggle-button");
    toggle_button->set_on(true);
    auto* toggle_button_ptr = toggle_button.get();
    root.add_child(std::move(toggle_button));

    WidgetBridge bridge(engine, root, store);
    // widget() is the lookup that adopts a natively-created view into the id map
    // that snapshot_values / restore_values iterate.
    for (const char* id : {"knob", "fader", "range", "toggle", "checkbox", "toggle-button"})
        REQUIRE(bridge.widget(id) != nullptr);

    std::unordered_map<std::string, float> snap;
    bridge.snapshot_values(snap);

    // Every ladder type contributes; booleans report 1.0f / 0.0f.
    REQUIRE(snap.at("knob") == Catch::Approx(0.25f));
    REQUIRE(snap.at("fader") == Catch::Approx(0.75f));
    REQUIRE(snap.at("range") == Catch::Approx(0.5f));
    REQUIRE(snap.at("toggle") == Catch::Approx(1.0f));
    REQUIRE(snap.at("checkbox") == Catch::Approx(1.0f));
    REQUIRE(snap.at("toggle-button") == Catch::Approx(1.0f));

    // Move every widget off its captured value, then restore.
    knob_ptr->set_value(0.9f);
    fader_ptr->set_value(0.1f);
    range_ptr->set_value(0.2f);
    toggle_ptr->set_on(false);
    checkbox_ptr->set_checked(false);
    toggle_button_ptr->set_on(false);

    bridge.restore_values(snap);

    REQUIRE(knob_ptr->value() == Catch::Approx(0.25f));
    REQUIRE(fader_ptr->value() == Catch::Approx(0.75f));
    REQUIRE(range_ptr->value() == Catch::Approx(0.5f));
    REQUIRE(toggle_ptr->is_on());
    REQUIRE(checkbox_ptr->is_checked());
    REQUIRE(toggle_button_ptr->is_on());

    // The canonical boolean threshold is inclusive: exactly 0.5 is on in the
    // importer, bridge APIs, host binding, and reload restore path.
    std::unordered_map<std::string, float> off;
    off["toggle"] = 0.5f;
    off["checkbox"] = 0.0f;
    off["toggle-button"] = 0.5f;
    bridge.restore_values(off);
    REQUIRE(toggle_ptr->is_on());
    REQUIRE_FALSE(checkbox_ptr->is_checked());
    REQUIRE(toggle_button_ptr->is_on());
}

// The reload snapshot layers selection controls and XYPad ON TOP of the shared
// scalar ladder: they are reached only when the scalar ladder declines the view.
// Pin that precedence — a widget that carries a scalar must never be captured as
// a selection index, and vice versa.
TEST_CASE("WidgetBridge reload snapshot layers selection + XY on the scalar ladder",
          "[view][bridge][hot-reload][snapshot][parity]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    auto knob = std::make_unique<Knob>();
    knob->set_id("knob");
    knob->set_value(0.25f);
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    auto combo = std::make_unique<ComboBox>();
    combo->set_id("combo");
    combo->set_items({"a", "b", "c"});
    combo->set_selected_silent(2);
    auto* combo_ptr = combo.get();
    root.add_child(std::move(combo));

    auto xy = std::make_unique<XYPad>();
    xy->set_id("xy");
    xy->set_x(0.3f);
    xy->set_y(0.7f);
    auto* xy_ptr = xy.get();
    root.add_child(std::move(xy));

    WidgetBridge bridge(engine, root, store);
    for (const char* id : {"knob", "combo", "xy"})
        REQUIRE(bridge.widget(id) != nullptr);

    WidgetReloadSnapshot snap;
    bridge.snapshot_values(snap);

    REQUIRE(snap.scalar_values.at("knob") == Catch::Approx(0.25f));
    // The selection index rides in scalar_values as an index-as-float.
    REQUIRE(snap.scalar_values.at("combo") == Catch::Approx(2.0f));
    // XYPad carries no scalar, so it lands in the XY channel only.
    REQUIRE(snap.scalar_values.count("xy") == 0);
    REQUIRE(snap.xy_values.at("xy").x == Catch::Approx(0.3f));
    REQUIRE(snap.xy_values.at("xy").y == Catch::Approx(0.7f));

    knob_ptr->set_value(0.9f);
    combo_ptr->set_selected_silent(0);
    xy_ptr->set_x(0.0f);
    xy_ptr->set_y(0.0f);

    bridge.restore_values(snap);

    REQUIRE(knob_ptr->value() == Catch::Approx(0.25f));
    REQUIRE(combo_ptr->selected() == 2);
    REQUIRE(xy_ptr->x_value() == Catch::Approx(0.3f));
    REQUIRE(xy_ptr->y_value() == Catch::Approx(0.7f));
}

// ── Native-event registration guards (WidgetBridge::registrations_) ───────────
//
// Registration state is one record per widget id covering every native channel
// (pointer / wheel / gesture), so tearing a subtree down forgets all of them in
// a single erase. The guard exists to stop a re-rendering reconciler stacking N
// lambdas on one widget (covered by the spectr idempotence tests); the contract
// pinned here is the OTHER half — the guard must not outlive the widget, or a
// recycled id silently wires nothing and the new widget is inert.
TEST_CASE("WidgetBridge re-wires a recycled widget id after its subtree is forgotten",
          "[view][bridge][registration]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Plain containers, not value widgets, exercise the registrar's DOM-origin
    // callback rather than a widget's own mouse handling.
    bridge.load_script(R"(
        globalThis.hits = 0;
        createRow('box');
        createRow('k', 'box');
        on('k', 'pointerdown', function() { globalThis.hits++; });
    )");

    MouseEvent down{};
    down.button = MouseButton::left;
    down.is_down = true;

    auto* first = bridge.widget("k");
    REQUIRE(first != nullptr);
    REQUIRE(first->on_dom_pointer_event);
    first->on_dom_pointer_event(down, true);
    REQUIRE(engine.evaluate("String(globalThis.hits)").toString() == "1");

    // Tearing the subtree down must forget every registration channel for 'k',
    // not just its entry in the widget id map.
    bridge.load_script("removeWidget('box'); void 0;");
    REQUIRE(bridge.widget("k") == nullptr);

    // A fresh widget recycling the same id wires again. Were the pointer guard
    // to survive the teardown, registerPointer would no-op here and this widget
    // would never see a pointer event.
    bridge.load_script(R"(
        createRow('box2');
        createRow('k', 'box2');
        on('k', 'pointerdown', function() { globalThis.hits++; });
    )");

    auto* second = bridge.widget("k");
    REQUIRE(second != nullptr);
    REQUIRE(second->on_dom_pointer_event);
    second->on_dom_pointer_event(down, true);
    REQUIRE(engine.evaluate("String(globalThis.hits)").toString() == "2");
}

TEST_CASE("WidgetBridge resolves script-relative asset paths against the script base dir",
          "[view][bridge][assets]") {
    namespace fs = std::filesystem;

    // A self-contained import artifact references its images as
    // `assets/<file>` next to the ui.js; hosts publish the script's directory
    // via set_script_base_dir so those references resolve regardless of the
    // process CWD.
    const auto base = pulp::test::make_unique_temp_dir("pulp-bridge-script-base");
    fs::create_directories(base / "assets");
    { std::ofstream f(base / "assets" / "hero.png", std::ios::binary); f << "png"; }
    const auto reviewed_root = base / "reviewed";
    const auto scratch_root = base / "scratch";
    fs::create_directories(reviewed_root / "assets");
    fs::create_directories(scratch_root);
    { std::ofstream f(reviewed_root / "assets" / "captured.png", std::ios::binary);
      f << "captured"; }

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 100, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    SECTION("persisted data URI remains an image source") {
        const std::string source = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB";
        bridge.load_script("createImage('img', ''); setImageSource('img', '" + source + "');");
        auto* image = dynamic_cast<ImageView*>(bridge.widget("img"));
        REQUIRE(image != nullptr);
        REQUIRE(image->image_source() == source);
    }

    SECTION("relative path that exists under the base resolves absolute") {
        bridge.set_script_base_dir(base);
        bridge.load_script(R"(
            createImage('img', '');
            setImageSource('img', 'assets/hero.png');
        )");
        auto* image = dynamic_cast<ImageView*>(bridge.widget("img"));
        REQUIRE(image != nullptr);
        REQUIRE(image->image_path()
                == "file://" + (base / "assets" / "hero.png").lexically_normal().generic_string());
    }

    SECTION("unset base leaves relative paths untouched (historical CWD behavior)") {
        bridge.load_script(R"(
            createImage('img', '');
            setImageSource('img', 'assets/hero.png');
        )");
        auto* image = dynamic_cast<ImageView*>(bridge.widget("img"));
        REQUIRE(image != nullptr);
        REQUIRE(image->image_path() == "file://assets/hero.png");
    }

    SECTION("a scratch script falls back to its reviewed artifact root") {
        static constexpr unsigned char tiny_png[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
            0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
            0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
            0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x04, 0x00,
            0x09, 0xfb, 0x03, 0xfd, 0xa7, 0xe9, 0x81, 0x86,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
            0xae, 0x42, 0x60, 0x82,
        };
        {
            std::ofstream f(reviewed_root / "assets" / "body.png", std::ios::binary);
            f.write(reinterpret_cast<const char*>(tiny_png), sizeof(tiny_png));
        }
        {
            std::ofstream f(reviewed_root / "assets" / "indicator.png", std::ios::binary);
            f.write(reinterpret_cast<const char*>(tiny_png), sizeof(tiny_png));
        }
        bridge.set_script_base_dir(scratch_root);
        bridge.set_asset_roots({reviewed_root});
        bridge.load_script(R"(
            createImage('img', '');
            setImageSource('img', 'assets/captured.png');
            createFader('fader', 0, 0, 100, 20, 'horizontal');
            setFaderCapturedArt('fader', 'assets/body.png', 1, 1,
                               'assets/indicator.png', 1, 1, 0.5,
                               0, 0, 100, 20);
        )");
        auto* image = dynamic_cast<ImageView*>(bridge.widget("img"));
        REQUIRE(image != nullptr);
        REQUIRE(image->image_path()
                == "file://" + fs::canonical(
                    reviewed_root / "assets" / "captured.png").generic_string());
        auto* fader = dynamic_cast<Fader*>(bridge.widget("fader"));
        REQUIRE(fader != nullptr);
        REQUIRE(fader->has_captured_indicator_art());
    }

    SECTION("reviewed roots reject a symlink escape") {
        const auto outside = base / "outside.png";
        { std::ofstream f(outside, std::ios::binary); f << "outside"; }
        std::error_code symlink_error;
        fs::create_symlink(outside, reviewed_root / "assets" / "escape.png",
                           symlink_error);
        if (!symlink_error) {
            bridge.set_script_base_dir(scratch_root);
            bridge.set_asset_roots({reviewed_root});
            REQUIRE(bridge.resolve_script_relative("assets/escape.png")
                    == "assets/escape.png");
        }
    }

    SECTION("absolute paths and misses pass through unchanged") {
        bridge.set_script_base_dir(base);
        REQUIRE(bridge.resolve_script_relative("/abs/elsewhere.png") == "/abs/elsewhere.png");
        REQUIRE(bridge.resolve_script_relative("assets/nope.png") == "assets/nope.png");
        REQUIRE(bridge.resolve_script_relative("memory://sha256=aa") == "memory://sha256=aa");
        REQUIRE(bridge.resolve_script_relative("") == "");
    }

    std::error_code ec;
    fs::remove_all(base, ec);
}

// A gesture's press/release edges and its moves travel to JS on two different
// callbacks: `on_dom_pointer_event` carries the edges and
// `on_dom_pointer_move_event` carries the moves. `deliver_mouse_drag` — the verb both macOS hosts call for
// every drag sample — hits BOTH, with `is_down == true` because the button is
// genuinely still held. Classifying that by `is_down` alone reported every drag
// sample as a fresh `pointerdown`, which re-latched the gesture origin of any
// handler that captures one on press.
TEST_CASE("WidgetBridge reports a drag tick as a move, not a fresh pointerdown",
          "[view][bridge][pointer]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var downs = 0, moves = 0, ups = 0;
        createCanvas('surface', '');
        on('surface', 'pointerdown', function() { downs += 1; });
        on('surface', 'pointermove', function() { moves += 1; });
        on('surface', 'pointerup',   function() { ups += 1; });
    )");

    auto* surface = bridge.widget("surface");
    REQUIRE(surface != nullptr);
    surface->set_bounds({0, 0, 200, 200});

    MouseEvent press;
    press.position = {10, 10};
    press.window_position = {10, 10};
    press.is_down = true;
    press.phase = MousePhase::press;
    REQUIRE(surface->on_dom_pointer_event);
    REQUIRE(surface->on_dom_pointer_move_event);
    surface->on_dom_pointer_event(press, true);

    for (int i = 1; i <= 3; ++i) {
        MouseEvent drag = press;
        drag.position = {10, 10.0f + i};
        drag.window_position = drag.position;
        drag.phase = MousePhase::drag;
        surface->on_dom_pointer_event(drag, true);  // edge channel stays silent
        surface->on_dom_pointer_move_event(drag, true);
    }

    MouseEvent release = press;
    release.is_down = false;
    release.phase = MousePhase::release;
    surface->on_dom_pointer_event(release, true);

    // Exactly one press edge for the whole gesture — the three drag samples must
    // not have re-fired it.
    CHECK(engine.evaluate("downs").getWithDefault<int>(-1) == 1);
    CHECK(engine.evaluate("ups").getWithDefault<int>(-1) == 1);
    // Each tick produced exactly one move: the modern channel stayed silent so
    // it did not double-report what on_drag already delivered.
    CHECK(engine.evaluate("moves").getWithDefault<int>(-1) == 3);
}

// A hover sample (button up, MousePhase::hover) carries is_down == false, which
// the same inference would have reported as a phantom `pointerup`.
TEST_CASE("WidgetBridge does not report a hover sample as a pointerup",
          "[view][bridge][pointer]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var ups = 0;
        createCanvas('surface', '');
        on('surface', 'pointerup', function() { ups += 1; });
    )");

    auto* surface = bridge.widget("surface");
    REQUIRE(surface != nullptr);

    MouseEvent hover;
    hover.position = {10, 10};
    hover.window_position = {10, 10};
    hover.is_down = false;
    hover.phase = MousePhase::hover;
    REQUIRE(surface->on_dom_pointer_event);
    surface->on_dom_pointer_event(hover, true);

    CHECK(engine.evaluate("ups").getWithDefault<int>(-1) == 0);
}

// The negative control for dropping the id seed: removing a default must not
// remove the real thing. Covers the FACTORY call shape and the accessible
// name, neither of which the tag-path case above reaches — an id that stopped
// painting but still got read aloud would be a half-fix.
TEST_CASE("an explicit label still reaches a bridge-made control",
          "[view][bridge][labels]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('k1', 'root');
        setLabel('k1', 'RATE');
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("k1"));
    REQUIRE(knob != nullptr);
    CHECK(knob->label() == "RATE");
    CHECK(knob->access_label() == "RATE");
}

TEST_CASE("setFontStyle preserves CSS oblique as a distinct native slant",
          "[view][widget-bridge][typography][oblique]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createLabel('oblique', 'slanted', '');\n"
        "setFontStyle('oblique', 'oblique 12deg');");

    auto* label = dynamic_cast<Label*>(bridge.widget("oblique"));
    REQUIRE(label != nullptr);
    CHECK(label->font_style() == 2);
}

TEST_CASE("text runs inherit base tracking and normalize slant case",
          "[view][widget-bridge][typography][text-runs]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createLabel('mixed', 'alpha beta', '');
        setLetterSpacing('mixed', 3);
        setFontStyle('mixed', 'Italic');
        setTextDecoration('mixed', 'underline');
        setTextRuns('mixed', [
          { start: 6, end: 10, fontStyle: 'Oblique 12deg', textDecoration: 'none' }
        ]);
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("mixed"));
    REQUIRE(label != nullptr);
    CHECK(label->font_style() == 1);
    label->set_bounds({0, 0, 200, 30});
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    bool saw_italic_tracking = false;
    bool saw_oblique_tracking = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::set_font_full)
            continue;
        if (command.f[2] == Catch::Approx(1.0f) &&
            command.f[3] == Catch::Approx(3.0f))
            saw_italic_tracking = true;
        if (command.f[2] == Catch::Approx(2.0f) &&
            command.f[3] == Catch::Approx(3.0f))
            saw_oblique_tracking = true;
    }
    CHECK(saw_italic_tracking);
    CHECK(saw_oblique_tracking);
    CHECK(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 2);
}

TEST_CASE("setTextRuns snaps malformed byte offsets to UTF-8 boundaries",
          "[view][widget-bridge][typography][text-runs][utf8]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createLabel('mixed', 'A\u00e9B', '');
        setTextRuns('mixed', [{ start: 1, end: 2, fontWeight: 700 }]);
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("mixed"));
    REQUIRE(label != nullptr);
    REQUIRE(label->attributed_span_count() == 3);
    label->set_bounds({0, 0, 100, 24});
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    std::string painted;
    for (const auto& command : canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
            painted += command.text;
    CHECK(painted == std::string("A") + "\xc3\xa9" + "B");
}

TEST_CASE("setCapturedLineBoxes rejects UTF-16 surrogate-pair splits",
          "[view][widget-bridge][typography][text-cache][utf16]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createLabel('emoji', 'A\ud83d\ude00B', '');
        setCapturedLineBoxes('emoji', [
          { left: 0, top: 0, width: 100, height: 18, start: 1, length: 1 }
        ], 100, 'Inter');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("emoji"));
    REQUIRE(label != nullptr);
    CHECK(label->cached_line_boxes().empty());
}

TEST_CASE("clearCapturedLineBoxes returns responsive text to native shaping",
          "[widget_bridge][typography][responsive]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"js(
        createLabel('responsive-copy', 'Readable copy', '');
        setCapturedLineBoxes('responsive-copy', [
          {left:0, top:0, width:70, height:13, start:0, length:13}
        ], 100, 'CapturedFace', false);
    )js");
    auto* label = dynamic_cast<Label*>(bridge.widget("responsive-copy"));
    REQUIRE(label != nullptr);
    REQUIRE(label->cached_line_boxes().size() == 1);

    bridge.load_script(R"js(clearCapturedLineBoxes('responsive-copy');)js");
    CHECK(label->cached_line_boxes().empty());
}

TEST_CASE("web-compat overflow auto materializes a real ScrollView",
          "[widget_bridge][web-compat][scroll][responsive]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"js(
        var panel = document.createElement('div');
        panel.id = 'scroll-panel';
        panel.style.overflowY = 'auto';
        panel.style.width = '120px';
        panel.style.height = '80px';
        panel.style.background = 'rgba(14,18,25,0.98)';
        panel.style.border = '1px solid rgba(255,255,255,0.1)';
        panel.style.borderWidth = '1px';
        panel.style.borderColor = 'rgba(255,255,255,0.1)';
        panel.style.borderStyle = 'solid';
        panel.style.borderRadius = '8px';
        document.body.appendChild(panel);
        var content = document.createElement('div');
        content.style.position = 'absolute';
        content.style.left = '8px';
        content.style.top = '70px';
        content.style.width = '80px';
        content.style.height = '40px';
        panel.appendChild(content);
        var scrollPanelNativeId = panel._id;
    )js");
    const auto native_id = engine.evaluate("scrollPanelNativeId")
        .getWithDefault<std::string>("");
    REQUIRE_FALSE(native_id.empty());
    auto* scroll = dynamic_cast<ScrollView*>(bridge.widget(native_id));
    REQUIRE(scroll != nullptr);
    REQUIRE(scroll->has_background_color());
    REQUIRE(scroll->has_border());
    REQUIRE(scroll->border_width() == Catch::Approx(1.0f));

    // The materialized container needs no explicit setScrollContentSize call:
    // its native ScrollView derives overflow from the child boxes it owns.
    scroll->set_bounds({0.0f, 0.0f, 120.0f, 80.0f});
    scroll->layout_children();
    CHECK(scroll->content_size().width == Catch::Approx(120.0f));
    CHECK(scroll->content_size().height == Catch::Approx(111.0f));
    CHECK(scroll->wants_wheel_scroll());

    // A materialized adapter may temporarily own an extent while replacing a
    // captured container. Omitting dimensions restores automatic sizing and
    // refreshes from the already-laid-out live descendants immediately.
    bridge.load_script("setScrollContentSize(scrollPanelNativeId, 120, 240);");
    REQUIRE(scroll->content_size().height == Catch::Approx(240.0f));
    bridge.load_script("setScrollContentSize(scrollPanelNativeId);");
    CHECK(scroll->content_size().height == Catch::Approx(111.0f));

    bridge.load_script("content.style.display = 'none';");
    scroll->layout_children();
    CHECK(scroll->content_size().width == Catch::Approx(120.0f));
    CHECK(scroll->content_size().height == Catch::Approx(80.0f));
    CHECK_FALSE(scroll->wants_wheel_scroll());

    // Exercise the shared primitive independently of CSS longhand precedence:
    // a ScrollView must paint its own box before applying child scroll offset.
    ScrollView painted_scroll;
    painted_scroll.set_bounds({0.0f, 0.0f, 120.0f, 80.0f});
    painted_scroll.set_background_color(
        pulp::canvas::Color::rgba8(14, 18, 25, 250));
    painted_scroll.set_border(
        pulp::canvas::Color::rgba8(255, 255, 255, 26), 1.0f, 8.0f);
    pulp::canvas::RecordingCanvas painted;
    painted_scroll.paint_all(painted);
    CHECK(painted.count(
              pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 1);
    const auto border_draws = painted.count(
        pulp::canvas::DrawCommand::Type::stroke_rounded_rect)
        + painted.count(
            pulp::canvas::DrawCommand::Type::fill_current_path);
    CHECK(border_draws == 1);

    bridge.load_script("__domAppend('', 'hinted-scroll', 'div', 'scroll');");
    CHECK(dynamic_cast<ScrollView*>(bridge.widget("hinted-scroll")) != nullptr);
}

// ── Layout-pass elision (getLayoutRect / getLayoutBoxMetrics storm) ─────────
//
// The geometry queries used to force a full `layout_children()` on EVERY call.
// A shipping scripted UI calls them dozens of times per frame and once per
// pointer event, so a drag paid a whole tree layout per sample on the UI
// thread. These cases pin the fix AND its correctness boundary: eliding a
// layout is only safe while nothing has changed, and the counter that decides
// that must see mutations from OUTSIDE the bridge too.

TEST_CASE("repeated geometry reads with no mutation cost one layout pass",
          "[view][bridge][layout][perf]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'width', 300);
        setFlex('outer', 'height', 200);
        createLabel('leaf', 'Leaf', 'outer');
        setFlex('leaf', 'height', 24);
        layout();
    )");

    const auto before = View::layout_pass_count();
    for (int i = 0; i < 60; ++i) {
        engine.evaluate("getLayoutRect('leaf').width");
        engine.evaluate("getLayoutBoxMetrics('leaf').offsetHeight");
    }
    const auto passes = View::layout_pass_count() - before;

    // 120 reads, nothing mutated: at most one pass. Before the fix this was 120.
    INFO("layout passes for 120 reads: " << passes);
    CHECK(passes <= 1);
}

TEST_CASE("host paint reuses a layout completed by the bridge",
          "[view][bridge][layout][perf]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'width', 300);
        setFlex('outer', 'height', 200);
        createLabel('leaf', 'Leaf', 'outer');
        setFlex('leaf', 'height', 24);
        layout();
    )");

    const auto before = View::layout_pass_count();
    engine.evaluate("getLayoutRect('leaf').height");
    root.layout_children_if_needed();
    CHECK(View::layout_pass_count() - before == 0);

    engine.evaluate("setFlex('leaf', 'height', 72)");
    root.layout_children_if_needed();
    engine.evaluate("getLayoutRect('leaf').height");
    CHECK(View::layout_pass_count() - before == 1);
}

TEST_CASE("layout invalidation is isolated between independent view trees",
          "[view][bridge][layout][perf]") {
    View editor_root;
    editor_root.set_bounds({0, 0, 400, 300});
    editor_root.layout_children_if_needed();

    View helper_root;
    helper_root.set_bounds({0, 0, 32, 32});

    const auto before = View::layout_pass_count();
    helper_root.invalidate_layout();
    editor_root.layout_children_if_needed();

    // A process-wide generation makes the editor pay for the helper tree's
    // mutation. Per-tree generations keep the already-current editor intact.
    CHECK(View::layout_pass_count() - before == 0);
}

TEST_CASE("live text in an explicitly sized label does not relayout the root",
          "[view][bridge][layout][perf][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('status', '');
        setFlex('status', 'width', 240);
        setFlex('status', 'height', 26);
        createLabel('status-text', 'BAND 1/64', 'status');
        setFlex('status-text', 'width', '100%');
        setFlex('status-text', 'height', '100%');
        layout();
    )");

    engine.evaluate("getLayoutBoxMetrics('status-text').offsetWidth");
    const auto before = View::layout_pass_count();
    engine.evaluate("setText('status-text', 'BAND 2/64')");
    root.layout_children_if_needed();

    CHECK(View::layout_pass_count() - before == 0);
    auto* label = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(label != nullptr);
    CHECK(label->text() == "BAND 2/64");
}

// A live readout that declares a width but leaves its height to the line box
// is the shape design-import emits by default -- an author only writes an
// explicit height when centering forces them to. Requiring both axes to be
// declared made the common case pay a full-tree Yoga pass per pointer sample.
// A single-line label's height is one line box, so the text can only move it
// by resolving a different font; measuring proves that per write far more
// cheaply than relaying out the tree.
TEST_CASE("live text in a width-only label does not relayout the root",
          "[view][bridge][layout][perf][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('status', '');
        setFlex('status', 'width', 240);
        setFlex('status', 'height', 26);
        createLabel('status-text', 'BAND 1/64', 'status');
        setFlex('status-text', 'width', '100%');
        layout();
    )");

    engine.evaluate("getLayoutBoxMetrics('status-text').offsetWidth");

    // Drive a synthetic drag: one status write per pointer sample, each
    // followed by the geometry read the draw path makes.
    const auto before = View::layout_pass_count();
    for (int i = 0; i < 60; ++i) {
        engine.evaluate("setText('status-text', 'BAND " + std::to_string(i) +
                        "/64')");
        engine.evaluate("getLayoutBoxMetrics('status-text').offsetWidth");
        root.layout_children_if_needed();
    }
    const auto passes = View::layout_pass_count() - before;

    INFO("layout passes for 60 live-readout text writes: " << passes);
    CHECK(passes == 0);

    auto* label = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(label != nullptr);
    CHECK(label->text() == "BAND 59/64");

    // Negative control. The instrument must be able to read non-zero on the
    // same tree with the same counter, or the zero above proves nothing: an
    // intrinsic-width label has no pinned horizontal axis, so its text change
    // really can move its siblings and MUST still invalidate.
    engine.evaluate(R"(
        createLabel('auto-text', 'BAND 1/64', 'status');
        layout();
    )");
    const auto control_before = View::layout_pass_count();
    engine.evaluate("setText('auto-text', 'BAND 2/64')");
    root.layout_children_if_needed();
    const auto control_passes = View::layout_pass_count() - control_before;

    INFO("control layout passes for an intrinsic-width label: "
         << control_passes);
    CHECK(control_passes > 0);
}

// Under `align-items: baseline` a row's cross-axis positions derive from each
// item's BASELINE, and Label feeds Yoga a real one: yoga_baseline() calls
// Label::baseline_y(), which shapes the current text and returns
// PreparedText::ascent(). Ascent is therefore text-dependent, and it is a
// separate max from the line height (TextShaper::prepare maxes ascent,
// descent and leading independently against the shaped box), so new copy can
// hold the height fixed while moving the ascent. Height alone is then not a
// sufficient proof that nothing moved, and neither is an explicit height --
// baseline_y() ignores the box height entirely. A baseline participant must
// reflow on every text write.
//
// This asserts the GUARD rather than a measured ascent move: an exhaustive
// scan of this platform's font stack (889 single-codepoint samples, 15
// distinct ascent/descent/leading triples, all 79 combinations reachable by
// mixing them) produced 79 distinct line heights and zero cases of equal
// height with differing ascent, so no real text pair can exercise the move
// here. The scan's collision detector was positive-controlled against a
// synthetic face offset by +1 ascent / -1 descent, which it did report. The
// guard still has to hold: the fast path ships to every Pulp app, on font
// stacks this scan never saw.
TEST_CASE("a baseline-aligned label reflows on every text write",
          "[view][bridge][layout][perf][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createRow('row', '');
        setFlex('row', 'width', 320);
        setFlex('row', 'height', 40);
        setFlex('row', 'align_items', 'baseline');
        createLabel('lead', 'BAND', 'row');
        setFlex('lead', 'width', 120);
        setFlex('lead', 'height', 26);
        createLabel('tail', 'x', 'row');
        setFlex('tail', 'width', 120);
        layout();
    )");
    engine.evaluate("getLayoutBoxMetrics('lead').offsetWidth");

    auto* lead = dynamic_cast<Label*>(bridge.widget("lead"));
    auto* tail = dynamic_cast<Label*>(bridge.widget("tail"));
    REQUIRE(lead != nullptr);
    REQUIRE(tail != nullptr);

    // Explicit width AND height -- the pre-existing fast path's own condition,
    // so this fails on the unguarded version for the strongest reason.
    const auto before = View::layout_pass_count();
    engine.evaluate("setText('lead', 'BAND 12/64')");
    root.layout_children_if_needed();
    const auto passes = View::layout_pass_count() - before;

    INFO("layout passes for a baseline-aligned text write: " << passes);
    CHECK(passes > 0);

    // The row must still be coherent afterwards: Yoga got a real baseline
    // from each participant rather than the degenerate box-bottom default.
    CHECK(lead->baseline_y() > 0.0f);
    CHECK(tail->baseline_y() > 0.0f);

    // Negative control on the same tree and the same counter: drop the
    // baseline participation and the identical write must stop invalidating.
    // Without this, `passes > 0` above could be any unrelated dirtying.
    engine.evaluate("setFlex('row', 'align_items', 'center')");
    root.layout_children_if_needed();
    const auto control_before = View::layout_pass_count();
    engine.evaluate("setText('lead', 'BAND 13/64')");
    root.layout_children_if_needed();
    const auto control_passes = View::layout_pass_count() - control_before;

    INFO("control layout passes once the row is not baseline-aligned: "
         << control_passes);
    CHECK(control_passes == 0);
}

// The fast path is a claim about geometry, not a licence to skip reflow when
// geometry actually moves. A multi-line label with a declared width still has
// a text-dependent height, so gaining a line must still invalidate.
TEST_CASE("a width-only multiline label still relayouts when it gains a line",
          "[view][bridge][layout][perf][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('status', '');
        setFlex('status', 'width', 240);
        createLabel('status-text', 'one line', 'status');
        setFlex('status-text', 'width', '100%');
        setMultiLine('status-text', true);
        layout();
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(label != nullptr);
    const float one_line = label->intrinsic_height();

    const auto before = View::layout_pass_count();
    engine.evaluate("setText('status-text', 'two\\nlines')");
    root.layout_children_if_needed();

    const float two_lines = label->intrinsic_height();
    REQUIRE(two_lines > one_line);
    CHECK(View::layout_pass_count() - before > 0);
    CHECK(label->text() == "two\nlines");
}

TEST_CASE("replaying an identical flex value does not dirty geometry",
          "[view][bridge][layout][perf]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'width', 300);
        setFlex('outer', 'height', 200);
        createLabel('leaf', 'Leaf', 'outer');
        setFlex('leaf', 'height', 24);
        layout();
    )");

    const auto before = View::layout_pass_count();
    for (int i = 0; i < 60; ++i) {
        engine.evaluate("setFlex('leaf', 'height', 24)");
        engine.evaluate("getLayoutRect('leaf').height");
    }
    const auto passes = View::layout_pass_count() - before;

    INFO("layout passes for 60 identical style writes: " << passes);
    CHECK(passes <= 1);

    engine.evaluate("setFlex('leaf', 'height', 72)");
    const auto changed = engine.evaluate("getLayoutRect('leaf').height")
                             .getWithDefault<double>(-1.0);
    CHECK(changed == 72.0);
}

TEST_CASE("a mutation on a DEEP child still forces a fresh layout",
          "[view][bridge][layout][perf]") {
    // The reachable negative for the elision, and the reason the root's
    // `layout_dirty_` flag cannot be the guard: invalidation does not propagate
    // upward, so a flex change on a nested child leaves the ROOT's flag false
    // while the tree is genuinely stale.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'direction', 'col');
        setFlex('outer', 'width', 300);
        setFlex('outer', 'height', 200);
        createCol('mid', 'outer');
        createLabel('leaf', 'Leaf', 'mid');
        setFlex('leaf', 'height', 24);
        layout();
    )");

    const auto h0 = engine.evaluate("getLayoutBoxMetrics('leaf').offsetHeight")
                        .getWithDefault<double>(-1.0);
    REQUIRE(h0 > 0.0);  // cold read must not be the stale 0x0

    const auto before = View::layout_pass_count();
    for (int i = 0; i < 25; ++i) engine.evaluate("getLayoutRect('leaf').height");
    engine.evaluate("setFlex('leaf', 'height', 72);");   // deep child, not root
    for (int i = 0; i < 25; ++i) engine.evaluate("getLayoutRect('leaf').height");
    const auto passes = View::layout_pass_count() - before;

    // Exactly the two passes that were needed: nothing before the mutation is
    // stale, everything after it is.
    INFO("layout passes across 50 reads with one mutation: " << passes);
    CHECK(passes <= 2);

    const auto h1 = engine.evaluate("getLayoutBoxMetrics('leaf').offsetHeight")
                        .getWithDefault<double>(-1.0);
    CHECK(h1 > h0);  // the post-mutation read sees the NEW geometry
}

TEST_CASE("a mutation from OUTSIDE the bridge still forces a fresh layout",
          "[view][bridge][layout][perf]") {
    // This is the case that catches a bridge-only counter. A host resize, C++
    // widget code, or anything else that never passes through a bridge entry
    // point still changes what a geometry query must return; a generation
    // bumped only at bridge mutation sites would happily serve stale bounds.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'width', '100%');
        setFlex('outer', 'height', '100%');
        layout();
    )");

    const auto w0 = engine.evaluate("getLayoutRect('outer').width")
                        .getWithDefault<double>(-1.0);
    REQUIRE(w0 > 0.0);

    root.set_bounds({0, 0, 800, 300});      // pure C++ path, no bridge call
    const auto w1 = engine.evaluate("getLayoutRect('outer').width")
                        .getWithDefault<double>(-1.0);

    INFO("width before=" << w0 << " after non-bridge resize=" << w1);
    CHECK(w1 > w0);
}

TEST_CASE("the first geometry read lays out rather than returning 0x0",
          "[view][bridge][layout][perf]") {
    // The stale-0x0 regression the getLayoutRect comment exists to prevent:
    // a mount-time getBoundingClientRect that reads 0 gates an entire canvas
    // paint pipeline, and it looks like a broken design import rather than a
    // layout bug. A generation guard initialised wrong reintroduces it, so the
    // cold read is pinned explicitly — note there is NO layout() below.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCol('outer', '');
        setFlex('outer', 'width', 250);
        setFlex('outer', 'height', 120);
        createLabel('leaf', 'Leaf', 'outer');
        setFlex('leaf', 'height', 30);
    )");

    const auto w = engine.evaluate("getLayoutRect('outer').width").getWithDefault<double>(-1.0);
    const auto h = engine.evaluate("getLayoutRect('outer').height").getWithDefault<double>(-1.0);
    INFO("cold read: " << w << "x" << h);
    CHECK(w > 0.0);
    CHECK(h > 0.0);
}

// ── Script-driven trace spans ───────────────────────────────────────────────
// A C++ scope span is balanced by lifetime; a script-driven one is balanced
// only by the script's control flow. These cover the two ways that goes wrong
// (more opens than closes, more closes than opens) and the depth cap, in a
// build where the Perfetto macros compile to nothing — the balance bookkeeping
// is deliberately independent of them so it is testable on the default gate.
TEST_CASE("script trace spans nest and unwind", "[widget-bridge][trace]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto depth = [&engine] {
        return engine.evaluate("__traceStats__().depth").getWithDefault<double>(-1.0);
    };

    REQUIRE(depth() == Catch::Approx(0.0));
    CHECK(engine.evaluate("pulpTrace.begin('outer')").getWithDefault<bool>(false));
    CHECK(engine.evaluate("pulpTrace.begin('inner')").getWithDefault<bool>(false));
    CHECK(depth() == Catch::Approx(2.0));
    CHECK(engine.evaluate("pulpTrace.end()").getWithDefault<bool>(false));
    CHECK(engine.evaluate("pulpTrace.end()").getWithDefault<bool>(false));
    CHECK(depth() == Catch::Approx(0.0));

    // scope() closes its span even when the body throws.
    engine.evaluate(
        "try { pulpTrace.scope('boom', function() { throw new Error('x'); }); }"
        "catch (e) {}");
    CHECK(depth() == Catch::Approx(0.0));
}

TEST_CASE("closing more spans than were opened is counted, not swallowed",
          "[widget-bridge][trace]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    const auto before =
        engine.evaluate("__traceStats__().unmatchedEnd").getWithDefault<double>(-1.0);
    CHECK_FALSE(engine.evaluate("pulpTrace.end()").getWithDefault<bool>(true));
    CHECK(engine.evaluate("__traceStats__().unmatchedEnd").getWithDefault<double>(-1.0)
          == Catch::Approx(before + 1.0));
    // A refused close must not push the depth negative.
    CHECK(engine.evaluate("__traceStats__().depth").getWithDefault<double>(-1.0)
          == Catch::Approx(0.0));
}

TEST_CASE("the span depth cap refuses instead of growing without bound",
          "[widget-bridge][trace]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    engine.evaluate(
        "var refusals = 0;"
        "for (var i = 0; i < 200; ++i) { if (!pulpTrace.begin('deep' + i)) refusals += 1; }");
    CHECK(engine.evaluate("__traceStats__().depth").getWithDefault<double>(-1.0)
          == Catch::Approx(64.0));
    CHECK(engine.evaluate("refusals").getWithDefault<double>(-1.0)
          == Catch::Approx(136.0));
    engine.evaluate("for (var i = 0; i < 64; ++i) pulpTrace.end();");
    CHECK(engine.evaluate("__traceStats__().depth").getWithDefault<double>(-1.0)
          == Catch::Approx(0.0));
}

TEST_CASE("a handler that leaves a span open is force-closed observably",
          "[widget-bridge][trace]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createToggleButton('btn', '');
        on('btn', 'click', function() {
            pulpTrace.begin('leaked_a');
            pulpTrace.begin('leaked_b');
        });
    )");
    auto* button = bridge.widget("btn");
    REQUIRE(button != nullptr);
    REQUIRE(static_cast<bool>(button->on_click));
    button->set_bounds({0, 0, 400, 300});

    const auto before =
        engine.evaluate("__traceStats__().forceClosed").getWithDefault<double>(-1.0);
    root.simulate_click({50, 60});

    // The handler returned with two spans open. Every later slice would have
    // nested under them, so the boundary closes them and says how many.
    CHECK(engine.evaluate("__traceStats__().depth").getWithDefault<double>(-1.0)
          == Catch::Approx(0.0));
    CHECK(engine.evaluate("__traceStats__().forceClosed").getWithDefault<double>(-1.0)
          == Catch::Approx(before + 2.0));
}

// ── Position writes must reach the next geometry read ───────────────────────
// Geometry readers lay out only when the tree's layout generation moved. The
// CSS position offsets feed the Yoga pass, so a write that changes one has to
// move that generation or the next read answers from the pre-move box.
TEST_CASE("setLeft/setTop are visible to the next geometry read", "[widget-bridge][layout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createCol('outer','');"
                       "setFlex('outer','width',300);"
                       "setFlex('outer','height',300);"
                       "createCol('inner','outer');"
                       "setFlex('inner','width',50);"
                       "setFlex('inner','height',50);"
                       "layout();");

    // The read that establishes a completed layout, so the next one is the
    // one the generation guard could wrongly elide.
    const auto x0 = engine.evaluate("getLayoutRect('inner').x").getWithDefault<double>(-1.0);
    REQUIRE(x0 == Catch::Approx(0.0));

    engine.evaluate("setPosition('inner','absolute');"
                    "setLeft('inner',120);"
                    "setTop('inner',60);");

    // No explicit layout() call: the read itself must observe the move.
    CHECK(engine.evaluate("getLayoutRect('inner').x").getWithDefault<double>(-1.0)
          == Catch::Approx(120.0));
    CHECK(engine.evaluate("getLayoutRect('inner').y").getWithDefault<double>(-1.0)
          == Catch::Approx(60.0));
}

TEST_CASE("re-writing the same position does not force a layout", "[widget-bridge][layout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createCol('outer','');"
                       "setFlex('outer','width',300);"
                       "setFlex('outer','height',300);"
                       "createCol('inner','outer');"
                       "setFlex('inner','width',50);"
                       "setFlex('inner','height',50);"
                       "setPosition('inner','absolute');"
                       "setLeft('inner',120);"
                       "setTop('inner',60);"
                       "layout();");
    engine.evaluate("getLayoutRect('inner').x");

    // The design-import replay re-applies captured position metadata on every
    // commit. Identical values must stay free, or each re-applied binding
    // costs a whole-tree pass.
    const auto before = View::layout_pass_count();
    for (int i = 0; i < 40; ++i) {
        engine.evaluate("setPosition('inner','absolute');"
                        "setLeft('inner',120);"
                        "setTop('inner',60);");
        engine.evaluate("getLayoutRect('inner').x");
    }
    CHECK(View::layout_pass_count() - before == 0);
}

// ── A scripted UI revises its cursor on hover, not only on drag ─────────────
//
// The whole chain a plugin editor actually uses: pointer motion with no button
// held → deliver_hover_move → on_dom_pointer_move_event → the JS `pointermove`
// listener → setCursor → View::cursor(). Every hop existed already except the
// first, so a scripted UI only ever saw a move once a button went down and its
// cursor changed only on mouse-down.
//
// It also pins the payload: a plain hover must report `buttons === 0`. That
// field is how a script tells hovering from dragging (grab vs grabbing), and
// the bridge used to hardcode it to 1 for every move — so even a delivered
// hover would have read as a drag.
TEST_CASE("a hover runs the scripted pointermove and its cursor decision",
          "[view][bridge][cursor][hover]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var moves = 0, downs = 0;
        var move_buttons = -1, move_is_down = -1, down_buttons = -1;
        createLabel('surface', 'Surface', '');
        on('surface', 'pointermove', function(e) {
            moves += 1;
            move_buttons = e.buttons;
            setCursor('surface', e.buttons ? 'grabbing' : 'grab');
        });
        on('surface', 'pointerdown', function(e) {
            downs += 1;
            down_buttons = e.buttons;
        });
        registerPointer('surface');
    )");

    auto* surface = bridge.widget("surface");
    REQUIRE(surface != nullptr);
    REQUIRE(static_cast<bool>(surface->on_dom_pointer_move_event));
    surface->set_bounds({0, 0, 400, 300});
    // Nothing has claimed a cursor yet, so a later reading cannot be state the
    // scene was built with.
    REQUIRE(surface->cursor() == View::CursorStyle::default_);

    // Positive control: the button path, which worked before the fix. If this
    // arm ever fails the harness is broken and the hover arm below proves
    // nothing.
    root.simulate_drag({50, 50}, {80, 80}, 1);
    REQUIRE(engine.evaluate("downs").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("moves").getWithDefault<int>(0) >= 1);
    CHECK(engine.evaluate("down_buttons").getWithDefault<int>(-1) == 1);
    CHECK(engine.evaluate("move_buttons").getWithDefault<int>(-1) == 1);
    CHECK(surface->cursor() == View::CursorStyle::grabbing);

    // The property: pointer motion, no button anywhere in it.
    engine.evaluate("moves = 0; move_buttons = -1;");
    pulp::view::deliver_hover_move(root, {120, 90});

    CHECK(engine.evaluate("moves").getWithDefault<int>(0) == 1);
    // A hover is not a drag. This is the bit a script reads to choose between
    // the two cursors.
    CHECK(engine.evaluate("move_buttons").getWithDefault<int>(-1) == 0);
    // No extra press was manufactured on the way through.
    CHECK(engine.evaluate("downs").getWithDefault<int>(0) == 1);
    // And the cursor the handler chose is the one the view now publishes — the
    // value a host reads back to tell AppKit what to display.
    CHECK(surface->cursor() == View::CursorStyle::grab);
}
