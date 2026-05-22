#include <catch2/catch_test_macros.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <string>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

MouseEvent mouse_down(float x, float y, int click_count = 1) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    e.click_count = click_count;
    return e;
}

KeyEvent key_down(KeyCode key) {
    KeyEvent e;
    e.key = key;
    e.is_down = true;
    return e;
}

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_text &&
                                  cmd.text == text;
                       });
}

bool has_fill_text(const RecordingCanvas& canvas, const std::string& text) {
    return has_text(canvas, text);
}

int text_count(const RecordingCanvas& canvas, const std::string& text) {
    return static_cast<int>(std::count_if(
        canvas.commands().begin(),
        canvas.commands().end(),
        [&](const DrawCommand& cmd) {
            return cmd.type == DrawCommand::Type::fill_text &&
                   cmd.text == text;
        }));
}

} // namespace

TEST_CASE("TreeNode add children", "[view][tree]") {
    TreeNode root;
    auto& child1 = root.add_child("Synths");
    auto& child2 = root.add_child("Effects");

    REQUIRE(root.has_children());
    REQUIRE(root.children.size() == 2);
    REQUIRE(child1.label == "Synths");
    REQUIRE(child2.label == "Effects");
}

// WYSIWYG P6 FIX 3 — the TreeView reports a content height equal to its
// visible-row count * row height. A wrapping ScrollView sizes the tree to
// this so an expanded hierarchy past the viewport scrolls instead of
// clipping. Collapsed subtrees must NOT count toward the height.
TEST_CASE("TreeView content height grows with visible nodes", "[view][tree]") {
    TreeView tree;
    tree.set_row_height(22.0f);
    auto& root = tree.root();
    root.expanded = true;

    // Empty tree: no visible rows.
    REQUIRE(tree.visible_node_count() == 0);
    REQUIRE(tree.content_height() == 0.0f);

    // Two top-level rows.
    auto& a = root.add_child("A");
    root.add_child("B");
    REQUIRE(tree.visible_node_count() == 2);
    REQUIRE(tree.content_height() == 44.0f);

    // Add two children under A, collapsed — they must not count yet.
    a.add_child("A1");
    a.add_child("A2");
    a.expanded = false;
    REQUIRE(tree.visible_node_count() == 2);
    REQUIRE(tree.content_height() == 44.0f);

    // Expand A — its two children now contribute.
    a.expanded = true;
    REQUIRE(tree.visible_node_count() == 4);
    REQUIRE(tree.content_height() == 88.0f);

    // Row height scales the content height.
    tree.set_row_height(30.0f);
    REQUIRE(tree.content_height() == 120.0f);
}

TEST_CASE("TreeNode nested children", "[view][tree]") {
    TreeNode root;
    auto& synths = root.add_child("Synths");
    synths.add_child("Bass");
    synths.add_child("Lead");

    REQUIRE(synths.has_children());
    REQUIRE(synths.children.size() == 2);
    REQUIRE(synths.children[0]->label == "Bass");
}

TEST_CASE("TreeNode toggle expand", "[view][tree]") {
    TreeNode node;
    node.label = "Test";
    REQUIRE_FALSE(node.expanded);

    node.toggle();
    REQUIRE(node.expanded);

    node.toggle();
    REQUIRE_FALSE(node.expanded);
}

TEST_CASE("TreeView on_select callback", "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.expanded = true;
    synths.add_child("Bass");

    std::string selected_label;
    tree.on_select = [&](TreeNode& node) { selected_label = node.label; };

    // Simulate click on the first visible item
    tree.set_bounds({0, 0, 300, 400});
    tree.on_mouse_event(mouse_down(50, 5)); // should hit "Synths"

    REQUIRE(selected_label == "Synths");
}

TEST_CASE("TreeView ignores mouse up and misses", "[view][tree]") {
    TreeView tree;
    tree.root().add_child("Synths");
    tree.set_bounds({0, 0, 300, 400});

    int selected_count = 0;
    tree.on_select = [&](TreeNode&) { ++selected_count; };

    auto up = mouse_down(50, 5);
    up.is_down = false;
    tree.on_mouse_event(up);
    tree.on_mouse_event(mouse_down(50, 200));

    REQUIRE(selected_count == 0);
    REQUIRE(tree.selected_node() == nullptr);
}

TEST_CASE("TreeView skips selection callback for non-selectable rows",
          "[view][tree][coverage][phase3]") {
    TreeView tree;
    auto& header = tree.root().add_child("Header");
    header.selectable = false;
    auto& selectable = tree.root().add_child("Selectable");
    tree.set_bounds({0, 0, 300, 400});

    int selected_count = 0;
    tree.on_select = [&](TreeNode&) { ++selected_count; };

    tree.on_mouse_event(mouse_down(50, 5));
    REQUIRE(selected_count == 0);
    REQUIRE(tree.selected_node() == nullptr);

    tree.on_mouse_event(mouse_down(50, 28));
    REQUIRE(selected_count == 1);
    REQUIRE(tree.selected_node() == &selectable);
}

TEST_CASE("TreeView triangle click toggles without selecting", "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.add_child("Bass");
    tree.set_bounds({0, 0, 300, 400});

    std::string toggled_label;
    bool toggled_state = true;
    int select_count = 0;
    tree.on_toggle = [&](TreeNode& node, bool expanded) {
        toggled_label = node.label;
        toggled_state = expanded;
    };
    tree.on_select = [&](TreeNode&) { ++select_count; };

    tree.on_mouse_event(mouse_down(4, 5));

    REQUIRE(synths.expanded);
    REQUIRE(toggled_label == "Synths");
    REQUIRE(toggled_state);
    REQUIRE(select_count == 0);
    REQUIRE(tree.selected_node() == nullptr);

    tree.on_mouse_event(mouse_down(4, 5));

    REQUIRE_FALSE(synths.expanded);
    REQUIRE(toggled_label == "Synths");
    REQUIRE_FALSE(toggled_state);
    REQUIRE(select_count == 0);
    REQUIRE(tree.selected_node() == nullptr);
}

TEST_CASE("TreeView double click activates without selecting", "[view][tree]") {
    TreeView tree;
    tree.root().add_child("Synths");
    tree.set_bounds({0, 0, 300, 400});

    std::string activated_label;
    int select_count = 0;
    tree.on_activate = [&](TreeNode& node) { activated_label = node.label; };
    tree.on_select = [&](TreeNode&) { ++select_count; };

    tree.on_mouse_event(mouse_down(50, 5, 2));

    REQUIRE(activated_label == "Synths");
    REQUIRE(select_count == 0);
    REQUIRE(tree.selected_node() == nullptr);
}

TEST_CASE("TreeView key right expands node", "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.add_child("Bass");

    // Select synths first
    tree.set_bounds({0, 0, 300, 400});
    tree.on_mouse_event(mouse_down(50, 5));

    REQUIRE(tree.selected_node() != nullptr);
    REQUIRE_FALSE(tree.selected_node()->expanded);

    REQUIRE(tree.on_key_event(key_down(KeyCode::right)));
    REQUIRE(tree.selected_node()->expanded);
}

TEST_CASE("TreeView key left collapses expanded selection", "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.expanded = true;
    synths.add_child("Bass");
    tree.set_selected_node(&synths);

    bool collapsed = false;
    tree.on_toggle = [&](TreeNode& node, bool expanded) {
        collapsed = node.label == "Synths" && !expanded;
    };

    REQUIRE(tree.on_key_event(key_down(KeyCode::left)));
    REQUIRE_FALSE(synths.expanded);
    REQUIRE(collapsed);
}

TEST_CASE("TreeView key left consumes collapsed selected nodes without toggling",
          "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.add_child("Bass");
    tree.set_selected_node(&synths);

    int toggle_count = 0;
    tree.on_toggle = [&](TreeNode&, bool) { ++toggle_count; };

    REQUIRE(tree.on_key_event(key_down(KeyCode::left)));
    REQUIRE_FALSE(synths.expanded);
    REQUIRE(toggle_count == 0);
}

TEST_CASE("TreeView ignores unhandled key edges", "[view][tree]") {
    TreeView tree;
    auto& leaf = tree.root().add_child("Leaf");

    REQUIRE_FALSE(tree.on_key_event(key_down(KeyCode::right)));

    tree.set_selected_node(&leaf);
    REQUIRE_FALSE(tree.on_key_event(key_down(KeyCode::right)));

    auto left_up = key_down(KeyCode::left);
    left_up.is_down = false;
    REQUIRE_FALSE(tree.on_key_event(left_up));

    REQUIRE_FALSE(tree.on_key_event(key_down(KeyCode::enter)));
}

TEST_CASE("TreeView user data lookup finds nested nodes only for non-null data",
          "[view][tree]") {
    TreeView tree;
    int root_payload = 1;
    int nested_payload = 2;

    tree.root().user_data = &root_payload;
    auto& group = tree.root().add_child("Group");
    auto& nested = group.add_child("Nested");
    nested.user_data = &nested_payload;

    REQUIRE(tree.find_node_by_user_data(&nested_payload) == &nested);
    REQUIRE(tree.find_node_by_user_data(nullptr) == nullptr);
    REQUIRE(tree.find_node_by_user_data(&root_payload) == &tree.root());
}

TEST_CASE("TreeView paint covers selection highlight and disclosure states",
          "[view][tree]") {
    TreeView tree;
    auto& group = tree.root().add_child("Group");
    group.add_child("Nested");
    tree.set_selected_node(&group);
    tree.set_bounds({0, 0, 200, 100});

    RecordingCanvas collapsed;
    tree.paint(collapsed);
    REQUIRE(collapsed.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(has_fill_text(collapsed, "\xe2\x96\xb6"));
    REQUIRE(has_fill_text(collapsed, "Group"));
    REQUIRE_FALSE(has_fill_text(collapsed, "Nested"));

    group.expanded = true;
    RecordingCanvas expanded;
    tree.paint(expanded);
    REQUIRE(expanded.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(has_fill_text(expanded, "\xe2\x96\xbc"));
    REQUIRE(has_fill_text(expanded, "Nested"));
}

TEST_CASE("TreeView paint produces draw commands", "[view][tree]") {
    TreeView tree;
    tree.root().add_child("Item 1");
    tree.root().add_child("Item 2");
    tree.set_bounds({0, 0, 200, 100});

    RecordingCanvas canvas;
    tree.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 2);
}

TEST_CASE("TreeView row_height and indent configurable", "[view][tree]") {
    TreeView tree;
    tree.set_row_height(30.0f);
    tree.set_indent(24.0f);

    REQUIRE(tree.row_height() == 30.0f);
    REQUIRE(tree.indent() == 24.0f);
}

TEST_CASE("TreeView collapsed children are hidden from paint and hit testing",
          "[view][tree][coverage][issue-654]") {
    TreeView tree;
    auto& group = tree.root().add_child("Group");
    group.add_child("Hidden child");
    auto& visible = tree.root().add_child("Visible");
    tree.set_bounds({0, 0, 240, 120});

    std::string selected;
    tree.on_select = [&](TreeNode& node) { selected = node.label; };

    RecordingCanvas collapsed;
    tree.paint(collapsed);
    REQUIRE(has_text(collapsed, "Group"));
    REQUIRE_FALSE(has_text(collapsed, "Hidden child"));
    REQUIRE(has_text(collapsed, "Visible"));

    tree.on_mouse_event(mouse_down(40, 28));
    REQUIRE(selected == "Visible");
    REQUIRE(tree.selected_node() == &visible);
}

TEST_CASE("TreeView expanded child activation and selected paint paths",
          "[view][tree][coverage][issue-654]") {
    TreeView tree;
    tree.set_bounds({0, 0, 240, 160});
    tree.set_row_height(20.0f);
    tree.set_indent(24.0f);
    auto& group = tree.root().add_child("Group");
    auto& child = group.add_child("Child");
    group.expanded = true;

    std::string activated;
    tree.on_activate = [&](TreeNode& node) { activated = node.label; };

    tree.on_mouse_event(mouse_down(54, 25, 2));
    REQUIRE(activated == "Child");

    tree.set_selected_node(&child);
    RecordingCanvas selected;
    tree.paint(selected);
    REQUIRE(selected.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(text_count(selected, "\xe2\x96\xbc") == 1);
    REQUIRE(has_text(selected, "Child"));
}

TEST_CASE("TreeView left key on collapsed parent is handled without toggling",
          "[view][tree][coverage][issue-654]") {
    TreeView tree;
    auto& group = tree.root().add_child("Group");
    group.add_child("Child");
    tree.set_selected_node(&group);

    int toggles = 0;
    tree.on_toggle = [&](TreeNode&, bool) { ++toggles; };

    REQUIRE(tree.on_key_event(key_down(KeyCode::left)));
    REQUIRE_FALSE(group.expanded);
    REQUIRE(toggles == 0);
}
