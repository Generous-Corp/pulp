#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>
#include <pulp/vellum_components/text_edit.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>

#include "render_helpers.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::kModAlt;
using ::vellum::view::kModCmd;
using ::vellum::view::kModCtrl;
using ::vellum::view::kModShift;
using ::vellum::view::MouseEvent;
using ::vellum::view::TextEditor;
using ::vellum::view::TextInputEvent;
using Tool = InspectorOverlay::Tool;

namespace {

KeyEvent key(KeyCode code, std::uint16_t modifiers = 0) {
    KeyEvent e;
    e.key = code;
    e.modifiers = modifiers;
    e.is_down = true;
    return e;
}

TextInputEvent typed(std::string text) {
    TextInputEvent e;
    e.text = std::move(text);
    return e;
}

MouseEvent press(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    return e;
}

#ifdef __APPLE__
constexpr std::uint16_t kMainModifier = kModCmd;
#else
constexpr std::uint16_t kMainModifier = kModCtrl;
#endif

struct Reported {
    std::string anchor;
    std::string path;
    std::string text;
    std::string source;
};

// An in-memory clipboard so no test touches the system pasteboard.
struct FakeClipboard {
    std::string content;
    bool writable = true;
    int writes = 0;

    overlay::TextClipboard handle() {
        return {[this](const std::string& text) {
                    ++writes;
                    if (!writable)
                        return false;
                    content = text;
                    return true;
                },
                [this]() -> std::optional<std::string> {
                    if (content.empty())
                        return std::nullopt;
                    return content;
                }};
    }
};

// A root with one anchored Label, and an overlay on the Text tool that
// records every committed edit.
struct TextScene {
    View root;
    Label* label = nullptr;
    FakeClipboard clipboard;
    std::vector<Reported> edits;
    InspectorOverlay overlay{root};

    explicit TextScene(const std::string& text = "Hello") {
        root.set_bounds({0, 0, 400, 300});
        auto l = std::make_unique<Label>(text);
        l->set_anchor_id("figma:label-1");
        l->set_bounds({20, 20, 120, 30});
        label = l.get();
        root.add_child(std::move(l));
        overlay.set_active(true);
        overlay.set_text_clipboard(clipboard.handle());
        overlay.set_edit_sink([this](std::string_view anchor, std::string_view path,
                                     const EditValue& value, std::string_view source) {
            // Only copy edits are of interest here; a numeric field commit is
            // reported through the same sink.
            if (const auto* text = std::get_if<std::string>(&value))
                edits.push_back(
                    {std::string(anchor), std::string(path), *text, std::string(source)});
        });
        overlay.set_tool(Tool::text);
    }
};

// Pin a view to a rectangle so the layout pass a render runs keeps it there.
void place(View& v, float x, float y, float w, float h) {
    v.set_position(View::Position::absolute);
    v.set_left(x);
    v.set_top(y);
    v.flex().preferred_width = w;
    v.flex().preferred_height = h;
    v.set_bounds({x, y, w, h});
}

} // namespace

// ── The tool and the click ──────────────────────────────────────────────────

TEST_CASE("V and T switch tools, and set_tool and toggle_tool mirror them",
          "[pulp-vellum-components][text-tool]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    REQUIRE(overlay.tool() == Tool::select);
    // Inactive overlays take no keys.
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::t)));
    REQUIRE(overlay.tool() == Tool::select);

    overlay.set_active(true);
    REQUIRE(overlay.handle_key_event(key(KeyCode::t)));
    REQUIRE(overlay.tool() == Tool::text);
    REQUIRE(overlay.handle_key_event(key(KeyCode::v)));
    REQUIRE(overlay.tool() == Tool::select);

    overlay.set_tool(Tool::text);
    REQUIRE(overlay.tool() == Tool::text);
    overlay.toggle_tool();
    REQUIRE(overlay.tool() == Tool::select);

    // A modified T or V is not a tool key.
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::t, kModAlt)));
    REQUIRE(overlay.tool() == Tool::select);
}

TEST_CASE("V and T do not flip the tool while an edit is open",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    REQUIRE(s.overlay.begin_text_edit(s.label));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::v)));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::t)));
    REQUIRE(s.overlay.tool() == Tool::text);
    REQUIRE(s.overlay.text_editing());

    // Escape ends the edit and returns to the Select tool, after which the
    // keys are tool keys again.
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::escape)));
    REQUIRE(s.overlay.tool() == Tool::select);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::t)));
    REQUIRE(s.overlay.tool() == Tool::text);
}

TEST_CASE("V and T do not flip the tool while a numeric field is being edited",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_tool(Tool::select);
    s.overlay.set_selected_view(s.label);
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 0.0f));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::t)));
    REQUIRE(s.overlay.tool() == Tool::select);
}

TEST_CASE("a Text-tool click begins an inline edit instead of selecting for a drag",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_dragging_enabled(true);
    REQUIRE(s.overlay.handle_mouse_event(press(40, 35)));
    REQUIRE(s.overlay.text_editing());
    REQUIRE(s.overlay.text_edit_target() == s.label);
    REQUIRE(s.overlay.text_edit_buffer() == "Hello");
    REQUIRE(s.overlay.text_caret() == 5);
    // The target becomes the selection, and nothing was reported.
    REQUIRE(s.overlay.selected_view() == s.label);
    REQUIRE(s.edits.empty());
}

TEST_CASE("a Text-tool press on something that is not text is consumed and edits nothing",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    auto plain = std::make_unique<View>();
    plain->set_bounds({200, 20, 80, 30});
    s.root.add_child(std::move(plain));
    REQUIRE(s.overlay.handle_mouse_event(press(220, 35)));
    REQUIRE_FALSE(s.overlay.text_editing());
    // It did not select either: the Text tool never selects or drags.
    REQUIRE(s.overlay.selected_view() == nullptr);
}

TEST_CASE("a Text-tool press on a child of a Label edits the Label",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    auto child = std::make_unique<View>();
    child->set_bounds({5, 5, 20, 10});
    s.label->add_child(std::move(child));
    REQUIRE(s.overlay.handle_mouse_event(press(30, 30)));
    REQUIRE(s.overlay.text_edit_target() == s.label);
}

TEST_CASE("pressing another text element commits the open edit and starts the next",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    auto second = std::make_unique<Label>("World");
    second->set_anchor_id("figma:label-2");
    second->set_bounds({20, 100, 120, 30});
    Label* second_ptr = second.get();
    s.root.add_child(std::move(second));

    REQUIRE(s.overlay.handle_mouse_event(press(40, 35)));
    s.overlay.handle_text_input(typed("!"));
    REQUIRE(s.overlay.handle_mouse_event(press(40, 115)));
    REQUIRE(s.overlay.text_edit_target() == second_ptr);
    REQUIRE(s.label->text() == "Hello!");
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].anchor == "figma:label-1");
    REQUIRE(s.edits[0].text == "Hello!");
}

TEST_CASE("pressing the element being edited keeps the edit and its text",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    REQUIRE(s.overlay.handle_mouse_event(press(40, 35)));
    s.overlay.handle_text_input(typed("!"));
    REQUIRE(s.overlay.handle_mouse_event(press(60, 35)));
    REQUIRE(s.overlay.text_editing());
    REQUIRE(s.edits.empty());
}

TEST_CASE("Select-tool presses still select", "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_tool(Tool::select);
    REQUIRE(s.overlay.handle_mouse_event(press(40, 35)));
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.overlay.selected_view() == s.label);
}

TEST_CASE("begin_text_edit refuses a null or non-text view",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    REQUIRE_FALSE(s.overlay.begin_text_edit(nullptr));
    REQUIRE_FALSE(s.overlay.begin_text_edit(&s.root));
    REQUIRE_FALSE(s.overlay.text_editing());
}

TEST_CASE("a TextEditor is editable through the Text tool", "[pulp-vellum-components][text-tool]") {
    TextScene s;
    auto editor = std::make_unique<TextEditor>();
    editor->set_text("field");
    editor->set_bounds({20, 100, 120, 30});
    TextEditor* editor_ptr = editor.get();
    s.root.add_child(std::move(editor));
    REQUIRE(s.overlay.handle_mouse_event(press(40, 115)));
    REQUIRE(s.overlay.text_edit_target() == editor_ptr);
    s.overlay.handle_text_input(typed("s"));
    REQUIRE(editor_ptr->text() == "fields");
    s.overlay.handle_key_event(key(KeyCode::escape));
    REQUIRE(editor_ptr->text() == "field");
}

// ── Typing, committing, cancelling ──────────────────────────────────────────

TEST_CASE("typing then Enter keeps the copy and reports a text edit",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("Old");
    REQUIRE(s.overlay.begin_text_edit(s.label));
    for (int i = 0; i < 3; ++i)
        REQUIRE(s.overlay.handle_key_event(key(KeyCode::backspace)));
    REQUIRE(s.overlay.text_edit_buffer().empty());
    REQUIRE(s.label->text().empty());

    REQUIRE(s.overlay.handle_text_input(typed("New")));
    REQUIRE(s.overlay.text_edit_buffer() == "New");
    REQUIRE(s.label->text() == "New");
    REQUIRE(s.edits.empty());

    REQUIRE(s.overlay.handle_key_event(key(KeyCode::enter)));
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.label->text() == "New");
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].anchor == "figma:label-1");
    REQUIRE(s.edits[0].path == "text");
    REQUIRE(s.edits[0].text == "New");
    REQUIRE(s.edits[0].source == "inspector-text-edit");
}

TEST_CASE("committing unchanged text reports nothing", "[pulp-vellum-components][text-tool]") {
    TextScene s("Same");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("x"));
    s.overlay.handle_key_event(key(KeyCode::backspace));
    REQUIRE_FALSE(s.overlay.commit_text_edit());
    REQUIRE(s.edits.empty());
    REQUIRE(s.label->text() == "Same");
}

TEST_CASE("the committed path is configurable and ignores an empty one",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_text_tweak_path("content");
    s.overlay.set_text_tweak_path("");
    REQUIRE(s.overlay.text_tweak_path() == "content");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("!"));
    s.overlay.commit_text_edit();
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].path == "content");
}

TEST_CASE("an edit on a Label with no anchor id keeps the live copy and reports nothing",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.label->set_anchor_id("");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("!"));
    REQUIRE_FALSE(s.overlay.commit_text_edit());
    REQUIRE(s.label->text() == "Hello!");
    REQUIRE(s.edits.empty());
}

TEST_CASE("Escape cancels the edit, restores the copy and leaves the Text tool",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("Keep");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("zzz"));
    REQUIRE(s.label->text() == "Keepzzz");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::escape)));
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.label->text() == "Keep");
    REQUIRE(s.overlay.tool() == Tool::select);
    REQUIRE(s.edits.empty());
    // The selection survives the cancel.
    REQUIRE(s.overlay.selected_view() == s.label);
}

TEST_CASE("switching to the Select tool mid-edit commits it",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("!"));
    s.overlay.set_tool(Tool::select);
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.label->text() == "Hello!");
    REQUIRE(s.edits.size() == 1);
}

TEST_CASE("deactivating the overlay cancels an open text edit",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_text_input(typed("!"));
    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.label->text() == "Hello");
    REQUIRE(s.edits.empty());
}

TEST_CASE("text input is declined unless an edit is open and the overlay is on",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    REQUIRE_FALSE(s.overlay.handle_text_input(typed("a")));
    s.overlay.begin_text_edit(s.label);
    // Empty input is still consumed so it cannot reach a widget.
    REQUIRE(s.overlay.handle_text_input(typed("")));
    REQUIRE(s.overlay.text_edit_buffer() == "Hello");
    s.overlay.cancel_text_edit();
    REQUIRE_FALSE(s.overlay.handle_text_input(typed("a")));
}

TEST_CASE("an open edit declines character keys and flips no mode",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::z)));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::m)));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::a)));
    REQUIRE_FALSE(s.overlay.zoom_active());
    REQUIRE(s.overlay.selection_mode() == InspectorOverlay::SelectionMode::follows_focus);
    REQUIRE(s.overlay.text_edit_buffer() == "Hello");
}

// ── Caret, selection, clipboard ─────────────────────────────────────────────

TEST_CASE("arrows move the caret and typing inserts at it", "[pulp-vellum-components][text-tool]") {
    TextScene s("abc");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.text_caret() == 3);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left)));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left)));
    REQUIRE(s.overlay.text_caret() == 1);
    REQUIRE(s.overlay.handle_text_input(typed("X")));
    REQUIRE(s.overlay.text_edit_buffer() == "aXbc");
    REQUIRE(s.label->text() == "aXbc");
    REQUIRE(s.overlay.text_caret() == 2);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::right)));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::right)));
    s.overlay.handle_text_input(typed("!"));
    REQUIRE(s.overlay.text_edit_buffer() == "aXbc!");
}

TEST_CASE("Shift-arrow extends the selection and typing replaces it",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("abcd");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left, kModShift)));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left, kModShift)));
    REQUIRE(s.overlay.text_has_selection());
    REQUIRE(s.overlay.text_selection() == std::pair<std::size_t, std::size_t>{2, 4});
    s.overlay.handle_text_input(typed("Z"));
    REQUIRE(s.overlay.text_edit_buffer() == "abZ");
    REQUIRE_FALSE(s.overlay.text_has_selection());
}

TEST_CASE("Home, End, Up, Down and the main modifier with Left and Right reach the ends",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("abcd");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::home)));
    REQUIRE(s.overlay.text_caret() == 0);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::end_)));
    REQUIRE(s.overlay.text_caret() == 4);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up)));
    REQUIRE(s.overlay.text_caret() == 0);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::down)));
    REQUIRE(s.overlay.text_caret() == 4);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left, kMainModifier)));
    REQUIRE(s.overlay.text_caret() == 0);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::right, kMainModifier | kModShift)));
    REQUIRE(s.overlay.text_caret() == 4);
    REQUIRE(s.overlay.text_has_selection());
}

TEST_CASE("Alt with an arrow jumps by word", "[pulp-vellum-components][text-tool]") {
    TextScene s("one two");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left, kModAlt)));
    REQUIRE(s.overlay.text_caret() == 4);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left, kModAlt)));
    REQUIRE(s.overlay.text_caret() == 0);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::right, kModAlt)));
    REQUIRE(s.overlay.text_caret() == 3);
}

TEST_CASE("Delete removes forward and honours a selection", "[pulp-vellum-components][text-tool]") {
    TextScene s("abcd");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_key_event(key(KeyCode::home));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::delete_)));
    REQUIRE(s.overlay.text_edit_buffer() == "bcd");
    s.overlay.handle_key_event(key(KeyCode::right, kModShift));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::delete_)));
    REQUIRE(s.overlay.text_edit_buffer() == "cd");
    REQUIRE(s.label->text() == "cd");
}

TEST_CASE("select-all then paste replaces the whole copy", "[pulp-vellum-components][text-tool]") {
    TextScene s("hello");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::a, kMainModifier)));
    REQUIRE(s.overlay.text_selection() == std::pair<std::size_t, std::size_t>{0, 5});
    s.clipboard.content = "WORLD";
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::v, kMainModifier)));
    REQUIRE(s.overlay.text_edit_buffer() == "WORLD");
    REQUIRE(s.label->text() == "WORLD");
    REQUIRE_FALSE(s.overlay.text_has_selection());
    REQUIRE(s.overlay.text_caret() == 5);
}

TEST_CASE("paste with an empty clipboard changes nothing", "[pulp-vellum-components][text-tool]") {
    TextScene s("hello");
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::v, kMainModifier)));
    REQUIRE_FALSE(s.overlay.text_paste());
    REQUIRE(s.overlay.text_edit_buffer() == "hello");
}

TEST_CASE("copy puts the selection on the clipboard and leaves the text",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("hello world");
    s.overlay.begin_text_edit(s.label);
    // Nothing selected: nothing is written.
    REQUIRE_FALSE(s.overlay.text_copy());
    REQUIRE(s.clipboard.writes == 0);

    s.overlay.handle_key_event(key(KeyCode::left, kModShift | kModAlt));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::c, kMainModifier)));
    REQUIRE(s.clipboard.content == "world");
    REQUIRE(s.overlay.text_edit_buffer() == "hello world");
}

TEST_CASE("cut removes the selection even when the clipboard write fails",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("hello world");
    s.overlay.begin_text_edit(s.label);
    s.overlay.handle_key_event(key(KeyCode::left, kModShift | kModAlt));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::x, kMainModifier)));
    REQUIRE(s.clipboard.content == "world");
    REQUIRE(s.overlay.text_edit_buffer() == "hello ");
    REQUIRE(s.label->text() == "hello ");

    s.clipboard.writable = false;
    s.overlay.text_select_all();
    REQUIRE_FALSE(s.overlay.text_cut());
    REQUIRE(s.overlay.text_edit_buffer().empty());
    REQUIRE(s.clipboard.content == "world");
}

// ── Edits against a view that is gone ───────────────────────────────────────

TEST_CASE("painting drops a text edit whose Label left the tree",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.root.remove_child(s.label) != nullptr); // frees the Label
    RecordingCanvas canvas;
    REQUIRE_NOTHROW(s.overlay.paint(canvas));
    REQUIRE_FALSE(s.overlay.text_editing());
    REQUIRE(s.overlay.text_edit_target() == nullptr);
    REQUIRE(s.overlay.text_edit_buffer().empty());
    REQUIRE_FALSE(s.overlay.handle_text_input(typed("x")));
    REQUIRE(s.edits.empty());
}

TEST_CASE("text input and a commit on a freed Label write nothing and report nothing",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.root.remove_child(s.label) != nullptr);
    REQUIRE_FALSE(s.overlay.handle_text_input(typed("x")));
    REQUIRE_FALSE(s.overlay.text_editing());

    TextScene t;
    t.overlay.begin_text_edit(t.label);
    REQUIRE(t.root.remove_child(t.label) != nullptr);
    REQUIRE_FALSE(t.overlay.commit_text_edit());
    REQUIRE_FALSE(t.overlay.text_editing());
    REQUIRE(t.edits.empty());
}

TEST_CASE("a key on a freed text target ends the edit and falls through",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.root.remove_child(s.label) != nullptr);
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::backspace)));
    REQUIRE_FALSE(s.overlay.text_editing());
}

TEST_CASE("a text edit and a numeric field edit never both own the keyboard",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_selected_view(s.label);
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 0.0f));
    REQUIRE(s.overlay.begin_text_edit(s.label));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.overlay.text_editing());

    REQUIRE_FALSE(s.overlay.begin_field_edit("layout.padding", 0.0f));
    REQUIRE_FALSE(s.overlay.is_editing());
}

// ── Selection chrome ────────────────────────────────────────────────────────

TEST_CASE("a text edit swaps the resize box for a subtle outline with no handles",
          "[pulp-vellum-components][text-tool]") {
    TextScene s;
    s.overlay.set_dragging_enabled(true);
    s.overlay.set_selected_view(s.label);
    REQUIRE_FALSE(s.overlay.selection_uses_subtle_edit_outline());

    RecordingCanvas before;
    s.overlay.paint(before);

    s.overlay.begin_text_edit(s.label);
    REQUIRE(s.overlay.selection_uses_subtle_edit_outline());
    RecordingCanvas during;
    s.overlay.paint(during);

    const auto rects = [](const RecordingCanvas& c, DrawCommand::Type type) {
        std::size_t n = 0;
        for (const auto& cmd : c.commands())
            if (cmd.type == type)
                ++n;
        return n;
    };
    // The box fills its rectangle and eight handles are filled squares; the
    // edit paints one outline and a caret band.
    REQUIRE(rects(before, DrawCommand::Type::fill_rect) >= 9);
    REQUIRE(rects(during, DrawCommand::Type::fill_rect) < 4);
    REQUIRE(rects(during, DrawCommand::Type::stroke_rect) == 1);
}

TEST_CASE("the caret and selection band paint as fills over the edited text",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("abcd");
    s.root.layout_children();
    s.overlay.begin_text_edit(s.label);
    s.overlay.text_select_all();
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    std::size_t fills = 0;
    for (const auto& cmd : canvas.commands())
        if (cmd.type == DrawCommand::Type::fill_rect)
            ++fills;
    REQUIRE(fills >= 1);
}

TEST_CASE("the caret blinks across painted frames and a keystroke makes it solid",
          "[pulp-vellum-components][text-tool]") {
    TextScene s("abcd");
    s.root.layout_children();
    s.overlay.begin_text_edit(s.label);
    const auto fills = [&] {
        RecordingCanvas canvas;
        s.overlay.paint(canvas);
        std::size_t n = 0;
        for (const auto& cmd : canvas.commands())
            if (cmd.type == DrawCommand::Type::fill_rect)
                ++n;
        return n;
    };
    const std::size_t on = fills();
    std::size_t off = on;
    // Thirty frames later the caret is in the dark half of its blink.
    for (int i = 0; i < 30; ++i)
        off = fills();
    REQUIRE(off < on);

    // Typing restarts the blink with the caret on.
    s.overlay.handle_text_input(typed("x"));
    REQUIRE(fills() == on);
}

// ── Hooks ───────────────────────────────────────────────────────────────────

TEST_CASE("the text hook routes input to the inspected root only",
          "[pulp-vellum-components][text-tool][hooks]") {
    TextScene s("Type here");
    View other_root;
    other_root.set_bounds({0, 0, 340, 300});
    s.overlay.begin_text_edit(s.label);
    install_inspector_hooks(s.overlay);

    REQUIRE(View::call_inspector_text_hook(typed("A"), &s.root));
    REQUIRE(s.overlay.text_edit_buffer() == "Type hereA");
    REQUIRE_FALSE(View::call_inspector_text_hook(typed("Z"), &other_root));
    REQUIRE(s.overlay.text_edit_buffer() == "Type hereA");
    // An event whose root is unknown is delivered.
    REQUIRE(View::call_inspector_text_hook(typed("B"), nullptr));
    REQUIRE(s.overlay.text_edit_buffer() == "Type hereAB");

    uninstall_inspector_hooks();
    REQUIRE_FALSE(View::call_inspector_text_hook(typed("C"), &s.root));
    REQUIRE(s.overlay.text_edit_buffer() == "Type hereAB");
}

TEST_CASE("destroying the owner leaves the text hook inert",
          "[pulp-vellum-components][text-tool][hooks]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    {
        InspectorOverlay overlay(root);
        overlay.set_active(true);
        install_inspector_hooks(overlay);
    }
    REQUIRE_FALSE(View::call_inspector_text_hook(typed("x"), &root));
}

// ── Pixels ──────────────────────────────────────────────────────────────────

TEST_CASE("a selected text edit renders the band, the caret and the thin outline",
          "[pulp-vellum-components][text-tool][pixels]") {
    View root;
    root.set_bounds({0, 0, 240, 100});
    auto label = std::make_unique<Label>("abcd");
    label->set_anchor_id("figma:label-1");
    place(*label, 20, 30, 160, 40);
    Label* label_ptr = label.get();
    root.add_child(std::move(label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(Tool::text);
    overlay.set_dragging_enabled(true);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    overlay.text_select_all();
    install_inspector_hooks(overlay);

    const auto frame = render(root, 240, 100);
    maybe_write_png(root, 240, 100, "inspector-overlay-text-edit.png");
    uninstall_inspector_hooks();

    const auto bg = frame.at(220, 90);
    // The selection band is a translucent blue over the glyph row: at least one
    // pixel inside the label is clearly bluer than the background, and none is
    // the orange of the resize box.
    int band_pixels = 0;
    int orange_pixels = 0;
    for (std::uint32_t y = 31; y < 69; ++y) {
        for (std::uint32_t x = 21; x < 179; ++x) {
            const auto p = frame.at(x, y);
            if (p.b > bg.b + 25 && p.b > p.r + 20)
                ++band_pixels;
            if (p.r > 200 && p.b < 100)
                ++orange_pixels;
        }
    }
    REQUIRE(band_pixels > 100);
    REQUIRE(orange_pixels == 0);

    // The thin outline is blue on the label's left edge, and the corner has no
    // handle square.
    const auto edge = frame.at(20, 50);
    REQUIRE(edge.b > edge.r + 20);
    const auto corner = frame.at(21, 31);
    REQUIRE_FALSE((corner.r > 200 && corner.b < 100));
}

TEST_CASE("a caret with no selection renders as a thin light column",
          "[pulp-vellum-components][text-tool][pixels]") {
    View root;
    root.set_bounds({0, 0, 240, 100});
    auto label = std::make_unique<Label>("abcd");
    place(*label, 20, 30, 160, 40);
    Label* label_ptr = label.get();
    root.add_child(std::move(label));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tool(Tool::text);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    install_inspector_hooks(overlay);

    // The first painted frame after the edit begins has the caret on.
    const auto frame = render(root, 240, 100);
    maybe_write_png(root, 240, 100, "inspector-overlay-text-caret.png");
    uninstall_inspector_hooks();

    const auto bg = frame.at(220, 90);
    int light_columns = 0;
    int brightest = 0;
    for (std::uint32_t x = 21; x < 179; ++x) {
        int run = 0;
        for (std::uint32_t y = 31; y < 69; ++y) {
            const auto p = frame.at(x, y);
            if (p.r > 220 && p.g > 220 && p.b > 220 && p.r > bg.r + 60) {
                ++run;
                brightest = std::max(brightest, p.r);
            }
        }
        if (run >= 8)
            ++light_columns;
    }
    // 1.5 px wide: one or two columns, not a band and not nothing.
    REQUIRE(light_columns >= 1);
    REQUIRE(light_columns <= 3);
    // The caret is 95% opaque off-white, not a saturated white: a colour built
    // from 0-255 values would clamp to pure white.
    REQUIRE(brightest < 250);
}
