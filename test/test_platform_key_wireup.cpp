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

// End-to-end: load the ACTUAL ui.js the importer just emitted (Phase A
// defaults + V1 extracted) into a fresh bridge, mount a Spectr-style
// window listener, simulate platform key presses, verify the right
// dispatches reach the listener. This validates the WHOLE pipeline:
//
//   pulp-import-design --from v0 --file source.tsx
//     -> emits registerShortcut + __pulpShortcutHandler_N thunks
//     -> generates per-platform default chords (Cmd+, AND Ctrl+,)
//   pulp-design-tool's platform key wire-up (under test)
//     -> root.on_global_key(KeyEvent{...})
//     -> bridge.forward_key_event(...)
//     -> matched shortcut -> thunk -> __dispatch__('__global__','keydown',{...})
//     -> window.addEventListener('keydown', ...) listener
//     -> records what fired
//
// If this passes, all 4 Phase A defaults (Settings × {mac,win}, Help ×
// {mac,win}) plus the Escape extracted shortcut fire as the imported
// script intends in the live tool — no need to drive physical keys.

TEST_CASE("E2E: imported ui.js + platform wire-up — defaults actually fire",
          "[design-tool][platform][keyboard][wireup][e2e]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    // Synthesize a Spectr-like source (canonical SettingsModal +
    // HelpPopover names + extracted Escape) and pipe through the same
    // codegen path the CLI uses.
    const char* source = R"JS(
        export function SettingsModal() {
            return <div role="dialog"><h1>Settings</h1></div>;
        }
        export function HelpPopover() {
            return <div role="dialog"><h1>Help</h1></div>;
        }
        export function Editor() {
            useEffect(() => {
                const onKey = (e) => {
                    if (e.key === 'Escape') closeAll();
                };
                window.addEventListener('keydown', onKey);
            }, []);
            return <div />;
        }
    )JS";

    auto extracted = extract_keyboard_shortcuts(source, "test.tsx");
    auto scan = detect_default_shortcuts(source, extracted);
    auto mac_defaults = apply_default_shortcuts(scan.accepted, TargetPlatform::macos);
    auto win_defaults = apply_default_shortcuts(scan.accepted, TargetPlatform::win_linux);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    opts.shortcuts = extracted;
    for (auto& d : mac_defaults) opts.shortcuts.push_back(std::move(d));
    for (auto& d : win_defaults) {
        bool dup = false;
        for (auto& e : opts.shortcuts) {
            if (e.key == d.key && e.modifiers == d.modifiers) { dup = true; break; }
        }
        if (!dup) opts.shortcuts.push_back(std::move(d));
    }
    DesignIR ir;
    std::string ui_js = generate_pulp_js(ir, opts);
    REQUIRE(ui_js.find("registerShortcut(44, 16,") != std::string::npos); // Cmd+,
    REQUIRE(ui_js.find("registerShortcut(44, 2,")  != std::string::npos); // Ctrl+,
    REQUIRE(ui_js.find("registerShortcut(63, 16,") != std::string::npos); // Cmd+?
    REQUIRE(ui_js.find("registerShortcut(290, 0,") != std::string::npos); // F1
    REQUIRE(ui_js.find("registerShortcut(274, 0,") != std::string::npos); // Escape

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 1280, 800});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Platform wire-up — the lambda main.cpp now installs.
    root.on_global_key = [&bridge](const KeyEvent& e) -> bool {
        bridge.forward_key_event(static_cast<int>(e.key), e.modifiers, e.is_down);
        return false;
    };

    // Mount a window-level listener BEFORE the import script runs —
    // mirrors Spectr's editor.tsx pattern.
    bridge.load_script(R"JS(
        var fired = [];
        window.addEventListener('keydown', function(e) {
            fired.push(e.key + '|' + (e.ctrlKey?'C':'-') + (e.metaKey?'M':'-'));
        });
        function fired_count() { return fired.length; }
        function fired_at(i)   { return fired[i] || ''; }
    )JS");

    // Now load the imported ui.js — register the native chord intercepts.
    bridge.load_script(ui_js);

    auto count   = [&]() { return engine.evaluate("fired_count()").getWithDefault<int>(-1); };
    auto fired   = [&](int i) { return engine.evaluate("fired_at(" + std::to_string(i) + ")").toString(); };

    REQUIRE(count() == 0);

    auto press = [&](char k, uint16_t mods) {
        KeyEvent e;
        e.key = static_cast<KeyCode>(k);
        e.modifiers = mods;
        e.is_down = true;
        root.on_global_key(e);
    };
    auto press_named = [&](KeyCode k, uint16_t mods) {
        KeyEvent e;
        e.key = k;
        e.modifiers = mods;
        e.is_down = true;
        root.on_global_key(e);
    };

    // 1. Cmd+, — Phase A Settings default (macOS chord)
    press(',', kModCmd);
    REQUIRE(count() == 1);
    REQUIRE(fired(0) == ",|-M");

    // 2. Ctrl+, — Phase A Settings default (Win/Linux variant, fires on Mac too)
    press(',', kModCtrl);
    REQUIRE(count() == 2);
    REQUIRE(fired(1) == ",|C-");

    // 3. Cmd+? — Phase A Help default (macOS)
    press('?', kModCmd);
    REQUIRE(count() == 3);
    REQUIRE(fired(2) == "?|-M");

    // 4. F1 — Phase A Help default (Win/Linux variant)
    press_named(KeyCode::f1, 0);
    REQUIRE(count() == 4);
    REQUIRE(fired(3) == "F1|--");

    // 5. Escape — V1 extracted from source
    press_named(KeyCode::escape, 0);
    REQUIRE(count() == 5);
    REQUIRE(fired(4) == "Escape|--");
}
