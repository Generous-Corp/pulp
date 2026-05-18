// pulp #2128 follow-up — pin WidgetBridge::dispatch_document_event +
// document.addEventListener fan-out.
//
// Architecture in brief:
//
//   React popovers commonly close via the `document.addEventListener
//   ('mousedown', onDoc)` click-outside idiom. Pre-fix, Pulp's JS
//   `document.addEventListener` was a NO-OP (pulp #2101, kept that way
//   to silence Three.js OrbitControls cleanup). With the no-op, every
//   React popover using click-outside was silently dead.
//
//   This test pins two contracts:
//   1. `document.addEventListener('mousedown', fn)` actually registers
//      the handler; `dispatchEvent` fires it.
//   2. `WidgetBridge::dispatch_document_event(type, jsonLiteral)` fans
//      out to every live bridge — platform hosts (window_host_mac.mm
//      Esc handler) use this to fire synthetic outside-click events on
//      Esc so popovers close without per-app wiring.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/state/store.hpp>

TEST_CASE("document.addEventListener is real (not a no-op)",
          "[view][widget-bridge][esc-dismiss][2128]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var docEvents = [];
        document.addEventListener('mousedown', function(e) {
            docEvents.push('m:' + e.clientX + ',' + e.clientY);
        });
        document.addEventListener('pointerdown', function(e) {
            docEvents.push('p:' + e.clientX + ',' + e.clientY);
        });
        function docCount() { return docEvents.length; }
        function docAt(i) { return docEvents[i] || ''; }
        function dispatchDoc(type, x, y) {
            document.dispatchEvent({ type: type, clientX: x, clientY: y });
        }
    )JS");

    auto count = [&] { return engine.evaluate("docCount()").getWithDefault<int>(-1); };

    REQUIRE(count() == 0);

    // Direct dispatchEvent reaches the registered listener.
    engine.evaluate("dispatchDoc('mousedown', 100, 200)");
    REQUIRE(count() == 1);
    REQUIRE(engine.evaluate("docAt(0)").toString() == "m:100,200");

    engine.evaluate("dispatchDoc('pointerdown', -1, -1)");
    REQUIRE(count() == 2);
    REQUIRE(engine.evaluate("docAt(1)").toString() == "p:-1,-1");

    // removeEventListener actually removes the handler (Three.js cleanup
    // contract is preserved — no throw on unknown handler either).
    bridge.load_script(R"JS(
        var firstHandler = document.__eventListeners__.mousedown[0];
        document.removeEventListener('mousedown', firstHandler);
        function mousedownCount() {
            return (document.__eventListeners__.mousedown || []).length;
        }
        // Unknown handler — must not throw.
        document.removeEventListener('mousedown', function() {});
        document.removeEventListener('totally-unknown', function() {});
    )JS");
    REQUIRE(engine.evaluate("mousedownCount()").getWithDefault<int>(-1) == 0);
}

TEST_CASE("WidgetBridge::dispatch_document_event fans out to every live bridge",
          "[view][widget-bridge][esc-dismiss][2128]") {
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

    auto install = [](WidgetBridge& b) {
        b.load_script(R"JS(
            var docHits = 0;
            var lastX = null, lastY = null;
            document.addEventListener('mousedown', function(e) {
                docHits++; lastX = e.clientX; lastY = e.clientY;
            });
            function docHits_fn() { return docHits; }
            function lastXY() { return lastX + ',' + lastY; }
        )JS");
    };
    install(bridge_a);
    install(bridge_b);

    auto a_hits = [&] { return engine_a.evaluate("docHits_fn()").getWithDefault<int>(-1); };
    auto b_hits = [&] { return engine_b.evaluate("docHits_fn()").getWithDefault<int>(-1); };

    REQUIRE(a_hits() == 0);
    REQUIRE(b_hits() == 0);

    // ONE static call delivers to BOTH bridges — same pattern as
    // dispatch_global_key. Coords -1,-1 are the sentinel platform
    // hosts use on Esc to mean "outside every real bounding box".
    WidgetBridge::dispatch_document_event(
        "mousedown", "{clientX:-1,clientY:-1,target:null}");

    REQUIRE(a_hits() == 1);
    REQUIRE(b_hits() == 1);
    REQUIRE(engine_a.evaluate("lastXY()").toString() == "-1,-1");
    REQUIRE(engine_b.evaluate("lastXY()").toString() == "-1,-1");
}

TEST_CASE("dispatch_document_event closes a Spectr-PickerDropdown-style click-outside listener",
          "[view][widget-bridge][esc-dismiss][2128][regression-spectr]") {
    // Mirrors Spectr's PickerDropdown (spectr-editor-extracted.js:3401):
    //
    //   React.useEffect(() => {
    //       if (!open) return;
    //       const onDoc = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(false); };
    //       document.addEventListener('mousedown', onDoc);
    //       return () => document.removeEventListener('mousedown', onDoc);
    //   }, [open]);
    //
    // We can't run React here, so simulate the effect's outcome: the
    // listener is attached when open=true. When the platform's Esc
    // handler fires a synthetic document.mousedown at coords (-1, -1)
    // with target=null, `ref.current.contains(null)` must return false
    // so the !contains check resolves true and setOpen(false) runs.
    //
    // If this test fails, the bug is somewhere in the chain
    // (preamble fan-out, document.dispatchEvent impl, or Element.contains
    // semantics). If it passes but Spectr's real dropdown doesn't
    // close, the bug is upstream — React-effect timing in @pulp/react,
    // esbuild rewriting `document`, or host-shims clobbering the
    // listener — and we can diagnose there.

    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var open = true;
        var closeCount = 0;
        // Use the bridge's Element.prototype.contains via a fake "ref"
        // that mirrors what React.useRef returns: { current: <Element> }.
        // We construct an Element directly using document.createElement
        // so Element.prototype.contains is exercised end-to-end.
        var refCurrent = document.createElement('div');
        var onDoc = function(e) {
            if (refCurrent && !refCurrent.contains(e.target)) {
                closeCount++;
                open = false;
            }
        };
        document.addEventListener('mousedown', onDoc);
        function isOpen() { return open ? 1 : 0; }
        function closes() { return closeCount; }
    )JS");

    auto is_open = [&] { return engine.evaluate("isOpen()").getWithDefault<int>(-1); };
    auto closes  = [&] { return engine.evaluate("closes()").getWithDefault<int>(-1); };

    REQUIRE(is_open() == 1);
    REQUIRE(closes() == 0);

    // Simulate Esc → window_host_mac.mm's dispatch_document_event call.
    WidgetBridge::dispatch_document_event(
        "mousedown", "{clientX:-1,clientY:-1,target:null}");

    REQUIRE(closes() == 1);
    REQUIRE(is_open() == 0);
}

TEST_CASE("dispatch_document_event survives bridge destruction mid-fan-out",
          "[view][widget-bridge][esc-dismiss][2128]") {
    // Same auto-unregister contract as dispatch_global_key — a bridge
    // that goes out of scope before the next fan-out must not leave a
    // dangling pointer in the static registry.
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine_keep;
    View root_keep;
    StateStore store_keep;
    WidgetBridge keeper(engine_keep, root_keep, store_keep);
    keeper.load_script(R"JS(
        var hits = 0;
        document.addEventListener('mousedown', function() { hits++; });
        function H() { return hits; }
    )JS");
    auto keeper_hits = [&] { return engine_keep.evaluate("H()").getWithDefault<int>(-1); };

    {
        ScriptEngine engine_eph;
        View root_eph;
        StateStore store_eph;
        WidgetBridge ephemeral(engine_eph, root_eph, store_eph);
        ephemeral.load_script("var h = 0; document.addEventListener('mousedown', function(){h++;});");

        WidgetBridge::dispatch_document_event(
            "mousedown", "{clientX:-1,clientY:-1,target:null}");
        REQUIRE(keeper_hits() == 1);
    }  // ephemeral's dtor unregisters

    // Must not crash on a freed bridge.
    WidgetBridge::dispatch_document_event(
        "mousedown", "{clientX:-1,clientY:-1,target:null}");
    REQUIRE(keeper_hits() == 2);
}
