#include <catch2/catch_test_macros.hpp>
#include "support/text_editor_test_utils.hpp"

#include <cctype>
#include <string_view>
#include <vector>

using namespace pulp::view;
using namespace pulp::test;

namespace {

bool is_cluster_boundary(const std::string& text, int pos) {
    if (pos < 0 || pos > static_cast<int>(text.size())) return false;
    if (pos == 0 || pos == static_cast<int>(text.size())) return true;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t next = pulp::canvas::cluster_step(text, cursor, true);
        if (next <= cursor || next > text.size()) break;
        cursor = next;
        if (cursor == static_cast<std::size_t>(pos)) return true;
    }
    return false;
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

TEST_CASE("TextEditor public selection APIs clamp to grapheme boundaries",
          "[view][text_editor][selection][api][unicode]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("A" "e\xCC\x81" "B");

    editor.set_selection(2, 3);  // byte offsets inside the combining sequence
    REQUIRE(editor.selection_anchor() == 1);
    REQUIRE(editor.selection_active() == 4);
    REQUIRE(editor.selection_range() == std::pair<int, int>{1, 4});
    REQUIRE(editor.selected_text() == "e\xCC\x81");

    editor.set_caret_pos(3);
    REQUIRE(editor.caret_pos() == 4);
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor UTF offset helpers cover scalar widths and malformed input",
          "[view][text_editor][unicode][utf16]") {
    const std::string mixed = "A"
        "\xC2\xA2"      // U+00A2, one UTF-16 unit, two UTF-8 bytes.
        "\xE2\x82\xAC"  // U+20AC, one UTF-16 unit, three UTF-8 bytes.
        "\xF0\x9F\x98\x80"  // U+1F600, two UTF-16 units, four UTF-8 bytes.
        "Z";

    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 0) == 0u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 1) == 1u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 2) == 3u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 3) == 6u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 4) == 6u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 5) == 10u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(mixed, 6) == mixed.size());

    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, 0) == 0u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, 1) == 1u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, 3) == 2u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, 6) == 3u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, 10) == 5u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(mixed, mixed.size()) == 6u);

    std::string malformed;
    malformed.push_back(static_cast<char>(0x80));
    malformed += "ab";
    REQUIRE(pulp::canvas::safe_utf8_prefix_size(malformed, malformed.size()) == 0u);
    REQUIRE(pulp::canvas::utf8_offset_for_utf16_offset(malformed, 1) == 1u);
    REQUIRE(pulp::canvas::utf16_offset_for_utf8_offset(malformed, 1) == 0u);
}

TEST_CASE("TextEditor undo/redo", "[view][text_editor]") {
    TextEditor editor;
    editor.on_focus_changed(true);

    TextInputEvent input;
    input.text = "First";
    editor.on_text_input(input);
    editor.select_all();
    input.text = "Second";
    editor.on_text_input(input);
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

TEST_CASE("TextEditor programmatic set_text clears undo history",
          "[view][text_editor][undo][api]") {
    TextEditor editor;
    editor.on_focus_changed(true);

    TextInputEvent input;
    input.text = "draft";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "draft");
    REQUIRE(editor.undo());
    REQUIRE(editor.text().empty());

    input.text = "typed";
    editor.on_text_input(input);
    editor.set_text("synced");

    REQUIRE(editor.text() == "synced");
    REQUIRE_FALSE(editor.undo());
    REQUIRE_FALSE(editor.redo());
}

TEST_CASE("TextEditor same-value set_text preserves local editing state",
          "[view][text_editor][undo][api]") {
    TextEditor editor;
    editor.on_focus_changed(true);

    TextInputEvent input;
    input.text = "abc";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "abc");

    int changes = 0;
    editor.on_change = [&](const std::string&) { ++changes; };
    editor.set_text("abc");

    REQUIRE(changes == 0);
    REQUIRE(editor.undo());
    REQUIRE(editor.text().empty());
}

TEST_CASE("TextEditor public caret and selection APIs break typing undo coalescing",
          "[view][text_editor][undo][api]") {
    TextEditor caret_editor;
    caret_editor.on_focus_changed(true);

    TextInputEvent input;
    input.text = "ab";
    caret_editor.on_text_input(input);
    REQUIRE(caret_editor.text() == "ab");

    caret_editor.set_caret_pos(0);
    input.text = "X";
    caret_editor.on_text_input(input);
    REQUIRE(caret_editor.text() == "Xab");
    REQUIRE(caret_editor.undo());
    REQUIRE(caret_editor.text() == "ab");

    TextEditor selection_editor;
    selection_editor.on_focus_changed(true);
    input.text = "alpha";
    selection_editor.on_text_input(input);
    selection_editor.set_selection(0, 2);
    input.text = "om";
    selection_editor.on_text_input(input);
    REQUIRE(selection_editor.text() == "ompha");
    REQUIRE(selection_editor.undo());
    REQUIRE(selection_editor.text() == "alpha");
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
          "[view][text_editor]") {
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
          "[view][text_editor]") {
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

TEST_CASE("TextEditor command editing shortcuts copy cut paste undo and redo",
          "[view][text_editor][keyboard][clipboard][undo]") {
    require_system_clipboard_text("");

    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("copy cut");
    editor.set_selection(0, 4);

    REQUIRE(editor.on_key_event(key_event(KeyCode::c, main_modifier())));
    REQUIRE(editor.text() == "copy cut");
    REQUIRE(editor.has_selection());

    editor.set_selection(5, 8);
    REQUIRE(editor.on_key_event(key_event(KeyCode::x, main_modifier())));
    REQUIRE(editor.text() == "copy ");
    REQUIRE_FALSE(editor.has_selection());

    REQUIRE(editor.on_key_event(key_event(KeyCode::v, main_modifier())));
    REQUIRE(editor.text() == "copy cut");

    REQUIRE(editor.on_key_event(key_event(KeyCode::z, main_modifier())));
    REQUIRE(editor.text() == "copy ");

    REQUIRE(editor.on_key_event(key_event(KeyCode::z, main_modifier() | kModShift)));
    REQUIRE(editor.text() == "copy cut");
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

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier())));
    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()) - 2);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier())));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, word_modifier())));
    REQUIRE(editor.caret_pos() == 17);

#ifdef __APPLE__
    REQUIRE(editor.on_key_event(key_event(KeyCode::left, main_modifier())));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, main_modifier())));
    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()));
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::end_)));
    REQUIRE(editor.caret_pos() == static_cast<int>(text.size()));
#endif
}

TEST_CASE("TextEditor shift navigation extends and clears selection",
          "[view][text_editor][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("abcd");

    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModShift)));
    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModShift)));
    REQUIRE(editor.caret_pos() == 2);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "ab");

    // A plain Left arrow with a selection collapses to the selection's LEFT
    // edge (0) without advancing — matching macOS/Windows. (This previously
    // asserted 1, which was the collapse-then-move-back bug users hit as
    // "arrow skips a character past the selection".)
    REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
    REQUIRE(editor.caret_pos() == 0);
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
    REQUIRE(editor.caret_pos() == 3);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 11);
}

TEST_CASE("TextEditor invalidates cached visual layout after programmatic text changes",
          "[view][text_editor][layout][keyboard]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 240, 120});
    editor.set_text("old first row\nold second row\nold third row");

    pulp::canvas::RecordingCanvas canvas;
    editor.paint(canvas);

    editor.set_text("a\nbcdef");
    editor.set_caret_pos(static_cast<int>(editor.text().size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::up)));
    REQUIRE(editor.caret_pos() == 1);
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
    REQUIRE(editor.caret_pos() == 4);
    REQUIRE(editor.selected_text() == "\nef\nghij");

    REQUIRE(editor.on_key_event(key_event(KeyCode::down, kModShift)));
    REQUIRE(editor.caret_pos() == 7);
    REQUIRE(editor.selected_text() == "\nghij");

    REQUIRE(editor.on_key_event(key_event(KeyCode::down, kModShift)));
    REQUIRE(editor.caret_pos() == 12);
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("TextEditor macOS command arrows move to line and document bounds",
          "[view][text_editor][keyboard][selection]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("one\ntwo three\nfour");

#ifdef __APPLE__
    REQUIRE(editor.on_key_event(key_event(KeyCode::left, kModCmd)));
    REQUIRE(editor.caret_pos() == 14);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, kModCmd | kModShift)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
    REQUIRE(editor.selected_text() == "four");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, kModCmd | kModShift)));
    REQUIRE(editor.caret_pos() == 14);
    REQUIRE_FALSE(editor.has_selection());

    REQUIRE(editor.on_key_event(key_event(KeyCode::up, kModCmd | kModShift)));
    REQUIRE(editor.caret_pos() == 0);
    REQUIRE(editor.selected_text() == "one\ntwo three\n");

    REQUIRE(editor.on_key_event(key_event(KeyCode::down, kModCmd)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::up, kModCmd)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::down, kModCmd | kModShift)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
    REQUIRE(editor.selected_text() == editor.text());
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::home)));
    REQUIRE(editor.caret_pos() == 14);

    REQUIRE(editor.on_key_event(key_event(KeyCode::home, kModCtrl | kModShift)));
    REQUIRE(editor.caret_pos() == 0);
    REQUIRE(editor.selected_text() == "one\ntwo three\n");

    REQUIRE(editor.on_key_event(key_event(KeyCode::end_, kModCtrl)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
#endif
}

TEST_CASE("TextEditor word navigation keeps attached punctuation with the word",
          "[view][text_editor][keyboard][selection]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("lo-fi jack, tail");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier())));
    REQUIRE(editor.caret_pos() == 12);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier())));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier())));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, word_modifier())));
    REQUIRE(editor.caret_pos() == 6);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, word_modifier())));
    REQUIRE(editor.caret_pos() == 12);

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, word_modifier() | kModShift)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
    REQUIRE(editor.selected_text() == "tail");
}

TEST_CASE("TextEditor keyboard navigation clamps at document boundaries",
          "[view][text_editor][keyboard][selection]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("alpha\nbeta");

#ifdef __APPLE__
    REQUIRE(editor.on_key_event(key_event(KeyCode::up, main_modifier())));
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::home, main_modifier())));
#endif
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
    REQUIRE(editor.caret_pos() == 0);

    REQUIRE(editor.on_key_event(key_event(KeyCode::left, word_modifier() | kModShift)));
    REQUIRE(editor.caret_pos() == 0);
    REQUIRE_FALSE(editor.has_selection());

#ifdef __APPLE__
    REQUIRE(editor.on_key_event(key_event(KeyCode::down, main_modifier())));
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::end_, main_modifier())));
#endif
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::right)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::right, word_modifier() | kModShift)));
    REQUIRE(editor.caret_pos() == static_cast<int>(editor.text().size()));
    REQUIRE_FALSE(editor.has_selection());
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

TEST_CASE("TextEditor deletion and arrows respect grapheme clusters",
          "[view][text_editor][unicode][keyboard]") {
    const std::string family =
        "\xF0\x9F\x91\xA8"      // man
        "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9"      // woman
        "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7"      // girl
        "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA6";     // boy
    const std::string acute_e = "e\xCC\x81";
    const std::string flag_us = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";

    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("A" + family + acute_e + flag_us + "B");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
    REQUIRE(editor.text().substr(static_cast<std::size_t>(editor.caret_pos())) == "B");

    REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
    REQUIRE(editor.text().substr(static_cast<std::size_t>(editor.caret_pos())) == flag_us + "B");

    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace)));
    REQUIRE(editor.text() == "A" + family + flag_us + "B");

    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace)));
    REQUIRE(editor.text() == "A" + flag_us + "B");
}

TEST_CASE("TextEditor navigation and deletion preserve cluster-boundary invariants",
          "[view][text_editor][unicode][property]") {
    const std::string family =
        "\xF0\x9F\x91\xA8" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA6";
    const std::string thumbs_up_tone = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";
    const std::string flag_us = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    const std::string devanagari_ksha = "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";
    std::string malformed;
    malformed.push_back(static_cast<char>(0xF0));
    malformed.push_back(static_cast<char>(0x28));
    malformed.push_back(static_cast<char>(0x8C));
    malformed.push_back(static_cast<char>(0x28));

    const std::vector<std::string> samples = {
        "e\xCC\x81",
        family,
        thumbs_up_tone,
        flag_us,
        devanagari_ksha,
        malformed,
    };

    for (const auto& sample : samples) {
        TextEditor editor;
        editor.on_focus_changed(true);
        editor.set_text("A" + sample + "B");

        int guard = 0;
        while (editor.caret_pos() > 0 && guard++ < 32) {
            REQUIRE(is_cluster_boundary(editor.text(), editor.caret_pos()));
            REQUIRE(editor.on_key_event(key_event(KeyCode::left)));
        }
        REQUIRE(editor.caret_pos() == 0);
        REQUIRE(is_cluster_boundary(editor.text(), editor.caret_pos()));

        editor.set_caret_pos(static_cast<int>(editor.text().size()));
        guard = 0;
        while (!editor.text().empty() && guard++ < 32) {
            REQUIRE(is_cluster_boundary(editor.text(), editor.caret_pos()));
            const auto before_size = editor.text().size();
            REQUIRE(editor.on_key_event(key_event(KeyCode::backspace)));
            REQUIRE(editor.text().size() < before_size);
            REQUIRE(is_cluster_boundary(editor.text(), editor.caret_pos()));
        }
        REQUIRE(editor.text().empty());
    }
}

TEST_CASE("TextEditor word and line delete shortcuts follow platform conventions",
          "[view][text_editor][keyboard][delete]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("lo-fi jack, tail");

    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace, word_modifier())));
    REQUIRE(editor.text() == "lo-fi jack, ");

    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace, word_modifier())));
    REQUIRE(editor.text() == "lo-fi ");

    editor.set_text("one two three");
    editor.set_caret_pos(4);
    REQUIRE(editor.on_key_event(key_event(KeyCode::delete_, word_modifier())));
    REQUIRE(editor.text() == "one three");

    editor.multi_line = true;
    editor.set_text("one\ntwo three");

#ifdef __APPLE__
    REQUIRE_FALSE(editor.on_key_event(key_event(KeyCode::u, kModCmd)));
    REQUIRE(editor.text() == "one\ntwo three");

    REQUIRE(editor.on_key_event(key_event(KeyCode::u, kModCtrl)));
    REQUIRE(editor.text() == "one\n");

    editor.set_text("one\ntwo three");
    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace, kModCmd)));
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::u, kModCtrl)));
#endif
    REQUIRE(editor.text() == "one\n");

    editor.set_text("alpha beta\ngamma");
    editor.set_caret_pos(2);
    REQUIRE(editor.on_key_event(key_event(KeyCode::k, kModCtrl)));
    REQUIRE(editor.text() == "al\ngamma");
    REQUIRE(editor.caret_pos() == 2);
}

TEST_CASE("TextEditor Page Up and Page Down move multiline caret by visible pages",
          "[view][text_editor][keyboard][page]") {
    TextEditor single_line;
    single_line.on_focus_changed(true);
    single_line.set_text("single line");
    single_line.set_caret_pos(6);

    REQUIRE(single_line.on_key_event(key_event(KeyCode::page_up)));
    REQUIRE(single_line.caret_pos() == 0);

    single_line.set_caret_pos(2);
    REQUIRE(single_line.on_key_event(key_event(KeyCode::page_down, kModShift)));
    REQUIRE(single_line.selection_range() == std::pair<int, int>{2, 11});

    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 200, 60});
    editor.set_text("one\ntwo\nthree\nfour\nfive\nsix");

    REQUIRE(editor.on_key_event(key_event(KeyCode::page_up)));
    REQUIRE(editor.caret_pos() < static_cast<int>(editor.text().size()));

    REQUIRE(editor.on_key_event(key_event(KeyCode::page_down, kModShift)));
    REQUIRE(editor.has_selection());
}

TEST_CASE("TextEditor paragraph shortcuts move and extend to line boundaries",
          "[view][text_editor][keyboard][paragraph]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("alpha beta\ngamma delta");
    editor.set_caret_pos(8);

    REQUIRE(editor.on_key_event(key_event(KeyCode::up, paragraph_modifier())));
    REQUIRE(editor.caret_pos() == 0);
    REQUIRE_FALSE(editor.has_selection());

    editor.set_caret_pos(8);
    REQUIRE(editor.on_key_event(key_event(KeyCode::down, paragraph_modifier() | kModShift)));
    REQUIRE(editor.selection_range() == std::pair<int, int>{8, 10});
    REQUIRE(editor.caret_pos() == 10);
}

TEST_CASE("TextEditor marked text replacement tracks the active range",
          "[view][text_editor][ime][issue-493]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text(" draft", 6, 0);
    REQUIRE(editor.text() == "base draft");
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{4, 6});
    REQUIRE(editor.caret_pos() == 10);

    editor.set_marked_text(" final", 6, 0);
    REQUIRE(editor.text() == "base final");
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{4, 6});
    REQUIRE(editor.caret_pos() == 10);

    editor.unmark_text();
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{0, 0});
    REQUIRE(editor.text() == "base final");
}

TEST_CASE("TextEditor marked text selected range drives caret and selection",
          "[view][text_editor][ime][selection]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text("draft", 2, 0);
    REQUIRE(editor.text() == "basedraft");
    REQUIRE(editor.has_marked_text());
    REQUIRE_FALSE(editor.has_selection());
    REQUIRE(editor.caret_pos() == 6);

    editor.set_marked_text("draft", 1, 3);
    REQUIRE(editor.text() == "basedraft");
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selection_range() == std::pair<int, int>{5, 8});
    REQUIRE(editor.caret_pos() == 8);
}

TEST_CASE("TextEditor marked text selected range uses UTF-8 byte offsets",
          "[view][text_editor][ime][unicode][selection]") {
    const std::string ni = "\xE3\x81\xAB";
    const std::string ho = "\xE3\x81\xBB";
    const std::string grin = "\xF0\x9F\x98\x80";

    TextEditor editor;
    editor.on_focus_changed(true);

    editor.set_marked_text(ni, 3, 0);
    REQUIRE(editor.text() == ni);
    REQUIRE(editor.caret_pos() == 3);
    REQUIRE(editor.selection_range() == std::pair<int, int>{3, 3});

    editor.set_marked_text(ni + ho, 3, 3);
    REQUIRE(editor.text() == ni + ho);
    REQUIRE(editor.selection_range() == std::pair<int, int>{3, 6});
    REQUIRE(editor.caret_pos() == 6);

    editor.unmark_text();
    editor.select_all();
    editor.set_marked_text(grin + "a", 4, 1);
    REQUIRE(editor.text() == grin + "a");
    REQUIRE(editor.selection_range() == std::pair<int, int>{4, 5});
    REQUIRE(editor.caret_pos() == 5);
}

TEST_CASE("TextEditor marked text selected range can convert native UTF-16 offsets",
          "[view][text_editor][ime][unicode][selection]") {
    const std::string ni = "\xE3\x81\xAB";
    const std::string ho = "\xE3\x81\xBB";
    const std::string grin = "\xF0\x9F\x98\x80";

    TextEditor editor;
    editor.on_focus_changed(true);

    editor.set_marked_text_utf16(ni, 1, 0);
    REQUIRE(editor.text() == ni);
    REQUIRE(editor.caret_pos() == 3);
    REQUIRE(editor.selection_range() == std::pair<int, int>{3, 3});

    editor.set_marked_text_utf16(ni + ho, 1, 1);
    REQUIRE(editor.text() == ni + ho);
    REQUIRE(editor.selection_range() == std::pair<int, int>{3, 6});
    REQUIRE(editor.caret_pos() == 6);

    editor.unmark_text();
    editor.select_all();
    editor.set_marked_text_utf16(grin + "a", 2, 1);
    REQUIRE(editor.text() == grin + "a");
    REQUIRE(editor.selection_range() == std::pair<int, int>{4, 5});
    REQUIRE(editor.caret_pos() == 5);
}

TEST_CASE("TextEditor empty marked text clears the previous composition",
          "[view][text_editor][ime]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text(" draft", 6, 0);
    REQUIRE(editor.text() == "base draft");
    REQUIRE(editor.has_marked_text());

    editor.set_marked_text("", 0, 0);
    REQUIRE(editor.text() == "base");
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{0, 0});
    REQUIRE(editor.caret_pos() == 4);
    REQUIRE_FALSE(editor.undo());

    TextInputEvent typed;
    typed.text = "!";
    editor.on_text_input(typed);
    REQUIRE(editor.text() == "base!");

    editor.set_marked_text(" draft", 6, 0);
    editor.set_marked_text("", 0, 0);
    REQUIRE(editor.text() == "base!");
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "base");
}

TEST_CASE("TextEditor empty marked text without composition is a no-op",
          "[view][text_editor][ime][selection]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("alpha beta");
    editor.set_selection(0, 5);

    int changes = 0;
    editor.on_change = [&](const std::string&) { ++changes; };
    editor.set_marked_text("", 0, 0);

    REQUIRE(editor.text() == "alpha beta");
    REQUIRE(editor.selection_range() == std::pair<int, int>{0, 5});
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(changes == 0);
    REQUIRE_FALSE(editor.undo());
}

TEST_CASE("TextEditor marked text uses input policy and undo pipeline",
          "[view][text_editor][ime][policy]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.max_length = 4;
    editor.input_filter = [](std::string_view text) {
        std::string filtered;
        filtered.reserve(text.size());
        for (char c : text)
            filtered += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return filtered;
    };
    editor.validator = [](std::string_view candidate) {
        return candidate.find('X') == std::string_view::npos;
    };
    editor.set_text("ab");

    editor.set_marked_text("cd\r\nef", 0, 0);
    REQUIRE(editor.text() == "abCD");
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.marked_range() == std::pair<int, int>{2, 2});

    editor.unmark_text();
    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "ab");
    REQUIRE_FALSE(editor.has_marked_text());

    editor.set_marked_text("x", 0, 0);
    REQUIRE(editor.text() == "ab");
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE_FALSE(editor.undo());
}

TEST_CASE("TextEditor text input commits active IME marked text by replacement",
          "[view][text_editor][ime][commit]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("base");

    editor.set_marked_text(" draft", 6, 0);
    REQUIRE(editor.text() == "base draft");
    REQUIRE(editor.has_marked_text());

    TextInputEvent commit;
    commit.text = " final";
    editor.on_text_input(commit);
    REQUIRE(editor.text() == "base final");
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.caret_pos() == 10);

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "base");
}

TEST_CASE("TextEditor select-on-focus selects all on focus", "[view][text_editor]") {
    TextEditor editor;
    editor.set_text("Focus me");
    editor.select_on_focus = true;

    editor.on_focus_changed(true);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "Focus me");
}

// A plain arrow with an active selection collapses to the selection edge in the
// direction of travel — WITHOUT advancing an extra cluster. Regression for the
// "→ after selecting a word skips into the next word" bug (adds a space / lands
// before the next word instead of at the selection end).
TEST_CASE("TextEditor arrow collapses selection to its edge without moving",
          "[view][text_editor][selection]") {
    auto key = [](KeyCode code, uint16_t mods = 0) {
        KeyEvent ev;
        ev.key = code;
        ev.modifiers = mods;
        ev.is_down = true;
        return ev;
    };

    SECTION("right arrow → caret at selection end (after 'analog'), no extra move") {
        TextEditor editor;
        editor.set_text("warm analog pads");
        editor.set_selection(5, 11);  // "analog"
        REQUIRE(editor.has_selection());
        REQUIRE(editor.on_key_event(key(KeyCode::right)));
        REQUIRE_FALSE(editor.has_selection());
        REQUIRE(editor.caret_pos() == 11);  // right after the 'g', not into "pads"
    }

    SECTION("left arrow → caret at selection start") {
        TextEditor editor;
        editor.set_text("warm analog pads");
        editor.set_selection(5, 11);
        REQUIRE(editor.on_key_event(key(KeyCode::left)));
        REQUIRE_FALSE(editor.has_selection());
        REQUIRE(editor.caret_pos() == 5);  // start of "analog"
    }

    SECTION("collapse is independent of selection direction (anchor > active)") {
        TextEditor editor;
        editor.set_text("warm analog pads");
        editor.set_selection(11, 5);  // selected right-to-left
        REQUIRE(editor.on_key_event(key(KeyCode::right)));
        REQUIRE(editor.caret_pos() == 11);
    }

    SECTION("no selection: arrow still advances one cluster") {
        TextEditor editor;
        editor.set_text("warm analog pads");
        editor.set_caret_pos(5);
        REQUIRE(editor.on_key_event(key(KeyCode::right)));
        REQUIRE(editor.caret_pos() == 6);
    }
}
