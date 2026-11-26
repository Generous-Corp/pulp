#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/text_edit.hpp>

#include <vellum/view/text_editor.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>

#include <string>
#include <utility>

using namespace pulp::vellum_components;
using ::vellum::view::TextEditor;

TEST_CASE("TextEditBuffer starts with the caret at the end and nothing selected",
          "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("abc");
    REQUIRE(b.caret() == 3);
    REQUIRE_FALSE(b.has_selection());
    REQUIRE(b.selection() == std::pair<std::size_t, std::size_t>{3, 3});
}

TEST_CASE("the caret steps whole code points, never into a multibyte character",
          "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    // "a", e-acute (2 bytes), a CJK character (3 bytes).
    b.begin("a\xC3\xA9\xE6\x97\xA5");
    REQUIRE(b.caret() == 6);
    b.move_caret(-1, false);
    REQUIRE(b.caret() == 3);
    b.move_caret(-1, false);
    REQUIRE(b.caret() == 1);
    b.move_caret(1, false);
    REQUIRE(b.caret() == 3);
    b.move_caret(-5, false);
    REQUIRE(b.caret() == 0);
}

TEST_CASE("Backspace and Delete remove a whole code point", "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("a\xC3\xA9\xE6\x97\xA5");
    REQUIRE(b.delete_backward());
    REQUIRE(b.text() == "a\xC3\xA9");
    b.move_home(false);
    REQUIRE(b.delete_forward());
    REQUIRE(b.text() == "\xC3\xA9");
    REQUIRE(b.delete_forward());
    REQUIRE(b.text().empty());
    // Nothing left to remove either way.
    REQUIRE_FALSE(b.delete_forward());
    REQUIRE_FALSE(b.delete_backward());
}

TEST_CASE("an arrow over a selection collapses to its near edge and stops",
          "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("abcdef");
    b.move_caret(-2, true); // selects "ef"
    REQUIRE(b.selection() == std::pair<std::size_t, std::size_t>{4, 6});
    b.move_caret(-1, false);
    REQUIRE_FALSE(b.has_selection());
    REQUIRE(b.caret() == 4);

    b.move_caret(2, true); // selects "ef" again, caret at 6
    b.move_caret(1, false);
    REQUIRE_FALSE(b.has_selection());
    REQUIRE(b.caret() == 6);
}

TEST_CASE("word moves skip separators then the word", "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("foo  bar_baz qux");
    b.move_word(-1, false);
    REQUIRE(b.caret() == 13);
    b.move_word(-1, false);
    REQUIRE(b.caret() == 5);
    b.move_word(-1, false);
    REQUIRE(b.caret() == 0);
    b.move_word(1, false);
    REQUIRE(b.caret() == 3);
    b.move_word(1, false);
    REQUIRE(b.caret() == 12);
    b.move_word(1, true);
    REQUIRE(b.selected_text() == " qux");
}

TEST_CASE("insert replaces the selection and lands at the caret",
          "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("abcd");
    b.move_caret(-2, true);
    REQUIRE(b.insert("Z"));
    REQUIRE(b.text() == "abZ");
    REQUIRE(b.caret() == 3);
    REQUIRE_FALSE(b.has_selection());

    b.move_caret(-2, false);
    b.insert("--");
    REQUIRE(b.text() == "a--bZ");
    REQUIRE(b.caret() == 3);
}

TEST_CASE("select_all and erase_selection", "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("hello");
    REQUIRE_FALSE(b.erase_selection());
    b.select_all();
    REQUIRE(b.selected_text() == "hello");
    REQUIRE(b.erase_selection());
    REQUIRE(b.text().empty());
    REQUIRE(b.caret() == 0);
}

TEST_CASE("the caret blink starts solid and a new edit restarts it",
          "[pulp-vellum-components][text-edit]") {
    overlay::TextEditBuffer b;
    b.begin("x");
    REQUIRE(b.tick_caret_visible(3));
    REQUIRE(b.tick_caret_visible(3));
    REQUIRE(b.tick_caret_visible(3));
    REQUIRE_FALSE(b.tick_caret_visible(3));
    b.insert("y");
    REQUIRE(b.blink_ticks() == 0);
    REQUIRE(b.tick_caret_visible(3));
}

TEST_CASE("only a Label or a TextEditor carries editable copy",
          "[pulp-vellum-components][text-edit]") {
    Label label("copy");
    TextEditor editor;
    editor.set_text("field");
    View plain;
    REQUIRE(overlay::view_has_editable_text(&label));
    REQUIRE(overlay::view_has_editable_text(&editor));
    REQUIRE_FALSE(overlay::view_has_editable_text(&plain));
    REQUIRE_FALSE(overlay::view_has_editable_text(nullptr));
    REQUIRE(overlay::editable_text_of(&label) == "copy");
    REQUIRE(overlay::editable_text_of(&editor) == "field");
    REQUIRE(overlay::editable_text_of(&plain).empty());

    overlay::set_editable_text_of(&label, "new");
    overlay::set_editable_text_of(&editor, "newer");
    overlay::set_editable_text_of(&plain, "ignored");
    REQUIRE(label.text() == "new");
    REQUIRE(editor.text() == "newer");
}

TEST_CASE("the system clipboard is wired without being touched",
          "[pulp-vellum-components][text-edit]") {
    // Calling it would read or overwrite the real pasteboard, so only the
    // wiring is checked.
    const auto clipboard = overlay::TextClipboard::system();
    REQUIRE(static_cast<bool>(clipboard.set));
    REQUIRE(static_cast<bool>(clipboard.get));
}
