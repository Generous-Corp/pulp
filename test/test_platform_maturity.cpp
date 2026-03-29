// Platform maturity tests: cursor, focus traversal, IME, context menu, accessibility
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>

using namespace pulp::view;

// ═══════════════════════════════════════════════════════════════════════════════
// Cursor management
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("View: default cursor is default_", "[view][cursor]") {
    View v;
    REQUIRE(v.cursor() == View::CursorStyle::default_);
}

TEST_CASE("View: set_cursor changes cursor style", "[view][cursor]") {
    View v;
    v.set_cursor(View::CursorStyle::pointer);
    REQUIRE(v.cursor() == View::CursorStyle::pointer);

    v.set_cursor(View::CursorStyle::text);
    REQUIRE(v.cursor() == View::CursorStyle::text);

    v.set_cursor(View::CursorStyle::crosshair);
    REQUIRE(v.cursor() == View::CursorStyle::crosshair);

    v.set_cursor(View::CursorStyle::grab);
    REQUIRE(v.cursor() == View::CursorStyle::grab);

    v.set_cursor(View::CursorStyle::grabbing);
    REQUIRE(v.cursor() == View::CursorStyle::grabbing);

    v.set_cursor(View::CursorStyle::not_allowed);
    REQUIRE(v.cursor() == View::CursorStyle::not_allowed);
}

TEST_CASE("TextEditor: default cursor is text", "[view][cursor]") {
    TextEditor te;
    REQUIRE(te.cursor() == View::CursorStyle::text);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Focus traversal
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Focus: Tab cycles forward through focusable views", "[view][focus]") {
    View root;
    auto a = std::make_unique<Knob>(); a->set_bounds({0, 0, 40, 40});
    auto b = std::make_unique<Fader>(); b->set_bounds({50, 0, 20, 80});
    auto c = std::make_unique<Toggle>(); c->set_bounds({80, 0, 40, 20});
    auto* pa = a.get(); auto* pb = b.get(); auto* pc = c.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.add_child(std::move(c));

    // First tab: focus first
    auto* focused = View::focus_next(root, nullptr);
    REQUIRE(focused == pa);
    REQUIRE(pa->has_focus());

    // Second tab: advance
    focused = View::focus_next(root, focused);
    REQUIRE(focused == pb);

    // Third tab: advance
    focused = View::focus_next(root, focused);
    REQUIRE(focused == pc);

    // Wrap around
    focused = View::focus_next(root, focused);
    REQUIRE(focused == pa);
}

TEST_CASE("Focus: Shift+Tab cycles backward", "[view][focus]") {
    View root;
    auto a = std::make_unique<Knob>(); a->set_bounds({0, 0, 40, 40});
    auto b = std::make_unique<Fader>(); b->set_bounds({50, 0, 20, 80});
    auto* pa = a.get(); auto* pb = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));

    // Start at second
    auto* focused = View::focus_next(root, nullptr); // -> a
    focused = View::focus_next(root, focused);        // -> b
    REQUIRE(focused == pb);

    // Shift+Tab back
    focused = View::focus_prev(root, focused);
    REQUIRE(focused == pa);
}

TEST_CASE("Focus: Skips non-focusable views", "[view][focus]") {
    View root;
    auto label = std::make_unique<Label>("Info");
    label->set_bounds({0, 0, 100, 20});
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({0, 30, 40, 40});
    auto* pk = knob.get();
    root.add_child(std::move(label)); // Not focusable
    root.add_child(std::move(knob));  // Focusable

    auto* focused = View::focus_next(root, nullptr);
    REQUIRE(focused == pk); // Skipped label, went to knob
}

// ═══════════════════════════════════════════════════════════════════════════════
// Context menu
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("View: on_context_menu callback fires", "[view][context_menu]") {
    View v;
    v.set_bounds({0, 0, 100, 100});
    bool fired = false;
    Point menu_pos{};
    v.on_context_menu = [&](Point pos) { fired = true; menu_pos = pos; };

    // Simulate right-click
    v.on_context_menu({25.0f, 30.0f});
    REQUIRE(fired);
    REQUIRE(menu_pos.x == 25.0f);
    REQUIRE(menu_pos.y == 30.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// IME composition (marked text)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("TextEditor: IME set_marked_text inserts composition", "[view][ime]") {
    TextEditor te;
    te.on_focus_changed(true);
    // Type some text first
    TextInputEvent e; e.text = "hello";
    te.on_text_input(e);
    REQUIRE(te.text() == "hello");

    // Start IME composition
    te.set_marked_text("ni", 0, 2);
    REQUIRE(te.has_marked_text());
    REQUIRE(te.text() == "helloni"); // Marked text appended at caret
    auto [start, len] = te.marked_range();
    REQUIRE(start == 5);
    REQUIRE(len == 2);
}

TEST_CASE("TextEditor: IME unmark_text commits composition", "[view][ime]") {
    TextEditor te;
    te.on_focus_changed(true);
    TextInputEvent e; e.text = "a";
    te.on_text_input(e);

    te.set_marked_text("bc", 0, 2);
    REQUIRE(te.has_marked_text());
    REQUIRE(te.text() == "abc");

    te.unmark_text();
    REQUIRE_FALSE(te.has_marked_text());
    REQUIRE(te.text() == "abc"); // Text preserved
}

TEST_CASE("TextEditor: IME set_marked_text replaces previous composition", "[view][ime]") {
    TextEditor te;
    te.on_focus_changed(true);

    // First composition
    te.set_marked_text("ni", 0, 2);
    REQUIRE(te.text() == "ni");

    // Update composition (replaces "ni" with "nihao")
    te.set_marked_text("nihao", 0, 5);
    REQUIRE(te.text() == "nihao");
    REQUIRE(te.has_marked_text());
    auto [start, len] = te.marked_range();
    REQUIRE(len == 5);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Accessibility
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Knob: has slider AccessRole", "[view][accessibility]") {
    Knob k;
    REQUIRE(k.access_role() == View::AccessRole::slider);
    REQUIRE(k.focusable());
}

TEST_CASE("Toggle: has toggle AccessRole", "[view][accessibility]") {
    Toggle t;
    REQUIRE(t.access_role() == View::AccessRole::toggle);
}

TEST_CASE("Label: has label AccessRole", "[view][accessibility]") {
    Label l("test");
    REQUIRE(l.access_role() == View::AccessRole::label);
}

TEST_CASE("View: access_label and access_value", "[view][accessibility]") {
    Knob k;
    k.set_access_label("Volume");
    k.set_access_value("75%");
    REQUIRE(k.access_label() == "Volume");
    REQUIRE(k.access_value() == "75%");
}

TEST_CASE("Meter: has meter AccessRole", "[view][accessibility]") {
    Meter m;
    REQUIRE(m.access_role() == View::AccessRole::meter);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Enabled/disabled
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("View: enabled default is true", "[view][enabled]") {
    View v;
    REQUIRE(v.enabled());
}

TEST_CASE("View: set_enabled changes state", "[view][enabled]") {
    Knob k;
    k.set_enabled(false);
    REQUIRE_FALSE(k.enabled());
    k.set_enabled(true);
    REQUIRE(k.enabled());
}
