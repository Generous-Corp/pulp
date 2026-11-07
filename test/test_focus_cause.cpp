// test_focus_cause.cpp — why focus moved, and who is told.
//
// Every focus change carries a cause (pointer, keyboard or programmatic).
// :focus-visible is true only for the last two. Every ancestor of the view that
// gained or lost focus is told through on_descendant_focus_changed, which is
// what :focus-within is built on.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

struct Record {
    std::string who;
    View* child;
    bool gained;
    FocusCause cause;
};

class Node : public View {
public:
    explicit Node(std::string n, std::vector<Record>* log = nullptr)
        : name(std::move(n)), log_(log) {}
    void on_descendant_focus_changed(View& child, bool gained,
                                     FocusCause cause) override {
        if (log_) log_->push_back({name, &child, gained, cause});
    }
    std::string name;

private:
    std::vector<Record>* log_;
};

// A widget written before causes existed: it overrides only the one-argument
// hook.
class LegacyField : public View {
public:
    void on_focus_changed(bool gained) override {
        View::on_focus_changed(gained);
        seen.push_back(gained);
    }
    std::vector<bool> seen;
};

struct Tree {
    std::vector<Record> log;
    View root;
    Node* group = nullptr;
    Node* row = nullptr;
    View* first = nullptr;
    View* second = nullptr;
    LegacyField* legacy = nullptr;

    Tree() {
        root.set_bounds({0, 0, 300, 100});
        auto g = std::make_unique<Node>("group", &log);
        group = g.get();
        auto r = std::make_unique<Node>("row", &log);
        row = r.get();
        auto a = std::make_unique<View>();
        auto b = std::make_unique<View>();
        auto l = std::make_unique<LegacyField>();
        first = a.get();
        second = b.get();
        legacy = l.get();
        for (View* v : {first, second, static_cast<View*>(legacy)}) v->set_focusable(true);
        row->add_child(std::move(a));
        row->add_child(std::move(b));
        group->add_child(std::move(r));
        group->add_child(std::move(l));
        root.add_child(std::move(g));
    }
};

}  // namespace

TEST_CASE("focus carries the cause that moved it", "[view][focus]") {
    Tree t;

    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::pointer));
    CHECK(t.first->has_focus());
    CHECK(t.first->focus_cause() == FocusCause::pointer);
    CHECK_FALSE(t.first->focus_visible());

    REQUIRE(transfer_input_focus(t.root, t.second, FocusCause::keyboard));
    CHECK(t.second->focus_cause() == FocusCause::keyboard);
    CHECK(t.second->focus_visible());
    CHECK_FALSE(t.first->has_focus());
    CHECK_FALSE(t.first->focus_visible());

    REQUIRE(transfer_input_focus(t.root, t.first));  // no cause given
    CHECK(t.first->focus_cause() == FocusCause::programmatic);
    CHECK(t.first->focus_visible());
}

TEST_CASE("a widget that overrides only the one-argument hook still hears focus",
          "[view][focus]") {
    Tree t;
    REQUIRE(transfer_input_focus(t.root, t.legacy, FocusCause::keyboard));
    REQUIRE(t.legacy->seen.size() == 1);
    CHECK(t.legacy->seen[0]);
    CHECK(t.legacy->has_focus());

    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::keyboard));
    REQUIRE(t.legacy->seen.size() == 2);
    CHECK_FALSE(t.legacy->seen[1]);
}

TEST_CASE("every ancestor is told, nearest first", "[view][focus]") {
    Tree t;
    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::keyboard));

    REQUIRE(t.log.size() == 2);
    CHECK(t.log[0].who == "row");
    CHECK(t.log[1].who == "group");
    for (const auto& r : t.log) {
        CHECK(r.child == t.first);
        CHECK(r.gained);
        CHECK(r.cause == FocusCause::keyboard);
    }
}

TEST_CASE("moving focus between siblings reports the loss then the gain",
          "[view][focus]") {
    Tree t;
    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::pointer));
    t.log.clear();

    REQUIRE(transfer_input_focus(t.root, t.second, FocusCause::keyboard));
    REQUIRE(t.log.size() == 4);
    CHECK(t.log[0].child == t.first);
    CHECK_FALSE(t.log[0].gained);
    CHECK(t.log[1].child == t.first);
    CHECK_FALSE(t.log[1].gained);
    CHECK(t.log[2].child == t.second);
    CHECK(t.log[2].gained);
    CHECK(t.log[3].child == t.second);
    CHECK(t.log[3].gained);
    CHECK(t.log[3].cause == FocusCause::keyboard);
}

TEST_CASE("focus_within follows focus up the tree", "[view][focus]") {
    Tree t;
    CHECK_FALSE(t.group->focus_within());

    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::keyboard));
    CHECK(t.first->focus_within());
    CHECK(t.row->focus_within());
    CHECK(t.group->focus_within());
    CHECK_FALSE(t.second->focus_within());

    REQUIRE(transfer_input_focus(t.root, t.legacy, FocusCause::keyboard));
    CHECK_FALSE(t.row->focus_within());
    CHECK(t.group->focus_within());

    transfer_input_focus(t.root, nullptr);
    CHECK_FALSE(t.group->focus_within());
}

TEST_CASE("blurring reports the loss to ancestors", "[view][focus]") {
    Tree t;
    REQUIRE(transfer_input_focus(t.root, t.first, FocusCause::keyboard));
    t.log.clear();

    transfer_input_focus(t.root, nullptr, FocusCause::pointer);
    REQUIRE(t.log.size() == 2);
    CHECK_FALSE(t.log[0].gained);
    CHECK(t.log[0].cause == FocusCause::pointer);
}

TEST_CASE("a disabled view cannot take focus", "[view][focus][disabled]") {
    Tree t;
    t.row->set_enabled(false);
    CHECK_FALSE(t.first->effectively_enabled());
    transfer_input_focus(t.root, t.first, FocusCause::keyboard);
    CHECK_FALSE(t.first->has_focus());
    CHECK(t.log.empty());
}

TEST_CASE("Tab traversal skips disabled subtrees", "[view][focus][disabled]") {
    Tree t;
    t.row->set_enabled(false);
    View* next = View::focus_next(t.root, nullptr);
    CHECK(next == t.legacy);
}
