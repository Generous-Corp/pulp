#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_update.hpp>
#include <utility>

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

IRInteractiveElement materialized_element() {
    IRInteractiveElement element;
    element.kind = InteractiveElementKind::dropdown;
    element.cx = 11.0f;
    element.cy = 12.0f;
    element.hit_radius = 13.0f;
    element.svg_patch_d = "M0 0L1 1";
    element.default_value = 0.25f;
    element.flash = true;
    element.x = 14.0f;
    element.y = 15.0f;
    element.w = 16.0f;
    element.h = 17.0f;
    element.options = {"one", "two"};
    element.selected_index = 1;
    element.placeholder = "placeholder";
    element.bg_color = "#123456";
    element.target_frame = 2;
    element.action = "octave_up";
    element.text = "value";
    element.value_left_align = true;
    element.default_value_y = 0.75f;
    element.factory_id = "factory";
    element.custom_props = R"({"mode":"compact"})";
    element.source_node_id = "source:1";
    element.param_key = "old.key";
    return element;
}

template <typename Mutator> void require_interactive_recreated(const char* key, Mutator mutate) {
    auto old_node = node(key);
    auto new_node = old_node;
    old_node.interactive_elements.push_back(materialized_element());
    new_node.interactive_elements.push_back(materialized_element());
    mutate(new_node.interactive_elements.front());
    require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                std::span<const IRNode>(&new_node, 1)),
                      key);
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
    CHECK(plan.updates[0].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[0].key == "b");
    CHECK(plan.updates[0].old_index == 1);
    CHECK(plan.updates[1].kind == DesignUpdateKind::moved);
    CHECK(plan.updates[1].key == "c");
    CHECK(plan.updates[1].old_index == 1);
    CHECK(plan.updates[1].new_index == 0);
    CHECK(plan.updates[2].kind == DesignUpdateKind::retained);
    CHECK(plan.updates[2].key == "a");
    CHECK(plan.updates[3].kind == DesignUpdateKind::inserted);
    CHECK(plan.updates[3].key == "d");
    REQUIRE(plan.blocks.size() == 4);
    CHECK(plan.blocks[0].kind == DesignUpdateKind::removed);
}

TEST_CASE("ambiguous anchors fail closed to positional planning", "[view][import][update]") {
    const std::vector<IRNode> old_children{node("duplicate"), node("duplicate")};
    const std::vector<IRNode> new_children{node("duplicate")};
    const auto plan = plan_design_child_updates(old_children, new_children);
    CHECK_FALSE(plan.keyed);
    CHECK(plan.ambiguous_keys);
    REQUIRE(plan.updates.size() == 3);
    CHECK(plan.updates[0].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[0].old_index == 1);
    CHECK(plan.updates[1].kind == DesignUpdateKind::removed);
    CHECK(plan.updates[1].old_index == 0);
    CHECK(plan.updates[2].kind == DesignUpdateKind::inserted);
}

TEST_CASE("keyed update indices remain safe for direct application", "[view][import][update]") {
    const auto apply = [](const std::vector<IRNode>& old_children,
                          const std::vector<IRNode>& new_children) {
        const auto plan = plan_design_child_updates(old_children, new_children);
        std::vector<std::string> working;
        for (const auto& child : old_children)
            working.push_back(*child.stable_anchor_id);
        for (const auto& update : plan.updates) {
            if (update.kind == DesignUpdateKind::removed) {
                const auto it = std::find(working.begin(), working.end(), update.key);
                REQUIRE(it != working.end());
                working.erase(it);
            } else if (update.kind == DesignUpdateKind::inserted) {
                REQUIRE(update.new_index <= working.size());
                working.insert(working.begin() + static_cast<std::ptrdiff_t>(update.new_index),
                               update.key);
            } else if (update.kind == DesignUpdateKind::moved) {
                REQUIRE(update.old_index < working.size());
                REQUIRE(working[update.old_index] == update.key);
                auto value = working[update.old_index];
                working.erase(working.begin() + static_cast<std::ptrdiff_t>(update.old_index));
                REQUIRE(update.new_index <= working.size());
                working.insert(working.begin() + static_cast<std::ptrdiff_t>(update.new_index),
                               std::move(value));
            }
        }
        std::vector<std::string> expected;
        for (const auto& child : new_children)
            expected.push_back(*child.stable_anchor_id);
        REQUIRE(working == expected);
    };

    SECTION("move then remove") {
        apply({node("a"), node("b")}, {node("b")});
    }
    SECTION("replace same slot") {
        apply({node("a")}, {node("b")});
    }
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

TEST_CASE("materialization identity changes recreate keyed nodes", "[view][import][update]") {
    SECTION("unchanged interactive materialization is retained") {
        auto old_node = node("interactive-stable");
        auto new_node = old_node;
        old_node.interactive_elements.push_back(materialized_element());
        new_node.interactive_elements.push_back(materialized_element());
        // Parameter names are the one mutable identity channel. A host can
        // re-key an existing DesignFrameElement without rebuilding it.
        new_node.interactive_elements.front().param_key = "new.key";
        const auto plan = plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1));
        REQUIRE(plan.keyed);
        REQUIRE(plan.updates.size() == 1);
        CHECK(plan.updates.front().kind == DesignUpdateKind::retained);
    }

    SECTION("the same SVG asset is retained") {
        auto old_node = node("svg");
        auto new_node = old_node;
        old_node.render_mode = NodeRenderMode::faithful_svg;
        old_node.svg_asset_id = "panel-v1";
        new_node.render_mode = NodeRenderMode::faithful_svg;
        new_node.svg_asset_id = "panel-v1";
        const auto plan = plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1));
        REQUIRE(plan.keyed);
        REQUIRE(plan.updates.size() == 1);
        CHECK(plan.updates.front().kind == DesignUpdateKind::retained);
    }

    SECTION("the same custom materialization is retained") {
        auto old_node = node("custom-stable");
        auto new_node = old_node;
        IRInteractiveElement element;
        element.kind = InteractiveElementKind::custom;
        element.factory_id = "factory";
        element.custom_props = R"({"mode":"compact"})";
        old_node.interactive_elements.push_back(element);
        new_node.interactive_elements.push_back(element);
        const auto plan = plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1));
        REQUIRE(plan.keyed);
        REQUIRE(plan.updates.size() == 1);
        CHECK(plan.updates.front().kind == DesignUpdateKind::retained);
    }

    SECTION("a changed SVG asset is recreated") {
        auto old_node = node("svg");
        auto new_node = old_node;
        old_node.render_mode = NodeRenderMode::faithful_svg;
        old_node.svg_asset_id = "panel-v1";
        new_node.render_mode = NodeRenderMode::faithful_svg;
        new_node.svg_asset_id = "panel-v2";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "svg");
    }

    SECTION("a changed capture asset is recreated") {
        auto old_node = node("capture");
        auto new_node = old_node;
        old_node.render_mode = NodeRenderMode::faithful_capture;
        old_node.capture_asset_id = "capture-v1";
        new_node.render_mode = NodeRenderMode::faithful_capture;
        new_node.capture_asset_id = "capture-v2";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "capture");
    }

    SECTION("a changed custom factory is recreated") {
        auto old_node = node("custom");
        auto new_node = old_node;
        IRInteractiveElement element;
        element.kind = InteractiveElementKind::custom;
        element.factory_id = "factory.v1";
        element.custom_props = R"({"mode":"compact"})";
        old_node.interactive_elements.push_back(element);
        new_node.interactive_elements.push_back(element);
        new_node.interactive_elements.front().factory_id = "factory.v2";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "custom");
    }

    SECTION("changed custom props are recreated") {
        auto old_node = node("custom-props");
        auto new_node = old_node;
        IRInteractiveElement element;
        element.kind = InteractiveElementKind::custom;
        element.factory_id = "factory";
        element.custom_props = R"({"mode":"compact"})";
        old_node.interactive_elements.push_back(element);
        new_node.interactive_elements.push_back(element);
        new_node.interactive_elements.front().custom_props = R"({"mode":"expanded"})";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "custom-props");
    }
}

TEST_CASE("interactive materialization fields recreate same-anchor nodes",
          "[view][import][update]") {
    SECTION("SVG patch path") {
        require_interactive_recreated("svg-patch",
                                      [](auto& element) { element.svg_patch_d = "M2 2L3 3"; });
    }

    SECTION("geometry") {
        require_interactive_recreated("geometry", [](auto& element) { element.x += 1.0f; });
    }

    SECTION("target frame") {
        require_interactive_recreated("target-frame",
                                      [](auto& element) { element.target_frame = 3; });
    }

    SECTION("action") {
        require_interactive_recreated("action",
                                      [](auto& element) { element.action = "octave_down"; });
    }

    SECTION("options") {
        require_interactive_recreated("options",
                                      [](auto& element) { element.options.push_back("three"); });
    }

    SECTION("selected index") {
        require_interactive_recreated("selected-index",
                                      [](auto& element) { element.selected_index = 0; });
    }

    SECTION("other overlay fields") {
        SECTION("placeholder") {
            require_interactive_recreated(
                "placeholder", [](auto& element) { element.placeholder = "new placeholder"; });
        }
        SECTION("background color") {
            require_interactive_recreated("bg-color",
                                          [](auto& element) { element.bg_color = "#654321"; });
        }
        SECTION("value label text") {
            require_interactive_recreated("text",
                                          [](auto& element) { element.text = "new value"; });
        }
        SECTION("value label alignment") {
            require_interactive_recreated("value-alignment",
                                          [](auto& element) { element.value_left_align = false; });
        }
    }

    SECTION("other patch fields") {
        SECTION("pivot") {
            require_interactive_recreated("pivot", [](auto& element) { element.cx += 1.0f; });
        }
        SECTION("hit radius") {
            require_interactive_recreated("hit-radius",
                                          [](auto& element) { element.hit_radius += 1.0f; });
        }
        SECTION("default value") {
            require_interactive_recreated("default-value",
                                          [](auto& element) { element.default_value = 0.5f; });
        }
        SECTION("toggle flash") {
            require_interactive_recreated("flash", [](auto& element) { element.flash = false; });
        }
        SECTION("xy pad default value") {
            require_interactive_recreated("default-value-y",
                                          [](auto& element) { element.default_value_y = 0.25f; });
        }
    }

    SECTION("source provenance") {
        require_interactive_recreated("source-node",
                                      [](auto& element) { element.source_node_id = "source:2"; });
    }
}

TEST_CASE("alternate frame materialization shape changes recreate the parent",
          "[view][import][update]") {
    SECTION("alternate node type") {
        auto old_node = node("alternate-type");
        auto new_node = old_node;
        old_node.render_mode = NodeRenderMode::faithful_svg;
        old_node.svg_asset_id = "panel";
        old_node.alternate_frames.push_back(node("alternate"));
        new_node.render_mode = NodeRenderMode::faithful_svg;
        new_node.svg_asset_id = "panel";
        new_node.alternate_frames.push_back(node("alternate"));
        new_node.alternate_frames.front().type = "knob";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "alternate-type");
    }

    SECTION("alternate audio widget") {
        auto old_node = node("alternate-audio");
        auto new_node = old_node;
        old_node.render_mode = NodeRenderMode::faithful_svg;
        old_node.svg_asset_id = "panel";
        old_node.alternate_frames.push_back(node("alternate"));
        new_node.render_mode = NodeRenderMode::faithful_svg;
        new_node.svg_asset_id = "panel";
        new_node.alternate_frames.push_back(node("alternate"));
        new_node.alternate_frames.front().audio_widget = AudioWidgetType::fader;
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "alternate-audio");
    }
}

TEST_CASE("native binding contract changes recreate keyed nodes", "[view][import][update]") {
    const auto require_contract_recreated = [](const char* key, const char* attribute) {
        auto old_node = node(key);
        auto new_node = old_node;
        old_node.attributes[attribute] = "old";
        new_node.attributes[attribute] = "new";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          key);
    };

    SECTION("route identity") {
        require_contract_recreated("route", "pulpRouteId");
    }
    SECTION("choice value") {
        require_contract_recreated("choice-value", "pulpChoiceValue");
    }
    SECTION("choice label") {
        require_contract_recreated("choice-label", "pulpChoiceLabel");
    }
    SECTION("waveform shape") {
        require_contract_recreated("waveform-shape", "pulpWaveformShape");
    }
    SECTION("event contract") {
        require_contract_recreated("event-contract", "pulpEventContract");
    }
    SECTION("gesture contract") {
        require_contract_recreated("gesture-contract", "pulpGestureContract");
    }
    SECTION("focus contract") {
        require_contract_recreated("focus-contract", "pulpFocusContract");
    }
    SECTION("style tokens") {
        require_contract_recreated("style-tokens", "pulpStyleTokens");
    }
    SECTION("widget schema") {
        require_contract_recreated("widget-schema", "pulpWidgetSchema");
    }
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
