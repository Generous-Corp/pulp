// StateTree::move_child, the structural listener contract, descendant
// search and structural equivalence.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/state_tree.hpp>

#include <string>
#include <vector>

using namespace pulp::state;

namespace {

StateTree::Ptr node(const char* type, int64_t id) {
    auto n = StateTree::create(type);
    n->set("id", id);
    return n;
}

std::vector<int64_t> ids(const StateTree& parent) {
    std::vector<int64_t> out;
    for (int i = 0; i < parent.child_count(); ++i)
        out.push_back(parent.child(i)->get_int("id"));
    return out;
}

} // namespace

TEST_CASE("move_child reorders within a parent and notifies child_moved", "[state][tree][move]") {
    auto root = StateTree::create("root");
    for (int i = 0; i < 4; ++i)
        root->add_child(node("n", i));
    int moved_from = -1, moved_to = -1, other_calls = 0;
    root->add_child_moved_listener([&](StateTree&, StateTree& child, int from, int to) {
        moved_from = from;
        moved_to = to;
        REQUIRE(child.get_int("id") == 0);
    });
    root->add_child_added_listener([&](auto&&...) { ++other_calls; });
    root->add_child_removed_listener([&](auto&&...) { ++other_calls; });

    REQUIRE(StateTree::move_child(*root, 0, *root, 3));
    REQUIRE(ids(*root) == std::vector<int64_t>{1, 2, 3, 0});
    REQUIRE(moved_from == 0);
    REQUIRE(moved_to == 3);
    REQUIRE(other_calls == 0);
    REQUIRE(StateTree::move_child(*root, 1, *root, 1)); // no-op
}

TEST_CASE("move_child across parents notifies in contract order with final state",
          "[state][tree][move]") {
    auto root = StateTree::create("root");
    auto a = node("a", 0);
    auto b = node("b", 1);
    root->add_child(a);
    root->add_child(b);
    auto moving = node("x", 7);
    auto grandchild = node("g", 8);
    moving->add_child(grandchild);
    a->add_child(moving);

    std::vector<std::string> order;
    a->add_child_removed_listener([&](StateTree&, StateTree& child, int index) {
        order.push_back("removed");
        REQUIRE(index == 0);
        REQUIRE(child.parent() == b.get()); // observes the final state
    });
    b->add_child_added_listener([&](StateTree&, StateTree&, int index) {
        order.push_back("added");
        REQUIRE(index == 0);
    });
    moving->add_parent_changed_listener([&](StateTree& n, StateTree* old_parent) {
        order.push_back("parent");
        REQUIRE(old_parent == a.get());
        REQUIRE(n.parent() == b.get());
    });
    moving->add_ancestor_changed_listener([&](StateTree&) { order.push_back("ancestor:x"); });
    grandchild->add_ancestor_changed_listener([&](StateTree&) { order.push_back("ancestor:g"); });

    REQUIRE(StateTree::move_child(*a, 0, *b, 0));
    REQUIRE(order ==
            std::vector<std::string>{"removed", "added", "parent", "ancestor:x", "ancestor:g"});
}

TEST_CASE("add_child of an attached node notifies removal with the new parent",
          "[state][tree][move]") {
    auto p1 = StateTree::create("p1");
    auto p2 = StateTree::create("p2");
    auto c = node("c", 1);
    p1->add_child(c);
    StateTree* seen_parent = nullptr;
    p1->add_child_removed_listener(
        [&](StateTree&, StateTree& child, int) { seen_parent = child.parent(); });
    p2->add_child(c);
    REQUIRE(seen_parent == p2.get());
}

TEST_CASE("Invalid moves are refused and change nothing", "[state][tree][move]") {
    auto root = StateTree::create("root");
    auto a = node("a", 0);
    auto deep = node("d", 1);
    a->add_child(deep);
    root->add_child(a);
    int calls = 0;
    root->add_child_removed_listener([&](auto&&...) { ++calls; });

    REQUIRE_FALSE(StateTree::move_child(*root, 5, *root, 0));
    REQUIRE_FALSE(StateTree::move_child(*root, 0, *root, 1)); // only one child
    REQUIRE_FALSE(StateTree::move_child(*root, 0, *deep, 0)); // into own descendant
    REQUIRE_FALSE(StateTree::move_child(*root, 0, *a, 0));    // into itself
    REQUIRE(StateTree::move_child_checked(*root, 0, *deep, 0).error().code ==
            TreeEditErrorCode::kWouldCreateCycle);
    REQUIRE(calls == 0);
    REQUIRE(a->parent() == root.get());
}

TEST_CASE("remove_all_children notifies from the last child backwards", "[state][tree][move]") {
    auto root = StateTree::create("root");
    std::vector<StateTree::Ptr> kids;
    for (int i = 0; i < 3; ++i) {
        kids.push_back(node("n", i));
        root->add_child(kids.back());
    }
    std::vector<std::pair<int64_t, int>> seen;
    root->add_child_removed_listener([&](StateTree& parent, StateTree& child, int index) {
        REQUIRE(parent.child_count() == 0);
        REQUIRE(child.parent() == nullptr);
        seen.emplace_back(child.get_int("id"), index);
    });
    root->remove_all_children();
    REQUIRE(seen == std::vector<std::pair<int64_t, int>>{{2, 2}, {1, 1}, {0, 0}});
}

TEST_CASE("for_each_descendant and find_descendant walk depth-first", "[state][tree][move]") {
    auto root = StateTree::create("root");
    auto a = node("a", 1);
    auto b = node("b", 2);
    a->add_child(node("a1", 3));
    a->add_child(node("a2", 4));
    b->add_child(node("b1", 5));
    root->add_child(a);
    root->add_child(b);

    std::vector<int64_t> visited;
    root->for_each_descendant([&](StateTree& n) { visited.push_back(n.get_int("id")); });
    REQUIRE(visited == std::vector<int64_t>{1, 3, 4, 2, 5});

    auto found = root->find_descendant([](const StateTree& n) { return n.get_int("id") > 3; });
    REQUIRE(found);
    REQUIRE(found->type_name() == "a2");
    REQUIRE_FALSE(
        root->find_descendant([](const StateTree& n) { return n.type_name() == "root"; }));

    // Editing during the walk is safe: children removed from the node being
    // visited are skipped, the rest of the snapshot is still walked.
    int count = 0;
    root->for_each_descendant([&](StateTree& n) {
        ++count;
        if (n.type_name() == "a")
            n.remove_all_children();
    });
    REQUIRE(count == 3);
}

TEST_CASE("is_equivalent_to compares type, properties and ordered children",
          "[state][tree][move]") {
    auto a = StateTree::create("t");
    a->set("x", int64_t(3));
    a->add_child(node("c", 1));
    auto b = a->deep_copy();
    REQUIRE(a->is_equivalent_to(*b));
    REQUIRE(a->is_equivalent_to(*a));

    b->set("x", 3.0); // different variant alternative
    REQUIRE_FALSE(a->is_equivalent_to(*b));
    b->set("x", int64_t(3));
    b->add_child(node("c", 2));
    REQUIRE_FALSE(a->is_equivalent_to(*b));
    b->remove_child(1);
    b->child(0)->set("id", int64_t(9));
    REQUIRE_FALSE(a->is_equivalent_to(*b));
    REQUIRE_FALSE(a->is_equivalent_to(*StateTree::create("other")));
}
