#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/field_edit.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::vellum_components;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::kModCmd;
using ::vellum::view::kModCtrl;
using ::vellum::view::kModShift;
using ::vellum::view::MouseEvent;

namespace {

KeyEvent key(KeyCode code, std::uint16_t modifiers = 0, bool down = true) {
    KeyEvent e;
    e.key = code;
    e.modifiers = modifiers;
    e.is_down = down;
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
    EditValue value;
    std::string source;
};

// A root, one anchored child, and an overlay that records every committed
// edit. The child is selected through the ordinary click path.
struct FieldScene {
    View root;
    View* child = nullptr;
    std::vector<Reported> edits;
    InspectorOverlay overlay{root};

    FieldScene() {
        root.set_bounds({0, 0, 500, 400});
        auto c = std::make_unique<View>();
        c->set_anchor_id("figma:field-edit:1");
        c->set_bounds({10, 10, 80, 40});
        c->flex().padding = 8;
        c->flex().margin = 4;
        c->flex().preferred_width = 80;
        c->flex().preferred_height = 40;
        child = c.get();
        root.add_child(std::move(c));

        overlay.set_active(true);
        overlay.set_edit_sink([this](std::string_view anchor, std::string_view path,
                                     const EditValue& value, std::string_view source) {
            edits.push_back({std::string(anchor), std::string(path), value, std::string(source)});
        });
        MouseEvent click;
        click.position = {20, 20};
        click.is_down = true;
        overlay.handle_mouse_event(click);
    }

    float last_number() const {
        REQUIRE_FALSE(edits.empty());
        return std::get<float>(edits.back().value);
    }
};

} // namespace

TEST_CASE("begin_field_edit seeds the buffer and caret", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    REQUIRE(s.overlay.selected_view() == s.child);
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() == "layout.padding");
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);
}

TEST_CASE("begin_field_edit refuses without a selection or a path",
          "[pulp-vellum-components][field-edit]") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    REQUIRE_FALSE(overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE_FALSE(overlay.is_editing());

    FieldScene s;
    REQUIRE_FALSE(s.overlay.begin_field_edit("", 8.0f));
    REQUIRE_FALSE(s.overlay.is_editing());
}

TEST_CASE("typing digits extends the buffer and previews live",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::num5)));
    REQUIRE(s.overlay.edit_buffer() == "85");
    REQUIRE(s.overlay.edit_caret_pos() == 2);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::num0)));
    REQUIRE(s.overlay.edit_buffer() == "850");
    REQUIRE(s.child->flex().padding == 850.0f);
    REQUIRE(s.edits.empty());
}

TEST_CASE("a digit with a chord modifier is not typed", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::num5, kMainModifier)));
    REQUIRE(s.overlay.edit_buffer() == "8");
}

TEST_CASE("backspace trims the buffer", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.overlay.edit_buffer() == "85");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::backspace)));
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);
    // The trim previews live like a typed digit.
    REQUIRE(s.child->flex().padding == 8.0f);
}

TEST_CASE("Enter applies the value and reports it to the sink",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.edits.empty());
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::enter)));

    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].anchor == "figma:field-edit:1");
    REQUIRE(s.edits[0].path == "layout.padding");
    REQUIRE(s.edits[0].source == "inspector-keyboard-edit");
    REQUIRE(s.last_number() == 85.0f);
    REQUIRE(s.child->flex().padding == 85.0f);
}

TEST_CASE("Escape cancels, reverts and reports nothing", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);

    REQUIRE(s.overlay.handle_key_event(key(KeyCode::escape)));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.edits.empty());
    REQUIRE(s.child->flex().padding == 8.0f);
}

TEST_CASE("Up and Down nudge by one", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "9");
    REQUIRE(s.child->flex().padding == 9.0f);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "10");
    REQUIRE(s.child->flex().padding == 10.0f);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::down)));
    REQUIRE(s.overlay.edit_buffer() == "9");
}

TEST_CASE("Shift nudges by ten and the main modifier by a hundred",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "18");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::down, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up, kMainModifier)));
    REQUIRE(s.overlay.edit_buffer() == "108");
    REQUIRE(s.child->flex().padding == 108.0f);
}

TEST_CASE("Tab commits and moves to the next field, Shift+Tab to the previous",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));

    REQUIRE(s.overlay.handle_key_event(key(KeyCode::tab)));
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].path == "layout.padding");
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() == "layout.margin");
    REQUIRE(s.overlay.edit_buffer() == "4");

    REQUIRE(s.overlay.handle_key_event(key(KeyCode::tab, kModShift)));
    REQUIRE(s.overlay.editing_field() == "layout.padding");
    // The padding commit above applied 85, and the field reads it back.
    REQUIRE(s.overlay.edit_buffer() == "85");
}

TEST_CASE("Tab wraps around the field list", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.margin", 4.0f);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::tab)));
    REQUIRE(s.overlay.editing_field() == "style.opacity");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::tab, kModShift)));
    REQUIRE(s.overlay.editing_field() == "layout.margin");
}

TEST_CASE("layout.width writes preferred_width", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.width", s.child->flex().preferred_width);
    REQUIRE(s.overlay.edit_buffer() == "80");
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.child->flex().preferred_width == 805.0f);
    s.overlay.handle_key_event(key(KeyCode::enter));
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].path == "layout.width");
    REQUIRE(s.last_number() == 805.0f);
}

TEST_CASE("layout.height writes preferred_height", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.height", s.child->flex().preferred_height);
    s.overlay.handle_key_event(key(KeyCode::num2));
    REQUIRE(s.child->flex().preferred_height == 402.0f);
}

TEST_CASE("opacity shows two decimals and clamps to one", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.child->set_opacity(0.5f);
    s.overlay.begin_field_edit("style.opacity", 0.5f);
    REQUIRE(s.overlay.edit_buffer() == "0.50");
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "1.50");
    REQUIRE(s.child->opacity() == 1.0f);
    s.overlay.handle_key_event(key(KeyCode::enter));
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].path == "style.opacity");
}

TEST_CASE("cancel_field_edit with no edit is a no-op", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.cancel_field_edit();
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.edits.empty());
}

TEST_CASE("commit with an empty buffer stops editing and reports nothing",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::backspace));
    REQUIRE(s.overlay.edit_buffer().empty());
    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.edits.empty());
}

TEST_CASE("the caret moves with Left, Right, Home and End",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 80.0f);
    REQUIRE(s.overlay.edit_caret_pos() == 2);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::left)));
    REQUIRE(s.overlay.edit_caret_pos() == 1);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::home)));
    REQUIRE(s.overlay.edit_caret_pos() == 0);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::end_)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::right)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);
}

TEST_CASE("a digit typed mid-buffer lands at the caret", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 80.0f);
    s.overlay.handle_key_event(key(KeyCode::left));
    s.overlay.handle_key_event(key(KeyCode::num1));
    REQUIRE(s.overlay.edit_buffer() == "810");
    REQUIRE(s.overlay.edit_caret_pos() == 2);
}

TEST_CASE("deactivating the overlay cancels an open edit", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);
    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.child->flex().padding == 8.0f);
    REQUIRE(s.edits.empty());
}

TEST_CASE("Escape inside an edit cancels it and keeps the selection; the next Escape deselects",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.overlay.is_active());
    REQUIRE(s.overlay.selected_view() == s.child);

    s.overlay.handle_key_event(key(KeyCode::escape));
    REQUIRE(s.overlay.is_active());
    REQUIRE(s.overlay.selected_view() == nullptr);
}

TEST_CASE("an open edit owns the keyboard: mode keys type nothing and flip nothing",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::z)));
    REQUIRE_FALSE(s.overlay.zoom_active());
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::m)));
    REQUIRE(s.overlay.selection_mode() == InspectorOverlay::SelectionMode::follows_focus);
    REQUIRE(s.overlay.edit_buffer() == "8");

    // Once the edit ends, the same keys are mode keys again.
    s.overlay.cancel_field_edit();
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::z)));
    REQUIRE(s.overlay.zoom_active());
}

TEST_CASE("a key release does not touch the edit", "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::num5, 0, false)));
    REQUIRE(s.overlay.edit_buffer() == "8");
}

TEST_CASE("the edit keeps writing to the view it began on after the selection moves",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    auto other = std::make_unique<View>();
    other->set_anchor_id("figma:field-edit:2");
    other->set_bounds({200, 10, 80, 40});
    other->flex().padding = 3;
    View* other_ptr = other.get();
    s.root.add_child(std::move(other));

    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.set_selected_view(other_ptr);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);
    REQUIRE(other_ptr->flex().padding == 3.0f);
    s.overlay.handle_key_event(key(KeyCode::enter));
    REQUIRE(s.edits.size() == 1);
    REQUIRE(s.edits[0].anchor == "figma:field-edit:1");
}

TEST_CASE("an edit on a view that left the tree is dropped without a write",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    // Removing the child frees it, so the overlay holds a dangling pointer it
    // must only compare, never read.
    REQUIRE(s.root.remove_child(s.child) != nullptr);
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::num7)));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.edits.empty());
}

TEST_CASE("painting drops an edit whose view left the tree",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(s.root.remove_child(s.child) != nullptr);
    ::vellum::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);
    REQUIRE_FALSE(s.overlay.is_editing());
}

TEST_CASE("a commit on a view that left the tree reports nothing",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    auto detached = s.root.remove_child(s.child);
    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE(s.edits.empty());
}

TEST_CASE("an edit is not reported for a view without an anchor id or with no sink",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.child->set_anchor_id("");
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE(s.edits.empty());
    // The live edit still applied.
    REQUIRE(s.child->flex().padding == 85.0f);

    s.child->set_anchor_id("figma:field-edit:1");
    s.overlay.set_edit_sink({});
    REQUIRE_FALSE(s.overlay.has_edit_sink());
    s.overlay.begin_field_edit("layout.padding", 85.0f);
    s.overlay.handle_key_event(key(KeyCode::num1));
    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE(s.child->flex().padding == 851.0f);
}

TEST_CASE("Cmd+I while editing closes the overlay and reverts the edit",
          "[pulp-vellum-components][field-edit]") {
    FieldScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(key(KeyCode::num5));
    REQUIRE(s.overlay.handle_key_event(key(KeyCode::i, kMainModifier)));
    REQUIRE_FALSE(s.overlay.is_active());
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.child->flex().padding == 8.0f);
}

TEST_CASE("field values read, write and format as the panel shows them",
          "[pulp-vellum-components][field-edit]") {
    View v;
    v.flex().padding = 6;
    v.flex().margin = 2;
    v.flex().preferred_width = 30;
    v.flex().preferred_height = 20;
    v.set_opacity(0.25f);
    REQUIRE(overlay::read_field_value(v, "layout.padding") == 6.0f);
    REQUIRE(overlay::read_field_value(v, "layout.margin") == 2.0f);
    REQUIRE(overlay::read_field_value(v, "layout.width") == 30.0f);
    REQUIRE(overlay::read_field_value(v, "layout.height") == 20.0f);
    REQUIRE(overlay::read_field_value(v, "style.opacity") == 0.25f);
    REQUIRE(overlay::read_field_value(v, "nonsense") == 0.0f);

    overlay::write_field_value(v, "layout.margin", 9.0f);
    REQUIRE(v.flex().margin == 9.0f);
    overlay::write_field_value(v, "nonsense", 9.0f);
    REQUIRE(v.flex().padding == 6.0f);

    REQUIRE(overlay::format_field_value(8.0f) == "8");
    REQUIRE(overlay::format_field_value(0.5f) == "0.50");
    REQUIRE(overlay::format_field_value(-3.0f) == "-3");
}

TEST_CASE("FieldEditBuffer refuses non-digits and nudges from the original while empty",
          "[pulp-vellum-components][field-edit]") {
    overlay::FieldEditBuffer b;
    REQUIRE_FALSE(b.active());
    b.begin("layout.padding", 8.0f);
    REQUIRE(b.active());
    REQUIRE_FALSE(b.insert_digit('x'));
    REQUIRE(b.text() == "8");
    b.backspace();
    REQUIRE_FALSE(b.value().has_value());
    REQUIRE(b.nudge(2.0f) == 10.0f);
    REQUIRE(b.text() == "10");

    // A fractional start keeps two decimals even when the result is whole.
    b.begin("style.opacity", 0.5f);
    REQUIRE(b.nudge(0.5f) == 1.0f);
    REQUIRE(b.text() == "1.00");
    b.clear();
    REQUIRE_FALSE(b.active());
    REQUIRE(b.text().empty());
}
