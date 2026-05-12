// SPDX-License-Identifier: MIT
//
// Unit tests for pulp::import_design::promote_interactive_frames — the
// post-parse pass that re-types interactive frames (<div onClick>, ARIA
// role="button", cursor:pointer) to "button" so the importer doesn't
// drop the user's interactive intent on the floor.
//
// pulp-internal task #84 (follow-up to issue #1814). Covers the four
// promotion paths plus the non-promotion guardrails: explicit
// role="presentation", non-frame node types, and already-typed widgets.

#include "../tools/import-design/widget_promotion.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>

using pulp::view::IRNode;
using pulp::import_design::WidgetPromotionSignal;
using pulp::import_design::classify_interactive_signal;
using pulp::import_design::promote_interactive_frames;

namespace {

IRNode make_frame(const std::string& name = "") {
    IRNode n;
    n.type = "frame";
    n.name = name;
    return n;
}

}  // namespace

TEST_CASE("widget_promotion: html lowercase onclick promotes a frame to button", "[import][widget][issue-1814]") {
    IRNode div = make_frame("clickable-div");
    div.attributes["onclick"] = "handleClick()";

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::onclick_attribute);

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
}

TEST_CASE("widget_promotion: jsx onClick promotes a frame to button", "[import][widget][issue-1814]") {
    IRNode div = make_frame("jsx-handler");
    // The runtime-import path (parse_claude_html_with_runtime) walks the
    // live DOM after React mount, so JSX-style camelCase props are what
    // it preserves in IRNode.attributes.
    div.attributes["onClick"] = "{handlePress}";

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::onclick_attribute);

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
}

TEST_CASE("widget_promotion: aria role=button promotes regardless of click handler presence", "[import][widget][issue-1814]") {
    IRNode div = make_frame("aria-btn");
    div.attributes["role"] = "button";
    // No onclick — purely the ARIA semantic claim.

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::aria_role_button);

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
}

TEST_CASE("widget_promotion: cursor:pointer promotes when no stronger signal disagrees", "[import][widget][issue-1814]") {
    IRNode div = make_frame("cursor-only");
    div.style.cursor = std::string{"pointer"};

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::cursor_pointer);

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
}

TEST_CASE("widget_promotion: role=presentation opts a cursor:pointer frame out", "[import][widget][issue-1814]") {
    // Designers occasionally set cursor:pointer on hover affordances or
    // pseudo-link wrappers that aren't actually clickable. The
    // canonical way to opt out is role="presentation"; honor that.
    IRNode div = make_frame("decorative");
    div.style.cursor = std::string{"pointer"};
    div.attributes["role"] = "presentation";

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::none);

    REQUIRE(promote_interactive_frames(div) == 0);
    REQUIRE(div.type == "frame");
}

TEST_CASE("widget_promotion: already-typed widgets are not re-classified", "[import][widget][issue-1814]") {
    // Critical guardrail — a designer who explicitly wrote <input
    // onClick=...> should keep the input type, not silently flip to
    // button.
    IRNode input;
    input.type = "input";
    input.name = "search-box";
    input.attributes["onClick"] = "{handleFocus}";

    REQUIRE(classify_interactive_signal(input) == WidgetPromotionSignal::none);

    REQUIRE(promote_interactive_frames(input) == 0);
    REQUIRE(input.type == "input");

    IRNode existing_button;
    existing_button.type = "button";
    existing_button.attributes["onClick"] = "{handlePress}";

    REQUIRE(classify_interactive_signal(existing_button) == WidgetPromotionSignal::none);

    REQUIRE(promote_interactive_frames(existing_button) == 0);
    REQUIRE(existing_button.type == "button");
}

TEST_CASE("widget_promotion: pass is recursive — nested interactive frames all promote", "[import][widget][issue-1814]") {
    IRNode root = make_frame("root");
    {
        IRNode header = make_frame("header");
        IRNode close = make_frame("close-icon");
        close.attributes["onclick"] = "dismiss()";
        header.children.push_back(std::move(close));
        root.children.push_back(std::move(header));
    }
    {
        IRNode list = make_frame("list");
        for (int i = 0; i < 3; ++i) {
            IRNode item = make_frame("item-" + std::to_string(i));
            item.style.cursor = std::string{"pointer"};
            list.children.push_back(std::move(item));
        }
        root.children.push_back(std::move(list));
    }

    REQUIRE(promote_interactive_frames(root) == 4);
    REQUIRE(root.type == "frame");                                  // root has no signal
    REQUIRE(root.children[0].type == "frame");                      // header has no signal
    REQUIRE(root.children[0].children[0].type == "button");         // close-icon
    REQUIRE(root.children[1].type == "frame");                      // list container
    REQUIRE(root.children[1].children[0].type == "button");
    REQUIRE(root.children[1].children[1].type == "button");
    REQUIRE(root.children[1].children[2].type == "button");
}

TEST_CASE("widget_promotion: pass is idempotent — second invocation is a no-op", "[import][widget][issue-1814]") {
    IRNode div = make_frame("idempotent");
    div.attributes["onclick"] = "go()";

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
    REQUIRE(promote_interactive_frames(div) == 0);  // already a button
    REQUIRE(div.type == "button");
}

TEST_CASE("widget_promotion: promoted parent stops descendant promotion — no nested buttons",
          "[import][widget][issue-1814]") {
    // Codex P2 on PR #1824 — a clickable container with clickable
    // children must not produce nested <button> elements. The
    // generated UI relies on the promoted parent's click handler to
    // cover the whole subtree; nested buttons would break click/focus
    // semantics.
    IRNode parent = make_frame("clickable-parent");
    parent.attributes["onclick"] = "selectRow()";

    IRNode child_a = make_frame("inner-clickable");
    child_a.style.cursor = std::string{"pointer"};

    IRNode child_b = make_frame("inner-aria");
    child_b.attributes["role"] = "button";

    parent.children.push_back(std::move(child_a));
    parent.children.push_back(std::move(child_b));

    REQUIRE(promote_interactive_frames(parent) == 1);
    REQUIRE(parent.type == "button");
    // Children stay as frames — the parent's promotion swallows
    // their interactive intent.
    REQUIRE(parent.children[0].type == "frame");
    REQUIRE(parent.children[1].type == "frame");
}

TEST_CASE("widget_promotion: iterative walker handles deep IR without stack blowup",
          "[import][widget][issue-1814]") {
    // Codex P2 on PR #1824 — the prior implementation called itself
    // recursively despite a comment claiming "iterative walk." On a
    // 5000-deep linear IR (within reason for some Figma exports
    // post-flatten), recursive descent would stack-overflow on most
    // hosts. The worklist-based pass should handle it cleanly.
    //
    // Tree shape: a 5000-deep linear chain with NO promotable nodes,
    // capped by a single promotable leaf. If the walker stack-
    // overflows, the test crashes; if the walker short-circuits
    // before reaching the leaf, the count is wrong.
    IRNode root = make_frame("root");
    IRNode* cursor = &root;
    constexpr int kDepth = 5000;
    for (int i = 0; i < kDepth; ++i) {
        cursor->children.push_back(make_frame("node-" + std::to_string(i)));
        cursor = &cursor->children.back();
    }
    // Promotable leaf at the bottom.
    cursor->attributes["onclick"] = "f()";

    REQUIRE(promote_interactive_frames(root) == 1);
    REQUIRE(cursor->type == "button");
}

TEST_CASE("widget_promotion: iterative walker handles wide IR without missing siblings",
          "[import][widget][issue-1814]") {
    // Sibling-coverage companion to the deep-walk test. 1000 sibling
    // frames under one root, every other one promotable. Confirms the
    // worklist visits every direct child + the promotion count
    // matches expectation (no off-by-one or worklist-order bug).
    IRNode root = make_frame("root");
    constexpr int kWidth = 1000;
    for (int i = 0; i < kWidth; ++i) {
        IRNode child = make_frame("sibling-" + std::to_string(i));
        if (i % 2 == 0) {
            child.attributes["onclick"] = "f()";
        }
        root.children.push_back(std::move(child));
    }

    REQUIRE(promote_interactive_frames(root) == kWidth / 2);
    // Spot-check pre-order: child[0] is promoted, child[1] is not,
    // child[2] is promoted — so the walker visited them in
    // left-to-right order.
    REQUIRE(root.children[0].type == "button");
    REQUIRE(root.children[1].type == "frame");
    REQUIRE(root.children[2].type == "button");
}

TEST_CASE("widget_promotion: stronger signal wins when several are present", "[import][widget][issue-1814]") {
    // When onClick + role=button + cursor:pointer are all set, we expect
    // the onClick branch (highest-priority signal). Classifier
    // documents the precedence; the test pins it.
    IRNode div = make_frame("all-signals");
    div.attributes["onclick"] = "press()";
    div.attributes["role"] = "button";
    div.style.cursor = std::string{"pointer"};

    REQUIRE(classify_interactive_signal(div) == WidgetPromotionSignal::onclick_attribute);

    REQUIRE(promote_interactive_frames(div) == 1);
    REQUIRE(div.type == "button");
}
