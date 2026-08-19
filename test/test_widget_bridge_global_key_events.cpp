// Keyboard events reaching JS listeners.
//
// A Pulp UI registers keyboard shortcuts the way any web UI does:
//
//     window.addEventListener('keydown', onKey)
//     document.addEventListener('keydown', onKey, true)
//
// Native forwards keys through `__dispatch__('__global__', 'keydown', {...})`.
// These cases pin BOTH halves of that connection, because a mechanism whose
// native side dispatches into a JS side that never listens is invisible from
// either end alone — exactly how Escape-closes-dropdowns stayed broken while
// every unit test was green.
//
// The payload is W3C KeyboardEvent-shaped (`key` string plus ctrl/shift/alt/
// meta booleans), so these assert on that shape rather than a Pulp-specific
// one — an app ported from the web must not need to learn a new event.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

using namespace pulp::view;

namespace {

// KeyCode::escape — see input_events.hpp (backspace = 270, then delete_, tab,
// enter, escape).
constexpr int kEscape = 274;
constexpr int kKeyA = 'a';

constexpr std::uint16_t kModShiftBit = 1u << 0;
constexpr std::uint16_t kModMetaBit = 1u << 3;

struct Fixture {
    ScriptEngine engine;
    View root;
    pulp::state::StateStore store;
    WidgetBridge bridge{engine, root, store};

    Fixture() { root.set_bounds({0, 0, 400, 300}); }

    bool flag(const char* expr) {
        return engine.evaluate(expr).getWithDefault<bool>(false);
    }
    std::string str(const char* expr) {
        return engine.evaluate(expr).getWithDefault<std::string>("");
    }
    int num(const char* expr) { return engine.evaluate(expr).getWithDefault<int>(-1); }
};

}  // namespace

TEST_CASE("a keydown reaches a document.addEventListener handler",
          "[view][bridge][keyboard]") {
    // Spectr registers four of its keydown listeners on `document` (and four on
    // `window`). React popovers and modals overwhelmingly use the document form
    // because that is where the click-outside idiom already lives.
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.docFired = 0;
        globalThis.docKey = '';
        document.addEventListener('keydown', function(e) {
            globalThis.docFired++;
            globalThis.docKey = e.key;
        });
    )");

    f.bridge.forward_key_event(kEscape, 0, /*is_down=*/true);

    INFO("document keydown listeners must fire for a native key");
    CHECK(f.num("globalThis.docFired") == 1);
    CHECK(f.str("globalThis.docKey") == "Escape");
}

TEST_CASE("a keydown reaches a window.addEventListener handler",
          "[view][bridge][keyboard]") {
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.winFired = 0;
        globalThis.winKey = '';
        window.addEventListener('keydown', function(e) {
            globalThis.winFired++;
            globalThis.winKey = e.key;
        });
    )");

    f.bridge.forward_key_event(kEscape, 0, /*is_down=*/true);

    INFO("window keydown listeners must fire for a native key");
    CHECK(f.num("globalThis.winFired") == 1);
    CHECK(f.str("globalThis.winKey") == "Escape");
}

TEST_CASE("a key registered on BOTH window and document fires exactly once each",
          "[view][bridge][keyboard]") {
    // The failure this guards is a double-fire, which is what you get by adding
    // a second fan-out path without checking whether one already exists. A
    // handler that toggles a dropdown would open and immediately close it, and
    // the symptom ("the menu won't open") looks nothing like the cause.
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.w = 0; globalThis.d = 0;
        window.addEventListener('keydown', function() { globalThis.w++; });
        document.addEventListener('keydown', function() { globalThis.d++; });
    )");

    f.bridge.forward_key_event(kEscape, 0, /*is_down=*/true);

    CHECK(f.num("globalThis.w") == 1);
    CHECK(f.num("globalThis.d") == 1);
}

TEST_CASE("modifier flags survive the trip into JS",
          "[view][bridge][keyboard]") {
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.gotShift = null; globalThis.gotMeta = null;
        globalThis.gotCtrl = null;  globalThis.gotAlt = null;
        document.addEventListener('keydown', function(e) {
            globalThis.gotShift = e.shiftKey; globalThis.gotMeta = e.metaKey;
            globalThis.gotCtrl = e.ctrlKey;   globalThis.gotAlt = e.altKey;
        });
    )");

    f.bridge.forward_key_event(kKeyA, kModShiftBit | kModMetaBit, /*is_down=*/true);

    CHECK(f.flag("globalThis.gotShift === true"));
    CHECK(f.flag("globalThis.gotMeta === true"));
    CHECK(f.flag("globalThis.gotCtrl === false"));
    CHECK(f.flag("globalThis.gotAlt === false"));
}

TEST_CASE("a handler may call preventDefault and stopPropagation without throwing",
          "[view][bridge][keyboard]") {
    // Web handlers call these unconditionally. If they are absent the handler
    // throws mid-callback, and everything it would have done afterwards —
    // including closing the dropdown — silently does not happen.
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.survived = false;
        document.addEventListener('keydown', function(e) {
            e.preventDefault();
            e.stopPropagation();
            globalThis.survived = true;
        });
    )");

    f.bridge.forward_key_event(kEscape, 0, /*is_down=*/true);
    CHECK(f.flag("globalThis.survived === true"));
}

TEST_CASE("capture-phase registration still delivers",
          "[view][bridge][keyboard]") {
    // `addEventListener('keydown', fn, true)` appears four times in Spectr's
    // runtime. Whatever the shim does about ordering, the third argument must
    // not cause the listener to be dropped.
    Fixture f;
    f.bridge.load_script(R"(
        globalThis.capFired = 0;
        document.addEventListener('keydown', function() { globalThis.capFired++; }, true);
        window.addEventListener('keydown', function() { globalThis.capFired++; }, true);
    )");

    f.bridge.forward_key_event(kEscape, 0, /*is_down=*/true);
    CHECK(f.num("globalThis.capFired") == 2);
}

TEST_CASE("a focused text input still receives keys the global route also saw",
          "[view][bridge][keyboard]") {
    // NEGATIVE CONTROL for the focus guard. The global route is ADDITIVE: it
    // must not consume the key, or typing into a field would stop working the
    // moment any app registered a global shortcut. Asserts the guard's
    // observable consequence — the editor's own text still changes — rather
    // than trusting that the code path was left alone.
    Fixture f;
    auto editor = std::make_unique<TextEditor>();
    editor->set_bounds({0, 0, 200, 24});
    TextEditor* ed = editor.get();
    f.root.add_child(std::move(editor));
    ed->on_focus_changed(true);
    View::focused_input_ = ed;

    f.bridge.load_script(R"(
        globalThis.globalSaw = 0;
        document.addEventListener('keydown', function() { globalThis.globalSaw++; });
    )");

    TextInputEvent te;
    te.text = "x";
    ed->on_text_input(te);
    f.bridge.forward_key_event(kKeyA, 0, /*is_down=*/true);

    INFO("the global route must observe the key WITHOUT swallowing text input");
    CHECK(ed->text() == "x");
    CHECK(f.num("globalThis.globalSaw") == 1);

    View::focused_input_ = nullptr;
}
