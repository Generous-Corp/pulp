// Tests for `extract_keyboard_shortcuts` + `serialize_detected_shortcuts`
// in design_import.cpp. Verifies the static-scan path on representative
// React patterns (inline JSX onKeyDown, window/document.addEventListener
// keydown, modifier combinations) lifted from the Spectr editor source.
//
// The extractor is regex-driven and lexical only — it does NOT evaluate
// handler bodies or resolve dynamic key references. Tests pin the
// recognized forms + verify de-dup + verify modifier normalization
// (metaKey || ctrlKey collapses to "meta", per the cross-platform idiom).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/input_events.hpp>
#include <algorithm>

using pulp::view::CodeGenMode;
using pulp::view::CodeGenOptions;
using pulp::view::DesignIR;
using pulp::view::DetectedShortcut;
using pulp::view::extract_keyboard_shortcuts;
using pulp::view::generate_pulp_js;
using pulp::view::key_string_to_keycode;
using pulp::view::modifier_strings_to_mask;
using pulp::view::serialize_detected_shortcuts;

TEST_CASE("extract_keyboard_shortcuts finds bare e.key === literal", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const onKey = (e) => { if (e.key === 'Escape') onClose(); };
            window.addEventListener('keydown', onKey);
        )JS", "editor.tsx");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "Escape");
    REQUIRE(out[0].modifiers.empty());
    REQUIRE(out[0].source_location.find("editor.tsx:") == 0);
    REQUIRE(out[0].handler_excerpt.find("onClose") != std::string::npos);
}

TEST_CASE("extract_keyboard_shortcuts handles inline onKeyDown JSX", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            onKeyDown={e => { if (e.key === 'Enter') e.target.blur();
                              if (e.key === 'Escape') setEditName(false); }}
        )JS", "");
    REQUIRE(out.size() == 2);
    // Sorted by (key, modifiers) — Enter comes before Escape.
    REQUIRE(out[0].key == "Enter");
    REQUIRE(out[1].key == "Escape");
}

TEST_CASE("extract_keyboard_shortcuts captures meta + ctrl separately", "[design-import][shortcuts]") {
    // Codex P1 review on #2119: the cross-platform `metaKey || ctrlKey`
    // idiom now yields BOTH "meta" AND "ctrl" modifiers. generate_pulp_js
    // emits a separate registerShortcut for each physical chord so a user
    // can hit Cmd+S on macOS or Ctrl+S on Win/Linux and the synthetic
    // event carries the right modifier flags. Previously the extractor
    // collapsed to a single "meta", which broke Ctrl-only handlers.
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const onKey = (e) => {
                if ((e.metaKey || e.ctrlKey) && e.key === 's') save();
            };
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "s");
    REQUIRE(out[0].modifiers.size() == 2);
    auto has = [&](const std::string& m) {
        return std::find(out[0].modifiers.begin(), out[0].modifiers.end(), m)
            != out[0].modifiers.end();
    };
    REQUIRE(has("meta"));
    REQUIRE(has("ctrl"));
}

TEST_CASE("extract_keyboard_shortcuts captures Ctrl-only modifier", "[design-import][shortcuts]") {
    // Codex P1 case from #2119: `e.ctrlKey && e.key === 's'` is a
    // Win/Linux-only Ctrl+S binding. Pre-fix the extractor renamed it to
    // "meta" and V2 codegen emitted Cmd-only registerShortcut + a synthetic
    // event with `ctrlKey: false` — so the source handler never fired.
    auto out = extract_keyboard_shortcuts(
        R"JS(
            if (e.ctrlKey && e.key === 's') save();
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "s");
    REQUIRE(out[0].modifiers.size() == 1);
    REQUIRE(out[0].modifiers[0] == "ctrl");
}

TEST_CASE("extract_keyboard_shortcuts captures Meta-only modifier", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            if (e.metaKey && e.key === 's') save();
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "s");
    REQUIRE(out[0].modifiers.size() == 1);
    REQUIRE(out[0].modifiers[0] == "meta");
}

TEST_CASE("extract_keyboard_shortcuts captures multiple modifiers", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            if (e.shiftKey && e.altKey && e.key === 'F') openFlare();
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "F");
    REQUIRE(out[0].modifiers.size() == 2);
    // collect_modifiers walks the window and adds in fixed order:
    // alt before shift (alphabetical by `add()` order in source).
    auto has = [&](const std::string& m) {
        return std::find(out[0].modifiers.begin(), out[0].modifiers.end(), m)
            != out[0].modifiers.end();
    };
    REQUIRE(has("alt"));
    REQUIRE(has("shift"));
}

TEST_CASE("extract_keyboard_shortcuts recognizes e.code variant", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            if (e.code === 'ArrowLeft') prevTab();
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "ArrowLeft");
}

TEST_CASE("extract_keyboard_shortcuts de-dupes same chord across branches", "[design-import][shortcuts]") {
    // Two checks for `e.key === 'Escape'` in different branches of the same
    // handler should produce one manifest entry, not two — the runtime
    // registers a single shortcut for the chord.
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const onKey = (e) => {
                if (isEditing) { if (e.key === 'Escape') cancelEdit(); }
                else { if (e.key === 'Escape') closeOverlay(); }
            };
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "Escape");
}

TEST_CASE("extract_keyboard_shortcuts returns empty on no match", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const x = 5;
            function helper() { return x * 2; }
        )JS", "");
    REQUIRE(out.empty());
}

TEST_CASE("extract_keyboard_shortcuts tolerates empty / whitespace input", "[design-import][shortcuts]") {
    REQUIRE(extract_keyboard_shortcuts("", "").empty());
    REQUIRE(extract_keyboard_shortcuts("   \n\t\n   ", "").empty());
}

TEST_CASE("serialize_detected_shortcuts emits stable JSON", "[design-import][shortcuts]") {
    std::vector<DetectedShortcut> shortcuts;
    DetectedShortcut a;
    a.key = "Escape";
    a.pattern = "e.key === 'Escape'";
    a.source_location = "editor.tsx:42";
    a.handler_excerpt = "onClose();";
    shortcuts.push_back(a);

    DetectedShortcut b;
    b.key = "s";
    b.modifiers = {"meta"};
    b.pattern = "e.metaKey && e.key === 's'";
    b.source_location = "editor.tsx:128";
    b.handler_excerpt = "save();";
    shortcuts.push_back(b);

    std::string json = serialize_detected_shortcuts(shortcuts);
    REQUIRE(json.find("\"key\": \"Escape\"") != std::string::npos);
    REQUIRE(json.find("\"key\": \"s\"") != std::string::npos);
    // The modifiers array — choc::json may use multi-line indentation, so
    // search for the substring `"meta"` after a `"modifiers":` key rather
    // than asserting an exact bracket-array form.
    auto mods_pos = json.find("\"modifiers\"");
    REQUIRE(mods_pos != std::string::npos);
    REQUIRE(json.find("\"meta\"", mods_pos) != std::string::npos);
    REQUIRE(json.find("\"source_location\": \"editor.tsx:42\"") != std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────
// V2 wire-up tests — helpers + codegen emission
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("key_string_to_keycode maps DOM key names", "[design-import][shortcuts][v2]") {
    REQUIRE(key_string_to_keycode("Escape") == 274);     // KeyCode::escape
    REQUIRE(key_string_to_keycode("escape") == 274);     // case-insensitive
    REQUIRE(key_string_to_keycode("Enter") == 273);
    REQUIRE(key_string_to_keycode("Return") == 273);     // alias
    REQUIRE(key_string_to_keycode("Tab") == 272);
    REQUIRE(key_string_to_keycode("ArrowLeft") == 256);
    REQUIRE(key_string_to_keycode("Left") == 256);       // alias
    REQUIRE(key_string_to_keycode("s") == 's');          // 115
    REQUIRE(key_string_to_keycode("S") == 's');          // upper→lower
    REQUIRE(key_string_to_keycode("F12") == 301);
    REQUIRE(key_string_to_keycode("Space") == ' ');
}

TEST_CASE("key_string_to_keycode returns 0 for unknown", "[design-import][shortcuts][v2]") {
    REQUIRE(key_string_to_keycode("") == 0);
    REQUIRE(key_string_to_keycode("Boop") == 0);
    // Printable ASCII (incl. punctuation) is accepted now; "@" maps to 0x40.
    REQUIRE(key_string_to_keycode("@") == 0x40);
    // Non-printable / non-ASCII still returns 0.
    REQUIRE(key_string_to_keycode(std::string(1, '\x01')) == 0);
}

TEST_CASE("modifier_strings_to_mask combines bits + 'meta' maps to kModCmd", "[design-import][shortcuts][v2]") {
    REQUIRE(modifier_strings_to_mask({}) == 0);
    REQUIRE(modifier_strings_to_mask({"shift"}) == 1);              // kModShift
    REQUIRE(modifier_strings_to_mask({"ctrl"}) == 2);               // kModCtrl
    REQUIRE(modifier_strings_to_mask({"alt"}) == 4);                // kModAlt
    // "meta" -> kModCmd (platform-primary) per the extractor's
    // cross-platform metaKey||ctrlKey collapse. kModCmd is bit 4 = 16.
    REQUIRE(modifier_strings_to_mask({"meta"}) == 16);
    REQUIRE(modifier_strings_to_mask({"meta", "shift"}) == 17);
    REQUIRE(modifier_strings_to_mask({"unknown-mod"}) == 0);        // dropped silently
}

TEST_CASE("generate_pulp_js emits registerShortcut + handler thunk per shortcut", "[design-import][shortcuts][v2]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;

    DetectedShortcut esc;
    esc.key = "Escape";
    esc.modifiers = {};
    opts.shortcuts.push_back(esc);

    DetectedShortcut save;
    save.key = "s";
    save.modifiers = {"meta"};
    opts.shortcuts.push_back(save);

    DesignIR ir;  // empty root — we only care about the shortcut block

    std::string js = generate_pulp_js(ir, opts);

    // Each shortcut produces:
    //   1. globalThis.__pulpShortcutHandler_N = function() { ... }
    //   2. registerShortcut(keycode, mask, '__pulpShortcutHandler_N')
    REQUIRE(js.find("globalThis.__pulpShortcutHandler_0") != std::string::npos);
    REQUIRE(js.find("globalThis.__pulpShortcutHandler_1") != std::string::npos);
    REQUIRE(js.find("registerShortcut(274, 0, '__pulpShortcutHandler_0')") != std::string::npos);
    REQUIRE(js.find("registerShortcut(115, 16, '__pulpShortcutHandler_1')") != std::string::npos);

    // Synthetic-keydown re-dispatch: each thunk calls __dispatch__ with
    // a properly-shaped W3C-ish event object so React handlers fire.
    REQUIRE(js.find("__dispatch__('__global__', 'keydown'") != std::string::npos);
    REQUIRE(js.find("key: 'Escape'") != std::string::npos);
    REQUIRE(js.find("key: 's'") != std::string::npos);
    REQUIRE(js.find("metaKey: true") != std::string::npos);
}

TEST_CASE("generate_pulp_js skips shortcuts whose key doesn't resolve", "[design-import][shortcuts][v2]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;

    DetectedShortcut weird;
    weird.key = "MysteryKey";
    opts.shortcuts.push_back(weird);

    DesignIR ir;
    std::string js = generate_pulp_js(ir, opts);

    // Unmapped key produces no registerShortcut entry. No __pulpShortcutHandler
    // is emitted either (we don't want orphan handlers).
    REQUIRE(js.find("registerShortcut") == std::string::npos);
    REQUIRE(js.find("__pulpShortcutHandler_0") == std::string::npos);
}

TEST_CASE("generate_pulp_js with empty shortcuts emits no shortcut block", "[design-import][shortcuts][v2]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    // opts.shortcuts is default-empty.

    DesignIR ir;
    std::string js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("registerShortcut") == std::string::npos);
    REQUIRE(js.find("__pulpShortcutHandler") == std::string::npos);
}

TEST_CASE("collect_modifiers scopes to enclosing if(...) only", "[design-import][shortcuts][v2]") {
    // Pre-fix this returned ["meta", "alt", "shift"] for every Escape match
    // because the modifier-detection window saw the modifier checks from
    // sibling branches. Now it walks back only within the same if condition.
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const onKey = (e) => {
                if (e.key === 'Escape') closeAll();
                if ((e.metaKey || e.ctrlKey) && e.key === 's') save();
                if (e.shiftKey && e.altKey && e.key === 'F') openFlare();
            };
        )JS", "");
    // Sorted by key: Escape, F, s.
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].key == "Escape");
    REQUIRE(out[0].modifiers.empty());  // bare check, no modifiers
    REQUIRE(out[1].key == "F");
    REQUIRE(out[2].key == "s");
    // `metaKey || ctrlKey` now emits BOTH (Codex P1 #2119) so codegen can
    // bind Cmd+S and Ctrl+S as distinct physical chords.
    REQUIRE(out[2].modifiers.size() == 2);
    auto s_has = [&](const std::string& m) {
        return std::find(out[2].modifiers.begin(), out[2].modifiers.end(), m)
            != out[2].modifiers.end();
    };
    REQUIRE(s_has("meta"));
    REQUIRE(s_has("ctrl"));
}

TEST_CASE("key_string_to_keycode maps KeyboardEvent.code letter/digit forms", "[design-import][shortcuts][v2]") {
    // Codex P2 review on #2119: the extractor pulls both `event.key` and
    // `event.code` patterns. Without these mappings `event.code === 'KeyS'`
    // and `event.code === 'Digit1'` fall through to 0 and codegen silently
    // drops the shortcut.
    REQUIRE(key_string_to_keycode("KeyS") == 's');
    REQUIRE(key_string_to_keycode("KeyA") == 'a');
    REQUIRE(key_string_to_keycode("KeyZ") == 'z');
    REQUIRE(key_string_to_keycode("keys") == 's');     // case-insensitive prefix
    REQUIRE(key_string_to_keycode("Digit0") == '0');
    REQUIRE(key_string_to_keycode("Digit9") == '9');
    REQUIRE(key_string_to_keycode("digit5") == '5');

    // Non-letter / non-digit suffixes return 0, not garbage.
    REQUIRE(key_string_to_keycode("Key1") == 0);       // not a letter
    REQUIRE(key_string_to_keycode("DigitA") == 0);     // not a digit
    REQUIRE(key_string_to_keycode("KeyAA") == 0);      // wrong length
}

TEST_CASE("generate_pulp_js emits two bindings for meta+ctrl cross-platform shortcut", "[design-import][shortcuts][v2]") {
    // Codex P1 review on #2119: when the source author writes
    // `(e.metaKey || e.ctrlKey) && e.key === 's'`, the codegen must emit
    // both registerShortcut(kc, kModCmd, ...) and registerShortcut(kc,
    // kModCtrl, ...) so the user gets Cmd+S on macOS and Ctrl+S on
    // Win/Linux. Each handler thunk sets only the modifier flag that
    // matches the physical chord, so the source handler's
    // `e.metaKey || e.ctrlKey` check sees the right flag.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;

    DetectedShortcut save;
    save.key = "s";
    save.modifiers = {"meta", "ctrl"};
    opts.shortcuts.push_back(save);

    DesignIR ir;
    std::string js = generate_pulp_js(ir, opts);

    // Two distinct handlers + two distinct registerShortcut calls.
    REQUIRE(js.find("__pulpShortcutHandler_0_cmd")  != std::string::npos);
    REQUIRE(js.find("__pulpShortcutHandler_0_ctrl") != std::string::npos);
    REQUIRE(js.find("registerShortcut(115, 16, '__pulpShortcutHandler_0_cmd')")  != std::string::npos);
    REQUIRE(js.find("registerShortcut(115, 2, '__pulpShortcutHandler_0_ctrl')")  != std::string::npos);

    // The Cmd-fired thunk sets metaKey:true, ctrlKey:false.
    auto cmd_pos = js.find("__pulpShortcutHandler_0_cmd = function");
    REQUIRE(cmd_pos != std::string::npos);
    auto cmd_end = js.find("};", cmd_pos);
    auto cmd_body = js.substr(cmd_pos, cmd_end - cmd_pos);
    REQUIRE(cmd_body.find("metaKey: true")  != std::string::npos);
    REQUIRE(cmd_body.find("ctrlKey: false") != std::string::npos);

    // The Ctrl-fired thunk sets ctrlKey:true, metaKey:false.
    auto ctrl_pos = js.find("__pulpShortcutHandler_0_ctrl = function");
    REQUIRE(ctrl_pos != std::string::npos);
    auto ctrl_end = js.find("};", ctrl_pos);
    auto ctrl_body = js.substr(ctrl_pos, ctrl_end - ctrl_pos);
    REQUIRE(ctrl_body.find("ctrlKey: true")  != std::string::npos);
    REQUIRE(ctrl_body.find("metaKey: false") != std::string::npos);
}

TEST_CASE("generate_pulp_js Ctrl-only emits ctrlKey:true synthetic event", "[design-import][shortcuts][v2]") {
    // Codex P1 case: a Win/Linux-only `e.ctrlKey && e.key === 's'`
    // handler must receive `ctrlKey: true` in the synthetic event so the
    // source check passes.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;

    DetectedShortcut save;
    save.key = "s";
    save.modifiers = {"ctrl"};
    opts.shortcuts.push_back(save);

    DesignIR ir;
    std::string js = generate_pulp_js(ir, opts);

    // Single binding with kModCtrl mask (== 2).
    REQUIRE(js.find("registerShortcut(115, 2, '__pulpShortcutHandler_0')") != std::string::npos);
    auto pos = js.find("__pulpShortcutHandler_0 = function");
    REQUIRE(pos != std::string::npos);
    auto body = js.substr(pos, js.find("};", pos) - pos);
    REQUIRE(body.find("ctrlKey: true")  != std::string::npos);
    REQUIRE(body.find("metaKey: false") != std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────
// End-to-end roundtrip — the wiring the user actually cares about.
//
// Path under test:
//   React source (TSX)
//     -> extract_keyboard_shortcuts
//     -> generate_pulp_js (emits registerShortcut + thunks)
//     -> WidgetBridge.load_script (JS engine evaluates the emitted code,
//                                  thunks register, native shortcuts get
//                                  hooked into shortcuts_)
//     -> bridge.forward_key_event(keycode, modifiers, down)
//     -> thunk fires -> __dispatch__('__global__', 'keydown', {...})
//     -> React-style window.addEventListener('keydown', ...) handler
//        receives a synthetic event with the right flags.
//
// Pre-V2 the React handler never fired because the bundled JS never saw
// the keypress at all (native intercept owned it).  V2's thunk closes
// the loop by re-dispatching as a synthetic event.  This test pins
// that loop end-to-end so a future change to either codegen or
// dispatch can't silently break it.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("E2E roundtrip: extract -> codegen -> WidgetBridge -> React-style handler",
          "[design-import][shortcuts][v2][e2e]") {
    using namespace pulp::view;
    using pulp::state::StateStore;

    // 1. A representative React source — covers the patterns the user
    //    cares about: bare key (Escape), mode key (F with chord), and
    //    the cross-platform save chord that motivated the Codex P1 fix.
    const char* tsx_source = R"JS(
        const onKey = (e) => {
            if (e.key === 'Escape') closeAll();
            if (e.shiftKey && e.altKey && e.key === 'F') openFlare();
            if ((e.metaKey || e.ctrlKey) && e.key === 's') save();
            if (e.ctrlKey && e.key === 'n') newFile();
        };
    )JS";
    auto shortcuts = extract_keyboard_shortcuts(tsx_source, "");
    // Sorted by key: Escape, F, n, s.
    REQUIRE(shortcuts.size() == 4);

    // 2. Hand the extracted shortcuts to the codegen.  Empty DesignIR is
    //    fine — we only want the shortcut block.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    opts.shortcuts = shortcuts;

    DesignIR ir;
    std::string emitted_js = generate_pulp_js(ir, opts);

    // 3. Spin up a WidgetBridge and install a React-style global keydown
    //    handler BEFORE evaluating the emitted JS, so the handler is
    //    already wired when registerShortcut runs.  Then load the
    //    emitted script.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var fired = [];
        function recordKey(e) {
            fired.push({
                key:      e.key,
                ctrlKey:  !!e.ctrlKey,
                shiftKey: !!e.shiftKey,
                altKey:   !!e.altKey,
                metaKey:  !!e.metaKey
            });
        }
        // Mimic the React-imported handlers — same shapes the developer
        // wrote in the source above.
        function escapeHandler(e) { if (e.key === 'Escape') recordKey(e); }
        function modeFHandler(e)  { if (e.shiftKey && e.altKey && e.key === 'F') recordKey(e); }
        function saveHandler(e)   { if ((e.metaKey || e.ctrlKey) && e.key === 's') recordKey(e); }
        function newCtrlOnly(e)   { if (e.ctrlKey && e.key === 'n') recordKey(e); }

        window.addEventListener('keydown', escapeHandler);
        window.addEventListener('keydown', modeFHandler);
        window.addEventListener('keydown', saveHandler);
        window.addEventListener('keydown', newCtrlOnly);

        function fired_count() { return fired.length; }
        function fired_at(i, field) { return fired[i] ? fired[i][field] : null; }
    )JS");

    // Now evaluate the codegen output — defines __pulpShortcutHandler_N
    // and calls registerShortcut() for each chord.
    bridge.load_script(emitted_js);

    auto fired_count = [&]() {
        return engine.evaluate("fired_count()").getWithDefault<int>(-1);
    };
    auto fired_field = [&](int i, const std::string& field) {
        return engine.evaluate("fired_at(" + std::to_string(i) + ", '" + field + "')");
    };

    REQUIRE(fired_count() == 0);

    // 4. Drive the chord through the native intercept path.

    // Escape — bare key, no modifiers.
    bridge.forward_key_event(static_cast<int>(KeyCode::escape), 0, true);
    REQUIRE(fired_count() == 1);
    REQUIRE(fired_field(0, "key").toString()         == "Escape");
    REQUIRE(fired_field(0, "metaKey").getWithDefault<bool>(true) == false);
    REQUIRE(fired_field(0, "ctrlKey").getWithDefault<bool>(true) == false);

    // Mode key F with shift+alt.
    bridge.forward_key_event(static_cast<int>('f'),
                             static_cast<uint16_t>(kModShift | kModAlt), true);
    REQUIRE(fired_count() == 2);
    REQUIRE(fired_field(1, "key").toString()         == "F");
    REQUIRE(fired_field(1, "shiftKey").getWithDefault<bool>(false) == true);
    REQUIRE(fired_field(1, "altKey").getWithDefault<bool>(false)   == true);

    // Cmd+S — the macOS branch of the cross-platform `metaKey||ctrlKey`
    // collapse.  V2 emits TWO bindings; this one matches the Cmd mask.
    // The handler's `e.metaKey || e.ctrlKey` evaluates true via metaKey.
    bridge.forward_key_event(static_cast<int>('s'), static_cast<uint16_t>(kModCmd), true);
    REQUIRE(fired_count() == 3);
    REQUIRE(fired_field(2, "key").toString()         == "s");
    REQUIRE(fired_field(2, "metaKey").getWithDefault<bool>(false) == true);
    REQUIRE(fired_field(2, "ctrlKey").getWithDefault<bool>(true)  == false);

    // Ctrl+S — the Win/Linux branch of the same source `||` check.
    // Pre-Codex-P1 this would have done nothing because V1 normalized
    // everything to "meta" and V2 only emitted the Cmd-mask binding.
    bridge.forward_key_event(static_cast<int>('s'), static_cast<uint16_t>(kModCtrl), true);
    REQUIRE(fired_count() == 4);
    REQUIRE(fired_field(3, "key").toString()         == "s");
    REQUIRE(fired_field(3, "ctrlKey").getWithDefault<bool>(false) == true);
    REQUIRE(fired_field(3, "metaKey").getWithDefault<bool>(true)  == false);

    // Ctrl+N — true Ctrl-only handler (no `||` collapse).  The synthetic
    // event must carry ctrlKey:true; otherwise the source check fails.
    bridge.forward_key_event(static_cast<int>('n'), static_cast<uint16_t>(kModCtrl), true);
    REQUIRE(fired_count() == 5);
    REQUIRE(fired_field(4, "key").toString()         == "n");
    REQUIRE(fired_field(4, "ctrlKey").getWithDefault<bool>(false) == true);
    REQUIRE(fired_field(4, "metaKey").getWithDefault<bool>(true)  == false);
}

// ────────────────────────────────────────────────────────────────────────
// Phase A — default shortcuts (source-matched). Heuristic detector +
// apply step + collision behavior. Spec: planning/2026-05-16-default-
// keyboard-shortcuts.md.
// ────────────────────────────────────────────────────────────────────────

using pulp::view::DefaultShortcutPattern;
using pulp::view::detect_default_shortcuts;
using pulp::view::apply_default_shortcuts;
using pulp::view::serialize_default_shortcut_scan;
using pulp::view::TargetPlatform;

TEST_CASE("default shortcuts: high-confidence Settings modal fires",
          "[design-import][shortcuts][defaults]") {
    auto scan = detect_default_shortcuts(R"JS(
        function SettingsModal({ onClose }) {
            return (
                <div role="dialog" aria-label="Settings">
                    <h1>Settings</h1>
                    <button onClick={onClose}>Close</button>
                </div>
            );
        }
    )JS", /*existing=*/{});

    REQUIRE(scan.accepted.size() == 1);
    REQUIRE(scan.accepted[0].pattern == DefaultShortcutPattern::settings);
    REQUIRE(scan.accepted[0].target == "SettingsModal");
    REQUIRE(scan.accepted[0].confidence == "high");
    REQUIRE(scan.collisions.empty());
}

TEST_CASE("default shortcuts: canonical name fires with no body signals",
          "[design-import][shortcuts][defaults]") {
    // Real-world apps (Spectr's `SettingsModal`, `HelpPopover`) use inline-
    // styled divs without role="dialog" or aria-label. The canonical-name
    // bonus catches `<Pattern>Modal/Dialog/Panel/Popover/Sheet/Window/Drawer`
    // exact shapes so those don't slip through. Single-signal + canonical
    // suffix = 2 signals → fires at medium confidence.
    auto scan = detect_default_shortcuts(R"JS(
        function SettingsModal() { return <div />; }
    )JS", {});
    REQUIRE(scan.accepted.size() == 1);
    REQUIRE(scan.accepted[0].pattern == DefaultShortcutPattern::settings);
    REQUIRE(scan.accepted[0].confidence == "medium");
}

TEST_CASE("default shortcuts: non-canonical name + heading fires at medium",
          "[design-import][shortcuts][defaults]") {
    // `HelpFooter` is NOT a canonical kind suffix, so we need a second
    // real signal — the heading provides it. Two signals → medium.
    auto scan = detect_default_shortcuts(R"JS(
        function HelpFooter() { return <div><h2>Help</h2><p>...</p></div>; }
    )JS", {});
    REQUIRE(scan.accepted.size() == 1);
    REQUIRE(scan.accepted[0].confidence == "medium");
    REQUIRE(scan.accepted[0].pattern == DefaultShortcutPattern::help);
}

TEST_CASE("default shortcuts: non-canonical single-signal still does NOT fire",
          "[design-import][shortcuts][defaults]") {
    // `SettingsList` is name-only AND non-canonical (List isn't in the
    // modal-kind suffix set). Should NOT fire — `<SettingsList>` is the
    // grouping widget, not the modal itself.
    auto scan = detect_default_shortcuts(R"JS(
        function SettingsList() { return <ul />; }
    )JS", {});
    REQUIRE(scan.accepted.empty());
    REQUIRE(scan.collisions.empty());
}

TEST_CASE("default shortcuts: multiple Settings candidates → COLLISION, no bind",
          "[design-import][shortcuts][defaults]") {
    auto scan = detect_default_shortcuts(R"JS(
        function AppSettingsModal() {
            return <div role="dialog" aria-label="Settings"><h1>Settings</h1></div>;
        }
        function TrackSettingsModal() {
            return <div role="dialog" aria-label="Settings"><h1>Settings</h1></div>;
        }
    )JS", {});
    REQUIRE(scan.accepted.empty());
    REQUIRE(scan.collisions.size() == 1);
    REQUIRE(scan.collisions[0].pattern == DefaultShortcutPattern::settings);
    REQUIRE(scan.collisions[0].candidates.size() == 2);
}

TEST_CASE("default shortcuts: cheatsheet vs help disambiguation via <kbd>",
          "[design-import][shortcuts][defaults]") {
    auto scan = detect_default_shortcuts(R"JS(
        function ShortcutsModal() {
            return (
                <div role="dialog" aria-label="Keyboard shortcuts">
                    <h1>Shortcuts</h1>
                    <kbd>Cmd+S</kbd> — save
                    <kbd>Cmd+,</kbd> — settings
                </div>
            );
        }
    )JS", {});
    REQUIRE(scan.accepted.size() == 1);
    REQUIRE(scan.accepted[0].pattern == DefaultShortcutPattern::cheatsheet);
    bool has_kbd_sig = false;
    for (const auto& s : scan.accepted[0].signals) {
        if (s == "kbd-tag-present") { has_kbd_sig = true; break; }
    }
    REQUIRE(has_kbd_sig);
}

TEST_CASE("default shortcuts: extracted shortcut suppresses same-chord default",
          "[design-import][shortcuts][defaults]") {
    // Developer already wrote Cmd+, manually. Don't double-bind.
    pulp::view::DetectedShortcut hand_written;
    hand_written.key = ",";
    hand_written.modifiers = {"meta"};
    auto scan = detect_default_shortcuts(R"JS(
        function SettingsModal() {
            return <div role="dialog" aria-label="Settings"><h1>Settings</h1></div>;
        }
    )JS", {hand_written});
    REQUIRE(scan.accepted.empty());
}

TEST_CASE("default shortcuts: apply_default_shortcuts maps per-platform chords",
          "[design-import][shortcuts][defaults]") {
    pulp::view::DefaultShortcutCandidate settings;
    settings.pattern = DefaultShortcutPattern::settings;
    settings.target = "SettingsModal";
    settings.confidence = "high";

    auto mac = apply_default_shortcuts({settings}, TargetPlatform::macos);
    REQUIRE(mac.size() == 1);
    REQUIRE(mac[0].key == ",");
    REQUIRE(mac[0].modifiers == std::vector<std::string>{"meta"});

    auto win = apply_default_shortcuts({settings}, TargetPlatform::win_linux);
    REQUIRE(win.size() == 1);
    REQUIRE(win[0].key == ",");
    REQUIRE(win[0].modifiers == std::vector<std::string>{"ctrl"});

    // Help: bare F1 on Win/Linux, Cmd+? on mac.
    pulp::view::DefaultShortcutCandidate help;
    help.pattern = DefaultShortcutPattern::help;
    help.target = "HelpPanel";
    help.confidence = "medium";
    auto help_mac = apply_default_shortcuts({help}, TargetPlatform::macos);
    REQUIRE(help_mac[0].key == "?");
    REQUIRE(help_mac[0].modifiers == std::vector<std::string>{"meta"});
    auto help_win = apply_default_shortcuts({help}, TargetPlatform::win_linux);
    REQUIRE(help_win[0].key == "F1");
    REQUIRE(help_win[0].modifiers.empty());
}

TEST_CASE("default shortcuts: serialize_default_shortcut_scan emits stable JSON",
          "[design-import][shortcuts][defaults]") {
    pulp::view::DefaultShortcutScan scan;
    pulp::view::DefaultShortcutCandidate c;
    c.pattern = DefaultShortcutPattern::settings;
    c.target = "SettingsModal";
    c.confidence = "high";
    c.signals = {"component-name:SettingsModal", "aria-role:dialog"};
    scan.accepted.push_back(c);

    pulp::view::DefaultShortcutCollision col;
    col.pattern = DefaultShortcutPattern::help;
    col.candidates = {"AppHelp", "TrackHelp"};
    col.reason = "multiple components match — no default bound";
    scan.collisions.push_back(col);

    auto json = serialize_default_shortcut_scan(scan);
    REQUIRE(json.find("\"defaults\"") != std::string::npos);
    REQUIRE(json.find("\"settings\"") != std::string::npos);
    REQUIRE(json.find("\"high\"") != std::string::npos);
    REQUIRE(json.find("\"collisions\"") != std::string::npos);
    REQUIRE(json.find("\"help\"") != std::string::npos);
    REQUIRE(json.find("\"AppHelp\"") != std::string::npos);
}

TEST_CASE("default shortcuts: E2E — codegen emits default thunks too",
          "[design-import][shortcuts][defaults][e2e]") {
    // The defaults ride V2's existing codegen path (no fork). Verify the
    // bound default produces registerShortcut + thunk just like an
    // extracted entry.
    auto scan = detect_default_shortcuts(R"JS(
        function SettingsModal({ onClose }) {
            return (
                <div role="dialog" aria-label="Settings">
                    <h1>Settings</h1>
                </div>
            );
        }
    )JS", {});
    REQUIRE(scan.accepted.size() == 1);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    opts.shortcuts = apply_default_shortcuts(scan.accepted, TargetPlatform::macos);

    DesignIR ir;
    auto js = generate_pulp_js(ir, opts);

    // Cmd+, → keycode 44 (comma) + mask 16 (kModCmd) + a synthetic event
    // with metaKey:true.
    REQUIRE(js.find("registerShortcut(44, 16, '__pulpShortcutHandler_0')") != std::string::npos);
    REQUIRE(js.find("metaKey: true") != std::string::npos);
    REQUIRE(js.find("key: ','") != std::string::npos);
}

TEST_CASE("default shortcuts: emitted JS escapes single-quote + backslash (Codex P1 #2128)",
          "[design-import][shortcuts][defaults][v2]") {
    // Widening key_string_to_keycode to accept all printable ASCII lets
    // `'` and `\` pass validation. Without escaping at emission time
    // the generated `key: '...'` literal becomes syntactically invalid
    // JS and the whole script fails to load. Pin the escape.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;

    DetectedShortcut quote;  quote.key = "'";  // single-quote literal
    DetectedShortcut backslash; backslash.key = "\\";  // single backslash
    opts.shortcuts = {quote, backslash};

    DesignIR ir;
    auto js = generate_pulp_js(ir, opts);

    // Both literals appear escaped — `key: '\''` and `key: '\\'`.
    REQUIRE(js.find("key: '\\''")  != std::string::npos);
    REQUIRE(js.find("key: '\\\\'") != std::string::npos);
    // No unescaped raw `key: '''` would tokenize as empty-string + stray
    // quote — assert that doesn't appear.
    REQUIRE(js.find("key: '''") == std::string::npos);
}

TEST_CASE("default shortcuts: apply_default_shortcuts win_linux maps File-menu chords correctly",
          "[design-import][shortcuts][defaults]") {
    // P2 follow-up — every Cmd-on-mac default has a Ctrl-on-win/linux
    // counterpart. Pin all four so a future re-map doesn't silently
    // drop one platform.
    auto chord_for = [&](DefaultShortcutPattern p) {
        pulp::view::DefaultShortcutCandidate c;
        c.pattern = p; c.target = "x"; c.confidence = "medium";
        auto mac = apply_default_shortcuts({c}, TargetPlatform::macos)[0];
        auto win = apply_default_shortcuts({c}, TargetPlatform::win_linux)[0];
        return std::make_pair(mac, win);
    };
    for (auto p : {DefaultShortcutPattern::new_file,
                   DefaultShortcutPattern::open_file,
                   DefaultShortcutPattern::save_file,
                   DefaultShortcutPattern::find}) {
        auto [mac, win] = chord_for(p);
        REQUIRE(mac.key == win.key);
        REQUIRE(mac.modifiers == std::vector<std::string>{"meta"});
        REQUIRE(win.modifiers == std::vector<std::string>{"ctrl"});
    }

    // Cheatsheet bare `?` is platform-agnostic (same chord both sides).
    auto [csm, csw] = chord_for(DefaultShortcutPattern::cheatsheet);
    REQUIRE(csm.key == "?");          REQUIRE(csw.key == "?");
    REQUIRE(csm.modifiers.empty());   REQUIRE(csw.modifiers.empty());
}
