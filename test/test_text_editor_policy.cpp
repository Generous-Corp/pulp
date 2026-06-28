#include <catch2/catch_test_macros.hpp>
#include "support/text_editor_test_utils.hpp"

#include <cctype>
#include <memory>
#include <string_view>

using namespace pulp::view;
using namespace pulp::test;

TEST_CASE("TextEditor Tab and multiline Return policies are configurable",
          "[view][text_editor][keyboard][policy]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("a");

    REQUIRE_FALSE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor.text() == "a");

    editor.tab_behavior = TextEditor::TabBehavior::insert_tab;
    REQUIRE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor.text() == "a\t");

    std::string tab_committed;
    editor.tab_behavior = TextEditor::TabBehavior::commit;
    editor.on_tab_commit = [&](const std::string& text) { tab_committed = text; };
    REQUIRE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(tab_committed == "a\t");
    REQUIRE(editor.text() == "a\t");

    TextEditor return_fallback;
    return_fallback.on_focus_changed(true);
    return_fallback.set_text("fallback");
    return_fallback.tab_behavior = TextEditor::TabBehavior::commit;
    std::string fallback_returned;
    return_fallback.on_return = [&](const std::string& text) { fallback_returned = text; };
    REQUIRE(return_fallback.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(fallback_returned == "fallback");

    editor.tab_behavior = TextEditor::TabBehavior::ignore;
    REQUIRE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor.text() == "a\t");

    editor.multi_line = true;
    editor.multi_line_return_behavior = TextEditor::MultiLineReturnBehavior::shift_inserts_newline;
    std::string returned;
    editor.on_return = [&](const std::string& text) { returned = text; };

    REQUIRE(editor.on_key_event(key_event(KeyCode::enter)));
    REQUIRE(returned == "a\t");
    REQUIRE(editor.text() == "a\t");

    REQUIRE(editor.on_key_event(key_event(KeyCode::enter, kModShift)));
    REQUIRE(editor.text() == "a\t\n");
}

TEST_CASE("TextEditor Tab commit callback may destroy the editor",
          "[view][text_editor][keyboard][policy][lifetime]") {
    auto editor = std::make_unique<TextEditor>();
    auto* raw = editor.get();
    raw->on_focus_changed(true);
    raw->set_text("commit me");
    raw->tab_behavior = TextEditor::TabBehavior::commit;

    std::string committed;
    raw->on_tab_commit = [&](const std::string& text) {
        committed = text;
        editor.reset();
    };

    REQUIRE(raw->on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor == nullptr);
    REQUIRE(committed == "commit me");
}

TEST_CASE("TextEditor insert-tab policy consumes rejected tab edits",
          "[view][text_editor][keyboard][policy][validation]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("abcd");
    editor.set_caret_pos(static_cast<int>(editor.text().size()));
    editor.max_length = 4;
    editor.tab_behavior = TextEditor::TabBehavior::insert_tab;

    REQUIRE(editor.on_key_event(key_event(KeyCode::tab)));
    REQUIRE(editor.text() == "abcd");
}

TEST_CASE("TextEditor input filter max length and validator gate edits",
          "[view][text_editor][validation]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.max_length = 4;
    editor.input_filter = [](std::string_view text) {
        std::string out;
        for (char c : text) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    };
    editor.validator = [](std::string_view text) {
        return text.find('X') == std::string_view::npos;
    };

    TextInputEvent input;
    input.text = "ab";
    editor.on_text_input(input);
    input.text = "cd";
    editor.on_text_input(input);
    input.text = "ef";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "ABCD");

    editor.select_all();
    input.text = "xx";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "ABCD");

    editor.validator = [](std::string_view text) {
        return !text.empty();
    };
    editor.select_all();
    REQUIRE(editor.on_key_event(key_event(KeyCode::delete_)));
    REQUIRE(editor.text() == "ABCD");
}

TEST_CASE("TextEditor paste sanitizer is separate from typed input filter",
          "[view][text_editor][clipboard][validation]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.paste_sanitizer = [](std::string_view text) {
        std::string out;
        for (char c : text) {
            if (c != '-') out += c;
        }
        return out;
    };
    editor.input_filter = [](std::string_view text) {
        std::string out;
        for (char c : text) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    };

    TextInputEvent input;
    input.text = "a-b";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "A-B");

    require_system_clipboard_text("c-d");
    REQUIRE(editor.paste_from_clipboard());
    REQUIRE(editor.text() == "A-BCD");
}

TEST_CASE("TextEditor line ending policy applies to input paste and IME",
          "[view][text_editor][policy][line-ending][ime]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.multi_line = true;
    editor.line_ending_policy = TextEditor::LineEndingPolicy::strip;

    TextInputEvent input;
    input.text = "a\r\nb\nc\rd";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "abcd");

    if (seed_system_clipboard_text("x\r\ny")) {
        editor.select_all();
        REQUIRE(editor.paste_from_clipboard());
        REQUIRE(editor.text() == "xy");
    } else {
        REQUIRE(editor.text() == "abcd");
    }

    TextEditor strip_ime;
    strip_ime.on_focus_changed(true);
    strip_ime.multi_line = true;
    strip_ime.line_ending_policy = TextEditor::LineEndingPolicy::strip;
    strip_ime.set_marked_text("p\r\nq", 0, 0);
    REQUIRE(strip_ime.text() == "pq");

    editor.select_all();
    editor.line_ending_policy = TextEditor::LineEndingPolicy::normalize;
    editor.set_marked_text("p\r\nq", 0, 0);
    REQUIRE(editor.text() == "p\nq");

    TextEditor preserve;
    preserve.on_focus_changed(true);
    preserve.multi_line = true;
    preserve.line_ending_policy = TextEditor::LineEndingPolicy::preserve;
    input.text = "a\r\nb";
    preserve.on_text_input(input);
    REQUIRE(preserve.text() == "a\r\nb");

    TextEditor single_line;
    single_line.on_focus_changed(true);
    single_line.line_ending_policy = TextEditor::LineEndingPolicy::preserve;
    input.text = "a\nb";
    single_line.on_text_input(input);
    REQUIRE(single_line.text() == "a b");
}

TEST_CASE("TextEditor IME marked text supports UTF-16 selection and preserve policy",
          "[view][text_editor][policy][ime][utf16]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.multi_line = true;
    editor.line_ending_policy = TextEditor::LineEndingPolicy::preserve;
    editor.set_text("ab");
    editor.set_caret_pos(1);

    editor.set_marked_text_utf16("\xF0\x9F\x98\x80\r\nz", 2, 2);
    REQUIRE(editor.has_marked_text());
    REQUIRE(editor.text() == "a\xF0\x9F\x98\x80\r\nzb");
    REQUIRE(editor.marked_range() == std::pair<int, int>{1, 7});
    REQUIRE(editor.selection_range() == std::pair<int, int>{5, 7});

    TextInputEvent input;
    input.text = "\xE3\x81\x82";
    editor.on_text_input(input);
    REQUIRE_FALSE(editor.has_marked_text());
    REQUIRE(editor.text() == "a\xE3\x81\x82" "b");
    REQUIRE(editor.caret_pos() == 4);

    REQUIRE(editor.undo());
    REQUIRE(editor.text() == "ab");
    REQUIRE_FALSE(editor.has_marked_text());
}

TEST_CASE("TextEditor paste-and-match-style uses the paste path",
          "[view][text_editor][clipboard][keyboard]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("a");
    require_system_clipboard_text("b");

#ifdef __APPLE__
    REQUIRE(editor.on_key_event(key_event(KeyCode::v, kModCmd | kModShift | kModAlt)));
#else
    REQUIRE(editor.on_key_event(key_event(KeyCode::v, kModCtrl | kModShift)));
#endif
    REQUIRE(editor.text() == "ab");
}

TEST_CASE("TextEditor read-only allows navigation selection and copy but blocks mutation",
          "[view][text_editor][readonly]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("read only");
    editor.read_only = true;

    REQUIRE_FALSE(editor.accepts_text_input());
    REQUIRE(editor.on_key_event(key_event(KeyCode::left, kModShift)));
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selected_text() == "y");
    REQUIRE(editor.copy_to_clipboard());

    TextInputEvent input;
    input.text = "!";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "read only");

    REQUIRE(editor.on_key_event(key_event(KeyCode::backspace)));
    REQUIRE(editor.text() == "read only");

    pulp::platform::Clipboard::set_text("paste");
    REQUIRE_FALSE(editor.paste_from_clipboard());
    REQUIRE_FALSE(editor.cut_to_clipboard());
    REQUIRE(editor.text() == "read only");
}

TEST_CASE("TextEditor read-only blocks undo redo mutations",
          "[view][text_editor][readonly][undo]") {
    TextEditor editor;
    editor.on_focus_changed(true);

    TextInputEvent input;
    input.text = "draft";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "draft");
    editor.read_only = true;

    REQUIRE_FALSE(editor.undo());
    REQUIRE(editor.text() == "draft");

    auto undo_key = key_event(KeyCode::z, main_modifier());
    REQUIRE(editor.on_key_event(undo_key));
    REQUIRE(editor.text() == "draft");
}

TEST_CASE("TextEditor disabled rejects keyboard mouse clipboard and text input",
          "[view][text_editor][disabled]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_text("disabled");
    editor.set_enabled(false);

    REQUIRE_FALSE(editor.accepts_text_input());
    REQUIRE_FALSE(editor.on_key_event(key_event(KeyCode::left)));

    TextInputEvent input;
    input.text = "!";
    editor.on_text_input(input);
    REQUIRE(editor.text() == "disabled");

    editor.select_all();
    REQUIRE_FALSE(editor.copy_to_clipboard());
    REQUIRE_FALSE(editor.cut_to_clipboard());
    pulp::platform::Clipboard::set_text("paste");
    REQUIRE_FALSE(editor.paste_from_clipboard());

    MouseEvent down;
    down.position = {10.0f, 10.0f};
    down.is_down = true;
    down.phase = MousePhase::press;
    editor.on_mouse_event(down);
    REQUIRE_FALSE(editor.has_pointer_capture(0));
}

TEST_CASE("TextEditor password mode blocks copy and cut by default",
          "[view][text_editor][clipboard][password]") {
    TextEditor editor;
    editor.password_mode = true;
    editor.on_focus_changed(true);
    editor.set_text("secret");
    editor.select_all();

    REQUIRE_FALSE(editor.copy_to_clipboard());
    REQUIRE_FALSE(editor.cut_to_clipboard());
    REQUIRE(editor.selected_text().empty());
    REQUIRE(editor.text() == "secret");
    REQUIRE(editor.has_selection());
}

TEST_CASE("TextEditor clipboard policy controls password export and paste",
          "[view][text_editor][clipboard][password][policy]") {
    TextEditor editor;
    editor.password_mode = true;
    editor.on_focus_changed(true);
    editor.set_text("secret");
    editor.select_all();

    editor.clipboard_policy = TextEditor::ClipboardPolicy::allow_password_contents;
    REQUIRE(editor.selected_text() == "secret");
    REQUIRE(editor.copy_to_clipboard());

    editor.clipboard_policy = TextEditor::ClipboardPolicy::disabled;
    REQUIRE_FALSE(editor.copy_to_clipboard());
    REQUIRE_FALSE(editor.cut_to_clipboard());
    pulp::platform::Clipboard::set_text(" replacement");
    editor.clear_selection();
    REQUIRE_FALSE(editor.paste_from_clipboard());
    REQUIRE(editor.text() == "secret");
}
