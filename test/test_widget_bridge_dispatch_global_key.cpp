// pulp #2128 follow-up — pin WidgetBridge::dispatch_global_key.
//
// Architecture in brief:
//
//   Every live WidgetBridge auto-registers itself in a static set in its
//   ctor (and unregisters in its dtor). The macOS platform host
//   (window_host_mac.mm) calls WidgetBridge::dispatch_global_key from
//   both keyDown: and performKeyEquivalent:, fanning the event out to
//   every live bridge's forward_key_event. That delivers:
//
//     1. `registerShortcut(key, mods, callback)` matches → JS callback fires.
//     2. Unconditional `__dispatch__('__global__', 'keydown', {...})` →
//        `window.addEventListener('keydown', ...)` listeners fire.
//
//   This means any app that uses @pulp/react / WidgetBridge (Spectr,
//   design-tool, any future Pulp standalone) gets keyboard delivery
//   without each app having to install its own View::on_global_key
//   lambda.
//
// What this test pins:
//
//   - Two live bridges → one dispatch_global_key call hits both.
//   - A bridge that goes out of scope is automatically removed from
//     the set (the static set's correctness depends on dtor cleanup;
//     a leaked entry would crash on the next dispatch).
//   - The dispatched key actually reaches a JS-registered shortcut
//     callback and a window.addEventListener('keydown',...) listener.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/state/store.hpp>

TEST_CASE("WidgetBridge::dispatch_global_key fans out to every live bridge",
          "[view][widget-bridge][keyboard][wireup][2128]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine_a;
    View root_a;
    StateStore store_a;
    WidgetBridge bridge_a(engine_a, root_a, store_a);

    ScriptEngine engine_b;
    View root_b;
    StateStore store_b;
    WidgetBridge bridge_b(engine_b, root_b, store_b);

    // Each bridge installs its own listener so we can distinguish
    // delivery to bridge A from delivery to bridge B.
    bridge_a.load_script(R"JS(
        var aEvents = [];
        window.addEventListener('keydown', function(e) {
            aEvents.push(e.key + '|' + (e.metaKey?'M':'-'));
        });
        var aSettings = 0;
        function openSettingsA() { aSettings++; }
        registerShortcut(44, 16, 'openSettingsA');  // Cmd+,
        function aEventCount() { return aEvents.length; }
        function aEventAt(i) { return aEvents[i] || ''; }
        function aSettingsCount() { return aSettings; }
    )JS");

    bridge_b.load_script(R"JS(
        var bEvents = [];
        window.addEventListener('keydown', function(e) {
            bEvents.push(e.key + '|' + (e.metaKey?'M':'-'));
        });
        function bEventCount() { return bEvents.length; }
        function bEventAt(i) { return bEvents[i] || ''; }
    )JS");

    auto a_count = [&] { return engine_a.evaluate("aEventCount()").getWithDefault<int>(-1); };
    auto a_settings = [&] { return engine_a.evaluate("aSettingsCount()").getWithDefault<int>(-1); };
    auto b_count = [&] { return engine_b.evaluate("bEventCount()").getWithDefault<int>(-1); };

    REQUIRE(a_count() == 0);
    REQUIRE(a_settings() == 0);
    REQUIRE(b_count() == 0);

    // ── Two checks in one fan-out ─────────────────────────────────────
    // Cmd+, dispatched once. Bridge A has a registerShortcut(44, kModCmd)
    // that matches → openSettingsA() fires AND consumes the dispatch (no
    // window listener fan-out from A). Bridge B has no matching shortcut
    // so it falls through to its window.addEventListener listener with
    // the W3C-escaped key string + metaKey:true.
    WidgetBridge::dispatch_global_key(',', kModCmd, /*is_down=*/true);

    REQUIRE(a_settings() == 1);
    REQUIRE(a_count() == 0);
    REQUIRE(b_count() == 1);
    // ',' (ASCII 44) is not in keycode_to_w3c_key's mapped set (only
    // letters/digits + named keys), so the bridge emits "Unidentified".
    // The actual ui.js codegen path emits explicit thunks that
    // re-dispatch with a known key string — that route is covered by
    // pulp-test-platform-key-wireup's E2E TEST_CASE. Here we pin only
    // the fan-out + modifier propagation contract.
    REQUIRE(engine_b.evaluate("bEventAt(0)").toString() == "Unidentified|M");

    // Now fire a mapped key ('s') to confirm string translation works
    // and that a SECOND dispatch lands on each bridge independently.
    WidgetBridge::dispatch_global_key('s', 0, /*is_down=*/true);
    REQUIRE(a_settings() == 1);  // unchanged — no 's' shortcut on A
    REQUIRE(a_count() == 1);     // A's window listener fires (no shortcut match)
    REQUIRE(b_count() == 2);
    REQUIRE(engine_b.evaluate("bEventAt(1)").toString() == "s|-");
}

TEST_CASE("WidgetBridge auto-unregisters from all_bridges_ on destruction",
          "[view][widget-bridge][keyboard][wireup][2128]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine_keep;
    View root_keep;
    StateStore store_keep;
    WidgetBridge keeper(engine_keep, root_keep, store_keep);

    keeper.load_script(R"JS(
        var events = [];
        window.addEventListener('keydown', function(e) { events.push(e.key); });
        function evCount() { return events.length; }
    )JS");
    auto keeper_count = [&] {
        return engine_keep.evaluate("evCount()").getWithDefault<int>(-1);
    };

    // Inner scope: an ephemeral bridge that must auto-unregister.
    {
        ScriptEngine engine_eph;
        View root_eph;
        StateStore store_eph;
        WidgetBridge ephemeral(engine_eph, root_eph, store_eph);
        ephemeral.load_script(R"JS(
            var ephEvents = [];
            window.addEventListener('keydown', function(e) { ephEvents.push(e.key); });
            function ephCount() { return ephEvents.length; }
        )JS");

        WidgetBridge::dispatch_global_key('a', 0, /*is_down=*/true);

        REQUIRE(keeper_count() == 1);
        REQUIRE(engine_eph.evaluate("ephCount()").getWithDefault<int>(-1) == 1);
    }  // ephemeral's dtor must remove it from all_bridges_ here.

    // Post-destruction: dispatch must not crash on a freed bridge.
    // If the dtor failed to unregister, this would invoke
    // forward_key_event on a dangling pointer — UB, but most often a
    // crash under ASan or a wrong-result under release.
    WidgetBridge::dispatch_global_key('b', 0, /*is_down=*/true);
    REQUIRE(keeper_count() == 2);
}

TEST_CASE("dispatch_global_key respects is_down filtering at the per-bridge level",
          "[view][widget-bridge][keyboard][wireup][2128]") {
    // forward_key_event early-returns on key-up to match the existing
    // contract (only keydown dispatches). dispatch_global_key forwards
    // is_down as-is, so a key-up dispatch reaches every bridge but
    // each one drops it. Pin that contract here so a future change to
    // the early-return has to be deliberate.
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var events = [];
        window.addEventListener('keydown', function(e) { events.push(e.key); });
        function count() { return events.length; }
    )JS");
    auto count = [&] { return engine.evaluate("count()").getWithDefault<int>(-1); };

    WidgetBridge::dispatch_global_key('s', 0, /*is_down=*/false);
    REQUIRE(count() == 0);

    WidgetBridge::dispatch_global_key('s', 0, /*is_down=*/true);
    REQUIRE(count() == 1);
}
