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
