// StateTree transactions recorded as UndoManager steps.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/state_tree.hpp>
#include <pulp/state/undo_manager.hpp>

using namespace pulp::state;

namespace {

StateTree::Ptr make_tree() {
    auto root = StateTree::create("root");
    root->set("gain", 0.5);
    for (int i = 0; i < 4; ++i) {
        auto c = StateTree::create("voice");
        c->set("id", int64_t(i));
        root->add_child(c);
    }
    root->child(0)->add_child(StateTree::create("mod"));
    return root;
}

} // namespace

TEST_CASE("Transaction with an UndoManager becomes one named step", "[state][tree][undo]") {
    auto root = make_tree();
    auto before = root->deep_copy();
    UndoManager undo;
    {
        auto tx = root->begin_transaction(&undo, "Rearrange voices");
        root->set("gain", 0.8);
        root->remove("gain");
        root->set("mode", std::string("poly"));
        root->add_child(StateTree::create("voice"));
        root->remove_child(1);
        StateTree::move_child(*root, 0, *root, 2);
        StateTree::move_child(*root->child(2), 0, *root->child(0), 0); // across parents
        root->child(1)->remove_all_children();
    }
    auto after = root->deep_copy();
    REQUIRE(undo.undo_count() == 1);
    REQUIRE(undo.undo_name() == "Rearrange voices");

    for (int round = 0; round < 3; ++round) {
        REQUIRE(undo.undo());
        REQUIRE(root->is_equivalent_to(*before));
        REQUIRE(undo.redo());
        REQUIRE(root->is_equivalent_to(*after));
    }
}

TEST_CASE("Aborted and empty transactions push no undo step", "[state][tree][undo]") {
    auto root = make_tree();
    UndoManager undo;
    {
        auto tx = root->begin_transaction(&undo, "Aborted");
        root->set("gain", 0.1);
        tx.abort();
    }
    {
        auto tx = root->begin_transaction(&undo, "Empty");
    }
    REQUIRE(undo.undo_count() == 0);
}

TEST_CASE("Nested transactions record a single undo step", "[state][tree][undo]") {
    auto root = make_tree();
    auto before = root->deep_copy();
    UndoManager undo;
    {
        auto outer = root->begin_transaction(&undo, "Outer");
        root->set("a", int64_t(1));
        {
            auto inner = root->begin_transaction(&undo, "Inner");
            root->remove_child(0);
        }
        {
            auto inner = root->begin_transaction();
            root->set("discarded", true);
            inner.abort();
        }
    }
    REQUIRE(undo.undo_count() == 1);
    REQUIRE(undo.undo_name() == "Outer");
    undo.undo();
    REQUIRE(root->is_equivalent_to(*before));
}

TEST_CASE("Undo and redo deliver one coalesced burst", "[state][tree][undo]") {
    auto root = make_tree();
    UndoManager undo;
    int prop_calls = 0;
    int bursts_open = 0;
    root->add_listener([&](StateTree& node, auto&&...) {
        ++prop_calls;
        // A listener runs only after the replay has finished, so the whole
        // step is visible at once.
        if (node.get_double("gain") == 0.5 && !node.has("pan"))
            ++bursts_open;
    });
    {
        auto tx = root->begin_transaction(&undo, "Mix");
        root->set("gain", 0.1);
        root->set("gain", 0.2);
        root->set("pan", 0.3);
    }
    REQUIRE(prop_calls == 2);
    prop_calls = 0;
    undo.undo();
    REQUIRE(prop_calls == 2);
    REQUIRE(bursts_open == 2);
}

TEST_CASE("Undo restores children by identity after unrelated drift", "[state][tree][undo]") {
    auto root = make_tree();
    UndoManager undo;
    auto target = root->child(2);
    {
        auto tx = root->begin_transaction(&undo, "Remove");
        root->remove_child(2);
    }
    // Unrecorded edit shifts indices.
    root->insert_child(0, StateTree::create("intruder"));
    undo.undo();
    REQUIRE(target->parent() == root.get());
    REQUIRE(root->child_count() == 5);
    undo.redo();
    REQUIRE(target->parent() == nullptr);
}

TEST_CASE("Tree transactions compose inside an UndoManager transaction", "[state][tree][undo]") {
    auto a = make_tree();
    auto b = make_tree();
    auto a0 = a->deep_copy();
    auto b0 = b->deep_copy();
    UndoManager undo;
    undo.begin_transaction("Both trees");
    {
        auto tx = a->begin_transaction(&undo, "A");
        a->set("gain", 1.0);
    }
    {
        auto tx = b->begin_transaction(&undo, "B");
        b->remove_child(0);
    }
    undo.end_transaction();
    REQUIRE(undo.undo_count() == 1);
    undo.undo();
    REQUIRE(a->is_equivalent_to(*a0));
    REQUIRE(b->is_equivalent_to(*b0));
}

TEST_CASE("Undo of remove_all_children restores the original order", "[state][tree][undo]") {
    auto root = make_tree();
    auto before = root->deep_copy();
    UndoManager undo;
    {
        auto tx = root->begin_transaction(&undo, "Clear");
        root->remove_all_children();
    }
    REQUIRE(root->child_count() == 0);
    undo.undo();
    REQUIRE(root->is_equivalent_to(*before));
}
