// Automated test for TextEditor keyboard input
// Tests the full pipeline: focus → text input → content verification → Enter callback

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <string>

using namespace pulp::view;

TEST_CASE("TextEditor: receives text input and updates content", "[text_editor][input]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});

    // Verify it's focusable (was missing before fix)
    REQUIRE(editor.focusable());

    // Simulate gaining focus
    editor.on_focus_changed(true);

    // Simulate typing "hello"
    for (char c : std::string("hello")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }

    REQUIRE(editor.text() == "hello");
}

TEST_CASE("TextEditor: on_return fires on Enter key", "[text_editor][input]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});

    std::string returned_text;
    editor.on_return = [&](const std::string& text) {
        returned_text = text;
    };

    editor.on_focus_changed(true);

    // Type "test"
    for (char c : std::string("test")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }

    REQUIRE(editor.text() == "test");

    // Press Enter
    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    editor.on_key_event(enter);

    REQUIRE(returned_text == "test");
}

TEST_CASE("TextEditor: backspace deletes characters", "[text_editor][input]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);

    // Type "abc"
    for (char c : std::string("abc")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }
    REQUIRE(editor.text() == "abc");

    // Press backspace
    KeyEvent bs;
    bs.key = KeyCode::backspace;
    bs.is_down = true;
    editor.on_key_event(bs);

    REQUIRE(editor.text() == "ab");
}

TEST_CASE("TextEditor: on_change fires on each character", "[text_editor][input]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});

    int change_count = 0;
    editor.on_change = [&](const std::string&) {
        ++change_count;
    };

    editor.on_focus_changed(true);

    TextInputEvent te;
    te.text = "a";
    editor.on_text_input(te);
    te.text = "b";
    editor.on_text_input(te);
    te.text = "c";
    editor.on_text_input(te);

    REQUIRE(change_count == 3);
    REQUIRE(editor.text() == "abc");
}

TEST_CASE("TextEditor: placeholder shown when empty", "[text_editor][input]") {
    TextEditor editor;
    editor.placeholder = "Type here...";
    REQUIRE(editor.text().empty());
    // Placeholder is a rendering concern, but verify it doesn't interfere with text
    editor.on_focus_changed(true);
    TextInputEvent te;
    te.text = "x";
    editor.on_text_input(te);
    REQUIRE(editor.text() == "x");
}

TEST_CASE("TextEditor: focusable by default after fix", "[text_editor][input]") {
    TextEditor editor;
    REQUIRE(editor.focusable() == true);
}

// ── Undo / redo ─────────────────────────────────────────────────────────

TEST_CASE("TextEditor: undo restores the previous text content",
          "[text_editor][undo]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);
    for (char c : std::string("hello")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }
    REQUIRE(editor.text() == "hello");

    auto before = editor.text();
    REQUIRE(editor.undo());
    REQUIRE(editor.text() != before);
    REQUIRE(editor.text().size() < before.size());
}

TEST_CASE("TextEditor: redo reapplies an undone edit",
          "[text_editor][undo][redo]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);

    TextInputEvent te;
    te.text = "x";
    editor.on_text_input(te);
    REQUIRE(editor.text() == "x");

    REQUIRE(editor.undo());
    REQUIRE(editor.redo());
    REQUIRE(editor.text() == "x");
}

// ── Selection ───────────────────────────────────────────────────────────

TEST_CASE("TextEditor: select_all + backspace clears the buffer",
          "[text_editor][selection]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);
    for (char c : std::string("delete-me")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }
    REQUIRE(editor.text() == "delete-me");

    editor.select_all();
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "delete-me");

    KeyEvent bs;
    bs.key = KeyCode::backspace;
    bs.is_down = true;
    editor.on_key_event(bs);
    REQUIRE(editor.text().empty());
}

TEST_CASE("TextEditor: clear_selection leaves text intact",
          "[text_editor][selection]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);
    TextInputEvent te;
    te.text = "abc";
    editor.on_text_input(te);
    editor.select_all();
    REQUIRE(editor.has_selection());

    editor.clear_selection();
    REQUIRE_FALSE(editor.has_selection());
    REQUIRE(editor.text() == "abc");
}

// ── set_text + caret position ───────────────────────────────────────────

TEST_CASE("TextEditor: set_text replaces buffer and bounds the caret",
          "[text_editor][caret]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.on_focus_changed(true);
    TextInputEvent te;
    te.text = "old";
    editor.on_text_input(te);
    REQUIRE(editor.text() == "old");
    REQUIRE(editor.caret_pos() >= 0);
    REQUIRE(editor.caret_pos() <= 3);

    editor.set_text("replaced");
    REQUIRE(editor.text() == "replaced");
    REQUIRE(editor.caret_pos() >= 0);
    REQUIRE(editor.caret_pos() <= static_cast<int>(editor.text().size()));
}

// ── Numeric-only / password-mode invariants ─────────────────────────────

TEST_CASE("TextEditor: numeric_only drops alphabetic input",
          "[text_editor][numeric]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.numeric_only = true;
    editor.on_focus_changed(true);
    for (char c : std::string("1a2b3c")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }
    for (char c : editor.text()) {
        INFO("unexpected non-digit: " << c);
        REQUIRE((c >= '0' && c <= '9'));
    }
    REQUIRE(editor.text() == "123");
}

TEST_CASE("TextEditor: password_mode preserves the underlying text",
          "[text_editor][password]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 30});
    editor.password_mode = true;
    editor.on_focus_changed(true);
    for (char c : std::string("s3cr3t")) {
        TextInputEvent te;
        te.text = std::string(1, c);
        editor.on_text_input(te);
    }
    // Password masking is a rendering concern — text() returns the
    // real buffer so the host can validate user input.
    REQUIRE(editor.text() == "s3cr3t");
}
