#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

#include <memory>
#include <stdexcept>

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

// A canvas whose shaped text_x_for_byte() deliberately diverges from the sum of
// isolated per-glyph measure_text() widths — the exact gap that kerning/spacing
// produces with a real shaper. A caret built from shaped offsets lands near 500+;
// one built by summing glyph widths (the pre-fix path) lands near 16. Used to
// assert TextEditor::paint feeds caret_rect() from text_x_for_byte, not glyph sums.
struct ShapedOffsetCanvas : RecordingCanvas {
    float measure_text(const std::string& text) override {
        return 8.0f * static_cast<float>(text.size());
    }
    float text_x_for_byte(const std::string& text, std::size_t byte_index) override {
        return 500.0f + static_cast<float>(std::min(byte_index, text.size()));
    }
};

struct Utf8StrictCanvas : RecordingCanvas {
    float measure_text(const std::string& text) override {
        if (safe_utf8_prefix_size(text, text.size()) != text.size())
            throw std::runtime_error("invalid utf8 text measured");
        return 8.0f * static_cast<float>(text.size());
    }
};

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

TEST_CASE("TextEditor caret X comes from shaped offsets, not summed glyph widths",
          "[view][text_editor][paint]") {
    // Regression guard for the caret/selection-X fix: paint() must populate the layout's
    // x_offsets from canvas.text_x_for_byte() (which a real shaper kerns), not from summing
    // isolated measure_text() advances. ShapedOffsetCanvas makes the two paths diverge by
    // ~500 px so the caret position reveals which one fed it.
    TextEditor editor;
    editor.on_focus_changed(true);              // so set_text leaves the caret at the end
    editor.set_bounds({0, 0, 800, 24});
    editor.set_text("AV");                      // caret now at byte 2

    ShapedOffsetCanvas canvas;
    editor.paint(canvas);

    const auto rect = editor.caret_rect();
    // Shaped offset for byte 2 is 502; the pre-fix glyph-sum path would put it near 16.
    REQUIRE(rect.x > 400.0f);
}

TEST_CASE("TextEditor paint never measures split UTF-8 byte prefixes",
          "[view][text_editor][paint][utf8]") {
    TextEditor editor;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 800, 30});
    editor.set_text("cafe\xCC\x81 synth"); // "cafe" + combining acute

    Utf8StrictCanvas canvas;
    REQUIRE_NOTHROW(editor.paint(canvas));
    REQUIRE_NOTHROW(canvas.text_x_for_byte(editor.text(), 5));
}

TEST_CASE("TextEditor multi-line offsets never measure split UTF-8 bytes",
          "[view][text_editor][paint][utf8]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_bounds({0, 0, 800, 80});
    editor.set_text("pad\ncafe\xCC\x81 arp");

    Utf8StrictCanvas canvas;
    REQUIRE_NOTHROW(editor.paint(canvas));
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

TEST_CASE("TextEditor mouse drag selects text",
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
    drag.is_down = true;
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

TEST_CASE("TextEditor legacy mouse drag selects text",
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

    // macOS WindowHost and PluginViewHost dispatch a rich press event, then
    // the legacy widget callback. Drag ticks arrive through the legacy path.
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

TEST_CASE("TextEditor caret-blink subscription is removed even after detach", "[view][text_editor]") {
    // Regression: the caret-blink frame-clock subscription must be torn down on
    // destruction even when the editor was removed from the view tree first.
    // frame_clock() walks parent_, so a detached editor can't find its clock; the
    // editor caches the clock pointer at subscribe time so it can always
    // unsubscribe. If it doesn't, the clock holds a callback capturing a destroyed
    // `this` and the next tick() is a use-after-free.
    FrameClock clock;

    auto parent = std::make_unique<View>();
    parent->set_frame_clock(&clock);

    auto editor_owned = std::make_unique<TextEditor>();
    TextEditor* editor = editor_owned.get();
    parent->add_child(std::move(editor_owned));

    editor->on_focus_changed(true);                 // subscribes to the clock
    REQUIRE(clock.has_active_subscribers());

    // Detach from the tree BEFORE destruction — now frame_clock() would return null.
    auto detached = parent->remove_child(editor);
    REQUIRE(detached != nullptr);
    detached.reset();                               // destroy the editor

    REQUIRE_FALSE(clock.has_active_subscribers());  // subscription cleaned up
    clock.tick(0.016f);                             // must not touch freed memory
    SUCCEED("tick after destruction did not use freed memory");
}

// Regression: a selection must recolor glyphs in place — never move them.
//
// The painter used to draw a selection as three independently-shaped runs
// (before / selected / after). Re-shaping a substring in isolation loses the
// kerning/left-side-bearing context it had inside the full string, so the
// selected glyphs landed at the wrong x — the "gap between the letters in the
// 2nd word" a user sees when dragging a selection across a space and into a
// word. The fix paints the whole string as ONE shaped run, then overlays the
// selected color clipped to the selection rect, so a glyph cannot move just
// because it became selected.
//
// The invariant under test is text-engine-level and design-agnostic. We
// neutralize every selection color (selection fill and selected-text color both
// resolve to colors that paint identically over the background) so that the
// ONLY thing that could change pixels between the unselected and selected
// renders is a glyph moving. A correct painter therefore yields two identical
// frames; the old three-run painter shifts the mid-word selected glyphs and the
// frames diverge.
TEST_CASE("TextEditor selecting mid-word recolors in place without moving glyphs",
          "[view][text_editor][selection][svg]") {
    const std::string kText = "WAVE table mix";   // strong W-A kern; spaces
    constexpr int kW = 240, kH = 30;
    constexpr float kScale = 2.0f;

    // White page, black text. selected-text color resolves from "bg.primary"
    // (black, == text) and the selection fill from "accent.primary" (white, ==
    // page, alpha-blended to a no-op). So selection changes no color — only a
    // moved glyph can change a pixel.
    Theme neutral;
    neutral.colors["text.primary"]   = Color::rgba8(0, 0, 0, 255);
    neutral.colors["bg.primary"]     = Color::rgba8(0, 0, 0, 255);
    neutral.colors["accent.primary"] = Color::rgba8(255, 255, 255, 255);

    auto render = [&](bool select_mid_word) {
        TextEditor editor;
        editor.set_theme(neutral);
        editor.set_background_color(Color::rgba8(255, 255, 255, 255));
        editor.set_bounds({0, 0, float(kW), float(kH)});
        editor.set_text(kText);
        if (select_mid_word) {
            editor.on_focus_changed(true);
            editor.on_key_event(key_event(KeyCode::home));         // caret -> 0
            editor.on_key_event(key_event(KeyCode::right));        // caret -> 1 (after 'W')
            auto shift_right = key_event(KeyCode::right, kModShift);
            for (int i = 0; i < 3; ++i) editor.on_key_event(shift_right);  // select "AVE"
            editor.on_focus_changed(false);   // drop the caret; selection persists
        }
        // Both frames render unfocused: no caret, identical background.
        return render_to_png(editor, kW, kH, kScale, ScreenshotBackend::skia);
    };

    const auto base = render(false);
    if (base.empty()) SKIP("Skia raster screenshot backend unavailable");
    const auto selected = render(true);
    REQUIRE_FALSE(selected.empty());

    // Some partial-Skia lanes no-op raster text; skip rather than false-fail.
    const auto stats = analyze_screenshot_content(base);
    if (!stats.passes_content_floor()) SKIP("native raster unavailable in this build");

    // With colors neutralized, the only source of difference is a moving glyph.
    // Concentrate the signal on the first word ("WAVE", left ~25% of the strip)
    // where the mid-word selection lives; the global frame is mostly the stable
    // tail, which dilutes the metric.
    const uint32_t png_w = static_cast<uint32_t>(kW * kScale);
    const uint32_t png_h = static_cast<uint32_t>(kH * kScale);
    const uint32_t band_w = png_w / 4;   // left quarter: comfortably covers "WAVE"
    const auto base_band = crop_png(base, 0, 0, band_w, png_h);
    const auto sel_band = crop_png(selected, 0, 0, band_w, png_h);
    REQUIRE_FALSE(base_band.empty());
    REQUIRE_FALSE(sel_band.empty());
    const auto cmp = compare_screenshots(base_band, sel_band, /*tolerance=*/4);
    REQUIRE(cmp.valid);
    INFO("band similarity = " << cmp.similarity);
    // A correct painter tiles the width with disjoint clips, so neutralized
    // selection leaves the band pixel-identical. The old three-run painter
    // shifted the selected glyphs, dropping the band well below this floor
    // (~0.95 at the time of the fix).
    CHECK(cmp.similarity >= 0.99f);
}
