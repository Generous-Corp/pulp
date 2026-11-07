// StateTree::Transaction — coalesced notifications, abort, nesting,
// reentrancy and the immediate-delivery observers built on it.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/state_tree.hpp>
#include <pulp/state/state_tree_sync.hpp>

#include <string>
#include <vector>

using namespace pulp::state;

namespace {

StateTree::Ptr make_tree() {
    auto root = StateTree::create("root");
    root->set("gain", 0.5);
    for (int i = 0; i < 3; ++i) {
        auto c = StateTree::create("voice");
        c->set("id", int64_t(i));
        root->add_child(c);
    }
    auto nested = StateTree::create("mod");
    nested->set("depth", 0.25);
    root->child(0)->add_child(nested);
    return root;
}

} // namespace

TEST_CASE("Transaction holds coalesced notifications until commit", "[state][tree][transaction]") {
    auto root = make_tree();
    std::vector<std::string> coalesced;
    std::vector<std::string> immediate;
    root->add_listener([&](StateTree&, std::string_view p, const PropertyValue&,
                           const PropertyValue&) { coalesced.emplace_back(p); });
    root->add_listener([&](StateTree&, std::string_view p, const PropertyValue&,
                           const PropertyValue&) { immediate.emplace_back(p); },
                       ListenerDelivery::kImmediate);

    {
        auto tx = root->begin_transaction();
        REQUIRE(tx.is_active());
        REQUIRE(root->in_transaction());
        root->set("gain", 0.6);
        root->set("pan", 0.0);
        REQUIRE(root->get_double("gain") == 0.6); // applied immediately
        REQUIRE(coalesced.empty());
        REQUIRE(immediate.size() == 2);
    }
    REQUIRE_FALSE(root->in_transaction());
    REQUIRE(coalesced == std::vector<std::string>{"gain", "pan"});
}

TEST_CASE("Transaction merges repeated property edits and drops net no-ops",
          "[state][tree][transaction]") {
    auto root = make_tree();
    struct Change {
        std::string name;
        PropertyValue old_v, new_v;
    };
    std::vector<Change> seen;
    root->add_listener([&](StateTree&, std::string_view p, const PropertyValue& o,
                           const PropertyValue& n) { seen.push_back({std::string(p), o, n}); });

    auto tx = root->begin_transaction();
    root->set("gain", 0.1);
    root->set("gain", 0.2);
    root->set("gain", 0.3);
    root->set("tmp", true);
    root->remove("tmp"); // never existed before: net nothing
    root->set("pan", 1.0);
    root->set("pan", 1.0);
    tx.commit();

    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0].name == "gain");
    REQUIRE(std::get<double>(seen[0].old_v) == 0.5);
    REQUIRE(std::get<double>(seen[0].new_v) == 0.3);
    REQUIRE(seen[1].name == "pan");
    REQUIRE(std::holds_alternative<std::monostate>(seen[1].old_v));
}

TEST_CASE("Transaction burst sees the final tree state", "[state][tree][transaction]") {
    auto root = make_tree();
    double seen_gain = 0.0;
    int seen_children = -1;
    root->add_child_added_listener([&](StateTree& parent, StateTree&, int) {
        seen_gain = parent.get_double("gain");
        seen_children = parent.child_count();
    });
    auto tx = root->begin_transaction();
    root->add_child(StateTree::create("voice"));
    root->set("gain", 0.9);
    root->add_child(StateTree::create("voice"));
    tx.commit();
    REQUIRE(seen_gain == 0.9);
    REQUIRE(seen_children == 5);
}

TEST_CASE("Transaction abort reverts every edit in reverse order", "[state][tree][transaction]") {
    auto root = make_tree();
    auto before = root->deep_copy();
    int coalesced_calls = 0;
    auto count = [&](auto&&...) { ++coalesced_calls; };
    root->add_listener(count);
    root->add_child_added_listener(count);
    root->add_child_removed_listener(count);
    root->add_child_moved_listener(count);

    auto tx = root->begin_transaction();
    root->set("gain", 0.0);
    root->remove("gain");
    root->set("new_prop", std::string("x"));
    auto extra = StateTree::create("voice");
    root->insert_child(1, extra);
    extra->set("id", int64_t(99));
    auto removed = root->child(0);
    root->remove_child(0);
    removed->set("id", int64_t(-1));      // edit on a detached node
    removed->child(0)->set("depth", 1.0); // and deep inside it
    REQUIRE(StateTree::move_child(*root, 2, *root, 0));
    root->child(1)->add_child(StateTree::create("lfo"));
    root->remove_all_children();
    REQUIRE(root->child_count() == 0);
    REQUIRE(tx.abort());

    REQUIRE_FALSE(tx.is_active());
    REQUIRE(root->is_equivalent_to(*before));
    REQUIRE(removed->parent() == root.get());
    REQUIRE(root->child(0) == removed);
    REQUIRE(extra->parent() == nullptr);
    REQUIRE(coalesced_calls == 0);
    REQUIRE_FALSE(root->in_transaction());
    REQUIRE_FALSE(removed->in_transaction());
}

TEST_CASE("Immediate listeners see the reverting edits of an abort", "[state][tree][transaction]") {
    auto root = make_tree();
    std::vector<std::pair<PropertyValue, PropertyValue>> seen;
    root->add_listener([&](StateTree&, std::string_view, const PropertyValue& o,
                           const PropertyValue& n) { seen.emplace_back(o, n); },
                       ListenerDelivery::kImmediate);
    auto tx = root->begin_transaction();
    root->set("gain", 0.75);
    tx.abort();
    REQUIRE(seen.size() == 2);
    REQUIRE(std::get<double>(seen[1].first) == 0.75);
    REQUIRE(std::get<double>(seen[1].second) == 0.5);
}

TEST_CASE("Nested transactions join the outer one", "[state][tree][transaction]") {
    auto root = make_tree();
    int calls = 0;
    root->add_listener([&](auto&&...) { ++calls; });

    auto outer = root->begin_transaction();
    root->set("a", int64_t(1));
    {
        // Begun through a child: still the same tree-wide transaction.
        auto inner = root->child(1)->begin_transaction();
        root->set("b", int64_t(2));
        inner.commit();
        REQUIRE(calls == 0);
        REQUIRE(root->in_transaction());
    }
    {
        auto inner = root->begin_transaction();
        root->set("c", int64_t(3));
        REQUIRE(inner.abort()); // reverts only "c"
    }
    REQUIRE(calls == 0);
    outer.commit();
    REQUIRE(calls == 2);
    REQUIRE(root->has("a"));
    REQUIRE(root->has("b"));
    REQUIRE_FALSE(root->has("c"));
}

TEST_CASE("Outer abort reverts edits committed by a nested transaction",
          "[state][tree][transaction]") {
    auto root = make_tree();
    auto before = root->deep_copy();
    auto outer = root->begin_transaction();
    {
        auto inner = root->begin_transaction();
        root->remove_child(1);
        root->set("gain", 1.0);
    }
    outer.abort();
    REQUIRE(root->is_equivalent_to(*before));
}

TEST_CASE("Listeners may not begin a transaction during the commit burst",
          "[state][tree][transaction]") {
    auto root = make_tree();
    bool inner_active = true;
    int inner_calls = 0;
    root->add_listener(
        [&](StateTree& node, std::string_view p, const PropertyValue&, const PropertyValue&) {
            ++inner_calls;
            if (p != "trigger")
                return;
            auto tx = node.begin_transaction();
            inner_active = tx.is_active();
            node.set("echo", true); // direct edit: notifies inline, not grouped
            REQUIRE_FALSE(tx.abort());
        });
    {
        auto tx = root->begin_transaction();
        root->set("trigger", true);
    }
    REQUIRE_FALSE(inner_active);
    REQUIRE(root->get_bool("echo"));
    REQUIRE(inner_calls == 2);
    // After the burst a new transaction works normally.
    auto tx = root->begin_transaction();
    REQUIRE(tx.is_active());
}

TEST_CASE("Moved-from transactions are inert and move-assign commits",
          "[state][tree][transaction]") {
    auto root = make_tree();
    int calls = 0;
    root->add_listener([&](auto&&...) { ++calls; });
    auto a = root->begin_transaction();
    root->set("x", int64_t(1));
    StateTree::Transaction b(std::move(a));
    REQUIRE_FALSE(a.is_active());
    REQUIRE(b.is_active());
    REQUIRE_FALSE(a.abort());

    auto other = StateTree::create("other");
    StateTree::Transaction c = other->begin_transaction();
    c = std::move(b); // commits other's empty transaction, takes root's
    REQUIRE(calls == 0);
    c.commit();
    REQUIRE(calls == 1);
}

TEST_CASE("SyncedClone mirrors edits and reverts inside a transaction",
          "[state][tree][transaction]") {
    auto root = make_tree();
    auto synced = root->clone_synced();
    {
        auto tx = root->begin_transaction();
        root->set("gain", 0.1);
        root->add_child(StateTree::create("voice"));
        root->child(3)->set("id", int64_t(3));
        StateTree::move_child(*root, 3, *root, 0);
        root->remove_child(2);
    }
    REQUIRE(synced.clone()->is_equivalent_to(*root));

    auto tx = root->begin_transaction();
    root->set("gain", 0.9);
    StateTree::move_child(*root, 0, *root, 2);
    root->remove_child(1);
    tx.abort();
    REQUIRE(synced.clone()->is_equivalent_to(*root));
}

TEST_CASE("Synchroniser deltas replay transactions and aborts in order",
          "[state][tree][transaction]") {
    auto root = make_tree();
    auto remote = root->deep_copy();
    StateTreeSynchroniser sync;
    sync.attach(root);
    {
        auto tx = root->begin_transaction();
        root->set("gain", 0.2);
        StateTree::move_child(*root, 0, *root, 2);
        root->child(1)->set("id", int64_t(42));
    }
    {
        auto tx = root->begin_transaction();
        root->set("gain", 0.4);
        StateTree::move_child(*root, 2, *root, 1);
        tx.abort();
    }
    auto bytes = StateTreeSynchroniser::encode(sync.take_deltas());
    StateTreeSynchroniser::apply(*remote,
                                 StateTreeSynchroniser::decode(bytes.data(), bytes.size()));
    REQUIRE(remote->is_equivalent_to(*root));
}
