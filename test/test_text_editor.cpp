#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

KeyEvent key_event(KeyCode key, uint16_t modifiers = 0) {
    KeyEvent event;
    event.key = key;
    event.modifiers = modifiers;
    event.is_down = true;
    return event;
}

uint16_t main_modifier() {
#ifdef __APPLE__
    return kModCmd;
#else
    return kModCtrl;
#endif
}

} // namespace

TEST_CASE("TextEditor set and get text", "[view][text_editor]") {
    TextEditor editor;
    REQUIRE(editor.is_empty());

    editor.set_text("Hello");
    REQUIRE(editor.text() == "Hello");
    REQUIRE_FALSE(editor.is_empty());
}

TEST_CASE("TextEditor select all and selected text", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("World");
    REQUIRE_FALSE(editor.has_selection());

    editor.select_all();
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "World");
}

TEST_CASE("TextEditor clear selection", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Test");
    editor.select_all();
    REQUIRE(editor.has_selection());

    editor.clear_selection();
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor undo/redo", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("First");
    editor.set_text("Second");
    REQUIRE(editor.text() == "Second");

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "First");

    REQUIRE(editor.redo());
    REQUIRE(editor.text() == "Second");
}

TEST_CASE("TextEditor undo on empty history returns false", "[view][text_editor]") {
    TextEditor editor;
    REQUIRE_FALSE(editor.undo());
    REQUIRE_FALSE(editor.redo());
}

TEST_CASE("TextEditor on_change callback fires", "[view][text_editor]") {
    TextEditor editor;
    std::string changed_text;
    editor.on_change = [&](const std::string& t) { changed_text = t; };

    editor.set_text("Callback");
    REQUIRE(changed_text == "Callback");
}

TEST_CASE("TextEditor text input inserts characters", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("");

    TextInputEvent e;
    e.text = "abc";
    editor.on_text_input(e);
    REQUIRE(editor.text() == "abc");
}

TEST_CASE("TextEditor text input replaces the active selection",
          "[view][text_editor][coverage]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("alpha beta");
    editor.select_all();

    TextInputEvent e;
    e.text = "omega";
    editor.on_text_input(e);

    REQUIRE(editor.text() == "omega");
    REQUIRE(editor.caret_pos() == 5);
    REQUIRE_FALSE(editor.has_selection());

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "alpha beta");
}

TEST_CASE("TextEditor numeric mode rejects non-digits", "[view][text_editor]") {
    TextEditor editor;
    editor.numeric_only = true;
    editor.set_text("");

    TextInputEvent e;
    e.text = "x";
    editor.on_text_input(e);
    REQUIRE(editor.text().empty());

    e.text = "5";
    editor.on_text_input(e);
    REQUIRE(editor.text() == "5");

    e.text = ".";
    editor.on_text_input(e);
    REQUIRE(editor.text() == "5.");
}

TEST_CASE("TextEditor numeric mode rejects mixed text atomically",
          "[view][text_editor][coverage]") {
    TextEditor editor;
    editor.numeric_only = true;
    editor.on_focus_changed(true);
    editor.set_text("12");

    TextInputEvent e;
    e.text = "3x";
    editor.on_text_input(e);
    REQUIRE(editor.text() == "12");
    REQUIRE(editor.caret_pos() == 2);

    e.text = "-4.5";
    editor.on_text_input(e);
    REQUIRE(editor.text() == "12-4.5");
}

TEST_CASE("TextEditor key event: Enter triggers on_return", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Result");

    std::string returned;
    editor.on_return = [&](const std::string& t) { returned = t; };

    KeyEvent e;
    e.key = KeyCode::enter;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(returned == "Result");
}

TEST_CASE("TextEditor key event: Escape triggers on_escape", "[view][text_editor]") {
    TextEditor editor;
    bool escaped = false;
    editor.on_escape = [&] { escaped = true; };

    KeyEvent e;
    e.key = KeyCode::escape;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(escaped);
}

TEST_CASE("TextEditor key event: Cmd+A selects all", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Select me");

    KeyEvent e;
    e.key = KeyCode::a;
#ifdef __APPLE__
    e.modifiers = kModCmd;
#else
    e.modifiers = kModCtrl;
#endif
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "Select me");
}

TEST_CASE("TextEditor key event: Backspace deletes before caret", "[view][text_editor]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("AB");

    KeyEvent e;
    e.key = KeyCode::backspace;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(editor.text() == "A");
}

TEST_CASE("TextEditor key event: Up goes to start in single-line", "[view][text_editor]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("Hello");

    KeyEvent e;
    e.key = KeyCode::up;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    // Up in single-line moves to start — verify by typing
}

TEST_CASE("TextEditor ignores key-up and unhandled keys", "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.set_text("abc");

    auto key_up = key_event(KeyCode::left);
    key_up.is_down = false;
    REQUIRE_FALSE(editor.on_key_event(key_up));
    REQUIRE_FALSE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor.text() == "abc");
}

TEST_CASE("TextEditor single-line navigation treats embedded newlines as plain text", "[view][text_editor]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("aa\nbb\ncc");

    REQUIRE(editor.caret_pos() == 8);

    REQUIRE(editor.on_key_event(key_event(KeyCode::up)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 8);

    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::end_)));
    REQUIRE(editor.caret_pos() == 8);
}

TEST_CASE("TextEditor word and modifier navigation clamp to expected caret positions",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    const std::string text = "alpha beta_gamma 42";
    editor.set_text(text);

    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, kModAlt)));
    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()) - 2);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, kModAlt)));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModAlt)));
    REQUIRE(editor.caret_pos() == 16);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, main_modifier())));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, main_modifier())));
    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()));
}

TEST_CASE("TextEditor shift navigation extends and clears selection",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("abcd");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, main_modifier())));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModShift)));
    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModShift)));
    REQUIRE(editor.caret_pos() == 2);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "ab");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
    REQUIRE(editor.caret_pos() == 1);
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor multi-line up and down preserve the visual column", "[view][text_editor]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("abc\nde\nfghi");

    REQUIRE(editor.caret_pos() == 11);

    REQUIRE(editor.on_key_event(key_event(KeyCode::up)));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::up)));
    REQUIRE(editor.caret_pos() == 2);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 9);
}

TEST_CASE("TextEditor multi-line home and end move within the current line", "[view][text_editor]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("abc\ndefg\nhi");

    REQUIRE(editor.on_key_event(key_event(KeyCode::up)));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 4);

    REQUIRE(editor.on_key_event(key_event(KeyCode::end_)));
    REQUIRE(editor.caret_pos() == 8);
}

TEST_CASE("TextEditor shift-up extends multi-line selection", "[view][text_editor]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("abcd\nef\nghij");

    REQUIRE(editor.on_key_event(key_event(KeyCode::up, kModShift)));
    REQUIRE(editor.caret_pos() == 7);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "\nghij");

    REQUIRE(editor.on_key_event(key_event(KeyCode::up, kModShift)));
    REQUIRE(editor.caret_pos() == 2);
    REQUIRE(editor.selected_text() == "cd\nef\nghij");
}

TEST_CASE("TextEditor multi-line Enter inserts a newline instead of returning", "[view][text_editor]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("alpha");

    bool returned = false;
    editor.on_return = [&](const std::string&) { returned = true; };

    REQUIRE(editor.on_key_event(key_event(KeyCode::enter)));
    REQUIRE(editor.text() == "alpha\n");
    REQUIRE_FALSE(returned);
}

TEST_CASE("TextEditor modified multi-line Enter triggers return callback",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("alpha");

    std::string returned;
    editor.on_return = [&](const std::string& text) { returned = text; };

    REQUIRE(editor.on_key_event(key_event(KeyCode::enter, main_modifier())));
    REQUIRE(editor.text() == "alpha");
    REQUIRE(returned == "alpha");
}

TEST_CASE("TextEditor Delete removes selected text and undo restores it",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("Delete me");
    editor.select_all();

    REQUIRE(editor.on_key_event(key_event(KeyCode::delete_)));
    REQUIRE(editor.text().empty());
    REQUIRE_FALSE(editor.has_selection());

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "Delete me");
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
}

TEST_CASE("TextEditor Delete removes the character after the caret and redo reapplies it",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("abc");

    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::delete_)));
    REQUIRE(editor.text() == "bc");
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "abc");
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.redo());
    REQUIRE(editor.text() == "bc");
    REQUIRE(editor.caret_pos() == 0);
}

TEST_CASE("TextEditor marked text replacement tracks the active range",
          "[view][text_editor][ime][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text(" draft", 0, 0);
    REQUIRE(editor.text() == "base draft");
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{4, 6});
    REQUIRE(editor.caret_pos() == 10);

    editor.set_marked_text(" final", 0, 0);
    REQUIRE(editor.text() == "base final");
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{4, 6});
    REQUIRE(editor.caret_pos() == 10);

    editor.unmark_text();
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{0, 0});
    REQUIRE(editor.text() == "base final");
}

TEST_CASE("TextEditor empty marked text clears the previous composition",
          "[view][text_editor][ime][coverage]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text(" draft", 0, 0);
    REQUIRE(editor.text() == "base draft");
    REQUIRE(editor.has_marked_text());

    editor.set_marked_text("", 0, 0);
    REQUIRE(editor.text() == "base");
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{4, 0});
    REQUIRE(editor.caret_pos() == 4);
}

TEST_CASE("TextEditor caret_rect has a fallback before first paint",
          "[view][text_editor][coverage]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 24});
    editor.set_text("abc");

    auto rect = editor.caret_rect();
    REQUIRE(rect.x >= 9.0f);
    REQUIRE(rect.y == 2.0f);
    REQUIRE(rect.width == 1.5f);
    REQUIRE(rect.height >= 13.0f);
}

TEST_CASE("TextEditor multi-line paint renders placeholder when unfocused",
          "[view][text_editor][paint][issue-493]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.placeholder = "Type notes";
    editor.set_bounds({0, 0, 180, 64});

    RecordingCanvas canvas;
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == "Type notes") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor paint produces draw commands", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.set_text("Paint test");

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("TextEditor paint clamps shell radius and insets the inner fill", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 20});
    editor.set_border(Color::hex(0xFFFFFF), 2.0f, 20.0f);
    editor.set_text("Paint test");

    RecordingCanvas canvas;
    editor.paint(canvas);

    std::vector<DrawCommand> rounded;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_rounded_rect) rounded.push_back(cmd);
    }

    REQUIRE(rounded.size() >= 2);
    const auto& outer = rounded[0];
    const auto& inner = rounded[1];

    REQUIRE(outer.f[4] <= 9.5f);
    REQUIRE(inner.f[0] == 2.0f);
    REQUIRE(inner.f[1] == 2.0f);
    REQUIRE(inner.f[2] == 116.0f);
    REQUIRE(inner.f[3] == 16.0f);
    REQUIRE(inner.f[4] < outer.f[4]);
}

TEST_CASE("TextEditor paint renders a visible selection highlight and split text", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 180, 28});
    editor.set_text("Select me");
    editor.select_all();

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("TextEditor paint keeps unfocused single-line text anchored to the start", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 26});
    editor.set_text("Some text");

    RecordingCanvas canvas;
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::fill_text || cmd.text != "Some text") continue;
        REQUIRE(cmd.f[0] >= 6.0f);
        REQUIRE(cmd.f[0] <= 20.0f);
        found = true;
        break;
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor paint resets canvas text alignment before drawing", "[view][text_editor]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 120, 26});
    editor.set_text("Some text");

    RecordingCanvas canvas;
    canvas.set_text_align(TextAlign::center);
    editor.paint(canvas);

    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::fill_text || cmd.text != "Some text") continue;
        REQUIRE(cmd.f[0] >= 6.0f);
        REQUIRE(cmd.f[0] <= 20.0f);
        found = true;
        break;
    }
    REQUIRE(found);
}

TEST_CASE("TextEditor password mode masks text", "[view][text_editor]") {
    TextEditor editor;
    editor.password_mode = true;
    editor.password_char = '*';
    editor.set_text("secret");
    editor.set_bounds({0, 0, 200, 30});

    RecordingCanvas canvas;
    editor.paint(canvas);

    // Should have rendered but with masked characters
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("TextEditor select-on-focus selects all on focus", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Focus me");
    editor.select_on_focus = true;

    editor.on_focus_changed(true);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "Focus me");
}

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
    REQUIRE(editor.selected_text() == "beta_2");
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

TEST_CASE("TextEditor ignores wheel input in single-line mode",
          "[view][text_editor][coverage]") {
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
          "[view][text_editor][coverage]") {
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
