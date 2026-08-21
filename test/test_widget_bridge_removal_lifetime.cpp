#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <string>

using namespace pulp::state;
using namespace pulp::view;

TEST_CASE("click callbacks can remove their own widget",
          "[view][bridge][click][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var click_hits = 0;
        createLabel('remover', 'Remove', '');
        on('remover', 'click', function () {
            click_hits += 1;
            removeWidget('remover');
        });
        registerClick('remover');
    )JS");

    auto* remover = bridge.widget("remover");
    REQUIRE(remover != nullptr);
    const auto removed_lifetime = remover->import_binding_lifetime_token();
    remover->set_bounds({10, 10, 100, 80});
    root.simulate_click({20, 20});

    REQUIRE(bridge.widget("remover") == nullptr);
    REQUIRE(root.child_count() == 0);
    REQUIRE(engine.evaluate("click_hits").getWithDefault<int>(0) == 1);
    REQUIRE_FALSE(removed_lifetime.expired());

    bridge.load_script(R"JS(
        createLabel('collector', 'Collect', '');
        on('collector', 'click', function () {});
        registerClick('collector');
    )JS");
    bridge.widget("collector")->set_bounds({10, 10, 100, 80});
    root.simulate_click({20, 20});
    REQUIRE(removed_lifetime.expired());
}

TEST_CASE("a claimed overlay can remove itself during a synthetic click",
          "[view][bridge][click][overlay][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 140});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var underlay_hits = 0;
        createLabel('underlay', 'Underlay', '');
        createLabel('overlay', 'Overlay', '');
        on('underlay', 'click', function () { underlay_hits += 1; });
        on('overlay', 'click', function () { removeWidget('overlay'); });
        registerClick('underlay');
        registerClick('overlay');
        claimOverlay('overlay');
    )JS");

    auto* underlay = bridge.widget("underlay");
    auto* overlay = bridge.widget("overlay");
    REQUIRE(underlay != nullptr);
    REQUIRE(overlay != nullptr);
    underlay->set_bounds({0, 0, 240, 140});
    overlay->set_bounds({20, 20, 100, 80});

    root.simulate_click({40, 40});
    REQUIRE(bridge.widget("overlay") == nullptr);
    REQUIRE(root.interaction().active_overlay == nullptr);

    // The next click must route to the live tree. Before the subtree-removal
    // guard, the root retained the detached overlay and overlay_contains()
    // dereferenced it here after callback retirement reclaimed the object.
    root.simulate_click({180, 100});
    REQUIRE(engine.evaluate("underlay_hits").getWithDefault<int>(0) == 1);
}

TEST_CASE("a synthetic click preserves a replacement overlay claim",
          "[view][bridge][click][overlay][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 140});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var replacement_dismissals = 0;
        createLabel('overlay', 'Overlay', '');
        on('overlay', 'click', function () {
            removeWidget('overlay');
            createLabel('replacement', 'Replacement', '');
            on('replacement', 'dismiss', function () {
                replacement_dismissals += 1;
            });
            claimOverlay('replacement');
        });
        registerClick('overlay');
        claimOverlay('overlay');
    )JS");

    auto* overlay = bridge.widget("overlay");
    REQUIRE(overlay != nullptr);
    overlay->set_bounds({20, 20, 100, 80});
    root.simulate_click({40, 40});

    auto* replacement = bridge.widget("replacement");
    REQUIRE(replacement != nullptr);
    replacement->set_bounds({20, 20, 100, 80});
    REQUIRE(root.interaction().active_overlay == replacement);

    root.simulate_click({180, 100});
    REQUIRE(root.interaction().active_overlay == nullptr);
    REQUIRE(engine.evaluate("replacement_dismissals").getWithDefault<int>(0) == 1);
}

TEST_CASE("a deferred pointer event is inert after its borrowed engine dies",
          "[view][bridge][lifetime][engine]") {
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    auto engine = std::make_unique<ScriptEngine>();
    auto bridge = std::make_unique<WidgetBridge>(*engine, root, store);
    bridge->load_script(R"JS(
        var hits = 0;
        createLabel('btn', 'Btn', '');
        on('btn', 'pointerdown', function () { hits += 1; });
        registerPointer('btn');
    )JS");
    auto* btn = bridge->widget("btn");
    REQUIRE(btn != nullptr);
    btn->set_bounds({10, 10, 100, 80});

    MouseEvent down{};
    down.is_down = true;
    btn->on_dom_pointer_event(down, true);
    REQUIRE(engine->evaluate("hits").getWithDefault<int>(0) == 1);

    engine.reset();
    REQUIRE_NOTHROW(btn->on_dom_pointer_event(down, true));
    REQUIRE(bridge->widget("btn") == btn);
}

TEST_CASE("overlay dismissal may synchronously release its own callback",
          "[view][bridge][lifetime][overlay]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"JS(
        var dismissed = 0;
        createLabel('pop', 'Pop', '');
        on('pop', 'dismiss', function () {
            dismissed += 1;
            releaseOverlay('pop');
        });
        claimOverlay('pop');
    )JS");

    View::dismiss_active_overlay();
    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 1);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("pointer callbacks can remove their own widget and siblings",
          "[view][bridge][pointer][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 320, 180});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var move_hits = 0;
        createLabel('remover', 'Remove', '');
        createLabel('sibling', 'Sibling', '');
        on('remover', 'pointermove', function () {
            move_hits += 1;
            removeWidget('sibling');
            removeWidget('remover');
        });
        registerPointer('remover');
    )JS");

    auto* remover = bridge.widget("remover");
    REQUIRE(remover != nullptr);
    const auto removed_lifetime = remover->import_binding_lifetime_token();
    remover->set_bounds({10, 10, 120, 100});
    bridge.widget("sibling")->set_bounds({150, 10, 120, 100});

    root.simulate_drag({30, 30}, {80, 70}, 8);

    REQUIRE(bridge.widget("remover") == nullptr);
    REQUIRE(bridge.widget("sibling") == nullptr);
    REQUIRE(root.child_count() == 0);
    REQUIRE(engine.evaluate("move_hits").getWithDefault<int>(0) == 1);

    // The callback body has finished, but its std::function invocation only
    // becomes provably collectable before the next outer bridge callback.
    REQUIRE_FALSE(removed_lifetime.expired());
    bridge.load_script(R"JS(
        createLabel('collector', 'Collect', '');
        on('collector', 'pointerdown', function () {});
        registerPointer('collector');
    )JS");
    bridge.widget("collector")->set_bounds({10, 10, 100, 80});
    root.simulate_click({20, 20});
    REQUIRE(removed_lifetime.expired());
}

TEST_CASE("nested bridge callbacks defer retirement to the outer callback",
          "[view][bridge][pointer][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 320, 180});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    engine.register_function(
        "dispatchNestedPointer",
        [&bridge](choc::javascript::ArgumentList) {
            if (auto* nested = bridge.widget("nested")) {
                MouseEvent event;
                event.is_down = true;
                event.phase = MousePhase::press;
                event.position = {5, 5};
                event.window_position = {205, 25};
                // This is an intentional nested bridge delivery, not a second
                // platform hit-test walk. The outer pointer dispatch already
                // owns the platform DOM token, so the historical
                // on_mouse_event seam correctly suppresses re-entry.
                if (nested->on_dom_pointer_event)
                    nested->on_dom_pointer_event(event, true);
            }
            return choc::value::Value();
        });

    bridge.load_script(R"JS(
        var nested_absent_during_outer = false;
        var outer_absent_during_outer = false;
        createLabel('outer', 'Outer', '');
        createLabel('nested', 'Nested', '');
        on('nested', 'pointerdown', function () {
            removeWidget('nested');
            nested_absent_during_outer =
                getComputedValue('nested', 'display') === '';
        });
        on('outer', 'pointermove', function () {
            dispatchNestedPointer();
            removeWidget('outer');
            outer_absent_during_outer =
                getComputedValue('outer', 'display') === '';
        });
        registerPointer('outer');
        registerPointer('nested');
    )JS");

    bridge.widget("outer")->set_bounds({10, 10, 120, 100});
    bridge.widget("nested")->set_bounds({180, 10, 120, 100});
    root.simulate_drag({30, 30}, {80, 70}, 4);

    REQUIRE(engine.evaluate("nested_absent_during_outer").getWithDefault(false));
    REQUIRE(engine.evaluate("outer_absent_during_outer").getWithDefault(false));
    REQUIRE(bridge.widget("outer") == nullptr);
    REQUIRE(bridge.widget("nested") == nullptr);
    REQUIRE(root.child_count() == 0);
}

TEST_CASE("pointermove self-removal remains stable across remounts",
          "[view][bridge][pointer][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var stress_removals = 0;
    )JS");

    constexpr int kIterations = 128;
    for (int i = 0; i < kIterations; ++i) {
        bridge.load_script(R"JS(
            createLabel('thrower', 'Throw', '');
            on('thrower', 'pointermove', function () {
                stress_removals += 1;
                removeWidget('thrower');
            });
            registerPointer('thrower');
        )JS");
        auto* thrower = bridge.widget("thrower");
        REQUIRE(thrower != nullptr);
        thrower->set_bounds({10, 10, 100, 80});
        root.simulate_drag({20, 20}, {60, 50}, 4);
        REQUIRE(bridge.widget("thrower") == nullptr);
        REQUIRE(root.child_count() == 0);
    }

    REQUIRE(engine.evaluate("stress_removals").getWithDefault<int>(0) ==
            kIterations);
}

TEST_CASE("DOM removal shares callback-safe retirement",
          "[view][bridge][pointer][lifetime][dom]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        createLabel('dom-remover', 'Remove', '');
        on('dom-remover', 'pointermove', function () {
            __domRemove('dom-remover');
        });
        registerPointer('dom-remover');
    )JS");

    auto* remover = bridge.widget("dom-remover");
    REQUIRE(remover != nullptr);
    const auto removed_lifetime = remover->import_binding_lifetime_token();
    remover->set_bounds({10, 10, 100, 80});
    root.simulate_drag({20, 20}, {60, 50}, 4);

    REQUIRE(bridge.widget("dom-remover") == nullptr);
    REQUIRE(root.child_count() == 0);
    REQUIRE_FALSE(removed_lifetime.expired());

    bridge.load_script(R"JS(
        createLabel('collector', 'Collect', '');
        on('collector', 'pointerdown', function () {});
        registerPointer('collector');
    )JS");
    bridge.widget("collector")->set_bounds({10, 10, 100, 80});
    root.simulate_click({20, 20});
    REQUIRE(removed_lifetime.expired());
}

TEST_CASE("throwing pointer dispatch still releases deferred widgets",
          "[view][bridge][pointer][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        createLabel('thrower', 'Throw', '');
        registerPointer('thrower');
        var normalDispatch = __dispatch__;
        __dispatch__ = function (id, type, payload) {
            if (id === 'thrower' && type === 'pointermove') {
                removeWidget('thrower');
                throw new Error('expected pointer handler failure');
            }
            return normalDispatch(id, type, payload);
        };
    )JS");

    auto* thrower = bridge.widget("thrower");
    REQUIRE(thrower != nullptr);
    const auto removed_lifetime = thrower->import_binding_lifetime_token();
    thrower->set_bounds({10, 10, 100, 80});
    root.simulate_drag({20, 20}, {60, 50}, 4);

    REQUIRE(bridge.widget("thrower") == nullptr);
    REQUIRE(root.child_count() == 0);
    REQUIRE_FALSE(removed_lifetime.expired());

    bridge.load_script(R"JS(
        createLabel('collector', 'Collect', '');
        on('collector', 'pointerdown', function () {});
        registerPointer('collector');
    )JS");
    bridge.widget("collector")->set_bounds({10, 10, 100, 80});
    root.simulate_click({20, 20});
    REQUIRE(removed_lifetime.expired());
}
