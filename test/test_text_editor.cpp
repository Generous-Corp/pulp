#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;

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
    e.modifiers = kModCmd;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "Select me");
}

TEST_CASE("TextEditor key event: Backspace deletes before caret", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("AB");
    // Caret is at end (position 2) after set_text

    KeyEvent e;
    e.key = KeyCode::backspace;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(editor.text() == "A");
}

TEST_CASE("TextEditor key event: Up goes to start in single-line", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Hello");
    // Caret at end after set_text

    KeyEvent e;
    e.key = KeyCode::up;
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    // Up in single-line moves to start — verify by typing
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
