#include <pulp/view/design_update.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pulp::view;
namespace { IRNode node(const char* key) { IRNode out; out.type = "frame"; out.stable_anchor_id = key; return out; } }

TEST_CASE("keyed design updates retain identity across reorder and batch blocks", "[view][import][update]") {
    const std::vector<IRNode> old_children{node("a"), node("b"), node("c")};
    const std::vector<IRNode> new_children{node("c"), node("a"), node("d")};
    const auto plan = plan_design_child_updates(old_children, new_children);
    REQUIRE(plan.keyed);
    REQUIRE_FALSE(plan.ambiguous_keys);
    REQUIRE(plan.updates.size() == 4);
    CHECK(plan.updates[0].kind == DesignUpdateKind::moved);
    CHECK(plan.updates[0].key == "c");
    CHECK(plan.updates[0].old_index == 2);
    CHECK(plan.updates[0].new_index == 0);
    CHECK(plan.updates[1].kind == DesignUpdateKind::moved);
    CHECK(plan.updates[1].key == "a");
    CHECK(plan.updates[2].kind == DesignUpdateKind::inserted);
    CHECK(plan.updates[2].key == "d");
    CHECK(plan.updates[3].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[3].key == "b");
    REQUIRE(plan.blocks.size() == 3);
    CHECK(plan.blocks[0].kind == DesignUpdateKind::moved);
    CHECK(plan.blocks[0].count == 2);
}

TEST_CASE("ambiguous anchors fail closed to positional planning", "[view][import][update]") {
    const std::vector<IRNode> old_children{node("duplicate"), node("duplicate")};
    const std::vector<IRNode> new_children{node("duplicate")};
    const auto plan = plan_design_child_updates(old_children, new_children);
    CHECK_FALSE(plan.keyed);
    CHECK(plan.ambiguous_keys);
    REQUIRE(plan.updates.size() == 3);
    CHECK(plan.updates[0].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[1].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[2].kind == DesignUpdateKind::inserted);
}
