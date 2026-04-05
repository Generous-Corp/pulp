#include <catch2/catch_test_macros.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;

TEST_CASE("TreeNode add children", "[view][tree]") {
    TreeNode root;
    auto& child1 = root.add_child("Synths");
    auto& child2 = root.add_child("Effects");

    REQUIRE(root.has_children());
    REQUIRE(root.children.size() == 2);
    REQUIRE(child1.label == "Synths");
    REQUIRE(child2.label == "Effects");
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
    MouseEvent e;
    e.position = {50, 5}; // should hit "Synths"
    e.is_down = true;
    e.click_count = 1;
    tree.on_mouse_event(e);

    REQUIRE(selected_label == "Synths");
}

TEST_CASE("TreeView key right expands node", "[view][tree]") {
    TreeView tree;
    auto& synths = tree.root().add_child("Synths");
    synths.add_child("Bass");

    // Select synths first
    tree.set_bounds({0, 0, 300, 400});
    MouseEvent click;
    click.position = {50, 5};
    click.is_down = true;
    click.click_count = 1;
    tree.on_mouse_event(click);

    REQUIRE(tree.selected_node() != nullptr);
    REQUIRE_FALSE(tree.selected_node()->expanded);

    KeyEvent right;
    right.key = KeyCode::right;
    right.is_down = true;
    REQUIRE(tree.on_key_event(right));
    REQUIRE(tree.selected_node()->expanded);
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
