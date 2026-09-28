// Focus event tests — validates focus/blur, focus traversal, and text input

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/text_editor.hpp>

#include <string>
#include <string_view>

using namespace pulp::test;
using namespace pulp::view;

TEST_CASE("Focus: on_focus_changed fires on gain", "[events][focus]") {
    struct FocusTracker : View {
        bool focused = false;
        void on_focus_changed(bool gained) override {
            View::on_focus_changed(gained);
            focused = gained;
        }
    };

    FocusTracker v;
    v.on_focus_changed(true);
    REQUIRE(v.focused);
    REQUIRE(v.has_focus());
}

TEST_CASE("Focus: on_focus_changed fires on blur", "[events][focus]") {
    struct FocusTracker : View {
        bool blur_fired = false;
        void on_focus_changed(bool gained) override {
            View::on_focus_changed(gained);
            if (!gained) blur_fired = true;
        }
    };

    FocusTracker v;
    v.on_focus_changed(true);
    v.on_focus_changed(false);
    REQUIRE(v.blur_fired);
    REQUIRE_FALSE(v.has_focus());
}

TEST_CASE("Focus: has_focus tracks state", "[events][focus]") {
    View v;
    REQUIRE_FALSE(v.has_focus());
    v.on_focus_changed(true);
    REQUIRE(v.has_focus());
    v.on_focus_changed(false);
    REQUIRE_FALSE(v.has_focus());
}

TEST_CASE("Focus: focus_next finds first focusable", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();

    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* focused = View::focus_next(root, nullptr);
    REQUIRE(focused == k1p);
}

TEST_CASE("Focus: focus_next advances to next widget", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* f1 = View::focus_next(root, nullptr);
    REQUIRE(f1 == k1p);
    auto* f2 = View::focus_next(root, f1);
    REQUIRE(f2 == k2p);
}

TEST_CASE("Focus: focus_prev goes backward", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    k1->set_bounds({10, 10, 48, 48});
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    k2->set_bounds({70, 10, 48, 48});
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    auto* prev = View::focus_prev(root, k2p);
    REQUIRE(prev == k1p);
}

TEST_CASE("Focus: TextInputEvent has text field", "[events][focus]") {
    TextInputEvent te;
    te.text = "hello";
    REQUIRE(te.text == "hello");
}

TEST_CASE("Focus: on_text_input default does not crash", "[events][focus]") {
    View v;
    TextInputEvent te;
    te.text = "x";
    v.on_text_input(te); // Should not crash
    REQUIRE(true);
}

TEST_CASE("Focus: focus cycle wraps around", "[events][focus]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto k1 = std::make_unique<Knob>();
    auto* k1p = k1.get();
    auto k2 = std::make_unique<Knob>();
    auto* k2p = k2.get();

    root.add_child(std::move(k1));
    root.add_child(std::move(k2));

    // After last widget, focus_next should wrap to first
    auto* after_last = View::focus_next(root, k2p);
    REQUIRE(after_last == k1p);
}

// ── Script-driven keyboard focus through the web-compat Element ─────────────
//
// element.focus() must move NATIVE keyboard focus, not just
// document.activeElement: the window hosts deliver keys and text to the
// root's focused input slot, so a field that only looks focused to script
// never gets a caret or a keystroke.

namespace {

std::string js_string(TestEnvironment& env, const std::string& expr) {
    auto value = env.engine.evaluate(expr);
    return std::string(value.getWithDefault<std::string_view>(""));
}

TextEditor* editor_for(TestEnvironment& env, const std::string& js_var) {
    return dynamic_cast<TextEditor*>(env.widget(js_string(env, js_var + "._id")));
}

// Deliver text the way a window host does: to whatever this root's focus
// slot names, never to a widget chosen by the test.
void type_into_focused(TestEnvironment& env, const std::string& text) {
    View* focused = focused_input_under_root(env.root);
    REQUIRE(focused != nullptr);
    TextInputEvent te;
    te.text = text;
    focused->on_text_input(te);
}

}  // namespace

TEST_CASE("Script focus: focus() gives an input's TextEditor native focus",
          "[events][focus][script-focus]") {
    TestEnvironment env;
    env.run(R"JS(
        var events = [];
        var first = document.createElement('input');
        var second = document.createElement('input');
        document.body.appendChild(first);
        document.body.appendChild(second);
        second.addEventListener('focus', function() { events.push('focus'); });
        second.focus();
    )JS");

    auto* first = editor_for(env, "first");
    auto* second = editor_for(env, "second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(second->has_focus());
    CHECK_FALSE(first->has_focus());
    CHECK(focused_input_under_root(env.root) == second);
    CHECK(js_string(env, "document.activeElement === second ? 'yes' : 'no'") == "yes");
    CHECK(js_string(env, "events.join(',')") == "focus");

    type_into_focused(env, "hello");
    CHECK(second->text() == "hello");
    CHECK(first->text().empty());

    // Moving focus by script blurs the previous holder through the same
    // transfer a click uses.
    env.eval("first.focus();");
    CHECK(first->has_focus());
    CHECK_FALSE(second->has_focus());
    type_into_focused(env, "x");
    CHECK(first->text() == "x");
}

TEST_CASE("Script focus: blur() removes native focus only from its own element",
          "[events][focus][script-focus]") {
    TestEnvironment env;
    env.run(R"JS(
        var a = document.createElement('input');
        var b = document.createElement('textarea');
        document.body.appendChild(a);
        document.body.appendChild(b);
        a.focus();
    )JS");
    auto* a = editor_for(env, "a");
    auto* b = editor_for(env, "b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->has_focus());

    // A stale blur on an element that does not hold focus changes nothing.
    env.eval("b.blur();");
    CHECK(a->has_focus());
    CHECK(focused_input_under_root(env.root) == a);

    env.eval("a.blur();");
    CHECK_FALSE(a->has_focus());
    CHECK(focused_input_under_root(env.root) == nullptr);
    CHECK(js_string(env, "document.activeElement === null ? 'null' : 'set'") == "null");
}

TEST_CASE("Script focus: focus() on an unfocusable element keeps the focused field",
          "[events][focus][script-focus]") {
    TestEnvironment env;
    env.run(R"JS(
        var field = document.createElement('input');
        var box = document.createElement('div');
        var off = document.createElement('input');
        document.body.appendChild(field);
        document.body.appendChild(box);
        document.body.appendChild(off);
        off.disabled = true;
        field.focus();
        box.focus();
        off.focus();
    )JS");
    auto* field = editor_for(env, "field");
    auto* off = editor_for(env, "off");
    REQUIRE(field != nullptr);
    REQUIRE(off != nullptr);
    CHECK(field->has_focus());
    CHECK_FALSE(off->has_focus());
    CHECK(focused_input_under_root(env.root) == field);
}

TEST_CASE("Script focus: an autofocus element takes focus once when it mounts",
          "[events][focus][autofocus]") {
    TestEnvironment env;
    env.run(R"JS(
        var panel = document.createElement('div');
        var plain = document.createElement('input');
        var wanted = document.createElement('input');
        var later = document.createElement('input');
        wanted.setAttribute('autofocus', '');
        later.autofocus = true;
        panel.appendChild(plain);
        panel.appendChild(wanted);
        panel.appendChild(later);
        document.body.appendChild(panel);
    )JS");
    auto* plain = editor_for(env, "plain");
    auto* wanted = editor_for(env, "wanted");
    auto* later = editor_for(env, "later");
    REQUIRE(wanted != nullptr);
    CHECK(wanted->has_focus());
    CHECK_FALSE(plain->has_focus());
    CHECK_FALSE(later->has_focus());

    // Once the user (or script) moves on, re-inserting the autofocus element
    // must not pull focus back.
    env.eval("plain.focus(); panel.appendChild(wanted);");
    CHECK(plain->has_focus());
    CHECK_FALSE(wanted->has_focus());
}

TEST_CASE("Script focus: autofocus waits until the subtree reaches the document",
          "[events][focus][autofocus]") {
    TestEnvironment env;
    env.run(R"JS(
        var detached = document.createElement('div');
        var field = document.createElement('input');
        field.setAttribute('autofocus', '');
        detached.appendChild(field);
    )JS");
    auto* field = editor_for(env, "field");
    REQUIRE(field != nullptr);
    CHECK_FALSE(field->has_focus());

    env.eval("document.body.appendChild(detached);");
    CHECK(field->has_focus());
}

TEST_CASE("Script focus: a mounting dialog focuses its first text field",
          "[events][focus][autofocus][dialog]") {
    TestEnvironment env;
    env.run(R"JS(
        var dialog = document.createElement('div');
        dialog.setAttribute('role', 'dialog');
        var check = document.createElement('input');
        check.type = 'checkbox';
        var body = document.createElement('div');
        var name = document.createElement('input');
        var notes = document.createElement('textarea');
        dialog.appendChild(check);
        dialog.appendChild(body);
        body.appendChild(name);
        body.appendChild(notes);
        document.body.appendChild(dialog);
    )JS");
    auto* name = editor_for(env, "name");
    auto* notes = editor_for(env, "notes");
    REQUIRE(name != nullptr);
    REQUIRE(notes != nullptr);
    CHECK(name->has_focus());
    CHECK_FALSE(notes->has_focus());
    type_into_focused(env, "Preset 1");
    CHECK(name->text() == "Preset 1");
}

TEST_CASE("Script focus: an autofocus element inside a dialog wins over the first field",
          "[events][focus][autofocus][dialog]") {
    TestEnvironment env;
    env.run(R"JS(
        var dialog = document.createElement('div');
        dialog.setAttribute('aria-modal', 'true');
        var first = document.createElement('input');
        var second = document.createElement('input');
        second.setAttribute('autofocus', '');
        dialog.appendChild(first);
        dialog.appendChild(second);
        document.body.appendChild(dialog);
    )JS");
    CHECK(editor_for(env, "second")->has_focus());
    CHECK_FALSE(editor_for(env, "first")->has_focus());
}

TEST_CASE("Script focus: data-pulp-autofocus=off opts a dialog out",
          "[events][focus][autofocus][dialog]") {
    SECTION("on the dialog itself") {
        TestEnvironment env;
        env.run(R"JS(
            var dialog = document.createElement('div');
            dialog.setAttribute('role', 'dialog');
            dialog.setAttribute('data-pulp-autofocus', 'off');
            var field = document.createElement('input');
            dialog.appendChild(field);
            document.body.appendChild(dialog);
        )JS");
        CHECK_FALSE(editor_for(env, "field")->has_focus());
        CHECK(focused_input_under_root(env.root) == nullptr);
    }
    SECTION("app-wide on document.body") {
        TestEnvironment env;
        env.run(R"JS(
            document.body.setAttribute('data-pulp-autofocus', 'off');
            var dialog = document.createElement('div');
            dialog.setAttribute('role', 'dialog');
            var field = document.createElement('input');
            dialog.appendChild(field);
            document.body.appendChild(dialog);
        )JS");
        CHECK_FALSE(editor_for(env, "field")->has_focus());
    }
    SECTION("explicit autofocus is still honoured") {
        TestEnvironment env;
        env.run(R"JS(
            var dialog = document.createElement('div');
            dialog.setAttribute('role', 'dialog');
            dialog.setAttribute('data-pulp-autofocus', 'off');
            var field = document.createElement('input');
            field.setAttribute('autofocus', '');
            dialog.appendChild(field);
            document.body.appendChild(dialog);
        )JS");
        CHECK(editor_for(env, "field")->has_focus());
    }
}

TEST_CASE("Script focus: a hidden dialog does not take focus at mount",
          "[events][focus][autofocus][dialog]") {
    TestEnvironment env;
    env.run(R"JS(
        var dialog = document.createElement('div');
        dialog.setAttribute('role', 'dialog');
        dialog.style.display = 'none';
        var field = document.createElement('input');
        dialog.appendChild(field);
        document.body.appendChild(dialog);
    )JS");
    CHECK_FALSE(editor_for(env, "field")->has_focus());
    CHECK(focused_input_under_root(env.root) == nullptr);
}

TEST_CASE("Script focus: <dialog>.showModal() focuses its first text field each time it opens",
          "[events][focus][autofocus][dialog]") {
    TestEnvironment env;
    env.run(R"JS(
        var outside = document.createElement('input');
        var dialog = document.createElement('dialog');
        var field = document.createElement('input');
        dialog.appendChild(field);
        document.body.appendChild(outside);
        document.body.appendChild(dialog);
        outside.focus();
    )JS");
    auto* outside = editor_for(env, "outside");
    auto* field = editor_for(env, "field");
    REQUIRE(outside != nullptr);
    REQUIRE(field != nullptr);
    // A closed dialog is not rendered, so mounting it takes nothing.
    CHECK(outside->has_focus());
    CHECK_FALSE(field->has_focus());

    env.eval("dialog.showModal();");
    CHECK(field->has_focus());
    CHECK_FALSE(outside->has_focus());

    env.eval("dialog.close(); outside.focus(); dialog.showModal();");
    CHECK(field->has_focus());
}
