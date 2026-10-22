#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <numeric>
#include <pulp/view/design_update.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    CHECK(plan.work == DesignUpdateWorkCounters{1, 1, 1, 1});
    CHECK(plan.work.total() == plan.updates.size());
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
    // Planted negative control: duplicate anchors must produce no retained or
    // moved work, so a caller cannot accidentally reuse an ambiguous view.
    CHECK(plan.work == DesignUpdateWorkCounters{0, 0, 1, 2});
    CHECK(plan.work.total() == plan.updates.size());
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

TEST_CASE("typed slot classification separates static and dynamic updates",
          "[view][import][update][slots]") {
    SECTION("a node without host metadata is a static visual slot") {
        const auto slots = classify_design_slots(node("static"));
        REQUIRE(slots == std::vector<DesignSlot>{{DesignSlotKind::static_visual, {}, {}}});
    }

    SECTION("mixed trees retain static parents alongside dynamic descendants") {
        auto root = node("static-parent");
        root.attributes["pulpRouteId"] = "parent";
        auto child = node("dynamic-child");
        child.attributes["pulpRouteId"] = "child";
        child.attributes["pulpParamKey"] = "gain";
        root.children.push_back(child);
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0] == DesignSlot{DesignSlotKind::dynamic_scalar, "child", "gain"});
        CHECK(slots[1] == DesignSlot{DesignSlotKind::static_visual, "parent", {}});
    }

    SECTION("scalar, vector, meter, text, and action routes are typed and sorted") {
        auto root = node("root");
        root.attributes["pulpRouteId"] = "panel.route";
        root.attributes["pulpParamKey"] = "gain";
        root.attributes["pulpParamKeyX"] = "pan.x";
        root.attributes["pulpParamKeyY"] = "pan.y";
        root.attributes["pulpMeterValueKey"] = "meter.level";
        root.attributes["pulpValueKey"] = "preset.name";
        root.attributes["pulpHostAction"] = "next_preset";
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 5);
        CHECK(slots[0] == DesignSlot{DesignSlotKind::dynamic_scalar, "panel.route", "gain"});
        CHECK(slots[1] ==
              DesignSlot{DesignSlotKind::dynamic_vector, "panel.route", "5:pan.x5:pan.y"});
        CHECK(slots[2] == DesignSlot{DesignSlotKind::dynamic_meter, "panel.route", "meter.level"});
        CHECK(slots[3] == DesignSlot{DesignSlotKind::dynamic_text, "panel.route", "preset.name"});
        CHECK(slots[4] == DesignSlot{DesignSlotKind::dynamic_action, "panel.route", "next_preset"});
    }

    SECTION("canonical lanes take precedence over the legacy binding fallback") {
        auto scalar = node("scalar");
        scalar.attributes["binding"] = "legacy";
        scalar.attributes["pulpParamKey"] = "canonical";
        auto meter = node("meter");
        meter.attributes["binding"] = "legacy-meter";
        meter.attributes["pulpMeterValueKey"] = "meter.level";
        scalar.children.push_back(meter);
        const auto slots = classify_design_slots(scalar);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0] == DesignSlot{DesignSlotKind::dynamic_scalar, {}, "canonical"});
        CHECK(slots[1] == DesignSlot{DesignSlotKind::dynamic_meter, {}, "meter.level"});
    }

    SECTION("descendants remain observable") {
        auto root = node("root");
        auto child = node("child");
        child.attributes["pulpParamKey"] = "child.gain";
        root.children.push_back(child);
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0] == DesignSlot{DesignSlotKind::static_visual, {}, {}});
        CHECK(slots[1] == DesignSlot{DesignSlotKind::dynamic_scalar, {}, "child.gain"});
    }

    SECTION("geometry-detected interactive routes use their typed lanes") {
        auto root = node("root");
        root.attributes["pulpRouteId"] = "geometry.route";
        IRInteractiveElement knob;
        knob.kind = InteractiveElementKind::knob;
        knob.param_key = "geometry.gain";
        root.interactive_elements.push_back(knob);
        IRInteractiveElement readout;
        readout.kind = InteractiveElementKind::value_label;
        readout.param_key = "geometry.display";
        root.interactive_elements.push_back(readout);
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0] ==
              DesignSlot{DesignSlotKind::dynamic_scalar, "geometry.route", "geometry.gain"});
        CHECK(slots[1] ==
              DesignSlot{DesignSlotKind::dynamic_text, "geometry.route", "geometry.display"});
    }

    SECTION("geometry text fields and XY pads retain their setter lanes") {
        IRNode root;
        root.stable_anchor_id = "root";
        root.attributes["pulpRouteId"] = "geometry.route";
        IRInteractiveElement text_field;
        text_field.kind = InteractiveElementKind::text_field;
        text_field.param_key = "geometry.label";
        IRInteractiveElement xy_pad;
        xy_pad.kind = InteractiveElementKind::xy_pad;
        xy_pad.param_key = "geometry.xy";
        root.interactive_elements = {text_field, xy_pad};

        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0] ==
              DesignSlot{DesignSlotKind::dynamic_vector, "geometry.route", "geometry.xy"});
        CHECK(slots[1] ==
              DesignSlot{DesignSlotKind::dynamic_text, "geometry.route", "geometry.label"});
    }

    SECTION("module and parameter binding fallback remains typed") {
        auto root = node("module-route");
        root.attributes["pulpBindingModule"] = "synth";
        root.attributes["pulpBindingParam"] = "cutoff";
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 1);
        CHECK(slots.front() == DesignSlot{DesignSlotKind::dynamic_scalar, {}, "5:synth6:cutoff"});
    }

    SECTION("module-bound XY attributes remain typed") {
        auto root = node("module-xy");
        root.attributes["pulpBindingModuleX"] = "synth";
        root.attributes["pulpBindingParamX"] = "pan";
        root.attributes["pulpBindingModuleY"] = "synth";
        root.attributes["pulpBindingParamY"] = "tilt";
        const auto slots = classify_design_slots(root);
        REQUIRE(slots.size() == 1);
        CHECK(slots.front().kind == DesignSlotKind::dynamic_vector);
        CHECK(slots.front().key == "12:5:synth3:pan13:5:synth4:tilt");
    }

    SECTION("vector keys remain injective when parameter names contain delimiters") {
        auto left = node("left");
        left.attributes["pulpParamKeyX"] = "a|b";
        left.attributes["pulpParamKeyY"] = "c";
        auto right = node("right");
        right.attributes["pulpParamKeyX"] = "a";
        right.attributes["pulpParamKeyY"] = "b|c";
        const auto left_slots = classify_design_slots(left);
        const auto right_slots = classify_design_slots(right);
        REQUIRE(left_slots.size() == 1);
        REQUIRE(right_slots.size() == 1);
        CHECK(left_slots.front().kind == DesignSlotKind::dynamic_vector);
        CHECK(right_slots.front().kind == DesignSlotKind::dynamic_vector);
        CHECK(left_slots.front().key != right_slots.front().key);
    }

    SECTION("module and meter fallback keys remain injective") {
        auto left = node("left");
        left.attributes["pulpBindingModule"] = "a::b";
        left.attributes["pulpBindingParam"] = "c";
        left.attributes["pulpMeterSource"] = "a/b";
        left.attributes["pulpMeterChannel"] = "c";
        auto right = node("right");
        right.attributes["pulpBindingModule"] = "a";
        right.attributes["pulpBindingParam"] = "b::c";
        right.attributes["pulpMeterSource"] = "a";
        right.attributes["pulpMeterChannel"] = "b/c";
        const auto left_slots = classify_design_slots(left);
        const auto right_slots = classify_design_slots(right);
        REQUIRE(left_slots.size() == 2);
        REQUIRE(right_slots.size() == 2);
        CHECK(left_slots[0].key != right_slots[0].key);
        CHECK(left_slots[1].key != right_slots[1].key);
    }
}

TEST_CASE("slot classification mutation control preserves changed binding identity",
          "[view][import][update][slots][negative]") {
    auto old_node = node("control");
    old_node.attributes["pulpRouteId"] = "route";
    old_node.attributes["pulpParamKey"] = "gain";
    auto new_node = old_node;
    new_node.attributes["pulpParamKey"] = "cutoff";
    const auto old_slots = classify_design_slots(old_node);
    const auto new_slots = classify_design_slots(new_node);
    REQUIRE(old_slots.size() == 1);
    REQUIRE(new_slots.size() == 1);
    CHECK(old_slots.front().kind == DesignSlotKind::dynamic_scalar);
    CHECK(new_slots.front().kind == DesignSlotKind::dynamic_scalar);
    CHECK(old_slots.front().route_id == new_slots.front().route_id);
    CHECK(old_slots.front().key != new_slots.front().key);
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
    SECTION("binding module is immutable after installation") {
        // NativeImportBindingContext receives the module while installing the
        // callback; there is no setter on a retained native control. This is
        // a planted negative control for accidentally treating every binding
        // string as a re-keyable value.
        auto old_node = node("binding-module");
        auto new_node = old_node;
        old_node.attributes["pulpParamKey"] = "gain";
        old_node.attributes["pulpBindingModule"] = "voice-a";
        new_node.attributes["pulpParamKey"] = "gain";
        new_node.attributes["pulpBindingModule"] = "voice-b";
        require_recreated(plan_design_child_updates(std::span<const IRNode>(&old_node, 1),
                                                    std::span<const IRNode>(&new_node, 1)),
                          "binding-module");
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

namespace {
using Keys = std::vector<std::string>;

Keys sequential_keys(std::size_t count) {
    Keys keys;
    for (std::size_t i = 0; i < count; ++i)
        keys.push_back("k" + std::to_string(i));
    return keys;
}

std::vector<IRNode> keyed_nodes(const Keys& keys) {
    std::vector<IRNode> nodes;
    nodes.reserve(keys.size());
    for (const auto& key : keys)
        nodes.push_back(node(key.c_str()));
    return nodes;
}

// Applies the plan to the old keys exactly as a materializer would and
// returns the resulting order.
Keys apply_plan(const DesignChildUpdatePlan& plan, Keys working) {
    for (const auto& update : plan.updates) {
        if (update.kind == DesignUpdateKind::removed) {
            REQUIRE(update.old_index < working.size());
            REQUIRE(working[update.old_index] == update.key);
            working.erase(working.begin() + static_cast<std::ptrdiff_t>(update.old_index));
        } else if (update.kind == DesignUpdateKind::inserted) {
            REQUIRE(update.new_index <= working.size());
            working.insert(working.begin() + static_cast<std::ptrdiff_t>(update.new_index),
                           update.key);
        } else if (update.kind == DesignUpdateKind::moved) {
            REQUIRE(update.old_index < working.size());
            REQUIRE(working[update.old_index] == update.key);
            auto value = std::move(working[update.old_index]);
            working.erase(working.begin() + static_cast<std::ptrdiff_t>(update.old_index));
            REQUIRE(update.new_index <= working.size());
            working.insert(working.begin() + static_cast<std::ptrdiff_t>(update.new_index),
                           std::move(value));
        }
    }
    return working;
}

// Reference minimum for an all-compatible keyed update: survivors minus the
// longest increasing run of their old positions taken in new order, computed
// by quadratic dynamic programming independently of the planner.
std::size_t minimum_moves(const Keys& old_keys, const Keys& new_keys) {
    std::unordered_map<std::string, std::size_t> old_position;
    for (std::size_t i = 0; i < old_keys.size(); ++i)
        old_position.emplace(old_keys[i], i);
    std::vector<std::size_t> sequence;
    for (const auto& key : new_keys) {
        if (const auto it = old_position.find(key); it != old_position.end())
            sequence.push_back(it->second);
    }
    std::vector<std::size_t> longest(sequence.size(), 1);
    std::size_t best = 0;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (sequence[j] < sequence[i])
                longest[i] = std::max(longest[i], longest[j] + 1);
        }
        best = std::max(best, longest[i]);
    }
    return sequence.size() - best;
}

struct KeyedMutation {
    const char* name;
    std::function<Keys(const Keys&)> mutate;
    std::size_t expected_moved;
};

Keys rotate_left(Keys keys, std::size_t count) {
    std::rotate(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(count), keys.end());
    return keys;
}

Keys rotate_right(Keys keys, std::size_t count) {
    std::rotate(keys.rbegin(), keys.rbegin() + static_cast<std::ptrdiff_t>(count), keys.rend());
    return keys;
}

// Named keyed-list mutations from the uibench tree suite, over 50 children.
// Expected move counts are the proven minimum for each shape.
std::vector<KeyedMutation> uibench_mutations() {
    return {
        {"no_change", [](const Keys& k) { return k; }, 0},
        {"moveFromStartToEnd(1)", [](const Keys& k) { return rotate_left(k, 1); }, 1},
        {"moveFromStartToEnd(2)", [](const Keys& k) { return rotate_left(k, 2); }, 2},
        {"moveFromEndToStart(1)", [](const Keys& k) { return rotate_right(k, 1); }, 1},
        {"moveFromEndToStart(2)", [](const Keys& k) { return rotate_right(k, 2); }, 2},
        {"swap(1,n-2)",
         [](Keys k) {
             std::swap(k[1], k[k.size() - 2]);
             return k;
         },
         2},
        {"reverse",
         [](Keys k) {
             std::reverse(k.begin(), k.end());
             return k;
         },
         49},
        {"react_worst_case",
         [](const Keys& k) {
             Keys middle(k.begin() + 1, k.end() - 1);
             return rotate_right(middle, 1);
         },
         1},
        {"kivi_worst_case",
         [](const Keys& k) {
             Keys middle(k.begin() + 1, k.end() - 1);
             std::reverse(middle.begin(), middle.end());
             return middle;
         },
         47},
        {"insertFirst(1)",
         [](Keys k) {
             k.insert(k.begin(), "new");
             return k;
         },
         0},
        {"insertLast(1)",
         [](Keys k) {
             k.push_back("new");
             return k;
         },
         0},
        {"removeFirst(1)",
         [](Keys k) {
             k.erase(k.begin());
             return k;
         },
         0},
        {"removeLast(1)",
         [](Keys k) {
             k.pop_back();
             return k;
         },
         0},
        {"shuffle(seed 7)",
         [](Keys k) {
             std::mt19937 random(7);
             std::shuffle(k.begin(), k.end(), random);
             return k;
         },
         0},
        {"shuffle(seed 1234)",
         [](Keys k) {
             std::mt19937 random(1234);
             std::shuffle(k.begin(), k.end(), random);
             return k;
         },
         0},
    };
}

std::size_t survivors_of(const Keys& old_keys, const Keys& new_keys) {
    return static_cast<std::size_t>(std::count_if(new_keys.begin(), new_keys.end(), [&](auto& key) {
        return std::find(old_keys.begin(), old_keys.end(), key) != old_keys.end();
    }));
}

void require_minimal_plan(const Keys& old_keys, const Keys& new_keys) {
    const auto plan = plan_design_child_updates(keyed_nodes(old_keys), keyed_nodes(new_keys));
    REQUIRE(plan.keyed);
    CHECK(plan.key_diagnostics.empty());
    const auto survivors = survivors_of(old_keys, new_keys);
    CHECK(plan.work.moved == minimum_moves(old_keys, new_keys));
    CHECK(plan.work.retained + plan.work.moved == survivors);
    CHECK(plan.work.inserted == new_keys.size() - survivors);
    CHECK(plan.work.removed == old_keys.size() - survivors);
    CHECK(plan.work.total() == plan.updates.size());
    CHECK(apply_plan(plan, old_keys) == new_keys);
}
} // namespace

TEST_CASE("keyed mutation corpus moves only children outside the longest increasing run",
          "[view][import][update][keyed]") {
    const auto old_keys = sequential_keys(50);
    for (const auto& mutation : uibench_mutations()) {
        INFO(mutation.name);
        const auto new_keys = mutation.mutate(old_keys);
        const auto plan = plan_design_child_updates(keyed_nodes(old_keys), keyed_nodes(new_keys));
        require_minimal_plan(old_keys, new_keys);
        // Shuffles have no closed-form minimum; the reference above covers them.
        if (std::string_view(mutation.name).rfind("shuffle", 0) != 0)
            CHECK(plan.work.moved == mutation.expected_moved);
    }
}

TEST_CASE("keyed planner trims a shared prefix and suffix as retained",
          "[view][import][update][keyed]") {
    const Keys old_keys{"a", "b", "c", "d", "e", "f"};
    const Keys new_keys{"a", "b", "d", "c", "e", "f"};
    const auto plan = plan_design_child_updates(keyed_nodes(old_keys), keyed_nodes(new_keys));
    REQUIRE(plan.updates.size() == 6);
    CHECK(plan.work == DesignUpdateWorkCounters{5, 1, 0, 0});
    for (const auto index : {0u, 1u, 4u, 5u}) {
        CHECK(plan.updates[index].kind == DesignUpdateKind::retained);
        CHECK(plan.updates[index].old_index == index);
        CHECK(plan.updates[index].new_index == index);
    }
    CHECK(apply_plan(plan, old_keys) == new_keys);
}

TEST_CASE("seeded random keyed permutations match the reference minimum",
          "[view][import][update][keyed]") {
    std::mt19937 random(0x5eed);
    for (int round = 0; round < 300; ++round) {
        const auto count = std::uniform_int_distribution<std::size_t>(0, 200)(random);
        const auto old_keys = sequential_keys(count);
        auto new_keys = old_keys;
        std::shuffle(new_keys.begin(), new_keys.end(), random);
        // Drop and add a random share of children so removals, inserts and
        // moves interleave.
        std::bernoulli_distribution drop(round % 3 == 0 ? 0.0 : 0.2);
        std::erase_if(new_keys, [&](const std::string&) { return drop(random); });
        const auto inserts = std::uniform_int_distribution<std::size_t>(0, round % 4)(random);
        for (std::size_t i = 0; i < inserts; ++i) {
            const auto at = std::uniform_int_distribution<std::size_t>(0, new_keys.size())(random);
            new_keys.insert(new_keys.begin() + static_cast<std::ptrdiff_t>(at),
                            "n" + std::to_string(i));
        }
        INFO("round " << round << " old " << old_keys.size() << " new " << new_keys.size());
        require_minimal_plan(old_keys, new_keys);
    }
}

TEST_CASE("recreated keyed children leave the move minimum to compatible survivors",
          "[view][import][update][keyed]") {
    const Keys old_keys{"a", "b", "c", "d"};
    const Keys new_keys{"d", "b", "c", "a"};
    auto old_children = keyed_nodes(old_keys);
    auto new_children = keyed_nodes(new_keys);
    new_children[1].audio_widget = AudioWidgetType::knob;
    const auto plan = plan_design_child_updates(old_children, new_children);
    REQUIRE(plan.keyed);
    // b is recreated; of the survivors d, c, a only one ordered pair stays.
    CHECK(plan.work == DesignUpdateWorkCounters{1, 2, 1, 1});
    CHECK(apply_plan(plan, old_keys) == new_keys);
}

TEST_CASE("ambiguous keys report the reason and location of each offender",
          "[view][import][update][keyed]") {
    const auto require_replaced = [](const DesignChildUpdatePlan& plan, std::size_t old_count,
                                     std::size_t new_count) {
        CHECK_FALSE(plan.keyed);
        CHECK(plan.ambiguous_keys);
        CHECK(plan.work == DesignUpdateWorkCounters{0, 0, new_count, old_count});
    };

    SECTION("duplicate") {
        const auto plan =
            plan_design_child_updates(keyed_nodes({"a", "b", "a", "a"}), keyed_nodes({"a", "b"}));
        require_replaced(plan, 4, 2);
        REQUIRE(plan.key_diagnostics == std::vector<DesignKeyDiagnostic>{
                                            {DesignKeyDiagnosticReason::duplicate, "a", 2, true},
                                            {DesignKeyDiagnosticReason::duplicate, "a", 3, true},
                                        });
        CHECK(to_string(plan.key_diagnostics.front().reason) == "duplicate");
    }

    SECTION("missing") {
        std::vector<IRNode> unkeyed(3);
        const auto plan = plan_design_child_updates(keyed_nodes({"a", "b"}), unkeyed);
        require_replaced(plan, 2, 3);
        REQUIRE(plan.key_diagnostics == std::vector<DesignKeyDiagnostic>{
                                            {DesignKeyDiagnosticReason::missing, "", 0, false},
                                        });
    }

    SECTION("mixed") {
        auto new_children = keyed_nodes({"a", "b", "c"});
        new_children[1].stable_anchor_id.reset();
        const auto plan = plan_design_child_updates(keyed_nodes({"a", "b", "c"}), new_children);
        require_replaced(plan, 3, 3);
        REQUIRE(plan.key_diagnostics == std::vector<DesignKeyDiagnostic>{
                                            {DesignKeyDiagnosticReason::mixed, "", 1, false},
                                        });
        CHECK(to_string(plan.key_diagnostics.front().reason) == "mixed");
    }

    SECTION("unique keys carry no diagnostics") {
        const auto plan =
            plan_design_child_updates(keyed_nodes({"a", "b"}), keyed_nodes({"b", "a"}));
        CHECK(plan.keyed);
        CHECK(plan.key_diagnostics.empty());
    }
}

TEST_CASE("keyed planner cost over large keyed lists", "[view][import][update][keyed][bench]") {
    for (const std::size_t count : {500u, 1000u}) {
        Keys old_keys = sequential_keys(count);
        const auto old_children = keyed_nodes(old_keys);
        for (const auto& mutation : uibench_mutations()) {
            const auto new_keys = mutation.mutate(old_keys);
            const auto new_children = keyed_nodes(new_keys);
            constexpr int repetitions = 20;
            DesignChildUpdatePlan plan;
            Keys applied;
            using clock = std::chrono::steady_clock;
            clock::duration planning{};
            clock::duration applying{};
            for (int i = 0; i < repetitions; ++i) {
                const auto start = clock::now();
                plan = plan_design_child_updates(old_children, new_children);
                const auto planned = clock::now();
                applied = apply_plan(plan, old_keys);
                planning += planned - start;
                applying += clock::now() - planned;
            }
            const auto micros = [](clock::duration total) {
                return std::chrono::duration<double, std::micro>(total).count() / repetitions;
            };
            std::printf("keyed-planner n=%zu %-24s moved=%zu plan=%.1f us apply=%.1f us\n", count,
                        mutation.name, plan.work.moved, micros(planning), micros(applying));
            CHECK(applied == new_keys);
        }
    }
}
