#include <catch2/catch_test_macros.hpp>
#include "support/text_editor_test_utils.hpp"

#include <pulp/view/context_menu.hpp>

#include <memory>

using namespace pulp::view;
using namespace pulp::canvas;
using namespace pulp::test;

TEST_CASE("TextEditor mouse double-click selects word", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    MouseEvent e;
    e.position = {10, 15}; // Near start of text
    e.click_count = 2;
    e.is_down = true;
    editor.on_mouse_event(e);
    REQUIRE(editor.has_selection());
}

TEST_CASE("TextEditor mouse shift-click extends selection from the caret",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    MouseEvent click;
    click.position = {9.0f + 5.0f * char_w, 15};
    click.is_down = true;
    editor.on_mouse_event(click);
    REQUIRE_FALSE(editor.has_selection());

    MouseEvent shift_click;
    shift_click.position = {9.0f + 2.0f * char_w, 15};
    shift_click.modifiers = kModShift;
    shift_click.is_down = true;
    editor.on_mouse_event(shift_click);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "llo");

    MouseEvent key_up = shift_click;
    key_up.is_down = false;
    key_up.position = {9.0f, 15};
    editor.on_mouse_event(key_up);
    REQUIRE(editor.selected_text() == "llo");
}

TEST_CASE("TextEditor rich press suppresses the paired legacy press",
          "[view][text_editor][selection][mouse][plugin-host]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    MouseEvent click;
    click.position = {9.0f + 5.0f * char_w, 15.0f};
    click.is_down = true;
    editor.on_mouse_event(click);
    editor.on_mouse_down(click.position);

    MouseEvent shift_click;
    shift_click.position = {9.0f + 2.0f * char_w, 15.0f};
    shift_click.modifiers = kModShift;
    shift_click.is_down = true;
    editor.on_mouse_event(shift_click);
    editor.on_mouse_down(shift_click.position);

    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "llo");
}

TEST_CASE("TextEditor explicit mouse drag selects text",
          "[view][text_editor][selection][drag]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    MouseEvent press;
    press.position = {9.0f + 2.0f * char_w, 15.0f};
    press.is_down = true;
    press.phase = MousePhase::press;
    editor.on_mouse_event(press);
    REQUIRE_FALSE(editor.has_selection());

    MouseEvent drag = press;
    drag.position = {9.0f + 7.0f * char_w, 15.0f};
    drag.phase = MousePhase::drag;
    editor.on_mouse_event(drag);

    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "llo w");

    MouseEvent release = drag;
    release.is_down = false;
    release.phase = MousePhase::release;
    editor.on_mouse_event(release);

    drag.position = {9.0f + 10.0f * char_w, 15.0f};
    editor.on_mouse_event(drag);
    REQUIRE(editor.selected_text() == "llo w");
}

TEST_CASE("TextEditor legacy simulate_drag selects text",
          "[view][text_editor][selection][drag]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    editor.simulate_drag({9.0f + 2.0f * char_w, 15.0f},
                         {9.0f + 7.0f * char_w, 15.0f},
                         2);

    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "llo w");
}

TEST_CASE("TextEditor host mouse press plus legacy drag selects text",
          "[view][text_editor][selection][drag][plugin-host]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    const Point press_pos{9.0f + 2.0f * char_w, 15.0f};
    const Point drag_pos{9.0f + 7.0f * char_w, 15.0f};

    MouseEvent press;
    press.position = press_pos;
    press.is_down = true;
    press.phase = MousePhase::press;
    editor.on_mouse_event(press);
    editor.on_mouse_down(press_pos);
    editor.on_mouse_drag(drag_pos);

    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "llo w");
}

TEST_CASE("TextEditor mouse double-click selects the exact word under the cursor",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.set_text("alpha beta_2!");
    editor.set_bounds({0, 0, 200, 30});

    constexpr float char_w = 13.0f * 0.6f;
    MouseEvent e;
    e.position = {9.0f + 8.0f * char_w, 15};
    e.click_count = 2;
    e.is_down = true;
    editor.on_mouse_event(e);

    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "beta_2!");
}

TEST_CASE("TextEditor double-click drag extends selection by words",
          "[view][text_editor][selection][mouse]") {
    constexpr float char_w = 13.0f * 0.6f;
    auto x_for_index = [](int index) {
        return 9.0f + static_cast<float>(index) * char_w;
    };

    TextEditor right;
    right.set_text("alpha beta gamma");
    right.set_bounds({0, 0, 240, 30});

    MouseEvent double_click;
    double_click.position = {x_for_index(8), 15.0f};
    double_click.click_count = 2;
    double_click.is_down = true;
    right.on_mouse_event(double_click);
    REQUIRE(right.selected_text() == "beta");

    right.on_mouse_drag({x_for_index(13), 15.0f});
    REQUIRE(right.selected_text() == "beta gamma");

    right.on_mouse_up({x_for_index(13), 15.0f});
    const auto selected_after_release = right.selected_text();
    right.on_mouse_drag({x_for_index(16), 15.0f});
    REQUIRE(right.selected_text() == selected_after_release);

    TextEditor left;
    left.set_text("alpha beta gamma");
    left.set_bounds({0, 0, 240, 30});
    left.on_mouse_event(double_click);
    REQUIRE(left.selected_text() == "beta");

    left.on_mouse_drag({x_for_index(2), 15.0f});
    REQUIRE(left.selected_text() == "alpha beta");
}

TEST_CASE("TextEditor rich mouse drag captures and releases the pointer",
          "[view][text_editor][selection][mouse][capture]") {
    TextEditor editor;
    editor.set_text("alpha beta");
    editor.set_bounds({0, 0, 200, 30});

    MouseEvent down;
    down.position = {10.0f, 15.0f};
    down.pointer_id = 9;
    down.phase = MousePhase::press;
    down.is_down = true;
    editor.on_mouse_event(down);
    REQUIRE(editor.has_pointer_capture(9));

    MouseEvent drag = down;
    drag.position = {80.0f, 15.0f};
    drag.phase = MousePhase::drag;
    drag.is_down = true;
    editor.on_mouse_event(drag);
    REQUIRE(editor.has_selection());

    MouseEvent up = down;
    up.position = {80.0f, 15.0f};
    up.phase = MousePhase::release;
    up.is_down = false;
    editor.on_mouse_event(up);
    REQUIRE_FALSE(editor.has_pointer_capture(9));
}

TEST_CASE("TextEditor rich mouse release clears capture after disabling mid-drag",
          "[view][text_editor][selection][mouse][capture]") {
    TextEditor editor;
    editor.set_text("alpha beta");
    editor.set_bounds({0, 0, 200, 30});

    MouseEvent down;
    down.position = {10.0f, 15.0f};
    down.pointer_id = 11;
    down.phase = MousePhase::press;
    down.is_down = true;
    editor.on_mouse_event(down);
    REQUIRE(editor.has_pointer_capture(11));

    editor.set_enabled(false);

    MouseEvent up = down;
    up.phase = MousePhase::release;
    up.is_down = false;
    editor.on_mouse_event(up);
    REQUIRE_FALSE(editor.has_pointer_capture(11));
}

TEST_CASE("TextEditor disabled legacy drag cancels capture and does not select",
          "[view][text_editor][selection][mouse][capture]") {
    TextEditor editor;
    editor.set_text("alpha beta");
    editor.set_bounds({0, 0, 200, 30});

    MouseEvent down;
    down.position = {10.0f, 15.0f};
    down.pointer_id = 12;
    down.is_down = true;
    editor.on_mouse_event(down);
    REQUIRE(editor.has_pointer_capture(12));

    editor.set_enabled(false);
    editor.on_mouse_drag({120.0f, 15.0f});

    REQUIRE_FALSE(editor.has_pointer_capture(12));
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor mouse triple-click selects all", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    MouseEvent e;
    e.position = {10, 15};
    e.click_count = 3;
    e.is_down = true;
    editor.on_mouse_event(e);
    REQUIRE(editor.selected_text() == "hello world");
}

TEST_CASE("TextEditor mouse triple-click selects the current multiline row",
          "[view][text_editor][mouse][selection]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_text("alpha\nbeta gamma\nomega");
    editor.set_bounds({0, 0, 260, 90});

    RecordingCanvas canvas;
    editor.paint(canvas);

    MouseEvent e;
    e.position = {20.0f, 26.0f};
    e.click_count = 3;
    e.is_down = true;
    editor.on_mouse_event(e);

    REQUIRE(editor.selected_text() == "beta gamma");
}

TEST_CASE("TextEditor ignores wheel input in single-line mode",
          "[view][text_editor]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("abcdef");
    REQUIRE(editor.on_key_event(key_event(KeyCode::left, main_modifier())));
    REQUIRE(editor.caret_pos() == 0);

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 40.0f;
    editor.on_mouse_event(wheel);

    REQUIRE(editor.caret_pos() == 0);
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor multi-line wheel clamps scroll offset before hit testing",
          "[view][text_editor]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_text("one\ntwo\nthree\nfour");
    editor.set_bounds({0, 0, 120, 28});

    RecordingCanvas canvas;
    editor.paint(canvas);

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = -100.0f;
    editor.on_mouse_event(wheel);
    editor.paint(canvas);

    MouseEvent click;
    click.is_down = true;
    click.position = {8.0f, 6.0f};
    editor.on_mouse_event(click);
    REQUIRE(editor.caret_pos() >= 0);
    REQUIRE(editor.caret_pos() <= static_cast<int>(editor.text().size()));
}

TEST_CASE("TextEditor multiline drag autoscroll keeps y-aware hit testing",
          "[view][text_editor][selection][drag][scroll]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_bounds({0, 0, 90, 40});
    editor.set_text("aaaa\nbbbb\ncccc\ndddd");

    RecordingCanvas canvas;
    editor.paint(canvas);

    MouseEvent press;
    press.is_down = true;
    press.phase = MousePhase::press;
    press.position = {6.0f, 6.0f};
    editor.on_mouse_event(press);
    REQUIRE(editor.caret_pos() == 0);

    MouseEvent drag = press;
    drag.phase = MousePhase::drag;
    drag.position = {6.0f, 120.0f};
    editor.on_mouse_event(drag);

    REQUIRE(editor.scroll_offset() > 0.0f);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.caret_pos() >= 15);
}

TEST_CASE("TextEditor default context menu routes standard edit commands",
          "[view][text_editor][context-menu]") {
    require_system_clipboard_text("text-editor-context-menu");

    View::focused_input_ = nullptr;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 400, 300});

    auto owned_editor = std::make_unique<TextEditor>();
    TextEditor* editor = owned_editor.get();
    editor->set_bounds({20, 30, 220, 32});
    editor->on_focus_changed(true);
    editor->set_text("alpha");
    editor->select_all();
    editor->claim_input_focus();
    root->add_child(std::move(owned_editor));
    REQUIRE(static_cast<bool>(editor->on_context_menu));
    REQUIRE(View::focused_input_ == editor);

    MouseEvent right_click;
    right_click.button = MouseButton::right;
    right_click.position = {10.0f, 10.0f};
    right_click.phase = MousePhase::press;
    right_click.is_down = true;

    editor->on_mouse_event(right_click);
    REQUIRE(root->child_count() == 2);
    auto* menu = dynamic_cast<ContextMenu*>(root->child_at(1));
    REQUIRE(menu != nullptr);
    REQUIRE(View::focused_input_ == menu);
    REQUIRE(menu->items().size() == 5);
    REQUIRE(menu->items()[0].label == "Cut");
    REQUIRE(menu->items()[0].enabled);
    REQUIRE(menu->items()[1].label == "Copy");
    REQUIRE(menu->items()[1].enabled);
    REQUIRE(menu->items()[2].label == "Paste");

    MouseEvent choose;
    choose.is_down = true;
    choose.position = {50.0f, 30.0f + 10.0f + 12.0f};
    menu->on_mouse_event(choose);

    REQUIRE(root->child_count() == 1);
    REQUIRE(editor->text().empty());
    REQUIRE(View::focused_input_ == editor);
    auto cut_text = pulp::platform::Clipboard::get_text();
    REQUIRE(cut_text.has_value());
    REQUIRE(*cut_text == "alpha");

    require_system_clipboard_text("beta");
    editor->on_context_menu({10.0f, 10.0f});
    REQUIRE(root->child_count() == 2);
    menu = dynamic_cast<ContextMenu*>(root->child_at(1));
    REQUIRE(menu != nullptr);
    REQUIRE(View::focused_input_ == menu);
    REQUIRE(menu->items()[2].enabled);

    choose.position = {50.0f, 30.0f + 10.0f + 2.0f * 24.0f + 12.0f};
    menu->on_mouse_event(choose);

    REQUIRE(root->child_count() == 1);
    REQUIRE(editor->text() == "beta");
    REQUIRE(View::focused_input_ == editor);

    editor->select_all();
    editor->on_context_menu({10.0f, 10.0f});
    REQUIRE(root->child_count() == 2);
    menu = dynamic_cast<ContextMenu*>(root->child_at(1));
    REQUIRE(menu != nullptr);

    MouseEvent dismiss;
    dismiss.is_down = true;
    dismiss.position = {390.0f, 290.0f};
    menu->on_mouse_event(dismiss);

    REQUIRE(root->child_count() == 1);
    REQUIRE(View::focused_input_ == editor);
}
