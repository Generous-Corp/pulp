// Who consumes a key inside an embedded plugin editor — and, just as much,
// who does NOT.
//
// An editor that consumes its own shortcuts is the easy half and the half a
// test usually covers. The half that breaks in a DAW is the other one: a key
// nothing in the editor handled must be reported NOT consumed so the host
// still receives it. Space is the one users feel — a musician expects it to
// start and stop the transport even while the editor has focus — and a suite
// that only asserts "⌘Z reaches the plugin" cannot fail the way this fails.
//
// Every case below therefore comes in a pair: the key the editor claims, and
// the sibling key it must hand back.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/input_events.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/plugin_key_routing.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using pulp::view::KeyCode;
using pulp::view::KeyEvent;
using pulp::view::PluginKeyDisposition;
using pulp::view::PluginKeyOffer;
using pulp::view::route_plugin_key;
using pulp::view::View;

namespace {

class TestView : public View {
  public:
    void paint(pulp::canvas::Canvas&) override {}
};

// A widget that answers key questions from a script rather than a guess: the
// test tells it which keys it genuinely handles, and everything else is
// declined — which is what must reach the host.
class ScriptedKeyView : public View {
  public:
    void paint(pulp::canvas::Canvas&) override {}

    bool takes_text = false;
    bool takes_navigation = false;
    KeyCode claims = KeyCode::unknown;
    std::uint16_t claims_modifiers = 0;
    int offered = 0;

    bool accepts_text_input() const override {
        return takes_text;
    }
    bool accepts_navigation_input() const override {
        return takes_navigation;
    }

    bool on_key_event(const KeyEvent& e) override {
        ++offered;
        return e.key == claims && e.modifiers == claims_modifiers;
    }
};

// Both focus slots are process-global mirrors; other cases in this binary
// leave them set.
struct RoutingGuard {
    RoutingGuard() {
        View::focused_input_ = nullptr;
        View::active_overlay_ = nullptr;
    }
    ~RoutingGuard() {
        View::focused_input_ = nullptr;
        View::active_overlay_ = nullptr;
    }
};

// add_child() returns void, so keep the raw pointer the test drives.
template <typename T> T* attach(View& parent) {
    auto owned = std::make_unique<T>();
    T* raw = owned.get();
    parent.add_child(std::move(owned));
    return raw;
}

PluginKeyOffer plain(KeyCode key, std::uint16_t modifiers = 0) {
    PluginKeyOffer offer;
    offer.key.key = key;
    offer.key.modifiers = modifiers;
    offer.key.is_down = true;
    return offer;
}

PluginKeyOffer function_key(KeyCode key) {
    PluginKeyOffer offer = plain(key);
    offer.is_function_key = true;
    return offer;
}

} // namespace

// ── The baseline: an editor that claims nothing keeps the DAW's keyboard ────

TEST_CASE("an idle editor forwards every key to the host",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView root;

    // The positive control for this whole file lives in the cases below: the
    // same instrument DOES report `consumed` when something claims a key, so a
    // forward_to_host verdict here is an answer, not a dead measurement.
    REQUIRE(route_plugin_key(root, plain(KeyCode::space)) == PluginKeyDisposition::forward_to_host);
    REQUIRE(route_plugin_key(root, plain(KeyCode::escape)) ==
            PluginKeyDisposition::forward_to_host);
    REQUIRE(route_plugin_key(root, plain(KeyCode::z, pulp::view::kModCmd)) ==
            PluginKeyDisposition::forward_to_host);
}

// ── A focused text field: the shortcut half AND the forwarding half ─────────

TEST_CASE("a focused text field consumes the editing chords it handles",
          "[view][plugin-key-routing]") {
    RoutingGuard g;
    TestView root;
    auto* field = attach<pulp::view::TextEditor>(root);
    field->set_text("hello");
    field->claim_input_focus();

    // ⌘A selects all inside the field — the host must not also see it.
    REQUIRE(route_plugin_key(root, plain(KeyCode::a, pulp::view::kModCmd)) ==
            PluginKeyDisposition::consumed);
    REQUIRE(field->has_selection());
}

TEST_CASE("a focused text field takes Space as text, not transport", "[view][plugin-key-routing]") {
    RoutingGuard g;
    TestView root;
    auto* field = attach<pulp::view::TextEditor>(root);
    field->claim_input_focus();

    // Not `consumed`: the platform owns dead keys and IME, so the seam runs
    // its own insertion path. Either way the host does not get the key.
    REQUIRE(route_plugin_key(root, plain(KeyCode::space)) == PluginKeyDisposition::insert_as_text);
}

TEST_CASE("a chord a focused text field declines goes back to the host",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView root;
    auto* field = attach<ScriptedKeyView>(root);
    field->takes_text = true;
    field->claims = KeyCode::z;
    field->claims_modifiers = pulp::view::kModCmd;
    field->claim_input_focus();

    // The one it handles.
    REQUIRE(route_plugin_key(root, plain(KeyCode::z, pulp::view::kModCmd)) ==
            PluginKeyDisposition::consumed);

    // The ones it does not. A chord is never text, so there is nothing left
    // for the field to do with it — swallowing it is what kills a host
    // shortcut for as long as a type-in happens to be open.
    REQUIRE(route_plugin_key(root, plain(KeyCode::s, pulp::view::kModCmd)) ==
            PluginKeyDisposition::forward_to_host);
    REQUIRE(route_plugin_key(root, plain(KeyCode::space, pulp::view::kModCtrl)) ==
            PluginKeyDisposition::forward_to_host);
    REQUIRE(field->offered == 3);
}

TEST_CASE("a function key a focused text field declines goes back to the host",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView root;
    auto* field = attach<ScriptedKeyView>(root);
    field->takes_text = true;
    field->claim_input_focus();

    // F-keys carry no character, so a field that declined one has genuinely
    // not consumed it. Several DAWs bind transport and marker actions there.
    REQUIRE(route_plugin_key(root, function_key(KeyCode::f5)) ==
            PluginKeyDisposition::forward_to_host);

    // Control: the same instrument, same field, a key that IS text.
    REQUIRE(route_plugin_key(root, plain(KeyCode::k)) == PluginKeyDisposition::insert_as_text);
}

// ── A focused non-text widget must never become a keyboard sink ─────────────

TEST_CASE("a navigation widget borrows arrows and never Space",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView root;
    auto* knob = attach<ScriptedKeyView>(root);
    knob->takes_navigation = true;
    knob->claims = KeyCode::left;
    knob->claim_input_focus();

    REQUIRE(route_plugin_key(root, plain(KeyCode::left)) == PluginKeyDisposition::consumed);

    // Outside the bounded set the widget is not even offered the key, so it
    // cannot claim transport by accident.
    const int offered_before = knob->offered;
    REQUIRE(route_plugin_key(root, plain(KeyCode::space)) == PluginKeyDisposition::forward_to_host);
    REQUIRE(knob->offered == offered_before);

    // Inside the set but declined — the widget's own answer, not a list.
    REQUIRE(route_plugin_key(root, plain(KeyCode::right)) == PluginKeyDisposition::forward_to_host);
    REQUIRE(knob->offered == offered_before + 1);
}

TEST_CASE("a focusable widget that claims neither keyboard forwards Space",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView root;
    auto* plain_view = attach<ScriptedKeyView>(root);
    plain_view->claims = KeyCode::enter;
    plain_view->claim_input_focus();

    REQUIRE(route_plugin_key(root, plain(KeyCode::enter)) == PluginKeyDisposition::consumed);
    REQUIRE(route_plugin_key(root, plain(KeyCode::space)) == PluginKeyDisposition::forward_to_host);
}

// ── An open overlay legitimately changes who consumes a key ─────────────────

TEST_CASE("an open modal consumes Escape and still forwards Space",
          "[view][plugin-key-routing][overlay]") {
    RoutingGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto* modal = attach<pulp::view::ModalOverlay>(root);
    modal->set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    // Nothing holds focus — the ordinary state while a menu is open — so a
    // focus-gated policy would never reach this at all.
    REQUIRE(View::focused_input_ == nullptr);
    REQUIRE(route_plugin_key(root, plain(KeyCode::escape)) == PluginKeyDisposition::consumed);

    // A menu being open does not make the editor a keyboard sink.
    REQUIRE(route_plugin_key(root, plain(KeyCode::space)) == PluginKeyDisposition::forward_to_host);
}

TEST_CASE("an open dropdown consumes Escape", "[view][plugin-key-routing][overlay]") {
    RoutingGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto* combo = attach<pulp::view::ComboBox>(root);
    combo->set_bounds({10.0f, 10.0f, 160.0f, 24.0f});
    combo->set_items({"One", "Two", "Three"});

    // Control: with the dropdown shut, Escape is the host's.
    REQUIRE(route_plugin_key(root, plain(KeyCode::escape)) ==
            PluginKeyDisposition::forward_to_host);

    pulp::view::MouseEvent open_click;
    open_click.position = {60.0f, 12.0f};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    REQUIRE(route_plugin_key(root, plain(KeyCode::escape)) == PluginKeyDisposition::consumed);
    REQUIRE_FALSE(combo->is_open());
}

// ── The framework-level hook is last, and is not a bypass ──────────────────

TEST_CASE("the global hook sees only what the focused view declined",
          "[view][plugin-key-routing]") {
    RoutingGuard g;
    TestView root;
    auto* field = attach<ScriptedKeyView>(root);
    field->takes_text = true;
    field->claims = KeyCode::z;
    field->claims_modifiers = pulp::view::kModCmd;
    field->claim_input_focus();

    int hook_calls = 0;
    root.on_global_key = [&](const KeyEvent& e) {
        ++hook_calls;
        return e.key == KeyCode::s;
    };

    // The field's own undo wins; the editor-wide hook never sees it.
    REQUIRE(route_plugin_key(root, plain(KeyCode::z, pulp::view::kModCmd)) ==
            PluginKeyDisposition::consumed);
    REQUIRE(hook_calls == 0);

    // A chord the field declined reaches the hook, which claims it.
    REQUIRE(route_plugin_key(root, plain(KeyCode::s, pulp::view::kModCmd)) ==
            PluginKeyDisposition::consumed);
    REQUIRE(hook_calls == 1);

    // And a chord neither claims still reaches the host.
    REQUIRE(route_plugin_key(root, plain(KeyCode::w, pulp::view::kModCmd)) ==
            PluginKeyDisposition::forward_to_host);
    REQUIRE(hook_calls == 2);
}

TEST_CASE("a seam that already offered the hook does not offer it twice",
          "[view][plugin-key-routing]") {
    RoutingGuard g;
    TestView root;
    int hook_calls = 0;
    root.on_global_key = [&](const KeyEvent&) {
        ++hook_calls;
        return true;
    };

    // Control: an ordinary offer does consult it.
    REQUIRE(route_plugin_key(root, plain(KeyCode::z, pulp::view::kModCmd)) ==
            PluginKeyDisposition::consumed);
    REQUIRE(hook_calls == 1);

    // macOS gives a command chord to -performKeyEquivalent: and then again to
    // -keyDown:. Without this the hook — and the script keydown listener
    // behind it — fires twice for one press.
    PluginKeyOffer second = plain(KeyCode::z, pulp::view::kModCmd);
    second.global_hook_already_offered = true;
    REQUIRE(route_plugin_key(root, second) == PluginKeyDisposition::forward_to_host);
    REQUIRE(hook_calls == 1);
}

// ── Two open editors share one process-global focus slot ───────────────────

TEST_CASE("a second editor's focused field never answers for this root",
          "[view][plugin-key-routing][host-forward]") {
    RoutingGuard g;
    TestView mine;
    TestView theirs;
    auto* their_field = attach<ScriptedKeyView>(theirs);
    their_field->takes_text = true;
    their_field->claims = KeyCode::space;
    their_field->claim_input_focus();

    // Control: the editor that owns the field does consume it.
    REQUIRE(route_plugin_key(theirs, plain(KeyCode::space)) == PluginKeyDisposition::consumed);

    // Mine must not report the key handled — doing so means the host never
    // sees it either, so the spacebar dies with no visible cause.
    REQUIRE(route_plugin_key(mine, plain(KeyCode::space)) == PluginKeyDisposition::forward_to_host);
    REQUIRE(pulp::view::plugin_key_focus(mine) == nullptr);
    REQUIRE(pulp::view::plugin_key_focus(theirs) == their_field);
}
