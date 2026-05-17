// Programmatic verification of the design-tool platform key wire-up
// (pulp #2128 follow-up). The macOS PulpView normally calls
// `rootView->on_global_key(KeyEvent{...})` for every keypress.  Without
// the wire-up in main.cpp, `on_global_key` is null and the bridge's
// shortcut + __dispatch__('__global__','keydown') paths are dead.
//
// This test stages the same arrangement main.cpp builds (View + Engine
// + Bridge + the lambda hook), then fires the synthetic `on_global_key`
// the platform host would fire, and asserts the bridge dispatched the
// keydown to `window.addEventListener('keydown', ...)` listeners — which
// is what Spectr's mode-switch handler at editor.generated.tsx:3753
// relies on.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/state/store.hpp>
#include <fstream>
#include <sstream>

TEST_CASE("design-tool platform key wire-up fires registerShortcut + __global__ keydown",
          "[design-tool][platform][keyboard][wireup]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 1280, 800});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // *** This is the wire-up under test — same lambda main.cpp installs. ***
    root.on_global_key = [&bridge](const KeyEvent& e) -> bool {
        bridge.forward_key_event(static_cast<int>(e.key), e.modifiers, e.is_down);
        return false;
    };

    // Match Spectr's pattern: a bare-key handler + a Cmd-chord handler,
    // both subscribed via window.addEventListener('keydown', ...).
    bridge.load_script(R"JS(
        var events = [];
        window.addEventListener('keydown', function(e) {
            events.push(e.key + ':ctrl=' + (!!e.ctrlKey) + ':meta=' + (!!e.metaKey));
        });

        // Also register a native shortcut callback like V2/Phase A emit:
        var settings_opened = 0;
        function openSettings() { settings_opened++; }
        registerShortcut(44, 16, 'openSettings');  // Cmd+, → kModCmd mask

        function event_count() { return events.length; }
        function event_at(i)   { return events[i] || ''; }
        function settings_count() { return settings_opened; }
    )JS");

    auto count = [&]() {
        return engine.evaluate("event_count()").getWithDefault<int>(-1);
    };
    auto at = [&](int i) {
        return engine.evaluate("event_at(" + std::to_string(i) + ")").toString();
    };
    auto settings = [&]() {
        return engine.evaluate("settings_count()").getWithDefault<int>(-1);
    };

    REQUIRE(count() == 0);
    REQUIRE(settings() == 0);

    // *** Simulate what PulpView::keyDown: would do post-wireup. ***
    SECTION("bare S key — Spectr-style mode-switch shortcut") {
        KeyEvent e;
        e.key = static_cast<KeyCode>('s');
        e.modifiers = 0;
        e.is_down = true;
        root.on_global_key(e);

        REQUIRE(count() == 1);
        REQUIRE(at(0) == "s:ctrl=false:meta=false");
    }

    SECTION("Cmd+, — what performKeyEquivalent: now routes through on_global_key") {
        KeyEvent e;
        e.key = static_cast<KeyCode>(',');
        e.modifiers = kModCmd;
        e.is_down = true;
        root.on_global_key(e);

        // When registerShortcut(44, kModCmd) matches, the bridge invokes
        // the registered callback (openSettings) but does NOT fall through
        // to window.addEventListener — the chord is "consumed" by the
        // native shortcut. The window-level fan-out only happens because
        // the codegen-emitted thunk explicitly re-dispatches via
        // __dispatch__('__global__','keydown',...). That re-dispatch path
        // is covered end-to-end below in the imported-ui.js TEST_CASE.
        REQUIRE(count() == 0);
        REQUIRE(settings() == 1);
    }

    SECTION("Escape — bare-key V1 extracted path") {
        KeyEvent e;
        e.key = KeyCode::escape;
        e.modifiers = 0;
        e.is_down = true;
        root.on_global_key(e);

        REQUIRE(count() == 1);
        REQUIRE(at(0) == "Escape:ctrl=false:meta=false");
    }

    SECTION("key-up is forwarded but is_down=false — listeners can filter") {
        KeyEvent up;
        up.key = static_cast<KeyCode>('s');
        up.modifiers = 0;
        up.is_down = false;
        root.on_global_key(up);
        // forward_key_event currently early-returns for is_down=false, so
        // no dispatch is expected. Pin that here so a future change has
        // to make the decision deliberately.
        REQUIRE(count() == 0);
    }
}

// The E2E test that loaded a Phase-A-generated ui.js + simulated
// platform key presses lives on #2128's branch (it needs
// `detect_default_shortcuts` / `apply_default_shortcuts` / `TargetPlatform`
// from the Phase A defaults work). This PR ships the platform wire-up
// in isolation so it can merge ahead of #2128. The first TEST_CASE
// above already exercises the wire-up directly via `registerShortcut`
// + `window.addEventListener` — no Phase-A symbol dependency. The
// imported-ui.js E2E will be re-added on top of this PR once #2128
// merges.
