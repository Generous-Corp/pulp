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

TEST_CASE("extract_keyboard_shortcuts captures meta modifier", "[design-import][shortcuts]") {
    auto out = extract_keyboard_shortcuts(
        R"JS(
            const onKey = (e) => {
                if ((e.metaKey || e.ctrlKey) && e.key === 's') save();
            };
        )JS", "");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].key == "s");
    // metaKey || ctrlKey collapses to a single "meta" entry — that's the
    // cross-platform shortcut idiom we want native Pulp to bind once.
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
    REQUIRE(key_string_to_keycode("@") == 0);  // not in alphanumeric range
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
    REQUIRE(out[2].modifiers.size() == 1);
    REQUIRE(out[2].modifiers[0] == "meta");
}
