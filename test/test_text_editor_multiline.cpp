// Multi-line TextEditor coverage for the audit's "text entry depth"
// follow-up. The base test_text_editor.cpp file already pins single-
// line behavior plus the hard-newline arrow-key path; these tests pin
// the previously-untested visual layer: wrapped layout, y-aware
// click-to-caret, caret-rect pixel positioning, and the contractual
// difference between single-line and multi-line Enter handling.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/text_editor.hpp>

#include <algorithm>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

KeyEvent key(KeyCode code, uint16_t modifiers = 0) {
    KeyEvent ev;
    ev.key = code;
    ev.modifiers = modifiers;
    ev.is_down = true;
    return ev;
}

// Render the editor once with a RecordingCanvas to populate the cached
// layout snapshot consulted by `caret_rect()` and the multi-line
// mouse hit-test. RecordingCanvas::measure_text returns
// `text.size() * 7.0f`, which keeps these tests deterministic across
// platforms — no Skia/HarfBuzz dependency.
void prime_layout(TextEditor& editor) {
    RecordingCanvas canvas;
    editor.paint(canvas);
}

constexpr float kRecChar = 7.0f; // RecordingCanvas glyph width.

} // namespace

TEST_CASE("TextEditor::caret_rect single-line reflects the measured caret column",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 200, 28});
    editor.on_focus_changed(true);
    editor.set_text("Hello"); // caret lands at end because focused
    prime_layout(editor);

    auto r0 = editor.caret_rect();
    REQUIRE(r0.width > 0.0f);
    REQUIRE(r0.height > 0.0f);

    KeyEvent left;
    left.key = KeyCode::left;
    left.is_down = true;
    REQUIRE(editor.on_key_event(left));
    prime_layout(editor);

    auto r1 = editor.caret_rect();
    REQUIRE(r1.y == r0.y);
    // Moving the caret left by one glyph must shift `caret_rect().x`
    // to the left by exactly one RecordingCanvas glyph width. This is
    // the pixel-level guarantee IME hosts depend on for the candidate
    // window placement.
    REQUIRE(r1.x == Catch::Approx(r0.x - kRecChar).margin(0.5));
}

TEST_CASE("TextEditor::caret_rect multi-line differs across visual rows for the same logical index",
          "[view][text_editor][multiline]") {
    TextEditor multi;
    multi.multi_line = true;
    multi.set_bounds({0, 0, 200, 80});
    multi.on_focus_changed(true);
    multi.set_text("aaaa\nbb"); // focused -> caret at end (index 7, row 1)
    prime_layout(multi);

    // Caret at end of doc — should be on the second hard line.
    auto r_multi_end = multi.caret_rect();

    TextEditor single;
    single.set_bounds({0, 0, 200, 28});
    single.on_focus_changed(true);
    single.set_text("aaaa\nbb");
    prime_layout(single);

    // Single-line treats the embedded newline as a literal char, so the
    // caret rect's y stays anchored to the one visible row.
    auto r_single_end = single.caret_rect();

    REQUIRE(r_multi_end.y > r_single_end.y);
}

TEST_CASE("TextEditor multi-line click in the second hard-newline row picks the right codepoint",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_bounds({0, 0, 200, 80});
    editor.on_focus_changed(true);
    editor.set_text("abcd\nefgh"); // caret at end (index 9, row 1)
    prime_layout(editor);

    // Move caret to start so we have a known anchor; then click row 2.
    REQUIRE(editor.on_key_event(key(KeyCode::home)));
    REQUIRE(editor.on_key_event(key(KeyCode::up)));
    prime_layout(editor);

    auto first_row_rect = editor.caret_rect();

    MouseEvent click;
    click.is_down = true;
    // Pick the column for the second 'g' (index 2 in "efgh") and use a
    // y inside the second row's band. The new char_index_at_point()
    // path uses the cached layout to find the row; before the fix the
    // hit-test ignored y entirely and would have collapsed to row 1.
    const float padding_x = 6.0f;
    click.position.x = padding_x + 2.5f * kRecChar;
    click.position.y = first_row_rect.y + first_row_rect.height * 1.5f;
    editor.on_mouse_event(click);

    // Expected: caret lands inside "efgh" near column 2 — that is,
    // index 5 (newline) + 2 = 7. Verify it is at least past the
    // newline; the half-glyph nearest-edge rule allows 7 or 8.
    REQUIRE(editor.caret_pos() >= 7);
    REQUIRE(editor.caret_pos() <= 8);
}

TEST_CASE("TextEditor multi-line soft-wrap recorded in the layout snapshot via caret_rect rows",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.multi_line = true;
    // Width chosen so RecordingCanvas (7px/char) wraps every ~5 chars
    // within the 6px padding on either side: inner_w ≈ 60 - 12 = 48,
    // which forces the 16-char string to wrap onto multiple visual rows.
    editor.set_bounds({0, 0, 60, 200});
    editor.on_focus_changed(true);
    editor.set_text("aaaaaaaaaaaaaaaa"); // 16 chars, no spaces, caret at end
    prime_layout(editor);

    auto end_rect = editor.caret_rect();

    // Move to the start of the logical line (no hard newlines exist
    // so this jumps to index 0). With soft-wrap engaged, caret_rect at
    // index 0 must be on row 0 — y must be lower than the end-of-text
    // y because the text wrapped to multiple visual rows.
    //
    // NB: arrow Up/Down across soft-wrapped lines is a known follow-up
    // (TODO below). This test only pins that the layout snapshot
    // records multiple visual rows, which is what enables the eventual
    // arrow-key fix to be observable.
    REQUIRE(editor.on_key_event(key(KeyCode::home)));
    prime_layout(editor);
    auto home_rect = editor.caret_rect();

    REQUIRE(home_rect.y < end_rect.y);
}

TEST_CASE("TextEditor single-line Enter commits without inserting a newline",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.multi_line = false;
    editor.on_focus_changed(true);
    editor.set_text("abc");

    bool returned = false;
    std::string returned_text;
    editor.on_return = [&](const std::string& t) {
        returned = true;
        returned_text = t;
    };

    REQUIRE(editor.on_key_event(key(KeyCode::enter)));
    REQUIRE(returned);
    REQUIRE(returned_text == "abc");
    // Critical: must NOT insert a newline into the buffer.
    REQUIRE(editor.text() == "abc");
}

TEST_CASE("TextEditor multi-line down-then-up round trips when columns line up",
          "[view][text_editor][multiline]") {
    // Pin the hard-newline navigation surface from the user-facing
    // perspective: caret should walk up the lines and end up exactly
    // back where it started for monotonic columns. This complements
    // the column-preservation test in test_text_editor.cpp which
    // exercises the ragged-column edge case.
    TextEditor editor;
    editor.multi_line = true;
    editor.on_focus_changed(true);
    editor.set_text("abcd\nefgh\nijkl");

    REQUIRE(editor.on_key_event(key(KeyCode::up)));
    int after_up = editor.caret_pos();
    REQUIRE(editor.on_key_event(key(KeyCode::down)));
    REQUIRE(editor.caret_pos() == 14); // back to end of "ijkl"
    REQUIRE(after_up == 9); // end-column carried up to "efgh" tail
}

TEST_CASE("TextEditor multi-line click above the first row clamps to the first row",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_bounds({0, 0, 200, 80});
    editor.set_text("abcd\nefgh");
    editor.on_focus_changed(true);
    prime_layout(editor);

    MouseEvent click;
    click.is_down = true;
    click.position.x = 6.0f + 1.0f * kRecChar; // near "b"
    click.position.y = -50.0f; // way above the editor
    editor.on_mouse_event(click);

    // Y clamps to row 0 and the half-glyph rule lands on 1 or 2.
    REQUIRE(editor.caret_pos() >= 0);
    REQUIRE(editor.caret_pos() <= 2);
}

TEST_CASE("TextEditor multi-line click below the last row clamps to the last row",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.multi_line = true;
    editor.set_bounds({0, 0, 200, 80});
    editor.set_text("abcd\nefgh");
    editor.on_focus_changed(true);
    prime_layout(editor);

    MouseEvent click;
    click.is_down = true;
    click.position.x = 6.0f + 1.0f * kRecChar; // near "f"
    click.position.y = 500.0f; // way below the editor
    editor.on_mouse_event(click);

    // Y clamps to last row. Last row holds "efgh" starting at index 5,
    // so the caret must land in [5, 7].
    REQUIRE(editor.caret_pos() >= 5);
    REQUIRE(editor.caret_pos() <= 7);
}

// ── Single-line horizontal scroll: hit-test + caret_rect must honor it ──

TEST_CASE("TextEditor single-line hit-test honors horizontal scroll offset",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    // Narrow field; long text scrolls left so the leading chars are
    // clipped out of view.
    editor.set_bounds({0, 0, 60, 28});
    editor.on_focus_changed(true);
    editor.set_text("abcdefghijklmnop");
    // Caret at end (set_text sets caret_position_ to size when focused),
    // so scroll_offset_ becomes non-zero on the next paint.
    prime_layout(editor);
    REQUIRE(editor.scroll_offset() > 0.0f);

    // Click at the same x where the visible caret lives — the right
    // edge of the visible area. Pre-fix, this collapsed to ~index 0
    // because the snapshot's inner_x ignored scroll.
    const float scroll = editor.scroll_offset();
    MouseEvent click;
    click.is_down = true;
    click.position.x = std::max(9.0f, 2.0f) + scroll + 0.5f * kRecChar;
    click.position.y = 14.0f;
    editor.on_mouse_event(click);

    // The clicked position is one glyph past `scroll`px from the
    // padded inner-x, which in measured coords is index
    // `(scroll + 0.5*char_w) / char_w`. Confirm the hit-test landed
    // well past index 0 — bug would collapse to ≤1.
    const int floor_idx = static_cast<int>(scroll / kRecChar);
    REQUIRE(editor.caret_pos() >= floor_idx);
}

TEST_CASE("TextEditor::caret_rect single-line subtracts scroll_offset",
          "[view][text_editor][multiline]") {
    TextEditor editor;
    editor.set_bounds({0, 0, 60, 28});
    editor.on_focus_changed(true);
    editor.set_text("abcdefghijklmnop"); // caret at end
    prime_layout(editor);

    const float scroll = editor.scroll_offset();
    REQUIRE(scroll > 0.0f);

    auto r = editor.caret_rect();
    // Caret must land within the visible field bounds. Pre-fix, x was
    // off by `scroll` px (returned the unscrolled x) so it landed
    // well beyond bounds.width.
    REQUIRE(r.x >= 0.0f);
    REQUIRE(r.x <= editor.bounds().width);
}
