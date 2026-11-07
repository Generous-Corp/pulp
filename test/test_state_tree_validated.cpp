// Checked StateTree edits and ValidatedTransaction: refused edits return a
// TreeEditError value and leave the tree unchanged.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/state_tree.hpp>
#include <pulp/state/undo_manager.hpp>

using namespace pulp::state;

namespace {

PropertyValidator gain_range() {
    return [](const StateTree&, std::string_view name,
              const PropertyValue& value) -> std::optional<std::string> {
        if (name != "gain")
            return std::nullopt;
        auto* d = std::get_if<double>(&value);
        if (!d || *d < 0.0 || *d > 1.0)
            return std::string("gain must be a double in [0, 1]");
        return std::nullopt;
    };
}

} // namespace

TEST_CASE("set_checked refuses invalid edits without notifying", "[state][tree][validated]") {
    auto root = StateTree::create("root");
    int calls = 0;
    root->add_listener([&](auto&&...) { ++calls; });

    auto empty = root->set_checked("", int64_t(1));
    REQUIRE(empty.is_err());
    REQUIRE(empty.error().code == TreeEditErrorCode::kInvalidName);

    auto rejected = root->set_checked("gain", 2.0, gain_range());
    REQUIRE(rejected.is_err());
    REQUIRE(rejected.error().code == TreeEditErrorCode::kRejectedByValidator);
    REQUIRE(rejected.error().message == "gain must be a double in [0, 1]");
    REQUIRE_FALSE(root->has("gain"));
    REQUIRE(calls == 0);

    REQUIRE(root->set_checked("gain", 0.5, gain_range()).has_value());
    REQUIRE(calls == 1);
    REQUIRE(root->remove_checked("missing").error().code == TreeEditErrorCode::kMissingProperty);
}

TEST_CASE("Checked child edits report the reason for a refusal", "[state][tree][validated]") {
    auto root = StateTree::create("root");
    auto child = StateTree::create("c");
    REQUIRE(root->add_child_checked(nullptr).error().code == TreeEditErrorCode::kNullChild);
    REQUIRE(root->add_child_checked(root).error().code == TreeEditErrorCode::kWouldCreateCycle);
    REQUIRE(root->insert_child_checked(1, child).error().code ==
            TreeEditErrorCode::kIndexOutOfRange);
    REQUIRE(root->insert_child_checked(0, child).has_value());
    REQUIRE(child->add_child_checked(root).error().code == TreeEditErrorCode::kWouldCreateCycle);
    REQUIRE(root->remove_child_checked(3).error().code == TreeEditErrorCode::kIndexOutOfRange);
    REQUIRE(root->remove_child_checked(0).has_value());

    // The unchecked insert keeps clamping out-of-range indices.
    root->insert_child(42, child);
    REQUIRE(root->child(0) == child);
}

TEST_CASE("ValidatedTransaction aborts everything when any edit failed",
          "[state][tree][validated]") {
    auto root = StateTree::create("root");
    root->set("gain", 0.25);
    root->add_child(StateTree::create("a"));
    auto before = root->deep_copy();
    UndoManager undo;
    int calls = 0;
    root->add_listener([&](auto&&...) { ++calls; });

    auto tx = root->begin_validated_transaction(gain_range(), &undo, "Script edit");
    REQUIRE(tx.set(*root, "gain", 0.75).has_value());
    REQUIRE(tx.add_child(*root, StateTree::create("b")).has_value());
    auto bad = tx.set(*root, "gain", 4.0);
    REQUIRE(bad.error().code == TreeEditErrorCode::kRejectedByValidator);
    REQUIRE(tx.remove_child(*root, 9).is_err());
    REQUIRE(tx.has_error());
    REQUIRE(root->get_double("gain") == 0.75); // good edits applied so far

    auto result = tx.commit();
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == TreeEditErrorCode::kRejectedByValidator); // first error
    REQUIRE(root->is_equivalent_to(*before));
    REQUIRE(calls == 0);
    REQUIRE(undo.undo_count() == 0);
    REQUIRE(tx.commit().error().code == TreeEditErrorCode::kTransactionClosed);
}

TEST_CASE("A clean ValidatedTransaction commits as one undo step", "[state][tree][validated]") {
    auto root = StateTree::create("root");
    auto before = root->deep_copy();
    UndoManager undo;
    {
        auto tx = root->begin_validated_transaction(gain_range(), &undo, "Script edit");
        REQUIRE(tx.set(*root, "gain", 0.3).has_value());
        REQUIRE(tx.add_child(*root, StateTree::create("a")).has_value());
        REQUIRE(tx.add_child(*root, StateTree::create("b")).has_value());
        REQUIRE(tx.move_child(*root, 1, *root, 0).has_value());
        REQUIRE(tx.remove(*root, "gain").has_value());
    } // destructor commits
    REQUIRE(undo.undo_count() == 1);
    REQUIRE(root->child(0)->type_name() == "b");
    undo.undo();
    REQUIRE(root->is_equivalent_to(*before));
}

TEST_CASE("ValidatedTransaction refuses nodes outside its tree and cross-tree conflicts",
          "[state][tree][validated]") {
    auto root = StateTree::create("root");
    root->add_child(StateTree::create("a"));
    auto other = StateTree::create("other");
    auto tx = root->begin_validated_transaction();
    REQUIRE(tx.set(*other, "x", true).error().code == TreeEditErrorCode::kNotInTransaction);
    REQUIRE(tx.set(*root->child(0), "x", true).has_value());

    auto other_tx = other->begin_transaction();
    REQUIRE(StateTree::move_child_checked(*root, 0, *other, 0).error().code ==
            TreeEditErrorCode::kTransactionConflict);
    REQUIRE(other->add_child_checked(root->child(0)).error().code ==
            TreeEditErrorCode::kTransactionConflict);
    REQUIRE(root->child_count() == 1);
    other_tx.commit();
    tx.abort();
    REQUIRE_FALSE(root->child(0)->has("x"));
}

TEST_CASE("ValidatedTransaction begun from a commit burst refuses edits",
          "[state][tree][validated]") {
    auto root = StateTree::create("root");
    std::optional<TreeEditErrorCode> code;
    root->add_listener([&](StateTree& n, std::string_view p, auto&&...) {
        if (p != "go")
            return;
        auto tx = n.begin_validated_transaction();
        code = tx.set(n, "echo", true).error().code;
    });
    {
        auto tx = root->begin_transaction();
        root->set("go", true);
    }
    REQUIRE(code == TreeEditErrorCode::kReentrantTransaction);
    REQUIRE_FALSE(root->has("echo"));
}
