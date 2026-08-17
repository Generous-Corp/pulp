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

// ── Engine lifetime ───────────────────────────────────────────────────────
//
// WidgetBridge holds `ScriptEngine&`: the engine is owned by the HOST, and
// every deferred native callback the bridge installs captures a raw
// `ScriptEngine*`. The callback-state flag those callbacks check is flipped by
// ~WidgetBridge, so it proves only that the BRIDGE is still standing. Nothing
// in the type system stops a host from destroying the engine first, and when it
// does, the flag still reads true while the captured pointer dangles — the next
// event dereferences recycled storage (ScriptEngine::operator bool() reads a
// unique_ptr member and calls a virtual through it).

TEST_CASE("a queued event after the engine dies is an inert no-op",
          "[view][bridge][lifetime][engine]") {
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    auto engine = std::make_unique<ScriptEngine>();
    auto bridge = std::make_unique<WidgetBridge>(*engine, root, store);

    bridge->load_script(R"JS(
        var hits = 0;
        createLabel('btn', 'Btn', '');
        on('btn', 'click', function () { hits += 1; });
        registerClick('btn');
    )JS");

    auto* btn = bridge->widget("btn");
    REQUIRE(btn != nullptr);
    btn->set_bounds({10, 10, 100, 80});

    // Control: the same click dispatches normally while the engine is alive, so
    // a pass below cannot come from the event never reaching the callback.
    root.simulate_click({20, 20});
    REQUIRE(engine->evaluate("hits").getWithDefault<int>(0) == 1);

    // Host teardown in the order the bridge cannot control. The widget tree and
    // its installed on_click closure stay live under `root`.
    engine.reset();

    // Must be an inert no-op. Before the engine-liveness guard this reached
    // `static_cast<bool>(*engine)` on freed storage.
    root.simulate_click({20, 20});

    REQUIRE(bridge->widget("btn") != nullptr);
    REQUIRE(root.child_count() == 1);
}

TEST_CASE("the callback guard covers a destroyed engine and not only a torn-down bridge",
          "[view][bridge][lifetime][engine]") {
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    auto engine = std::make_unique<ScriptEngine>();
    WidgetBridge bridge(*engine, root, store);

    bridge.load_script(R"JS(
        var hits = 0;
        createLabel('btn', 'Btn', '');
        on('btn', 'click', function () { hits += 1; });
        registerClick('btn');
    )JS");
    auto* btn = bridge.widget("btn");
    REQUIRE(btn != nullptr);
    btn->set_bounds({10, 10, 100, 80});

    engine.reset();

    // The bridge is untouched — its own alive flag is still true — so the only
    // thing that can suppress this dispatch is engine liveness.
    REQUIRE(bridge.widget("btn") == btn);
    root.simulate_click({20, 20});
    REQUIRE(root.child_count() == 1);
}

// ── Overlay dismiss ───────────────────────────────────────────────────────

TEST_CASE("an overlay dismiss handler may release the overlay from inside itself",
          "[view][bridge][lifetime][overlay]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // The React shape this models: `onDismissed` flips setOpen(false), the
    // popover unmounts, and the unmount calls releaseOverlay(id) — which
    // assigns `on_overlay_dismissed = nullptr` and frees the closure whose body
    // is still on the stack. dismiss_active_overlay() must hold its own copy.
    bridge.load_script(R"JS(
        var dismissed = 0;
        createLabel('pop', 'Pop', '');
        on('pop', 'dismiss', function () {
            dismissed += 1;
            releaseOverlay('pop');
        });
        claimOverlay('pop');
    )JS");

    auto* pop = bridge.widget("pop");
    REQUIRE(pop != nullptr);
    REQUIRE(View::active_overlay_ == pop);

    View::dismiss_active_overlay();

    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 1);
    REQUIRE(View::active_overlay_ == nullptr);
    // The self-release cleared the slot, so a second dismiss is inert rather
    // than re-entering the freed closure.
    View::dismiss_active_overlay();
    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 1);
}

TEST_CASE("an overlay dismiss handler may re-claim the overlay from inside itself",
          "[view][bridge][lifetime][overlay]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 120});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Re-claiming from inside the handler ASSIGNS a new std::function to
    // `on_overlay_dismissed`, which destroys the closure whose body is
    // currently executing and frees its out-of-line heap block. This is the
    // reentrancy shape a React popover produces when `onDismissed` re-opens.
    bridge.load_script(R"JS(
        var dismissed = 0;
        createLabel('pop', 'Pop', '');
        on('pop', 'dismiss', function () {
            dismissed += 1;
            claimOverlay('pop');
        });
        claimOverlay('pop');
    )JS");

    auto* pop = bridge.widget("pop");
    REQUIRE(pop != nullptr);
    REQUIRE(View::active_overlay_ == pop);

    View::dismiss_active_overlay();
    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 1);
    // The handler re-claimed, so the slot points at the replacement closure.
    REQUIRE(View::active_overlay_ == pop);

    // The second dismiss must reach the REPLACEMENT, not the block the first
    // dismiss freed out from under itself.
    View::dismiss_active_overlay();
    REQUIRE(engine.evaluate("dismissed").getWithDefault<int>(0) == 2);
    REQUIRE(View::active_overlay_ == pop);
}
