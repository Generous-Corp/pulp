// SPDX-License-Identifier: MIT
//
// Implementation of the post-parse widget-promotion pass.
// See widget_promotion.hpp for the rationale.

#include "widget_promotion.hpp"

#include <pulp/view/design_import.hpp>

#include <string>
#include <vector>

namespace pulp::import_design {

WidgetPromotionSignal classify_interactive_signal(const pulp::view::IRNode& node) {
    // We only promote frames — never re-classify already-typed widgets.
    if (node.type != "frame") {
        return WidgetPromotionSignal::none;
    }

    // 1. Direct click handler — strongest signal. The importer attribute
    //    walker preserves whatever the HTML / JSX exporter wrote, so we
    //    accept both the HTML spelling (`onclick`) and the JSX spelling
    //    (`onClick`).
    if (node.attributes.count("onclick") || node.attributes.count("onClick")) {
        return WidgetPromotionSignal::onclick_attribute;
    }

    // 2. ARIA role — explicit semantic claim from the designer that the
    //    div behaves like a button. Treat as authoritative.
    if (auto it = node.attributes.find("role");
        it != node.attributes.end() && it->second == "button") {
        return WidgetPromotionSignal::aria_role_button;
    }

    // 3. cursor:pointer — weaker signal because designers also set it on
    //    decorative links / hover affordances. But for the import path
    //    it's still the only signal Figma copy-CSS / Stitch usually emit
    //    on otherwise-bare clickable divs, so we accept it. Producers
    //    that want a static cursor:pointer frame can opt out by setting
    //    `role="presentation"` (handled implicitly: we don't recognize
    //    presentation as a promotion signal, and the explicit role wins
    //    earlier).
    if (node.style.cursor && *node.style.cursor == "pointer") {
        // If the producer simultaneously set role="presentation", honor
        // their explicit non-widget intent.
        if (auto it = node.attributes.find("role");
            it != node.attributes.end() && it->second == "presentation") {
            return WidgetPromotionSignal::none;
        }
        return WidgetPromotionSignal::cursor_pointer;
    }

    return WidgetPromotionSignal::none;
}

std::size_t promote_interactive_frames(pulp::view::IRNode& root) {
    std::size_t promoted = 0;

    // Heap-backed worklist so deeply-nested designs (Figma / Claude
    // Design exports occasionally reach 60+ levels) don't blow the
    // process stack — the prior recursive walker tripped Codex P2 on
    // PR #1824 for this exact reason. We push raw pointers so the
    // children-pointer stays stable across pushes; the worklist never
    // outlives the call so there's no dangling-pointer risk.
    std::vector<pulp::view::IRNode*> worklist;
    worklist.push_back(&root);

    while (!worklist.empty()) {
        pulp::view::IRNode* node = worklist.back();
        worklist.pop_back();

        if (classify_interactive_signal(*node) != WidgetPromotionSignal::none) {
            node->type = "button";
            ++promoted;
            // Codex P2 on PR #1824 — once a node becomes a button, do
            // NOT recurse into its descendants. A clickable container
            // with clickable children would otherwise produce nested
            // <button> elements (invalid interactive nesting + broken
            // click/focus semantics in the generated UI). The native
            // click handler attached to the promoted parent already
            // covers the whole subtree.
            continue;
        }

        // Push children in reverse so pre-order is preserved (left-to-right
        // visit order matches the user-visible IR layout).
        for (auto it = node->children.rbegin();
             it != node->children.rend();
             ++it) {
            worklist.push_back(&*it);
        }
    }

    return promoted;
}

}  // namespace pulp::import_design
