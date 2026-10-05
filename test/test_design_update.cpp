#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_update.hpp>

using namespace pulp::view;
namespace {
IRNode node(const char* key) {
    IRNode out;
    out.type = "frame";
    out.stable_anchor_id = key;
    return out;
}

void require_recreated(const DesignChildUpdatePlan& plan, const char* key) {
    REQUIRE(plan.keyed);
    REQUIRE(plan.updates.size() == 2);
    CHECK(plan.updates[0].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[0].key == key);
    CHECK(plan.updates[1].kind == DesignUpdateKind::inserted);
    CHECK(plan.updates[1].key == key);
}
} // namespace

TEST_CASE("keyed design updates retain identity across reorder and batch blocks",
          "[view][import][update]") {
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

TEST_CASE("compatible keyed changes retain the existing materialization shape",
          "[view][import][update]") {
    auto old_node = node("control");
    auto new_node = old_node;
    old_node.attributes["pulpParamKey"] = "filter.cutoff";
    new_node.attributes["pulpParamKey"] = "filter.resonance";
    new_node.style.opacity = 0.75f;

    const auto plan = plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                std::span<const IRNode>(&new_node, 1));
    REQUIRE(plan.keyed);
    REQUIRE(plan.updates.size() == 1);
    CHECK(plan.updates.front().kind == DesignUpdateKind::retained);
    CHECK(plan.updates.front().key == "control");
}

TEST_CASE("value-only binding metadata does not change the materialization shape",
          "[view][import][update]") {
    auto old_node = node("editor");
    auto new_node = old_node;
    old_node.attributes["pulpInitialValue"] = "old text";
    old_node.attributes["pulpPlaceholder"] = "old placeholder";
    new_node.attributes["pulpInitialValue"] = "new text";
    new_node.attributes["pulpPlaceholder"] = "new placeholder";

    const auto plan = plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                std::span<const IRNode>(&new_node, 1));
    REQUIRE(plan.keyed);
    REQUIRE(plan.updates.size() == 1);
    CHECK(plan.updates.front().kind == DesignUpdateKind::retained);
    CHECK(plan.updates.front().key == "editor");
}

TEST_CASE("same-anchor shape changes recreate instead of reusing stale controls",
          "[view][import][update]") {
    SECTION("node type") {
        auto old_node = node("control");
        auto new_node = old_node;
        new_node.type = "button";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "control");
    }

    SECTION("render mode") {
        auto old_node = node("control");
        auto new_node = old_node;
        new_node.render_mode = NodeRenderMode::faithful_svg;
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "control");
    }

    SECTION("audio widget") {
        auto old_node = node("control");
        auto new_node = old_node;
        new_node.audio_widget = AudioWidgetType::knob;
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "control");
    }

    SECTION("binding topology") {
        auto old_node = node("control");
        auto new_node = old_node;
        old_node.attributes["pulpParamKey"] = "gain";
        new_node.attributes["pulpParamKeyX"] = "pan.x";
        new_node.attributes["pulpParamKeyY"] = "pan.y";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "control");
    }

    SECTION("interactive overlay kind") {
        auto old_node = node("control");
        auto new_node = old_node;
        old_node.interactive_elements.push_back({});
        new_node.interactive_elements.push_back({});
        new_node.interactive_elements.front().kind = InteractiveElementKind::fader;
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "control");
    }
}
